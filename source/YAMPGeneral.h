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
		VF5FS,
		StF,
		VF2,
		FV,
		MR,
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

	// Display names for the hosted arcade game and the parent game it was extracted from,
	// driven by the GameId set in wWinMain before the window/UI ever read them.
	const char* GetArcadeGameName() const;
	const char* GetParentGameName() const;
	// Short per-game abbreviation for log prefixes ("StF", "FV", "VF2", "VF5FS"), so logs from
	// code paths shared between games (LJ m2ftg host, HostCdevice) name the game actually running.
	const char* GetGameTag() const;

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
	GameId m_gameId = GameId::VF5FS;
	std::filesystem::path m_userDataPath;
	std::unique_ptr<YAMPSettings> m_settings;

	std::array<bool, 256> m_pressedKeyboardKeys;
};

extern YAMPGeneral gGeneral;