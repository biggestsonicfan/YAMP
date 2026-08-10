#pragma once

// Shared internals of the settings/debug UI, split out of the 3,500-line
// YAMPUserInterface.cpp (2026-08-09): the game-family predicates and dispatch shims, the
// RoomSetting policy, the SRC2 board strings, and the small ImGui helpers - everything the
// per-panel translation units (ui/, m2ftg/, pre3/, net/, input/) share. Header-inline
// because every panel TU uses it and none of it holds cross-TU state.

#include "../YAMPUserInterface.h"

#include "../m2ftg/DisplayModes.h"

#include "../YAMPGeneral.h"
#include "../GameVerify.h"
#include "../m2ftg/ELF/ElfRom.h"
#include "../m2ftg/Debug/DebugWindows.h"
#include "../m2ftg/Determinism.h"
#include "../m2ftg/Debug/HleHooks.h"
#include "../m2ftg/LJ/LJHost.h"
#include "../m2ftg/K2/K2Host.h"   // GetLinkedCabinet - the overlay for a game with no round
#include "../net/LinkedCabinet.h" // the shared linked-cabinet report all three games fill
#include "../m2ftg/LJ/MrLink.h"   // Motor Raid's linked cabinet, same questions
#include "../pre3/Debug/HleHooks.h"
#include "../pre3/ArcadeSettings.h"
#include "../pre3/CommBoard.h"
#include "../pre3/Determinism.h"

// Draw isolation, defined in pxd/LJ/HostCdevice.cpp next to the D3D12 draw hooks it drives. Free
// functions rather than a header, matching how the other host-facing entry points there are used.
unsigned int ModuleDrawsLastFrameNow();
void SetModuleDrawLimitNow(unsigned int limit);
unsigned int ModuleDrawLimitNow();
void SetModuleSkipDrawNow(unsigned int index);
unsigned int ModuleSkipDrawNow();

#include "../net/NetPlugin.h"

#include "../imgui/imgui.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cstdlib>

inline const ImVec4 WARNING_COLOUR { 1.000f, 1.000f, 0.000f, 1.000f };

// The three Lost Judgment m2ftg modules share one hosting path, one settings section and one
// input layer, so they also share the Game/Controls panels.
inline bool IsLJm2ftgGame()
{
	const auto id = gGeneral.GetGameId();
	return id == YAMPGeneral::GameId::StF
		|| id == YAMPGeneral::GameId::FV
		|| id == YAMPGeneral::GameId::MR
		|| id == YAMPGeneral::GameId::MR_GAIDEN;
}

// ...as do the Yakuza Kiwami 2 modules, which share the K2 host the same way. Kiwami 2 ships two
// (Virtua Fighter 2 and Virtua On), so this is deliberately a per-PARENT-GAME test rather than a
// per-game one — the second module needs nothing here beyond its GameId.
// BOTH Kiwami 2 arcade modules, not just VF2. Virtual On was left out when it was added, and
// because this predicate is what routes a game to the m2ftg settings panel, Virtual On fell all
// the way through to the VF5FS one: it was offered "Language", "Confirmation button" and "Arcade\n// Machine Mode" - three settings that belong to a different engine and that its host never reads,
// so none of them did anything - while the dip switches it DOES honour (region, difficulty, free
// play, versus, render resolution) had no UI at all. K2Host has always applied
// `params.config.country` from `m_m2Country`; nothing could set it.
inline bool IsKiwami2Game()
{
	const auto id = gGeneral.GetGameId();
	return id == YAMPGeneral::GameId::VF2_K2 || id == YAMPGeneral::GameId::VON_K2;
}

inline bool IsM2ftgGame()
{
	return IsLJm2ftgGame() || IsKiwami2Game()
		|| gGeneral.GetGameId() == YAMPGeneral::GameId::VF2;
}

// The two Model 3 games, both out of Like a Dragon Gaiden's pre3 module. They share the m2ftg
// dip switches (difficulty/region/free play/versus are the same config bytes) but nothing else:
// there is no display-mode table, no CRT filter and no HLE hook table here. Without this they
// fell through to the VF5FS branch of DrawGame and were offered Language / Confirmation button /
// Arcade Machine Mode, none of which mean anything to a Model 3 board.
inline bool IsPre3Game()
{
	const auto id = gGeneral.GetGameId();
	return id == YAMPGeneral::GameId::FV2 || id == YAMPGeneral::GameId::SRC2;
}

// Netplay hangs off the LJ m2ftg host loop, but holding two emulators in lockstep needs the
// determinism work as well: board reset, host-RNG seeding, the texture-budget pin, and the ROM
// frame counter the round anchors and desync-checks against. All of it is now reconstructed for
// Fighting Vipers too (see docs/fv-hle-hooks.md and the DwGame table in DebugWindows.cpp), so
// both games get the page.
//
// FV's RNG needed the one genuinely new piece: it draws from TWO host twisters, not one, and
// seeding only `rand` would leave the two peers agreeing on the fight and disagreeing on the
// stage. SeedHostRng seeds every stream in the game's descriptor.
//
// Derived rather than listed, so this cannot drift from what NetSession will actually agree to
// run. The ROM frame counter is the last per-game fact a new game needs (the rest of the
// determinism set is already in its DwGame row), so "has a measured counter" is exactly "can\n// sustain a session" - and a game that gets one starts offering the page automatically.
inline bool IsNetplayGame()
{
	// THREE ANSWERS, NOT TWO, and the third is a different question entirely. The first two ask
	// "can this game sustain a LOCKSTEP round", which is derived from having a measured ROM frame
	// counter (m2ftg) or a pinnable deterministic clock (pre3, FV2 only). A LINKED-CABINET game
	// needs none of that and would fail both tests forever - so Sega Racing Classic 2 showed no
	// Netplay page and no overlay at all while its link worked perfectly, which is the same gap
	// Virtual On had on the m2ftg side before `GetLinkedCabinet` existed.
	return m2ftg::NetplaySupported() || pre3::NetplaySupported()
		|| pre3::CommBoard::LinkedCabinetSupported()
		|| m2ftg::MrLink::LinkedCabinetSupported();
}

// The linked-cabinet report, whichever emulator is running. All three games fill the shared
// net::LinkedCabinetStatus (net/LinkedCabinet.h), so the overlay below asks once and does not
// care which board it is.
inline bool GetLinkedCabinetStatus(net::LinkedCabinetStatus& out)
{
	return pre3::CommBoard::GetLinkedCabinet(out)
		|| m2ftg::MrLink::GetLinkedCabinet(out)
		|| m2ftg::K2::GetLinkedCabinet(out);
}

// The two families answer "has the emulated board booted?" and "put it back to a clean state"
// through their own emulators, and the netplay UI needs both without caring which is running.
// Written as a pair of one-liners rather than a shared interface: two implementations do not
// justify a vtable, and the alternative - the UI reaching for m2ftg unconditionally - is what
// would silently do nothing on the Model 3 path.
inline bool NetplayBoardBooted()
{
	return IsPre3Game() ? pre3::IsBoardBooted() : m2ftg::IsBoardBooted();
}

// WHICH per-game setting a room publishes, if any. Not every game has one, and the lobby used to
// assume that "not VF2" meant "has DAMAGE" - so Fighting Vipers, Motor Raid and both Model 3 games
// were all shown a Damage column that means nothing to them. DAMAGE is Sonic the Fighters' alone
// (m2ftg::UpdateDamageAssignment no-ops for every other game), and the Model 3 boards publish
// something else entirely: which state a round starts from.
enum class RoomSetting { None, Damage, Vf2Version, Pre3Start, Src2Assign };

inline RoomSetting CurrentRoomSetting()
{
	switch (gGeneral.GetGameId())
	{
	case YAMPGeneral::GameId::StF:     return RoomSetting::Damage;
	case YAMPGeneral::GameId::VF2:
	case YAMPGeneral::GameId::VF2_K2:  return RoomSetting::Vf2Version;
	case YAMPGeneral::GameId::FV2:     return RoomSetting::Pre3Start;
	// NOT Pre3Start, though SRC2 is a pre3 board: a linked-cabinet session never starts a round,
	// so the round-start state is a control that decides nothing there - the same reasoning that
	// keeps the Start-match barrier off its lobby. What an SRC2 room publishes instead is its
	// GAME ASSIGNMENTS: the five rows the race is played under.
	case YAMPGeneral::GameId::SRC2:    return RoomSetting::Src2Assign;
	default:                           return RoomSetting::None;
	}
}

// The browser's words for the five SRC2 assignment fields - the board's own option lists, read
// out of its service-menu row table (see pre3::ArcadeSettings::ReadLiveAssignments). Indexed by
// the room's published value, which the decode already masked to each field's range.
inline const char* const SRC2_CABINET_NAMES[] = { "Deluxe", "Twin", "Special" };
inline const char* const SRC2_DIFFICULTY_NAMES[] = { "Easy", "Normal", "Hard", "Hardest" };
inline const char* const SRC2_MODE_NAMES[] =
	{ "Sprint", "Grand Prix", "100 miles", "200 miles", "300 miles", "400 miles", "500 miles" };
inline const char* const SRC2_MOTOR_NAMES[] = { "50%", "60%", "70%", "80%", "90%", "100%" };
inline const char* const SRC2_RANKING_NAMES[] = { "Normal", "Campaign", "Internet" };

// VS mode is an m2ftg config byte all three of those games read at boot. The Model 3 round-start
// reset restores a whole saved machine over the top of it, so publishing it there would claim an
// agreement that decides nothing.
inline bool RoomHasVsMode()
{
	return !IsPre3Game();
}

inline void NetplayResetBoard()
{
	if (IsPre3Game()) pre3::ResetBoard();
	else              m2ftg::ResetBoard();
}

// The Virtua Fighter 2 MODULE, in either of the two parent games that ship it. Separate from the
// test above because m2ftg_config_t's is_vf20 is VF2's own switch — it
// mean nothing to Virtua On, StF, FV or MR.
inline bool IsVF2Module()
{
	const auto id = gGeneral.GetGameId();
	return id == YAMPGeneral::GameId::VF2 || id == YAMPGeneral::GameId::VF2_K2;
}

// "Custom" ImGui wrappers
namespace ImGuiCustom
{
	inline bool ButtonToggleable(const char* label, const bool enabled, const ImVec2& size = { 0, 0 })
	{
		bool result = false;
		if (!enabled)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
		}

		result = ImGui::Button(label, size);

		if (!enabled)
		{
			ImGui::PopStyleVar();
		}
		return enabled ? result : false;
	}

}

#define STRINGIZE(s) STRINGIZE2(s)
#define STRINGIZE2(s) #s

#define WIDEN(x) WIDEN2(x)
#define WIDEN2(x) L ##x
