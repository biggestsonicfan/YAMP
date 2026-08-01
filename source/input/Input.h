#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Host-side input bindings, shared by EVERY game YAMP hosts — the four m2ftg boards
// (StF/FV/MR/VF2) and all three VF5FS builds. Each player maps physical inputs (keyboard keys
// and/or one XInput controller's buttons) onto the arcade panel's actions.
//
// Pad.cpp (csl_pad::set_state) turns active actions into csl_pad button bits, and the fixed
// slot->P/K/G table (MODULE_ASSIGN below, written into execute_info.assign by the m2ftg hosts)
// gives those bits their meaning inside the module — so remapping never touches the
// module-facing protocol. The VF5FS modules do their own remap of the same bits instead.
namespace Input
{
	enum Action : uint32_t
	{
		// The first WIZARD_ACTION_COUNT actions are the ones the "Program All Inputs"
		// wizard walks through, in this prompt order.
		Action_Up,
		Action_Down,
		Action_Left,
		Action_Right,
		Action_Punch,
		Action_Kick,
		Action_Guard,
		Action_Start,
		Action_Coin,
		// Extra bindable actions (arcade panel macros + host back/cancel), not in the wizard.
		Action_PG,
		Action_PK,
		Action_KG,
		Action_PKG,
		Action_Back,
		// The cabinet's SERVICE PANEL switches, wired to the emulated Model 2 I/O board's
		// system port rather than to a player's buttons (m2ftg boards only - see
		// m2ftg::InstallSystemSwitches). TEST opens the board's own service menu, which is
		// where the operator checks how the panel is wired up, input by input; SERVICE is the
		// credit / navigate button next to it. Per-player only because the bindings are stored
		// per player - either player's binding closes the one cabinet switch.
		Action_Test,
		Action_Service,
		Action_Count
	};
	inline constexpr uint32_t WIZARD_ACTION_COUNT = Action_Coin + 1;

	// How many numbered buttons a DirectInput pad can bind. DIJOYSTATE2 reports up to 128, but
	// nothing with a usable panel has more than this, and the whole set has to fit in the
	// PadState::buttons mask alongside the XInput names and the axes.
	inline constexpr uint32_t DI_BUTTON_COUNT = 24;
	// The six axes DIJOYSTATE2 has a fixed slot for (X, Y, Z, Rx, Ry, Rz). Each binds as two
	// digital directions, because that is what an arcade panel actually wants out of them.
	inline constexpr uint32_t DI_AXIS_COUNT = 6;

	// Bindable controller inputs. Used both as binding values (0 = unbound) and as bit indices
	// in PadState::buttons. The left stick is not bindable - it always drives movement,
	// like the cabinet lever.
	//
	// TWO DISJOINT RANGES, because YAMP reads two kinds of controller. An XInput pad has a known
	// button contract, so its inputs keep their real names. A DirectInput pad - any other HID
	// game controller Windows exposes: arcade encoders, fight sticks, adapters, third-party pads
	// - has no such contract; its buttons are simply numbered, and calling button 1 "A" would be
	// a guess. So those get their own range. The practical consequence is that a binding only
	// fires on the KIND of pad it was made on, which is the honest behaviour: "A" on an Xbox pad
	// and "Button 1" on an arcade stick are not the same input and should not silently alias.
	enum PadButton : uint32_t
	{
		Pad_None = 0,
		// XInput. Values 1..16 are frozen - settings.ini stores them by number.
		Pad_A,
		Pad_B,
		Pad_X,
		Pad_Y,
		Pad_LB,
		Pad_RB,
		Pad_LT,
		Pad_RT,
		Pad_Start,
		Pad_Back,
		Pad_LThumb,
		Pad_RThumb,
		Pad_DPadUp,
		Pad_DPadDown,
		Pad_DPadLeft,
		Pad_DPadRight,
		// DirectInput: numbered button n (1-based) is Pad_Btn1 + n - 1, then the POV hat's four
		// directions, then each axis as a pair of digital directions. Only the first of each run
		// is named; PadButtonName formats the rest.
		Pad_Btn1,
		Pad_HatUp = Pad_Btn1 + DI_BUTTON_COUNT,
		Pad_HatDown,
		Pad_HatLeft,
		Pad_HatRight,
		// AXES BIND AS DIRECTIONS, and they have to: plenty of panels have no hat at all. The
		// user's own encoder ("USB Gamepad", 2 axes / 9 buttons / 0 POVs) reports its stick
		// purely as axes, so with only buttons and a hat bindable its directions were invisible.
		// Axis n (1-based) is Pad_Axis1Minus + (n - 1) * 2, with +1 for the positive direction.
		// Deliberately numbered rather than named X/Y/Z: DirectInput assigns this device's axes
		// to the DIJOYSTATE2 slots in an order that does NOT match their reported names (its
		// "Y Axis" lands in the X slot), so any semantic label here would be a lie half the time.
		// The player pushes the stick and binds whatever lights up, which is always right.
		Pad_Axis1Minus,
		Pad_Axis1Plus,
		Pad_Count = Pad_Axis1Minus + DI_AXIS_COUNT * 2,
	};
	static_assert(Pad_Count <= 64, "PadState::buttons is a 64-bit mask");

	// [player][action] = virtual-key code (0 = unbound) / PadButton (Pad_None = unbound)
	using KeyBinds = std::array<std::array<uint32_t, Action_Count>, 2>;
	using PadBinds = std::array<std::array<uint32_t, Action_Count>, 2>;

	// Keyboard defaults keep the historical StF layout (WASD + K/L/J, F to start, Tab back;
	// combo keys match the old fixed assigns: I=P+G, O=P+K, U=K+G, M=P+K+G). '5' inserts a
	// coin, MAME-style, and F2 / F3 are the service panel's Test and Service switches - also the
	// keys MAME uses for them, so anyone who has been inside a Model 2 test menu already knows
	// them. The panel is one cabinet fixture, so only Player 1 gets it by default.
	// Player 2 has no keyboard defaults - run the wizard to set them up.
	inline constexpr KeyBinds DEFAULT_KEY_BINDS = { {
		//  Up   Down  Left  Right  Punch Kick  Guard Start Coin  P+G  P+K  K+G  P+K+G Back               Test              Service
		{ { 'W', 'S',  'A',  'D',   'K',  'L',  'J',  'F',  '5',  'I', 'O', 'U', 'M',  0x09 /* VK_TAB */, 0x71 /* VK_F2 */, 0x72 /* VK_F3 */ } },
		{ {  0,   0,    0,    0,     0,    0,    0,    0,    0,    0,   0,   0,   0,    0,                 0,                0 } },
	} };

	// Controller defaults follow the module's own slot template (A=P, B=K, Y=G, LT=P+G,
	// LB=P+K+G, RT=P+K, RB=K+G); X is free because the template only had a second Punch on it.
	// Test / Service stay unbound on the pad: they are operator switches, and a stray press
	// drops the whole board into a menu mid-match.
	inline constexpr PadBinds DEFAULT_PAD_BINDS = { {
		{ { Pad_DPadUp, Pad_DPadDown, Pad_DPadLeft, Pad_DPadRight,
		    Pad_A, Pad_B, Pad_Y, Pad_Start, Pad_None,
		    Pad_LT, Pad_RT, Pad_RB, Pad_LB, Pad_Back, Pad_None, Pad_None } },
		{ { Pad_DPadUp, Pad_DPadDown, Pad_DPadLeft, Pad_DPadRight,
		    Pad_A, Pad_B, Pad_Y, Pad_Start, Pad_None,
		    Pad_LT, Pad_RT, Pad_RB, Pad_LB, Pad_Back, Pad_None, Pad_None } },
	} };

	// execute_info.assign values (m2ftg assign_t), indexed by the module's SLOT, which is not
	// the same thing as our button. Fixed table: Pad.cpp routes each action to the matching sl
	// button, so this is the single source of truth for what each button bit means to the module.
	//
	// assign_t -> M2 button code, from the module's own lookup (StF DLL 0x180177B90, consumed by
	// FUN_180003AC0): 1=none, 2=p(0x07), 3=k(0x08), 4=g(0x09), 5=pg(0x6F), 6=pkg(0x72),
	// 7=pk(0x71), 8=kg(0x70).
	//
	// WHICH SLOT A BUTTON LANDS IN (corrected 2026-08-01 after a service-menu input test; the
	// old table had Punch/Barrier/Punch+Barrier rotated between them). The module's slot table
	// (StF DLL 0x180126770) keys each slot by a button MASK, in the order
	// 0x01, 0x02, 0x08, 0x04, 0x40, 0x10, 0x80, 0x20 — and those masks are in the MODULE's own
	// bit order, not ours. The engine's pad conversion (FUN_180062470, reading
	// execute_info.pad[p] +0x00) permutes the four face bits on the way in:
	//     our A (bit0) -> module bit2 (0x04)      our X (bit2) -> module bit3 (0x08)
	//     our B (bit1) -> module bit1 (0x02)      our Y (bit3) -> module bit0 (0x01)
	// so the slots run Y, B, X, A, LT, LB, RT, RB in OUR button terms. The shoulders pass
	// through unpermuted and happen to line up already. Verified identical in the FV and both
	// VF2 module builds (same mask order at DLL 0x123740 / 0x106860 / 0x10EB50); Motor Raid and
	// Virtual On ship no table of this shape, so their slot meanings are still unconfirmed.
	//
	// Net effect for the player, which is what the Controls page promises:
	// A=P, B=K, Y=G, X=P+G, LT=P+K+G, LB=P+K, RT=K+G, RB=none.
	inline constexpr uint8_t MODULE_ASSIGN[8] = {
		4, // slot 0 (mask 0x01) <- our BUTTON_Y  = Guard      -> g
		3, // slot 1 (mask 0x02) <- our BUTTON_B  = Kick       -> k
		5, // slot 2 (mask 0x08) <- our BUTTON_X  = P+G        -> pg
		2, // slot 3 (mask 0x04) <- our BUTTON_A  = Punch      -> p
		6, // slot 4 (mask 0x40) <- our BUTTON_LT = P+K+G      -> pkg
		7, // slot 5 (mask 0x10) <- our BUTTON_LB = P+K        -> pk
		8, // slot 6 (mask 0x80) <- our BUTTON_RT = K+G        -> kg
		1, // slot 7 (mask 0x20) <- our BUTTON_RB = unbound    -> none
	};

	// Display / persistence names, indexed by Action.
	const char* ActionName(uint32_t action);    // "Punch", "P + G", ...
	const char* ActionIniName(uint32_t action); // compact, for settings.ini keys
	std::string PadButtonName(uint32_t button); // "A", "D-Pad Up", "Button 7", "Hat Up", ...
	std::string KeyName(uint32_t vk);           // localized key name via GetKeyNameText

	// Cached per-frame controller state. PollPads once per frame (the game loops and the
	// settings UI both do; polling twice is harmless), then GetPadState is free.
	struct PadState
	{
		uint64_t buttons = 0; // bitmask of (1ull << PadButton)
		float x = 0.0f, y = 0.0f; // left stick, deadzone applied, y+ = down (sl convention)
		bool connected = false;
	};

	// One attached controller, XInput or DirectInput.
	//
	// IDENTITY IS `id`, NOT A LIST POSITION. The device list changes as things are plugged in
	// and out, and an index would quietly hand a player whoever moved into that slot - the
	// classic "my controls swapped after I unplugged the headset" bug. Ids are
	// "xinput:<slot>" or "dinput:<instance guid>" and are what settings.ini stores, so a pad
	// keeps its bindings across a replug and across other devices coming and going.
	struct PadDevice
	{
		std::string id;
		std::string name;      // "XInput Controller 1", or the DirectInput product name
		bool connected = false;
	};

	// DirectInput needs a top-level window to set its cooperative level; RenderWindow hands its
	// own over as soon as it has one. Until then only XInput pads are visible.
	void SetWindow(void* hwnd);

	// Rebuilds the device list, rescanning DirectInput. EXPENSIVE - measured at ~100 ms, because
	// IDirectInput8::EnumDevices walks the whole HID stack (and a virtual-pad driver in it can
	// dominate that). NOT something to call per frame, per second, or off a timer: at 60 fps
	// that is six dropped frames every time it runs, which is exactly what it felt like.
	//
	// It runs at startup and then only when the player asks (the Controls page has a Rescan
	// button). Deliberately NOT hooked to WM_DEVICECHANGE either: that fires for any device node
	// on the system - a headset, a phone on a charger - so hot-plug detection would mean
	// freezing a match for a tenth of a second because something unrelated was plugged in.
	void RefreshDevices();
	// Asks for a RefreshDevices at the next PollPads. Safe from any thread.
	void RequestDeviceRescan();
	const std::vector<PadDevice>& Devices();
	// Index into Devices() for a stored id, or -1 when that pad is not attached (or id is empty).
	int FindDevice(const std::string& id);

	void PollPads();
	const PadState& GetPadState(int deviceIndex);      // out of range returns a disconnected dummy
	const PadState& GetPadState(const std::string& id);

	void ShutdownPads();

	// True while any input bound to the action (on the player's controller per the current
	// settings, or the keyboard) is held.
	bool ActionDown(unsigned int player, uint32_t action);
}
