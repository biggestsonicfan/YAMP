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
		// The cabinet's service-panel switches, wired to the emulated Model 2 I/O board's system
		// port rather than to a player's buttons (m2ftg boards only - see
		// m2ftg::InstallSystemSwitches). TEST opens the board's own service menu; SERVICE is the
		// credit / navigate button next to it. Per-player only because bindings are stored that
		// way - either player's binding closes the one cabinet switch.
		Action_Test,
		Action_Service,
		Action_Count
	};
	inline constexpr uint32_t WIZARD_ACTION_COUNT = Action_Coin + 1;

	// DIJOYSTATE2 reports up to 128 buttons; 24 is well past any real panel and keeps the whole
	// set inside PadState::buttons. Six axes = the fixed X/Y/Z/Rx/Ry/Rz slots.
	inline constexpr uint32_t DI_BUTTON_COUNT = 24;
	inline constexpr uint32_t DI_AXIS_COUNT = 6;

	// Bindable controller inputs: binding values (0 = unbound) and bit indices in
	// PadState::buttons. The left stick is not bindable - it always steers, like the lever.
	//
	// Two disjoint ranges, one per kind of controller. An XInput pad has a known button
	// contract and keeps the real names; a DirectInput pad (arcade encoders, sticks, adapters)
	// does not, so its inputs are simply numbered. A binding therefore only fires on the kind
	// of pad it was made on, rather than "A" and "Button 1" silently aliasing.
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
		// DirectInput: button n (1-based) = Pad_Btn1 + n - 1, then the POV hat, then each axis as
		// a pair of digital directions (axis n = Pad_Axis1Minus + (n - 1) * 2, +1 for positive).
		// Only the first of each run is named; PadButtonName formats the rest.
		//
		// Axes must bind: plenty of panels have no hat at all, and a stick reported purely as
		// axes would otherwise have no bindable directions. They are numbered rather than named
		// X/Y/Z because DirectInput does not guarantee an axis lands in the DIJOYSTATE2 slot
		// matching its reported name - so a semantic label here would be wrong on some devices.
		Pad_Btn1,
		Pad_HatUp = Pad_Btn1 + DI_BUTTON_COUNT,
		Pad_HatDown,
		Pad_HatLeft,
		Pad_HatRight,
		Pad_Axis1Minus,
		Pad_Axis1Plus,
		Pad_Count = Pad_Axis1Minus + DI_AXIS_COUNT * 2,
	};
	static_assert(Pad_Count <= 64, "PadState::buttons is a 64-bit mask");

	// [player][action] = virtual-key code (0 = unbound) / PadButton (Pad_None = unbound)
	using KeyBinds = std::array<std::array<uint32_t, Action_Count>, 2>;
	using PadBinds = std::array<std::array<uint32_t, Action_Count>, 2>;

	// Keyboard defaults follow MAME's arcade layout: the arrow keys for the stick, Z/X/C for
	// Punch / Kick / Guard, '1' to start and '5' to insert a coin. The combo keys keep the old
	// fixed assigns (I=P+G, O=P+K, U=K+G, M=P+K+G) and Back stays on Tab. Coin and the two
	// service-panel switches ('5', F2, F3) are one cabinet fixture, so only Player 1 gets them.
	// Player 2 has no keyboard defaults - run the wizard to set them up.
	inline constexpr KeyBinds DEFAULT_KEY_BINDS = { {
		//  Up                  Down                Left                Right               Punch Kick Guard Start Coin  P+G  P+K  K+G  P+K+G Back               Test              Service
		{ { 0x26 /* VK_UP */,   0x28 /* VK_DOWN */, 0x25 /* VK_LEFT */, 0x27 /* VK_RIGHT */,
		                                                                                    'Z',  'X', 'C',  '1',  '5',  'I', 'O', 'U', 'M',  0x09 /* VK_TAB */, 0x71 /* VK_F2 */, 0x72 /* VK_F3 */ } },
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

	// execute_info.assign values (m2ftg assign_t), indexed by the MODULE's slot, which is not the
	// same thing as our button. Pad.cpp routes each action to the matching sl button, so this is
	// the single source of truth for what each button bit means to the module. Net effect for the
	// player: A=P, B=K, Y=G, X=P+G, LT=P+K+G, LB=P+K, RT=K+G, RB=none.
	//
	// Two module facts this table depends on, both from the StF DLL:
	//  - assign_t -> M2 button code (lookup 0x180177B90, consumed by FUN_180003AC0): 1=none,
	//    2=p(0x07), 3=k(0x08), 4=g(0x09), 5=pg(0x6F), 6=pkg(0x72), 7=pk(0x71), 8=kg(0x70).
	//  - the slot table (0x180126770) keys each slot by a button mask in the MODULE's bit order,
	//    and the engine's pad conversion (FUN_180062470, reading execute_info.pad[p] +0x00)
	//    permutes the four face bits on the way in: our A(bit0)->module bit2, B(bit1)->bit1,
	//    X(bit2)->bit3, Y(bit3)->bit0. So the slots run Y, B, X, A, LT, LB, RT, RB in OUR terms;
	//    the shoulders pass through unpermuted. Reading the mask order as our own order is what
	//    used to rotate Punch / Barrier / Punch+Barrier between each other.
	//
	// Identical mask order in FV and both VF2 builds (DLL 0x123740 / 0x106860 / 0x10EB50). Motor
	// Raid and Virtual On ship no table of this shape, so their slot meanings are unconfirmed.
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

	// One attached controller, XInput or DirectInput. Identity is `id` ("xinput:<slot>" or
	// "dinput:<instance guid>"), never a list position - the list shifts as devices come and go,
	// and an index would hand a player whoever moved into that slot. settings.ini stores the id.
	struct PadDevice
	{
		std::string id;
		std::string name;      // "XInput Controller 1", or the DirectInput product name
		bool connected = false;
	};

	// DirectInput needs a top-level window for its cooperative level; until RenderWindow hands
	// one over, only XInput pads are visible.
	void SetWindow(void* hwnd);

	// Rebuilds the device list, rescanning DirectInput. ~100 ms - EnumDevices walks the whole HID
	// stack. Never call it per frame or off a timer; at 60 fps that is six dropped frames each
	// time. It runs at startup and from the Controls page's Rescan button. Deliberately not on
	// WM_DEVICECHANGE, which fires for any device node on the system (a headset, a phone on a
	// charger) and would stutter a match over something unrelated.
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
