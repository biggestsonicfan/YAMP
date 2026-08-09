#pragma once

// The m2ftg cabinet's FRONT PANEL - the host<->module protocol blocks every m2ftg host was
// carrying its own copy of (LJ StF/FV/MR, YLAD VF2, K2 VF2/VON). Everything here drives the
// module CONTRACT - the execute_info status word (bit0 pause in, bit5 coin in, bit6 "press
// start" out) and the fixed button-assign table - so it is a property of the module family,
// not of any game or host generation. What stays per-host is the execute_info layout itself
// (LJ copies a 0xE0 pad prefix, K2 embeds the whole csl_pad) and each host's lock conditions,
// which are passed in as plain bools.

#include <cstdint>

#include "../../input/Input.h"

namespace pxd
{
	struct csl_pad;
}

namespace m2ftg
{
	namespace Cabinet
	{
		// ---- Escape pause menu ---------------------------------------------------------------
		// Edge-detected Escape toggle driving the module's own pause path (status bit0); the
		// host draws the shell (DrawPauseMenu in HostUI) around `open`. `locked` - a netplay
		// session or a live cabinet link - force-closes the menu and ignores the key: the
		// module's pause stops the emulated board, and stopping it on one machine only desyncs
		// a lockstep pair or trips a linked peer's ring watchdog within seconds. There is no
		// per-player pause in an arcade match.
		struct PauseMenu
		{
			void Poll(bool locked);
			bool open = false;

		private:
			bool m_escWasDown = false;
		};

		// ---- Dedicated coin binding ----------------------------------------------------------
		// Edge-detected per player; true = insert a credit this frame (the caller ORs status
		// bit5). `suppressed` covers freeplay (there is no coin to insert), the pause menu, and
		// a netplay session (the coin bit is not transmitted, so honouring it would raise a
		// credit on one machine only - players use START online, which travels in the
		// synchronised pad). Edges advance even while suppressed, so a held button does not
		// fire the moment suppression lifts.
		struct CoinBinding
		{
			bool Pressed(bool suppressed);

		private:
			bool m_wasDown[2] = {};
		};

		// ---- TEST / SERVICE ------------------------------------------------------------------
		// The cabinet service panel (see InstallSystemSwitches): TEST opens the board's own
		// service menu - the operator's input test - and SERVICE is the credit / navigate
		// button beside it. Held lines, like the panel; the ROM decides what latches. Either
		// player's binding closes the one switch. Suppress while paused or in a netplay
		// session: neither is in the transmitted pad, so honouring them would drop one machine
		// into the operator's menu (or feed it a service credit) while the other carried on.
		// `forceTest` ORs the host's own TEST press in (K2's role reset drives the switch
		// itself) - ORed rather than overriding, so a user holding the real switch is never
		// fought, and never suppressed, because the host asked for it.
		void PollSystemSwitches(bool suppressed, bool forceTest = false);

		// ---- Pad routing ---------------------------------------------------------------------
		// set_state for both players, with the netplay slot swap. ONLINE, YOU ALWAYS PLAY AS
		// PLAYER 1 LOCALLY: the guest occupies pad slot 1 in the match, but they are the only
		// person at that keyboard and have their own Player 1 bindings - making them remap to
		// the Player 2 controls just to play online would be absurd. So this machine's P1
		// bindings drive whichever slot this machine owns on the wire.
		void RoutePads(pxd::csl_pad pads[2], bool netplayMatch, int32_t netplayLocalPad);

		// ---- Button assignments --------------------------------------------------------------
		// The FIXED module-facing table (slot order A, B, Y, X, LT, LB, RT, RB). Remapping
		// happens host-side in csl_pad::set_state, which routes each player's bound inputs onto
		// the button bit carrying the wanted combo - so this must be Input::MODULE_ASSIGN, the
		// table those routes assume. Using a module's raw template instead shifts every combo
		// button by one slot - found and fixed once in the YLAD VF2 host; this helper exists so
		// that bug cannot be re-introduced one host at a time.
		template <typename ExecuteInfo>
		inline void FillAssignTable(ExecuteInfo& info)
		{
			for (int p = 0; p < 2; p++)
			{
				for (int i = 0; i < 8; i++)
				{
					info.assign[p][i] = Input::MODULE_ASSIGN[i];
				}
			}
		}

		// ---- Master volume -------------------------------------------------------------------
		// YAMPSettings::m_volumePercent as the module's own sound_volume fraction (100% = 1.0f,
		// exactly what every host passed before the setting existed) - the module's mixer does
		// the attenuation.
		float VolumeFraction();

		// ---- The arcade coin/start protocol --------------------------------------------------
		// LJ FUN_142494450, verbatim: while the module shows the start screen (status bit6
		// out), a START press becomes a COIN INSERT (status bit5 in; the press itself is
		// swallowed), then START is injected on alternating frames until the module leaves the
		// start screen. ONLY meaningful with the freeplay dip switch OFF - with is_freeplay=1
		// the module takes START directly and there is no coin to insert; the dance would just
		// swallow the player's first press.
		//
		// Under NETPLAY, Run() must happen AFTER NetSession::Step(), not before: the dance both
		// reads and writes pad[0], and Step() replaces the pads wholesale, so running it on the
		// local pads would set the coin status bit from one machine's inputs only. That WAS a
		// desync once: identical inputs, different execute_info.status.
		class CoinStart
		{
		public:
			// The dance itself. Pad is the generation's pad block (LJ's m2ftg pad or K2's
			// embedded csl_pad - both carry m_now/m_push), Status the execute_info status word.
			template <typename Pad, typename Status>
			void Run(Pad& pad0, Status& status)
			{
				if (m_coinPending && m_startScreen)
				{
					if (!m_startToggle)
					{
						pad0.m_now |= 0x100;
						pad0.m_push |= 0x100;
					}
					m_startToggle = !m_startToggle;
				}
				else
				{
					m_coinPending = false;
					if (m_startScreen && (pad0.m_now & 0x100) != 0)
					{
						status |= 0x20;
						pad0.m_now &= ~0x100u;
						pad0.m_push &= ~0x100u;
						m_coinPending = true;
						m_startToggle = false;
					}
				}
			}

			// Module->host feedback, after module_main: status bit6 = "press start" screen
			// active (LJ mirrors it to scene+0x2B58 and gates the injection on it).
			template <typename Status>
			void NoteStatus(Status status)
			{
				m_startScreen = (status & 0x40) != 0;
			}

		private:
			bool m_startScreen = false;
			bool m_coinPending = false;
			bool m_startToggle = false;
		};
	}
}
