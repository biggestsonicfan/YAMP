#pragma once

#include <cstdint>

#include "m2ftg.h"

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
	//   Idle      -> reset the board, pin the texture budget
	//   Resetting -> wait for frame_counter to restart (proving the reset landed)
	//   Settling  -> wait for frame_counter to reach ANCHOR - the same value on both peers
	//   Announced -> HOLD the emulator here and open the barrier
	//   (barrier) -> seed the RNG (instantaneous, so it needs no settling) and run frame 0
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
		struct Status
		{
			// A session is up - NOT merely "a round is live". Every host->module input that is
			// not part of the transmitted pad has to be dead for the whole session: coin,
			// TEST/SERVICE, pause, the debug windows, the HLE mask. The window between pressing
			// Start and frame 0 is the dangerous one, because the board is reset and then runs
			// FREELY until it reaches the anchor - so an input pressed in there changes this
			// machine's state before the round begins, and the anchor only aligns the ROM's
			// frame counter, not the rest of the board.
			bool locked;
			// A round is live: inputs are being exchanged and the pads are authoritative.
			bool inMatch;
			// Which pad slot this machine drives on the wire: 0 = host, 1 = guest, -1 = local.
			int32_t localPad;
		};

		// Named GetStatus, not Status: a member function with the same name as the nested type
		// hides that type at every call site.
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

		// UINT32_MAX means "not in a netplay round"; anything else is the frame the lockstep
		// engine keys inputs by. It advances ONLY on frames the emulator actually ran, which is
		// why EndFrame() is a separate call rather than part of Step().
		uint32_t m_frame = UINT32_MAX;
		Prep m_prep = Prep::Idle;
		uint32_t m_anchor = 0;
		uint32_t m_roundNumber = 0;
		bool m_roundRequested = false;
		bool m_hold = false;
		// Previous frame's canary value, for the "did the emulator actually advance?" test in
		// EndFrame. Kept across the whole session (not just a round) so frame 0 has a baseline.
		uint32_t m_lastCheck = 0;
		bool m_haveLastCheck = false;
	};

	// The one instance. A session is a property of the process, not of a host loop, and the
	// hosts are mutually exclusive anyway (one game runs per launch).
	NetSession& NetplaySession();
}
