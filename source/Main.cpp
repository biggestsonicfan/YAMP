// Main.cpp
#include "YAMPGeneral.h"
#include "RenderWindow.h"
#include "Y6/VF5FS.h"
#include "LJ/StF.h"
#include "imgui/imgui.h"

// If you still keep the DX12 example fallback around:
namespace DX12 { namespace Example { void Run(RenderWindow& window); } }

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nShowCmd)
{
    // TODO: This is a hack, currently shutdown crashes because of mismatched allocators
    // Once this is handled, remove this
    auto shutdownHack = wil::scope_exit([] { ::TerminateProcess(::GetCurrentProcess(), 0); });
    auto coinit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // Toggle which game to run (wire to args/config as you like)
    const bool runStF = /* TODO: detect user choice (args, config, etc.) */ true;

    if (!runStF) {
        // --- VF5FS path (unchanged; DX11on12)
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
