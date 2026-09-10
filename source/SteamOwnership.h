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

	// Connect, ask about every app id, disconnect. Cached: a second call returns the same report
	// until Refresh() — the connection is not free and the answers do not change while YAMP
	// runs. "-nosteam" on the command line skips the client entirely, which is how the
	// executable-only path gets exercised on a machine that has Steam.
	const Report& Query(const uint32_t* appIds, size_t count);

	// The last report, or a default (attempted == false) one before any Query.
	const Report& LastReport();

	// Forget the cached answers, so the next Query asks the client again (the launcher's Rescan,
	// for someone who signed in to Steam after opening YAMP).
	void Refresh();

	// The cached answer for one app id; null when no available report covers it.
	const AppOwnership* Find(uint32_t appId);
}
