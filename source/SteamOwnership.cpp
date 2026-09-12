// SteamOwnership.cpp — see SteamOwnership.h for why this exists and why it loads the DLL by hand.

#include "SteamOwnership.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// Declarations only. STEAM_API_NODLL turns S_API into plain extern "C" so nothing in here becomes
// a dllimport reference: every export is resolved with GetProcAddress below, and the ISteam*
// interfaces are the SDK's own pure-virtual classes, called through their vtables the way every
// Steam game calls them.
#define STEAM_API_NODLL
#include "steam/steam_api.h"

#include <cstring>
#include <iterator>
#include <memory>

#include "DebugLog.h"
#include "GameVerify.h"   // Verify::OwnershipBypassed - the one place the switch names live
#include "StringUtil.h"
#include "wil/resource.h"

namespace Steamworks
{
	namespace
	{
		// Spacewar, Valve's test app. Every Steam account can run it, which is what makes it the
		// app id for a tool that is not itself a Steam product. The client shows the account as
		// "playing Spacewar" for the moment the connection is open — milliseconds, since the
		// session is closed as soon as the answers are in.
		constexpr const wchar_t* SPACEWAR_APP_ID = L"480";

		using PFN_InitFlat = ESteamAPIInitResult (S_CALLTYPE*)(SteamErrMsg*);
		using PFN_Init = bool (S_CALLTYPE*)();
		using PFN_Shutdown = void (S_CALLTYPE*)();
		using PFN_GetHSteamUser = HSteamUser (S_CALLTYPE*)();
		using PFN_FindOrCreateUserInterface = void* (S_CALLTYPE*)(HSteamUser, const char*);

		Report g_report;

		bool FlagOnCommandLine(const wchar_t* flag)
		{
			const wchar_t* cmdLine = GetCommandLineW();
			return cmdLine != nullptr && wcsstr(cmdLine, flag) != nullptr;
		}

		// steam_api reads the app id it should connect as out of the environment (SteamAppId,
		// and SteamGameId for the client's notion of "which game is this"). Both are set for the
		// duration of the query and put back afterwards: a child process the launcher spawns
		// inherits the environment, and if YAMP itself was started through Steam as a non-Steam
		// shortcut the client already gave those variables values of its own.
		class ScopedEnv
		{
		public:
			ScopedEnv(const wchar_t* name, const wchar_t* value) : m_name(name)
			{
				wchar_t old[64] {};
				const DWORD len = GetEnvironmentVariableW(name, old, static_cast<DWORD>(std::size(old)));
				if (len > 0 && len < std::size(old))
				{
					m_hadValue = true;
					m_old = old;
				}
				SetEnvironmentVariableW(name, value);
			}
			~ScopedEnv()
			{
				SetEnvironmentVariableW(m_name, m_hadValue ? m_old.c_str() : nullptr);
			}
			ScopedEnv(const ScopedEnv&) = delete;
			ScopedEnv& operator=(const ScopedEnv&) = delete;

		private:
			const wchar_t* m_name;
			bool m_hadValue = false;
			std::wstring m_old;
		};

		// Next to YAMP.exe, by absolute path. A bare "steam_api64.dll" would let the loader pick
		// one up from the CWD — which is a game folder whenever a game is booted — and the copy
		// beside YAMP.exe is the one this build was compiled against.
		std::filesystem::path SteamApiDllPath()
		{
			wchar_t exePath[MAX_PATH];
			if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return {};
			return std::filesystem::path(exePath).parent_path() / L"steam_api64.dll";
		}

		std::string DescribeInitFailure(ESteamAPIInitResult result, const SteamErrMsg& message)
		{
			std::string text;
			switch (result)
			{
			case k_ESteamAPIInitResult_NoSteamClient: text = "Steam is not running"; break;
			case k_ESteamAPIInitResult_VersionMismatch: text = "the Steam client is out of date"; break;
			default: text = "Steam refused the connection"; break;
			}
			if (message[0] != '\0')
			{
				text += " (";
				text += message;
				text += ")";
			}
			return text;
		}

		void Fail(std::string why)
		{
			g_report.available = false;
			g_report.failure = std::move(why);
			DebugLog("[steam] ownership check unavailable: %s\n", g_report.failure.c_str());
		}
	}

	const Report& Query(const uint32_t* appIds, size_t count)
	{
		if (g_report.attempted) return g_report;

		g_report = Report {};
		g_report.attempted = true;
		for (size_t i = 0; i < count; i++)
		{
			AppOwnership app;
			app.appId = appIds[i];
			g_report.apps.push_back(std::move(app));
		}

		if (FlagOnCommandLine(L"-nosteam"))
		{
			Fail("disabled with -nosteam");
			return g_report;
		}

		// The ownership bypass implies this one. Asking the client is how ownership gets proven,
		// and a run that is not gated on ownership has nothing to do with the answer - so loading
		// steam_api64.dll and opening a session (which shows the account as "playing Spacewar"
		// for as long as it is up) would be pure cost. The switch names are GameVerify's; this
		// reads the decision rather than re-spelling them.
		if (Verify::OwnershipBypassed())
		{
			Fail("not asked - ownership was bypassed on the command line");
			return g_report;
		}

		const std::filesystem::path dllPath = SteamApiDllPath();
		// Never freed. steamclient keeps per-process state behind this module, and a game
		// process runs for as long as the game does anyway.
		static HMODULE dll = nullptr;
		if (dll == nullptr && !dllPath.empty())
		{
			dll = LoadLibraryW(dllPath.c_str());
		}
		if (dll == nullptr)
		{
			Fail("steam_api64.dll is missing next to YAMP.exe");
			return g_report;
		}

		const auto initFlat = reinterpret_cast<PFN_InitFlat>(GetProcAddress(dll, "SteamAPI_InitFlat"));
		const auto init = reinterpret_cast<PFN_Init>(GetProcAddress(dll, "SteamAPI_Init"));
		const auto shutdown = reinterpret_cast<PFN_Shutdown>(GetProcAddress(dll, "SteamAPI_Shutdown"));
		const auto getUser = reinterpret_cast<PFN_GetHSteamUser>(GetProcAddress(dll, "SteamAPI_GetHSteamUser"));
		const auto findInterface = reinterpret_cast<PFN_FindOrCreateUserInterface>(
			GetProcAddress(dll, "SteamInternal_FindOrCreateUserInterface"));
		if ((initFlat == nullptr && init == nullptr) || shutdown == nullptr || getUser == nullptr
			|| findInterface == nullptr)
		{
			Fail("steam_api64.dll next to YAMP.exe is not the Steamworks redistributable YAMP was built with");
			return g_report;
		}

		ScopedEnv appIdEnv(L"SteamAppId", SPACEWAR_APP_ID);
		ScopedEnv gameIdEnv(L"SteamGameId", SPACEWAR_APP_ID);

		// InitFlat is the export meant for exactly this — a caller that loaded the DLL by hand —
		// and it explains a failure. The plain Init is the older export, kept as a fallback.
		SteamErrMsg message {};
		ESteamAPIInitResult result = k_ESteamAPIInitResult_FailedGeneric;
		if (initFlat != nullptr)
		{
			result = initFlat(&message);
		}
		else if (init())
		{
			result = k_ESteamAPIInitResult_OK;
		}
		if (result != k_ESteamAPIInitResult_OK)
		{
			Fail(DescribeInitFailure(result, message));
			return g_report;
		}
		auto disconnect = wil::scope_exit([shutdown] { shutdown(); });

		const HSteamUser user = getUser();
		auto* apps = static_cast<ISteamApps*>(findInterface(user, STEAMAPPS_INTERFACE_VERSION));
		if (apps == nullptr)
		{
			Fail("the Steam client does not provide " STEAMAPPS_INTERFACE_VERSION " - update Steam");
			return g_report;
		}

		if (auto* friends = static_cast<ISteamFriends*>(findInterface(user, STEAMFRIENDS_INTERFACE_VERSION)))
		{
			if (const char* name = friends->GetPersonaName())
			{
				g_report.personaName = name;
			}
		}
		if (auto* steamUser = static_cast<ISteamUser*>(findInterface(user, STEAMUSER_INTERFACE_VERSION)))
		{
			g_report.steamId = steamUser->GetSteamID().ConvertToUint64();
		}

		for (AppOwnership& app : g_report.apps)
		{
			app.owned = apps->BIsSubscribedApp(app.appId);
			app.installed = apps->BIsAppInstalled(app.appId);
			if (app.installed)
			{
				char folder[1024] {};
				if (apps->GetAppInstallDir(app.appId, folder, static_cast<uint32>(sizeof(folder))) > 0
					&& folder[0] != '\0')
				{
					app.installDir = std::filesystem::u8path(folder);
				}
			}
			DebugLog("[steam] app %u: %s, %s%s\n", app.appId, app.owned ? "owned" : "not owned",
				app.installed ? "installed at " : "not installed",
				app.installed ? WcharToUTF8(app.installDir.wstring()).c_str() : "");
		}

		g_report.available = true;
		DebugLog("[steam] signed in as %s (%llu), %zu app ids checked\n",
			g_report.personaName.empty() ? "?" : g_report.personaName.c_str(),
			static_cast<unsigned long long>(g_report.steamId), g_report.apps.size());
		return g_report;
	}

	const Report& LastReport() { return g_report; }

	void Refresh() { g_report.attempted = false; }

	const AppOwnership* Find(uint32_t appId)
	{
		if (!g_report.available) return nullptr;
		for (const AppOwnership& app : g_report.apps)
		{
			if (app.appId == appId) return &app;
		}
		return nullptr;
	}
}
