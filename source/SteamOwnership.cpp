// SteamOwnership.cpp — see SteamOwnership.h for why this exists, why it loads the DLL by hand, and
// why the loading happens in a helper process rather than here.

#include "SteamOwnership.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// Declarations only. STEAM_API_NODLL turns S_API into plain extern "C" so nothing in here becomes
// a dllimport reference: every export is resolved with GetProcAddress below, and the ISteam*
// interfaces are the SDK's own pure-virtual classes, called through their vtables the way every
// Steam game calls them.
#define STEAM_API_NODLL
#include "steam/steam_api.h"

#include <cstdio>
#include <cstdlib>
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
		// "playing Spacewar" for as long as the helper process is connected — well under a second.
		constexpr const wchar_t* SPACEWAR_APP_ID = L"480";

		// The helper's switch, followed by the app ids to ask about: "-steamquery 2058190 1235140".
		constexpr const wchar_t* HELPER_SWITCH = L"-steamquery";

		// SteamAPI_InitFlat returns at once when the client is absent; a client that is running
		// but still starting up (or updating) can hold it for several seconds. Past this, the
		// helper is killed and the check reads as unavailable - the executable search still stands.
		constexpr DWORD HELPER_TIMEOUT_MS = 30000;

		// The report crosses the pipe as lines of text: this header (anything before it is
		// steam_api's own stdout chatter, see DecodeReport), then "fail <why>",
		// "persona <name>", "steamid <n>" and "app <id> <owned> <installed> <install dir>" lines,
		// then "end". A helper that dies half way never writes "end", so a torn report is told
		// apart from an honest "not available".
		constexpr const char* REPORT_HEADER = "YAMP-STEAM 1";

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

		void Fail(Report& report, std::string why)
		{
			report.available = false;
			report.failure = std::move(why);
			DebugLog("[steam] ownership check unavailable: %s\n", report.failure.c_str());
		}

		// The Steamworks session itself. Runs ONLY in the helper process: SteamAPI_InitFlat loads
		// the client's gameoverlayrenderer64.dll into whatever process calls it, and that DLL
		// detours XInputGetState, DirectInput8Create, HidD_GetAttributes and the SetupDi device
		// enumeration - every entry point YAMP's three pad backends use - and stays loaded after
		// SteamAPI_Shutdown. Measured 2026-09-21 on a Release build with cdb: all six detoured
		// with the session run in-process, all pristine with -nosteam. With Steam Input managing
		// an Xbox pad, those detours are how Steam hides the physical device from a "game".
		void AskClient(Report& report)
		{
			const std::filesystem::path dllPath = SteamApiDllPath();
			// Never freed: the helper exits right after this returns.
			const HMODULE dll = dllPath.empty() ? nullptr : LoadLibraryW(dllPath.c_str());
			if (dll == nullptr)
			{
				Fail(report, "steam_api64.dll is missing next to YAMP.exe");
				return;
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
				Fail(report, "steam_api64.dll next to YAMP.exe is not the Steamworks redistributable YAMP was built with");
				return;
			}

			// steam_api reads the app id it should connect as out of the environment (SteamAppId,
			// and SteamGameId for the client's notion of "which game is this"). Set outright, not
			// saved and restored: this process ends with the query, and the YAMP that started it
			// never had its own environment touched - which is what matters when YAMP was itself
			// started from Steam as a non-Steam shortcut and the client gave these values of its own.
			SetEnvironmentVariableW(L"SteamAppId", SPACEWAR_APP_ID);
			SetEnvironmentVariableW(L"SteamGameId", SPACEWAR_APP_ID);

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
				Fail(report, DescribeInitFailure(result, message));
				return;
			}
			auto disconnect = wil::scope_exit([shutdown] { shutdown(); });

			const HSteamUser user = getUser();
			auto* apps = static_cast<ISteamApps*>(findInterface(user, STEAMAPPS_INTERFACE_VERSION));
			if (apps == nullptr)
			{
				Fail(report, "the Steam client does not provide " STEAMAPPS_INTERFACE_VERSION " - update Steam");
				return;
			}

			if (auto* friends = static_cast<ISteamFriends*>(findInterface(user, STEAMFRIENDS_INTERFACE_VERSION)))
			{
				if (const char* name = friends->GetPersonaName())
				{
					report.personaName = name;
				}
			}
			if (auto* steamUser = static_cast<ISteamUser*>(findInterface(user, STEAMUSER_INTERFACE_VERSION)))
			{
				report.steamId = steamUser->GetSteamID().ConvertToUint64();
			}

			for (AppOwnership& app : report.apps)
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
			}

			report.available = true;
		}

		// ---- The pipe protocol ----------------------------------------------------------------

		// One field per line, so a line break inside a value (a persona name is free text) must
		// not survive into the pipe.
		std::string OneLine(std::string text)
		{
			for (char& c : text)
			{
				if (c == '\r' || c == '\n') c = ' ';
			}
			return text;
		}

		std::string EncodeReport(const Report& report)
		{
			std::string text = std::string(REPORT_HEADER) + "\n";
			if (!report.available)
			{
				text += "fail " + OneLine(report.failure) + "\n";
			}
			else
			{
				text += "persona " + OneLine(report.personaName) + "\n";
				text += "steamid " + std::to_string(report.steamId) + "\n";
				for (const AppOwnership& app : report.apps)
				{
					text += "app " + std::to_string(app.appId) + (app.owned ? " 1" : " 0")
						+ (app.installed ? " 1 " : " 0 ") + OneLine(app.installDir.u8string()) + "\n";
				}
			}
			text += "end\n";
			return text;
		}

		// Fills `report` (whose apps list already holds the ids that were asked about) from the
		// helper's output. False when the output is not a complete report.
		bool DecodeReport(const std::string& text, Report& report)
		{
			bool sawHeader = false;
			bool sawEnd = false;
			bool failed = false;
			std::string failure;

			size_t pos = 0;
			while (pos < text.size() && !sawEnd)
			{
				size_t eol = text.find('\n', pos);
				if (eol == std::string::npos) eol = text.size();
				std::string line = text.substr(pos, eol - pos);
				pos = eol + 1;
				if (!line.empty() && line.back() == '\r') line.pop_back();

				// steam_api writes chatter of its own to the same stdout before the report
				// ("Setting breakpad minidump AppID = 480", "SteamInternal_SetMinidumpSteamID: ..."),
				// so everything ahead of the header is skipped rather than refused.
				if (!sawHeader)
				{
					sawHeader = line == REPORT_HEADER;
					continue;
				}

				const size_t space = line.find(' ');
				const std::string key = line.substr(0, space);
				const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
				if (key == "end")
				{
					sawEnd = true;
				}
				else if (key == "fail")
				{
					failed = true;
					failure = rest;
				}
				else if (key == "persona")
				{
					report.personaName = rest;
				}
				else if (key == "steamid")
				{
					report.steamId = std::strtoull(rest.c_str(), nullptr, 10);
				}
				else if (key == "app")
				{
					// "<id> <owned> <installed> <install dir>"; the directory is the remainder, spaces
					// and all.
					char* cursor = nullptr;
					const auto appId = static_cast<uint32_t>(std::strtoul(rest.c_str(), &cursor, 10));
					const bool owned = std::strtoul(cursor, &cursor, 10) != 0;
					const bool installed = std::strtoul(cursor, &cursor, 10) != 0;
					if (*cursor == ' ') cursor++;
					for (AppOwnership& app : report.apps)
					{
						if (app.appId != appId) continue;
						app.owned = owned;
						app.installed = installed;
						app.installDir = std::filesystem::u8path(cursor);
					}
				}
			}
			if (!sawEnd) return false;

			if (failed)
			{
				Fail(report, failure);
			}
			else
			{
				report.available = true;
			}
			return true;
		}

		// Runs this same YAMP.exe as "-steamquery <ids>" and reads the report back from its stdout.
		void AskHelper(Report& report)
		{
			wchar_t exePath[MAX_PATH];
			if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
			{
				Fail(report, "could not find YAMP.exe to start the Steam helper");
				return;
			}
			std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" " + HELPER_SWITCH;
			for (const AppOwnership& app : report.apps)
			{
				cmdLine += L" " + std::to_wstring(app.appId);
			}

			// The helper's stdout. 64 KiB is far past any report, so the helper never blocks on a
			// full pipe while this side waits for it to exit.
			SECURITY_ATTRIBUTES inheritable { sizeof(inheritable), nullptr, TRUE };
			wil::unique_handle readEnd;
			wil::unique_handle writeEnd;
			if (!CreatePipe(readEnd.put(), writeEnd.put(), &inheritable, 64 * 1024))
			{
				Fail(report, "could not create the Steam helper's pipe");
				return;
			}
			SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0);

			// Inherit the pipe and NOTHING else: a booted game has plenty of handles open, and a
			// short-lived helper has no business holding any of them.
			SIZE_T attrSize = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
			std::unique_ptr<char[]> attrBuffer(new char[attrSize]);
			auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.get());
			if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize))
			{
				Fail(report, "could not prepare the Steam helper's handle list");
				return;
			}
			auto deleteAttrs = wil::scope_exit([attrs] { DeleteProcThreadAttributeList(attrs); });
			HANDLE inherited[] = { writeEnd.get() };
			UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
				sizeof(inherited), nullptr, nullptr);

			STARTUPINFOEXW startup {};
			startup.StartupInfo.cb = sizeof(startup);
			startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			startup.StartupInfo.hStdOutput = writeEnd.get();
			startup.lpAttributeList = attrs;
			PROCESS_INFORMATION process {};
			if (!CreateProcessW(exePath, cmdLine.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
				&startup.StartupInfo, &process))
			{
				Fail(report, "could not start the Steam helper (error " + std::to_string(GetLastError()) + ")");
				return;
			}
			wil::unique_handle helper(process.hProcess);
			CloseHandle(process.hThread);
			// This side's copy of the write end has to go, or ReadFile below never sees the end of
			// the pipe.
			writeEnd.reset();

			if (WaitForSingleObject(helper.get(), HELPER_TIMEOUT_MS) != WAIT_OBJECT_0)
			{
				TerminateProcess(helper.get(), 1);
				Fail(report, "Steam did not answer within " + std::to_string(HELPER_TIMEOUT_MS / 1000) + " seconds");
				return;
			}

			std::string output;
			char buffer[4096];
			DWORD got = 0;
			while (ReadFile(readEnd.get(), buffer, sizeof(buffer), &got, nullptr) && got > 0)
			{
				output.append(buffer, got);
			}

			if (!DecodeReport(output, report))
			{
				DWORD exitCode = 0;
				GetExitCodeProcess(helper.get(), &exitCode);
				char code[16];
				sprintf_s(code, "0x%08X", static_cast<unsigned>(exitCode));
				Fail(report, std::string("the Steam helper ended without answering (exit code ") + code + ")");
			}
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
			Fail(g_report, "disabled with -nosteam");
			return g_report;
		}

		// The ownership bypass implies this one. Asking the client is how ownership gets proven,
		// and a run that is not gated on ownership has nothing to do with the answer - so starting
		// the helper and opening a session (which shows the account as "playing Spacewar" for as
		// long as it is up) would be pure cost. The switch names are GameVerify's; this reads the
		// decision rather than re-spelling them.
		if (Verify::OwnershipBypassed())
		{
			Fail(g_report, "not asked - ownership was bypassed on the command line");
			return g_report;
		}

		AskHelper(g_report);
		if (!g_report.available) return g_report;

#if YAMP_DEBUG_LOGGING
		for (const AppOwnership& app : g_report.apps)
		{
			DebugLog("[steam] app %u: %s, %s%s\n", app.appId, app.owned ? "owned" : "not owned",
				app.installed ? "installed at " : "not installed",
				app.installed ? WcharToUTF8(app.installDir.wstring()).c_str() : "");
		}
#endif
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

	bool IsHelperInvocation(const wchar_t* cmdLine)
	{
		return cmdLine != nullptr && wcsstr(cmdLine, HELPER_SWITCH) != nullptr;
	}

	int RunHelper(const wchar_t* cmdLine)
	{
		Report report;
		report.attempted = true;
		const wchar_t* cursor = wcsstr(cmdLine, HELPER_SWITCH) + wcslen(HELPER_SWITCH);
		for (;;)
		{
			wchar_t* end = nullptr;
			const unsigned long appId = wcstoul(cursor, &end, 10);
			if (end == cursor) break;
			AppOwnership app;
			app.appId = static_cast<uint32_t>(appId);
			report.apps.push_back(std::move(app));
			cursor = end;
		}

		AskClient(report);

		const std::string text = EncodeReport(report);
		const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD written = 0;
		const bool sent = out != nullptr && out != INVALID_HANDLE_VALUE
			&& WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
			&& written == text.size();
		return sent ? 0 : 1;
	}
}
