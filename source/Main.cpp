// Main.cpp
#include "YAMPGeneral.h"
#include "GameRegistry.h"
#include "RenderWindow.h"
#include "GameLauncher.h"
#include "vf5fs/Y6/VF5FS.h"
#include "vf5fs/LJ/VF5FS.h"
#include "vf5fs/YLAD/VF5FS.h"
#include "m2ftg/YLAD/VF2.h"
#include "m2ftg/K2/K2Host.h"
#include "m2ftg/LJ/LJHost.h"
#include "pre3/Gaiden/Pre3Host.h"
#include "imgui/imgui.h"
#include "net/NetPlugin.h"

#ifdef _DEBUG
#include <crtdbg.h>
#include <cstdlib>
#endif

// NOTE: tried the D3D12 Agility SDK opt-in (D3D12SDKVersion/D3D12SDKPath exports + bundled
// .\D3D12\D3D12Core.dll v1.619.1) to fix the failing PSO — the Agility runtime loaded correctly but
// CreatePipelineState STILL returned E_INVALIDARG. So the runtime version is NOT the cause: the
// subobject stream is genuinely malformed (type tags are 0). Reverted; the DLL's stream builder is
// the thing to fix.

#ifdef _DEBUG
// The pxd engine's trap/log path formats with exotic and often mismatched varargs (e.g. a
// "%ls" whose wide-string argument is misaligned). In a Debug build the UCRT's stdio internals
// raise _CrtDbgReport assertions (corecrt_internal_stdio_output.h) on that path. With a debugger
// attached each report returns "break" -> an inline int3, and the engine hits it in a tight
// logging loop, halting forever; without a debugger it would pop assertion dialogs instead.
// Route CRT reports to the debugger output (no window, no break) and force _CrtDbgReport to
// return 0 ("continue") via a report hook, so these purely-cosmetic log asserts never stall us.
static void SuppressDebugCrtAsserts()
{
    _set_invalid_parameter_handler(
        [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {});
    for (const int reportType : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
    {
        _CrtSetReportMode(reportType, _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(reportType, _CRTDBG_FILE_STDERR);
    }
    // Belt-and-suspenders: even under _CRTDBG_MODE_WNDW, force "continue, never break".
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL,
        [](int, char*, int* retVal) -> int { if (retVal) *retVal = 0; return TRUE; });
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,
        [](int, wchar_t*, int* retVal) -> int { if (retVal) *retVal = 0; return TRUE; });
}
#endif

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nShowCmd)
{
#ifdef _DEBUG
    SuppressDebugCrtAsserts();
#endif

    // TODO: This is a hack, currently shutdown crashes because of mismatched allocators
    // Once this is handled, remove this
    auto shutdownHack = wil::scope_exit([] { ::TerminateProcess(::GetCurrentProcess(), 0); });
    auto coinit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // Game-select arguments: "-stf" runs Sonic the Fighters (LJ), "-fv" runs Fighting Vipers
    // and "-mr" runs Motor Raid (both LJ, same m2ftg path as StF), "-vf5fs" runs VF5FS (Y6),
    // "-vf5fs-lj" runs Lost Judgment's VF5FS (a different pxd generation — DX12 — hence its own
    // host), "-vf2" runs VF2 (YLAD). With no game argument, show the launcher menu instead — it
    // discovers which games are present (next to YAMP.exe and in the Steam installs of the
    // parent games) and boots the chosen one as a child process with these arguments and the
    // game folder as its CWD.
    const wchar_t* cmdLine = GetCommandLineW();
    // Longest-match over the registry's boot switches - the rule the hand-ordered wcsstr chain
    // that used to live here enforced with five comments about prefix traps ("-vf2-k2" before
    // "-vf2", "-fv2" before "-fv", "-mr-gaiden" before "-mr", the "-vf5fs-*" pair before
    // "-vf5fs"). Launcher when no game switch is present.
    const YAMPGeneral::GameId bootId = GameRegistry::ParseBootArg(cmdLine);
    const bool runVF2_K2 = bootId == YAMPGeneral::GameId::VF2_K2;
    const bool runVON_K2 = bootId == YAMPGeneral::GameId::VON_K2;
    const bool runVF2 = bootId == YAMPGeneral::GameId::VF2;
    const bool runFV2 = bootId == YAMPGeneral::GameId::FV2;
    const bool runSRC2 = bootId == YAMPGeneral::GameId::SRC2;
    const bool runFV = bootId == YAMPGeneral::GameId::FV;
    const bool runMR_GAIDEN = bootId == YAMPGeneral::GameId::MR_GAIDEN;
    const bool runMR = bootId == YAMPGeneral::GameId::MR;
    const bool runVF5FS_LJ = bootId == YAMPGeneral::GameId::VF5FS_LJ;
    const bool runVF5FS_YLAD = bootId == YAMPGeneral::GameId::VF5FS_YLAD;
    const bool runVF5FS = bootId == YAMPGeneral::GameId::VF5FS;
    const bool runStF = bootId == YAMPGeneral::GameId::StF;
    const bool runStF_GAIDEN = bootId == YAMPGeneral::GameId::StF_GAIDEN;
    // The five games the Lost Judgment m2ftg host runs, INCLUDING the two Like a Dragon Gaiden
    // rebuilds: which build loaded is a timestamp question inside the host, not a different path.
    // This has to be the whole set, because the branch below falls through to the VF5FS host for
    // anything it does not claim - which is where Fighting Vipers and Motor Raid ended up when
    // this was written as "not StF" against a bootId that, unlike the wcsstr chain it replaced,
    // means StF alone.
    const bool runLJm2ftg = runStF || runStF_GAIDEN || runFV || runMR || runMR_GAIDEN;

    // "-frames N": run N frames, then leave the loop normally and shut the module down. For
    // automated smoke tests, so a run ends through the real teardown path instead of being killed
    // (see YAMPGeneral::GetFrameLimit). Absent or malformed = unlimited, the normal case.
    if (const wchar_t* framesArg = wcsstr(cmdLine, L"-frames "))
    {
        gGeneral.SetFrameLimit(static_cast<uint32_t>(_wtoi(framesArg + 8)));
    }

    if (bootId == YAMPGeneral::GameId::Launcher) {
        Launcher::Run(hInstance, nShowCmd);
        ImGui::DestroyContext();
        return 0;
    }

    if (runVF5FS_LJ) {
        // --- Lost Judgment VF5FS path (DX12, on the shared source/pxd platform layer)
        gGeneral.SetGameId(YAMPGeneral::GameId::VF5FS_LJ);
        HMODULE dll = vf5fs::LJ::LoadDLL();
        if (!dll) {
            // LoadDLL already told the user what is missing; nothing to run without it.
            ImGui::DestroyContext();
            return 0;
        }

        vf5fs::LJ::PreInitialize();
        RenderWindow window(hInstance, dll, nShowCmd);
        vf5fs::LJ::Run(window);
        ImGui::DestroyContext();
        return 0;
    }

    if (runVF2_K2 || runVON_K2) {
        // --- Yakuza Kiwami 2 m2ftg path (GOG; a third pxd generation, sl 0xF3C0 / gs 0x202140).
        // Both of Kiwami 2's modules run here: they are the same engine build and differ only in
        // GameDesc (DLL name + config.kind), which m2ftg::K2::CurrentGame() picks off the GameId.
        gGeneral.SetGameId(runVON_K2 ? YAMPGeneral::GameId::VON_K2 : YAMPGeneral::GameId::VF2_K2);
        HMODULE dll = m2ftg::K2::LoadDLL();
        if (!dll) {
            ImGui::DestroyContext();
            return 0;
        }

        m2ftg::K2::PreInitialize();

        // Optional netplay plugin, same as the LJ and YLAD paths: absent yampnet.dll = netplay
        // simply does not exist. Without this the session driver K2::GameLoop now calls is
        // permanently inert, because net::IsAvailable() stays false - which is exactly how
        // Virtual On presented as "the plugin will not load" while Sonic the Fighters loaded it
        // fine. Nothing was wrong with the plugin; this branch just never asked for it.
        net::ParseCommandLine();
        net::Load();

        RenderWindow window(hInstance, dll, nShowCmd);
        m2ftg::K2::Run(window);
        net::Unload();
        ImGui::DestroyContext();
        return 0;
    }

    if (runVF5FS_YLAD) {
        // --- Yakuza: Like a Dragon VF5FS path (DX11; VF2's engine generation, LJ's protocol)
        gGeneral.SetGameId(YAMPGeneral::GameId::VF5FS_YLAD);
        HMODULE dll = vf5fs::YLAD::LoadDLL();
        if (!dll) {
            // LoadDLL already told the user what is missing; nothing to run without it.
            ImGui::DestroyContext();
            return 0;
        }

        vf5fs::YLAD::PreInitialize();
        RenderWindow window(hInstance, dll, nShowCmd);
        vf5fs::YLAD::Run(window);
        ImGui::DestroyContext();
        return 0;
    }

    if (runFV2 || runSRC2) {
        // --- Model 3 path (Like a Dragon Gaiden's pre3 module, DX12). One DLL hosts both
        // games; the GameId is what picks which of them module_start boots.
        gGeneral.SetGameId(runSRC2 ? YAMPGeneral::GameId::SRC2 : YAMPGeneral::GameId::FV2);
        HMODULE dll = pre3::LoadDLL();

        gGeneral.SetDLLName(gGeneral.GetArcadeGameName());
        gGeneral.SetDLLTimestamp(0);
        gGeneral.SetDataPath();
        gGeneral.LoadSettings();

        if (!dll) {
            // LoadDLL already told the user what is missing; nothing to run without it.
            ImGui::DestroyContext();
            return 0;
        }

        // Optional netplay plugin, same as every other hosted path: absent yampnet.dll = netplay
        // simply does not exist. Easy to forget and impossible to diagnose from the game - without
        // it net::IsAvailable() stays false, so the session driver pre3::GameLoop calls is
        // permanently inert and the Netplay page never appears, which reads as "the plugin will
        // not load" when nothing ever asked it to. That is precisely how Virtual On presented; see
        // the same note on the Kiwami 2 branch above.
        net::ParseCommandLine();
        net::Load();

        RenderWindow window(hInstance, dll, nShowCmd);
        pre3::Run(window);
        net::Unload();
        ImGui::DestroyContext();
        return 0;
    }

    if (runVF2) {
        // --- VF2 path (YLAD m2ftg module, DX11)
        gGeneral.SetGameId(YAMPGeneral::GameId::VF2);
        HMODULE dll = m2ftg::VF2::LoadDLL();
        if (!dll) {
            ImGui::DestroyContext();
            return 0;
        }

        m2ftg::VF2::PreInitialize();

        // Optional netplay plugin, same as the LJ path: absent yampnet.dll = netplay simply does
        // not exist. Without this the session driver VF2::GameLoop now calls is permanently
        // inert, because net::IsAvailable() stays false.
        net::ParseCommandLine();
        net::Load();

        RenderWindow window(hInstance, dll, nShowCmd);
        m2ftg::VF2::Run(window);
        net::Unload();
        ImGui::DestroyContext();
        return 0;
    }
    else if (!runLJm2ftg) {
        // --- VF5FS path (unchanged; DX11on12)
        gGeneral.SetGameId(YAMPGeneral::GameId::VF5FS);
        HMODULE dll = vf5fs::Y6::LoadDLL();
        if (!dll) {
            // LoadDLL already told the user what is missing; nothing to run without it.
            ImGui::DestroyContext();
            return 0;
        }

        vf5fs::Y6::PreInitialize();
        RenderWindow window(hInstance, dll, nShowCmd);
        vf5fs::Y6::Run(window);
        ImGui::DestroyContext();
        return 0;
    }
    else {
        // --- LJ m2ftg path: StF by default, FV with "-fv", Motor Raid with "-mr", and the two
        // Like a Dragon Gaiden rebuilds with "-stf-gaiden" / "-mr-gaiden". Every one of them
        // shares the whole hosting path; the per-game differences live in m2ftg::GameDesc
        // (m2ftg/LJ/LJHost.h) and the per-build ones in m2ftg::IsGaidenBuild().
        gGeneral.SetGameId(bootId);
        HMODULE stfDll = m2ftg::LoadDLL();  // stf- / fv- / mr-pxd-w64-d3d12_retail.dll

        // Always seed settings so UI has something to read
        gGeneral.SetDLLName(gGeneral.GetArcadeGameName());
        gGeneral.SetDLLTimestamp(0);
        gGeneral.SetDataPath();
        gGeneral.LoadSettings();

        if (!stfDll) {
            // LoadDLL already told the user what is missing; nothing to run without it.
            ImGui::DestroyContext();
            return 0;
        }

        // Optional netplay plugin. Absent yampnet.dll = netplay simply does not exist; nothing
        // else in YAMP depends on it, which is what lets a release ship without any netcode.
        net::ParseCommandLine();
        net::Load();

        // DLL is present: wire the window to the module and run the real game
        RenderWindow window(hInstance, stfDll, nShowCmd);
        m2ftg::Run(window);
        net::Unload();
        ImGui::DestroyContext();
        return 0;
    }
}
