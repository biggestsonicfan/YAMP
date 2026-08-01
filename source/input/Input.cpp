#include "Input.h"
#include "DirectInputPad.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>

#include "../YAMPGeneral.h"

#include <atomic>

namespace Input
{
	namespace
	{
		decltype(XInputGetState)* GetXInputGetState()
		{
			// xinput1_4 ships with Windows 8 and later; 1_3 only exists if the legacy DirectX
			// redist was installed, so it is the fallback rather than the first choice.
			HMODULE xinputLib = LoadLibraryW(L"xinput1_4");
			if (xinputLib == nullptr)
			{
				xinputLib = LoadLibraryW(L"xinput1_3");
			}
			if (xinputLib == nullptr)
			{
				xinputLib = LoadLibraryW(L"xinput9_1_0");
			}
			if (xinputLib == nullptr)
			{
				return nullptr;
			}
			return reinterpret_cast<decltype(XInputGetState)*>(GetProcAddress(xinputLib, "XInputGetState"));
		}

		// The attached controllers and their state, index-aligned. Rebuilt by RefreshDevices;
		// everything outside this file addresses a pad by PadDevice::id, never by index.
		std::vector<PadDevice> s_devices;
		std::vector<PadState> s_states;

		// XInputGetState is expensive for absent controllers; once a slot reports
		// disconnected, only re-check it every so often instead of every frame.
		uint32_t s_reconnectCooldown[XUSER_MAX_COUNT];
		constexpr uint32_t RECONNECT_COOLDOWN_FRAMES = 60;
		PadState s_xinputPads[XUSER_MAX_COUNT];

		// Set from the Controls page's Rescan button (and on a device that stops answering);
		// atomic only because it is trivially cheap to make it so.
		std::atomic<bool> s_rescanRequested{ true };  // true = enumerate on the first poll
		uint32_t s_xinputMask = 0;                    // which XInput slots were connected last poll

		std::string XInputId(int slot)
		{
			return "xinput:" + std::to_string(slot);
		}

		void PollXInput()
		{
			static decltype(XInputGetState)* const getStateFunc = GetXInputGetState();

			for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
			{
				PadState& pad = s_xinputPads[i];
				if (!pad.connected && s_reconnectCooldown[i] > 0)
				{
					s_reconnectCooldown[i]--;
					continue;
				}

				XINPUT_STATE state;
				if (getStateFunc == nullptr || getStateFunc(i, &state) != ERROR_SUCCESS)
				{
					pad = PadState{};
					s_reconnectCooldown[i] = RECONNECT_COOLDOWN_FRAMES;
					continue;
				}

				pad.connected = true;
				pad.buttons = 0;
				auto mapButton = [&](WORD xiMask, PadButton button) {
					if (state.Gamepad.wButtons & xiMask)
					{
						pad.buttons |= 1ull << button;
					}
					};
				mapButton(XINPUT_GAMEPAD_A, Pad_A);
				mapButton(XINPUT_GAMEPAD_B, Pad_B);
				mapButton(XINPUT_GAMEPAD_X, Pad_X);
				mapButton(XINPUT_GAMEPAD_Y, Pad_Y);
				mapButton(XINPUT_GAMEPAD_LEFT_SHOULDER, Pad_LB);
				mapButton(XINPUT_GAMEPAD_RIGHT_SHOULDER, Pad_RB);
				mapButton(XINPUT_GAMEPAD_START, Pad_Start);
				mapButton(XINPUT_GAMEPAD_BACK, Pad_Back);
				mapButton(XINPUT_GAMEPAD_LEFT_THUMB, Pad_LThumb);
				mapButton(XINPUT_GAMEPAD_RIGHT_THUMB, Pad_RThumb);
				mapButton(XINPUT_GAMEPAD_DPAD_UP, Pad_DPadUp);
				mapButton(XINPUT_GAMEPAD_DPAD_DOWN, Pad_DPadDown);
				mapButton(XINPUT_GAMEPAD_DPAD_LEFT, Pad_DPadLeft);
				mapButton(XINPUT_GAMEPAD_DPAD_RIGHT, Pad_DPadRight);
				if (state.Gamepad.bLeftTrigger > 48)
				{
					pad.buttons |= 1ull << Pad_LT;
				}
				if (state.Gamepad.bRightTrigger > 48)
				{
					pad.buttons |= 1ull << Pad_RT;
				}

				// TODO: Proper deadzone (kept from the old sl.cpp XInput reader)
				constexpr float DEADZONE = 0.25f;
				float x = state.Gamepad.sThumbLX / 32767.0f;
				float y = -state.Gamepad.sThumbLY / 32767.0f;
				if (x * x + y * y < DEADZONE * DEADZONE)
				{
					pad.x = pad.y = 0.0f;
				}
				else
				{
					pad.x = x;
					pad.y = y;
				}
			}
		}
	}

	const char* ActionName(uint32_t action)
	{
		static constexpr const char* NAMES[Action_Count] = {
			"Up", "Down", "Left", "Right", "Punch", "Kick", "Guard", "Start", "Coin",
			"P + G", "P + K", "K + G", "P + K + G", "Back",
			"Test (Service Menu)", "Service",
		};
		return action < Action_Count ? NAMES[action] : "?";
	}

	const char* ActionIniName(uint32_t action)
	{
		static constexpr const char* NAMES[Action_Count] = {
			"Up", "Down", "Left", "Right", "Punch", "Kick", "Guard", "Start", "Coin",
			"PG", "PK", "KG", "PKG", "Back",
			"Test", "Service",
		};
		return action < Action_Count ? NAMES[action] : "?";
	}

	std::string PadButtonName(uint32_t button)
	{
		static constexpr const char* XINPUT_NAMES[] = {
			"-", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Back",
			"L Stick Click", "R Stick Click", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
		};
		if (button < std::size(XINPUT_NAMES))
		{
			return XINPUT_NAMES[button];
		}
		if (button >= Pad_Btn1 && button < Pad_Btn1 + DI_BUTTON_COUNT)
		{
			return "Button " + std::to_string(button - Pad_Btn1 + 1);
		}
		if (button >= Pad_Axis1Minus && button < Pad_Count)
		{
			const uint32_t offset = button - Pad_Axis1Minus;
			return "Axis " + std::to_string(offset / 2 + 1) + ((offset & 1) ? " +" : " -");
		}
		switch (button)
		{
		case Pad_HatUp:    return "Hat Up";
		case Pad_HatDown:  return "Hat Down";
		case Pad_HatLeft:  return "Hat Left";
		case Pad_HatRight: return "Hat Right";
		default:           return "?";
		}
	}

	std::string KeyName(uint32_t vk)
	{
		if (vk == 0 || vk >= 256)
		{
			return "-";
		}

		UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
		switch (vk)
		{
		// Keys whose scan code needs the extended bit, or GetKeyNameText returns the
		// numpad variant's name (e.g. VK_LEFT -> "Num 4").
		case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
		case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
		case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
			scanCode |= 0x100;
			break;
		default:
			break;
		}

		wchar_t name[64];
		if (scanCode != 0 && GetKeyNameTextW(static_cast<LONG>(scanCode << 16), name, static_cast<int>(std::size(name))) > 0)
		{
			return WcharToUTF8(name);
		}
		return "Key " + std::to_string(vk);
	}

	void SetWindow(void* hwnd)
	{
		DI::SetWindow(hwnd);
	}

	void RefreshDevices()
	{
		// XInput first, so the slot numbering players already know stays at the top of the
		// picker, then whatever DirectInput found. Only attached devices are listed; a pad that
		// is configured but currently unplugged is the caller's business (the Controls page
		// still shows it, so the binding is visibly intact rather than silently gone).
		std::vector<PadDevice> devices;
		std::vector<PadState> states;

		for (int slot = 0; slot < static_cast<int>(XUSER_MAX_COUNT); slot++)
		{
			if (!s_xinputPads[slot].connected)
			{
				continue;
			}
			PadDevice dev;
			dev.id = XInputId(slot);
			dev.name = "XInput Controller " + std::to_string(slot + 1);
			dev.connected = true;
			devices.push_back(std::move(dev));
			states.push_back(s_xinputPads[slot]);
		}

		std::vector<DI::DeviceInfo> diDevices;
		DI::Enumerate(diDevices, true);
		for (const DI::DeviceInfo& info : diDevices)
		{
			PadDevice dev;
			dev.id = info.id;
			dev.name = info.name;
			dev.connected = true;
			devices.push_back(std::move(dev));
			// Carry the last poll over so a rescan does not blank the pad for a frame, which
			// would read as a spurious button release mid-input.
			const int old = FindDevice(info.id);
			states.push_back(old >= 0 ? s_states[old] : PadState{});
		}

		s_devices = std::move(devices);
		s_states = std::move(states);
	}

	const std::vector<PadDevice>& Devices()
	{
		return s_devices;
	}

	int FindDevice(const std::string& id)
	{
		if (id.empty())
		{
			return -1;
		}
		for (size_t i = 0; i < s_devices.size(); i++)
		{
			if (s_devices[i].id == id)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	void RequestDeviceRescan()
	{
		s_rescanRequested.store(true, std::memory_order_relaxed);
	}

	void PollPads()
	{
		PollXInput();

		// The list is rebuilt only when it can actually have changed: the first poll, an XInput
		// pad appearing or vanishing (which XInput tells us for free, as part of the poll), or
		// an explicit request. Everything else just reads the devices already open, which is
		// two microseconds. Rebuilding per frame was the lag spike.
		uint32_t xinputMask = 0;
		for (uint32_t slot = 0; slot < XUSER_MAX_COUNT; slot++)
		{
			if (s_xinputPads[slot].connected)
			{
				xinputMask |= 1u << slot;
			}
		}
		if (s_rescanRequested.exchange(false, std::memory_order_relaxed) || xinputMask != s_xinputMask)
		{
			s_xinputMask = xinputMask;
			RefreshDevices();
		}

		for (size_t i = 0; i < s_devices.size(); i++)
		{
			const PadDevice& dev = s_devices[i];
			if (dev.id.compare(0, 7, "xinput:") == 0)
			{
				const int slot = std::atoi(dev.id.c_str() + 7);
				s_states[i] = s_xinputPads[slot];
				continue;
			}
			if (!DI::Poll(dev.id, s_states[i]))
			{
				// Stopped answering - unplugged, or lost for good. Drop its state now and let
				// the next poll rebuild the list, which is the one case where a rescan is
				// worth its cost without the player asking for it.
				s_states[i] = PadState{};
				RequestDeviceRescan();
			}
		}
	}

	const PadState& GetPadState(int deviceIndex)
	{
		static const PadState DUMMY{};
		if (deviceIndex < 0 || deviceIndex >= static_cast<int>(s_states.size()))
		{
			return DUMMY;
		}
		return s_states[deviceIndex];
	}

	const PadState& GetPadState(const std::string& id)
	{
		return GetPadState(FindDevice(id));
	}

	void ShutdownPads()
	{
		s_devices.clear();
		s_states.clear();
		DI::Shutdown();
	}

	bool ActionDown(unsigned int player, uint32_t action)
	{
		if (player >= 2 || action >= Action_Count)
		{
			return false;
		}

		const YAMPSettings* settings = gGeneral.GetSettings();
		const uint32_t vk = settings->m_m2KeyBinds[player][action];
		if (vk != 0 && vk < 256 && gGeneral.GetPressedKeys()[vk])
		{
			return true;
		}

		const uint32_t button = settings->m_m2PadBinds[player][action];
		if (button != Pad_None && button < Pad_Count)
		{
			const PadState& pad = GetPadState(settings->m_m2PadId[player]);
			if (pad.buttons & (1ull << button))
			{
				return true;
			}
		}
		return false;
	}
}
