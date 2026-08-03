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

	// ---- Timer-channel trace (Netplay/TimerTrace) -----------------------------------------
	//
	// One line per emulated netplay frame, on both peers, so the two logs diff directly. What
	// it is for: VF2's two simulations run one emulated frame apart, and the mechanism is that
	// the i960's timers count INSTRUCTIONS (twelve per batch) rather than wall-clock, so the
	// frame interrupt lands either side of the ROM's frame yield depending on how many
	// instructions that frame happened to take. This makes that count observable.
	//
	// Held here rather than in NetSession so the class keeps its published state; it is
	// single-threaded module-thread state, same as everything else EndFrame touches.
	m2ftg::I960Timers g_prevTimers = {};
	bool g_havePrevTimers = false;

	// How the frame's timer movement is summarised. Measured on VF2, attract mode:
	//
	//   t0  expired early in the boot and is never re-armed  (enabled=0, count=-9 forever)
	//   t1  re-armed to the same value every frame           (delta 0 or +-12)
	//   t2  likewise
	//   t3  the live one: counts down 11,000-14,600 per frame, and every second frame or so
	//       expires and is re-armed by the ROM, which reads as a large NEGATIVE delta
	//
	// So a channel's delta only means "instructions executed" while it is counting DOWN. On a
	// frame where the informative channel was re-armed, the largest decrease left is a floor,
	// not the total - which is why the re-arm set is reported alongside it instead of being
	// smoothed away. Two peers that re-arm different channels on the same netplay frame have
	// already diverged, and that is visible here directly.
	struct TimerDelta
	{
		int32_t  charged;   // largest decrease; -1 = no channel enabled at both samples
		uint32_t rearmed;   // bit n set = channel n's count ROSE, i.e. the ROM re-armed it
	};

	TimerDelta MeasureTimers(const m2ftg::I960Timers& prev, const m2ftg::I960Timers& now)
	{
		TimerDelta out = { -1, 0 };
		for (size_t i = 0; i < 4; ++i)
		{
			if (prev.enabled[i] == 0 || now.enabled[i] == 0)
			{
				continue;
			}
			const int32_t delta = prev.count[i] - now.count[i];
			if (delta < 0)
			{
				out.rearmed |= 1u << i;
				continue;   // a reload is not an instruction count; never let it win the max
			}
			if (delta > out.charged)
			{
				out.charged = delta;
			}
		}
		return out;
	}

	// One trace line for the emulated frame that just finished. `netFrame` is the netplay frame
	// index, or UINT32_MAX outside a round.
	//
	// Sampled AFTER module_main has returned, so `ins` covers exactly one emulated frame of CPU
	// work and the counts are the state the next frame starts from. Both peers write the same
	// fields in the same order at the same netplay frame number, so the two logs line up with a
	// plain diff and no post-processing.
	//
	// Goes to yampnet.log rather than DebugLog: that sink is not compiled out of a Release
	// build, and it opens and closes per line, so the trace survives killing the process and can
	// be read live over a share from the other machine.
	//
	// Takes the sample rather than taking it itself: EndFrame needs the same numbers for its
	// stall test, and reading the context twice would be both wasteful and a chance for the two
	// readings to disagree.
	void TraceTimers(uint32_t netFrame, const m2ftg::I960Timers& t, const TimerDelta& delta,
	                 uint32_t check)
	{
		const YAMPSettings* settings = gGeneral.GetSettings();
		if (settings == nullptr || !settings->m_netTimerTrace)
		{
			return;
		}

		uint32_t romFrame = 0;
		m2ftg::ReadEmulatedRam32(m2ftg::RomFrameCounterAddress(), romFrame);

		char frameLabel[16];
		if (netFrame == UINT32_MAX)
		{
			frameLabel[0] = '-';
			frameLabel[1] = '\0';
		}
		else
		{
			snprintf(frameLabel, sizeof(frameLabel), "%u", netFrame);
		}

		// Per-channel delta, positive = counted down. Only meaningful against a baseline, so it
		// reads 0 on the first traced frame after a gap (where `ins` reads -1 to say so).
		int32_t d[4] = {};
		for (size_t i = 0; i < 4; ++i)
		{
			d[i] = g_havePrevTimers ? g_prevTimers.count[i] - t.count[i] : 0;
		}

		// Field order is the diff contract: two peers' logs are compared line for line at equal
		// f=, so nothing here may be conditional on anything host-local.
		net::Logf("timers f=%s rom=%u chk=0x%08X ins=%d rearm=%X ip=0x%06X yield=%u irq=%02X/%02X "
		          "t0=%d,%u,%d t1=%d,%u,%d t2=%d,%u,%d t3=%d,%u,%d",
		          frameLabel, romFrame, check, delta.charged, delta.rearmed,
		          t.ip, static_cast<unsigned>(t.yield), t.pending, t.mask,
		          t.count[0], static_cast<unsigned>(t.enabled[0]), d[0],
		          t.count[1], static_cast<unsigned>(t.enabled[1]), d[1],
		          t.count[2], static_cast<unsigned>(t.enabled[2]), d[2],
		          t.count[3], static_cast<unsigned>(t.enabled[3]), d[3]);
	}
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
			// VS mode, same window and for a stronger reason: it decides whether the module
			// credits both players, which cabinet mode byte reaches SRAM, and whether the stage
			// comes from the SECOND host RNG stream or the ROM's fixed sequence. A peer with it
			// clear never draws from that generator at all, so a mismatch is not a shade of
			// difference - the two machines are running different games.
			const bool wantVs = net::EffectiveVsMode(preset != nullptr && preset->m_m2VersusMode);
			const bool didVs = SetVsMode(wantVs);

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

			// *** KNOWN BUG, DO NOT "FIX" BY WAITING HERE ***
			//
			// On a GUEST this reads 0. Measured on two machines 2026-08-03: the guest logged
			// `RNG seeded 0x00000000, board reset` and only afterwards `barrier released; round 1
			// seed 0x813AC110`, while the host seeded with the real value - so the two peers reset
			// their boards drawing from differently seeded generators, which is a divergence before
			// frame 0 and precisely what this seeding exists to prevent.
			//
			// It is not an accident of timing. YampNet.h states the contract outright: the match
			// seed is "valid once state is SYNCING or IN_MATCH". Prep runs at IN_ROOM, BEFORE the
			// barrier, so reading it here is reading a value the ABI does not promise yet. It
			// happens to be populated on the host because the host generated it.
			//
			// Blocking prep until the seed is non-zero DEADLOCKS: prep would wait for the seed, the
			// seed arrives with the barrier, and the barrier only opens once both peers finish prep
			// and call begin_round. Tried 2026-08-03; the guest's Start Match silently did nothing.
			//
			// The real fix has to reconcile two requirements that currently conflict - seed BEFORE
			// the reset (the ROM's re-init draws from the generator), but the seed is not knowable
			// until after the barrier. Options: have the plugin publish the seed with the room so a
			// guest has it at IN_ROOM (best - it is the host's from room creation either way), or
			// run a second seed+reset+re-anchor cycle once the barrier releases.
			const bool didSeed = SeedHostRng(preSeed);

			// Then reset, and let the ROM come back up on its own - now drawing from a generator
			// both peers agree on. The texture budget is pinned NOW rather than at frame 0 so
			// the post-reset boot runs under the same rules on both machines too.
			const bool didReset = ResetBoard();
			const bool didBudget = SetTextureBudgetDeterministic(true);
			if (didReset && didBudget && didSeed)
			{
				m_prep = Prep::Resetting;
				// vsMode is reported rather than required: SetVsMode returns false for a game
				// whose config base is unmeasured or does not check out, and that must not stop a
				// round - it means both peers keep their own launch-time switch, which is the
				// behaviour that existed before the flag. It is logged because a peer silently
				// ignoring the room's VS setting looks exactly like a desync later on.
				net::Logf("RNG seeded 0x%08X, board reset, vsMode=%d(applied=%d); waiting for the "
				          "ROM to restart", preSeed, static_cast<int>(wantVs),
				          static_cast<int>(didVs));
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
	// Sample the machine ONCE, and use that one sample for both the trace and the stall test.
	m2ftg::I960Timers timers = {};
	const bool haveTimers = ReadI960Timers(timers);
	const TimerDelta delta = (haveTimers && g_havePrevTimers)
		? MeasureTimers(g_prevTimers, timers)
		: TimerDelta{ -1, 0 };

	// Desync canary: at a given netplay frame this MUST read the same on both machines, and it
	// stops matching the moment one side executes a different amount of game code. WHAT gets
	// submitted is per-game - StF and FV send the ROM's frame_counter, VF2 a hash of work RAM.
	// See StateCheckValue().
	const uint32_t check = StateCheckValue();

	// Trace unconditionally: it runs whether or not a round is in progress, so the
	// instrumentation can be validated on one machine before two are booked to run it. Out of a
	// round the frame label is "-", which is also how the log shows where the round began.
	TraceTimers(m_frame, timers, delta, check);

	g_prevTimers = timers;
	g_havePrevTimers = haveTimers;

	// DID THIS module_main CALL ACTUALLY ADVANCE THE EMULATED FRAME?
	//
	// Often it does not. `module_main` returning is a HOST event; the emulated board completing
	// a frame is a guest one, and they are not the same. The CPU loop returns as soon as the
	// ROM's yield flag is set, so a call can execute almost nothing - and measured 2026-08-02
	// over ~12,000 frames on two machines, ~5% of VF2's calls execute NOTHING: the canary, every
	// timer channel and the ROM's frame counter are all unmoved.
	//
	// Which calls those are depends on host timing, not on simulation state. Proven in the round
	// that produced this fix: both peers were bit-identical through netplay frame 1 (same canary,
	// same 2,988 instructions, same timer counts), then the guest's frame-2 call ran nothing
	// while the host's ran a full frame. The guest then submitted its frame-1 value AS frame 2 -
	// which is exactly the "one emulated frame apart" this game has always shown. The simulations
	// never disagreed; the frame NUMBERING did.
	//
	// So a call that advanced nothing must not consume a netplay frame. Skipping it leaves
	// m_frame where it was, and the next host frame re-runs the same netplay frame with the same
	// inputs - the identical path an ordinary lockstep stall already takes.
	//
	// The test is deliberately conservative: it declares a stall only when the canary is
	// unchanged AND no timer channel counted down AND none was re-armed (a re-arm is a guest
	// store, so it proves the CPU ran). Against those 12,000 frames it caught 227 and 171 stalls
	// with ZERO false positives; the ~14 per run it misses are counted as real frames, which is
	// the safe direction to be wrong in. StF and FV are unaffected - they only stall during boot,
	// long before a round can start.
	const bool advanced = !m_haveLastCheck
		|| check != m_lastCheck
		|| delta.charged > 0
		|| delta.rearmed != 0;

	m_lastCheck = check;
	m_haveLastCheck = true;

	if (m_frame == UINT32_MAX || !advanced)
	{
		return;
	}

	net::Api()->submit_state_check(net::Session(), m_frame, check);
	++m_frame;
}
