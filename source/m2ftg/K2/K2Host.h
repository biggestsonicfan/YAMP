#pragma once

// Host for Yakuza Kiwami 2's m2ftg arcade modules (GOG build).
//
// Kiwami 2 ships two, built one second apart on 2019-09-30 and therefore the same engine build:
//   m2ftg/vf2-pxd-w64-gog_retail.dll   Virtua Fighter 2      config.kind 0
//   m2ftg/omg-pxd-w64-gog_retail.dll   "omg" (unidentified)  config.kind 3
// so they share this host exactly as StF/FV/MR share LJHost. Per-game facts live in GameDesc
// below; only VF2 is wired up so far.
//
// This is a THIRD pxd generation (sl 0xF3C0 / gs 0x202140) and it differs from LJ and YLAD in
// ways that matter to a host — see K2Host.cpp for the live-captured details:
//   * module_main is neither exported nor returned through params; YAMP pattern-scans it.
//   * module_start is handed ONLY params+0x20 (ICriWare) and params+0x38 (the 0x100C m2ftg
//     config). No sl/gs/ct module blocks — the module owns its own contexts.
//   * execute_info is 0x16E0, and the master volume is the +0x1C FLOAT (the m2ftg/Y6 mechanism),
//     NOT Lost Judgment's +0x663 byte.

#include <cstddef>
#include <cstdint>

#include "../../RenderWindow.h"
#include "../../YAMPGeneral.h"

namespace m2ftg
{
	namespace K2
	{
		// Scoped to this namespace on purpose: LJHost.h declares a global module_func_t and
		// Main.cpp includes both headers.
		using module_func_t = int(*)(size_t args, const void* argp);

		struct GameDesc
		{
			const wchar_t* dll_name;
			const wchar_t* subdir;      // fallback folder next to YAMP.exe
			const char* display_name;
			uint32_t kind;              // indexes the module's own ROM-name table
		};

		// The ROM-name table module_start indexes with config.kind is at 0x18015DF9C.. in the VF2
		// DLL and reads { 0:"vf2", 1:"vf", 2:"stf", 3:"omg", 4:"mr" } — a DIFFERENT order from the
		// Lost Judgment modules' table (where 1=fv, 2=stf, 4=mr). Combined with "%s/rom/%s..." it
		// picks m2ftg/rom/<name>_rom.par, which is exactly what Kiwami 2 ships.
		inline constexpr GameDesc GAME_VF2 = {
			L"vf2-pxd-w64-gog_retail.dll", L"m2ftg", "Virtua Fighter 2", 0,
		};
		inline constexpr GameDesc GAME_OMG = {
			L"omg-pxd-w64-gog_retail.dll", L"m2ftg", "Yakuza Kiwami 2 arcade (omg)", 3,
		};

		const GameDesc& CurrentGame();

		HMODULE LoadDLL();
		void PreInitialize();
		void Run(RenderWindow& window);
	}
}
