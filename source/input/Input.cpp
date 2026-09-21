#include "Input.h"
#include "DirectInputPad.h"
#include "BlissBox.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>

#include "../YAMPGeneral.h"
#include "../DebugLog.h"

#include <atomic>
#include <cstring>

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

		// Set from the Controls page's Rescan button, and on a device that stops answering.
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

				// The driving axes, and note steer does NOT reuse pad.x - see PadState. A separate
				// one-dimensional deadzone, rescaled so the usable travel still reaches full lock
				// rather than stopping short by the deadzone width.
				constexpr float STEER_DEADZONE = 0.12f;
				const float sign = (x < 0.0f) ? -1.0f : 1.0f;
				const float magnitude = (x < 0.0f) ? -x : x;
				pad.steer = (magnitude <= STEER_DEADZONE)
					? 0.0f
					: sign * ((magnitude - STEER_DEADZONE) / (1.0f - STEER_DEADZONE));

				// Triggers are already 0..255 with a flat rest, so they only need scaling. The
				// digital Pad_LT / Pad_RT bits above keep their own threshold and are unaffected.
				pad.throttle = state.Gamepad.bRightTrigger / 255.0f;
				pad.brake = state.Gamepad.bLeftTrigger / 255.0f;
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
		// XInput first so its familiar slot numbering stays at the top of the picker, then
		// whatever DirectInput found. Attached devices only - the Controls page is what shows a
		// configured-but-unplugged pad.
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
			// Carry the last poll over; a blank frame would read as a spurious button release.
			const int old = FindDevice(info.id);
			states.push_back(old >= 0 ? s_states[old] : PadState{});
		}

		s_devices = std::move(devices);
		s_states = std::move(states);

#if YAMP_DEBUG_LOGGING
		// Every rebuild, so a pad that vanishes has a line saying so. The hook report rides along:
		// "the pad is not listed" and "Steam's overlay has the pad APIs" belong next to each other.
		const Diagnostics diag = Diagnose();
		DebugLog("[input] %zu controller(s): %d XInput, %d DirectInput%s\n", s_devices.size(),
			diag.xinputPads, diag.directInputPads,
			diag.steamOverlayLoaded ? " - Steam's overlay IS loaded in this process" : "");
		for (const PadDevice& dev : s_devices)
		{
			DebugLog("[input]   %s  %s\n", dev.id.c_str(), dev.name.c_str());
		}
		for (const HookedFunction& fn : diag.functions)
		{
			if (fn.detoured)
			{
				DebugLog("[input]   %s!%s is detoured into %s\n", fn.module, fn.function, fn.target.c_str());
			}
		}
#endif
	}

	namespace
	{
		// The module that owns an address, by file name; empty when no module does (a trampoline
		// allocated beside the hooked DLL, for one).
		std::string ModuleAt(uintptr_t address)
		{
			HMODULE owner = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(address), &owner))
			{
				return {};
			}
			wchar_t path[MAX_PATH];
			if (GetModuleFileNameW(owner, path, MAX_PATH) == 0) return {};
			const wchar_t* name = wcsrchr(path, L'\\');
			return WcharToUTF8(name != nullptr ? name + 1 : path);
		}

		// Where the jump at `address` goes, or 0 when the code there is not one. Covers the
		// shapes x64 detour libraries write: jmp rel32, jmp rel8, jmp [rip+disp32], and
		// mov rax, imm64 / jmp rax. ReadProcessMemory on ourselves rather than a raw read, so a
		// page that is not readable is a failed call instead of a crash.
		uintptr_t JumpTarget(uintptr_t address)
		{
			uint8_t code[12] {};
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), code, sizeof(code), &got)
				|| got != sizeof(code))
			{
				return 0;
			}
			if (code[0] == 0xE9)
			{
				int32_t rel;
				memcpy(&rel, code + 1, sizeof(rel));
				return address + 5 + rel;
			}
			if (code[0] == 0xEB)
			{
				return address + 2 + static_cast<int8_t>(code[1]);
			}
			if (code[0] == 0xFF && code[1] == 0x25)
			{
				int32_t disp;
				memcpy(&disp, code + 2, sizeof(disp));
				uintptr_t target = 0;
				if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + 6 + disp),
					&target, sizeof(target), &got) || got != sizeof(target))
				{
					return 0;
				}
				return target;
			}
			if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0)
			{
				uintptr_t target;
				memcpy(&target, code + 2, sizeof(target));
				return target;
			}
			return 0;
		}
	}

	Diagnostics Diagnose()
	{
		Diagnostics diag;
		diag.steamOverlayLoaded = GetModuleHandleW(L"gameoverlayrenderer64.dll") != nullptr;
		diag.startedFromSteam = GetEnvironmentVariableW(L"SteamGameId", nullptr, 0) > 0;

		// Every entry point one of the three backends reads a pad through: XInput (this file),
		// DirectInput (DirectInputPad.cpp) and the Bliss-Box HID layer (BlissBox.cpp).
		static constexpr struct { const wchar_t* dll; const char* module; const char* function; } CHECKED[] = {
			{ L"xinput1_4.dll", "xinput1_4.dll", "XInputGetState" },
			{ L"xinput1_4.dll", "xinput1_4.dll", "XInputGetCapabilities" },
			{ L"xinput1_3.dll", "xinput1_3.dll", "XInputGetState" },
			{ L"dinput8.dll", "dinput8.dll", "DirectInput8Create" },
			{ L"hid.dll", "hid.dll", "HidD_GetAttributes" },
			{ L"setupapi.dll", "setupapi.dll", "SetupDiGetClassDevsW" },
			{ L"setupapi.dll", "setupapi.dll", "SetupDiEnumDeviceInterfaces" },
		};
		for (const auto& entry : CHECKED)
		{
			// GetModuleHandle, never LoadLibrary: a DLL YAMP has not loaded cannot be in its way.
			const HMODULE dll = GetModuleHandleW(entry.dll);
			if (dll == nullptr) continue;
			const auto address = reinterpret_cast<uintptr_t>(GetProcAddress(dll, entry.function));
			if (address == 0) continue;

			HookedFunction fn;
			fn.function = entry.function;
			fn.module = entry.module;
			// Follow the chain for a few hops: a detour usually lands on a trampoline next to the
			// hooked DLL first, and only the jump after that reaches the hooking module.
			uintptr_t target = JumpTarget(address);
			fn.detoured = target != 0;
			for (int hop = 0; target != 0 && hop < 4; hop++)
			{
				const std::string owner = ModuleAt(target);
				if (!owner.empty())
				{
					fn.target = owner;
					break;
				}
				target = JumpTarget(target);
			}
			if (fn.detoured && fn.target.empty())
			{
				fn.target = "unknown code";
			}
			// A jump that stays inside the function's own DLL is the DLL's code, not a hook.
			if (fn.detoured && _stricmp(fn.target.c_str(), entry.module) == 0)
			{
				fn.detoured = false;
				fn.target.clear();
			}
			diag.functions.push_back(std::move(fn));
		}

		for (const PadDevice& dev : s_devices)
		{
			if (dev.id.compare(0, 7, "xinput:") == 0) diag.xinputPads++;
			else diag.directInputPads++;
		}
		return diag;
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

	namespace
	{
		// The keyboard's driving axes, advanced once per PollPads. Steering is the only one that
		// needs history: throttle and brake on a key are honestly binary (a real pedal is not, but
		// nothing is gained by pretending a key is analogue), while steering that snaps to full
		// lock cannot hold a line.
		//
		// ~0.35 s to full lock, self-centring twice as fast. Frame-rate dependent by design: this
		// runs off the same poll as everything else, and threading a delta through would make the
		// wheel behave differently on a stalled frame than the buttons beside it do.
		constexpr float STEER_RATE = 1.0f / 21.0f;
		constexpr float STEER_RETURN = STEER_RATE * 2.0f;
		float s_keySteer = 0.0f;

		void PollKeyboardAxes()
		{
			const auto& keys = gGeneral.GetPressedKeys();
			const bool left = keys['A'];
			const bool right = keys['D'];

			if (left == right)
			{
				// Both or neither: wind back to centre and stop there rather than overshooting.
				if (s_keySteer > STEER_RETURN) s_keySteer -= STEER_RETURN;
				else if (s_keySteer < -STEER_RETURN) s_keySteer += STEER_RETURN;
				else s_keySteer = 0.0f;
			}
			else if (right)
			{
				s_keySteer = (s_keySteer + STEER_RATE > 1.0f) ? 1.0f : s_keySteer + STEER_RATE;
			}
			else
			{
				s_keySteer = (s_keySteer - STEER_RATE < -1.0f) ? -1.0f : s_keySteer - STEER_RATE;
			}
		}
	}

	void PollPads()
	{
		PollXInput();
		PollKeyboardAxes();

		// Rebuilt only when the list can actually have changed: first poll, an XInput pad
		// appearing or vanishing (free - the poll above already told us), or an explicit
		// request. Rebuilding per frame is what caused the lag spikes.
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
				// Stopped answering: the one case where a rescan is worth its cost unprompted.
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
		// The Bliss-Box backend owns a polling thread rather than being driven from PollPads, so
		// it has to be joined here or the process outlives it. Harmless when it never started.
		BlissBox::Shutdown();
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

	void DrivingAxes(unsigned int player, float& steer, float& throttle, float& brake)
	{
		steer = 0.0f;
		throttle = 0.0f;
		brake = 0.0f;
		if (player >= 2) return;

		const YAMPSettings* settings = gGeneral.GetSettings();
		const PadState& pad = GetPadState(settings->m_m2PadId[player]);
		if (pad.connected)
		{
			steer = pad.steer;
			throttle = pad.throttle;
			brake = pad.brake;
		}

		// KEYBOARD IS PLAYER 1's ONLY, matching how the coin and service keys already work: there
		// is one keyboard on the cabinet. Whichever source is pushed FURTHER wins per axis, so a
		// connected pad resting at zero does not veto the keyboard and vice versa.
		if (player == 0)
		{
			const auto& keys = gGeneral.GetPressedKeys();
			const float keySteerMagnitude = (s_keySteer < 0.0f) ? -s_keySteer : s_keySteer;
			const float padSteerMagnitude = (steer < 0.0f) ? -steer : steer;
			if (keySteerMagnitude > padSteerMagnitude) steer = s_keySteer;
			if (keys['W'] && throttle < 1.0f) throttle = 1.0f;
			if (keys['S'] && brake < 1.0f) brake = 1.0f;
		}

		if (steer < -1.0f) steer = -1.0f;
		if (steer > 1.0f) steer = 1.0f;
		if (throttle < 0.0f) throttle = 0.0f;
		if (throttle > 1.0f) throttle = 1.0f;
		if (brake < 0.0f) brake = 0.0f;
		if (brake > 1.0f) brake = 1.0f;
	}
}
