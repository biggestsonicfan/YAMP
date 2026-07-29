#include "M2Input.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>

#include "../YAMPGeneral.h"

namespace M2Input
{
	namespace
	{
		decltype(XInputGetState)* GetXInputGetState()
		{
			HMODULE xinputLib = LoadLibraryW(L"xinput1_3");
			if (xinputLib == nullptr)
			{
				xinputLib = LoadLibraryW(L"xinput1_4");
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

		PadState s_pads[XUSER_MAX_COUNT];
		// XInputGetState is expensive for absent controllers; once a slot reports
		// disconnected, only re-check it every so often instead of every frame.
		uint32_t s_reconnectCooldown[XUSER_MAX_COUNT];
		constexpr uint32_t RECONNECT_COOLDOWN_FRAMES = 60;
	}

	const char* ActionName(uint32_t action)
	{
		static constexpr const char* NAMES[Action_Count] = {
			"Up", "Down", "Left", "Right", "Punch", "Kick", "Guard", "Start", "Coin",
			"P + G", "P + K", "K + G", "P + K + G", "Back",
		};
		return action < Action_Count ? NAMES[action] : "?";
	}

	const char* ActionIniName(uint32_t action)
	{
		static constexpr const char* NAMES[Action_Count] = {
			"Up", "Down", "Left", "Right", "Punch", "Kick", "Guard", "Start", "Coin",
			"PG", "PK", "KG", "PKG", "Back",
		};
		return action < Action_Count ? NAMES[action] : "?";
	}

	const char* PadButtonName(uint32_t button)
	{
		static constexpr const char* NAMES[Pad_Count] = {
			"-", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Back",
			"L Stick Click", "R Stick Click", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
		};
		return button < Pad_Count ? NAMES[button] : "?";
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

	void PollPads()
	{
		static decltype(XInputGetState)* const getStateFunc = GetXInputGetState();

		for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
		{
			PadState& pad = s_pads[i];
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
					pad.buttons |= 1u << button;
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
				pad.buttons |= 1u << Pad_LT;
			}
			if (state.Gamepad.bRightTrigger > 48)
			{
				pad.buttons |= 1u << Pad_RT;
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

	const PadState& GetPadState(int xinputIndex)
	{
		static const PadState DUMMY{};
		if (xinputIndex < 0 || xinputIndex >= static_cast<int>(XUSER_MAX_COUNT))
		{
			return DUMMY;
		}
		return s_pads[xinputIndex];
	}

	bool ActionDown(unsigned int player, uint32_t action)
	{
		if (player >= 2 || action >= Action_Count)
		{
			return false;
		}

		const YAMPSettings* settings = gGeneral.GetSettings();
		const uint32_t vk = settings->m_stfKeyBinds[player][action];
		if (vk != 0 && vk < 256 && gGeneral.GetPressedKeys()[vk])
		{
			return true;
		}

		const uint32_t button = settings->m_stfPadBinds[player][action];
		if (button != Pad_None && button < Pad_Count)
		{
			const PadState& pad = GetPadState(settings->m_stfPadIndex[player]);
			if (pad.buttons & (1u << button))
			{
				return true;
			}
		}
		return false;
	}
}
