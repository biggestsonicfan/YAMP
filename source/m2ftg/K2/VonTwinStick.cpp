#include "VonTwinStick.h"

#include "../../input/BlissBox.h"
#include "../../YAMPGeneral.h"

#include <utility>

namespace m2ftg
{
	namespace K2
	{
		namespace TwinStick
		{
			// =====================================================================================
			// WHAT THE MODULE ACTUALLY READS
			//
			// Getting a lever to move is two hops, and BOTH of them permute, which is why guessing
			// at this produces a stick that turns left when you push it up.
			//
			// HOP 1 — csl_pad to the module's own button mask. omg's pad reader FUN_180081140 does
			// not take execute_info's pad bits as they lie. It rebuilds a mask, and the direction
			// and face-button groups come out reordered, because the module enumerates directions
			// U, R, D, L where sl enumerates them U, D, L, R:
			//
			//     sl BUTTON_UP    (bit12) -> module bit12       sl BUTTON_A (bit0) -> module bit2
			//     sl BUTTON_DOWN  (bit13) -> module bit14       sl BUTTON_B (bit1) -> module bit1
			//     sl BUTTON_LEFT  (bit14) -> module bit15       sl BUTTON_X (bit2) -> module bit3
			//     sl BUTTON_RIGHT (bit15) -> module bit13       sl BUTTON_Y (bit3) -> module bit0
			//     sl BUTTON_START (bit8)  -> module bit9        sl BUTTON_BACK (bit9) -> module bit8
			//     LB/RB/LT/RT (bits 4-7) pass through unchanged.
			//
			// The module's bits 16-23 — its L_UP..R_RIGHT — are NOT read from csl_pad at all. They
			// are derived from the four ANALOG floats (m_x1/m_y1/m_x2/m_y2) against a threshold of
			// 0x4651/0x7FFF ~ 0.55, in the same U, R, D, L order:
			//
			//     m_y1 <= -0.55 -> bit16      m_x1 >= +0.55 -> bit17
			//     m_y1 >= +0.55 -> bit18      m_x1 <= -0.55 -> bit19        (left  stick group)
			//     m_y2 <= -0.55 -> bit20      m_x2 >= +0.55 -> bit21
			//     m_y2 >= +0.55 -> bit22      m_x2 <= -0.55 -> bit23        (right stick group)
			//
			// SO THE RIGHT LEVER IS UNREACHABLE FROM BUTTON BITS. It exists only behind m_x2/m_y2,
			// and a fill that sets sl::BUTTON_R_UP and friends moves nothing whatsoever.
			//
			// HOP 2 — module mask to cabinet input. FUN_180004cc0 walks the selected table entry's
			// 24 {mask -> code} pairs and sets one bit per matched code in a 110-bit set, so any
			// number of cabinet inputs can be held at once. Entry 3 (the full twin-stick set)
			// yields, after hop 1:
			//
			//     LEFT LEVER   up 0x03   down 0x04   left 0x05   right 0x06
			//     RIGHT LEVER  up 0x0B   down 0x0C   left 0x0D   right 0x0E
			//     BUTTONS      0x07 (LB)   0x08 (LT)   0x09 (RB)   0x0A (RT)
			//
			// AND THE TABLE PROVES ITS OWN DIRECTION LABELS, which is the part worth trusting.
			// The beginner entries reach the same levers through composed gesture codes, and the
			// composer spells out what each gesture is made of:
			//     0x70 -> {0x03, 0x0B}  both levers up ......... walk forward
			//     0x71 -> {0x04, 0x0C}  both levers down ....... walk back
			//     0x72 -> {0x05, 0x0D}  both levers left ....... strafe left
			//     0x73 -> {0x06, 0x0E}  both levers right ...... strafe right
			//     0x76 -> {0x05, 0x0E}  left LEFT + right RIGHT  = both outward = JUMP
			//     0x77 -> {0x06, 0x0D}  left RIGHT + right LEFT  = both inward  = CROUCH
			// Jump and crouch are the two gestures every Virtual On player knows by feel, and they
			// only come out as jump and crouch under this labelling — which is also exactly what
			// the beginner scheme was observed to do in-game (K2Host's note: Punch read as crouch,
			// Guard as jump).
			//
			// The four button codes pair up as {0x07, 0x09} and {0x08, 0x0A}: code 0x6F, the
			// composer's one button gesture, expands to {0x07, 0x09}, and "both triggers" is the
			// cabinet's centre-weapon input. That makes 0x07/0x09 the two WEAPON TRIGGERS and
			// 0x08/0x0A the two DASH thumb-buttons, left before right in each pair.
			//
			// All of the above is read out of omg-pxd-w64 itself: the table at DLL+0x12ABB0, the
			// scheme latch FUN_180004BE0, the composer FUN_180004CC0 and the pad reader
			// FUN_180081140.
			// =====================================================================================

			namespace
			{
				// Past the module's 0.55 threshold with room to spare, and the value a real lever
				// would produce: a four-way switch is either at the stop or centred.
				constexpr float LEVER = 1.0f;

				bool IsVirtualOn()
				{
					return gGeneral.GetGameId() == YAMPGeneral::GameId::VON_K2;
				}
			}

			bool Enabled()
			{
				const auto* settings = gGeneral.GetSettings();
				return IsVirtualOn() && settings != nullptr && settings->m_vonTwinStick;
			}

			int PortForPlayer(unsigned int player)
			{
				if (!Enabled())
				{
					return -1;
				}
				const auto* settings = gGeneral.GetSettings();
				const unsigned int slot = player & 1;
				const int configured = settings->m_vonTwinStickPort[slot];
				if (configured >= 0)
				{
					// An explicit pick is honoured even if the port is empty — silently sliding a
					// player onto somebody else's stick is worse than that player having no input
					// and being able to see why on the settings page.
					Input::BlissBox::PortState state;
					const bool usable = Input::BlissBox::GetPort(configured, state)
						&& state.IsTwinStickCapable();
					return usable ? configured : -1;
				}

				// Auto: first capable port to player 0, the next one to player 1. Player 1 must
				// skip whatever player 0 resolved to, or a single stick drives both pads.
				if (slot == 0)
				{
					return Input::BlissBox::FindTwinStick();
				}
				const int taken = settings->m_vonTwinStickPort[0] >= 0
					? settings->m_vonTwinStickPort[0]
					: Input::BlissBox::FindTwinStick();
				return Input::BlissBox::FindTwinStick(taken);
			}

			bool SetPadState(pxd::csl_pad& pad, int port)
			{
				Input::BlissBox::PortState state;
				if (!Input::BlissBox::GetPort(port, state) || !state.IsTwinStickCapable())
				{
					return false;
				}
				const Input::BlissBox::TwinStickState& stick = state.stick;

				// Same contract as csl_pad::set_state: this owns the whole pad for the frame,
				// edges included, so the binding layer's m_now is discarded rather than merged.
				pad.m_prev = std::exchange(pad.m_now, 0);
				pad.m_x1 = pad.m_y1 = pad.m_x2 = pad.m_y2 = 0.0f;
				for (uint8_t& pressure : pad.m_buttons)
				{
					pressure = 0;
				}

				auto press = [&pad](pxd::sl::BUTTON button)
					{
						pad.m_now |= (1u << button);
						pad.m_buttons[button] = 0xFF;
					};

				// LEFT LEVER on the d-pad bits. m_x1/m_y1 are driven to match: entry 3 maps the
				// module's analog-derived left group to the same four codes, so a stick that is
				// digitally up is also analogue-up and the two can never disagree.
				if (stick.leftUp)    { press(pxd::sl::BUTTON_UP);    pad.m_y1 = -LEVER; }
				if (stick.leftDown)  { press(pxd::sl::BUTTON_DOWN);  pad.m_y1 = LEVER; }
				if (stick.leftLeft)  { press(pxd::sl::BUTTON_LEFT);  pad.m_x1 = -LEVER; }
				if (stick.leftRight) { press(pxd::sl::BUTTON_RIGHT); pad.m_x1 = LEVER; }

				// RIGHT LEVER. Axes only — see hop 1 above; there is no button bit that reaches
				// it. Opposite directions cancel to centre, which is what the lever does too.
				if (stick.rightUp)    pad.m_y2 -= LEVER;
				if (stick.rightDown)  pad.m_y2 += LEVER;
				if (stick.rightLeft)  pad.m_x2 -= LEVER;
				if (stick.rightRight) pad.m_x2 += LEVER;

				// Weapon triggers and dash thumb-buttons, left pair then right pair.
				if (stick.leftTrigger)  press(pxd::sl::BUTTON_LB);   // -> code 0x07
				if (stick.leftThumb)    press(pxd::sl::BUTTON_LT);   // -> code 0x08
				if (stick.rightTrigger) press(pxd::sl::BUTTON_RB);   // -> code 0x09
				if (stick.rightThumb)   press(pxd::sl::BUTTON_RT);   // -> code 0x0A

				// START goes on sl BUTTON_START, which hop 1 turns into the module's bit 9 and
				// entry 3 maps to code 0x02 — the same byte every other start path in YAMP already
				// produces (the coin/start dance and -von-autostart both set this bit), so the
				// stick's Start behaves exactly like the one that works today. Worth knowing that
				// the module's OTHER start-shaped code, 0x6D, hangs off sl BUTTON_BACK because the
				// reader swaps those two bits; nothing here wants it, and BACK is deliberately
				// left unset so it cannot fire by accident.
				if (stick.start) press(pxd::sl::BUTTON_START);

				pad.m_push = ~pad.m_prev & pad.m_now;
				pad.m_pull = pad.m_prev & ~pad.m_now;
				return true;
			}
		}
	}
}
