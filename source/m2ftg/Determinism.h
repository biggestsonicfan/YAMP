#pragma once

#include <cstddef>
#include <cstdint>

// The m2ftg board-determinism API - everything a netplay round needs to make two emulated
// boards start identical and stay comparable: boot state, reset, RNG seeding, the texture-
// budget pin, the frame-counter anchor and the desync canary. Split out of the Debug/
// DebugWindows pair (2026-08-09) to mirror pre3/Determinism.{h,cpp} - the two families now
// have the same file-level shape, though the mechanisms inside remain deliberate siblings.
// The per-game descriptor data both halves read lives in Debug/DwGame.h.

namespace m2ftg
{
		// Reads 32 bits of emulated i960 memory through the DLL's own memory-map dispatch.
		// Returns false (leaving `out` untouched) if the module is not loaded or the address
		// has no reader in the map.
		bool ReadEmulatedRam32(uint32_t address, uint32_t& out);

		// Holds Sonic the Fighters' GAME ASSIGNMENTS -> DAMAGE item at the value the YAMP dip
		// switch (or, during netplay, the ROOM) selects, by writing bit 0x80 of the ROM's
		// game_assignments_flag in emulated RAM. Call once per EMULATED frame, from the module
		// thread, before module_main - not from the UI thread, whose timing relative to the
		// emulator differs between machines. No-op unless the game is StF and the board has booted.
		void UpdateDamageAssignment();

		// True once the emulated board has finished booting (the DLL's own phase dword
		// +0x6B9300 reaches 2, which only happens after module_main has run for a while).
		//
		// EVERY determinism helper below is gated on this and quietly returns false before it,
		// so a netplay round MUST NOT begin until it is true. A guest that joins an already
		// waiting host reaches the barrier within a couple of hundred milliseconds of launching
		// - long before its board is up - and would then start a match with no reset, no shared
		// RNG seed and no texture-budget pin, while the host (which had been waiting, booted)
		// applied all three. The result is an emulated CPU rolling its own random numbers on one
		// side only: an AI desync that looks like a network fault and is not one.
		bool IsBoardBooted();

		// Re-runs the DLL's own i960 CPU/board initialisation - the DEBUG MENU's "RESET" item
		// (handler at DLL+0x4C840), which unlike STEP/GO is NOT a stub in retail.
		//
		// Netplay uses this to give both machines an identical starting state. Lockstep keeps two
		// emulators in step only if they START in the same state; merely beginning to exchange
		// inputs at the same moment does nothing if one side has already been running attract mode
		// for thirty seconds. Resetting both at the barrier makes "frame 0" mean the same thing.
		//
		// Returns false if the game is not StF or the board has not booted (dword +0x6B9300 != 2),
		// in which case nothing was called.
		bool ResetBoard();

		// Seeds the host RNG that the ROM's `rand` HLE hook draws from, so both machines in a
		// netplay match produce identical "random" values.
		//
		// The ROM's rand is HLE'd: handler DLL+0x53070 calls the DLL's own generator
		// (DLL+0x8D40) and writes the result into i960 g0. That generator is a MERSENNE TWISTER
		// (N=624, M=397, the standard tempering shifts) whose state object lives at
		// *(*(DLL+0x68BB88) + 0x20): u32 state[624] at +0x08, circular index at +0x9C8.
		// Normally it is seeded per-process, so two machines roll different numbers - which is
		// exactly what makes the CPU behave differently on each client in a netplay match.
		//
		// This re-runs the standard MT init_genrand with the shared match seed. Returns false if
		// the game is not StF, the board has not booted, or the state object fails a sanity check
		// (in which case nothing was written).
		bool SeedHostRng(uint32_t seed);

		// Makes the ROM's texture-upload budget deterministic, which netplay requires.
		//
		// Four Core HLE hooks (all sharing handler DLL+0x52FD0) answer the ROM's "have 9 ms
		// elapsed?" question by reading a WALL CLOCK and writing the boolean into i960 g0. The
		// unpack loop yields on that answer, so a fast machine and a slow one do different amounts
		// of work in the same emulated frame and their states diverge - a desync that no amount of
		// input synchronisation can fix, because the divergence is not in the inputs.
		//
		// Enabling this repoints those table entries at a wrapper that runs the original handler
		// (for its instruction-length return value) and then overwrites g0 with a constant "budget
		// not expired", so the loop always completes the same work on both machines. The cost is
		// smoothness during big uploads; the benefit is a reproducible simulation.
		//
		// Returns false if the game is not StF, the board has not booted, or no entries matched.
		bool SetTextureBudgetDeterministic(bool enable);

		// The address in EMULATED RAM of the ROM's own per-frame counter - the value a netplay
		// round anchors frame 0 to and desync-checks against every frame. 0 means this game has
		// no measured counter, which disables netplay for it rather than anchoring on a value
		// that is not a frame count.
		uint32_t RomFrameCounterAddress();

		// Whether this game can currently sustain a netplay session.
		//
		// Separate from RomFrameCounterAddress() on purpose: a measured frame counter says a
		// round can be ANCHORED, not that it will hold. VF2 spent a while as exactly that case -
		// counter measured and correct, rounds still wrong - and the split is what let it be
		// expressed without either shipping a known-broken game or discarding a measured fact.
		// VF2 has been ready since 2026-08-02; the distinction stays for the next game in that
		// position. The `[Netplay] ForceUnsupported` setting overrides this verdict (and only
		// this one) so such a game can still be measured on two machines.
		bool NetplaySupported();

		// Sets the module's is_vf20 config byte, which selects Virtua Fighter 2 version 2.0 or
		// 2.1. The two are different games mechanically, so netplay has to agree on it.
		//
		// Deliberately NOT a live setting: nothing re-reads it per frame. The module's
		// backup-RAM injector (HLE hook 8) reads it each time the ROM initialises the board, so
		// a write here takes effect on the NEXT board init - which is exactly what a netplay
		// round already does before frame 0. Writing it without a reset changes nothing.
		//
		// Returns false, having written nothing, for a game with no such byte.
		bool SetVf2Version20(bool version20);

		// Sets the module's is_vs_mode config byte (config +0x0A), which all three games read.
		//
		// This is not a difficulty tweak - it selects a different simulation. Set, the module
		// force-credits both coin counters, skips the attract boot, writes SRAM cabinet mode 3
		// instead of 2, and draws the stage from the SECOND host RNG stream (StF 0-8, FV %9,
		// VF2 %11) rather than the ROM's fixed sequence. A peer with it clear never touches that
		// generator at all, so two peers disagreeing diverge immediately and for good.
		//
		// Same timing contract as SetVf2Version20: nothing re-reads it per frame, so it takes
		// effect at the next board init and must be written BEFORE the round-start reset.
		//
		// Returns false, having written nothing, for a game whose config base is not yet measured
		// (currently StF - see DwGame::rvaConfigVsMode).
		bool SetVsMode(bool vsMode);

		// ---- The emulated i960's four interval timers ----------------------------------------
		//
		// These are what VF2's one-frame divergence is measured with. The CPU loop runs
		// instructions in batches of TWELVE and then decrements every enabled channel by
		// twelve, so a channel's count is an instruction budget, not a clock - and the drop in
		// any enabled channel across one emulated frame is twelve times the number of complete
		// batches that frame. That makes this a direct readout of instructions-executed-per-
		// frame, which is the quantity two peers have to agree on and (in VF2) do not.
		//
		// See docs/vf2-hle-hooks.md. Layout read out of the CPU loop's own decompilation
		// (VF2 FUN_1800210C0) rather than pattern-matched.
		struct I960Timers
		{
			uint8_t  enabled[4];   // ctx+0x190 + n*8: nonzero while the channel is counting
			int32_t  count[4];     // ctx+0x194 + n*8: instructions remaining, -12 per batch
			uint32_t pending;      // ctx+0x188: raised interrupt bits
			uint32_t mask;         // ctx+0x18C: which raised bits actually interrupt
			uint32_t ip;           // ctx+0x08:  IP within the live code bank
			uint8_t  yield;        // ctx+0x1B0: the ROM's frame-yield flag, cleared on return
		};

		// Fills `out` from the live i960 context. False (leaving `out` zeroed) if no game is
		// running, the board has not booted, or the context pointer is not up yet - so a caller
		// may call it every frame from boot without checking anything first.
		bool ReadI960Timers(I960Timers& out);

		// The value this peer submits to the desync canary for the current emulated frame.
		//
		// StF and FV submit the ROM's frame_counter, which advances exactly once per
		// module_main in those games. VF2 hashes a slice of work RAM instead: its counter is
		// bumped by an interrupt that can land either side of the module_main boundary, so it
		// jitters by a frame even when the two simulations are identical - measured, and it was
		// producing false desyncs on a game that was in sync. See the DwGame comment for the
		// evidence. 0 if no game is running.
		uint32_t StateCheckValue();

		// Whether StateCheckValue() returns a HASH (true) or the ROM's own frame counter (false),
		// which decides how the plugin is allowed to compare two peers' values.
		//
		// This is not a detail the plugin can infer, and getting it wrong is silent: a counter
		// tolerates a constant offset between the peers and a hash does not, so comparing a hash
		// the counter way either fires on the frame after the baseline or - worse - baselines a
		// genuine divergence and then treats it as expected. See
		// yampnet_match_config::state_check_exact.
		bool StateCheckIsHash();
}
