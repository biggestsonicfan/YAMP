#include "LJHost.h"

// Defined in HostCdevice.cpp: close + ExecuteCommandLists StF's recorded render lists (host's job).
void ExecuteStfRenderListsNow();
// Defined in HostCdevice.cpp: mark the DLL's per-frame render window so the ResourceBarrier hook
// only corrects StF's own barrier StateBefore values (not d3d11on12's blit barriers).
void SetStfRenderActiveNow(bool active);
// Defined in HostCdevice.cpp: PATH B — close+execute+flush+reopen StF's own draw list each frame.
void SubmitStfFrameListNow();
// Defined in HostCdevice.cpp: true once a submit hung/removed the device (stop the loop, DRED dumped).
bool StfExecDisabledNow();
// Defined in HostCdevice.cpp: the host's per-frame upload-pool frame-advance — bump the upload-frame
// stamp + recycle StF's upload buffers (in-use -> available). Fixes the pool-exhaustion crash.
void AdvanceFrameStampNow();

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "../../wil/resource.h"

#include "../../criware/Cri.h"

#include "../sl.h"
#include "gs.h"
#include "../m2ftg.h"
#include "../Imports.h"
#include "Patch.h"
#include "sys_util.h"
#include "cs_game.h"

#include "../ImportSymbols.h"
#include "HostCdevice.h"
#include "HleHooks.h"
#include "CharRamFix.h"
#include "../ElfRom.h"
#include "../M2Input.h"
#include "../HostUI.h"
#include "../../YAMPGeneral.h"
#include "../../imgui/imgui.h"

#include "../../DebugLog.h"
#include "../../Utils/MemoryMgr.h"
#include "../../Utils/ScopedUnprotect.hpp"

namespace m2ftg
{
        // Per-game facts (DLL name, config.kind, ROM names, i960 RVAs) live in LJHost.h
        // (GameDesc / CurrentGame()) — the LJ-specific but m2ftg-generic hosting header.

        // Resolved from the DLL (ImportSymbol::STF_*): M2FTGAppModule's per-frame render-system submit
        // (FUN_18003b530) and the live-execute_info global (DAT_1801ee4a0) that submit dereferences.
        // module_main only RECORDS StF's frame; the host must drive this submit each frame (see GameLoop).
        static void  (*g_stfFrameSubmit)()   = nullptr;
        static void** g_stfRenderExecInfo    = nullptr;

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
            Import(g_stfFrameSubmit, ImportSymbol::STF_FRAME_SUBMIT);
            Import(g_stfRenderExecInfo, ImportSymbol::STF_RENDER_EXECINFO);
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

            gameDll.reset(LoadLibraryW((gamePath / game.dll_name).c_str()));
            if (gameDll == nullptr)
            {
                // Try loading from a subdirectory ("stf" / "fv")
                gamePath.append(game.subdir);
                gameDll.reset(LoadLibraryW((gamePath / game.dll_name).c_str()));
            }

            if (!gameDll)
            {
                const std::wstring str(L"Could not load " + std::wstring(game.dll_name) + L"!\n\nMake sure that YAMP.exe is located next to the DLL file or its \"" + game.subdir + L"\" subdirectory contains it.");
                MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
            }
            else
            {
                gGeneral.SetDLLName(WcharToUTF8(game.dll_name));

                // Get the checksum
                PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(gameDll.get());
                PIMAGE_NT_HEADERS ntHeader = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<char*>(dosHeader) + dosHeader->e_lfanew);
                const DWORD timeStamp = ntHeader->FileHeader.TimeDateStamp;
                gGeneral.SetDLLTimestamp(timeStamp);

                // Reject known old DLLs
                if (timeStamp == 0x603E22E3 || timeStamp == 0x606D6969 || timeStamp == 0x6075A65A)
                {
                    const std::wstring str(std::wstring(game.dll_name) + L" is of an unsupported version!\n\nPlease update your game to the latest version.");
                    MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
                    gameDll.reset();
                }
            }

            return gameDll.get();
        }

        void m2ftg::PreInitialize()
        {
            gGeneral.SetDataPath();
            gGeneral.LoadSettings();
        }

        static void CheckForExecutable()
        {
            // TODO: Make more graceful instead of killing the app
            const std::wstring executablePath = (gamePath.parent_path() / L"Yakuza6.exe").native();

            // We'll consider the file valid if it has MZ and PE magic values
            wil::unique_hfile file(CreateFileW(executablePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            FAIL_FAST_IMMEDIATE_IF(!file);

            wil::unique_handle fileMapping(CreateFileMapping(file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
            FAIL_FAST_IMMEDIATE_IF_NULL(fileMapping);

            wil::unique_mapview_ptr<IMAGE_DOS_HEADER> dosHeaderView(static_cast<PIMAGE_DOS_HEADER>(MapViewOfFile(fileMapping.get(), FILE_MAP_READ, 0, 0, 0)));
            FAIL_FAST_IMMEDIATE_IF_NULL(dosHeaderView);

            FAIL_FAST_IMMEDIATE_IF(dosHeaderView->e_magic != 'ZM');

            PIMAGE_NT_HEADERS64 peHeaderView = reinterpret_cast<PIMAGE_NT_HEADERS64>(reinterpret_cast<char*>(dosHeaderView.get()) + dosHeaderView->e_lfanew);

            FAIL_FAST_IMMEDIATE_IF(peHeaderView->Signature != 'EP');
        }

        void m2ftg::Run(RenderWindow& window)
        {
            const auto module_start = reinterpret_cast<module_func_t>(GetProcAddress(gameDll.get(), "module_start"));
            THROW_LAST_ERROR_IF_NULL(module_start);
            const auto module_stop = reinterpret_cast<module_func_t>(GetProcAddress(gameDll.get(), "module_stop"));
            THROW_LAST_ERROR_IF_NULL(module_stop);
            module_func_t module_main;

            //CheckForExecutable();

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
            params.config.difficulty = settings->m_stfDifficulty <= 3 ? static_cast<uint8_t>(settings->m_stfDifficulty) : 1;
            params.config.country = settings->m_stfCountry <= 2 ? static_cast<uint8_t>(settings->m_stfCountry) : 0;
            params.config.is_freeplay = settings->m_stfFreeplay ? 1 : 0;
            params.config.is_vs_mode = settings->m_stfVersusMode ? 1 : 0;

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

            ApplyAspectSetting(window, settings->m_stfAspect);

            // MUST precede module_start too, and precede ApplyRetarget: the loose-ROM gate is
            // consulted during the archive mount inside module_start, and retarget entries may
            // name symbols that only exist once the ELF is parsed. Path mirrors the loose-ROM
            // layout - rom/<archive stem>/game.elf, next to the ROM images it replaces.
            {
                const GameDesc& game = CurrentGame();
                std::string stem(game.rom_archive_name);
                if (const size_t dot = stem.rfind('.'); dot != std::string::npos) { stem.erase(dot); }
                const std::wstring elfPath = L"rom/" + UTF8ToWchar(stem) + L'/' + ElfRom::OVERRIDE_FILE_NAME;
                if (ElfRom::Load(elfPath, ElfRom::PROGRAM_ROM_SIZE))
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
            DebugLog("[%s::Run] module_start returned %lld\n", gGeneral.GetGameTag(), (long long)msRet);

            // Must follow module_start: board bring-up is what fills the memory-map table this
            // wraps. Off unless [Debug] LogHardwareWrites names regions.
            CharRamFix::Install();
            if (msRet == 0)
            {
                LARGE_INTEGER lastTime;
                QueryPerformanceCounter(&lastTime);
                while (!window.IsShuttingDown())
                {
                    DebugLog("[%s::Run] GameLoop iter\n", gGeneral.GetGameTag());
                    if (!GameLoop(module_main, window)) { DebugLog("[%s::Run] GameLoop returned false\n", gGeneral.GetGameTag()); break; }

                    // TODO: Waitable timer
                    LARGE_INTEGER currentTime;
                    do
                    {
                        QueryPerformanceCounter(&currentTime);
                    } while (((currentTime.QuadPart - lastTime.QuadPart) * 1000) < frameTimeTicks);
                    lastTime = currentTime;
                }

                // TODO: module_stop
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

        // SEH-guarded call of StF's per-frame render-system submit (FUN_18003b530). Kept in its own
        // function because __try/__except cannot share a scope with C++ object unwinding. One-shot: on a
        // fault it latches off so we never repeat a crash. Returns false once disabled.
        static bool CallStfFrameSubmit(void* submitFn)
        {
            static bool s_ok = true;
            if (!s_ok || !submitFn) return false;
            __try { reinterpret_cast<void(__cdecl*)()>(submitFn)(); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                s_ok = false;
                DebugLog("[%s-submit] FUN_18003b530 faulted -> disabled\n", gGeneral.GetGameTag());
            }
            return s_ok;
        }

        bool m2ftg::GameLoop(module_func_t func, RenderWindow& window)
        {
            // Persistent input/arcade state (LJ keeps these in the scene object across frames):
            // s_startScreen  <- module status bit6 from last frame (scene+0x2B58, "press start" active)
            // s_coinPending  <- a credit was inserted, START injection phase   (scene+0x2B59)
            // s_startToggle  <- alternate-frame START injector                 (scene+0x2B5A)
            static csl_pad s_pads[2];
            static bool s_startScreen = false;
            static bool s_coinPending = false;
            static bool s_startToggle = false;

            const auto* settings = gGeneral.GetSettings();

            // Live-apply the aspect-ratio setting (the ImGui Apply button only writes the settings
            // struct; this poll is what makes the change take effect without a restart).
            {
                static uint32_t s_lastAspect = UINT32_MAX;
                if (settings->m_stfAspect != s_lastAspect)
                {
                    s_lastAspect = settings->m_stfAspect;
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
            execute_info.sound_volume = 1.0f;

            // Escape toggles the pause menu; while it is open, drive the MODULE'S OWN pause path
            // (status bit0 — see DrawPauseMenu's RE notes). Escape is reserved for this, like
            // in LJ, and is no longer a game button on the StF keyboard layout (sl.cpp).
            static bool s_pauseMenuOpen = false;
            {
                static bool s_escWasDown = false;
                const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
                if (escDown && !s_escWasDown)
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
            // csl_pad (M2Input, set up on the YAMP Controls page), then copy the shared
            // 0xE0-byte prefix into the m2ftg pad blocks (same pxd sl layout, same button bits).
            M2Input::PollPads();
            s_pads[0].set_state(0);
            s_pads[1].set_state(1);
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
                    execute_info.assign[p][i] = M2Input::MODULE_ASSIGN[i];
                }
            }

            // Dedicated coin binding: a press becomes the coin status bit (host->module bit5,
            // same bit LJ's start/coin protocol below injects). Meaningless in freeplay, and
            // swallowed while the pause menu is open like the rest of the inputs.
            {
                static bool s_coinWasDown[2] = {};
                for (int p = 0; p < 2; p++)
                {
                    const bool down = M2Input::ActionDown(p, M2Input::Action_Coin);
                    if (down && !s_coinWasDown[p] && !settings->m_stfFreeplay && !s_pauseMenuOpen)
                    {
                        execute_info.status |= 0x20;
                    }
                    s_coinWasDown[p] = down;
                }
            }

            // Arcade coin/start protocol (LJ FUN_142494450): while the module shows the start
            // screen (status bit6 out), a START press becomes a COIN INSERT (status bit5 in, the
            // press itself is swallowed); afterwards START is injected on alternating frames until
            // the module leaves the start screen. ONLY meaningful with the freeplay dip switch
            // OFF — with is_freeplay=1 the module takes START directly and there is no coin to
            // insert; the dance would just swallow the player's first press. Follows the same
            // setting Run passed to module_start as config.is_freeplay (changing it needs a
            // restart, so the two always agree within a session).
            const bool isFreeplay = settings->m_stfFreeplay;
            if (!isFreeplay && !s_pauseMenuOpen)
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

            // TEMP: the D3D11-on-12 overlay/blit path trips the D3D12 debug layer at frame 0
            // (a null-SRV/shared-resource issue in d3d11), crashing before the StF DX12 PSO builds
            // at ~frame 18 and masking the CreatePipelineState error we're diagnosing. func() (the
            // StF render that builds the PSO) is independent of it, so skip the D3D11 path while
            // debugging the PSO. Flip back to false to restore the visible overlay/output.
            constexpr bool kDebugSkipD3D11Overlay = false;

            window.BeginFrame();
            if (!kDebugSkipD3D11Overlay) window.NewImGuiFrame();
            if (!kDebugSkipD3D11Overlay && s_pauseMenuOpen)
            {
                if (!DrawPauseMenu(window, s_pauseMenuOpen))
                {
                    return false; // Quit picked
                }
            }

            // The CBV/SRV descriptor-copy rings are per-frame transient; reset their cursors before the
            // DLL's render (func) records this frame, or they grow past the heap -> CopyDescriptors AV.
            ResetCbvSrvRingCursors(gs::sm_context);

            // Bracket the DLL's render so the ResourceBarrier hook corrects StF's barrier StateBefore
            // values (ping-pong RTs assume last-frame state; YAMP creates in COMMON -> id=527 desync).
            SetStfRenderActiveNow(true);
            const int funcResult = func(sizeof(execute_info), &execute_info);
            SetStfRenderActiveNow(false);

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

            // THE REAL SUBMIT (found by RE): module_main only RECORDS. In "handler mode" (which StF's
            // vtbl[0] init installs: DAT_18058aad0 = FUN_18003b530), module_main's submit stage vtbl[5] is
            // a no-op — StF's actual per-frame GPU submit is g_stfFrameSubmit (FUN_18003b530), invoked ONLY
            // by the engine's render-system loop (the 0x180c945xx table walker) that LJ runs but YAMP does
            // not. So the host must call it every frame after module_main.
            // g_stfFrameSubmit dereferences g_stfRenderExecInfo (DAT_1801ee4a0 = the LIVE execute_info):
            // module_main sets it on entry and CLEARS it to 0 on return, so by here it's null -> the
            // submit's FUN_180062470 did `*(execinfo + n*400 + 0x20)` = null+0x20 AV. Restore it around the
            // call (then clear, matching module_main's post-frame cleanup).
            // PATH B: StF records its whole frame into its own command list but never submits it (host's
            // job). Close + ExecuteCommandLists + flush + reopen that exact list now. Do this BEFORE the
            // handler below so the GPU has the frame before StF's frame-boundary recycle runs.
            // TEST (black-screen): the draws bind CBVs at ring offsets 512/768 whose contents are
            // never written (stale NaN / zeros) while ring+0 holds a valid 2D ortho. Hand-copy the
            // valid matrix into the dead slots before the submit: if ANY geometry appears (even
            // wrongly transformed), those slots are proven to be the live 3D-transform CBs and the
            // missing piece is their writer — not the D3D12 plumbing.
            {
                constexpr bool kTestFillDeadCbSlots = false; // tested 2026-07-25: no change (see memory)
                if (kTestFillDeadCbSlots)
                {
                    auto* ringCpu = *reinterpret_cast<uint8_t**>(
                        reinterpret_cast<uint8_t*>(gs::sm_context) + 0x188208);
                    if (ringCpu != nullptr)
                    {
                        memcpy(ringCpu + 256, ringCpu, 64);
                        memcpy(ringCpu + 512, ringCpu, 64);
                        memcpy(ringCpu + 768, ringCpu, 64);
                    }
                }
            }

            // TEST (overlay canary): the ImGui/11on12 overlay renders every frame but never
            // reaches the screen, while VF2's identical overlay path DOES show. The only thing
            // StF's loop adds is this mid-frame close/execute/flush of StF's list. Disable it to
            // see whether OUR submit is what destroys the presented frame.
            // *** ORDER MATTERS (HUD-flashing fix, 2026-07-26): record EVERYTHING, then submit ONCE. ***
            // FUN_18003b530 is NOT just profiling: its second half (FUN_18003a540) is the TASK PUMP that
            // records TaskM2E's draws (the Model 2 -> D3D12 frame translation). The old order
            // (submit THEN pump) meant every submit executed a MIX of this frame's func() recordings and
            // LAST frame's task-pump recordings — the display texture was composed from two different
            // frames' passes, flashing the HUD every frame regardless of which texture we blitted.
            // Needs the live execute_info in DAT_1801ee4a0 (module_main clears it on return) or it AVs.
            // *** GAME-SPEED FIX (2026-07-26): DISABLED. module_main's render stages ALREADY invoke
            // FUN_18003b530 internally through the installed submit-handler hook (DAT_18058aad0 —
            // see the "render stages call the handler" RE finding). Calling it here again ran the
            // TASK PUMP (FUN_18003a540 -> TaskM2E = the Model 2 emulator step) a SECOND time per
            // frame -> the whole game (timers included) ran ~2x. Left compiled for quick A/B.
            constexpr bool kCallFrameSubmitManually = false;
            if (kCallFrameSubmitManually && g_stfFrameSubmit && g_stfRenderExecInfo)
            {
                *g_stfRenderExecInfo = &execute_info;
                CallStfFrameSubmit(reinterpret_cast<void*>(g_stfFrameSubmit));
                *g_stfRenderExecInfo = nullptr;
            }

            // Now the whole frame (func() + task pump) is recorded: close/execute/flush it in ONE submit.
            // PAUSED (LJ ground truth, C:\temp\menu PIX captures 2026-07-26): while the pause menu is
            // open LJ drops the module's command list from ExecuteCommandLists entirely (10 lists -> 9,
            // zero module draws, zero ResolveSubresource, no module upload traffic; the CRT pass keeps
            // re-sampling the LAST resolved frame, which receives no barriers). With status bit0 set the
            // module records nothing anyway, so skip the close/execute/reopen dance and the upload-stamp
            // advance — submit nothing, exactly like LJ.
            constexpr bool kDisableStfSubmit = false;
            if (!kDisableStfSubmit && !s_pauseMenuOpen) SubmitStfFrameListNow();
            if (StfExecDisabledNow())
            {
                // A submit hung/removed the GPU — DRED was dumped. Stop now so StF's next-frame upload
                // allocation doesn't crash on the dead device (that cascade was masking the real fault).
                DebugLog("[%s::Run] submit hang/device-removed -> stopping loop (see [DRED])\n", gGeneral.GetGameTag());
                return false;
            }

            // The other half of the host's per-frame job (twin of SubmitStfFrameListNow): advance the
            // upload-frame stamp + recycle StF's upload buffers (in-use -> available). Runs AFTER the flush
            // above, so every recycled buffer is GPU-complete. This is the fix for the upload-pool
            // exhaustion crash (FUN_18009be60, ~frame 570). Must precede StF's next-frame func().
            if (!s_pauseMenuOpen) AdvanceFrameStampNow();

            // (Legacy) our force-submit of StF's lists — currently a no-op (kExecuteStfLists=false). The
            // handler call above is the correct engine-driven submit; keep this disabled.
            ExecuteStfRenderListsNow();

            cgs_tex* display_tex = gs::sm_context->handle_tex.get(execute_info.output_texid);
            if (display_tex == nullptr) return false;
            if (display_tex->m_type != 2) return false;

            if (!kDebugSkipD3D11Overlay)
            {
                // StF renders DX12-native; the DX11-typed fields (m_pD3DShaderResourceView / m_pD3DResource)
                // are null on this path. The output ID3D12Resource lives at sbgl_resource+0x98 (found via QI:
                // a 1024x768 B8G8R8A8 Texture2D). Wrap it through 11on12 into a D3D11 SRV and blit it.
                auto* res = display_tex->mp_sbgl_resource;
                ID3D12Resource* rt = res
                    ? *reinterpret_cast<ID3D12Resource**>(reinterpret_cast<uint8_t*>(res) + 0x98)
                    : nullptr;
                // CANARY TEST: skip the StF-texture 11on12 blit, keep ONLY ImGui. If the overlay
                // reappears, BlitDX12Texture's wrap/acquire is what kills the whole overlay path.
                constexpr bool kSkipStfBlit = false; // canary test done: skipping the blit does NOT restore ImGui
                if (!kSkipStfBlit) window.BlitDX12Texture(rt);
                else (void)rt;
                window.RenderImGui();
            }
            window.EndFrame();

            // Present using the swapchain the game already created
            auto& swapChain = gs::sm_context->sbgl_device.m_swap_chain;
            // CANARY (user insight): YAMP's own ImGui/11on12 overlay never reaches the screen in the
            // StF path but does in VF2 — a HOST-side symptom. If the gs swapchain is not the very
            // object the 11on12 backbuffers were wrapped from, we composite into one swapchain and
            // present a different one => black screen AND no overlay, regardless of what StF renders.
            {
                static bool s_logged = false;
                if (!s_logged)
                {
                    s_logged = true;
                    DebugLog("[swapchain] gs=%p window=%p SAME=%d\n",
                        static_cast<void*>(swapChain.m_pDXGISwapChain),
                        static_cast<void*>(window.GetSwapChain()),
                        swapChain.m_pDXGISwapChain == window.GetSwapChain() ? 1 : 0);
                }
            }
            HRESULT hr = swapChain.m_pDXGISwapChain->Present(1, 0);
            if (FAILED(hr)) return false;

            gs::sm_context->frame_counter++;
            return true;
        }
    }

