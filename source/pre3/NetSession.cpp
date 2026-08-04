#include "NetSession.h"

#include "Determinism.h"
#include "SystemSwitches.h"

#include "../YAMPGeneral.h"
#include "../YAMPSettings.h"
#include "../net/NetPlugin.h"

#include <cstring>

namespace
{
	// How long the savestate restore is allowed to stay outstanding before the round is abandoned.
	//
	// It lands on the first frame in every observed case - the frame step drains the request
	// bitfield at the top of the update stage, and the only thing that defers it is the module's
	// own two busy flags. This bound exists for the case where the request is silently DROPPED (a
	// state whose recorded size does not match what the machine expects is discarded without a
	// word), which would otherwise hold both peers at a frozen screen with nothing in the log.
	//
	// It is also the only thing watching the network during the restore: Step() does not call the
	// plugin while the reset is outstanding, so a peer that vanishes in that window is noticed
	// here rather than as a lockstep timeout. One frame of exposure in practice; this is the cap.
	constexpr uint32_t RESET_FRAME_BUDGET = 60;

	// Put a pad back to "nothing is being pressed", keeping the identity fields the module reads.
	//
	// Used on the one frame that performs the savestate restore. That frame runs a full simulation
	// step immediately after the restore, so whatever it is fed is the first thing the two peers
	// compute from their newly identical state - and the local pads are the one input that is NOT
	// identical at that moment. Zeroing both on both machines is what makes that frame agree.
	//
	// Deliberately the same 0xE0-byte prefix the host fills from csl_pad, so this cannot drift from
	// what the input path writes: everything past it is the port/user/connected identity, which the
	// module does read and which must survive.
	void NeutralisePad(pxd::lj_pad_t& pad, unsigned int player)
	{
		memset(&pad, 0, pxd::kLjPadCopyBytes);
		pad.m_port = player;
		pad.m_user_id = static_cast<int>(player);
		pad.m_is_connected = true;
		pad.m_is_remote = false;
	}
}

pre3::NetSession& pre3::NetplaySession()
{
	static NetSession s_session;
	return s_session;
}

pre3::NetSession::Status pre3::NetSession::GetStatus() const
{
	Status st = {};
	st.inMatch = net::IsAvailable() && m_frame != UINT32_MAX;
	st.localPad = net::IsAvailable() ? net::Api()->get_local_player(net::Session()) : -1;
	st.locked = net::SessionInProgress();
	return st;
}

void pre3::NetSession::EndRound(const char* why)
{
	// Un-pin the clock. It was pinned as part of a round that is not happening any more, and a
	// stand-alone session has no reason to be told it is permanently 2000-01-01.
	SetDeterministicClock(false);
	net::ClearStartRequest();
	if (net::IsAvailable())
	{
		net::Api()->end_round(net::Session());
	}
	m_frame = UINT32_MAX;
	// end_round leaves us IN_ROOM, so re-arm the request flag or Start match would never open a
	// second barrier.
	m_roundRequested = false;
	m_prep = Prep::Idle;
	m_resetFrames = 0;
	if (why != nullptr)
	{
		net::Logf("%s", why);
	}
}

void pre3::NetSession::Drive()
{
	m_hold = false;
	if (!net::IsAvailable())
	{
		return;
	}

	const yampnet_api* api = net::Api();
	yampnet_session* sess = net::Session();
	api->poll(sess);
	net::DriveSession();   // connect -> discovery -> host/join (idempotent)

	const yampnet_state ns = api->get_state(sess);

	// Room is up, but only announce when we are allowed to: the command-line harness auto-starts,
	// a UI session waits for the host's Start button.
	//
	// IsBoardBooted is a HARD precondition rather than a nicety, for the same reason it is on the
	// m2ftg path: a guest reaches the barrier within a couple of hundred milliseconds of launching,
	// long before its board is up, and every determinism call this driver makes is a no-op until it
	// is. Because each peer gates its own announce, the barrier comes to mean "both boards are
	// ready", which is what it was always supposed to mean.
	if (ns == YAMPNET_STATE_IN_ROOM && !m_roundRequested && NetplaySupported()
		&& net::ShouldStartRound() && IsBoardBooted())
	{
		const YAMPSettings* settings = gGeneral.GetSettings();
		yampnet_match_config mc = {};
		mc.frame_delay = static_cast<uint8_t>(
			settings != nullptr && settings->m_netFrameDelay > 0 ? settings->m_netFrameDelay : 3);
		mc.input_redundancy = 10;
		mc.stall_timeout_ms = 10000;
		// The canary is a HASH, so the peers' values must match exactly - the plugin's other mode
		// baselines the difference between them, which only means anything for a counter. See
		// yampnet_match_config::state_check_exact.
		mc.state_check_exact = 1;
		// Round number: counts up per round so late packets from the PREVIOUS round cannot poison
		// this one. The plugin makes the HOST authoritative and a guest adopts whatever round the
		// host announces, exactly as it adopts the seed.
		++m_roundNumber;
		if (api->begin_round(sess, m_roundNumber, &mc) == YAMPNET_OK)
		{
			m_prep = Prep::Announced;
			m_roundRequested = true;
			net::Logf("barrier opened, emulator held until the peer arrives");
		}
	}

	// Announced but the barrier has not released: FREEZE the emulator.
	//
	// Cheaper here than on the m2ftg path, and worth stating because it looks like the same line.
	// There, holding is about keeping an anchor meaningful. Here it is about not letting this
	// machine run frames the peer does not, in a window that ends with both boards being restored
	// from the same file anyway - so the freeze is belt and braces rather than the mechanism.
	m_hold = (m_prep == Prep::Announced && ns != YAMPNET_STATE_IN_MATCH && m_frame == UINT32_MAX);

	// Barrier released. THIS is where the determinism work happens, and doing it here rather than
	// before the barrier is the whole reason this driver is not a copy of m2ftg's: the match seed
	// is only promised from SYNCING onwards, so anything that consumes it before the barrier reads
	// zero on a guest (m2ftg has exactly that bug, documented in its own Drive()). A pre3 reset is
	// one savestate restore, so it fits after the barrier where the seed is real.
	if (ns == YAMPNET_STATE_IN_MATCH && m_prep == Prep::Announced)
	{
		const uint32_t seed = api->get_match_seed(sess);
		const int32_t me = api->get_local_player(sess);

		// The board's real-time clock, pinned to the shared seed. This is the pre3 analogue of
		// m2ftg's "re-seed the rand hook": there is no rand hook here, and M3EInput::get_time is
		// the only caller of _time64 in the whole module - so this single value is the entirety of
		// what the two boards have to agree about beyond their inputs.
		//
		// A zero seed would be a plugin that has not delivered one; fall back to the module's own
		// reset epoch rather than pin the board to 1970, which is a date its own code never
		// produces. Both peers apply the same rule to the same value, so either way they agree.
		SetDeterministicClock(true, seed != 0 ? static_cast<long long>(seed) : DETERMINISTIC_EPOCH);

		// Suppress the cabinet switches for the whole round. They are sampled at read time from a
		// host-side mask and are not part of the transmitted pad, so one machine holding TEST would
		// drop into the operator's menu while the other played. The host loop also refuses to set
		// them while a session is up; this is the belt to that brace, and it covers the frame on
		// which the round starts.
		SetSystemSwitches(false, false);

		// WHICH state the round starts from is the ROOM'S choice, never this machine's - both
		// peers must restore the same bytes or there is nothing for lockstep to keep in step. The
		// local setting is only what this machine OFFERS when it hosts; a guest adopts the host's.
		const YAMPSettings* settings = gGeneral.GetSettings();
		const bool vsStart = net::EffectivePre3VsStart(
			settings != nullptr && settings->m_netPre3VsStart);

		// Both are the module's own mechanisms and both land in one frame; they differ only in
		// which saved machine gets restored. See Determinism.h.
		const bool restored = vsStart ? ResetBoard() : RestoreResetSnapshot();
		if (!restored)
		{
			EndRound(vsStart
				? "ABORT: the board's versus start state could not be restored - refusing the "
				  "round rather than starting the two peers from different states"
				: "ABORT: no power-on snapshot on this machine - refusing the round rather than "
				  "starting the two peers from different states");
			return;
		}

		m_prep = Prep::Resetting;
		m_resetFrames = 0;
		net::Logf("match start: player=%d seed=0x%08X; clock pinned, board restoring from its %s",
		          me, seed, vsStart ? "VS start state" : "power-on state");
	}

	// Waiting for the restore to land. Frames still run - the restore happens INSIDE the update
	// stage - but Step() neutralises the pads so the frame that performs it is identical on both
	// machines.
	if (m_prep == Prep::Resetting)
	{
		if (!BoardRestorePending())
		{
			m_prep = Prep::Live;
			m_frame = 0;
			net::Logf("board restored after %u frame(s); frame 0 starts now", m_resetFrames);
		}
		else if (++m_resetFrames > RESET_FRAME_BUDGET)
		{
			EndRound("ABORT: the board's savestate restore never completed - the module dropped "
			         "the request, so the two peers would not be starting from one state");
		}
	}

	// Back to a state with no round in it - the session ended, or the lobby left the room.
	if (ns == YAMPNET_STATE_IDLE || ns == YAMPNET_STATE_FAILED || ns == YAMPNET_STATE_ONLINE)
	{
		if (m_roundRequested || m_prep != Prep::Idle)
		{
			net::ClearStartRequest();
			SetDeterministicClock(false);
		}
		m_roundRequested = false;
		m_prep = Prep::Idle;
		m_frame = UINT32_MAX;
		m_resetFrames = 0;
	}
}

bool pre3::NetSession::Step(pre3_execute_info_t& info)
{
	// LAST HOST CODE BEFORE THE EMULATOR THREAD IS RELEASED. Whatever the rest of this function
	// decides, the board's sequence marker has to be captured here, because the update stage that
	// follows is what lets the worker run the next frame - and EndFrame can only tell "the frame
	// finished" from "the frame has not started" by watching this value move. See
	// pre3::WaitForEmulatedFrame for what goes wrong without it.
	m_frameMarker = BoardFrameMarker();

	bool advanceFrame = !m_hold;
	if (!net::IsAvailable())
	{
		return advanceFrame;
	}

	// The restore frame. Run it - the restore only happens because the update stage runs - but
	// hand it nothing local. See NeutralisePad.
	if (m_prep == Prep::Resetting)
	{
		for (unsigned int p = 0; p < 2; p++)
		{
			NeutralisePad(info.pad[p], p);
		}
		// Host->module status: no coin, no pause. Both are local one-shots that would otherwise
		// land on one machine only.
		info.status &= ~0x21;
		return advanceFrame;
	}

	if (m_frame == UINT32_MAX)
	{
		return advanceFrame;
	}

	const yampnet_step st = net::Api()->step(net::Session(), m_frame, &info);
	advanceFrame = (st == YAMPNET_STEP_READY);

	if (st == YAMPNET_STEP_TIMEOUT || st == YAMPNET_STEP_DISCONNECTED)
	{
		net::Logf("round ended (step=%d); returning to local play", static_cast<int>(st));
		// Tell the player. Both ends of a lost match see this: the peer that went away is gone,
		// and the one still running would otherwise find itself back in attract mode with no
		// explanation.
		net::NotePeerLost(st == YAMPNET_STEP_TIMEOUT
		                      ? "The other player stopped responding."
		                      : "The other player disconnected.");
		EndRound(nullptr);
		advanceFrame = true;
	}

	// Divergence detected. Stop AT the divergence rather than playing on: both screens then hold
	// the last frame the two machines still agreed on, and the frame number in the log is the one
	// to investigate.
	uint32_t dFrame = 0, dLocal = 0, dRemote = 0;
	if (net::Api()->get_desync(net::Session(), &dFrame, &dLocal, &dRemote) != 0)
	{
		net::Logf("DESYNC at frame %u (local=0x%08X, peer=0x%08X) - ending the round; the two "
		          "emulators stopped agreeing here", dFrame, dLocal, dRemote);
		EndRound(nullptr);
		advanceFrame = true;
	}
	return advanceFrame;
}

void pre3::NetSession::EndFrame(const pre3_execute_info_t& info)
{
	if (m_frame == UINT32_MAX)
	{
		return;
	}

	// The canary, submitted once per EMULATED frame. No "did the frame really advance?" test is
	// needed or wanted here: the board's per-frame CPU budget is a fixed instruction count, so an
	// update stage that ran is exactly one emulated frame. (That test is load-bearing on m2ftg,
	// where ~5% of VF2's calls execute nothing at all - see m2ftg/NetSession.cpp. The difference is
	// real and measured, not an assumption carried over.)
	//
	// A zero means the hash could not be taken. Submitting it would have both peers "agree" on a
	// value neither of them computed, which is worse than no canary - so skip the frame's check
	// and keep the frame count moving.
	// WAIT FOR THE EMULATED FRAME TO ACTUALLY FINISH before reading anything out of the board.
	// The update stage returning does not mean the frame is over - the emulator runs on the module's
	// own "m3e_ctrl" worker thread and the update stage releases it on the way out. Sampling here
	// without waiting is a data race, and it is what ended a two-machine round at frame 4 with
	// identical settings, an identical seed and a verifiably identical restore. See
	// pre3::WaitForEmulatedFrame.
	uint32_t cpu = 0, ram = 0;
	const bool settled = WaitForEmulatedFrame(m_frameMarker);
	const bool haveParts = settled && StateCheckParts(cpu, ram);
	const uint32_t check = haveParts ? StateCheckValue() : 0;

	// ---- The per-frame trace (Netplay/TimerTrace) --------------------------------------------
	//
	// One line per netplay frame, on both peers, so the two logs diff directly - the same tool the
	// m2ftg path built to find VF2's one-frame offset, and needed here for the same reason: when
	// the canary says two boards disagree, it does not say WHICH board state disagreed.
	//
	// The two halves are printed separately because that split is most of the diagnosis:
	//
	//   cpu differs, ram agrees -> the emulated program took a different path. A real desync.
	//   ram differs, cpu agrees -> something wrote into guest memory from outside the simulation.
	//                              On this module that means the render stages or the display
	//                              thread, and it is a fault in what the canary covers rather
	//                              than in the round.
	//   both differ             -> a real desync, several frames after the cause if the RAM half
	//                              was the first to notice (it moves sluggishly - see
	//                              Determinism.h).
	//
	// Goes to yampnet.log rather than DebugLog for the reasons m2ftg's does: that sink survives a
	// Release build, and it opens and closes per line so the trace can be read live over a share
	// from the other machine while the round is still running.
	{
		const YAMPSettings* settings = gGeneral.GetSettings();
		if (settings != nullptr && settings->m_netTimerTrace)
		{
			// Field order is the diff contract: two peers' logs are compared line for line at
			// equal f=, so nothing here may be conditional on anything host-local.
			net::Logf("pre3 f=%u chk=0x%08X cpu=0x%08X ram=0x%08X mode=%02X/%02X "
			          "p0=0x%06X p1=0x%06X status=0x%X settled=%d",
			          m_frame, check, cpu, ram, info.game_mode, info.game_sub_mode,
			          info.pad[0].m_now, info.pad[1].m_now, info.status,
			          static_cast<int>(settled));
		}
	}

	if (check != 0)
	{
		net::Api()->submit_state_check(net::Session(), m_frame, check);
	}
	else
	{
		// Said once, loudly: a round that runs with no canary looks exactly like a round that
		// runs correctly, right up until someone reports that the two screens diverged and there
		// is nothing in either log to say where.
		static bool s_warned = false;
		if (!s_warned)
		{
			s_warned = true;
			net::Logf("WARNING: the desync canary is unavailable (%s) - this round has no "
			          "divergence detection",
			          settled ? "the board's RAM could not be read"
			                  : "the emulator thread did not finish its frame in time");
		}
	}
	++m_frame;
}
