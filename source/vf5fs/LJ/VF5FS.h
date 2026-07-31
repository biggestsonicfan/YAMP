#pragma once

// Host for Lost Judgment's Virtua Fighter 5: Final Showdown module
// (runtime/media/vf5fs/vf5fs-pxd-w64-d3d12_retail.dll).
//
// Same entry points as every other YAMP host. Unlike the Y6 VF5FS host (../Y6, DX11) this one is
// built on the shared Lost-Judgment-era pxd platform layer in source/pxd — the same sl (0xF000),
// gs (0x388A00) and DX12 host cdevice the m2ftg hosts use, which this DLL requires byte-for-byte.

#include "../../RenderWindow.h"
#include "../vf5fs.h"

namespace vf5fs
{
	namespace LJ
	{
		HMODULE LoadDLL();
		void PreInitialize();
		void Run(RenderWindow& window);
		bool GameLoop(module_func_t func, RenderWindow& window);
	}
}
