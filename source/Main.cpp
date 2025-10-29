// Main.cpp
#include "YAMPGeneral.h"
#include "RenderWindow.h"
#include "Y6/VF5FS.h"
#include "imgui/imgui.h"

// NEW: forward-declare in our DX12::Example namespace
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

    HMODULE dll = Y6::VF5FS::LoadDLL();
    if (!dll) {
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