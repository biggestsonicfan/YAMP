#include "Cabinet.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "../SystemSwitches.h"
#include "../../YAMPGeneral.h"
#include "../../YAMPSettings.h"
#include "../../pxd/LJ/sl.h"

namespace m2ftg
{
	namespace Cabinet
	{
		void PauseMenu::Poll(bool locked)
		{
			if (locked)
			{
				open = false;   // close it if a session started while it was open
			}
			const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
			if (escDown && !m_escWasDown && !locked)
			{
				open = !open;
			}
			m_escWasDown = escDown;
		}

		bool CoinBinding::Pressed(bool suppressed)
		{
			bool pressed = false;
			for (int p = 0; p < 2; p++)
			{
				const bool down = Input::ActionDown(p, Input::Action_Coin);
				if (down && !m_wasDown[p] && !suppressed)
				{
					pressed = true;
				}
				m_wasDown[p] = down;
			}
			return pressed;
		}

		void PollSystemSwitches(bool suppressed, bool forceTest)
		{
			bool test = forceTest;
			bool service = false;
			if (!suppressed)
			{
				for (int p = 0; p < 2; p++)
				{
					test = test || Input::ActionDown(p, Input::Action_Test);
					service = service || Input::ActionDown(p, Input::Action_Service);
				}
			}
			SetSystemSwitches(test, service);
		}

		void RoutePads(pxd::csl_pad pads[2], bool netplayMatch, int32_t netplayLocalPad)
		{
			if (netplayMatch && netplayLocalPad >= 0)
			{
				// The other slot is filled from the P2 bindings purely to keep it well-formed
				// for this frame: NetSession::Step() overwrites BOTH pads from the transmitted
				// inputs before module_main sees them, so nothing local survives into the
				// simulation (see PadCodec.h).
				pads[netplayLocalPad].set_state(0);
				pads[1 - netplayLocalPad].set_state(1);
			}
			else
			{
				pads[0].set_state(0);
				pads[1].set_state(1);
			}
		}

		float VolumeFraction()
		{
			return static_cast<float>(gGeneral.GetSettings()->m_volumePercent) / 100.0f;
		}
	}
}
