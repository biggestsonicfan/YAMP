// Main.cpp
#include "YAMPGeneral.h"
#include "RenderWindow.h"
#include "Y6/VF5FS.h"
#include "Y6/VF2.h"
#include "LJ/StF.h"
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

// If you still keep the DX12 example fallback around:
namespace DX12 { namespace Example { void Run(RenderWindow& window); } }

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

    // Toggle which game to run: StF by default, "-vf5fs" runs VF5FS (Y6), "-vf2" runs VF2 (YLAD).
    const bool runVF2 = wcsstr(GetCommandLineW(), L"-vf2") != nullptr;
    const bool runStF = !runVF2 && wcsstr(GetCommandLineW(), L"-vf5fs") == nullptr;

    if (runVF2) {
        // --- VF2 path (YLAD m2ftg module, DX11)
        gGeneral.SetGameId(YAMPGeneral::GameId::VF2);
        HMODULE dll = Y6::VF2::LoadDLL();
        if (!dll) {
            ImGui::DestroyContext();
            return 0;
        }

        Y6::VF2::PreInitialize();
        RenderWindow window(hInstance, dll, nShowCmd);
        Y6::VF2::Run(window);
        ImGui::DestroyContext();
        return 0;
    }
    else if (!runStF) {
        // --- VF5FS path (unchanged; DX11on12)
        gGeneral.SetGameId(YAMPGeneral::GameId::VF5FS);
        HMODULE dll = Y6::VF5FS::LoadDLL();
        if (!dll) {
            // Keep your existing DX12 fallback if VF5FS DLL missing
            gGeneral.SetDLLName("DX12 Fallback");
            gGeneral.SetDLLTimestamp(0);
            gGeneral.SetDataPath(u8"Sega", u8"Virtua Fighter 5 Final Showdown");
            gGeneral.LoadSettings();

            RenderWindow window(hInstance, hInstance, nShowCmd);
            DX12::Example::Run(window);
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
        // --- StF path (DX12 game DLL if present; otherwise DX12 fallback)
        gGeneral.SetGameId(YAMPGeneral::GameId::StF);
        HMODULE stfDll = LJ::StF::LoadDLL();  // tries to load stf-pxd-w64-d3d12_retail.dll

        // Always seed settings so UI has something to read
        gGeneral.SetDLLName("Sonic the Fighters");
        gGeneral.SetDLLTimestamp(0);
        gGeneral.SetDataPath(u8"Sega", u8"Sonic The Fighters");
        gGeneral.LoadSettings();

        if (!stfDll) {
            // Fallback: pure DX12 spinning-cube + ImGui on 11on12
            RenderWindow window(hInstance, hInstance, nShowCmd);
            LJ::StF::DX12::Run(window);
            ImGui::DestroyContext();
            return 0;
        }

        // DLL is present: wire the window to the module and run the real game
        RenderWindow window(hInstance, stfDll, nShowCmd);
        LJ::StF::Run(window);
        ImGui::DestroyContext();
        return 0;
    }
}
