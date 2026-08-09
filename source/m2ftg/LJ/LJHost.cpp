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
#include "../Debug/HleHooks.h"
#include "../Debug/DebugWindows.h"
#include "../NetSession.h"
#include "../SystemSwitches.h"
#include "../ELF/CharRamFix.h"
#include "MrLink.h"
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

            // Which BUILD of the module this is, before anything reads a per-build RVA or a pxd
            // context field. Sonic the Fighters ships in Lost Judgment and in Like a Dragon
            // Gaiden; the ROM and every asset are byte-identical, but the DLL is a 2023 rebuild
            // whose globals moved and whose pxd gs context is 0x3898C0 rather than 0x388A00
            // (see ../ModuleBuild.h and ../../pxd/LJ/gs.h).
            SetCurrentModuleBuild(ModuleTimestamp(dll));
            gs::sm_is_gaiden_layout = IsGaidenBuild();
            // The host-provided cgs_device_context slots moved too, and NOT by a single uniform
            // shift the way the gs context did (+0x18/+0x20/+0x28 in three different regions).
            gs::sm_dc = IsGaidenBuild() ? gs::DC_OFFSETS_GAIDEN : gs::DC_OFFSETS_LJ;
            DebugLogFile("[%s] module build 0x%08X%s\n", gGeneral.GetGameTag(),
                CurrentModuleBuild(), IsGaidenBuild() ? " (Like a Dragon Gaiden)" : "");

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

                // The sl context is 0xF000 in the Lost Judgment / Y:LAD modules and 0xF140 in the
                // Gaiden one - the SAME layout grown at the tail (allocated_heap is still at
                // 0xEFD0 in both, and the archive lock word is still reached as sl_ctx+0x1D00),
                // so only the size the module's own initialize_module checks at +8 differs.
                const int ic = sl::initialize(IsGaidenBuild() ? 0xF140 : 0xF000);

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
                //
                // MOTOR RAID does not share the StF/FV I/O core the pattern install finds — its
                // refresh is FUN_180054c00, and the two calls to it live in the frame driver
                // FUN_18005c030 (which IS the retail driver: its gate DLL+0x741CB7 reads as
                // "machine running", not "linked mode" — settled 2026-08-09 when the io probe
                // showed the refresh rebuilding io[9] every module_main). Same io[9] bits 2/3,
                // same post-refresh hook; only the addresses differ, hardcoded here because
                // GameVerify pins this exact build. NOT multiplexed: MR's second bank (io[0xA])
                // is the DIP byte, like the other Lost Judgment modules.
                if (CurrentModuleBuild() == build::LJ_MR)
                {
                    uint8_t* const base = static_cast<uint8_t*>(dll);
                    InstallSystemSwitchesAt(dll, base + 0x5C386, base + 0x5C3A3,
                        reinterpret_cast<uint8_t* const*>(base + 0x75A0A8),
                        /*multiplexedSystemPort=*/false);
                }
                else
                {
                    InstallSystemSwitches(dll, symbolMap);
                }
                // ...and one bad byte in the backup RAM the module injects, which hangs the
                // board as soon as that menu's GAME ASSIGNMENTS page is drawn.
                FixBackupRamTimeIndex(dll);
            }

            PatchSl(sl::sm_context);
            // Most of what PatchGs writes lives below the build-variant gap, so it runs against
            // the layout the loaded module actually uses (LJ/Y:LAD or Gaiden) - see pxd/LJ/gs.h.
            gs::with_tail([&window](auto* ctx) { PatchGs(ctx, window); });

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

            // Motor Raid's linked-cabinet link (MrLink.h). Latches only for the MR module build
            // its RVAs were reversed from; every other game leaves it inert.
            if (gGeneral.GetGameId() == YAMPGeneral::GameId::MR
                || gGeneral.GetGameId() == YAMPGeneral::GameId::MR_GAIDEN)
            {
                MrLink::Configure(reinterpret_cast<uint8_t*>(gameDll.get()));
            }

            // Initialize Criware stub and module stubs
            Cri criware;
            // Gaiden's module was built against a newer CRIWARE whose icri has one more virtual,
            // so every slot from 9 up is shifted; without this the module's alloc() call lands on
            // free(). See Cri.h.
            if (IsGaidenBuild())
            {
                criware.UseGaidenVtable();
            }

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
            int64_t sixtyHzTicks;
            {
                LARGE_INTEGER frequency;
                QueryPerformanceFrequency(&frequency);
                sixtyHzTicks = (frequency.QuadPart * 50) / 3;
                // We want to enforce 60 FPS, unless the cap is disabled in Debug
                frameTimeTicks = settings->m_enableFpsCap ? sixtyHzTicks : 0;
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
                    //
                    // GAME SPEED IS A COMPETITIVE ADVANTAGE for a linked pair, and the single-
                    // frame handshake states in Motor Raid's ring protocol are only safe between
                    // boards running at the same rate (the Virtual On stage-desync lesson). So a
                    // live cabinet link forces the 60 Hz cap even when the Debug setting turned
                    // it off; solo play keeps the configured policy.
                    const int64_t waitTicks =
                        (frameTimeTicks == 0 && MrLink::LinkActive()) ? sixtyHzTicks : frameTimeTicks;
                    LARGE_INTEGER currentTime;
                    do
                    {
                        QueryPerformanceCounter(&currentTime);
                    } while (((currentTime.QuadPart - lastTime.QuadPart) * 1000) < waitTicks);
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
            // (1) Read the session state ONCE, at the top, before Drive() can advance it - so pad
            // routing, the coin protocol and the input suppression below all see the same answer
            // for the whole frame. See NetSession.h for the four-call-point contract.
            NetSession& net_ = NetplaySession();
            const NetSession::Status netStatus = net_.GetStatus();
            const bool netplayMatch = netStatus.inMatch;
            const int32_t netplayLocalPad = netStatus.localPad;
            const bool netplayLocked = netStatus.locked;

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
            execute_info.p_device_context = gs::device_context();
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
                // A linked cabinet may not pause either: stopping this board mid-ring trips the
                // peer's link watchdog (the ROM's own error accounting) within seconds.
                const bool pauseLocked = netplayLocked || MrLink::LinkActive();
                if (pauseLocked)
                {
                    s_pauseMenuOpen = false;   // close it if a session started while it was open
                }

                static bool s_escWasDown = false;
                const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
                if (escDown && !s_escWasDown && !pauseLocked)
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

            // Motor Raid analogue-mapping harness (see MrLink.h). Inert without -mr-iotest.
            MrLink::IoTest(execute_info);

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
            gs::with_tail([](auto* ctx) { ResetCbvSrvRingCursors(ctx); });

            // ---- NETPLAY SESSION DRIVER ----------------------------------------------------
            // (2) Poll the plugin and run the round-start state machine, and (3) decide whether
            // the emulator may advance. Both used to be ~250 lines inline here; they now live in
            // m2ftg::NetSession so the YLAD/K2 hosts can drive the identical sequence instead of
            // growing a second copy of it. See NetSession.h.
            //
            // Drive() runs HERE rather than at the top of the frame on purpose: Status() above is
            // deliberately last frame's answer, so the frame on which a barrier releases still
            // routes its pads and runs its coin protocol against a stable view.
            net_.Drive();

            // Motor Raid's linked-cabinet comm board (MrLink.h). After Drive() so the room state
            // it reads (which decides the cabinet role) is this frame's, and before module_main
            // so the peer's packet and the firmware status are resident when the ROM reads the
            // comm window. Inert for every other game and for Motor Raid outside a room.
            MrLink::PreFrame();

            // Step() also overwrites BOTH pads in execute_info with the transmitted inputs, which
            // is why it runs after the local pad fill above: whatever csl_pad produced is a local
            // prediction that the network's copy must replace, or the two machines simulate
            // different inputs and desync.
            // The on-screen status overlay that used to live here is drawn by the UI layer
            // (YAMPUserInterface::DrawNetplayOverlay), next to the lobby whose state it reports.
            const bool advanceFrame = net_.Step(execute_info);

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

                // (4) The frame really ran, so submit the desync canary and advance the netplay
                // frame index. Only reachable here - a stalled frame must not advance it.
                net_.EndFrame();

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
                ? gs::handle_tex().get(execute_info.output_texid)
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
            auto& swapChain = gs::sbgl_device().m_swap_chain;
            HRESULT hr = swapChain.m_pDXGISwapChain->Present(1, 0);
            if (FAILED(hr)) return false;

            gs::sm_context->frame_counter++;
            return true;
        }
    }

