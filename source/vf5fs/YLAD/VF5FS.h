#pragma once

// Host for Yakuza: Like a Dragon's Virtua Fighter 5: Final Showdown module
// (runtime/media/vf5fs/vf5fs-pxd-w64-retail.dll) — the THIRD build of this game YAMP hosts.
//
// Like a Dragon is Yakuza 7, not 6, so this is its own generation and its own namespace rather
// than anything under ../Y6. What it shares with which sibling is the whole point of this host:
//
//   * PLATFORM  = YLAD's VF2 module (source/m2ftg/YLAD). Same engine build, compiled 13 minutes
//     apart: DX11, sl context 0xF000, ct 0x30, and a gs context of 0x3820C0 that the module
//     supplies itself (the DLL's own embedded template) rather than the host allocating one.
//     NOT the source/pxd Lost-Judgment layer, whose gs context is 0x388A00.
//   * PROTOCOL  = Lost Judgment's VF5FS (../LJ). execute_info is 0x680 here vs LJ's 0x690 (LJ
//     adds the two button-assignment arrays at the end), and the master volume is the BYTE at
//     +0x663, not the +0x1C float the m2ftg/Y6 modules read.
//
// See source/vf5fs/vf5fs.h for the parts of the protocol both VF5FS generations share.

#include "../../RenderWindow.h"
#include "../vf5fs.h"

namespace vf5fs
{
	namespace YLAD
	{
		HMODULE LoadDLL();
		void PreInitialize();
		void Run(RenderWindow& window);
		bool GameLoop(module_func_t func, RenderWindow& window);
	}
}
