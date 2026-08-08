#pragma once

#include <cstddef>

// Patches to the pre3 module itself - the same role m2ftg/LJ/Patch.h and vf5fs/Y6/Patch.h play for
// their hosts. Things that REDIRECT the module's own vtables live beside this in
// BoardVtables/Determinism/SystemSwitches/SecurityBoard; what belongs here is the module's static
// tables, which those cannot reach.

namespace pre3
{
	// THE MODEL 3 TILEMAP WINDOW'S MISSING NARROW ACCESSORS.
	//
	// pre3 maps guest 0xF1xxxxxx - Model 3 tilemap RAM plus the video registers - through
	// `CM3Mem`'s window table (DLL 0x1801077C0, the entry whose top byte is 0xF1), and implements
	// only HALF of it:
	//
	//     read8   0x1800127C0   `mov al, 0xFF; ret`        a stub
	//     write8  _guard_check_icall                        DISCARDED
	//     read16  0x1800127D0   `mov eax, 0xFFFF; ret`     a stub
	//     write16 _guard_check_icall                        DISCARDED
	//     read32  0x180013A50   real - forwards to the device at mem+0x370
	//     write32 0x180013B00   real - same, with a bswap
	//
	// The device itself is present and installed; it is `CM3Mem::init`'s seventh argument. Only
	// the narrow paths are missing, and what they cost is EVERY PIECE OF TEXT THE BOARD DRAWS. The
	// ROM composes text through a BIOS syscall service (`FUN_0000624C` is a bare PowerPC `sc`,
	// commands 0x20 locate / 0x22 print) whose natural store width for a tile entry is a halfword,
	// so `NETWORK CHECKING`, `NO CARRIER! CHECK NETWORK CABLE`, `WARNING` / `TROUBLE OCCURRED` and
	// the rest are composed and thrown away. That is why every diagnosis of the linked-cabinet work
	// had to be done by reading guest RAM: the screen was never going to say anything. See
	// docs/src2-netplay-recon.md §6.
	//
	// This builds the four missing accessors out of the two that work - read the containing word,
	// splice the lane, write it back - and patches them into the window table.
	//
	// BIG-ENDIAN LANES, because the guest is. read32/write32 both `bswap`, so the value they carry
	// is the guest's word as a host integer: the byte at guest address A sits at bit
	// `24 - 8*(A & 3)` of the word at `A & ~3`, and the halfword at bit `16 - 8*(A & 2)`. Getting
	// that backwards would put text in the wrong half of every tile rather than failing visibly.
	//
	// SCOPED TO TILEMAP RAM ONLY (below 0xF1120000). The video REGISTERS at 0xF1180000 keep the
	// module's existing behaviour, because a register is not memory: turning a narrow write into a
	// read-modify-write of its neighbours would invent side effects on a device whose semantics
	// have not been read. Discarding those is at least what the board has always done.
	//
	// Disable with YAMP_PRE3_TILEMAP=0 if it ever needs bisecting against.
	void InstallTilemapAccess();

	// How many narrow accesses the board actually made, written to the log at teardown.
	//
	// This is the ACCEPTANCE TEST, not decoration. That the ROM uses narrow stores for text was an
	// INFERENCE from "a halfword is the natural width for a tile entry", and this patch exists to
	// act on it - so it counts, and a zero count would mean the inference was wrong and this
	// changes nothing.
	void LogTilemapAccess();
}
