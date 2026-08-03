#include "NetSession.h"

#include "DebugWindows.h"   // the determinism helpers + RomFrameCounterAddress()
#include "../YAMPGeneral.h"
#include "../YAMPSettings.h"
#include "../net/NetPlugin.h"


namespace
{
	// A few frames into the ROM's main loop: far enough past the post-reset boot to be steady
	// state, small enough that starting a match stays snappy.
	constexpr uint32_t NETPLAY_ANCHOR = 8;
}

m2ftg::NetSession& m2ftg::NetplaySession()
{
	static NetSession s_session;
	return s_session;
}

m2ftg::NetSession::Status m2ftg::NetSession::GetStatus() const
{
	Status st = {};
	st.inMatch = net::IsAvailable() && m_frame != UINT32_MAX;
	st.localPad = net::IsAvailable() ? net::Api()->get_local_player(net::Session()) : -1;
	st.locked = net::SessionInProgress();
	return st;
}

void m2ftg::NetSession::EndRound(const char* why)
{
	// Un-pin the budget: it was pinned as part of a round that is not happening any more, and
	// leaving it on would slow local play for no reason.
	SetTextureBudgetDeterministic(false);
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
	if (why != nullptr)
	{
		net::Logf("%s", why);
	}
}

void m2ftg::NetSession::Drive()
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

	// Netplay needs the whole determinism set - reset, RNG seeding, texture-budget pin and the
	// ROM frame counter. The counter is the one that is per-game DATA rather than code, so it
	// doubles as the "is this game supported" test: a game with no measured counter cannot
	// anchor a round and must not try.
	const uint32_t frameCounterAddr = RomFrameCounterAddress();
	const bool netplayPossible = frameCounterAddr != 0 && NetplaySupported();

	// Room is up, but only start when we are allowed to: the command-line harness auto-starts,
	// a UI session waits for the host's Start button.
	//
	// IsBoardBooted is a HARD precondition, not a nicety. Announcing before the board is up
	// means the barrier can release while ResetBoard / SeedHostRng /
	// SetTextureBudgetDeterministic are all still no-ops on this machine, and the round then
	// starts with this peer's simulation un-reset and un-seeded. Because every peer gates its
	// own announce, the barrier itself comes to mean "both boards are ready", which is what it
	// was always supposed to mean.
	if (ns == YAMPNET_STATE_IN_ROOM && !m_roundRequested
		&& netplayPossible && net::ShouldStartRound() && IsBoardBooted())
	{
		if (m_prep == Prep::Idle)
		{
			// Adopt the ROOM's cabinet settings before resetting, not after: the module's
			// backup-RAM injector reads them while the board initialises, so this is the only
			// window in which they can reach the simulation without relaunching the game.
			// A host whose own switch has not moved sees no change; a guest silently plays the
			// host's version instead of its own, which is the whole point.
			const YAMPSettings* preset = gGeneral.GetSettings();
			SetVf2Version20(net::EffectiveVf2Version20(preset != nullptr && preset->m_vf2Version20));

			// SEED THE RNG BEFORE THE RESET, NOT AFTER.
			//
			// This used to happen at the barrier, on the reasoning that seeding "consumes no
			// emulated frames, which is why it can happen here while the reset could not". That
			// reasoning was about cost and missed the thing that actually matters: ORDER. The
			// reset makes the ROM re-run its whole initialisation - and that initialisation
			// DRAWS from the host RNG. Seeding afterwards leaves every one of those draws coming
			// from the module's own wall-clock seed, so the two peers build different initial
			// state and are already divergent before frame 0.
			//
			// Measured 2026-08-02: with the seeding late, the two boards' high-score tables came
			// out in different orders (readable as three-letter initials at RAM 0x59C194+, on a
			// 0x10 stride) along with ~50 bytes of related init state. That is ROM
			// initialisation, not gameplay, and it is exactly what the post-reset boot builds.
			//
			// Seeding here is safe: the match seed is known as soon as the room exists (the host
			// generates it at creation, the guest adopts it on join), and the generators are
			// constructed once per process (`if (holder == 0)`), so a board reset does not
			// discard them.
			const uint32_t preSeed = net::IsAvailable()
				? net::Api()->get_match_seed(net::Session()) : 0;
			const bool didSeed = SeedHostRng(preSeed);

			// Then reset, and let the ROM come back up on its own - now drawing from a generator
			// both peers agree on. The texture budget is pinned NOW rather than at frame 0 so
			// the post-reset boot runs under the same rules on both machines too.
			const bool didReset = ResetBoard();
			const bool didBudget = SetTextureBudgetDeterministic(true);
			if (didReset && didBudget && didSeed)
			{
				m_prep = Prep::Resetting;
				net::Logf("RNG seeded 0x%08X, board reset; waiting for the ROM to restart",
				          preSeed);
			}
			else
			{
				net::Logf("ABORT: round prep failed (seed=%d reset=%d texBudget=%d)",
				          static_cast<int>(didSeed), static_cast<int>(didReset),
				          static_cast<int>(didBudget));
				net::ClearStartRequest();
			}
		}

		uint32_t romFrame = 0;
		const bool haveRomFrame = ReadEmulatedRam32(frameCounterAddr, romFrame);

		// The counter still holds its PRE-reset value for a while, so "it is small again" is
		// what proves the reset actually landed. Anchoring without this check would latch onto
		// a stale value that differs per machine.
		if (m_prep == Prep::Resetting && haveRomFrame && romFrame <= 1)
		{
			m_prep = Prep::Settling;
		}

		if (m_prep == Prep::Settling && haveRomFrame && romFrame >= NETPLAY_ANCHOR)
		{
			const YAMPSettings* settings = gGeneral.GetSettings();
			yampnet_match_config mc = {};
			mc.frame_delay = static_cast<uint8_t>(
				settings != nullptr && settings->m_netFrameDelay > 0
					? settings->m_netFrameDelay : 3);
			mc.input_redundancy = 10;
			mc.stall_timeout_ms = 10000;
			// Round number: counts up per round so late packets from the PREVIOUS round cannot
			// poison this one. A guest's count would drift from the host's (neither knows how
			// many rounds the other played), so the plugin makes the HOST authoritative - a
			// guest adopts whatever round the host announces, exactly as it adopts the seed.
			// Wraps at 32 (5 bits on the wire), which is harmless: adoption keeps both sides on
			// the same value.
			++m_roundNumber;
			if (api->begin_round(sess, m_roundNumber, &mc) == YAMPNET_OK)
			{
				m_prep = Prep::Announced;
				m_anchor = romFrame;
				m_roundRequested = true;
				net::Logf("anchored at ROM frame_counter=%u; barrier opened, emulator held "
				          "until the peer arrives", romFrame);
			}
		}
	}

	// Announced but the barrier has not released: FREEZE the emulator. Without this the ROM
	// keeps running while we wait for the peer, and the anchor we just took is meaningless by
	// the time the round actually starts - the whole point is that both machines sit at the
	// same counter value when frame 0 begins.
	m_hold = (m_prep == Prep::Announced && ns != YAMPNET_STATE_IN_MATCH && m_frame == UINT32_MAX);

	// Barrier released: frame 0 starts now.
	if (ns == YAMPNET_STATE_IN_MATCH && m_frame == UINT32_MAX)
	{
		m_frame = 0;
		const uint32_t seed = api->get_match_seed(sess);
		const int32_t me = api->get_local_player(sess);

		// RE-seed. The generators were already seeded before the reset, which is what makes the
		// ROM's post-reset initialisation deterministic (see the note there). This second pass
		// is a safety net rather than the main event: it puts both peers into an identical RNG
		// state at frame 0 regardless of how many draws each one's init happened to consume, so
		// a difference in draw COUNT during the boot cannot carry into the match.
		//
		// Same seed value on both sides, so re-seeding cannot itself introduce a difference.
		const bool didSeed = SeedHostRng(seed);

		uint32_t romFrame = 0;
		ReadEmulatedRam32(frameCounterAddr, romFrame);
		net::Logf("match start: player=%d seed=0x%08X rngSeeded=%d anchor=%u romFrame=%u",
		          me, seed, static_cast<int>(didSeed), m_anchor, romFrame);

		// The RNG is the one step left that can still fail here, and a match with an unseeded
		// generator is guaranteed to diverge. Refuse it rather than play it.
		if (!didSeed)
		{
			EndRound("ABORT: RNG seeding failed on this peer - refusing the round rather than "
			         "desyncing");
		}
	}

	// Back to a state with no round in it - the session ended, or the lobby left the room.
	// Re-arm: the next round has to announce again, and the Start press that opened the last
	// barrier must not carry over into it.
	if (ns == YAMPNET_STATE_IDLE || ns == YAMPNET_STATE_FAILED || ns == YAMPNET_STATE_ONLINE)
	{
		if (m_roundRequested || m_prep != Prep::Idle)
		{
			net::ClearStartRequest();
			SetTextureBudgetDeterministic(false);
		}
		m_roundRequested = false;
		m_prep = Prep::Idle;
		m_frame = UINT32_MAX;
	}
}

bool m2ftg::NetSession::Step(m2ftg_execute_info_t& info)
{
	// Held at the anchor while the barrier waits for the peer. Same mechanism as a lockstep
	// stall - module_main simply does not run - so the last frame keeps being re-presented and
	// the overlay explains the wait.
	//
	// NOTE: an earlier version held the emulator at frame 0 instead, to stop the host drifting
	// through attract mode while it waited. That was wrong, and self-defeating: the board only
	// reports "booted" once module_main has actually run, so holding meant the board never
	// booted and ResetBoard() - gated on that - silently did nothing. Both machines then
	// started a round with no reset AND no shared state. The reset is the stronger mechanism,
	// so the emulator runs freely until the barrier and both sides are re-initialised at match
	// start instead; pre-barrier divergence does not matter when the board is about to be reset
	// out from under it.
	bool advanceFrame = !m_hold;
	if (!net::IsAvailable() || m_frame == UINT32_MAX)
	{
		return advanceFrame;
	}

	const yampnet_step st = net::Api()->step(net::Session(), m_frame, &info);
	advanceFrame = (st == YAMPNET_STEP_READY);

	if (st == YAMPNET_STEP_TIMEOUT || st == YAMPNET_STEP_DISCONNECTED)
	{
		net::Logf("round ended (step=%d); returning to local play", static_cast<int>(st));
		// Tell the player. Both ends of a lost match see this: the peer that went away is gone,
		// and the one still running would otherwise just find itself back in attract mode with
		// no explanation.
		net::NotePeerLost(st == YAMPNET_STEP_TIMEOUT
		                      ? "The other player stopped responding."
		                      : "The other player disconnected.");
		EndRound(nullptr);
		advanceFrame = true;
	}

	// Divergence detected. Stop AT the divergence rather than playing on: both screens then
	// hold the last frame the two machines still agreed on, and the frame number in the log is
	// the one to investigate. Continuing would only pile consequences on top of the cause.
	uint32_t dFrame = 0, dLocal = 0, dRemote = 0;
	if (net::Api()->get_desync(net::Session(), &dFrame, &dLocal, &dRemote) != 0)
	{
		net::Logf("DESYNC at frame %u (local=%u, peer=%u) - ending the round; the two emulators "
		          "stopped agreeing here", dFrame, dLocal, dRemote);
		EndRound(nullptr);
		advanceFrame = true;
	}
	return advanceFrame;
}

void m2ftg::NetSession::EndFrame()
{
	if (m_frame == UINT32_MAX)
	{
		return;
	}

	// Desync canary: at a given netplay frame this MUST read the same on both machines, and it
	// stops matching the moment one side executes a different amount of game code.
	//
	// WHAT gets submitted is per-game, because the obvious choice is not universally sound. StF
	// and FV submit the ROM's frame_counter, which in those games advances exactly once per
	// module_main. VF2's does not: its counter is bumped by an interrupt that can land either
	// side of the module_main boundary depending on host timing, so it jitters by a frame while
	// the simulations stay identical - which produced false desyncs on a game that was in sync.
	// VF2 therefore submits a hash of actual game state instead. See StateCheckValue().
	net::Api()->submit_state_check(net::Session(), m_frame, StateCheckValue());
	++m_frame;
}
