#include "YAMPSettings.h"

#include "m2ftg/LJ/HleHooks.h"

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

static uint64_t GetPrivateProfileHex64W(LPCWSTR lpAppName, LPCWSTR lpKeyName, uint64_t defaultValue, LPCWSTR lpFileName)
{
	wchar_t buf[32];
	if (GetPrivateProfileStringW(lpAppName, lpKeyName, nullptr, buf, static_cast<DWORD>(std::size(buf)), lpFileName) == 0)
	{
		return defaultValue;
	}
	wchar_t* end = nullptr;
	const uint64_t val = _wcstoui64(buf, &end, 16);
	return (end != nullptr && end != buf) ? val : defaultValue;
}

// One [HleRetarget] entry, returned verbatim for HleHooks::ResolveRetarget to interpret - it may
// name an ELF symbol, which is not resolvable this early. All this does is strip the annotation
// and the surrounding whitespace.
static std::string GetHleRetargetEntryW(LPCWSTR lpAppName, LPCWSTR lpKeyName, LPCWSTR lpFileName)
{
	wchar_t buf[96];
	if (GetPrivateProfileStringW(lpAppName, lpKeyName, nullptr, buf, static_cast<DWORD>(std::size(buf)), lpFileName) == 0)
	{
		return {};
	}

	// The profile API only honours ';' as a comment when it starts the line, so a trailing
	// annotation - which this section is meant to carry, one per hook - arrives as part of the
	// value. Cut it here, then trim, or every entry silently reads as "leave it alone".
	if (wchar_t* comment = wcspbrk(buf, L";#"))
	{
		*comment = L'\0';
	}

	const wchar_t* text = buf;
	while (*text == L' ' || *text == L'\t')
	{
		text++;
	}
	size_t length = wcslen(text);
	while (length != 0 && (text[length - 1] == L' ' || text[length - 1] == L'\t'))
	{
		length--;
	}
	// Symbol names and hex are ASCII; a local narrowing keeps this file free of YAMPGeneral.h,
	// which includes this header back.
	std::string result(length, '\0');
	for (size_t i = 0; i < length; i++)
	{
		result[i] = text[i] < 0x80 ? static_cast<char>(text[i]) : '?';
	}
	return result;
}

static void WritePrivateProfileHex64W(LPCWSTR lpAppName, LPCWSTR lpKeyName, uint64_t value, LPCWSTR lpFileName)
{
	wchar_t buf[32];
	swprintf_s(buf, L"%016llX", value);
	WritePrivateProfileStringW(lpAppName, lpKeyName, buf, lpFileName);
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
		// 76 bits, so it does not fit the profile API's integer reads - stored as hex text.
		m_stfHleDisableMask[0] = GetPrivateProfileHex64W(SECTION_NAME, L"DisabledHleHooksLo", 0, iniPath.c_str());
		m_stfHleDisableMask[1] = GetPrivateProfileHex64W(SECTION_NAME, L"DisabledHleHooksHi", 0, iniPath.c_str());
		// Belt and braces with the strip on save: a hand-edited ini must not be able to make
		// YAMP unbootable either, since a hung board also takes the settings UI down with it.
		m2ftg::HleHooks::MaskStripKinds(m_stfHleDisableMask, m2ftg::HleHooks::SESSION_ONLY_KINDS);
	}

	{
		// Advanced, hand-authored, and deliberately not exposed as a checkbox: this decides what
		// the module's hook installer patches before it runs, which is the only point at which a
		// non-StF program ROM can be protected from it. The live disable mask cannot serve here,
		// because Core bits are stripped from the ini by design (see SESSION_ONLY_KINDS) and a
		// homebrew ROM's whole problem is the Core hooks.
		const wchar_t* SECTION_NAME = L"HleRetarget";
		for (size_t i = 0; i < m2ftg::HleHooks::COUNT; i++)
		{
			wchar_t key[16];
			swprintf_s(key, L"Hook%u", static_cast<unsigned>(i));
			m_stfHleRetarget[i] = GetHleRetargetEntryW(SECTION_NAME, key, iniPath.c_str());
		}
	}

	{
		const wchar_t* SECTION_NAME = L"VF5FS";
		m_arcadeMode = GetPrivateProfileIntW(SECTION_NAME, L"ArcadeMode", 0, iniPath.c_str()) != 0;
		m_circleConfirm = GetPrivateProfileIntW(SECTION_NAME, L"CircleConfirm", 0, iniPath.c_str()) != 0;
		m_language = GetPrivateProfileIntW(SECTION_NAME, L"Language", 1, iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"Audio";
		m_volumePercent = GetPrivateProfileIntW(SECTION_NAME, L"Volume", m_volumePercent, iniPath.c_str());
		if (m_volumePercent > 100) m_volumePercent = 100;
	}

	{
		const wchar_t* SECTION_NAME = L"StF";
		m_m2Aspect = GetPrivateProfileIntW(SECTION_NAME, L"AspectRatio", m_m2Aspect, iniPath.c_str());
		m_m2CrtFilter = GetPrivateProfileIntW(SECTION_NAME, L"CRTFilter", m_m2CrtFilter, iniPath.c_str()) != 0;
		m_m2Difficulty = GetPrivateProfileIntW(SECTION_NAME, L"Difficulty", m_m2Difficulty, iniPath.c_str());
		m_m2Country = GetPrivateProfileIntW(SECTION_NAME, L"Country", m_m2Country, iniPath.c_str());
		m_m2Freeplay = GetPrivateProfileIntW(SECTION_NAME, L"FreePlay", m_m2Freeplay, iniPath.c_str()) != 0;
		m_m2VersusMode = GetPrivateProfileIntW(SECTION_NAME, L"VersusMode", m_m2VersusMode, iniPath.c_str()) != 0;
		for (int player = 0; player < 2; player++)
		{
			wchar_t key[48];
			swprintf_s(key, L"P%dController", player + 1);
			int padIndex = static_cast<int>(GetPrivateProfileIntW(SECTION_NAME, key, m_m2PadIndex[player], iniPath.c_str()));
			m_m2PadIndex[player] = (padIndex >= -1 && padIndex < 4) ? padIndex : -1;

			for (uint32_t action = 0; action < Input::Action_Count; action++)
			{
				swprintf_s(key, L"P%dKey%hs", player + 1, Input::ActionIniName(action));
				uint32_t vk = GetPrivateProfileIntW(SECTION_NAME, key, m_m2KeyBinds[player][action], iniPath.c_str());
				m_m2KeyBinds[player][action] = vk < 256 ? vk : 0;

				swprintf_s(key, L"P%dPad%hs", player + 1, Input::ActionIniName(action));
				uint32_t button = GetPrivateProfileIntW(SECTION_NAME, key, m_m2PadBinds[player][action], iniPath.c_str());
				m_m2PadBinds[player][action] = button < Input::Pad_Count ? button : Input::Pad_None;
			}
		}
	}

	{
		const wchar_t* SECTION_NAME = L"VF2";
		m_vf2Version20 = GetPrivateProfileIntW(SECTION_NAME, L"Version20", m_vf2Version20, iniPath.c_str()) != 0;
		m_vf2DisablePepsi = GetPrivateProfileIntW(SECTION_NAME, L"DisablePepsi", m_vf2DisablePepsi, iniPath.c_str()) != 0;
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
		// Core hooks stay session-only, so a restart always gets back to a bootable state.
		uint64_t persisted[2] = { m_stfHleDisableMask[0], m_stfHleDisableMask[1] };
		m2ftg::HleHooks::MaskStripKinds(persisted, m2ftg::HleHooks::SESSION_ONLY_KINDS);
		WritePrivateProfileHex64W(SECTION_NAME, L"DisabledHleHooksLo", persisted[0], iniPath.c_str());
		WritePrivateProfileHex64W(SECTION_NAME, L"DisabledHleHooksHi", persisted[1], iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"VF5FS";
		WritePrivateProfileIntW(SECTION_NAME, L"ArcadeMode", m_arcadeMode, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"CircleConfirm", m_circleConfirm, iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"Audio";
		WritePrivateProfileIntW(SECTION_NAME, L"Volume", m_volumePercent, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"Language", m_language, iniPath.c_str());
	}

	{
		const wchar_t* SECTION_NAME = L"StF";
		WritePrivateProfileIntW(SECTION_NAME, L"AspectRatio", m_m2Aspect, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"CRTFilter", m_m2CrtFilter, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"Difficulty", m_m2Difficulty, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"Country", m_m2Country, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"FreePlay", m_m2Freeplay, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"VersusMode", m_m2VersusMode, iniPath.c_str());
		for (int player = 0; player < 2; player++)
		{
			wchar_t key[48];
			swprintf_s(key, L"P%dController", player + 1);
			WritePrivateProfileIntW(SECTION_NAME, key, m_m2PadIndex[player], iniPath.c_str());

			for (uint32_t action = 0; action < Input::Action_Count; action++)
			{
				swprintf_s(key, L"P%dKey%hs", player + 1, Input::ActionIniName(action));
				WritePrivateProfileIntW(SECTION_NAME, key, m_m2KeyBinds[player][action], iniPath.c_str());

				swprintf_s(key, L"P%dPad%hs", player + 1, Input::ActionIniName(action));
				WritePrivateProfileIntW(SECTION_NAME, key, m_m2PadBinds[player][action], iniPath.c_str());
			}
		}
	}

	{
		const wchar_t* SECTION_NAME = L"VF2";
		WritePrivateProfileIntW(SECTION_NAME, L"Version20", m_vf2Version20, iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"DisablePepsi", m_vf2DisablePepsi, iniPath.c_str());
	}
}
