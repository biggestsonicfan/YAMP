#pragma once

// SteamOwnership — asks the running Steam client which parent games the signed-in account owns.
//
// Why: the parent-game gate in GameVerify used to have exactly one way to prove ownership —
// find the title's executable on disk. That made people copy LostJudgment.exe (or a whole
// install) next to YAMP.exe just to satisfy the check. Steam already knows the answer: the
// client holds every license the account has, so a Steamworks query settles "owns Lost
// Judgment?" without a single game file being present. The arcade module folder — the DLL with
// its rom/ and sound assets — is then all YAMP needs on disk.
//
// How: steam_api64.dll (the Steamworks SDK redistributable, copied next to YAMP.exe at build
// time from external/SteamworksSDK) is loaded BY HAND with LoadLibrary rather than linked, so a
// missing DLL costs nothing but this feature — a GOG-only user never needs Steam at all. The
// session is short-lived: connect, ask ISteamApps::BIsSubscribedApp for each app id, disconnect.
// It connects as app id 480 (Spacewar, Valve's test app that every account may use), which is
// the documented way for something that is not itself a Steam product to talk to the client;
// BIsSubscribedApp answers for ANY app id, which is precisely what the SDK header says it is for
// ("check ownership of another game related to yours"). The answers are cached until Refresh().
//
// Where: in a HELPER PROCESS, never in the YAMP that asked. Query() runs this same YAMP.exe as
// "YAMP.exe -steamquery <app ids>", which connects, asks, writes the report to its stdout and
// exits. The reason is controllers: SteamAPI_InitFlat pulls the client's
// gameoverlayrenderer64.dll into the calling process, and that DLL detours XInput, DirectInput,
// HidD_* and the SetupDi device enumeration - every entry point YAMP's pad backends read - and
// SteamAPI_Shutdown does not take it back out. With Steam Input managing an Xbox pad, that is how
// the pad disappeared from YAMP once the in-process check shipped. The helper takes the hooks
// with it when it exits. Input::Diagnose() reports whether they are present anyway (YAMP
// started from Steam as a non-Steam shortcut gets the overlay injected at launch).
//
// Steam absent, not signed in, or the DLL missing = "not available", never a failure: the
// executable search in GameVerify still stands, exactly as before.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Steamworks
{
	struct AppOwnership
	{
		uint32_t appId = 0;
		bool owned = false;       // the signed-in account holds a license (ISteamApps::BIsSubscribedApp)
		bool installed = false;   // the client has it installed (BIsAppInstalled) — a different question
		std::filesystem::path installDir;   // where, when installed (GetAppInstallDir)
	};

	struct Report
	{
		bool attempted = false;    // Query() has run
		bool available = false;    // the client answered, so the per-app answers mean something
		std::string failure;       // why not, when !available ("Steam is not running", ...)
		std::string personaName;   // the signed-in account, for the UI
		uint64_t steamId = 0;      // ...and its SteamID64, for the log
		std::vector<AppOwnership> apps;
	};

	// Ask the helper process about every app id (it connects, asks and disconnects). Cached: a
	// second call returns the same report until Refresh() — the connection is not free and the
	// answers do not change while YAMP runs. "-nosteam" on the command line skips the client
	// entirely, which is how the executable-only path gets exercised on a machine that has Steam.
	const Report& Query(const uint32_t* appIds, size_t count);

	// The last report, or a default (attempted == false) one before any Query.
	const Report& LastReport();

	// Forget the cached answers, so the next Query asks the client again (the launcher's Rescan,
	// for someone who signed in to Steam after opening YAMP).
	void Refresh();

	// The cached answer for one app id; null when no available report covers it.
	const AppOwnership* Find(uint32_t appId);

	// The helper side, for wWinMain: true when this process IS the helper ("-steamquery" on the
	// command line). RunHelper then does the Steamworks session here, writes the report to stdout
	// and returns the process exit code - before any window, device or input exists.
	bool IsHelperInvocation(const wchar_t* cmdLine);
	int RunHelper(const wchar_t* cmdLine);
}
