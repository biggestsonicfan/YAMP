#pragma once

#include <cstdint>

namespace m2ftg
{
	// Fixes the module's 8-bit access path to System-24 character RAM (guest 0x1080000..0x10FFFFF).
	//
	// The DLL's 16-bit char-RAM write handler nibble-reverses each halfword on the way into the
	// host buffer (0xABCD -> 0xDCBA at DLL+0x508B8) - the hardware byte-lane + nibble convention
	// baked into the write path, matched by a linear left-pixel-low-nibble decoder
	// (FUN_180036060). The 8-bit handler stores bytes PLAIN, with no transform. Sonic the
	// Fighters only ever writes char RAM with 16-bit stores, so the 8-bit path was never
	// exercised and never right; a homebrew ROM using byte stores (the SDK's m2_loadfont /
	// tfb_putpixel) gets every 4-pixel group rendered mirrored (x -> 3-x). Verified end to end:
	// both guests' char data decodes clean under the hardware convention, StF's w16 data is
	// transformed on ingest, pengo's w8 data is not.
	//
	// Per byte, the w16 transform is: internal[i] = nibble_swap(guest[i ^ 1]). So the corrected
	// 8-bit write forwards to the DLL's own handler at offset^1 with swapped nibbles, and the
	// corrected 8-bit read undoes the same - keeping the DLL's own bounds checks, buffer mapping
	// and dirty flags in play rather than reimplementing them.
	//
	// StF-safe by construction: only the 8-bit slots change, and StF never uses them here.
	namespace CharRamFix
	{
		// Install after module_start (the memory-map table must exist). Idempotent.
		void Install();
		bool IsInstalled();
	}
}
