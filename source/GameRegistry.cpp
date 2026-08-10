#include "GameRegistry.h"

#include <cstring>

namespace GameRegistry
{
	// One row per game. Sonic the Fighters is the one arcade game shipping in more than one
	// title, and owning either is enough - so its parent must not tell a Like a Dragon Gaiden
	// owner they need Lost Judgment; which build actually loaded is reported separately, by
	// the module verdict.
	static constexpr Entry ENTRIES[] = {
		{ YAMPGeneral::GameId::StF,        "StF",        "Sonic the Fighters",               "Lost Judgment or Like a Dragon Gaiden", L"-stf",        true  },
		{ YAMPGeneral::GameId::FV,         "FV",         "Fighting Vipers",                  "Lost Judgment",                         L"-fv",         true  },
		{ YAMPGeneral::GameId::MR,         "MR",         "Motor Raid",                       "Lost Judgment",                         L"-mr",         true  },
		{ YAMPGeneral::GameId::MR_GAIDEN,  "MR-Gaiden",  "Motor Raid",                       "Like a Dragon Gaiden",                  L"-mr-gaiden",  true  },
		{ YAMPGeneral::GameId::VF2,        "VF2",        "Virtua Fighter 2",                 "Yakuza: Like a Dragon",                 L"-vf2",        true  },
		{ YAMPGeneral::GameId::VF2_K2,     "VF2-K2",     "Virtua Fighter 2",                 "Yakuza Kiwami 2",                       L"-vf2-k2",     true  },
		{ YAMPGeneral::GameId::VON_K2,     "VON-K2",     "Virtual On",                       "Yakuza Kiwami 2",                       L"-von-k2",     true  },
		{ YAMPGeneral::GameId::FV2,        "FV2",        "Fighting Vipers 2",                "Like a Dragon Gaiden",                  L"-fv2",        true  },
		{ YAMPGeneral::GameId::SRC2,       "SRC2",       "Sega Racing Classic 2",            "Like a Dragon Gaiden",                  L"-src2",       true  },
		{ YAMPGeneral::GameId::VF5FS,      "VF5FS",      "Virtua Fighter 5: Final Showdown", "Yakuza 6: The Song of Life",            L"-vf5fs",      false },
		{ YAMPGeneral::GameId::VF5FS_LJ,   "VF5FS-LJ",   "Virtua Fighter 5: Final Showdown", "Lost Judgment",                         L"-vf5fs-lj",   false },
		{ YAMPGeneral::GameId::VF5FS_YLAD, "VF5FS-YLAD", "Virtua Fighter 5: Final Showdown", "Yakuza: Like a Dragon",                 L"-vf5fs-ylad", false },
		{ YAMPGeneral::GameId::Launcher,   "YAMP",       "Yakuza Arcade Machines Player",    "the supported Yakuza games",            nullptr,        false },
	};

	const Entry* Entries(size_t& count)
	{
		count = std::size(ENTRIES);
		return ENTRIES;
	}

	const Entry* Find(YAMPGeneral::GameId id)
	{
		for (const Entry& e : ENTRIES)
		{
			if (e.id == id)
			{
				return &e;
			}
		}
		// The old switches all defaulted to the VF5FS (Yakuza 6) answers; keep that exactly.
		for (const Entry& e : ENTRIES)
		{
			if (e.id == YAMPGeneral::GameId::VF5FS)
			{
				return &e;
			}
		}
		return &ENTRIES[0];   // unreachable; ENTRIES always carries a VF5FS row
	}

	YAMPGeneral::GameId ParseBootArg(const wchar_t* cmdLine)
	{
		const Entry* best = nullptr;
		size_t bestLen = 0;
		for (const Entry& e : ENTRIES)
		{
			if (e.bootArg == nullptr || wcsstr(cmdLine, e.bootArg) == nullptr)
			{
				continue;
			}
			const size_t len = wcslen(e.bootArg);
			if (len > bestLen)
			{
				best = &e;
				bestLen = len;
			}
		}
		return best != nullptr ? best->id : YAMPGeneral::GameId::Launcher;
	}
}
