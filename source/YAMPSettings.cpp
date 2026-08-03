#include "YAMPSettings.h"

#include "YAMPGeneral.h" // UTF8ToWchar / WcharToUTF8, for the string-valued settings
#include "m2ftg/DisplayModes.h"

#include "m2ftg/HleHooks.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string_view>

static constexpr std::string_view INI_FILE_NAME = "settings.ini";
static constexpr uint32_t SETTINGS_VERSION = 1;

// Scopes an HLE-hook ini name to the running game.
//
// settings.ini sits next to YAMP.exe and every title shares it, but the hook mask and the
// retarget list are both indexed by position in ONE game's hook table - and those tables have
// different lengths and completely different contents. Sharing a key would mean a hook the user
// disabled in Sonic the Fighters silently disabling whatever Fighting Vipers keeps at that
// index, which for a Core hook is a hang.
//
// StF keeps the bare name so ini files written before Fighting Vipers had a hook table keep
// working; every other game gets a ".<tag>" suffix.
static std::wstring HleKeyW(const wchar_t* base)
{
	if (gGeneral.GetGameId() == YAMPGeneral::GameId::StF)
	{
		return base;
	}
	return std::wstring(base) + L"." + UTF8ToWchar(gGeneral.GetGameTag());
}

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

// Reads a UTF-8 string setting. Netplay credentials and host names are ASCII in practice, so the
// narrow profile API is used directly rather than round-tripping through wide strings.
static std::string GetPrivateProfileStdStringA(LPCSTR section, LPCSTR key, LPCSTR def,
                                               const std::wstring& iniPathW)
{
	const std::string iniPath(iniPathW.begin(), iniPathW.end());
	char buf[256] = {};
	GetPrivateProfileStringA(section, key, def ? def : "", buf, static_cast<DWORD>(std::size(buf)),
	                         iniPath.c_str());
	return std::string(buf);
}

static void WritePrivateProfileStdStringA(LPCSTR section, LPCSTR key, const std::string& value,
                                          const std::wstring& iniPathW)
{
	const std::string iniPath(iniPathW.begin(), iniPathW.end());
	WritePrivateProfileStringA(section, key, value.c_str(), iniPath.c_str());
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

		// Stored as the module's own switch text, so reordering m2ftg::DISPLAY_MODES cannot
		// silently change what an existing ini means. The launcher writes this same key.
		wchar_t renderMode[64] {};
		GetPrivateProfileStringW(SECTION_NAME, L"Model2RenderMode", L"", renderMode,
			static_cast<DWORD>(std::size(renderMode)), iniPath.c_str());
		m_m2RenderMode = static_cast<uint32_t>(m2ftg::DisplayModeFromArg(renderMode));
		m_m2WindowMatchesRender = GetPrivateProfileIntW(SECTION_NAME, L"Model2WindowMatchesRender", 0, iniPath.c_str()) != 0;
	}

	{
		const wchar_t* SECTION_NAME = L"Debug";
		m_dontApplyPatches = GetPrivateProfileIntW(SECTION_NAME, L"DoNotApplyPatches", 0, iniPath.c_str()) != 0;
		m_useD3DDebugLayer = GetPrivateProfileIntW(SECTION_NAME, L"UseDebugD3D", 0, iniPath.c_str()) != 0;
		m_stfShowDebugFeatures = GetPrivateProfileIntW(SECTION_NAME, L"ShowDLLDebugFeatures", 0, iniPath.c_str()) != 0;
		m_stfLooseRomFiles = GetPrivateProfileIntW(SECTION_NAME, L"LoadLooseRomFiles", 0, iniPath.c_str()) != 0;
		m_stfGameDebugFlag = GetPrivateProfileIntW(SECTION_NAME, L"SetGameDebugFlag", 0, iniPath.c_str()) != 0;
		m_stfFixBackupTimeIndex = GetPrivateProfileIntW(SECTION_NAME, L"FixBackupRamTimeIndex", 1, iniPath.c_str()) != 0;
		// Netplay lives in its own ini section: it is host configuration, not per-title state,
		// and keeping it separate means a credential never lands in a game's settings block.
		m_netServer = GetPrivateProfileStdStringA("Netplay", "Server", "", iniPath);
		m_netNpid = GetPrivateProfileStdStringA("Netplay", "Npid", "", iniPath);
		m_netToken = GetPrivateProfileStdStringA("Netplay", "Token", "", iniPath);
		m_netCertFingerprint = GetPrivateProfileStdStringA("Netplay", "CertFingerprint", "", iniPath);
		m_netComId = GetPrivateProfileStdStringA("Netplay", "CommunicationId", "NPWR02113_00", iniPath);
		m_netFrameDelay = GetPrivateProfileIntW(L"Netplay", L"FrameDelay", 3, iniPath.c_str());
		if (m_netFrameDelay < 0 || m_netFrameDelay > 20)
		{
			m_netFrameDelay = 3;   // a hand-edited ini must not be able to wedge the lockstep
		}
		// Up to 96 bits, so it does not fit the profile API's integer reads - stored as hex
		// text. The default is the game's own default mask, not 0: StF's hook 16 fights the
		// backup-RAM TIME fix
		// (see HleHooks.h). NB an ini written by an older build already carries an explicit
		// 0 here, which is indistinguishable from a deliberate "enable everything" - those
		// users get the hook back until they clear the checkbox or delete the two lines.
		//
		// The KEY IS PER-GAME, because the value is a set of bit INDICES into one game's hook
		// table and every game's table is different. settings.ini lives next to YAMP.exe and is
		// shared by every title, so a single key would carry StF's choices into Fighting Vipers
		// and silently disable whatever FV happens to keep at those indices - a Content hook in
		// one game is a Core hook in the other. StF keeps the unsuffixed key so existing ini
		// files still load.
		m_stfHleDisableMask[0] = GetPrivateProfileHex64W(SECTION_NAME, HleKeyW(L"DisabledHleHooksLo").c_str(),
			m2ftg::HleHooks::DefaultDisableMask()[0], iniPath.c_str());
		m_stfHleDisableMask[1] = GetPrivateProfileHex64W(SECTION_NAME, HleKeyW(L"DisabledHleHooksHi").c_str(),
			m2ftg::HleHooks::DefaultDisableMask()[1], iniPath.c_str());
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
		//
		// Per-game section for the same reason the disable mask has a per-game key: Hook33 is
		// StF's `rand` and FV's `char_add2`-era slot, and pointing one game's retarget list at
		// the other's installer would stamp traps into unrelated code before the CPU runs.
		const std::wstring section = HleKeyW(L"HleRetarget");
		const wchar_t* SECTION_NAME = section.c_str();
		for (size_t i = 0; i < m2ftg::HleHooks::MAX_COUNT; i++)
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
		m_m2RealDamage = GetPrivateProfileIntW(SECTION_NAME, L"RealDamage", m_m2RealDamage, iniPath.c_str()) != 0;
		for (int player = 0; player < 2; player++)
		{
			wchar_t key[48];
			// Controller identity is an Input::PadDevice id, not an XInput slot number - a bare
			// index means nothing once the list holds both kinds. ControllerId wins; the old
			// integer key is honoured when it is the only one present, so existing setups
			// migrate silently and are rewritten in the new form on the next save.
			wchar_t idBuf[128] = {};
			swprintf_s(key, L"P%dControllerId", player + 1);
			GetPrivateProfileStringW(SECTION_NAME, key, L"", idBuf,
				static_cast<DWORD>(std::size(idBuf)), iniPath.c_str());
			if (idBuf[0] != L'\0')
			{
				m_m2PadId[player] = WcharToUTF8(idBuf);
			}
			else
			{
				swprintf_s(key, L"P%dController", player + 1);
				const int padIndex = static_cast<int>(
					GetPrivateProfileIntW(SECTION_NAME, key, player, iniPath.c_str()));
				m_m2PadId[player] = (padIndex >= 0 && padIndex < 4)
					? "xinput:" + std::to_string(padIndex)
					: std::string();
			}

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
		WritePrivateProfileStringW(SECTION_NAME, L"Model2RenderMode",
			m2ftg::DISPLAY_MODES[m_m2RenderMode < m2ftg::DISPLAY_MODE_COUNT ? m_m2RenderMode : 0].arg,
			iniPath.c_str());
		WritePrivateProfileIntW(SECTION_NAME, L"Model2WindowMatchesRender", m_m2WindowMatchesRender, iniPath.c_str());
	
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
		WritePrivateProfileIntW(SECTION_NAME, L"FixBackupRamTimeIndex", m_stfFixBackupTimeIndex, iniPath.c_str());
		WritePrivateProfileStdStringA("Netplay", "Server", m_netServer, iniPath);
		WritePrivateProfileStdStringA("Netplay", "Npid", m_netNpid, iniPath);
		WritePrivateProfileStdStringA("Netplay", "Token", m_netToken, iniPath);
		WritePrivateProfileStdStringA("Netplay", "CertFingerprint", m_netCertFingerprint, iniPath);
		WritePrivateProfileStdStringA("Netplay", "CommunicationId", m_netComId, iniPath);
		WritePrivateProfileIntW(L"Netplay", L"FrameDelay", m_netFrameDelay, iniPath.c_str());
		// Core hooks stay session-only, so a restart always gets back to a bootable state.
		uint64_t persisted[2] = { m_stfHleDisableMask[0], m_stfHleDisableMask[1] };
		m2ftg::HleHooks::MaskStripKinds(persisted, m2ftg::HleHooks::SESSION_ONLY_KINDS);
		// Same per-game key as the load side - see HleKeyW.
		WritePrivateProfileHex64W(SECTION_NAME, HleKeyW(L"DisabledHleHooksLo").c_str(), persisted[0], iniPath.c_str());
		WritePrivateProfileHex64W(SECTION_NAME, HleKeyW(L"DisabledHleHooksHi").c_str(), persisted[1], iniPath.c_str());
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
		WritePrivateProfileIntW(SECTION_NAME, L"RealDamage", m_m2RealDamage, iniPath.c_str());
		for (int player = 0; player < 2; player++)
		{
			wchar_t key[48];
			swprintf_s(key, L"P%dControllerId", player + 1);
			WritePrivateProfileStringW(SECTION_NAME, key,
				UTF8ToWchar(m_m2PadId[player]).c_str(), iniPath.c_str());

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
	}
}
