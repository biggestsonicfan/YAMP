#pragma once

#include "YAMPGeneral.h"

// The one table of game identity (2026-08-09). Before it, the same facts were enumerated in
// four unrelated places that had to agree by hand: YAMPGeneral's four switches (arcade name /
// tag / parent name / low-res test), GameLauncher's GAMES[] literals, Main.cpp's hand-ordered
// wcsstr chain (with its five comments about prefix traps), and GameVerify's entry lists.
// YAMPGeneral and Main read this now; the launcher's literals and GameVerify's hash tables are
// the remaining consumers to converge.
namespace GameRegistry
{
	struct Entry
	{
		YAMPGeneral::GameId id;
		const char* tag;          // log/ini scoping tag ("StF", "VON-K2", ...)
		const char* arcadeName;   // the arcade game, as shown to the user
		const char* parentName;   // the Yakuza title that ships it
		const wchar_t* bootArg;   // the command-line switch; null = not directly bootable
		// "A low-resolution arcade board" - what the CRT filter and the 4:3 presentation key
		// off. True for every Model 2 AND Model 3 game (FV2/SRC2 render 496x384 like the
		// rest); false only for the modern-widescreen VF5FS builds. This is the test the
		// misleadingly-named YAMPGeneral::IsModel2ArcadeGame answers.
		bool lowResBoard;
	};

	const Entry* Entries(size_t& count);

	// Never null: an unknown id answers the VF5FS (Yakuza 6) row, preserving the old switch
	// defaults exactly.
	const Entry* Find(YAMPGeneral::GameId id);

	// The boot-argument parser: LONGEST bootArg that appears in the command line wins, which
	// is exactly what Main.cpp's ordered wcsstr chain enforced by hand ("-vf2-k2" before
	// "-vf2", "-fv2" before "-fv", "-mr-gaiden" before "-mr", "-vf5fs-*" before "-vf5fs").
	// Returns Launcher when no game switch is present.
	YAMPGeneral::GameId ParseBootArg(const wchar_t* cmdLine);
}
