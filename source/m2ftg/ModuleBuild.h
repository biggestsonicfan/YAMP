#pragma once

// Which BUILD of an m2ftg module DLL is loaded.
//
// The same arcade game ships in more than one Yakuza/Judgment title, and the module DLL is
// recompiled each time. Sonic the Fighters is the case that forced this to exist: Lost Judgment
// and Like a Dragon Gaiden ship byte-identical stf_rom.par / stf_all.acb / stf_all.awb / stf.acf
// and shader archives — the GAME is the same down to the byte, and all 76 HLE hook records have
// identical ROM offsets — but the host DLL around it is a 2023-11-27 rebuild of the 2022-11-21
// engine. Everything YAMP addresses by DLL RVA therefore moves, and two pxd context layouts
// change size.
//
// So "which game" (GameDesc, HleHooks::Info, the ROM tables) and "which build" (every RVA, the
// two context sizes) are different questions, and this header answers the second. Tables that
// carry per-build data key off these timestamps so the identity constants live in exactly one
// place; the data itself stays next to whatever owns it.
//
// The PE TimeDateStamp is the key rather than the SHA-256 because GameVerify has already hashed
// the file and refused to load anything it did not recognise by the time these are consulted —
// so this only has to tell KNOWN-GOOD builds apart, which a timestamp does in four bytes.

#include <cstdint>

namespace m2ftg
{
	// PE FileHeader.TimeDateStamp of each module build YAMP hosts. Read with
	// `Get-FileHash`/a PE header dump, or copy from the GameVerify table, which records the same
	// value for the same file.
	namespace build
	{
		// Lost Judgment 1.0.0.12, modules built 2022-11-21. stf/fv/mr.
		inline constexpr uint32_t LJ_STF = 0x637B11A3;
		inline constexpr uint32_t LJ_FV  = 0x637B119D;
		inline constexpr uint32_t LJ_MR  = 0x637B1190;

		// Like a Dragon Gaiden, modules built 2023-11-27. Ships stf/vf2/mr; this is the stf one.
		inline constexpr uint32_t GAIDEN_STF = 0x65647FB5;

		// Yakuza: Like a Dragon, 2021-02-01. vf2.
		inline constexpr uint32_t YLAD_VF2 = 0x601763D1;
	}

	// TimeDateStamp of an already-loaded module, read back out of its mapped PE header. Returns 0
	// if the pointer is not a mapped PE image. Every pxd module is ASLR'd, so this takes the live
	// base rather than assuming 0x180000000.
	uint32_t ModuleTimestamp(const void* moduleBase);

	// The running module's build, latched by the host right after LoadDLL. Zero before that, and
	// for hosts that never call SetCurrentModuleBuild (K2, the VF5FS hosts), which have a single
	// known build each and no per-build tables.
	uint32_t CurrentModuleBuild();
	void SetCurrentModuleBuild(uint32_t timestamp);

	// True when the loaded module is one of the Like a Dragon Gaiden builds. The pxd gs context
	// is 0x3898C0 there rather than 0x388A00, with every field below gs.h's gap18 at +0xBA8, so
	// this is what pxd::gs::sm_is_gaiden_layout gets set from.
	bool IsGaidenBuild();
}
