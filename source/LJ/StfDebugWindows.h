#pragma once

namespace LJ {
	namespace StF {

		// Draws the StF DLL's own "dw" debug-menu windows (DEBUG MENU / CONFIG / PERFORMANCE /
		// 960STAT) as ImGui windows. The window layout, item labels, bound variables and action
		// handlers all come from the descriptor tree inside the game DLL itself; YAMP only
		// interprets that data. No-op unless the game is StF, the DLL is loaded and booted, and
		// the "Display debugging features" debug setting is enabled.
		void DrawDebugWindows();
	}
}
