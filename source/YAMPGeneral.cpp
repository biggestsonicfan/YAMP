#include "YAMPGeneral.h"
#include "GameRegistry.h"

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

// The four identity answers all read the one registry row now (GameRegistry.cpp) - they used
// to be four independent switches that had to agree with each other, the launcher's table and
// Main.cpp's argument parser by hand.
const char* YAMPGeneral::GetArcadeGameName() const
{
	return GameRegistry::Find(m_gameId)->arcadeName;
}

const char* YAMPGeneral::GetGameTag() const
{
	return GameRegistry::Find(m_gameId)->tag;
}

bool YAMPGeneral::IsModel2ArcadeGame() const
{
	// The name says Model 2 but the test is really "a low-resolution arcade board" - see
	// GameRegistry::Entry::lowResBoard.
	return GameRegistry::Find(m_gameId)->lowResBoard;
}

const char* YAMPGeneral::GetParentGameName() const
{
	return GameRegistry::Find(m_gameId)->parentName;
}

void YAMPGeneral::SetDataPath()
{
	wchar_t exePath[MAX_PATH];
	if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
	{
		m_userDataPath = std::filesystem::path(exePath).parent_path();
	}
}
