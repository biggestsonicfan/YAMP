#pragma once

// Virtual On driven by a REAL Sega Saturn Twin Stick, through a Bliss-Box adapter.
//
// Virtual On is a twin-lever cabinet. Every other control path YAMP offers reaches those levers
// through the module's five-entry pad-mapping table (K2Host's assign[0][4] note), which exists to
// squeeze two four-way levers, two weapon triggers and two dash buttons onto a gamepad — either by
// pre-composing gestures onto buttons (the beginner types) or by spending the whole pad on them
// (type 4). A Saturn Twin Stick needs none of that: it IS the cabinet's control set, one physical
// switch per cabinet input, and this file is the wiring that says so.
//
// It is a controller OVERRIDE, not another mapping: when it is on and a stick is attached, the
// player's key/pad bindings do not fill that player's pad at all. Nothing is remapped, nothing is
// approximated, and there is no binding for a player to get wrong.
//
// See BlissBox.h for the adapter protocol and the Twin Stick's decode; the module half — which
// csl_pad bit becomes which cabinet input, and how it was established — is documented in the .cpp.

#include <cstdint>

#include "../../pxd/LJ/sl.h"

namespace m2ftg
{
	namespace K2
	{
		namespace TwinStick
		{
			// Which Bliss-Box port drives `player`, honouring the setting's Auto mode: Auto hands
			// player 0 the first twin-stick-capable port and player 1 the next one, so one stick
			// on any port just plays and a second stick needs no configuration either. Returns -1
			// when this player has no stick, which is the caller's cue to leave the pad alone.
			int PortForPlayer(unsigned int player);

			// Fills `pad` from the twin stick on `port`, replacing whatever the binding layer put
			// there — including the edge sets, so m_push/m_pull stay honest. False means the port
			// stopped answering between PortForPlayer and here (a mid-frame unplug), and the
			// caller should keep the binding-driven pad it already had.
			bool SetPadState(pxd::csl_pad& pad, int port);

			// True when the override is enabled in settings AND this is Virtual On. The port may
			// still be empty — ask PortForPlayer for that.
			bool Enabled();
		}
	}
}
