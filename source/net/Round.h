#pragma once

#include <cstdint>

#include "YampNet.h"

namespace net
{
	// The lockstep round SHELL shared by the family netplay drivers (m2ftg::NetSession and
	// pre3::NetSession): the plugin-facing bookkeeping the two had line for line - the status
	// snapshot, the poll prologue, the begin_round announcement, the step/timeout/desync
	// handling, the teardown re-arm and the canary/frame-numbering tail. Held by COMPOSITION:
	// each family's driver keeps its own name, its four-call-point contract, and - deliberately
	// - every line of its round-start determinism sequencing. Those sequences are NOT shared
	// and must not be: m2ftg's pre-barrier seed -> reset -> settle -> anchor and pre3's
	// post-barrier clock-pin -> savestate-restore are each the measured fix for their own
	// family's failure mode (the two NetSession headers carry the write-ups).
	//
	// One log format converged in the merge: the DESYNC line now prints local/peer as hex on
	// both families (m2ftg printed decimal). It is terminal diagnosis, not part of the
	// per-frame diff contract, so the formats were free to agree.
	class Round
	{
	public:
		struct Status
		{
			// A session is up - NOT merely "a round is live". Every host->module input that is
			// not part of the transmitted pad has to be dead for the whole session: coin,
			// TEST/SERVICE, pause, the debug windows, the HLE mask. The window between pressing
			// Start and frame 0 is the dangerous one - an input in there changes one machine's
			// state on frames that are supposed to end up identical.
			bool locked;
			// A round is live: inputs are being exchanged and the pads are authoritative.
			bool inMatch;
			// Which pad slot this machine drives on the wire: 0 = host, 1 = guest, -1 = local.
			int32_t localPad;
		};
		Status GetStatus() const;

		// The Drive() prologue: clears the per-frame hold; when the plugin is up, polls it,
		// drives the session state machine (connect -> discovery -> host/join, idempotent) and
		// reports the session state. False when netplay is unavailable - the caller returns.
		bool Poll(yampnet_state& state);

		// Announce the round barrier: fills the match config from settings (frame delay,
		// redundancy 10, stall timeout 10 s) and calls begin_round with the next round number.
		// Round numbers count up so late packets from the PREVIOUS round cannot poison this
		// one; a guest's count would drift from the host's (neither knows how many rounds the
		// other played), so the plugin makes the HOST authoritative and a guest adopts the
		// host's, exactly as it adopts the seed. Wraps at 32 on the wire, which adoption makes
		// harmless.
		//
		// `exactCheck` is yampnet_match_config::state_check_exact: set when the family's canary
		// is a HASH (only equality means anything), clear when it is a COUNTER (a constant
		// offset between peers is harmless). On success the round is marked requested.
		bool Begin(bool exactCheck);

		// End the round: run the family's un-pin (texture budget / deterministic clock), clear
		// the Start request, tell the plugin, drop out of the round and re-arm - end_round
		// leaves the session IN_ROOM, so without the re-arm Start match would never open a
		// second barrier. `why`, when non-null, is logged.
		void End(void (*unpin)(), const char* why);

		// The lockstep step: feeds the pads through the plugin (which overwrites BOTH with the
		// transmitted inputs) and reads back the verdict.
		//
		//   Ready      the frame may run.
		//   Stalled    an input is missing; the frame must not run - that skip IS the stall.
		//   RoundOver  the peer timed out or disconnected (logged, and the player is told via
		//              NotePeerLost), or the desync canary tripped (logged with the frame to
		//              investigate; stopping AT the divergence is the point - both screens then
		//              hold the last frame the machines still agreed on). The caller runs its
		//              own EndRound and lets the frame advance back into local play.
		enum class StepVerdict { Ready, Stalled, RoundOver };
		StepVerdict Step(void* executeInfo);

		// The teardown at the bottom of Drive(): true when the session fell back to a state
		// with no round in it (idle / failed / online-no-room). Re-arms the shell, and if
		// anything was in flight - a requested round, or the family's own prep machine
		// (`prepActive`) - clears the Start request and runs the family's un-pin, so the next
		// round has to announce again and the Start press that opened the last barrier does not
		// carry over. The caller resets its own prep state when this returns true.
		bool DriveTeardown(yampnet_state state, bool prepActive, void (*unpin)());

		// The canary + frame numbering, split in two because the families disagree about when
		// each happens: m2ftg advances only on frames its stall test says really ran (a skipped
		// submit re-runs the same netplay frame), pre3 advances even on a frame whose canary
		// could not be taken.
		void SubmitCheck(uint32_t check);
		void AdvanceFrame() { ++m_frame; }

		// UINT32_MAX means "not in a netplay round"; anything else is the frame the lockstep
		// engine keys inputs by.
		bool InRound() const { return m_frame != UINT32_MAX; }
		uint32_t Frame() const { return m_frame; }
		void BeginFrameZero() { m_frame = 0; }

		bool Requested() const { return m_roundRequested; }

		// The emulator freeze while an announced barrier waits for the peer. Recomputed by the
		// family's Drive() every frame; Poll() clears it first so a frame that returns early
		// never holds.
		void SetHold(bool hold) { m_hold = hold; }
		bool Hold() const { return m_hold; }

	private:
		uint32_t m_frame = UINT32_MAX;
		uint32_t m_roundNumber = 0;
		bool m_roundRequested = false;
		bool m_hold = false;
	};
}
