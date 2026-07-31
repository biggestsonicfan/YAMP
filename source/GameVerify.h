#pragma once

// GameVerify — file integrity and parent-game ownership checks.
//
// Two separate questions, answered with two different (deliberately different) methods:
//
//  * "Is this arcade module DLL one YAMP can actually host?"  -> full SHA-256 of the file,
//    matched against the KNOWN_MODULES table below. Every host resolves symbols by byte
//    pattern and hard-coded RVAs, so a DLL from a different build silently mis-patches
//    instead of failing cleanly; an exact hash is the only honest answer. A DLL that does
//    not match is BLOCKED before LoadLibrary ever runs its DllMain.
//
//  * "Does the user own the parent game?"  -> PE header identity of the parent executable
//    (LostJudgment.exe and friends). Those are hundreds of megabytes, so hashing them would
//    add seconds to every boot for no extra certainty: the MZ/PE magic, TimeDateStamp,
//    SizeOfImage and file size together already pin an exact retail build, and cost a couple
//    of header reads. A missing parent game blocks; a parent game of an unrecognised build
//    only warns, since the arcade DLL is the file compatibility actually depends on.
//
// Games with no table entry yet (VF2 from Yakuza: Like a Dragon, VF5FS from Yakuza 6) report
// NotChecked and are not gated — their hashes get added as those paths are revisited.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "YAMPGeneral.h"

namespace Verify
{
	enum class ModuleStatus
	{
		Verified,        // SHA-256 matches a known-good build
		UnknownBuild,    // readable, but not a build YAMP knows -> blocked
		OutdatedBuild,   // a build YAMP explicitly cannot host (pre-update DLL) -> blocked
		Unreadable,      // missing, locked, or not a PE file at all -> blocked
		NotChecked,      // no table entry for this game yet -> not gated
	};

	enum class ParentStatus
	{
		Verified,        // parent executable found, PE identity matches a known build
		UnknownBuild,    // parent executable found, but a build we have no identity for -> warns
		NotFound,        // no parent executable anywhere we looked -> blocked
		NotChecked,      // no table entry for this game yet -> not gated
	};

	struct ModuleResult
	{
		ModuleStatus status = ModuleStatus::NotChecked;
		std::string sha256;             // uppercase hex of the file, empty when unreadable
		uint64_t size = 0;
		uint32_t timestamp = 0;         // PE FileHeader.TimeDateStamp
		const char* buildLabel = nullptr;   // known build name, null when unrecognised
		const char* expectedSha256 = nullptr;  // what a good file of this game hashes to

		bool Blocks() const { return status != ModuleStatus::Verified && status != ModuleStatus::NotChecked; }
	};

	struct ParentResult
	{
		ParentStatus status = ParentStatus::NotChecked;
		std::filesystem::path exePath;      // where it was found, empty when NotFound
		const char* exeName = nullptr;      // the executable we looked for ("LostJudgment.exe")
		const char* buildLabel = nullptr;   // known build name, null when unrecognised

		bool Blocks() const { return status == ParentStatus::NotFound; }
	};

	// Hash + identify an arcade module DLL. Reads the file; does not load it.
	ModuleResult CheckModule(YAMPGeneral::GameId id, const std::filesystem::path& dllPath);

	// Locate and identify the parent game's executable. Searches, in order: the DLL's own
	// folder and its two parents (LJ ships the modules in runtime/media/<module>/, next to
	// runtime/media/LostJudgment.exe), the YAMP.exe folder and the CWD, then every Steam
	// install found through the registry + libraryfolders.vdf.
	ParentResult CheckParentGame(YAMPGeneral::GameId id, const std::filesystem::path& dllDir);

	// The gate every host runs before LoadLibrary: checks the module, then the parent game,
	// stores both as the "last" results for the About panel, and shows an explanatory message
	// box for whichever one blocks. Returns false when the DLL must not be loaded.
	bool CheckBeforeLoad(YAMPGeneral::GameId id, const std::filesystem::path& dllPath);

	// What CheckBeforeLoad last found — read by the About panel (YAMPUserInterface.cpp).
	const ModuleResult& LastModuleResult();
	const ParentResult& LastParentResult();

	// Short human-readable verdicts for the launcher table and the About panel.
	const char* Describe(ModuleStatus status);
	const char* Describe(ParentStatus status);

	// Steam base install + every secondary library from libraryfolders.vdf.
	std::vector<std::filesystem::path> SteamLibraryRoots();

	// One installed game, from whichever store put it there.
	struct InstallRoot
	{
		std::filesystem::path path;
		std::string label;   // "Steam: <folder>" / "GOG: <game name>" / "Next to YAMP: <folder>",
		                     // for the launcher's UI
	};

	// EVERY game install directory on this system: each Steam library's steamapps/common/*, every
	// GOG game from the registry, and — for installs no store knows about — YAMP.exe's own folder
	// plus each folder sitting beside it (one level, directories only). Both the launcher's game
	// discovery and the parent-game ownership search run off this, so a source that is missing here
	// makes its games both undiscoverable and unverifiable.
	std::vector<InstallRoot> GameInstallRoots();
}
