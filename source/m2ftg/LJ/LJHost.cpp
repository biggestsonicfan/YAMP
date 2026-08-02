#include "LJHost.h"

// Defined in HostCdevice.cpp: mark the DLL's per-frame render window so the ResourceBarrier hook
// only corrects StF's own barrier StateBefore values (not d3d11on12's blit barriers).
void SetModuleRenderActiveNow(bool active);
// Defined in HostCdevice.cpp: PATH B — close+execute+flush+reopen StF's own draw list each frame.
void SubmitModuleFrameListNow();
// Defined in HostCdevice.cpp: true once a submit hung/removed the device (stop the loop, DRED dumped).
bool ModuleExecDisabledNow();
// Defined in HostCdevice.cpp: the host's per-frame upload-pool frame-advance — bump the upload-frame
// stamp + recycle StF's upload buffers (in-use -> available). Fixes the pool-exhaustion crash.
void AdvanceFrameStampNow();

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "../../wil/resource.h"

#include "../../criware/Cri.h"

#include "../../pxd/LJ/sl.h"
#include "../../pxd/LJ/gs.h"
#include "../m2ftg.h"
#include "../ModuleArgs.h"
#include "../DisplayModes.h"
#include "../../pxd/Imports.h"
#include "Patch.h"
#include "../../pxd/LJ/sys_util.h"
#include "../../pxd/LJ/cs_game.h"

#include "../ImportSymbols.h"
#include "../../pxd/LJ/HostCdevice.h"
#include "HleHooks.h"
#include "DebugWindows.h"
#include "../ELF/CharRamFix.h"
#include "../ELF/ElfRom.h"
#include "../../input/Input.h"
#include "../HostUI.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../imgui/imgui.h"

#include "../../DebugLog.h"
#include "../../net/NetPlugin.h"
#include "../../Utils/MemoryMgr.h"
#include "../../Utils/ScopedUnprotect.hpp"

namespace m2ftg
{
	// The pxd platform layer this host is built on (source/pxd): sl/gs/cgs_device_context,
	// the host cdevice, Imports/ImportSymbol. Shared with the LJ VF5FS host.
	using namespace pxd;
        // Per-game facts (DLL name, config.kind, ROM names, i960 RVAs) live in LJHost.h
        // (GameDesc / CurrentGame()) — the LJ-specific but m2ftg-generic hosting header.

        // Contexts
        // TODO: Move elsewhere, as they will get very, very long
        // But not in V6-VF5FS.h as they are private!

        // ct::initialize_module (0x1800c6ff0) requires size_of_struct == 0x30
        struct ct_context_t
        {
            uint32_t tag_id; // Unknown
            uint32_t version; // Unknown
            uint32_t size_of_struct = 0x30;
            std::byte pad[36]{};
        };
        static_assert(sizeof(ct_context_t) == 0x30);

        // TODO: Later these can be put by value, for now put them on the heap to make full use of page heap
        // and catch any out-of-bounds access
        ct_context_t* g_ct_context = new ct_context_t;

        // m2ftg_config_t / m2ftg_pad_t / m2ftg_execute_info_t now live in m2ftg.h (shared with
        // the VF2 host). module_start (0x180062830) memcpys the 0x100C config into DAT_1801ed490.



        static void ImportFunctions(const Imports& symbols)
        {
            auto Import = [&symbols](auto& var, auto symbol)
                {
                    var = static_cast<std::decay_t<decltype(var)>>(symbols.GetSymbol(symbol));
                };

            Import(sl::sm_context, ImportSymbol::SL_CONTEXT_INSTANCE);
            Import(gs::sm_context, ImportSymbol::GS_CONTEXT_INSTANCE);
            Import(sl::file_create_internal, ImportSymbol::SL_FILE_CREATE);
            Import(sl::file_open_internal, ImportSymbol::SL_FILE_OPEN);
            Import(sl::file_read, ImportSymbol::SL_FILE_READ);
            Import(sl::file_close, ImportSymbol::SL_FILE_CLOSE);
            Import(sl::handle_create_internal, ImportSymbol::SL_HANDLE_CREATE);
            Import(sl::file_handle_destroy, ImportSymbol::SL_FILE_HANDLE_DESTROY);
            Import(sl::archive_lock_wlock, ImportSymbol::ARCHIVE_LOCK_WLOCK);
            Import(sl::archive_lock_wunlock, ImportSymbol::ARCHIVE_LOCK_WUNLOCK);
            Import(cgs_device_context::reset_state_all_internal, ImportSymbol::DEVICE_CONTEXT_RESET_STATE_ALL);
            Import(gs::vb_create, ImportSymbol::VB_CREATE);
            Import(gs::ib_create, ImportSymbol::IB_CREATE);
			Import(sl::kernel_calloc_internal, ImportSymbol::SL_KERNEL_CALLOC);
			//Import(sl::memset, ImportSymbol::MEMSET);
        }

        static void PrefillVariables(const Imports& symbols, const RenderWindow& window)
        {
            auto Import = [&symbols](auto& var, auto symbol)
                {
                    var = static_cast<std::decay_t<decltype(var)>>(symbols.GetSymbol(symbol));
                };

            gs::context_t** ppContext;
            Import(ppContext, ImportSymbol::GS_CONTEXT_PTR);
            *ppContext = gs::sm_context;

            // The DLL's "D3DDEVICE" symbol is cdevice_common::g_pD3DDevice: a POINTER
            // slot the DX12 renderer treats as the pxd host cdevice object (freelist at
            // +0x38, device at +0x08, allocator at +0x68, resource factory at +0x17b0).
            // Writing the raw ID3D12Device here (as the VF5FS/DX11 path does) leaves the
            // intermediate-buffer freelist empty -> infinite spin. Build a proper host
            // cdevice and store a pointer to IT instead.
            void** ppDevice12;
            Import(ppDevice12, ImportSymbol::D3DDEVICE);

            using cdevice_ctor_fn = void* (*)(void*);
            auto cdeviceCtor = reinterpret_cast<cdevice_ctor_fn>(symbols.GetSymbol(ImportSymbol::CDEVICE_CTOR));
            *ppDevice12 = BuildHostCdevice(window.GetD3D12Device(), window.GetD3D12Queue(), cdeviceCtor);
        }

        static bool ResolveSymbolsAndInstallPatches(void* dll, const RenderWindow& window) try
        {
            // Register the module's real load range for the hooks' return-address checks.
            // The FV DLL is ASLR'd (DYNAMIC_BASE) so it does NOT sit at 0x180000000 like StF.
            SetGameDllRange(dll);

            const Imports symbolMap = BuildSymbolMap(dll);

            const ScopedUnprotect::Section text(static_cast<HMODULE>(dll), ".text");
            const ScopedUnprotect::Section rdata(static_cast<HMODULE>(dll), ".rdata");

            // Lets YAMP's command line reach the module's own option parser, which module_start
            // otherwise calls with an empty argv. Must be before module_start - the parse happens
            // inside it.
            ModuleArgs::Install(dll);

            // Patch up structures and do post-DllMain work here
            // Saves having to reimplement all the complex constructors and data types
            ImportFunctions(symbolMap);
            PrefillVariables(symbolMap, window); // Pre-fill those variables gs/sl initialization relies on

            // Only once
            if (sl::sm_context && sl::sm_context->handles.p_handle_buffer == nullptr) {
                // Pick a capacity that�s plenty for StF (tweak if you learn the real number)
                constexpr uint32_t kHandleCapacity = 0x100000;

                const int ic = sl::initialize();

                if (ic != 0) {
                    // Mirror E_FAIL path used in your implementation; abort init on failure.
                    return false;
                }

                const int rc = sl::handle_initialize(kHandleCapacity);
                if (rc != 0) {
                    // Mirror E_FAIL path used in your implementation; abort init on failure.
                    return false;
                }
            }

            if (!gGeneral.GetSettings()->m_dontApplyPatches)
            {
                // Install hooks re-adding logging
                ReinstateLogging(dll, symbolMap);
                // Install additional "assertions"
                InjectTraps(symbolMap);
                // Region-aware i960 instruction fetch, so the ROM debug menu's RAM
                // trampoline (and any other RAM-resident code) can execute.
                InstallRamExecFetch(dll, symbolMap);
                // Cabinet Test / Service switches on the emulated I/O board's system port,
                // which is what makes the board's own service menu reachable.
                InstallSystemSwitches(dll, symbolMap);
                // ...and one bad byte in the backup RAM the module injects, which hangs the
                // board as soon as that menu's GAME ASSIGNMENTS page is drawn.
                FixBackupRamTimeIndex(dll);
            }

            PatchSl(sl::sm_context);
            PatchGs(gs::sm_context, window);

            return true;
        }
        catch (...)
        {
            // TODO: Show this in native UI
            const std::wstring str(L"Failed to resolve imports and/or patch " + std::wstring(CurrentGame().dll_name) + L"!\n\nIt's either not a valid arcade module DLL from Lost Judgment, "
                "or the game has been updated and YAMP is not forward compatible with that new version.");
            MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);

            return false;
        }

        // (ApplyAspectSetting / DrawPauseMenu moved to ../HostUI.cpp — shared with the YLAD
        // VF2 host.)

        // TODO: Move elsewhere
        static std::filesystem::path gamePath;

        static wil::unique_hmodule gameDll;
        HMODULE m2ftg::LoadDLL()
        {
            const GameDesc& game = CurrentGame();

            // TODO: Clean up
            {
                DWORD dwSize = GetCurrentDirectoryW(0, nullptr);
                auto buf = std::make_unique<wchar_t[]>(dwSize);
                GetCurrentDirectoryW(dwSize, buf.get());
                gamePath.assign(buf.get());
            }

            // Locate the DLL as a FILE first (the CWD, then the "stf"/"fv"/"mr" subdirectory):
            // the integrity check has to run before LoadLibrary, or a module YAMP is about to
            // reject has already run its DllMain by the time we know what it is.
            std::filesystem::path dllFile = gamePath / game.dll_name;
            if (!std::filesystem::is_regular_file(dllFile))
            {
                gamePath.append(game.subdir);
                dllFile = gamePath / game.dll_name;
            }

            if (!std::filesystem::is_regular_file(dllFile))
            {
                const std::wstring str(L"Could not load " + std::wstring(game.dll_name) + L"!\n\nMake sure that YAMP.exe is located next to the DLL file or its \"" + game.subdir + L"\" subdirectory contains it.");
                MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
                return nullptr;
            }

            gGeneral.SetDLLName(WcharToUTF8(game.dll_name));

            // Hard gate: the module's SHA-256 must match a build YAMP knows how to patch, and
            // the parent game must actually be installed. CheckBeforeLoad sets the DLL
            // timestamp for the About panel and explains whichever check failed to the user.
            if (!Verify::CheckBeforeLoad(gGeneral.GetGameId(), dllFile))
            {
                return nullptr;
            }

            gameDll.reset(LoadLibraryW(dllFile.c_str()));
            if (!gameDll)
            {
                const std::wstring str(L"Could not load " + std::wstring(game.dll_name) + L"!\n\nThe file exists but Windows refused to load it.");
                MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
            }

            return gameDll.get();
        }

        void m2ftg::PreInitialize()
        {
            gGeneral.SetDataPath();
            gGeneral.LoadSettings();
        }

        // (The old CheckForExecutable stub — a hardcoded Yakuza6.exe MZ/PE test inherited from the
        // VF5FS host, never enabled here — is gone: Verify::CheckParentGame in LoadDLL does this
        // properly, for the right executable, before the module is loaded.)

        void m2ftg::Run(RenderWindow& window)
        {
            const auto module_start = reinterpret_cast<module_func_t>(GetProcAddress(gameDll.get(), "module_start"));
            THROW_LAST_ERROR_IF_NULL(module_start);
            const auto module_stop = reinterpret_cast<module_func_t>(GetProcAddress(gameDll.get(), "module_stop"));
            THROW_LAST_ERROR_IF_NULL(module_stop);
            module_func_t module_main;

            if (!ResolveSymbolsAndInstallPatches(gameDll.get(), window))
            {
                // Init failed
                DebugLog("[%s::Run] ResolveSymbolsAndInstallPatches FAILED\n", gGeneral.GetGameTag());
                return;
            }
            DebugLog("[%s::Run] patches installed OK\n", gGeneral.GetGameTag());

            // Initialize Criware stub and module stubs
            Cri criware;

            struct sl_module_t
            {
                size_t size = sizeof(decltype(*this));
                sl::context_t* context;
            } sl_module;
            sl_module.context = sl::sm_context;

            struct gs_module_t
            {
                size_t size = sizeof(decltype(*this)); // gs::initialize_module (0x180092fe0) requires 0x58
                gs::context_t* context;
                uint8_t pad[72];
            } gs_module;
            static_assert(sizeof(gs_module_t) == 0x58);
            gs_module.context = gs::sm_context;

            const struct ct_module_t
            {
                size_t size = sizeof(decltype(*this));
                ct_context_t* context = g_ct_context;
            } ct_module;

            struct module_params_t
            {
                size_t size = sizeof(decltype(*this));
                const sl_module_t* sl_module;
                const gs_module_t* gs_module;
                const ct_module_t* ct_module;
                const icri* cri_ptr;
                const char* root_path;
                module_func_t* module_main;
                m2ftg_config_t config{};
            } params;
            static_assert(offsetof(module_params_t, config) == 0x38);

            const std::string utf8Path = gamePath.u8string();

            params.sl_module = &sl_module;
            params.gs_module = &gs_module;
            params.ct_module = &ct_module;
            params.cri_ptr = &criware;
            params.module_main = &module_main;
            params.root_path = utf8Path.c_str();

            const auto* settings = gGeneral.GetSettings();

            // Config values captured from a LIVE Lost Judgment m2ftg minigame (DAT_1801ed490) and
            // field-named via YLAD's symbolized scene ctor: LJ ran kind=2, difficulty=1 (normal),
            // is_freeplay=0 (LJ charges in-game money), is_vs_mode=1 (its minigame boots a credited
            // 2P quick match — see m2ftg.h +0x0A notes). YAMP defaults to the authentic ARCADE
            // boot instead (vs_mode off: attract loop -> coin/start -> 1P ladder); all of these
            // are dip switches in the YAMP settings (module_start copies the config once, so
            // changes need a restart).
            params.config.kind = CurrentGame().kind; // 2 = Sonic the Fighters, 1 = Fighting Vipers
            params.config.difficulty = settings->m_m2Difficulty <= 3 ? static_cast<uint8_t>(settings->m_m2Difficulty) : 1;
            params.config.country = settings->m_m2Country <= 2 ? static_cast<uint8_t>(settings->m_m2Country) : 0;
            params.config.is_freeplay = settings->m_m2Freeplay ? 1 : 0;
            params.config.is_vs_mode = settings->m_m2VersusMode ? 1 : 0;

            // Set up a FPS limiter
            // TODO: Do more gracefully
            int64_t frameTimeTicks;
            {
                // We want to enforce 60 FPS, unless the cap is disabled in Debug
                if (!settings->m_enableFpsCap)
                {
                    frameTimeTicks = 0;
                }
                else
                {
                    LARGE_INTEGER frequency;
                    QueryPerformanceFrequency(&frequency);
                    frameTimeTicks = (frequency.QuadPart * 50) / 3;
                }
            }

            ApplyAspectSetting(window, settings->m_m2Aspect);

            // MUST precede module_start too, and precede ApplyRetarget: the loose-ROM gate is
            // consulted during the archive mount inside module_start, and retarget entries may
            // name symbols that only exist once the ELF is parsed. Path mirrors the loose-ROM
            // layout - rom/<archive stem>/game.elf, next to the ROM images it replaces.
            {
                const GameDesc& game = CurrentGame();
                std::string stem(game.rom_archive_name);
                if (const size_t dot = stem.rfind('.'); dot != std::string::npos) { stem.erase(dot); }
                const std::wstring elfPath = L"rom/" + UTF8ToWchar(stem) + L'/' + ElfRom::OVERRIDE_FILE_NAME;
                // Only parse it when it can actually be served. The ELF reaches the i960 solely
                // through the loose-ROM file path (file_access.cpp swaps it in for an open of
                // rom_code1.bin), which exists only while the archive bypass is on. Loading it
                // regardless left IsLoaded() true with the game running from the .par: the
                // 960STAT panes then relabel the real ROM with the homebrew's symbols (every
                // address past its _etext collapses onto that one marker), and the HLE retarget
                // resolves against a program that is not the one executing.
                if (!settings->m_stfLooseRomFiles)
                {
                    if (GetFileAttributesW(elfPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        DebugLog("[%s::Run] '%ls' ignored: loose ROM files are off, the archive supplies the program ROM\n",
                            gGeneral.GetGameTag(), elfPath.c_str());
                    }
                }
                else if (ElfRom::Load(elfPath, ElfRom::PROGRAM_ROM_SIZE))
                {
                    DebugLog("[%s::Run] program ROM overridden by '%ls'\n", gGeneral.GetGameTag(), elfPath.c_str());
                }
            }

            // MUST precede module_start: board bring-up is what runs the HLE hook installer, and
            // the installer stamps its 76 traps at Sonic the Fighters' own ROM offsets whatever
            // program ROM is actually loaded. For a homebrew rom_code1.bin those offsets are 76
            // unrelated instructions, corrupted before the i960 runs one of them - too early for
            // the per-frame reconcile in HleHooks::Update() to help. Retargeting the installer's
            // table here is the only place the damage can be avoided rather than repaired.
            HleHooks::ApplyRetarget(settings->m_stfHleRetarget);

            // Kick off the game
            DebugLog("[%s::Run] calling module_start...\n", gGeneral.GetGameTag());
            const auto msRet = module_start(sizeof(params), &params);
            // The module renders into a fixed 1024x768 texture whatever its own resolution option
            // selected, so tell the compositor which sub-rect of it actually holds the frame. Read
            // after module_start: that is when the module's parse ran, and a switch on YAMP's command
            // line can differ from the setting.
            {
            	uint32_t srcW = 0, srcH = 0;
            	if (ModuleArgs::ResolvedRenderSize(srcW, srcH))
            	{
            		const_cast<RenderWindow&>(window).SetModuleSourceRect(srcW, srcH);
            	}
            }
            DebugLog("[%s::Run] module_start returned %lld\n", gGeneral.GetGameTag(), (long long)msRet);

            // Must follow module_start: board bring-up is what fills the memory-map table this
            // wraps. Off unless [Debug] LogHardwareWrites names regions.
            CharRamFix::Install();
            if (msRet == 0)
            {
                LARGE_INTEGER lastTime;
                QueryPerformanceCounter(&lastTime);
                // "-frames N" ends the run HERE rather than by killing the process, so smoke tests
                // take the real teardown path (see YAMPGeneral::GetFrameLimit).
                const uint32_t frameLimit = gGeneral.GetFrameLimit();
                uint32_t framesRun = 0;
                while (!window.IsShuttingDown())
                {
                    DebugLog("[%s::Run] GameLoop iter\n", gGeneral.GetGameTag());
                    if (!GameLoop(module_main, window)) { DebugLog("[%s::Run] GameLoop returned false\n", gGeneral.GetGameTag()); break; }
                    if (frameLimit != 0 && ++framesRun >= frameLimit)
                    {
                        DebugLogFile("[%s::Run] frame limit %u reached, shutting down\n",
                            gGeneral.GetGameTag(), frameLimit);
                        break;
                    }

                    // TODO: Waitable timer
                    LARGE_INTEGER currentTime;
                    do
                    {
                        QueryPerformanceCounter(&currentTime);
                    } while (((currentTime.QuadPart - lastTime.QuadPart) * 1000) < frameTimeTicks);
                    lastTime = currentTime;
                }

                // Tell the module to shut down. Completes the start/main/stop protocol instead of
                // relying on process exit, and gives the module a chance to release what it
                // allocated. VF5FS-LJ returns 0 here; the m2ftg modules are logged the same way.
                const int stopResult = module_stop(0, nullptr);
                DebugLogFile("[%s::Run] module_stop -> 0x%X\n", gGeneral.GetGameTag(), stopResult);
            }
        }

        // The Escape pause menu shell lives in ../HostUI.cpp (DrawPauseMenu). RE ground truth
        // (2026-07-26) kept here: LJ's menu SHELL (View Controls / Button/Key Configuration /
        // Quit) is HOST UI — the strings exist nowhere in the DLL, the DLL imports no keyboard
        // APIs, and the DLL's own internal menu (TaskCsMenu/TaskCmdList singleton @0x18058ab00
        // with MENU_EXPLAIN/MENU_CONTROL panels) is dead code in the LJ build: it is constructed
        // at DLL init but never registered into the task list (verified by a full rip-relative
        // reference scan — ctor + dtor thunk only). What IS the module's own handling is the
        // PAUSE: status bit0 makes module_main freeze the emulation, pause all 6 audio channels
        // (FUN_180043020) and keep presenting the last completed frame; clearing the bit resumes.
        // So, like LJ, the host draws the menu and the module handles the pause.

        bool m2ftg::GameLoop(module_func_t func, RenderWindow& window)
        {
            // Persistent input/arcade state (LJ keeps these in the scene object across frames):
            // s_startScreen  <- module status bit6 from last frame (scene+0x2B58, "press start" active)
            // s_coinPending  <- a credit was inserted, START injection phase   (scene+0x2B59)
            // s_startToggle  <- alternate-frame START injector                 (scene+0x2B5A)
            static csl_pad s_pads[2];
            // Hoisted so the input/status code below can tell whether a netplay round is live.
            // UINT32_MAX = not in a round.
            static uint32_t s_netplayFrame = UINT32_MAX;
            const bool netplayMatch = net::IsAvailable() && s_netplayFrame != UINT32_MAX;
            // Which pad slot this machine drives on the wire: 0 = host, 1 = guest, -1 = local play.
            const int32_t netplayLocalPad =
                net::IsAvailable() ? net::Api()->get_local_player(net::Session()) : -1;
            // Every host->module input that is NOT part of the transmitted pad has to be dead for
            // the whole session, not merely during a live round. The window between pressing Start
            // and frame 0 is the dangerous one: the board is reset and then runs FREELY until it
            // reaches the anchor, so a coin or a service switch pressed in there changes this
            // machine's state before the round begins - and the anchor only aligns the ROM's frame
            // counter, not the rest of the board. Using the session predicate closes that window
            // and covers the lobby too.
            const bool netplayLocked = net::SessionInProgress();

            static bool s_startScreen = false;
            static bool s_coinPending = false;
            static bool s_startToggle = false;

            const auto* settings = gGeneral.GetSettings();

            // Live-apply the aspect-ratio setting (the ImGui Apply button only writes the settings
            // struct; this poll is what makes the change take effect without a restart).
            {
                static uint32_t s_lastAspect = UINT32_MAX;
                if (settings->m_m2Aspect != s_lastAspect)
                {
                    s_lastAspect = settings->m_m2Aspect;
                    ApplyAspectSetting(window, s_lastAspect);
                }
            }

            // PERSISTENT across frames (confirmed via YLAD's symbolized driver
            // cscene_minigame_m2ftg::method_pre_render @0x1426a78b0: the execute_info is embedded in
            // the scene and only these fields are refreshed per frame — the +0x1660 "work" region is
            // module-visible state (game kind + assigns + event payloads) that must NOT be wiped).
            static m2ftg_execute_info_t execute_info{};
            execute_info.size_of_struct = sizeof(execute_info);
            execute_info.p_device_context = gs::sm_context->p_device_context;
            execute_info.status = 0;          // host zeroes each frame, then ORs pause/coin bits
            execute_info.output_texid = 0;
            execute_info.result = 0x80004005; // LJ presets E_FAIL; module_main overwrites
            // Master volume: the module's own field, so its mixer does the attenuation (see
            // YAMPSettings::m_volumePercent). 100% = 1.0f, exactly what this host passed before.
            execute_info.sound_volume =
                static_cast<float>(gGeneral.GetSettings()->m_volumePercent) / 100.0f;

            // Escape toggles the pause menu; while it is open, drive the MODULE'S OWN pause path
            // (status bit0 — see DrawPauseMenu's RE notes). Escape is reserved for this, like
            // in LJ, and is no longer a game button on the StF keyboard layout (sl.cpp).
            static bool s_pauseMenuOpen = false;
            {
                // DISABLED DURING NETPLAY. Pausing is host->module input like any other: the
                // module's pause path (status bit0) stops the emulated board, and stopping it on
                // one machine only desyncs the pair. It also suppresses the per-frame submit and
                // the upload-stamp advance below, so an "only cosmetic" pause would still change
                // how much work this side does. There is no per-player pause in an arcade match;
                // the honest behaviour is for Escape to do nothing while a session is up.
                if (netplayLocked)
                {
                    s_pauseMenuOpen = false;   // close it if a session started while it was open
                }

                static bool s_escWasDown = false;
                const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
                if (escDown && !s_escWasDown && !netplayLocked)
                {
                    s_pauseMenuOpen = !s_pauseMenuOpen;
                }
                s_escWasDown = escDown;
            }
            if (s_pauseMenuOpen)
            {
                execute_info.status |= 1;
            }

            // Input: refresh the shared XInput snapshot, evaluate each player's bindings via
            // csl_pad (Input, set up on the YAMP Controls page), then copy the shared
            // 0xE0-byte prefix into the m2ftg pad blocks (same pxd sl layout, same button bits).
            Input::PollPads();
            if (netplayMatch && netplayLocalPad >= 0)
            {
                // ONLINE, YOU ALWAYS PLAY AS PLAYER 1 LOCALLY. The guest occupies pad slot 1 in
                // the match, but they are the only person at that keyboard/controller and have
                // their own Player 1 bindings set up - making them remap to the Player 2 controls
                // just to play online would be absurd. So this machine's P1 bindings drive
                // whichever slot this machine owns on the wire.
                //
                // The other slot is filled from P2 purely to keep it well-formed for this frame:
                // step() overwrites BOTH pads from the transmitted inputs before module_main sees
                // them, so nothing local survives into the simulation (see PadCodec.h).
                s_pads[netplayLocalPad].set_state(0);
                s_pads[1 - netplayLocalPad].set_state(1);
            }
            else
            {
                s_pads[0].set_state(0);
                s_pads[1].set_state(1);
            }
            for (int i = 0; i < 2; i++)
            {
                memcpy(&execute_info.pad[i], &s_pads[i], 0xE0);
                execute_info.pad[i].m_port = static_cast<unsigned int>(i);
                execute_info.pad[i].m_user_id = i;
                execute_info.pad[i].m_is_connected = true;
            }

            // Button assignments (host->module; LJ fills these from player settings). Slot order
            // is the module's own (slot template @DLL 0x180126770): A, B, Y, X, LT, LB, RT, RB.
            // Ours are a FIXED table: remapping happens host-side in csl_pad::set_state, which
            // routes each player's bound inputs onto the button bit carrying the wanted combo.
            for (int p = 0; p < 2; p++)
            {
                for (int i = 0; i < 8; i++)
                {
                    execute_info.assign[p][i] = Input::MODULE_ASSIGN[i];
                }
            }

            // Dedicated coin binding: a press becomes the coin status bit (host->module bit5,
            // same bit LJ's start/coin protocol below injects). Meaningless in freeplay, and
            // swallowed while the pause menu is open like the rest of the inputs.
            {
                static bool s_coinWasDown[2] = {};
                for (int p = 0; p < 2; p++)
                {
                    const bool down = Input::ActionDown(p, Input::Action_Coin);
                    // Not transmitted, so it cannot be honoured during netplay without desyncing:
                    // it would set the coin status bit on one machine only. It is also the one
                    // input a bored opponent can hold down to make noise on both screens. Players
                    // use START during netplay, which travels in the synchronised pad.
                    if (down && !s_coinWasDown[p] && !settings->m_m2Freeplay && !s_pauseMenuOpen
                        && !netplayLocked)
                    {
                        execute_info.status |= 0x20;
                    }
                    s_coinWasDown[p] = down;
                }
            }

            // Cabinet service panel (see InstallSystemSwitches): TEST opens the board's own
            // service menu - the operator's input test - and SERVICE is the credit / navigate
            // button beside it. Held lines, like the panel; the ROM decides what latches. Either
            // player's binding closes the one switch.
            //
            // Suppressed while paused, and for the whole of a netplay session: neither is in the
            // transmitted pad, so honouring them would drop one machine into the operator's menu
            // (or feed it a service credit) while the other carried on playing.
            {
                bool test = false;
                bool service = false;
                if (!s_pauseMenuOpen && !netplayLocked)
                {
                    for (int p = 0; p < 2; p++)
                    {
                        test = test || Input::ActionDown(p, Input::Action_Test);
                        service = service || Input::ActionDown(p, Input::Action_Service);
                    }
                }
                SetSystemSwitches(test, service);
            }

            // Arcade coin/start protocol (LJ FUN_142494450): while the module shows the start
            // screen (status bit6 out), a START press becomes a COIN INSERT (status bit5 in, the
            // press itself is swallowed); afterwards START is injected on alternating frames until
            // the module leaves the start screen. ONLY meaningful with the freeplay dip switch
            // OFF — with is_freeplay=1 the module takes START directly and there is no coin to
            // insert; the dance would just swallow the player's first press. Follows the same
            // setting Run passed to module_start as config.is_freeplay (changing it needs a
            // restart, so the two always agree within a session).
            // During a netplay round this runs AFTER the gate instead (see RunCoinStartProtocol
            // below): it both reads and writes pad[0], and step() replaces the pads wholesale, so
            // running it here would read the LOCAL pad and set the coin status bit from it - on one
            // machine only. That was the desync: identical inputs, different execute_info.status.
            const bool isFreeplay = settings->m_m2Freeplay;
            const bool runProtocolNow = !isFreeplay && !s_pauseMenuOpen && !netplayMatch;
            if (runProtocolNow)
            {
                if (s_coinPending && s_startScreen)
                {
                    if (!s_startToggle)
                    {
                        execute_info.pad[0].m_now |= 0x100;
                        execute_info.pad[0].m_push |= 0x100;
                    }
                    s_startToggle = !s_startToggle;
                }
                else
                {
                    s_coinPending = false;
                    if (s_startScreen && (execute_info.pad[0].m_now & 0x100) != 0)
                    {
                        execute_info.status |= 0x20;
                        execute_info.pad[0].m_now &= ~0x100u;
                        execute_info.pad[0].m_push &= ~0x100u;
                        s_coinPending = true;
                        s_startToggle = false;
                    }
                }
            }

            window.BeginFrame();
            window.NewImGuiFrame();
            if (s_pauseMenuOpen)
            {
                if (!DrawPauseMenu(window, s_pauseMenuOpen))
                {
                    return false; // Quit picked
                }
            }

            // The CBV/SRV descriptor-copy rings are per-frame transient; reset their cursors before the
            // DLL's render (func) records this frame, or they grow past the heap -> CopyDescriptors AV.
            ResetCbvSrvRingCursors(gs::sm_context);

            // ---- NETPLAY SESSION DRIVER ----------------------------------------------------
            // UINT32_MAX means "not in a netplay round"; anything else is the frame counter the
            // lockstep engine keys inputs by. It must start at 0 for a round and advance ONLY on
            // frames the emulator actually ran, which is why it is bumped next to module_main
            // rather than once per loop iteration.
            // ROUND-START SEQUENCE. Getting two emulators to start a round from the SAME state
            // needs more than a barrier, because a barrier only proves both peers are present.
            // Measured 2026-08-01: resetting the board and starting frame 0 in the same instant
            // left the ROM ~40 frames into its post-reset boot when the match began, and the two
            // machines reached the ROM's main loop one emulated frame apart - identical inputs
            // applied one ROM frame out of step, which compounds into a visible AI desync.
            //
            // So frame 0 is anchored to the ROM's OWN progress instead of to a wall-clock moment:
            //   Idle      -> reset the board, pin the texture budget
            //   Resetting -> wait for frame_counter to restart (proving the reset landed)
            //   Settling  -> wait for frame_counter to reach ANCHOR - the same value on both peers
            //   Announced -> HOLD the emulator here and open the barrier
            //   (barrier)  -> seed the RNG (instantaneous, so it needs no settling) and run frame 0
            // Both machines therefore enter frame 0 with their ROM at the same counter value, by
            // construction rather than by luck.
            enum class RoundPrep { Idle, Resetting, Settling, Announced };
            static RoundPrep s_prep = RoundPrep::Idle;
            static uint32_t s_anchorCounter = 0;
            // A few frames into the ROM's main loop: far enough past the post-reset boot to be
            // steady state, small enough that starting a match stays snappy.
            constexpr uint32_t NETPLAY_ANCHOR = 8;
            // The ROM's own per-frame counter in emulated RAM - and the SAME address in both
            // games. That is not the coincidence it looks like: Sonic the Fighters was built on
            // Fighting Vipers' engine and inherited its low-RAM global block. The rest of the
            // layout did not come across (the module reads 0x50004C / 0x500064 / 0x5000A2 /
            // 0x50016C / 0x500248 / 0x500704 in FV and none of those in StF), so this was
            // measured rather than assumed: both games were logged advancing it by exactly 1
            // per module_main call, over two sample bursts 340 frames apart.
            constexpr uint32_t RVA_ROM_FRAME_COUNTER = 0x500020;
            // Netplay needs the whole determinism set - reset, RNG seeding, texture-budget pin
            // and this counter - which is exactly the set of games DebugWindows describes.
            const bool netplayPossible = gGeneral.GetGameId() == YAMPGeneral::GameId::StF
                || gGeneral.GetGameId() == YAMPGeneral::GameId::FV;

            static bool s_netplayRoundRequested = false;
            bool holdForBarrier = false;
            if (net::IsAvailable())
            {
                const yampnet_api* api = net::Api();
                yampnet_session* sess = net::Session();
                api->poll(sess);
                net::DriveSession();   // connect -> discovery -> host/join (idempotent)

                const yampnet_state ns = api->get_state(sess);

                // Room is up but no round yet: open the barrier. Both peers do this; the barrier
                // is what makes them agree on when frame 0 is.
                // Room is up, but only start when we are allowed to: the command-line harness
                // auto-starts, a UI session waits for the host's Start button.
                // IsBoardBooted is a HARD precondition, not a nicety. Announcing before the board
                // is up means the barrier can release while ResetBoard/SeedHostRng/
                // SetTextureBudgetDeterministic are all still no-ops on this machine, and the
                // round then starts with this peer's simulation un-reset and un-seeded. Because
                // every peer gates its own announce, the barrier itself comes to mean "both
                // boards are ready", which is what it was always supposed to mean.
                if (ns == YAMPNET_STATE_IN_ROOM && !s_netplayRoundRequested
                    && netplayPossible && net::ShouldStartRound() && IsBoardBooted())
                {
                    if (s_prep == RoundPrep::Idle)
                    {
                        // Reset first, then let the ROM come back up on its own. The texture
                        // budget is pinned NOW rather than at frame 0 so the post-reset boot runs
                        // under the same rules on both machines too.
                        const bool didReset = ResetBoard();
                        const bool didBudget = SetTextureBudgetDeterministic(true);
                        if (didReset && didBudget)
                        {
                            s_prep = RoundPrep::Resetting;
                            net::Logf("board reset; waiting for the ROM to restart");
                        }
                        else
                        {
                            net::Logf("ABORT: board reset failed (reset=%d texBudget=%d)",
                                      static_cast<int>(didReset), static_cast<int>(didBudget));
                            net::ClearStartRequest();
                        }
                    }

                    uint32_t romFrame = 0;
                    const bool haveRomFrame =
                        ReadEmulatedRam32(RVA_ROM_FRAME_COUNTER, romFrame);

                    // The counter still holds its PRE-reset value for a while, so "it is small
                    // again" is what proves the reset actually landed. Anchoring without this
                    // check would latch onto a stale value that differs per machine.
                    if (s_prep == RoundPrep::Resetting && haveRomFrame && romFrame <= 1)
                    {
                        s_prep = RoundPrep::Settling;
                    }

                    if (s_prep == RoundPrep::Settling && haveRomFrame && romFrame >= NETPLAY_ANCHOR)
                    {
                        yampnet_match_config mc = {};
                        mc.frame_delay = static_cast<uint8_t>(
                            settings != nullptr && settings->m_netFrameDelay > 0
                                ? settings->m_netFrameDelay : 3);
                        mc.input_redundancy = 10;
                        mc.stall_timeout_ms = 10000;
                        // Round number: counts up per round so late packets from the PREVIOUS
                        // round cannot poison this one. A guest's count would drift from the
                        // host's (neither knows how many rounds the other played), so the plugin
                        // makes the HOST authoritative - a guest adopts whatever round the host
                        // announces, exactly as it adopts the seed. Wraps at 32 (5 bits on the
                        // wire), which is harmless: adoption keeps both sides on the same value.
                        static uint32_t s_roundNumber = 0;
                        ++s_roundNumber;
                        if (api->begin_round(sess, s_roundNumber, &mc) == YAMPNET_OK)
                        {
                            s_prep = RoundPrep::Announced;
                            s_anchorCounter = romFrame;
                            s_netplayRoundRequested = true;
                            net::Logf("anchored at ROM frame_counter=%u; barrier opened, emulator "
                                      "held until the peer arrives", romFrame);
                        }
                    }
                }

                // Announced but the barrier has not released: FREEZE the emulator. Without this
                // the ROM keeps running while we wait for the peer, and the anchor we just took
                // is meaningless by the time the round actually starts - the whole point is that
                // both machines sit at the same counter value when frame 0 begins.
                holdForBarrier = (s_prep == RoundPrep::Announced
                                  && ns != YAMPNET_STATE_IN_MATCH
                                  && s_netplayFrame == UINT32_MAX);

                // Barrier released: frame 0 starts now.
                if (ns == YAMPNET_STATE_IN_MATCH && s_netplayFrame == UINT32_MAX)
                {
                    s_netplayFrame = 0;
                    const uint32_t seed = api->get_match_seed(sess);
                    const int32_t me = api->get_local_player(sess);

                    // The board was already reset and settled at the anchor above; all that is
                    // left is the RNG. Seeding writes the Mersenne Twister state directly and
                    // consumes no emulated frames, which is exactly why it can happen here while
                    // the reset could not: the ROM's `rand` is HLE'd onto a host generator that is
                    // otherwise seeded per process, so without this each machine rolls its own
                    // numbers and the CPU behaves differently on each client with identical inputs.
                    const bool didSeed = SeedHostRng(seed);

                    uint32_t romFrame = 0;
                    ReadEmulatedRam32(RVA_ROM_FRAME_COUNTER, romFrame);
                    net::Logf("match start: player=%d seed=0x%08X rngSeeded=%d anchor=%u romFrame=%u",
                              me, seed, static_cast<int>(didSeed), s_anchorCounter, romFrame);

                    // The RNG is the one step left that can still fail here, and a match with an
                    // unseeded generator is guaranteed to diverge. Refuse it rather than play it.
                    if (!didSeed)
                    {
                        net::Logf("ABORT: RNG seeding failed on this peer - refusing the round "
                                  "rather than desyncing");
                        SetTextureBudgetDeterministic(false);
                        net::ClearStartRequest();
                        api->end_round(sess);
                        s_netplayRoundRequested = false;
                        s_prep = RoundPrep::Idle;
                        s_netplayFrame = UINT32_MAX;
                    }
                }

                // Back to a state with no round in it - the session ended, or the lobby left the
                // room. Re-arm: the next round has to announce again, and the Start press that
                // opened the last barrier must not carry over into it.
                if (ns == YAMPNET_STATE_IDLE || ns == YAMPNET_STATE_FAILED
                    || ns == YAMPNET_STATE_ONLINE)
                {
                    if (s_netplayRoundRequested || s_prep != RoundPrep::Idle)
                    {
                        net::ClearStartRequest();
                        // Un-pin the budget: it was pinned as part of a round that is not
                        // happening, and leaving it on would slow local play for no reason.
                        SetTextureBudgetDeterministic(false);
                    }
                    s_netplayRoundRequested = false;
                    s_prep = RoundPrep::Idle;
                    s_netplayFrame = UINT32_MAX;
                }
            }

            // NETPLAY GATE. Under lockstep the emulator may only advance once every player's input
            // for this frame is known, so module_main is skipped entirely on a stall - that skip IS
            // the stall. The plugin also overwrites BOTH pads with the transmitted inputs (its own
            // included), which is why this runs after the local pad fill above: whatever csl_pad
            // produced is a local prediction that the network's copy must replace, or the two
            // machines simulate different inputs and desync.
            // The on-screen status overlay that used to live here is now drawn by the UI layer
            // (YAMPUserInterface::DrawNetplayOverlay), next to the lobby whose state it reports -
            // it has to cover the command-line and lobby paths alike, and it must not be a second
            // place where session state is interpreted.

            // NOTE: an earlier version HELD the emulator at frame 0 until the barrier released, to
            // stop the host drifting through attract mode while it waited. That was wrong, and
            // self-defeating: the board only reports "booted" (+0x6B9300 == 2) once module_main has
            // actually run, so holding meant the board never booted, and ResetBoard() - which is
            // gated on that - silently did nothing (the log showed reset=0). Both machines then
            // started a round with no reset AND no shared state.
            // The reset is the stronger mechanism, so the emulator now runs freely until the
            // barrier and both sides are re-initialised at match start instead. Pre-barrier
            // divergence does not matter when the board is about to be reset out from under it.
            // Held at the anchor while the barrier waits for the peer. Same mechanism as a
            // lockstep stall - module_main simply does not run - so the last frame keeps being
            // re-presented and the overlay explains the wait.
            bool advanceFrame = !holdForBarrier;
            if (net::IsAvailable() && s_netplayFrame != UINT32_MAX)
            {
                const yampnet_step st =
                    net::Api()->step(net::Session(), s_netplayFrame, &execute_info);
                advanceFrame = (st == YAMPNET_STEP_READY);

                if (st == YAMPNET_STEP_TIMEOUT || st == YAMPNET_STEP_DISCONNECTED)
                {
                    net::Logf("round ended (step=%d); returning to local play",
                              static_cast<int>(st));
                    // Tell the player. Both ends of a lost match see this: the peer that went
                    // away is gone, and the one still running would otherwise just find itself
                    // back in attract mode with no explanation.
                    net::NotePeerLost(st == YAMPNET_STEP_TIMEOUT
                                          ? "The other player stopped responding."
                                          : "The other player disconnected.");
                    SetTextureBudgetDeterministic(false);
                    net::ClearStartRequest();
                    net::Api()->end_round(net::Session());
                    s_netplayFrame = UINT32_MAX;   // back to local play
                    // end_round leaves us IN_ROOM, so re-arm the request flag or Start match would
                    // never open a second barrier.
                    s_netplayRoundRequested = false;
                    s_prep = RoundPrep::Idle;
                    advanceFrame = true;
                }

                // Divergence detected. Stop AT the divergence rather than playing on: both
                // screens then hold the last frame the two machines still agreed on, and the
                // frame number in the log is the one to investigate. Continuing would only pile
                // consequences on top of the cause.
                uint32_t dFrame = 0, dLocal = 0, dRemote = 0;
                if (net::Api()->get_desync(net::Session(), &dFrame, &dLocal, &dRemote) != 0)
                {
                    net::Logf("DESYNC at frame %u (local frame_counter=%u, peer=%u) - ending the "
                              "round; the two emulators stopped agreeing here",
                              dFrame, dLocal, dRemote);
                    SetTextureBudgetDeterministic(false);
                    net::ClearStartRequest();
                    net::Api()->end_round(net::Session());
                    s_netplayFrame = UINT32_MAX;
                    s_netplayRoundRequested = false;
                    s_prep = RoundPrep::Idle;
                    advanceFrame = true;
                }
            }

            // The host zeroes output_texid every frame and module_main fills it in. On a stalled
            // frame module_main never runs, so it would stay 0 - and 0 is not a valid handle (the
            // pxd handle table is 1-based and asserts on get(0)). Remember the last good id so a
            // stall simply re-presents the previous frame.
            static unsigned int s_lastOutputTexId = 0;

            // Now that step() has written BOTH pads from the transmitted inputs, run the arcade
            // coin/start protocol off that synchronised state. Every input it consumes (pad[0]
            // START, and s_startScreen from the module's own status output) is identical on both
            // machines now, so the coin status bit it raises is identical too.
            if (netplayMatch && !isFreeplay)
            {
                if (s_coinPending && s_startScreen)
                {
                    if (!s_startToggle)
                    {
                        execute_info.pad[0].m_now |= 0x100;
                        execute_info.pad[0].m_push |= 0x100;
                    }
                    s_startToggle = !s_startToggle;
                }
                else
                {
                    s_coinPending = false;
                    if (s_startScreen && (execute_info.pad[0].m_now & 0x100) != 0)
                    {
                        execute_info.status |= 0x20;
                        execute_info.pad[0].m_now &= ~0x100u;
                        execute_info.pad[0].m_push &= ~0x100u;
                        s_coinPending = true;
                        s_startToggle = false;
                    }
                }
            }

            int funcResult = 0;
            if (advanceFrame)
            {
                // GAME ASSIGNMENTS -> DAMAGE, held at the dip switch's (or the room's) value.
                // Deliberately inside the advanceFrame branch and immediately before the call:
                // this writes emulated RAM, so under lockstep it must happen exactly once per
                // EMULATED frame on both machines. Driving it from the UI thread - where the other
                // live setting, the debug flag, is driven - would tie it to host frame pacing
                // instead, and a write that lands a frame earlier on one peer than the other is a
                // divergence even when the value is identical.
                UpdateDamageAssignment();

                // Bracket the DLL's render so the ResourceBarrier hook corrects StF's barrier
                // StateBefore values (ping-pong RTs assume last-frame state; YAMP creates in
                // COMMON -> id=527 desync).
                SetModuleRenderActiveNow(true);
                funcResult = func(sizeof(execute_info), &execute_info);
                SetModuleRenderActiveNow(false);

                if (execute_info.output_texid != 0)
                    s_lastOutputTexId = execute_info.output_texid;

                if (s_netplayFrame != UINT32_MAX)
                {
                    // Desync canary. The ROM's frame_counter advances exactly once per emulated
                    // frame (measured; see the note above), so at a given netplay frame it MUST
                    // read the same on both machines. It is the cheapest value that is both
                    // guaranteed-equal and sensitive: it stops advancing in lockstep the moment
                    // one side executes a different amount of game code.
                    uint32_t stateCheck = 0;
                    if (ReadEmulatedRam32(RVA_ROM_FRAME_COUNTER, stateCheck))
                    {
                        net::Api()->submit_state_check(net::Session(), s_netplayFrame, stateCheck);
                    }
                    ++s_netplayFrame;
                }
            }
            else
            {
                // Stalled: show the frame we already have.
                execute_info.output_texid = s_lastOutputTexId;
            }

            // Module->host feedback: bit6 = "press start" screen active (LJ mirrors it to
            // scene+0x2B58 and gates the coin/start injection on it).
            s_startScreen = (execute_info.status & 0x40) != 0;

            // One-time state trace to correlate with the arcade flow (status out, event channel).
            {
                static int s_frame = 0;
                static int s_lastLogged = -1;
                const int interesting = (execute_info.status & ~0x20) | (execute_info.work_kind << 16);
                if (interesting != s_lastLogged && s_frame < 2000)
                {
                    DebugLog("[m2ftg] frame=%d status=0x%X result=0x%X event=%d texid=%u\n",
                        s_frame, execute_info.status, execute_info.result,
                        execute_info.work_kind, execute_info.output_texid);
                    s_lastLogged = interesting;
                }
                s_frame++;
            }

            if (funcResult != 0) return false;

            // The whole frame (func() + the module's own internal task pump) is now recorded:
            // close/execute/flush it in ONE submit. StF's per-frame GPU submit is invoked internally by
            // module_main's render stages through the installed submit-handler hook (DAT_18058aad0), so
            // the host must NOT call g_stfFrameSubmit itself — doing so ran the task pump (the Model 2
            // emulator step) twice per frame and the whole game, timers included, ran ~2x.
            // PAUSED (LJ ground truth, C:\temp\menu PIX captures 2026-07-26): while the pause menu is
            // open LJ drops the module's command list from ExecuteCommandLists entirely (10 lists -> 9,
            // zero module draws, zero ResolveSubresource, no module upload traffic; the CRT pass keeps
            // re-sampling the LAST resolved frame, which receives no barriers). With status bit0 set the
            // module records nothing anyway, so skip the close/execute/reopen dance and the upload-stamp
            // advance — submit nothing, exactly like LJ.
            if (!s_pauseMenuOpen) SubmitModuleFrameListNow();
            if (ModuleExecDisabledNow())
            {
                // A submit hung/removed the GPU — DRED was dumped. Stop now so StF's next-frame upload
                // allocation doesn't crash on the dead device (that cascade was masking the real fault).
                DebugLog("[%s::Run] submit hang/device-removed -> stopping loop (see [DRED])\n", gGeneral.GetGameTag());
                return false;
            }

            // The other half of the host's per-frame job (twin of SubmitModuleFrameListNow): advance the
            // upload-frame stamp + recycle StF's upload buffers (in-use -> available). Runs AFTER the flush
            // above, so every recycled buffer is GPU-complete. This is the fix for the upload-pool
            // exhaustion crash (FUN_18009be60, ~frame 570). Must precede StF's next-frame func().
            if (!s_pauseMenuOpen) AdvanceFrameStampNow();

            // Measured 2026-08-01 (netplay determinism survey): the ROM's own counters in work RAM
            // advance EXACTLY once per module_main call - frame_counter (0x500020) +1 per frame,
            // CTRL_TIMER (0x500024) -1 per frame - so they are pure simulation state, driven by the
            // emulated CPU and not by any host clock. Nothing about them needs synchronising, and a
            // stalled netplay frame cannot skew them because it skips module_main entirely on both
            // peers. See m2ftg::ReadEmulatedRam32 if this needs re-measuring.

            // texid 0 means "nothing rendered yet" - only reachable when netplay stalls before the
            // very first frame. It is NOT a lookup failure, so it must not go through handle_tex
            // (1-based, asserts on 0) and must not quit the loop the way a genuine failure does.
            cgs_tex* display_tex = (execute_info.output_texid != 0)
                ? gs::sm_context->handle_tex.get(execute_info.output_texid)
                : nullptr;
            if (execute_info.output_texid != 0)
            {
                if (display_tex == nullptr) return false;
                if (display_tex->m_type != 2) return false;
            }

            if (display_tex != nullptr)
            {
                // StF renders DX12-native; the DX11-typed fields (m_pD3DShaderResourceView / m_pD3DResource)
                // are null on this path. The output ID3D12Resource lives at sbgl_resource+0x98 (found via QI:
                // a 1024x768 B8G8R8A8 Texture2D). Wrap it through 11on12 into a D3D11 SRV and blit it.
                auto* res = display_tex->mp_sbgl_resource;
                ID3D12Resource* rt = res
                    ? *reinterpret_cast<ID3D12Resource**>(reinterpret_cast<uint8_t*>(res) + 0x98)
                    : nullptr;
                window.BlitDX12Texture(rt);
                window.RenderImGui();
            }
            else
            {
                // No game frame to composite yet; still draw the overlay so the window is alive
                // rather than looking hung while we wait for the peer.
                window.RenderImGui();
            }
            window.EndFrame();

            // Present using the swapchain the game already created (the same object the 11on12
            // backbuffers were wrapped from — gs::sm_context's sbgl_device holds YAMP's swapchain).
            auto& swapChain = gs::sm_context->sbgl_device.m_swap_chain;
            HRESULT hr = swapChain.m_pDXGISwapChain->Present(1, 0);
            if (FAILED(hr)) return false;

            gs::sm_context->frame_counter++;
            return true;
        }
    }

