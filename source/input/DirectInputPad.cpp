#include "DirectInputPad.h"

#include "Input.h"
#include "../YAMPGeneral.h"
#include "../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>

#include <map>
#include <memory>

namespace Input::DI
{
	namespace
	{
		// Axes are rescaled to this range at open time so normalising is a plain divide and no
		// device-specific calibration is needed.
		constexpr LONG AXIS_RANGE = 1000;
		// Matches the XInput reader's radial deadzone, so both kinds of pad feel the same.
		constexpr float DEADZONE = 0.25f;
		// How far an axis must travel to count as a digital direction press. Well past the
		// deadzone: a resting analog stick drifts, and a binding prompt must not catch that.
		constexpr float AXIS_DIGITAL_THRESHOLD = 0.6f;

		struct ComDeleter
		{
			void operator()(IUnknown* p) const noexcept { if (p) p->Release(); }
		};
		using DevicePtr = std::unique_ptr<IDirectInputDevice8W, ComDeleter>;

		// A device that refuses DIPROP_RANGE keeps its own scale, which is usually 0..65535 with
		// the rest position in the MIDDLE - normalising that as if it were centred on zero would
		// peg the stick permanently in one corner. So the real range is read back per axis and
		// used as-is, and only the requested range is assumed when even the query fails.
		struct AxisRange
		{
			LONG min = -AXIS_RANGE;
			LONG max = AXIS_RANGE;
		};

		struct OpenDevice
		{
			DevicePtr device;
			std::string name;
			AxisRange axes[DI_AXIS_COUNT];
			bool seen = false;   // survived the most recent enumeration
		};

		HWND s_hwnd = nullptr;
		std::unique_ptr<IDirectInput8W, ComDeleter> s_di;
		std::map<std::string, OpenDevice> s_devices;
		bool s_createFailed = false;

		std::string GuidToId(const GUID& guid)
		{
			wchar_t buf[64] = {};
			if (StringFromGUID2(guid, buf, static_cast<int>(std::size(buf))) == 0)
			{
				return {};
			}
			return "dinput:" + WcharToUTF8(buf);
		}

		bool EnsureDirectInput()
		{
			if (s_di != nullptr)
			{
				return true;
			}
			// No window yet means no legal cooperative level, so there is nothing useful to do -
			// and no reason to latch a failure, since the window arrives moments later.
			if (s_hwnd == nullptr || s_createFailed)
			{
				return false;
			}

			IDirectInput8W* di = nullptr;
			const HRESULT hr = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
				IID_IDirectInput8W, reinterpret_cast<void**>(&di), nullptr);
			if (FAILED(hr) || di == nullptr)
			{
				// Only DirectInput itself being missing lands here, which is permanent.
				s_createFailed = true;
				DebugLog("[input] DirectInput8Create failed (0x%08X) - only XInput pads will be listed.\n",
					static_cast<unsigned>(hr));
				return false;
			}
			s_di.reset(di);
			return true;
		}

		// XInput pads are ALSO enumerated by DirectInput, where they show up as a generic
		// controller with mangled triggers and no reliable names. Reading one twice would put a
		// duplicate in the picker and let a player bind the same physical button under two
		// identities. Microsoft's own marker for this is the device interface path: every XInput
		// device's path contains "ig_", and nothing else does.
		bool IsXInputDevice(IDirectInputDevice8W* device)
		{
			DIPROPGUIDANDPATH prop = {};
			prop.diph.dwSize = sizeof(prop);
			prop.diph.dwHeaderSize = sizeof(DIPROPHEADER);
			prop.diph.dwHow = DIPH_DEVICE;
			if (FAILED(device->GetProperty(DIPROP_GUIDANDPATH, &prop.diph)))
			{
				return false;
			}
			for (const wchar_t* p = prop.wszPath; *p != L'\0'; p++)
			{
				if ((p[0] == L'i' || p[0] == L'I') && (p[1] == L'g' || p[1] == L'G') && p[2] == L'_')
				{
					return true;
				}
			}
			return false;
		}

		struct AxisSetupContext
		{
			IDirectInputDevice8W* device;
			AxisRange* axes;
		};

		BOOL CALLBACK EnumAxesCallback(const DIDEVICEOBJECTINSTANCEW* obj, void* ctx)
		{
			AxisSetupContext* setup = static_cast<AxisSetupContext*>(ctx);

			DIPROPRANGE range = {};
			range.diph.dwSize = sizeof(range);
			range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
			range.diph.dwHow = DIPH_BYID;
			range.diph.dwObj = obj->dwType;
			range.lMin = -AXIS_RANGE;
			range.lMax = AXIS_RANGE;
			setup->device->SetProperty(DIPROP_RANGE, &range.diph);

			// dwOfs is where DirectInput actually puts this axis in DIJOYSTATE2, which is the
			// only thing the reader can trust: the slot an axis lands in does not have to agree
			// with the name the device reports for it (and on some encoders it does not).
			const DWORD slot = obj->dwOfs / sizeof(LONG);
			if (slot >= DI_AXIS_COUNT)
			{
				return DIENUM_CONTINUE;
			}
			DIPROPRANGE actual = {};
			actual.diph.dwSize = sizeof(actual);
			actual.diph.dwHeaderSize = sizeof(DIPROPHEADER);
			actual.diph.dwHow = DIPH_BYID;
			actual.diph.dwObj = obj->dwType;
			if (SUCCEEDED(setup->device->GetProperty(DIPROP_RANGE, &actual.diph))
				&& actual.lMax > actual.lMin)
			{
				setup->axes[slot] = { actual.lMin, actual.lMax };
			}
			return DIENUM_CONTINUE;
		}

		BOOL CALLBACK EnumDevicesCallback(const DIDEVICEINSTANCEW* instance, void* /*ctx*/)
		{
			const std::string id = GuidToId(instance->guidInstance);
			if (id.empty())
			{
				return DIENUM_CONTINUE;
			}

			// Already open: just mark it as still present and move on.
			if (auto it = s_devices.find(id); it != s_devices.end())
			{
				it->second.seen = true;
				return DIENUM_CONTINUE;
			}

			IDirectInputDevice8W* raw = nullptr;
			if (FAILED(s_di->CreateDevice(instance->guidInstance, &raw, nullptr)) || raw == nullptr)
			{
				return DIENUM_CONTINUE;
			}
			DevicePtr device(raw);

			if (IsXInputDevice(device.get()))
			{
				return DIENUM_CONTINUE;
			}
			if (FAILED(device->SetDataFormat(&c_dfDIJoystick2)))
			{
				return DIENUM_CONTINUE;
			}
			// BACKGROUND so the pad keeps working when YAMP is not the foreground window (the
			// settings UI and the game both read it), NONEXCLUSIVE so nothing else loses the pad.
			if (FAILED(device->SetCooperativeLevel(s_hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)))
			{
				return DIENUM_CONTINUE;
			}
			OpenDevice open;
			AxisSetupContext setup{ device.get(), open.axes };
			device->EnumObjects(&EnumAxesCallback, &setup, DIDFT_AXIS);
			device->Acquire();   // may fail here and succeed on the first poll; not fatal

			open.name = WcharToUTF8(instance->tszProductName);
			if (open.name.empty())
			{
				open.name = "DirectInput Controller";
			}
			open.seen = true;
			open.device = std::move(device);
			DebugLog("[input] DirectInput pad: %s (%s)\n", open.name.c_str(), id.c_str());
			s_devices.emplace(id, std::move(open));
			return DIENUM_CONTINUE;
		}
	}

	void SetWindow(void* hwnd)
	{
		HWND wnd = static_cast<HWND>(hwnd);
		if (wnd == s_hwnd)
		{
			return;
		}
		s_hwnd = wnd;
		// The cooperative level is bound to the window, so everything opened against the old
		// one has to be reopened. Dropping them is enough - the next rescan reopens.
		s_devices.clear();
	}

	void Enumerate(std::vector<DeviceInfo>& out, bool rescan)
	{
		if (rescan && EnsureDirectInput())
		{
			for (auto& [id, dev] : s_devices)
			{
				dev.seen = false;
			}
			s_di->EnumDevices(DI8DEVCLASS_GAMECTRL, &EnumDevicesCallback, nullptr,
				DIEDFL_ATTACHEDONLY);
			for (auto it = s_devices.begin(); it != s_devices.end();)
			{
				it = it->second.seen ? std::next(it) : s_devices.erase(it);
			}
		}

		for (const auto& [id, dev] : s_devices)
		{
			out.push_back({ id, dev.name });
		}
	}

	bool Poll(const std::string& id, PadState& state)
	{
		const auto it = s_devices.find(id);
		if (it == s_devices.end())
		{
			return false;
		}
		IDirectInputDevice8W* device = it->second.device.get();

		HRESULT hr = device->Poll();
		if (FAILED(hr))
		{
			// Losing acquisition is routine (another app took focus, the machine slept). Retry
			// once here; a device that is genuinely gone fails GetDeviceState below instead.
			hr = device->Acquire();
			while (hr == DIERR_INPUTLOST)
			{
				hr = device->Acquire();
			}
			if (FAILED(hr))
			{
				return false;
			}
			device->Poll();
		}

		DIJOYSTATE2 js = {};
		if (FAILED(device->GetDeviceState(sizeof(js), &js)))
		{
			return false;
		}

		state = PadState{};
		state.connected = true;

		for (uint32_t i = 0; i < DI_BUTTON_COUNT; i++)
		{
			if (js.rgbButtons[i] & 0x80)
			{
				state.buttons |= 1ull << (Pad_Btn1 + i);
			}
		}

		// POV hat. Centered is reported as -1 or with the low word set to 0xFFFF depending on the
		// driver; anything else is an angle in hundredths of a degree, clockwise from up. Treat
		// it as the eight-way switch it physically is, so diagonals set both directions.
		const DWORD pov = js.rgdwPOV[0];
		if (LOWORD(pov) != 0xFFFF)
		{
			const int angle = static_cast<int>(pov % 36000);
			if (angle > 27000 || angle < 9000)  state.buttons |= 1ull << Pad_HatUp;
			if (angle > 0 && angle < 18000)     state.buttons |= 1ull << Pad_HatRight;
			if (angle > 9000 && angle < 27000)  state.buttons |= 1ull << Pad_HatDown;
			if (angle > 18000)                  state.buttons |= 1ull << Pad_HatLeft;
		}

		// Every axis normalised against ITS OWN range, so a device that kept a 0..65535 scale
		// reads centred at rest instead of jammed at one extreme.
		const LONG* const raw[DI_AXIS_COUNT] = { &js.lX, &js.lY, &js.lZ, &js.lRx, &js.lRy, &js.lRz };
		float normalised[DI_AXIS_COUNT] = {};
		for (uint32_t i = 0; i < DI_AXIS_COUNT; i++)
		{
			const AxisRange& range = it->second.axes[i];
			const float centre = (static_cast<float>(range.min) + static_cast<float>(range.max)) * 0.5f;
			const float half = (static_cast<float>(range.max) - static_cast<float>(range.min)) * 0.5f;
			float f = half > 0.0f ? (static_cast<float>(*raw[i]) - centre) / half : 0.0f;
			normalised[i] = f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);

			// Each axis also reads as two digital directions, which is what makes a hat-less
			// panel bindable at all. The threshold is deliberately well past the analog
			// deadzone so half-resting an analog stick cannot capture a binding by accident.
			if (normalised[i] <= -AXIS_DIGITAL_THRESHOLD)
			{
				state.buttons |= 1ull << (Pad_Axis1Minus + i * 2);
			}
			else if (normalised[i] >= AXIS_DIGITAL_THRESHOLD)
			{
				state.buttons |= 1ull << (Pad_Axis1Plus + i * 2);
			}
		}

		// The analog lever. Slots 0 and 1 by convention; DirectInput y+ is down, which is
		// already the sl convention.
		float x = normalised[0];
		float y = normalised[1];
		if (x * x + y * y < DEADZONE * DEADZONE)
		{
			x = y = 0.0f;
		}
		// Encoders that report their stick ONLY as a hat leave the axes centered. Fall back to
		// the hat so a plain arcade stick steers without the player binding anything first,
		// exactly like the analog stick does on a pad.
		if (x == 0.0f && y == 0.0f)
		{
			if (state.buttons & (1ull << Pad_HatLeft))  x = -1.0f;
			if (state.buttons & (1ull << Pad_HatRight)) x = 1.0f;
			if (state.buttons & (1ull << Pad_HatUp))    y = -1.0f;
			if (state.buttons & (1ull << Pad_HatDown))  y = 1.0f;
		}
		state.x = x;
		state.y = y;
		return true;
	}

	void Shutdown()
	{
		for (auto& [id, dev] : s_devices)
		{
			if (dev.device)
			{
				dev.device->Unacquire();
			}
		}
		s_devices.clear();
		s_di.reset();
	}
}
