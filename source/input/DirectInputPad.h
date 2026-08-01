#pragma once

// The DirectInput half of the controller layer. XInput only ever sees Xbox-compatible pads, and
// a great deal of what people plug into an arcade emulator is not one: USB arcade encoders,
// fight sticks, PSX/Saturn adapters and most third-party pads are plain HID game controllers.
// Those are invisible to XInput - which is why they used to read "(not connected)" - and
// DirectInput is the API that still enumerates them all.
//
// Kept behind its own header so Input.cpp does not have to include dinput.h, and so the whole
// backend can no-op if DirectInput is unavailable.

#include <cstdint>
#include <string>
#include <vector>

namespace Input
{
	struct PadState;

	namespace DI
	{
		struct DeviceInfo
		{
			std::string id;   // "dinput:{instance-guid}" - stable across replug, see Input::PadDevice
			std::string name; // the device's own product name
		};

		// DirectInput needs a top-level window for SetCooperativeLevel; until it has one the
		// backend stays dormant (Enumerate returns nothing). RenderWindow supplies it.
		void SetWindow(void* hwnd);

		// Fills `out` with the attached game controllers, excluding the ones XInput also reports
		// (so a single Xbox pad never appears twice in the picker).
		//
		// `rescan` drives the expensive half. IDirectInput8::EnumDevices is NOT cheap - measured
		// at ~100 ms on the development machine, dominated by the installed virtual-pad driver -
		// which is a tenth of a second of frozen gameplay every time it runs. So it runs only
		// when the device set can actually have changed (startup, a WM_DEVICECHANGE, a device
		// that stopped answering); otherwise this just hands back the cached list. Polling the
		// devices themselves costs ~0.005 ms and stays per-frame.
		void Enumerate(std::vector<DeviceInfo>& out, bool rescan);

		// Reads one device into YAMP's shared pad state. False = the device stopped responding
		// (unplugged); the caller marks it disconnected and the next Enumerate drops it.
		bool Poll(const std::string& id, PadState& state);

		void Shutdown();
	}
}
