#pragma once

#include <cstdint>

#include "pre3.h"
#include "../net/Round.h"

namespace pre3
{
	// The netplay session driver for the Model 3 boards.
	//
	// A deliberate sibling of m2ftg::NetSession rather than a reuse of it: the two share the
	// plugin, the ABI and the four-call-point shape, and nothing else. Everything that made the
	// m2ftg driver long is machinery for problems this board does not have.
	//
	// WHAT PRE3 DOES NOT NEED, and why the file is a third the size:
	//
	//   * No RNG seeding. There is no `rand` hook anywhere in the module - searched exhaustively.
	//     The board's only host-varying input is its real-time clock, and that is one atomic store
	//     (pre3::SetDeterministicClock). The match seed still gets used; it just lands on the clock
	//     instead of on a Mersenne Twister.
	//   * No texture-budget pin. Nothing in the frame path samples elapsed time. The per-frame CPU
	//     budget is computed once as cpu_clock/60 and split by compile-time constants, so Fighting
	//     Vipers 2 executes 2,216,666 instructions per emulated frame on every machine.
	//   * No "did this call actually advance a frame?" test. On m2ftg it is load-bearing - ~5% of
	//     VF2's module_main calls execute nothing, which is what put two identical simulations one
	//     frame apart. Here the update stage and the emulator's worker thread are two halves of a
	//     strict per-frame handshake (the stage waits for the worker at the top and releases it on
	//     the way out), so one update stage is one emulated frame, exactly.
	//
	// WHAT IT MUST RESPECT INSTEAD: the emulator is on ITS OWN THREAD, and the board is only safe
	// to read while that thread is parked - which is during the update stage, not after it. That
	// is why EndFrame waits on the board's frame marker before touching anything. Reading the
	// canary straight after the update stage returned is a data race, and it ended a two-machine
	// round at frame 4 that had an identical seed, identical settings and a verifiably identical
	// restore. See pre3::WaitForEmulatedFrame.
	//   * No settling window. m2ftg's reset restarts the ROM and lets it boot for ~40 frames, so
	//     frame 0 has to be anchored to the ROM's own counter to mean the same thing on both peers.
	//     A pre3 reset is a savestate restore: one frame, and the state afterwards is the file's
	//     bytes on both machines by construction.
	//
	// WHAT IT ADDS. Because the reset is instantaneous, the whole determinism sequence happens
	// AFTER the start barrier rather than before it - which is the fix for a real m2ftg bug it
	// would otherwise have inherited. There, the RNG has to be seeded before the reset while the
	// match seed is only promised from SYNCING onwards, so a guest seeds with zero and the peers
	// diverge before frame 0 (see the KNOWN BUG note in m2ftg/NetSession.cpp). Doing the work after
	// the barrier makes that unreachable: the seed is valid, the clock is pinned from it, and the
	// board is restored from it, all on a machine that is already synchronised with its peer.
	//
	//   Idle      -> announce once the board has booted, and HOLD the emulator
	//   (barrier) -> pin the clock to the match seed, ask for the savestate restore
	//   Resetting -> run frames with NEUTRALISED inputs until the restore lands
	//   Live      -> frame 0
	//
	// The neutralised frame matters and is not a detail. The restore happens at the top of the
	// update stage and that stage then simulates a whole frame; if the two peers fed it their own
	// local pads, they would diverge on the first frame after being made identical.
	//
	// ---- How the host loop drives it ---------------------------------------------------------
	//
	// Four call points, in this order, once per host frame. The ORDER IS PART OF THE CONTRACT:
	// GetStatus() is deliberately read from last frame's state, before Drive() can advance it, so
	// pad routing and the input suppression see a stable answer for the whole frame.
	//
	//   const auto st = net.GetStatus();     // (1) top of frame, before input is polled
	//   ... poll input, fill execute_info.pad[] ...
	//   net.Drive();                         // (2) poll the plugin + run the round-start machine
	//   const bool advance = net.Step(info); // (3) after the pads are filled; may overwrite them
	//   if (advance) { update(...); net.EndFrame(); }   // (4) only if the frame really ran
	//
	// A host that never calls these behaves exactly as it did before netplay existed.
	class NetSession
	{
	public:
		// The shared shape (net/Round.h). The pre3 wrinkle on `locked`: the window between
		// pressing Start and frame 0 is dangerous here because the board is about to be
		// restored, and one stray input in there changes this machine's state on the frame
		// that is supposed to be identical on both.
		using Status = net::Round::Status;
		Status GetStatus() const;

		// Polls the plugin and runs the round-start state machine. Safe to call every frame
		// whether or not netplay is loaded.
		void Drive();

		// Returns whether the update stage may run this frame.
		//
		// Under lockstep the emulator may only advance once every player's input for this frame is
		// known, so a stalled frame simply does not call the update stage - that skip IS the stall.
		// The plugin also overwrites BOTH pads in `info` with the transmitted inputs (this
		// machine's included), which is why the host must fill its local pads FIRST: whatever the
		// local bindings produced is a prediction the network's copy must replace, or the two
		// machines simulate different inputs and desync.
		//
		// Also what neutralises the pads on the restore frame - see the class comment.
		//
		// It is additionally where the board's frame marker is captured, because this is the last
		// host code to run before the update stage releases the emulator thread. EndFrame waits
		// for that marker to move - see pre3::WaitForEmulatedFrame.
		bool Step(pre3_execute_info_t& info);

		// Call after every update stage that actually ran. Submits the desync canary and advances
		// the netplay frame index.
		//
		// Takes the execute_info so the per-frame trace can carry the board's game-mode pair
		// alongside the canary. That pair is the cheapest window there is into what the board is
		// actually doing, and a trace whose two halves disagree is far easier to read when it also
		// says which screen each peer thought it was on.
		void EndFrame(const pre3_execute_info_t& info);

	private:
		enum class Prep { Idle, Announced, Resetting, Live };

		void EndRound(const char* why);

		// The plugin-facing shell (frame numbering, round bookkeeping, step/desync handling)
		// lives in net::Round, shared with m2ftg's driver.
		net::Round m_round;
		Prep m_prep = Prep::Idle;
		// How many frames the restore has been outstanding. It should land on the first one; this
		// is here so a request the module silently drops ends the round with a diagnosis instead
		// of hanging both machines at a black screen.
		uint32_t m_resetFrames = 0;
		// The board's per-frame sequence marker as it stood before the update stage released the
		// emulator thread. EndFrame waits for it to move before reading anything.
		unsigned long long m_frameMarker = 0;
	};

	// The one instance. A session is a property of the process, not of a host loop.
	NetSession& NetplaySession();
}
