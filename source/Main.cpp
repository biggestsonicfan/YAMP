// Main.cpp
#include "YAMPGeneral.h"
#include "RenderWindow.h"
#include "GameLauncher.h"
#include "vf5fs/VF5FS.h"
#include "m2ftg/YLAD/VF2.h"
#include "m2ftg/LJ/LJHost.h"
#include "imgui/imgui.h"

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
    // "-vf2" runs VF2 (YLAD). With no game argument, show the launcher menu instead — it
    // discovers which games are present (next to YAMP.exe and in the Steam installs of the
    // parent games) and boots the chosen one as a child process with these arguments and the
    // game folder as its CWD.
    const wchar_t* cmdLine = GetCommandLineW();
    const bool runVF2 = wcsstr(cmdLine, L"-vf2") != nullptr;
    const bool runFV = !runVF2 && wcsstr(cmdLine, L"-fv") != nullptr;
    const bool runMR = !runVF2 && !runFV && wcsstr(cmdLine, L"-mr") != nullptr;
    const bool runVF5FS = !runVF2 && !runFV && !runMR && wcsstr(cmdLine, L"-vf5fs") != nullptr;
    const bool runStF = !runVF2 && !runVF5FS;

    const bool anyGameArg = runVF2 || runFV || runMR || runVF5FS || wcsstr(cmdLine, L"-stf") != nullptr;
    if (!anyGameArg) {
        Launcher::Run(hInstance, nShowCmd);
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
        RenderWindow window(hInstance, dll, nShowCmd);
        m2ftg::VF2::Run(window);
        ImGui::DestroyContext();
        return 0;
    }
    else if (!runStF) {
        // --- VF5FS path (unchanged; DX11on12)
        gGeneral.SetGameId(YAMPGeneral::GameId::VF5FS);
        HMODULE dll = Y6::VF5FS::LoadDLL();
        if (!dll) {
            // LoadDLL already told the user what is missing; nothing to run without it.
            ImGui::DestroyContext();
            return 0;
        }

        Y6::VF5FS::PreInitialize();
        RenderWindow window(hInstance, dll, nShowCmd);
        Y6::VF5FS::Run(window);
        ImGui::DestroyContext();
        return 0;
    }
    else {
        // --- LJ m2ftg path: StF by default, FV with "-fv", Motor Raid with "-mr". All three
        // games share the whole hosting path; the per-game differences live in
        // m2ftg::GameDesc (m2ftg/LJ/LJHost.h).
        gGeneral.SetGameId(runFV ? YAMPGeneral::GameId::FV
            : runMR ? YAMPGeneral::GameId::MR
            : YAMPGeneral::GameId::StF);
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

        // DLL is present: wire the window to the module and run the real game
        RenderWindow window(hInstance, stfDll, nShowCmd);
        m2ftg::Run(window);
        ImGui::DestroyContext();
        return 0;
    }
}
