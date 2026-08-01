#pragma once

// The DirectInput half of the controller layer. XInput only sees Xbox-compatible pads; arcade
// encoders, fight sticks, adapters and most third-party pads are plain HID game controllers and
// invisible to it. Kept behind its own header so Input.cpp need not include dinput.h.

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
		// (so a single Xbox pad never appears twice in the picker). `rescan` drives the expensive
		// half: EnumDevices measures ~100 ms, so it runs only when the device set can actually
		// have changed; otherwise this hands back the cached list. Poll is ~0.005 ms per frame.
		void Enumerate(std::vector<DeviceInfo>& out, bool rescan);

		// Reads one device into YAMP's shared pad state. False = the device stopped responding
		// (unplugged); the caller marks it disconnected and the next Enumerate drops it.
		bool Poll(const std::string& id, PadState& state);

		void Shutdown();
	}
}
