#include "YAMPSettings.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string_view>

static constexpr std::string_view INI_FILE_NAME = "settings.ini";
static constexpr uint32_t SETTINGS_VERSION = 1;

static float GetPrivateProfileFloatW(LPCWSTR lpAppName, LPCWSTR lpKeyName, float fDefault, LPCWSTR lpFileName)
{
	wchar_t buf[32];
	GetPrivateProfileStringW(lpAppName, lpKeyName, nullptr, buf, std::size(buf), lpFileName);
	float val = _wtof(buf);
	if (val != 0.0f)
	{
		return val;
	}
	return fDefault;
}

static void WritePrivateProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName, int nValue, LPCWSTR lpFileName)
{
	wchar_t buf[32];
	swprintf_s(buf, L"%d", nValue);
	WritePrivateProfileStringW(lpAppName, lpKeyName, buf, lpFileName);
}

static void WritePrivateProfileFloatW(LPCWSTR lpAppName, LPCWSTR lpKeyName, float fValue, LPCWSTR lpFileName)
{
	wchar_t buf[32];
	swprintf_s(buf, L"%g", fValue);
	WritePrivateProfileStringW(lpAppName, lpKeyName, buf, lpFileName);
}

void YAMPSettings::LoadSettings(const std::filesystem::path& dirPath)
{
	const std::filesystem::path iniPath = dirPath / std::filesystem::u8path(INI_FILE_NAME);

	{
		const wchar_t* SECTION_NAME = L"General";
		if (int version = GetPrivateProfileIntW(SECTION_NAME, L"Version", 0, iniPath.c_str()); version != SETTINGS_VERSION)
		{
			return;
		}

		m_buildLastShowedDisclaimer = GetPrivateProfileIntW(SECTION_NAME, L"Disclaimer", 0, iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"Graphics";
		int resX = GetPrivateProfileIntW(SECTION_NAME, L"ResolutionX", 0, iniPath.c_str());
		int resY = GetPrivateProfileIntW(SECTION_NAME, L"ResolutionY", 0, iniPath.c_str());
		if (resX != 0 && resY != 0)
		{
			m_resX = resX;
			m_resY = resY;
		}

		float refRate = GetPrivateProfileFloatW(SECTION_NAME, L"RefreshRate", 0.0f, iniPath.c_str());
		if (refRate != 0.0f)
		{
			m_refreshRate = refRate;
		}

		m_fullscreen = GetPrivateProfileIntW(SECTION_NAME, L"Fullscreen", 0, iniPath.c_str()) != 0;
		m_enableFpsCap = GetPrivateProfileIntW(SECTION_NAME, L"FPSCap", 1, iniPath.c_str()) != 0;
	}

	{
		const wchar_t* SECTION_NAME = L"Debug";
		m_dontApplyPatches = GetPrivateProfileIntW(SECTION_NAME, L"DoNotApplyPatches", 0, iniPath.c_str()) != 0;
		m_useD3DDebugLayer = GetPrivateProfileIntW(SECTION_NAME, L"UseDebugD3D", 0, iniPath.c_str()) != 0;
		m_stfShowDebugFeatures = GetPrivateProfileIntW(SECTION_NAME, L"ShowDLLDebugFeatures", 0, iniPath.c_str()) != 0;
		m_stfLooseRomFiles = GetPrivateProfileIntW(SECTION_NAME, L"LoadLooseRomFiles", 0, iniPath.c_str()) != 0;
		m_stfGameDebugFlag = GetPrivateProfileIntW(SECTION_NAME, L"SetGameDebugFlag", 0, iniPath.c_str()) != 0;
	}

	{
		const wchar_t* SECTION_NAME = L"VF5FS";
		m_arcadeMode = GetPrivateProfileIntW(SECTION_NAME, L"ArcadeMode", 0, iniPath.c_str()) != 0;
		m_circleConfirm = GetPrivateProfileIntW(SECTION_NAME, L"CircleConfirm", 0, iniPath.c_str()) != 0;
		m_language = GetPrivateProfileIntW(SECTION_NAME, L"Language", 1, iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"StF";
		m_stfAspect = GetPrivateProfileIntW(SECTION_NAME, L"AspectRatio", m_stfAspect, iniPath.c_str());
		m_stfCrtFilter = GetPrivateProfileIntW(SECTION_NAME, L"CRTFilter", m_stfCrtFilter, iniPath.c_str()) != 0;
		m_stfDifficulty = GetPrivateProfileIntW(SECTION_NAME, L"Difficulty", m_stfDifficulty, iniPath.c_str());
		m_stfCountry = GetPrivateProfileIntW(SECTION_NAME, L"Country", m_stfCountry, iniPath.c_str());
		m_stfFreeplay = GetPrivateProfileIntW(SECTION_NAME, L"FreePlay", m_stfFreeplay, iniPath.c_str()) != 0;
		m_stfVersusMode = GetPrivateProfileIntW(SECTION_NAME, L"VersusMode", m_stfVersusMode, iniPath.c_str()) != 0;
		for (int player = 0; player < 2; player++)
		{
			wchar_t key[48];
			swprintf_s(key, L"P%dController", player + 1);
			int padIndex = static_cast<int>(GetPrivateProfileIntW(SECTION_NAME, key, m_stfPadIndex[player], iniPath.c_str()));
			m_stfPadIndex[player] = (padIndex >= -1 && padIndex < 4) ? padIndex : -1;

			for (uint32_t action = 0; action < StFInput::Action_Count; action++)
			{
				swprintf_s(key, L"P%dKey%hs", player + 1, StFInput::ActionIniName(action));
				uint32_t vk = GetPrivateProfileIntW(SECTION_NAME, key, m_stfKeyBinds[player][action], iniPath.c_str());
				m_stfKeyBinds[player][action] = vk < 256 ? vk : 0;

				swprintf_s(key, L"P%dPad%hs", player + 1, StFInput::ActionIniName(action));
				uint32_t button = GetPrivateProfileIntW(SECTION_NAME, key, m_stfPadBinds[player][action], iniPath.c_str());
				m_stfPadBinds[player][action] = button < StFInput::Pad_Count ? button : StFInput::Pad_None;
			}
		}
	}
}

void YAMPSettings::SaveSettings(const std::filesystem::path& dirPath)
{
	const std::filesystem::path iniPath = dirPath / std::filesystem::u8path(INI_FILE_NAME);

	{
		const wchar_t* SECTION_NAME = L"General";
		WritePrivateProfileIntW(SECTION_NAME, L"Version", SETTINGS_VERSION, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"Disclaimer", m_buildLastShowedDisclaimer, iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"Graphics";
		WritePrivateProfileIntW(SECTION_NAME, L"ResolutionX", m_resX, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"ResolutionY", m_resY, iniPath.c_str());
	
		WritePrivateProfileFloatW(SECTION_NAME, L"RefreshRate", m_refreshRate, iniPath.c_str());

		WritePrivateProfileIntW(SECTION_NAME, L"Fullscreen", m_fullscreen, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"FPSCap", m_enableFpsCap, iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"Debug";
		WritePrivateProfileIntW(SECTION_NAME, L"DoNotApplyPatches", m_dontApplyPatches, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"UseDebugD3D", m_useD3DDebugLayer, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"ShowDLLDebugFeatures", m_stfShowDebugFeatures, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"LoadLooseRomFiles", m_stfLooseRomFiles, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"SetGameDebugFlag", m_stfGameDebugFlag, iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"VF5FS";
		WritePrivateProfileIntW(SECTION_NAME, L"ArcadeMode", m_arcadeMode, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"CircleConfirm", m_circleConfirm, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"Language", m_language, iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"StF";
		WritePrivateProfileIntW(SECTION_NAME, L"AspectRatio", m_stfAspect, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"CRTFilter", m_stfCrtFilter, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"Difficulty", m_stfDifficulty, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"Country", m_stfCountry, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"FreePlay", m_stfFreeplay, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"VersusMode", m_stfVersusMode, iniPath.c_str());
		for (int player = 0; player < 2; player++)
		{
			wchar_t key[48];
			swprintf_s(key, L"P%dController", player + 1);
			WritePrivateProfileIntW(SECTION_NAME, key, m_stfPadIndex[player], iniPath.c_str());

			for (uint32_t action = 0; action < StFInput::Action_Count; action++)
			{
				swprintf_s(key, L"P%dKey%hs", player + 1, StFInput::ActionIniName(action));
				WritePrivateProfileIntW(SECTION_NAME, key, m_stfKeyBinds[player][action], iniPath.c_str());

				swprintf_s(key, L"P%dPad%hs", player + 1, StFInput::ActionIniName(action));
				WritePrivateProfileIntW(SECTION_NAME, key, m_stfPadBinds[player][action], iniPath.c_str());
			}
		}
	}
}
