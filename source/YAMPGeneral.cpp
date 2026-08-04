#include "YAMPGeneral.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <filesystem>

YAMPGeneral gGeneral;

void YAMPGeneral::LoadSettings()
{
	// TODO: Make per-game settings somehow
	m_settings = std::make_unique<YAMPSettings>();
	m_settings->LoadSettings(GetDataPath());
}

const char* YAMPGeneral::GetArcadeGameName() const
{
	switch (m_gameId)
	{
	case GameId::StF: return "Sonic the Fighters";
	case GameId::FV: return "Fighting Vipers";
	case GameId::MR: return "Motor Raid";
	case GameId::VF2:
	case GameId::VF2_K2: return "Virtua Fighter 2";
	case GameId::VON_K2: return "Virtual On";
	case GameId::FV2: return "Fighting Vipers 2";
	case GameId::SRC2: return "Sega Rally Championship 2";
	case GameId::Launcher: return "Yakuza Arcade Machines Player";
	default: return "Virtua Fighter 5: Final Showdown";
	}
}

const char* YAMPGeneral::GetGameTag() const
{
	switch (m_gameId)
	{
	case GameId::StF: return "StF";
	case GameId::FV: return "FV";
	case GameId::MR: return "MR";
	case GameId::VF2: return "VF2";
	case GameId::VF2_K2: return "VF2-K2";
	case GameId::VON_K2: return "VON-K2";
	case GameId::VF5FS_LJ: return "VF5FS-LJ";
	case GameId::VF5FS_YLAD: return "VF5FS-YLAD";
	case GameId::FV2: return "FV2";
	case GameId::SRC2: return "SRC2";
	case GameId::Launcher: return "YAMP";
	default: return "VF5FS";
	}
}

bool YAMPGeneral::IsModel2ArcadeGame() const
{
	switch (m_gameId)
	{
	case GameId::StF:
	case GameId::FV:
	case GameId::MR:
	case GameId::VF2:
	case GameId::VF2_K2:
	case GameId::VON_K2:
	// The Model 3 games too. The name says Model 2 but the test is really "a low-resolution
	// arcade board", which is what the CRT filter and the 4:3 presentation are for - the
	// exclusion it exists to make is the VF5FS builds (modern widescreen 3D). FV2 renders at
	// 496x384 like the rest, so leaving it out only meant its CRT setting silently did nothing.
	case GameId::FV2:
	case GameId::SRC2: return true;
	default: return false;
	}
}

const char* YAMPGeneral::GetParentGameName() const
{
	switch (m_gameId)
	{
	// Sonic the Fighters is the one arcade game that ships in more than one title, and owning
	// either is enough — so it must not tell a Like a Dragon Gaiden owner that they need Lost
	// Judgment. Which build is actually loaded is reported separately, by the module verdict.
	case GameId::StF: return "Lost Judgment or Like a Dragon Gaiden";
	case GameId::FV:
	case GameId::MR:
	case GameId::VF5FS_LJ: return "Lost Judgment";
	case GameId::VF2:
	case GameId::VF5FS_YLAD: return "Yakuza: Like a Dragon";
	case GameId::VF2_K2:
	case GameId::VON_K2: return "Yakuza Kiwami 2";
	case GameId::FV2:
	case GameId::SRC2: return "Like a Dragon Gaiden";
	case GameId::Launcher: return "the supported Yakuza games";
	default: return "Yakuza 6: The Song of Life";
	}
}

void YAMPGeneral::SetDataPath()
{
	wchar_t exePath[MAX_PATH];
	if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
	{
		m_userDataPath = std::filesystem::path(exePath).parent_path();
	}
}
