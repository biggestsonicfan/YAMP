#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace Launcher
{
	// Runs the game-select menu (the no-command-line-argument path): discovers which arcade
	// modules are present (next to YAMP.exe and inside Steam installs of the parent games)
	// and lets the user pick one. Returns true when a game was chosen and booted in a child
	// process; false when the user just closed the window.
	bool Run(HINSTANCE instance, int cmdShow);
}
