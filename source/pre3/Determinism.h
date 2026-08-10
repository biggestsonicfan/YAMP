#pragma once

#include <cstddef>
#include <cstdint>

namespace pre3
{
	// Everything needed to make two pre3 boards simulate identically - which is the precondition
	// for lockstep netplay, and is nearly free here.
	//
	// THE BOARD IS ALREADY FRAME-DETERMINISTIC. Its per-frame CPU budget is not wall-clock
	// derived: the board computes `cpu_clock / 60` once (DLL 0x180038D60, the classic
	// 0x88888889/shr-5 divide-by-60) and splits it into three fixed fractions by compile-time
	// float constants, then runs the CPU for exactly those counts every frame plus two literal
	// 200-instruction slices. Fighting Vipers 2's 133 MHz clock therefore advances 2,216,666
	// instructions per emulated frame on every machine, whatever the host is doing. Nothing in
	// the frame path samples elapsed time. That is the opposite of the m2ftg situation, where
	// instruction-count-driven timers are what made netplay hard.
	//
	// Which leaves exactly ONE host-varying input: the real-time clock.
	//
	//   M3EInput::get_time (DLL 0x180033C40, vtable slot 17) is the ONLY caller of `_time64` in
	//   the entire module. It returns the board's own clock - a fixed epoch at rom+0x470, reset
	//   to 0x386D4380 (2000-01-01 UTC) by slot 18 and advanced by the board - when the optional
	//   interface at params+0x1070 is present, and the HOST WALL CLOCK when it is not. YAMP
	//   passes null there, so today every machine seeds from its own clock.
	//
	// Rather than fabricate that interface - it is the module's LINKED-CABINET interface, wanted
	// by Sega Rally 2 and Spikeout, and handing one to Fighting Vipers 2 would drag in behaviour
	// that game does not want (see docs/pre3-netplay.md) - this pins the clock directly, by
	// redirecting slot 17 in the board's vtables exactly as the TEST/SERVICE switches redirect
	// slot 0.
	//
	// PINNED FROM INSTALL, which is before module_start - and that is a CORRECTION to how this was
	// first written ("off by default; netplay turns it on"). The board reads its clock while it
	// BOOTS, and the power-on state a netplay round restores is captured at the end of that boot,
	// so an unpinned boot hands the two peers different snapshots - a divergence baked into the
	// state a round starts from, which is the one place lockstep can never recover from.
	//
	// Measured: with the clock unpinned, two runs of the same build disagree on the board's state
	// at the same point; with it pinned, every frame of the boot is reproducible across processes.
	//
	// The cost to a stand-alone session is a board that believes it is permanently 2000-01-01. The
	// benefit is that the whole board becomes reproducible run to run.
	//
	// THAT COST IS NOT FREE, and the original claim here - "which is exactly what the module's own
	// linked-cabinet path does to it, and which no Model 3 game here exposes" - was wrong on both
	// halves. The linked path returns a clock the board ADVANCES, not a frozen one; and Sega
	// Racing Classic 2 never finishes booting with a clock that does not tick. So the pin is
	// applied per game, by measurement - see ClockPinBootSafe in the .cpp, which is also where the
	// prerequisite for ever pinning SRC2 is written down.
	// The default epoch a pinned clock reports: the module's own reset value, so a pinned board
	// sees exactly what a linked one would - 2000-01-01 00:00:00 UTC.
	inline constexpr long long DETERMINISTIC_EPOCH = 0x386D4380;

	// Takes the board vtables already located - see InstallSystemSwitches for why.
	void InstallDeterministicClock(void* const* vtables, size_t count);

	// Pin the board's clock (true) or let it read the host's (false). Safe to call at any time;
	// takes effect at the board's next clock read, and is a no-op if the install found no
	// accessor.
	//
	// `epoch` is what a pinned clock reports. It defaults to the module's own reset value, but a
	// netplay session should pass the SHARED MATCH SEED instead (YampNet_GetMatchSeed): both
	// peers then agree on the clock, which is the whole requirement, and the value differs per
	// match so two sessions do not replay identically. This is the pre3 equivalent of the m2ftg
	// path's "re-seed the rand hook from the match seed" - pre3 has no rand hook to re-seed, and
	// the clock is the only thing downstream of it.
	void SetDeterministicClock(bool pinned, long long epoch = DETERMINISTIC_EPOCH);

	// True when the redirect is installed - i.e. SetDeterministicClock can do anything at all.
	bool DeterministicClockAvailable();

	// ---- The emulated machine, for a netplay round ------------------------------------------
	//
	// Everything below reaches the module's own emulator object - the one module_start builds and
	// stores in the global at DLL 0x180527BF8, whose vtable slot 2 (DLL 0x1800393D0) IS the frame
	// step module_main drives. All of it is addressed by RVA on the same justification as
	// HleHooks and the pxd system-shader loader: GameVerify has already pinned this exact build by
	// SHA-256, and these are DATA offsets with no code to pattern-match. Each was read out of the
	// function named beside it.

	// The machine's bring-up phase - the word at machine+0x88 that the frame step switches on.
	// 0x10 is running; -1 means the module is not loaded.
	//
	// Exposed because "not running" has more than one cause and the caller usually needs to tell
	// them apart: a linked cabinet held at the boot barrier parks at 0xF, which from outside looks
	// exactly like a board whose offsets are wrong. See CommBoard.cpp's diagnostic.
	int BoardPhase();

	// Hand the board's own symbols over, resolved from the module by pattern rather than by RVA.
	// Called once from the host's install step, BEFORE module_start.
	//
	// Passing null for either is legal and means "not resolved": the hardcoded RVA is then used and
	// the fact is logged. That is a deliberate fallback rather than a silent one - the RVA is
	// correct for the build GameVerify pins, so a failed pattern must not stop the game booting,
	// but it must also not pass unnoticed, because the next module build is exactly when the RVA
	// stops being right and the symptom is a board that reads as "never booted".
	void SetBoardSymbols(void* machineObject, const void* cxcommVtable);

	// The CXComm vtable the comm board self-checks against, resolved or fallen back. Published
	// here rather than in CommBoard because this file already owns "where the module's things are"
	// and one owner was the point of the last cleanup.
	const void* BoardCommVtable();

	// The board's MAIN RAM, or null before it is up: mem+0x60 -> bank descriptor -> the 0xC00000
	// bank, which is the chain the canary already walks. Published so a probe does not have to
	// re-derive it, and because the alternative route (HleHooks' RAM_PTR global) is the one whose
	// mis-derivation is already written up as having produced a run of confident, wrong readings.
	//
	// A guest WORD at address A is the host dword at base + A. Byte access is swizzled - the
	// module's own handlers use `A ^ 3` - so the top byte of a host dword read is the byte at the
	// LOWEST guest address. Cross-checked at runtime: guest 0x737984 reads 0x6E and the emulated
	// IRQ enable at mem+0x34 is 0x6E, independently.
	const void* BoardGuestRam();

	// The CM3Mem object (machine+0xA0), or null until the machine is up. It owns the guest's
	// address decode and, at +0x36, the Model 3 INTERRUPT STATE byte the guest polls at
	// 0xF0100018 - so this is what a host has to reach to assert an interrupt the module does not.
	void* BoardMemObject();

	// The ROM object the machine is running (machine+0x90), or null until it is up.
	//
	// THE ONE PLACE THAT KNOWS WHERE THE MACHINE IS. Everything reaching into the emulator - the
	// canary, the savestate requests, the comm board's CXComm subobject - hangs off this object,
	// and the RVA that finds it was briefly written down in two files at once. A second copy is a
	// second thing to get wrong when a module build moves, and the copies would not disagree
	// loudly: they would each read a plausible number out of a different structure.
	void* BoardRomObject();

	// True once the machine has finished booting - its phase word (machine+0x88, the switch in
	// the frame step) reaches 0x10, the running state.
	//
	// EVERY helper below is gated on this and quietly fails before it, so a netplay round MUST NOT
	// begin until it is true. A guest that joins an already-waiting host reaches the barrier within
	// a couple of hundred milliseconds of launching, long before its board is up; announcing then
	// would start a round on a machine where ResetBoard() was still a no-op.
	bool IsBoardBooted();

	// Put the board back to its shipped VS start state - the savestate netplay starts both peers
	// from, and the whole reason a pre3 round needs no boot dance.
	//
	// THE BOARD ALREADY HAS THIS MECHANISM; nothing here is invented. The module never cold-boots
	// Fighting Vipers 2: late in bring-up it reads image/fv2/vs_start.bin into a vector of
	// preloaded machine states (machine+0x108, count at machine+0x114) and, when the config's VS
	// byte is set, asks for state 0 to be restored. The ask is one byte - a request bitfield at
	// machine+0x120 - which the frame step drains at the top of every frame (DLL 0x180037E50):
	//
	//     bit0 save    bit1 restore-newest-save   bit2 pop   bit3 discard all
	//     bit4 RESTORE PRELOADED STATE (bits5-6 select the slot)   bit7 restore happened
	//
	// and bit4's handler (DLL 0x1800382F0) rewrites the CPU registers, both 0xC00000 RAM banks,
	// 0x800000 of VRAM and the 0x120000 scroll buffer from the file's bytes. So this sets bit4 for
	// slot 0 and the module does the rest, exactly as its own boot path does.
	//
	// ASYNCHRONOUS BY ONE FRAME, and the caller must respect that: the restore happens inside the
	// NEXT update stage, before that frame simulates. BoardResetPending() reports the wait, and the
	// frame that clears it must be run with NEUTRAL inputs on both peers or the two machines
	// simulate one different frame from the identical state they just restored.
	//
	// Returns false, having asked for nothing, if the machine is not up or carries no preloaded
	// state (which is every game but Fighting Vipers 2 and Virtua Fighter 3 today).
	bool ResetBoard();

	// ---- The OTHER reset: the board's power-on state ----------------------------------------
	//
	// vs_start.bin drops a round straight into a match, which is the wrong place to watch the AI -
	// and the AI is the most sensitive thing two peers can be seen to disagree about. So YAMP also
	// takes a snapshot of its own, through the same request bitfield: bit 0 is SAVE, which pushes
	// the whole machine onto a second vector (machine+0xF8, count at machine+0x104), and bit 1
	// restores the newest entry without popping it - so one snapshot can start any number of
	// rounds.
	//
	// WHEN IT IS TAKEN is what makes it usable, and it is not a tuning choice. Requested on the
	// first host frame that sees the machine RUNNING, which is before the frame step has executed
	// a single guest frame: the board reaches phase 0x10 at the end of one update stage and only
	// begins stepping on the next. The snapshot is therefore the pristine post-boot state, which
	// two peers reach identically by construction - same ROM images, same config, no input, no
	// emulated time elapsed. Nothing about it depends on when the player got around to hosting.
	//
	// Costs one ~22 MB allocation for the life of the process, which is the module's own save
	// buffer at the size it computes for itself.
	bool SaveResetSnapshot();

	// Restore the snapshot SaveResetSnapshot took. Same one-frame, asynchronous contract as
	// ResetBoard - see BoardRestorePending - and false, having asked for nothing, if no snapshot
	// has been taken.
	bool RestoreResetSnapshot();

	// True once a snapshot exists to restore.
	bool ResetSnapshotTaken();

	// True while EITHER kind of restore request is still outstanding. Both go through the same
	// bitfield and the same one-per-frame drain, so a caller waiting for "the board is back" wants
	// this rather than either bit on its own.
	bool BoardRestorePending();

	// The value this peer submits to the desync canary for the current emulated frame.
	//
	// TWO HALVES, and each covers what the other misses:
	//
	//   * the PowerPC core's register file, which changes on EVERY frame (the frame ends after a
	//     fixed instruction count, so the registers are a sharp function of everything the frame
	//     did) but says nothing about the rest of the board;
	//   * the main RAM bank, sampled one dword per 256 bytes across the whole 0xC00000, which says
	//     everything about the board but moves sluggishly - measured, it repeats for five frames
	//     at a stretch on a static screen.
	//
	// The RAM half is sampled rather than dense because dense is 12 MB of memory traffic per
	// emulated frame for an answer the register half already gives: a divergence that never
	// touches a sampled dword for more than a frame or two is not one that stays small. The pair
	// costs ~50 us.
	//
	// 0 when there is no board to read, which the caller must treat as "no canary" rather than as
	// a value - two peers both reading 0 would agree forever.
	uint32_t StateCheckValue();

	// The two halves separately, for the per-frame netplay trace.
	//
	// Worth having as its own entry point rather than as a debug print inside the combined value:
	// when two peers disagree, WHICH half moved first is most of the diagnosis. A CPU half that
	// diverges while the RAM half still agrees is the emulated program taking a different path -
	// a real desync. A RAM half that moves while the CPU half agrees is something writing into
	// guest memory from OUTSIDE the simulation, which on this module means the render stages or
	// the display thread, and is a fault in the canary's coverage rather than in the round.
	//
	// False, leaving both zero, when the board cannot be read.
	bool StateCheckParts(uint32_t& cpu, uint32_t& ram);

	// FNV-combine two parts already read (what StateCheckValue does internally), so a caller
	// that just paid for StateCheckParts is not forced to run the 12 MB sweep a second time.
	uint32_t StateCheckCombine(uint32_t cpu, uint32_t ram);

	// ---- Reading board state safely: THE EMULATOR IS ON ITS OWN THREAD ----------------------
	//
	// This is the single most important fact about hosting pre3, and it was not in the original
	// recon. The board does NOT run inside the update stage. The frame step - `FUN_18003B0A0`, the
	// function that runs the CPU for its fixed instruction budget - is the body of a **worker
	// thread**, `FUN_18003B270`, created under the name "m3e_ctrl" at DLL 0x1800393D0 case 0xF.
	// Its loop is: run one frame step, clear the busy flag, sleep against a WALL CLOCK to hit the
	// frame period at `machine+0x150`, stamp `machine+0x148`, signal, wait to be released again.
	//
	// The update stage is the other half of that handshake: it waits for the worker at the top,
	// does its own work (the savestate pump, the screen capture) while the worker is parked, and
	// releases it at the end. So one update stage IS one emulated frame - the frame accounting is
	// sound - but the WINDOW in which the board is safe to read is the update stage's own body,
	// which the host is not inside.
	//
	// The consequence, and it cost a two-machine round: reading the canary immediately after the
	// update stage RETURNS reads the board while the worker is running the next frame. That is a
	// plain data race, and two peers that race differently disagree about state their simulations
	// agree on completely. It presented as a desync at frame 4 with identical settings, an
	// identical seed and a verifiably identical restore.
	//
	// The module publishes exactly what is needed to do this properly, and its own savestate
	// handler leans on the same two flags (DLL 0x1800382F0 refuses to restore while either is
	// set), so this is the module's own contract rather than an invention:
	//
	//   machine+0x16C  the "m3e_ctrl" worker is inside the frame step
	//   machine+0x16D  the "m3e_disp" worker is inside the memory object
	//   machine+0x148  a timestamp the worker stamps ONCE per completed frame - a sequence marker
	//
	// Read the marker before releasing the worker (i.e. before the update stage), then wait for it
	// to change afterwards. Waiting on the busy flags alone is NOT enough: finding them clear
	// cannot distinguish "the frame finished" from "the frame has not started", and picking wrong
	// makes the sampled frame host-timing dependent again - which is the bug, not the fix.
	unsigned long long BoardFrameMarker();

	// Block until the marker moves off `previous` and both workers are out of the machine, so the
	// state is the completed emulated frame's. False on timeout, in which case nothing should be
	// sampled - a racy canary is worse than none, because it ends rounds that were never wrong.
	bool WaitForEmulatedFrame(unsigned long long previous, unsigned int timeoutMs = 250);

	// Whether this game can currently sustain a netplay session.
	//
	// Fighting Vipers 2 only. Sega Racing Classic 2 is the other game this module can run and
	// is excluded on two counts that are not guesses: it is the LINKED-CABINET game, so it wants
	// the interface at params+0x1070 that YAMP deliberately passes as null (see
	// docs/pre3-netplay.md), and it reads the analogue ADC ring for its wheel and pedals, which
	// nothing in the transmitted pad carries. It has also never been run at all.
	//
	// Also requires the clock pin, since an unpinned board seeds from the host wall clock and the
	// two peers would then disagree about the time - the single host-varying input this whole
	// header exists to remove.
	bool NetplaySupported();

	// One line, once, the first time the machine reports itself running: the emulator object, its
	// phase word, how many preloaded states it holds, and whether the canary can be computed.
	//
	// Netplay is the one feature here whose groundwork cannot be checked by playing the game - a
	// wrong offset in any of the above produces a round that starts and then quietly diverges on
	// two machines, which is the most expensive way there is to find out. This makes every one of
	// them observable from a single headless `-fv2 -frames N` run instead. Cheap enough to leave
	// in: it is one line for the life of the process.
	void LogBoardStateOnce();
}
