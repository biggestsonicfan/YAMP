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

			// REMOVED 2026-08-04: PER_BOARD_INIT, once believed to be "the module's own per-board
			// init, what boot phase 2 calls for board 0 and then board 1", and used to revive board
			// 1 after a round-start reset. It was matched from boot phase 2's `XOR ECX,ECX / CALL /
			// MOV ECX,1 / CALL` pair, which is a real for-both-boards call - just not that one.
			// 0x18006BAF0 is the per-board VIDEO init: it walks the 2 MB framebuffer region at
			// 0x1807DADC0 + board*0x20A080 building a 6-bit palette table, and touches nothing the
			// i960 owns. Calling it returned cleanly and left board 1 exactly as broken, which is
			// what made the bug so hard to see - the log line said the board had been re-initialised.
			//
			// The real routine is the module's board reset 0x180069E80, and it does not need a
			// symbol here: it is already two-board aware and the debug-menu RESET ends with it. See
			// DwGame::rvaBoardResetAll, which is where it is now driven from.
			//
			// The lesson worth keeping: a pattern that matches uniquely still only proves the SHAPE
			// is unique, not that it is the shape you wanted. This one was never checked against the
			// function's body.

			// The frame driver's CALL to the I/O refresh, for BOARD 0 - the one that rebuilds the
			// emulated I/O board's ports from the host's pads, immediately before that board's CPU
			// step. Hooking it is how the cabinet TEST and SERVICE lines get pulled low, since the
			// module protocol has no host source for them and the refresh rebuilds the port every
			// frame (so a plain host write is simply overwritten).
			//
			// BOTH BOARDS, and the second one is not optional on a linked pair. Driving TEST into
			// board 0 alone deadlocks the machine: board 0 enters the operator menu and stops
			// servicing the inter-cabinet link, board 1 spins in `Net_check` waiting for it, and
			// board 0 blocks forever in `synch` waiting for board 1. Caught live with x64dbg -
			// board 0 frozen at `synch+0xC` across every sample, board 1 looping `Net_check+0x64`.
			//
			// Entering on both boards together is symmetric, so neither is waiting on the other.
			// It is also what the always-linked stance requires: YAMP runs two boards from boot
			// precisely so the topology never changes mid-session, and a switch that only reaches
			// one of them reintroduces exactly that asymmetry.
			//
			// OPTIONAL: absent leaves the switches released, exactly as on Motor Raid.
			I960_IO_REFRESH_CALL,
			I960_IO_REFRESH_CALL_B1,

			// The frame driver's CALL to the inter-cabinet LINK TRANSFER (omg 0x18006A310), the
			// `E8` between `INC dword [link_frame_counter]` and the `XOR ECX,ECX` that selects
			// board 0. YAMP wraps it to model the one piece of comm-board firmware the module
			// lacks: RESET SEMANTICS.
			//
			// The ROM's own link check (`Net_check`, i960 0xC5870) drives the comm board through
			// its two registers: it holds the board in reset (CommBoardReset = 0), releases it
			// (= 1), and then polls CommFlagReg until bit7 - data ready - reads 0, which on the
			// hardware is the firmware acknowledging its boot. The module's transfer sets bit7 on
			// every frame REGARDLESS of the reset register, so that poll can never terminate once
			// the link is running - and it is a tight i960 loop with no frame yield, so the CPU
			// step never returns and the whole host spins. Cold boot escapes only by ordering (the
			// link is not enabled until after the first check); the debug menu's exit re-runs the
			// check with the link live and hangs the machine. Diagnosed live 2026-08-04: clearing
			// bit7 by hand un-froze the board, the site screens printed, and the pair re-linked.
			//
			// OPTIONAL, and only installed where TWO_BOARD_GATE also resolved: the same engine
			// ships in the vf2 module, which has no comm board for the registers to describe.
			LINK_TRANSFER_CALL,

			// The module's QueryPerformanceCounter wrapper (omg 0x1800856B0) - a leaf that zeroes a
			// local, calls QPC through the IAT and returns it. 49 call sites: the task layer's frame
			// pacing, the frame driver, and a great many `t1 - t0` profiling pairs.
			//
			// YAMP patches it so the module's whole timebase becomes a counter the host advances,
			// because the board is paced by REAL TIME and therefore runs a host-dependent number of
			// emulated frames per module_main call - which is not something netplay can be made to
			// agree about. See VirtualClock.h for the full reasoning.
			//
			// OPTIONAL: absent means the module keeps real time and behaves exactly as before.
			WALL_CLOCK,

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
