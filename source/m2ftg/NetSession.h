#pragma once

#include <cstdint>

#include "m2ftg.h"
#include "../net/Round.h"

namespace m2ftg
{
	// The netplay session driver, shared by every m2ftg host loop.
	//
	// This used to live inline in the Lost Judgment host's GameLoop, which is why Fighting
	// Vipers got netplay for free (it runs on that same loop) and Virtua Fighter 2 could not
	// (YLAD/VF2.cpp has its own). None of what follows is LJ-specific: it drives the plugin,
	// sequences the round-start handshake, and reads the determinism helpers in DebugWindows.h -
	// all of which are keyed off the running game, not off a particular host.
	//
	// WHY A ROUND NEEDS SEQUENCING AT ALL. Getting two emulators to start from the SAME state
	// needs more than a barrier, because a barrier only proves both peers are present. Measured
	// 2026-08-01: resetting the board and starting frame 0 in the same instant left the ROM
	// ~40 frames into its post-reset boot when the match began, and the two machines reached the
	// ROM's main loop one emulated frame apart - identical inputs applied one ROM frame out of
	// step, which compounds into a visible AI desync.
	//
	// So frame 0 is anchored to the ROM's OWN progress instead of to a wall-clock moment:
	//   Idle      -> WAIT for the shared match seed, then seed the RNG, reset the board and pin
	//                the texture budget. The seed must precede the reset because the ROM's
	//                post-reset initialisation draws from the generator, so a peer that reset
	//                without it has already diverged. Only a guest ever waits, and never on
	//                anything the other peer has to do first - see get_match_seed in YampNet.h.
	//   Resetting -> wait for frame_counter to restart (proving the reset landed)
	//   Settling  -> wait for frame_counter to reach ANCHOR - the same value on both peers
	//   Announced -> HOLD the emulator here and open the barrier
	//   (barrier) -> RE-seed from the same value (a safety net against the two boots having drawn
	//                different NUMBERS of values) and run frame 0
	// Both machines therefore enter frame 0 with their ROM at the same counter value, by
	// construction rather than by luck.
	//
	// ---- How a host loop drives it -------------------------------------------------------
	//
	// Four call points, in this order, once per host frame. The ORDER IS PART OF THE CONTRACT:
	// GetStatus() is deliberately read from last frame's state, before Drive() can advance it, so
	// that pad routing and the coin protocol see a stable answer for the whole frame.
	//
	//   const auto st = net.GetStatus();     // (1) top of frame, before input is polled
	//   ... poll input, fill execute_info.pad[] ...
	//   net.Drive();                         // (2) poll the plugin + run the round-start machine
	//   const bool advance = net.Step(info); // (3) after the pads are filled; may overwrite them
	//   ... arcade coin/start protocol, which must run off the SYNCHRONISED pads ...
	//   if (advance) { module_main(...); net.EndFrame(); }   // (4) only if the frame really ran
	//
	// A host that never calls these behaves exactly as it did before netplay existed, and a
	// game with no measured frame counter (RomFrameCounterAddress() == 0) never starts a round.
	class NetSession
	{
	public:
		// The shared shape (net/Round.h). The m2ftg wrinkle on `locked`: the window between
		// pressing Start and frame 0 is dangerous here because the board is reset and then runs
		// FREELY until it reaches the anchor - the anchor only aligns the ROM's frame counter,
		// not the rest of the board.
		using Status = net::Round::Status;
		Status GetStatus() const;

		// Polls the plugin and runs the round-start state machine. Safe to call every frame
		// whether or not netplay is loaded.
		void Drive();

		// Returns whether module_main may run this frame.
		//
		// Under lockstep the emulator may only advance once every player's input for this frame
		// is known, so a stalled frame simply does not call module_main - that skip IS the
		// stall. The plugin also overwrites BOTH pads in `info` with the transmitted inputs
		// (this machine's included), which is why the host must fill its local pads FIRST:
		// whatever the local bindings produced is a prediction the network's copy must replace,
		// or the two machines simulate different inputs and desync.
		bool Step(m2ftg_execute_info_t& info);

		// Call after every module_main call. Submits the desync canary and advances the netplay
		// frame index - but ONLY if that call actually advanced the emulated frame.
		//
		// It often does not. `module_main` returning is not the same event as the emulated board
		// completing a frame: measured 2026-08-02, ~5% of VF2's calls execute nothing at all (the
		// ROM's frame counter, the work-RAM hash and every timer channel are all unmoved), and
		// WHICH calls those are is host-timing-dependent, not simulation state. Counting one as a
		// netplay frame is what put the two peers one emulated frame apart: their simulations were
		// bit-identical, their frame NUMBERING was not. StF and FV only ever stall this way during
		// boot, before a round can start, which is why they never showed it.
		void EndFrame();

	private:
		enum class Prep { Idle, Resetting, Settling, Announced };

		void EndRound(const char* why);

		// The plugin-facing shell (frame numbering, round bookkeeping, step/desync handling)
		// lives in net::Round, shared with pre3's driver. The frame advances ONLY on frames the
		// emulator actually ran, which is why EndFrame() is a separate call rather than part of
		// Step().
		net::Round m_round;
		Prep m_prep = Prep::Idle;
		uint32_t m_anchor = 0;
		// Previous frame's canary value, for the "did the emulator actually advance?" test in
		// EndFrame. Kept across the whole session (not just a round) so frame 0 has a baseline.
		uint32_t m_lastCheck = 0;
		bool m_haveLastCheck = false;
		// Whether "waiting for the host's match seed" has already been said for this wait, so a
		// guest that pressed Start first reports the wait once instead of every frame.
		bool m_loggedSeedWait = false;
	};

	// The one instance. A session is a property of the process, not of a host loop, and the
	// hosts are mutually exclusive anyway (one game runs per launch).
	NetSession& NetplaySession();
}
