#pragma once

// Symbols YAMP resolves out of a Yakuza Kiwami 2 m2ftg module
// (m2ftg/vf2-pxd-w64-gog_retail.dll, and later omg-pxd-w64-gog_retail.dll).
//
// This is a THIRD pxd generation, older than both the Lost Judgment and the Like a Dragon ones:
// sl context 0xF3C0 and gs context 0x202140, versus 0xF000/0x388A00 (LJ) and 0xF000/0x3820C0
// (YLAD). Only 4 of the 10 YLAD VF2 patterns survive here, so this family gets its own table.
//
// The notable difference from every other generation: **module_main is not exported and is not
// returned through params**. The real host keeps it in its own wrapper object (verified live:
// `call qword ptr [rdi+0x2A40]` from YakuzaKiwami2.exe+0x84B85C) and module_start never writes
// params+0x30. YAMP does not need to replicate that handoff — it pattern-scans module_main like
// any other symbol, which is what MODULE_MAIN below is for.

#include "../../pxd/Imports.h"

namespace m2ftg
{
	namespace K2
	{
		enum class ImportSymbol
		{
			// The per-frame entry, found by its `CMP RCX, 0x16E0` execute_info size gate.
			MODULE_MAIN,

			// The DLL's own embedded sl context (0xF3C0), built by its CRT static init. The host
			// must hand this back in params.sl_module — sl's initialize_module dereferences the
			// block unconditionally, so a null there is an instant crash.
			SL_CONTEXT_INSTANCE,

			// sl::handle_create_internal(&out, ptr, type). This ONE import unblocks the whole file
			// subsystem: PatchSl's objects reach it via file_handle_internal_t::_afterConstruct ->
			// sl::handle_create, and via semaphore_create/thread_create (which are YAMP's own Win32
			// code but still funnel through handle_create).
			SL_HANDLE_CREATE,

			// sl::file_handle_destroy — returns a file handle object to the pool at sl+0x1FC8
			// (tail index +0x1FDC, wrap at +0x1FD0, count +0x1FD4). YAMP's async request worker
			// calls it on every completed close, so a null pointer here is a call through zero on
			// the first one — which is what faulted during teardown.
			SL_FILE_HANDLE_DESTROY,

			// The archive lock pair. This generation predates the recursive rwspinlock the LJ/YLAD
			// hosts import: its archive registry is guarded by a plain **read** lock (waits while
			// the high 16 bits hold a writer, then bumps the low-16 reader count) and released by
			// an atomic decrement. Those are exactly what YAMP's csl_archive::create_instance
			// needs — a lookup plus an add_ref — so they go into sl::archive_lock_wlock/wunlock.
			// Leaving them null is a call through a null pointer on the first archive read, from
			// inside YAMP's own async worker thread.
			ARCHIVE_LOCK_RLOCK,
			ARCHIVE_LOCK_RUNLOCK,

			// The two-board gate: ONE BYTE that decides whether the module emulates the second
			// Model 2 board. Not a routine — nothing needs calling. The frame step already runs
			// the inter-cabinet link transfer and the board-bank switch; they are simply fenced
			// off behind `CMP byte ptr [gate], 0 / JZ done`.
			//
			// Virtual On is a LINKED-CABINET game: two boards, one player and one screen each,
			// exchanging 0x700 bytes per frame through the comm board. The module implements all
			// of it (link transfer at omg 0x18006A310, bank switch at 0x180069D30, plus ~15 other
			// branches gated on this same byte), and reads the gate 15 times — but NOTHING in the
			// DLL ever writes it non-zero. That is what "the Kiwami 2 module has no two-player
			// mode" means at the byte level: not missing code, just a switch never thrown.
			//
			// OPTIONAL, and present in exactly the two LINKED-CABINET titles: `omg` (Virtual On)
			// and `mr` (Motor Raid). StF, FV and both VF2 builds have the frame step but no
			// board-1 tail at all, which is the correlation that says this is the right byte.
			// Absent symbol => the gate is never written and the module behaves as it always has.
			//
			// See docs/von-netplay-recon.md for the whole mechanism, reversed from the PS3 build.
			TWO_BOARD_GATE,

			// The `XOR ECX,ECX` two bytes before the frame step's LAST call to the board-bank
			// switch - i.e. the instruction that decides which board is selected when the step
			// returns.
			//
			// It matters because RENDERING IS NOT PER-BOARD. module_main runs the frame step
			// (vtable slot 0x10), which steps board 0 and then board 1, and only afterwards calls
			// the render slots 0x18/0x20/0x28 - ONCE, against whichever board is still selected.
			// The step ends by re-selecting board 0, so board 0 is always what gets drawn and
			// `output_texid` never changes, which is exactly what the measurements showed.
			//
			// Patching these two bytes to `MOV CL,1` (same length) leaves board 1 selected instead,
			// so the render draws the other cabinet. That is how a guest presents its own screen.
			// HYPOTHESIS, not yet confirmed on screen.
			RENDER_BOARD_SELECT,

			// The board-bank switch, `void select(int board)`. It re-points work RAM, the comm
			// block, backup/IO and the video region at that board.
			//
			// Needed because the DESYNC CANARY reads emulated RAM through DwGame::rvaRamBasePtr,
			// which is one of the globals this function re-points - so the canary hashes whichever
			// board happens to be selected, not a fixed one. Measured: board 0 and board 1 differ
			// in the very chunk the canary covers (link_ID alone is 1 on the master and 2 on the
			// slave). Two peers that sample with different boards selected therefore compare a
			// master against a slave and desync on frame 1 without any real divergence - and
			// -von-render1 makes that certain, because it deliberately leaves board 1 selected.
			//
			// So the host pins board 0 around the canary. Optional, like the gate: absent in the
			// non-linked-cabinet modules, and then nothing pins anything.
			BOARD_SELECT,

			// The module's own per-board init, `void init(int board)` - what boot phase 2 calls for
			// board 0 and then board 1 (0x18006A168).
			//
			// Needed because the debug-menu RESET (rvaResetHandler) re-inits board 0 ONLY. Board 1
			// is left with a stale i960 context, and the moment two-board mode comes back on its
			// CPU resumes from a garbage IP. Caught in the act: a second-chance AV in the i960
			// instruction FETCH (`mov r8,[rdi]; movzx eax,[r8+3]`, the opcode-class decode) with
			// rdi = 0x7C2380 - which is 0x7C21D0 + 1*0x1B0, i.e. BOARD 1's context - and an i960 IP
			// of 0x500440, inside work RAM. Board 1 was trying to execute its own data.
			//
			// Calling this after a reset is the native fix; the alternative was leaving the second
			// cabinet out of every round.
			PER_BOARD_INIT,

			// The DLL's own embedded gs context (0x202140) and the global that points at it.
			// Both come from gs's self-init path, the same shape as the YLAD generation — this
			// is one of the few patterns that transfers unchanged, and it matches BOTH Kiwami 2
			// modules (vf2 and omg).
			GS_CONTEXT_INSTANCE,
			GS_CONTEXT_PTR,
		};

		using Imports = pxd::ImportsT<ImportSymbol>;

		Imports BuildSymbolMap(void* dll);

		// Comma-separated names of any required symbol that failed to resolve ("" when all present).
		const char* RequiredButMissing(const Imports& symbols);
	}
}
