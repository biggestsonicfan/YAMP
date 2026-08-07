#pragma once

// The Like a Dragon Gaiden pre3 host: entry points plus the per-game facts for the Model 3
// games this path can run.
//
// One DLL, six games — the opposite of m2ftg, where each arcade game ships as its own module.
// So there is no per-game DLL name here and no per-game symbol table; the only thing that
// differs between the six is the config byte module_start uses to pick a ROM factory and an
// image archive. Which of them are actually playable depends on what the parent title shipped:
// Like a Dragon Gaiden includes image/fv2.par and image/src2.par and nothing else.
//
// The pxd platform layer underneath is source/pxd/LJ in its GAIDEN configuration (sl 0xF140,
// gs 0x3898C0, DC_OFFSETS_GAIDEN) — pre3 is the same pxd generation as Gaiden's Sonic the
// Fighters module, so nothing about that layer is new here. See ../pre3.h.

#include <cstddef>
#include <cstdint>

#include "../../RenderWindow.h"
#include "../../YAMPGeneral.h"
#include "../pre3.h"

using module_func_t = int(*)(size_t args, const void* argp);

namespace pre3
{
	struct GameDesc
	{
		const char* display_name;
		Game game;                  // pre3_config_t.game — selects the ROM factory and the archive
		const char* image_archive;   // "image/<stem>.par", the file module_start opens
	};

	// Only the two Gaiden actually ships data for. The module supports vf3tb / vf3a / getbass /
	// spkfe as well (see Game in ../pre3.h) but no title YAMP knows about provides their
	// archives, so offering them would only produce a load failure with no way to fix it.
	inline constexpr GameDesc GAME_FV2 = { "Fighting Vipers 2", Game::Fv2, "image/fv2.par" };
	inline constexpr GameDesc GAME_SRC2 = { "Sega Racing Classic 2", Game::Src2, "image/src2.par" };

	inline const GameDesc& CurrentGame()
	{
		switch (gGeneral.GetGameId())
		{
		case YAMPGeneral::GameId::SRC2: return GAME_SRC2;
		default: return GAME_FV2;
		}
	}

	// The module DLL. Shared by every pre3 game, unlike the m2ftg family.
	inline constexpr const wchar_t* DLL_NAME = L"pre3-pxd-w64-d3d12_retail.dll";
	inline constexpr const wchar_t* DLL_SUBDIR = L"pre3";

	// ---- Host entry points (DLL-based path, DirectX 12 game) ------------------------------
	// The module's per-frame entry points. Unlike m2ftg, which has ONE, pre3 splits the frame into
	// an update stage and two render stages, and hands the host a separate callback slot for each
	// (params+0x30, +0x1060, +0x1068). They map onto M2FTGAppModule vtable slots 0x10, 0x18, and
	// 0x20+0x28. All three take the same (sizeof(execute_info), execute_info) pair.
	//
	// Calling only the first is not a degraded frame, it is an INVISIBLE one: the emulated board
	// still runs, but slot 0x18 is what uploads the 0x120000 scroll buffer and slot 0x20 is what
	// walks the texture tables, so nothing is ever recorded and nothing is ever drawn.
	struct ModuleEntries
	{
		module_func_t update = nullptr;        // params+0x30  -> vtable 0x10
		module_func_t render_begin = nullptr;  // params+0x1060 -> vtable 0x18
		module_func_t render_end = nullptr;    // params+0x1068 -> vtable 0x20 then 0x28
	};

	HMODULE LoadDLL();
	void PreInitialize();
	void Run(RenderWindow& window);
	bool GameLoop(const ModuleEntries& entries, RenderWindow& window);
}
