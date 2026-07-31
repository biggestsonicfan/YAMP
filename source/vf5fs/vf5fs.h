#pragma once

// Shared Virtua Fighter 5: Final Showdown module-hosting protocol, used by both VF5FS hosts:
//   Y6/  — vf5fs-pxd-w64-Retail Steam.dll from Yakuza 6: The Song of Life   (DX11)
//   LJ/  — vf5fs-pxd-w64-d3d12_retail.dll from Lost Judgment                (DX12)
//
// The two DLLs are different pxd generations — different gs/sl context layouts, different renderer —
// but they take the SAME module_start protocol, so the parts that do not change live here and each
// host keeps only its own generation's structures.
//
// LJ facts below were read out of the DLL itself (Ghidra, 2026-07-29): module_start at RVA 0x1EDEF0,
// module_main at 0x1EDCC0, module_stop at 0x1EE3D0. Y6 facts come from the working Y6 host.
// NOTE both DLLs are built DYNAMIC_BASE (ASLR) — resolve everything against the runtime module base,
// never against 0x180000000.

#include <cstdint>
#include <cstddef>

namespace vf5fs
{
	using module_func_t = int(*)(size_t args, const void* argp);

	// The game config the host hands the module at module_params_t+0x38.
	//
	// Y6 reads 8 bytes here. LJ's module_start loads 16 (one VMOVUPS of params+0x38 into its config
	// global at 0x18054D4A0) but only decodes the same first 8: byte 5 is the game mode, and it
	// derives two flag globals from it (mode != 0 and mode == 2); byte 7's bit0/bit1 are the
	// triangle-start / Dural-unlocked flags. So this is the shared core.
	struct game_config_t
	{
		uint16_t energy;
		int8_t round;
		int8_t time;
		int8_t diff;
		int8_t game_mode;                 // LJ: 0/1/2, drives two derived globals in module_start
		int8_t lang;
		bool is_triangle_start : 4;
		bool is_dural_unlocked : 4;
	};
	static_assert(sizeof(game_config_t) == 8);
	static_assert(offsetof(game_config_t, game_mode) == 5);

	// LJ's 16-byte config. The upper 8 bytes are copied into the module's config global with the
	// rest but have no decoder in module_start — whatever reads them does so later, through that
	// global (0x18054D4A0), so they are unknown-but-preserved rather than unused.
	struct lj_game_config_t
	{
		game_config_t core;
		std::byte unknown[8]{};
	};
	static_assert(sizeof(lj_game_config_t) == 16);

	// module_start's parameter block. Identical in both generations except for the config type, so
	// it is a template over that. Field meanings verified against LJ's module_start disassembly:
	//   +0x00 size of this struct        +0x08 sl module    +0x10 gs module    +0x18 ct module
	//   +0x20 ICriWare implementation    +0x28 root path (UTF-8, copied out, max 0x103 bytes)
	//   +0x30 out: module_main pointer   +0x38 config
	// Each of the three *_module blocks is {size_t size; context*} and is size-checked by the
	// module's own initialize (LJ: sl 0x10/context 0xF000, gs 0x58/context 0x388A00, ct 0x10/
	// context 0x30 — the same values the m2ftg LJ modules require).
	template<typename ConfigT, typename SlModuleT, typename GsModuleT, typename CtModuleT, typename CriT>
	struct module_params_t
	{
		size_t size = sizeof(module_params_t);
		const SlModuleT* sl_module;
		const GsModuleT* gs_module;
		const CtModuleT* ct_module;
		const CriT* cri_ptr;
		const char* root_path;
		module_func_t* module_main;
		ConfigT config{};
	};

	// The execute_info header every pxd module generation shares (the pxd "base_execute_info_t"):
	//   +0x00 size (the module rejects a mismatch: Y6 0x320, LJ 0x690, m2ftg 0x1760)
	//   +0x08 cgs_device_context*   +0x10 status   +0x14 result   +0x18 output_texid
	//   +0x1C sound_volume (0.0f mutes ALL audio — this was the VF5FS mute bug)
	// status bit0 = pause (host -> module): the module skips its update, which is what the host
	// pause menu drives. Everything past the header is generation-specific; see each host.
	constexpr size_t EXECUTE_INFO_STATUS_PAUSE = 1;
}
