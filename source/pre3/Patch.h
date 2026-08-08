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

	// THE BOARD'S BOOT SCREENS, which the module never draws.
	//
	// `FUN_18003AC20` is the scene submit - the Real3D geometry AND the tilemap layer, both - and
	// it runs only when the ROM object's vtable slot 42 (+0x150) returns non-zero:
	//
	//     FUN_180037950:  return *(int *)(this + 0x205B8) != 0;
	//
	// That field is not the module's. It is mirrored out of GUEST RAM once per frame
	// (DLL 0x180037C0D: `rom[0x205B8] = mem->read32(0x00737978)`), and guest 0x00737978 is
	// Daytona 2's own MASTER FRAME COUNTER - incremented in the game's main loop
	// (`FUN_0009A39C`, entered from the boot chain at guest 0x18AE8) and in the pre-main frame
	// step `FUN_0001F3E8` (called from the boot loop at guest 0x18A94). The counter's low byte is
	// also written to the hardware register at 0xFE10001C every frame, which is presumably why
	// the module picked it as its "the game is submitting frames" signal.
	//
	// The consequence is exact: the ROM does not start counting frames until AFTER its self-test
	// phase, so the counter is zero throughout the ROM/RAM check, the WARNING screen and the
	// network check at guest 0x18A1C - and the module draws none of them. The text is composed
	// correctly, uploaded correctly, and then not rendered, because the module is told the game
	// has not started yet. A real cabinet shows all of it: the Model 3's video hardware
	// composites the tilemap continuously and does not consult this variable.
	//
	// So this redirects M3ERomSrc2's slot 42 to a host predicate that is always true, which is
	// the smallest override that restores the boot display. Deliberately NOT done by writing the
	// guest counter - some thirty places in the ROM read it, several of them timing, and moving
	// a game variable to fix a presentation gap is how a subtle desync gets introduced.
	//
	// SRC2 only, by patching that ONE vtable rather than the shared function, so Fighting Vipers
	// 2 is untouched. Self-checking: it refuses unless the slot currently holds FUN_180037950.
	// Disable with YAMP_PRE3_BOOTRENDER=0.
	void InstallBootRender();

	// How many frames the override actually forced, written to the log at teardown beside the
	// tilemap counters. Zero forced frames would mean the gate was never closed and this changes
	// nothing - the same acceptance test the narrow accessors got, for the same reason.
	void LogBootRender();

	// THE OTHER HALF OF THE SAME PICTURE: guest writes go INTO the window (counted above), and
	// this is what comes OUT of it. The two together say whether the 2D layer is lost on the way
	// in, on the way to the GPU, or after that.
	//
	// The module's tilemap unit is `CScr` (ctor DLL 0x180014760, vftable 0x18010BE18), and the
	// object pointer lives at DLL 0x180189680. Its pipeline is a producer/consumer ring:
	//
	//   +0x88 -> live register struct  (at this+0x08; +0x00 is the 0x120000 VRAM+palette shadow,
	//                                   +0x10..+0x38 the video registers the 0xF1180000 window
	//                                   writes, +0x38 the layer control word)
	//   +0x90 -> latched register struct (at this+0x48, same layout - what the frame renders from)
	//   +0x98    DIRTY. Set ONLY by render_begin, and FUN_180014DC0 - the whole tilemap render -
	//            returns immediately when it is clear.
	//   +0x9c    ring slot count, min(gs->backbuffer_count, 3)
	//   +0xa0    PRODUCER index      +0xa4  CONSUMER index
	//   +0x100 / +0x104   the two 496x384 tilemap textures; FUN_18003AC20 blits +0x100
	//   +0x110   the layer-enable mask FUN_180014DC0 computes, OR'd into the scene state
	//
	// and the gate that matters is in render_begin (DLL 0x1800432B0):
	//
	//     if (boardState == 0x10 && scr && cdevice && scr[0xa4] != scr[0xa0]) {
	//         CopyBufferRegion(scr[0xf8], ring[scr[0xa4]], 0x120000);
	//         scr[0xa4] = scr[0xa0];
	//         scr[0x98] = 1;                 // <-- the ONLY writer of the dirty flag
	//     }
	//
	// So a producer index that never moves means no upload, no dirty flag, no tilemap render, and
	// a blit of a texture nothing ever drew - which is exactly what "the text is composed and
	// never appears" looks like from outside. This reports which of those is happening.
	//
	// Logs on change plus a slow heartbeat, and is a no-op until the object exists.
	//
	// SAMPLED TWICE PER FRAME, because one sample cannot tell the two failures apart. Call
	// SampleTilemapPipelinePreFrame() before the module's update stage and LogTilemapPipeline()
	// after the render stages: dirty pre=0 post=1 means render_begin set the flag and the scene
	// submit never consumed it (so FUN_180014DC0 did not run), while pre=1 post=1 would mean the
	// flag is stuck for some other reason entirely.
	void SampleTilemapPipelinePreFrame();
	void LogTilemapPipeline();

	// How many narrow accesses the board actually made, written to the log at teardown.
	//
	// This is the ACCEPTANCE TEST, not decoration. That the ROM uses narrow stores for text was an
	// INFERENCE from "a halfword is the natural width for a tile entry", and this patch exists to
	// act on it - so it counts, and a zero count would mean the inference was wrong and this
	// changes nothing.
	void LogTilemapAccess();
}
