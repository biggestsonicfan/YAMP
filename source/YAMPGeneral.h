#pragma once

#include <array>
#include <filesystem>
#include <memory>

#include <cassert>

#include "wil/resource.h"

#include "YAMPSettings.h"

// TODO: Move
static std::wstring UTF8ToWchar(std::string_view text)
{
	std::wstring result;

	const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if ( count != 0 )
	{
		result.resize(count);
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
	}

	return result;
}

static std::string WcharToUTF8(std::wstring_view text)
{
	std::string result;

	const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if ( count != 0 )
	{
		result.resize(count);
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
	}

	return result;
}

// TODO: Move to YAMP namespace maybe?
class YAMPGeneral
{
public:
	// Which arcade module this process is hosting; drives the per-game UI panels.
	// Launcher = no game selected yet (the no-argument game-select menu).
	enum class GameId
	{
		VF5FS,      // Virtua Fighter 5: Final Showdown from Yakuza 6 (DX11)
		StF,
		VF2,
		FV,
		MR,
		VF5FS_LJ,   // the same game from Lost Judgment (DX12) — a different pxd generation, so a
		            // separate host (source/vf5fs/LJ) but the same settings and save data
		VF2_K2,     // Virtua Fighter 2 again, from Yakuza Kiwami 2 (GOG) — a THIRD pxd
		            // generation (sl 0xF3C0 / gs 0x202140), hosted by source/m2ftg/K2
		VON_K2,     // Virtual On, Kiwami 2's OTHER arcade module ("omg" = Operation Moon Gate).
		            // Built one second after the VF2 one, so it shares the whole K2 host.
		VF5FS_YLAD, // ...and again from Yakuza: Like a Dragon (= Yakuza 7, DX11), a third build:
		            // VF2's engine generation running the LJ module protocol (source/vf5fs/YLAD)
		FV2,        // Fighting Vipers 2, and...
		SRC2,       // ...Sega Racing Classic 2 — the two MODEL 3 games, hosted out of Like a
		            // Dragon Gaiden's pre3 module (source/pre3). Unlike every entry above, these
		            // two share ONE DLL: pre3 emulates six Model 3 games and picks between them
		            // with a config byte, so the GameId is the only thing that differs.
		MR_GAIDEN,  // Motor Raid again, from Like a Dragon Gaiden's own mr module (a 2023
		            // rebuild, same file name as Lost Judgment's). A separate id so the
		            // launcher can tell the two builds apart — without it, whichever install
		            // the search found first decided the "Motor Raid" verdict, and a Gaiden
		            // copy blocked the Lost Judgment one as an unknown build.
		Launcher,
	};

	const auto& GetDataPath() const { return m_userDataPath; }
	const auto* GetSettings() const { return m_settings.get(); }
	const auto& GetPressedKeys() const { return m_pressedKeyboardKeys; }
	const auto& GetDLLName() const { return m_dllName; }
	const auto GetDLLTimestamp() const { return m_dllTimestamp; }

	// Portable mode: user data (settings.ini + save files) lives next to YAMP.exe, shared by
	// every game. The ini is per-game sectioned ([StF]/[VF2]/[VF5FS]) and the save files carry
	// the game tag, so one folder holds them all. Resolved from the module path, NOT the CWD —
	// games booted from the launcher run with their game folder as CWD.
	void SetDataPath();
	
	void SetDLLName(std::string name) { m_dllName = std::move(name); }
	void SetDLLTimestamp(uint32_t timestamp) { m_dllTimestamp = timestamp; }

	GameId GetGameId() const { return m_gameId; }
	void SetGameId(GameId id) { m_gameId = id; }

	// Optional "-frames N" run limit: after N frames a host leaves its loop NORMALLY and shuts the
	// module down, instead of the run being ended by killing the process. Zero means unlimited (the
	// normal case). This exists because a killed process cannot be told apart from a crashed one:
	// terminating a debuggee mid-frame faults whichever worker thread was inside an allocator, which
	// looks exactly like a real bug in the module. A frame limit gives smoke tests a deterministic
	// length AND a real teardown path to exercise.
	uint32_t GetFrameLimit() const { return m_frameLimit; }
	void SetFrameLimit(uint32_t frames) { m_frameLimit = frames; }

	// Display names for the hosted arcade game and the parent game it was extracted from,
	// driven by the GameId set in wWinMain before the window/UI ever read them.
	const char* GetArcadeGameName() const;
	const char* GetParentGameName() const;
	// Short per-game abbreviation for log prefixes ("StF", "FV", "VF2", "VF5FS"), so logs from
	// code paths shared between games (LJ m2ftg host, HostCdevice) name the game actually running.
	const char* GetGameTag() const;

	// True only for the Model 2 arcade boards (StF/FV/MR/VF2). Those render a 4:3 240p-era image
	// on a CRT cabinet, so the aspect-ratio boxing and the CRT filter belong to them. Every VF5FS
	// build is a modern widescreen 3D game that must NOT get either treatment — the settings are
	// shared (one [StF] section), so the presentation path has to gate on the game, not the flag.
	bool IsModel2ArcadeGame() const;

	void SetKeyPressed(uint32_t key, bool pressed)
	{
		assert(key < m_pressedKeyboardKeys.size());
		m_pressedKeyboardKeys[key] = pressed;
	}
	
	void LoadSettings();

	// For use in the UI
	auto GetSettingsUpdateToken()
	{
		// (settings_ptr, apply_token) pair
		// Calls SaveSettings when it goes out of scope
		return std::make_pair(m_settings.get(), wil::scope_exit([this] {
			m_settings->SaveSettings(GetDataPath());
		}));
	}

private:
	std::string m_dllName;
	uint32_t m_dllTimestamp = 0;
	uint32_t m_frameLimit = 0;
	GameId m_gameId = GameId::VF5FS;
	std::filesystem::path m_userDataPath;
	std::unique_ptr<YAMPSettings> m_settings;

	std::array<bool, 256> m_pressedKeyboardKeys;
};

extern YAMPGeneral gGeneral;