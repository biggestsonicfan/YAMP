#pragma once

#include <array>
#include <cstdint>
#include <string>

// Host-side input bindings for Sonic the Fighters. Each player maps physical inputs
// (keyboard keys and/or one XInput controller's buttons) onto the arcade panel's actions.
// LJ/sl.cpp turns active actions into csl_pad button bits, and the fixed slot->P/K/G table
// (MODULE_ASSIGN below, written into execute_info.assign by StF.cpp) gives those bits their
// meaning inside the module - so remapping never touches the module-facing protocol.
namespace M2Input
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
		Action_Count
	};
	inline constexpr uint32_t WIZARD_ACTION_COUNT = Action_Coin + 1;

	// Bindable XInput inputs. Used both as binding values (0 = unbound) and as bit indices
	// in PadState::buttons. The left stick is not bindable - it always drives movement,
	// like the cabinet lever.
	enum PadButton : uint32_t
	{
		Pad_None = 0,
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
		Pad_Count
	};

	// [player][action] = virtual-key code (0 = unbound) / PadButton (Pad_None = unbound)
	using KeyBinds = std::array<std::array<uint32_t, Action_Count>, 2>;
	using PadBinds = std::array<std::array<uint32_t, Action_Count>, 2>;

	// Keyboard defaults keep the historical StF layout (WASD + K/L/J, F to start, Tab back;
	// combo keys match the old fixed assigns: I=P+G, O=P+K, U=K+G, M=P+K+G). '5' inserts a
	// coin, MAME-style. Player 2 has no keyboard defaults - run the wizard to set them up.
	inline constexpr KeyBinds DEFAULT_KEY_BINDS = { {
		//  Up   Down  Left  Right  Punch Kick  Guard Start Coin  P+G  P+K  K+G  P+K+G Back
		{ { 'W', 'S',  'A',  'D',   'K',  'L',  'J',  'F',  '5',  'I', 'O', 'U', 'M',  0x09 /* VK_TAB */ } },
		{ {  0,   0,    0,    0,     0,    0,    0,    0,    0,    0,   0,   0,   0,    0 } },
	} };

	// Controller defaults follow the module's own slot template (A=P, B=K, Y=G, LT=P+G,
	// LB=P+K+G, RT=P+K, RB=K+G); X is free since it was only a duplicate Guard there.
	inline constexpr PadBinds DEFAULT_PAD_BINDS = { {
		{ { Pad_DPadUp, Pad_DPadDown, Pad_DPadLeft, Pad_DPadRight,
		    Pad_A, Pad_B, Pad_Y, Pad_Start, Pad_None,
		    Pad_LT, Pad_RT, Pad_RB, Pad_LB, Pad_Back } },
		{ { Pad_DPadUp, Pad_DPadDown, Pad_DPadLeft, Pad_DPadRight,
		    Pad_A, Pad_B, Pad_Y, Pad_Start, Pad_None,
		    Pad_LT, Pad_RT, Pad_RB, Pad_LB, Pad_Back } },
	} };

	// execute_info.assign values (m2ftg assign_t) in the module's slot order A, B, Y, X,
	// LT, LB, RT, RB. Fixed: sl.cpp routes each action to the matching sl button, so this
	// table is the single source of truth for what each button bit means to the module.
	// A=P, B=K, Y=G, X=P+G, LT=P+K+G, LB=P+K, RT=K+G, RB=none.
	inline constexpr uint8_t MODULE_ASSIGN[8] = { 2, 3, 4, 5, 6, 7, 8, 1 };

	// Display / persistence names, indexed by Action.
	const char* ActionName(uint32_t action);    // "Punch", "P + G", ...
	const char* ActionIniName(uint32_t action); // compact, for settings.ini keys
	const char* PadButtonName(uint32_t button); // "A", "D-Pad Up", ...
	std::string KeyName(uint32_t vk);           // localized key name via GetKeyNameText

	// Cached per-frame XInput state. PollPads once per frame (StF's GameLoop and the
	// settings UI both do; polling twice is harmless), then GetPadState is free.
	struct PadState
	{
		uint32_t buttons = 0; // bitmask of (1 << PadButton)
		float x = 0.0f, y = 0.0f; // left stick, deadzone applied, y+ = down (sl convention)
		bool connected = false;
	};
	void PollPads();
	const PadState& GetPadState(int xinputIndex); // -1 or out of range returns a dummy

	// True while any input bound to the action (on the player's controller per the current
	// settings, or the keyboard) is held.
	bool ActionDown(unsigned int player, uint32_t action);
}
