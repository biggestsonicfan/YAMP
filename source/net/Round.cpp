#include "Round.h"

#include "NetPlugin.h"
#include "../YAMPGeneral.h"
#include "../YAMPSettings.h"

namespace net
{
	Round::Status Round::GetStatus() const
	{
		Status st = {};
		st.inMatch = net::IsAvailable() && m_frame != UINT32_MAX;
		st.localPad = net::IsAvailable() ? net::Api()->get_local_player(net::Session()) : -1;
		st.locked = net::SessionInProgress();
		return st;
	}

	bool Round::Poll(yampnet_state& state)
	{
		m_hold = false;
		if (!net::IsAvailable())
		{
			return false;
		}
		net::Api()->poll(net::Session());
		net::DriveSession();   // connect -> discovery -> host/join (idempotent)
		state = net::Api()->get_state(net::Session());
		return true;
	}

	bool Round::Begin(bool exactCheck)
	{
		const YAMPSettings* settings = gGeneral.GetSettings();
		yampnet_match_config mc = {};
		mc.frame_delay = static_cast<uint8_t>(
			settings != nullptr && settings->m_netFrameDelay > 0 ? settings->m_netFrameDelay : 3);
		mc.input_redundancy = 10;
		mc.stall_timeout_ms = 10000;
		mc.state_check_exact = exactCheck ? 1 : 0;
		++m_roundNumber;
		if (net::Api()->begin_round(net::Session(), m_roundNumber, &mc) != YAMPNET_OK)
		{
			return false;
		}
		m_roundRequested = true;
		return true;
	}

	void Round::End(void (*unpin)(), const char* why)
	{
		// Un-pin first: it was pinned as part of a round that is not happening any more, and
		// leaving it on would burden local play for no reason.
		unpin();
		net::ClearStartRequest();
		if (net::IsAvailable())
		{
			net::Api()->end_round(net::Session());
		}
		m_frame = UINT32_MAX;
		// end_round leaves us IN_ROOM, so re-arm the request flag or Start match would never
		// open a second barrier.
		m_roundRequested = false;
		if (why != nullptr)
		{
			net::Logf("%s", why);
		}
	}

	Round::StepVerdict Round::Step(void* executeInfo)
	{
		const yampnet_step st = net::Api()->step(net::Session(), m_frame, executeInfo);
		StepVerdict verdict = (st == YAMPNET_STEP_READY) ? StepVerdict::Ready : StepVerdict::Stalled;

		if (st == YAMPNET_STEP_TIMEOUT || st == YAMPNET_STEP_DISCONNECTED)
		{
			net::Logf("round ended (step=%d); returning to local play", static_cast<int>(st));
			// Tell the player. Both ends of a lost match see this: the peer that went away is
			// gone, and the one still running would otherwise just find itself back in attract
			// mode with no explanation.
			net::NotePeerLost(st == YAMPNET_STEP_TIMEOUT
			                      ? "The other player stopped responding."
			                      : "The other player disconnected.");
			verdict = StepVerdict::RoundOver;
		}

		// Divergence detected. Stop AT the divergence rather than playing on: both screens then
		// hold the last frame the two machines still agreed on, and the frame number in the log
		// is the one to investigate. Continuing would only pile consequences on top of the
		// cause.
		uint32_t dFrame = 0, dLocal = 0, dRemote = 0;
		if (net::Api()->get_desync(net::Session(), &dFrame, &dLocal, &dRemote) != 0)
		{
			net::Logf("DESYNC at frame %u (local=0x%08X, peer=0x%08X) - ending the round; the "
			          "two emulators stopped agreeing here", dFrame, dLocal, dRemote);
			verdict = StepVerdict::RoundOver;
		}
		return verdict;
	}

	bool Round::DriveTeardown(yampnet_state state, bool prepActive, void (*unpin)())
	{
		if (state != YAMPNET_STATE_IDLE && state != YAMPNET_STATE_FAILED
			&& state != YAMPNET_STATE_ONLINE)
		{
			return false;
		}
		if (m_roundRequested || prepActive)
		{
			net::ClearStartRequest();
			unpin();
		}
		m_roundRequested = false;
		m_frame = UINT32_MAX;
		return true;
	}

	void Round::SubmitCheck(uint32_t check)
	{
		net::Api()->submit_state_check(net::Session(), m_frame, check);
	}
}
