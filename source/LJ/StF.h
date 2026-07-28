#pragma once

#include "../RenderWindow.h"

#include "m2ftg_host.h" // GameDesc / CurrentGame() — the StF/FV-shared LJ hosting facts

using module_func_t = int(*)(size_t args, const void* argp);

namespace LJ {
    namespace StF {

        // DLL-based path (DirectX 12 game)
        HMODULE LoadDLL();
        void PreInitialize();                 // sets DataPath/loads settings (kept for parity)
        void Run(RenderWindow& window);       // run via the StF game DLL
        bool GameLoop(module_func_t func, RenderWindow& window);

        // Pure DX12 fallback (spinning cube + ImGui via D3D11on12)
        namespace DX12 {
            void Run(RenderWindow& window);
        }
    }
}
