#pragma once

// Host-side presentation helpers shared by every m2ftg host (LJ StF/FV in LJ/LJHost.cpp and
// YLAD VF2 in YLAD/VF2.cpp): the 4:3/16:9/fill aspect setting and the Escape pause-menu shell.

#include <cstdint>

#include "../RenderWindow.h"

namespace m2ftg
{
	// The Model 2 arcade games are native 4:3 (496x384 internal, upscaled by the module to a
	// 1024x768 display texture). The presentation is a user setting: 0 = keep 4:3 (pillarboxed
	// in widescreen windows), 1 = stretch to 16:9, 2 = fill the whole window. Called at startup
	// and re-applied live from the game loops whenever the setting changes.
	void ApplyAspectSetting(RenderWindow& window, uint32_t mode);

	// The Escape pause menu, mirroring Lost Judgment's: the host draws the menu shell while the
	// module's own pause path (execute_info status bit0) freezes the emulation. Returns false
	// when the player picks Quit. (RE notes on LJ's menu shell live in LJ/LJHost.cpp.)
	bool DrawPauseMenu(RenderWindow& window, bool& menuOpen);
}
