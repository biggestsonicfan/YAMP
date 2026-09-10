// GameLauncher.cpp
// The no-argument boot menu: discovers every arcade module YAMP can host and lets the user
// pick one. Discovery checks the folders the per-game LoadDLL implementations already accept
// (next to YAMP.exe / the documented subfolder) plus every game install root Verify::
// GameInstallRoots() knows about: each Steam library's steamapps/common/* (from the registry
// SteamPath + steamapps/libraryfolders.vdf, so renamed install folders still match by DLL
// layout), every GOG game from the registry, and — for installs no store knows about — each
// folder sitting beside YAMP.exe.
//
// Ownership of the parent game is what gates Play, and it has two proofs: the Steam account
// signed in right now owns the title (SteamOwnership.h — no game files needed beyond the arcade
// module folder), or the title's executable is found and identified (GameVerify.h). Either one
// is enough.
//
// "Play" relaunches YAMP.exe as a child process with the game's command-line switch and the
// working directory set to the discovered game folder, so the per-game boot paths (which
// resolve the DLL and its rom/sound assets relative to the CWD) run completely unchanged.

#include "GameLauncher.h"

#include "YAMPGeneral.h"
#include "GameVerify.h"
#include "GameRegistry.h"
#include "SteamOwnership.h"
#include "RenderWindow.h"
#include "StringUtil.h"
#include "DebugLog.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // PushItemFlag(ImGuiItemFlags_NoNav) for the title rows
#include "m2ftg/DisplayModes.h"

#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Launcher
{
	namespace
	{
		struct DllCandidate
		{
			const wchar_t* dll;      // DLL path relative to a search root
			const wchar_t* bootDir;  // the CWD the game's LoadDLL expects, relative to the root
		};

		struct GameInfo
		{
			YAMPGeneral::GameId id;
			const char* name;
			const char* parent;
			const wchar_t* bootArg;
			const DllCandidate* candidates;
			size_t candidateCount;
		};

		// Layouts each host's LoadDLL accepts, plus the parent game's own install layout.
		// LJ keeps the m2ftg modules in runtime/media/m2ftg/ next to their rom/ + sound
		// assets (verified on a live Steam install); the LJ/Y6 hosts also probe a subfolder
		// of the CWD themselves, hence bootDir "." for the stf/fv/vf5fs subfolder shapes.
		constexpr DllCandidate STF_CANDIDATES[] = {
			{ L"stf-pxd-w64-d3d12_retail.dll", L"." },
			{ L"stf\\stf-pxd-w64-d3d12_retail.dll", L"." },
			{ L"runtime\\media\\m2ftg\\stf-pxd-w64-d3d12_retail.dll", L"runtime\\media\\m2ftg" },
		};
		constexpr DllCandidate FV_CANDIDATES[] = {
			{ L"fv-pxd-w64-d3d12_retail.dll", L"." },
			{ L"fv\\fv-pxd-w64-d3d12_retail.dll", L"." },
			{ L"runtime\\media\\m2ftg\\fv-pxd-w64-d3d12_retail.dll", L"runtime\\media\\m2ftg" },
		};
		constexpr DllCandidate MR_CANDIDATES[] = {
			{ L"mr-pxd-w64-d3d12_retail.dll", L"." },
			{ L"mr\\mr-pxd-w64-d3d12_retail.dll", L"." },
			{ L"runtime\\media\\m2ftg\\mr-pxd-w64-d3d12_retail.dll", L"runtime\\media\\m2ftg" },
		};
		constexpr DllCandidate VF2_CANDIDATES[] = {
			{ L"vf2\\vf2-pxd-w64-retail.dll", L"." },
			{ L"runtime\\media\\vf2\\vf2-pxd-w64-retail.dll", L"runtime\\media" },
		};
		constexpr DllCandidate VF5FS_CANDIDATES[] = {
			{ L"vf5fs-pxd-w64-Retail Steam.dll", L"." },
			{ L"vf5fs\\vf5fs-pxd-w64-Retail Steam.dll", L"." },
			{ L"media\\vf5fs-pxd-w64-Retail Steam.dll", L"media" },
		};
		// Lost Judgment's VF5FS: a DX12 build of the same game, in runtime/media/vf5fs next to
		// vf5fs_data.par + vf5fs_media, so the boot CWD is that folder itself.
		constexpr DllCandidate VF5FS_LJ_CANDIDATES[] = {
			{ L"vf5fs-pxd-w64-d3d12_retail.dll", L"." },
			{ L"vf5fs\\vf5fs-pxd-w64-d3d12_retail.dll", L"vf5fs" },
			{ L"runtime\\media\\vf5fs\\vf5fs-pxd-w64-d3d12_retail.dll", L"runtime\\media\\vf5fs" },
		};

		// Yakuza: Like a Dragon's own VF5FS, in runtime/media/vf5fs/ next to vf5fs_media/.
		constexpr DllCandidate VF5FS_YLAD_CANDIDATES[] = {
			{ L"vf5fs-pxd-w64-retail.dll", L"." },
			{ L"vf5fs\\vf5fs-pxd-w64-retail.dll", L"vf5fs" },
			{ L"runtime\\media\\vf5fs\\vf5fs-pxd-w64-retail.dll", L"runtime\\media\\vf5fs" },
		};

		// Yakuza Kiwami 2 (GOG) keeps both its modules in <install>/m2ftg/ next to rom/ and w64/.
		constexpr DllCandidate VF2_K2_CANDIDATES[] = {
			{ L"vf2-pxd-w64-gog_retail.dll", L"." },
			{ L"m2ftg\\vf2-pxd-w64-gog_retail.dll", L"m2ftg" },
		};

		// ...and "omg" = Operation Moon Gate, i.e. Virtual On, is the other one.
		constexpr DllCandidate VON_K2_CANDIDATES[] = {
			{ L"omg-pxd-w64-gog_retail.dll", L"." },
			{ L"m2ftg\\omg-pxd-w64-gog_retail.dll", L"m2ftg" },
		};

		// Like a Dragon Gaiden's Model 3 emulator sits in runtime/media/pre3/ beside the m2ftg
		// folder. Both Model 3 games come out of this ONE DLL, so the two entries below share a
		// candidate list and differ only in the switch they pass on.
		constexpr DllCandidate PRE3_CANDIDATES[] = {
			{ L"pre3-pxd-w64-d3d12_retail.dll", L"." },
			{ L"pre3\\pre3-pxd-w64-d3d12_retail.dll", L"pre3" },
			{ L"runtime\\media\\pre3\\pre3-pxd-w64-d3d12_retail.dll", L"runtime\\media\\pre3" },
		};

		constexpr GameInfo GAMES[] = {
			// Like a Dragon Gaiden ships its m2ftg modules at the same runtime/media/m2ftg path
			// Lost Judgment does, so one candidate list finds both — the titles differ only in
			// which BUILD of the DLL is there. That still needs TWO rows: one row naming both
			// titles cannot say which install it found, cannot check the right executable, and
			// (because the search below prefers the copy that verifies FOR THIS ENTRY) would let
			// one title's copy decide the verdict for the other. Same shape as Motor Raid below.
			{ YAMPGeneral::GameId::StF, "Sonic the Fighters", "Lost Judgment", L"-stf",
				STF_CANDIDATES, std::size(STF_CANDIDATES) },
			{ YAMPGeneral::GameId::StF_GAIDEN, "Sonic the Fighters", "Like a Dragon Gaiden", L"-stf-gaiden",
				STF_CANDIDATES, std::size(STF_CANDIDATES) },
			{ YAMPGeneral::GameId::FV, "Fighting Vipers", "Lost Judgment", L"-fv",
				FV_CANDIDATES, std::size(FV_CANDIDATES) },
			{ YAMPGeneral::GameId::MR, "Motor Raid", "Lost Judgment", L"-mr",
				MR_CANDIDATES, std::size(MR_CANDIDATES) },
			// Gaiden's own rebuild of the module, same file name at the same relative path — a
			// SEPARATE entry so each build verifies against its own table. Without it, whichever
			// install the search found first decided the one "Motor Raid" verdict, and a Gaiden
			// copy blocked the Lost Judgment one as an unknown build (the per-entry search below
			// prefers the copy that verifies, which is what actually tells them apart).
			{ YAMPGeneral::GameId::MR_GAIDEN, "Motor Raid", "Like a Dragon Gaiden", L"-mr-gaiden",
				MR_CANDIDATES, std::size(MR_CANDIDATES) },
			{ YAMPGeneral::GameId::VF2, "Virtua Fighter 2", "Yakuza: Like a Dragon", L"-vf2",
				VF2_CANDIDATES, std::size(VF2_CANDIDATES) },
			{ YAMPGeneral::GameId::VF5FS, "Virtua Fighter 5: Final Showdown", "Yakuza 6: The Song of Life", L"-vf5fs",
				VF5FS_CANDIDATES, std::size(VF5FS_CANDIDATES) },
			{ YAMPGeneral::GameId::VF5FS_LJ, "Virtua Fighter 5: Final Showdown", "Lost Judgment", L"-vf5fs-lj",
				VF5FS_LJ_CANDIDATES, std::size(VF5FS_LJ_CANDIDATES) },
			{ YAMPGeneral::GameId::VF2_K2, "Virtua Fighter 2", "Yakuza Kiwami 2", L"-vf2-k2",
				VF2_K2_CANDIDATES, std::size(VF2_K2_CANDIDATES) },
			{ YAMPGeneral::GameId::VON_K2, "Virtual On", "Yakuza Kiwami 2", L"-von-k2",
				VON_K2_CANDIDATES, std::size(VON_K2_CANDIDATES) },
			{ YAMPGeneral::GameId::VF5FS_YLAD, "Virtua Fighter 5: Final Showdown", "Yakuza: Like a Dragon",
				L"-vf5fs-ylad", VF5FS_YLAD_CANDIDATES, std::size(VF5FS_YLAD_CANDIDATES) },
			{ YAMPGeneral::GameId::FV2, "Fighting Vipers 2", "Like a Dragon Gaiden", L"-fv2",
				PRE3_CANDIDATES, std::size(PRE3_CANDIDATES) },
			{ YAMPGeneral::GameId::SRC2, "Sega Racing Classic 2", "Like a Dragon Gaiden", L"-src2",
				PRE3_CANDIDATES, std::size(PRE3_CANDIDATES) },
		};

		struct SearchRoot
		{
			fs::path path;
			std::string label;
		};

		struct FoundGame
		{
			const GameInfo* info = nullptr;
			bool found = false;
			fs::path dllPath;
			fs::path bootDir;
			std::string dllPathUtf8;
			std::string sourceLabel;
			// Filled in for every found module: the DLL's checksum verdict and whether the
			// parent game is installed. A blocking verdict here is the same one the game's own
			// boot path would hit, so the launcher shows it up front and refuses to Play.
			Verify::ModuleResult module;
			Verify::ParentResult parent;

			bool CanPlay() const { return found && !module.Blocks() && !parent.Blocks(); }
		};

		// One parent title and the games it supplies - the tree's top level. Ownership is a
		// property of the TITLE, not of any one module, so it is checked once here even when
		// none of the title's modules turned up: that is exactly the case where "you own it,
		// copy the module folder next to YAMP.exe" is the useful thing to say.
		struct ParentGroup
		{
			const char* name = nullptr;
			Verify::ParentResult ownership;
			std::vector<size_t> games;   // indices into Catalogue::games, in GAMES order
		};

		struct Catalogue
		{
			std::vector<FoundGame> games;
			std::vector<ParentGroup> parents;
		};

		// Stronger proof ranks higher. A row's own check can beat the title-level one (its
		// module-relative probes see folders the title-level search does not), never the reverse.
		int OwnershipRank(Verify::ParentStatus status)
		{
			switch (status)
			{
			case Verify::ParentStatus::Verified:     return 4;
			case Verify::ParentStatus::OwnedOnSteam: return 3;
			case Verify::ParentStatus::UnknownBuild: return 2;
			case Verify::ParentStatus::NotChecked:   return 1;
			default:                                 return 0;
			}
		}

		std::vector<SearchRoot> CollectSearchRoots()
		{
			std::vector<SearchRoot> roots;
			auto addRoot = [&roots](const fs::path& raw, std::string label) {
				std::error_code ec;
				fs::path p = fs::weakly_canonical(raw, ec);
				if (ec || p.empty()) p = raw;
				if (!fs::is_directory(p, ec) || ec) return;
				for (const SearchRoot& existing : roots)
				{
					if (existing.path == p) return;
				}
				roots.push_back({ std::move(p), std::move(label) });
			};

			wchar_t exePath[MAX_PATH];
			if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
			{
				addRoot(fs::path(exePath).parent_path(), "Next to YAMP.exe");
			}

			{
				const DWORD size = GetCurrentDirectoryW(0, nullptr);
				auto buf = std::make_unique<wchar_t[]>(size);
				if (GetCurrentDirectoryW(size, buf.get()) != 0)
				{
					addRoot(buf.get(), "Current folder");
				}
			}

			// Every installed game on the system, from every store YAMP knows (Steam libraries
			// and GOG). Matching by DLL layout rather than by folder name keeps renamed installs
			// discoverable, and the label already says which store it came from.
			for (Verify::InstallRoot& install : Verify::GameInstallRoots())
			{
				addRoot(install.path, std::move(install.label));
			}
			DebugLog("[launcher] %zu search roots\n", roots.size());
			for (const SearchRoot& root : roots)
			{
				DebugLog("[launcher]   root: %s\n", root.label.c_str());
			}
			return roots;
		}

		Catalogue DiscoverGames()
		{
			// Ask Steam afresh on every scan, so Rescan picks up an account that signed in after
			// the launcher opened. The connection is made once per scan, lazily, by the first
			// check below that needs it.
			Verify::RefreshSteamOwnership();

			const std::vector<SearchRoot> roots = CollectSearchRoots();

			std::vector<FoundGame> games;
			for (const GameInfo& info : GAMES)
			{
				FoundGame result;
				result.info = &info;
				// The nearest copy is only a FALLBACK: several games ship the same DLL file name
				// in more than one title (Motor Raid exists in Lost Judgment AND Like a Dragon
				// Gaiden at the same runtime/media path), so the search keeps looking until it
				// finds a copy whose build verifies FOR THIS ENTRY. A first-found copy that does
				// not verify is remembered and used only if nothing else does — the same rule
				// CheckParentGame applies to a second install of the parent title.
				for (const SearchRoot& root : roots)
				{
					bool foundHere = false;
					FoundGame candidate;
					candidate.info = &info;
					for (size_t c = 0; c < info.candidateCount && !foundHere; c++)
					{
						std::error_code ec;
						fs::path dll = root.path / info.candidates[c].dll;
						if (!fs::is_regular_file(dll, ec) || ec) continue;

						foundHere = true;
						candidate.found = true;
						candidate.dllPath = std::move(dll);
						candidate.bootDir = fs::weakly_canonical(root.path / info.candidates[c].bootDir, ec);
						if (ec || candidate.bootDir.empty()) candidate.bootDir = root.path;
						candidate.dllPathUtf8 = WcharToUTF8(candidate.dllPath.wstring());
						candidate.sourceLabel = root.label;
					}
					if (!foundHere) continue;

					// A few megabytes per module — cheap enough to hash every scan, so the
					// table always reflects what is on disk right now.
					candidate.module = Verify::CheckModule(info.id, candidate.dllPath);
					candidate.parent = Verify::CheckParentGame(info.id, candidate.dllPath.parent_path());
					const bool verifies = candidate.module.status == Verify::ModuleStatus::Verified
						|| candidate.module.status == Verify::ModuleStatus::NotChecked;
					if (!result.found || verifies)
					{
						result = std::move(candidate);
					}
					if (verifies) break;
				}
				DebugLog("[launcher] %s: %s (module %s, parent %s)\n", info.name,
					result.found ? result.dllPathUtf8.c_str() : "not found",
					Verify::Describe(result.module.status), Verify::Describe(result.parent.status));
				games.push_back(std::move(result));
			}

			// File each game under its title, in GAMES order, and settle each title's ownership:
			// the title-level check (no module folder to probe around) as the floor, raised by
			// any found module's own check when that saw more.
			Catalogue catalogue;
			catalogue.games = std::move(games);
			for (size_t i = 0; i < catalogue.games.size(); i++)
			{
				const FoundGame& game = catalogue.games[i];
				ParentGroup* group = nullptr;
				for (ParentGroup& existing : catalogue.parents)
				{
					if (strcmp(existing.name, game.info->parent) == 0)
					{
						group = &existing;
						break;
					}
				}
				if (group == nullptr)
				{
					catalogue.parents.emplace_back();
					group = &catalogue.parents.back();
					group->name = game.info->parent;
					group->ownership = Verify::CheckParentGame(game.info->id, {});
				}
				group->games.push_back(i);
				if (game.found && OwnershipRank(game.parent.status) > OwnershipRank(group->ownership.status))
				{
					group->ownership = game.parent;
				}
			}
			for (const ParentGroup& group : catalogue.parents)
			{
				DebugLog("[launcher] title %s: %s (%zu games)\n", group.name,
					Verify::Describe(group.ownership.status), group.games.size());
			}
			return catalogue;
		}

		// ---- Model 2 render resolution --------------------------------------------------
		// The module's own internal resolution, not YAMP's window: m2ftg::ModuleArgs feeds the
		// module's command-line option parser (which module_start otherwise calls with an empty argv)
		// and the emulator lays its viewport out at the size it picks. The launcher edits the same
		// [Graphics] Model2RenderMode key the in-game settings panel does, and the child process
		// reads it from the ini next to YAMP.exe - GetDataPath() resolves from the module path, not
		// the CWD, so the game folder the child runs in makes no difference.

		// VF5FS is not a Model 2 emulator - its module has no mode table for these switches.
		bool HasDisplayModes(YAMPGeneral::GameId id)
		{
			switch (id)
			{
			case YAMPGeneral::GameId::VF5FS:
			case YAMPGeneral::GameId::VF5FS_LJ:
			case YAMPGeneral::GameId::VF5FS_YLAD:
				return false;
			default:
				return true;
			}
		}

		std::filesystem::path LauncherIniPath()
		{
			wchar_t exePath[MAX_PATH];
			if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return {};
			return fs::path(exePath).parent_path() / L"settings.ini";
		}

		int LoadDisplayMode()
		{
			const std::filesystem::path ini = LauncherIniPath();
			if (ini.empty()) return 0;
			wchar_t buf[64] {};
			GetPrivateProfileStringW(L"Graphics", L"Model2RenderMode", L"", buf,
				static_cast<DWORD>(std::size(buf)), ini.c_str());
			return m2ftg::DisplayModeFromArg(buf);
		}

		bool LoadWindowMatchesRender()
		{
			const std::filesystem::path ini = LauncherIniPath();
			if (ini.empty()) return false;
			return GetPrivateProfileIntW(L"Graphics", L"Model2WindowMatchesRender", 0, ini.c_str()) != 0;
		}

		void SaveWindowMatchesRender(bool enabled)
		{
			const std::filesystem::path ini = LauncherIniPath();
			if (ini.empty()) return;
			WritePrivateProfileStringW(L"Graphics", L"Model2WindowMatchesRender",
				enabled ? L"1" : L"0", ini.c_str());
		}

		void SaveDisplayMode(int index)
		{
			const std::filesystem::path ini = LauncherIniPath();
			if (ini.empty() || index < 0 || index >= static_cast<int>(m2ftg::DISPLAY_MODE_COUNT)) return;
			WritePrivateProfileStringW(L"Graphics", L"Model2RenderMode",
				m2ftg::DISPLAY_MODES[index].arg, ini.c_str());
		}

		bool BootGame(const FoundGame& game)
		{
			wchar_t exePath[MAX_PATH];
			if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;

			std::wstring cmdLine = L"\"";
			cmdLine += exePath;
			cmdLine += L"\" ";
			cmdLine += game.info->bootArg;

			STARTUPINFOW si { sizeof(si) };
			PROCESS_INFORMATION pi {};
			if (!CreateProcessW(exePath, cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
				game.bootDir.c_str(), &si, &pi))
			{
				const std::wstring message = L"Failed to start " + UTF8ToWchar(game.info->name) + L"!";
				MessageBoxW(nullptr, message.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
				return false;
			}
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			return true;
		}

		// The Escape / Exit confirmation. A modal, so a stray click or Enter behind it cannot boot a
		// game while the prompt is up, and so it grabs nav focus for pad and keyboard users. NOTE:
		// this ImGui build deliberately does NOT close modals on Escape (imgui.cpp NavCancel only
		// closes non-modal popups), so cancelling with Escape is the caller's job - see Run().
		void DrawQuitPrompt(bool& promptOpen, bool& openPopup, bool& exitConfirmed)
		{
			if (openPopup)
			{
				ImGui::OpenPopup("Quit YAMP?");
				openPopup = false;
			}

			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
				{ 0.5f, 0.5f });
			if (ImGui::BeginPopupModal("Quit YAMP?", &promptOpen,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
			{
				ImGui::TextUnformatted("Do you really want to quit?");
				ImGui::Spacing();
				if (ImGui::Button("Quit", { 120.0f, 0.0f }))
				{
					exitConfirmed = true;
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel", { 120.0f, 0.0f }))
				{
					promptOpen = false;
					ImGui::CloseCurrentPopup();
				}
				// Cancel is where nav starts: the destructive button should never be one Enter away.
				ImGui::SetItemDefaultFocus();
				ImGui::EndPopup();
			}
		}

		// ---- The tree: one row per parent title, its games beneath ------------------------
		// The gate asks two independent questions, and the launcher used to squash both into one
		// "Status" cell, so "Lost Judgment not found" sat on a row whose module was the one thing
		// that WAS present. Now the title row answers "do you own it?" (Steam account, or an
		// installation found on disk) and each game row under it answers "is its module in one
		// of the search locations?". Read together they say what is missing and what to do.

		const ImVec4 GOOD { 0.3f, 0.9f, 0.3f, 1.0f };
		const ImVec4 WARN { 0.9f, 0.8f, 0.35f, 1.0f };
		const ImVec4 BAD { 0.95f, 0.35f, 0.35f, 1.0f };

		struct Verdict
		{
			const char* label;
			ImVec4 colour;
		};

		Verdict OwnershipVerdict(const Verify::ParentResult& parent)
		{
			const Steamworks::Report& steam = Steamworks::LastReport();
			switch (parent.status)
			{
			case Verify::ParentStatus::Verified:
				return { parent.steamOwned ? "Installed, owned on Steam" : "Installed", GOOD };
			case Verify::ParentStatus::OwnedOnSteam:
				return { "Owned on Steam", GOOD };
			case Verify::ParentStatus::UnknownBuild:
				return { "Installed, unrecognised version", WARN };
			case Verify::ParentStatus::NotFound:
				// "Not owned" only when Steam actually answered; otherwise all YAMP knows is
				// that it found nothing.
				return { steam.available ? "Not owned" : "Not found", BAD };
			default:
				return { "Not checked", ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) };
			}
		}

		Verdict ModuleVerdict(const FoundGame& game)
		{
			if (!game.found) return { "Not found", ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) };
			switch (game.module.status)
			{
			case Verify::ModuleStatus::Verified:      return { "Verified", GOOD };
			case Verify::ModuleStatus::NotChecked:    return { "Found, unverified", WARN };
			case Verify::ModuleStatus::OutdatedBuild: return { "Outdated build", BAD };
			case Verify::ModuleStatus::Unreadable:    return { "Unreadable", BAD };
			default:                                  return { "Wrong build", BAD };
			}
		}

		// What proved (or failed to prove) a title's ownership, for its "Where" cell.
		std::string OwnershipSource(const Verify::ParentResult& parent)
		{
			const Steamworks::Report& steam = Steamworks::LastReport();
			switch (parent.status)
			{
			case Verify::ParentStatus::Verified:
			case Verify::ParentStatus::UnknownBuild:
				return WcharToUTF8(parent.exePath.parent_path().wstring());
			case Verify::ParentStatus::OwnedOnSteam:
				return "Steam account " + steam.personaName;
			case Verify::ParentStatus::NotFound:
				return steam.available ? "not in " + steam.personaName + "'s Steam library, no installation found"
					: steam.failure;
			default:
				return "";
			}
		}

		// The folder inside the parent game's own install that holds this module, so the advice
		// can name it: the deepest candidate path is the as-shipped layout (runtime\media\m2ftg
		// for Lost Judgment), the shallower ones are the portable shapes.
		std::string ModuleFolderHint(const GameInfo& info)
		{
			fs::path best;
			size_t bestDepth = 0;
			for (size_t c = 0; c < info.candidateCount; c++)
			{
				const fs::path dll = info.candidates[c].dll;
				const size_t depth = static_cast<size_t>(std::distance(dll.begin(), dll.end()));
				if (depth > bestDepth)
				{
					bestDepth = depth;
					best = dll.parent_path();
				}
			}
			return WcharToUTF8(best.wstring());
		}

		const ParentGroup& GroupOf(const Catalogue& catalogue, const FoundGame& game)
		{
			for (const ParentGroup& group : catalogue.parents)
			{
				if (strcmp(group.name, game.info->parent) == 0) return group;
			}
			return catalogue.parents.front();   // unreachable: every game was filed under its title
		}

		// The one sentence that gets from the row's state to a playable one. Empty when there is
		// nothing to do, or when the module verdict line above it already said what to do.
		std::string WhatToDo(const FoundGame& game, const ParentGroup& group)
		{
			const Steamworks::Report& steam = Steamworks::LastReport();
			const std::string parent = game.info->parent;

			if (!game.found)
			{
				const std::string folder = ModuleFolderHint(*game.info);
				const bool owned = !group.ownership.Blocks()
					&& group.ownership.status != Verify::ParentStatus::NotChecked;
				if (owned)
				{
					return "You own " + parent + ". Copy its " + folder + " folder (the module DLL with its "
						"rom and sound files) next to YAMP.exe, or install the game, then Rescan.";
				}
				if (steam.available)
				{
					return "Install " + parent + ", or sign in to Steam with the account that owns it and "
						"put its " + folder + " folder next to YAMP.exe, then Rescan.";
				}
				return "Install " + parent + ", or start Steam, sign in with the account that owns it and "
					"put its " + folder + " folder next to YAMP.exe, then Rescan.";
			}
			if (game.module.Blocks()) return "";
			if (game.parent.Blocks())
			{
				if (steam.available)
				{
					return "Sign in to Steam with the account that owns " + parent + " (" + steam.personaName +
						" does not), or install " + parent + ", then Rescan.";
				}
				return "Start Steam and sign in with the account that owns " + parent + ", or install " +
					parent + ", then Rescan.";
			}
			return "";
		}

		void DrawLauncherUI(const Catalogue& catalogue, int& selected, int& displayMode,
			bool& matchWindow, bool& playRequested, bool& rescanRequested, bool& quitRequested)
		{
			const std::vector<FoundGame>& games = catalogue.games;
			const Steamworks::Report& steam = Steamworks::LastReport();

			const ImVec2& displaySize = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos({ 0.0f, 0.0f });
			ImGui::SetNextWindowSize(displaySize);
			if (ImGui::Begin("##launcher", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoBringToFrontOnFocus))
			{
				ImGui::Text("Yakuza Arcade Machines Player");
				// Steam first: it is the proof that needs no files, and "not running" is the one
				// failure the user can fix from right here with Rescan.
				if (steam.available)
				{
					ImGui::TextColored(GOOD, "Steam: signed in as %s", steam.personaName.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("- games this account owns play from just their module folder.");
				}
				else
				{
					ImGui::TextColored(WARN, "Steam: %s", steam.failure.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("- ownership is proven by finding each game on disk instead.");
				}
				ImGui::TextDisabled("Each title shows whether you own it. The games under it show whether "
					"their module was found: next to YAMP.exe, in a folder beside it, or in a Steam or "
					"GOG install.");
				ImGui::Separator();

				// Footer: the details block (name, module path, source, module verdict, ownership
				// verdict, advice - the wrapped ones can take two lines), the resolution combo and
				// its checkbox, then the buttons.
				const float footerHeight = 9.0f * ImGui::GetTextLineHeightWithSpacing()
					+ 2.0f * ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 3.0f;
				if (ImGui::BeginTable("##games", 3,
					ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
					{ 0.0f, -footerHeight }))
				{
					ImGui::TableSetupColumn("Title / game", ImGuiTableColumnFlags_WidthStretch, 0.38f);
					ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.26f);
					ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 0.36f);
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableHeadersRow();

					for (const ParentGroup& group : catalogue.parents)
					{
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						// Open by default: the tree exists to state ownership once per title, not
						// to hide the games. Title rows are MOUSE-ONLY: keyboard and pad navigation
						// moves between the game rows and never lands on a title, so ImGui's
						// nav-left ("collapse the focused node") cannot fold the tree. Seen on this
						// machine, where an idle analogue stick walks the nav cursor by itself and
						// had every title folded within ten seconds of opening the launcher.
						ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
						const bool open = ImGui::TreeNodeEx(group.name,
							ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth);
						ImGui::PopItemFlag();
						ImGui::TableSetColumnIndex(1);
						const Verdict ownership = OwnershipVerdict(group.ownership);
						ImGui::TextColored(ownership.colour, "%s", ownership.label);
						ImGui::TableSetColumnIndex(2);
						ImGui::TextDisabled("%s", OwnershipSource(group.ownership).c_str());
						if (!open) continue;

						for (const size_t index : group.games)
						{
							const FoundGame& game = games[index];
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							ImGui::PushID(static_cast<int>(index));
							if (ImGui::Selectable(game.info->name, selected == static_cast<int>(index),
								ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
							{
								selected = static_cast<int>(index);
								if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && game.CanPlay())
								{
									playRequested = true;
								}
							}
							ImGui::PopID();

							ImGui::TableSetColumnIndex(1);
							const Verdict module = ModuleVerdict(game);
							ImGui::TextColored(module.colour, "%s", module.label);

							ImGui::TableSetColumnIndex(2);
							ImGui::TextDisabled("%s", game.found ? game.sourceLabel.c_str() : "-");
						}
						ImGui::TreePop();
					}
					ImGui::EndTable();
				}

				if (selected >= 0 && selected < static_cast<int>(games.size()))
				{
					const FoundGame& game = games[selected];
					const ParentGroup& group = GroupOf(catalogue, game);

					ImGui::Text("%s", game.info->name);
					ImGui::SameLine();
					ImGui::TextDisabled("from %s", game.info->parent);

					if (!game.found)
					{
						ImGui::TextDisabled("Module: not found in any search location.");
					}
					else
					{
						ImGui::TextWrapped("Module: %s", game.dllPathUtf8.c_str());
						ImGui::TextDisabled("Located: %s", game.sourceLabel.c_str());

						switch (game.module.status)
						{
						case Verify::ModuleStatus::Verified:
							// Short form here; the full digest is in the About panel and the log.
							ImGui::TextDisabled("Checksum %.16s... matches %s",
								game.module.sha256.c_str(), game.module.buildLabel);
							break;
						case Verify::ModuleStatus::OutdatedBuild:
							ImGui::TextColored(BAD, "This module is from an older version of %s. "
								"Update the game through Steam or GOG, then Rescan.", game.info->parent);
							break;
						case Verify::ModuleStatus::Unreadable:
							ImGui::TextColored(BAD, "The module file could not be read. Check that nothing "
								"else has it open, then Rescan.");
							break;
						case Verify::ModuleStatus::UnknownBuild:
							ImGui::TextColored(BAD, "Checksum mismatch - this is not a build of %s "
								"that YAMP supports. Restore the original DLL from your own copy of "
								"the game, then Rescan.", game.info->name);
							break;
						default:
							ImGui::TextDisabled("Checksum: no reference for this game yet.");
							break;
						}

						// This copy's own ownership check - the one its boot repeats. Normally the
						// title row's verdict; it differs only when the executable turned up next
						// to this copy of the module and nowhere else.
						switch (game.parent.status)
						{
						case Verify::ParentStatus::Verified:
							ImGui::TextDisabled("%s: installed, %s%s", game.info->parent,
								game.parent.buildLabel, game.parent.steamOwned ? " (also owned on Steam)" : "");
							break;
						case Verify::ParentStatus::OwnedOnSteam:
							ImGui::TextDisabled("%s: owned on Steam by %s - no installation needed.",
								game.info->parent, steam.personaName.c_str());
							break;
						case Verify::ParentStatus::UnknownBuild:
							ImGui::TextDisabled("%s: installed, unrecognised version.", game.info->parent);
							break;
						case Verify::ParentStatus::NotFound:
							if (steam.available)
							{
								ImGui::TextColored(BAD, "%s: not installed, and not owned by %s on Steam.",
									game.info->parent, steam.personaName.c_str());
							}
							else
							{
								ImGui::TextColored(BAD, "%s: not installed, and Steam could not be asked (%s).",
									game.info->parent, steam.failure.c_str());
							}
							break;
						default:
							break;
						}
					}

					const std::string advice = WhatToDo(game, group);
					if (!advice.empty())
					{
						ImGui::PushStyleColor(ImGuiCol_Text, WARN);
						ImGui::TextWrapped("%s", advice.c_str());
						ImGui::PopStyleColor();
					}
					else if (game.CanPlay())
					{
						ImGui::TextColored(GOOD, "Ready to play.");
					}
				}

				ImGui::Spacing();

				// Model 2 resolution. This is the module's own option, not a YAMP scaler: it picks
				// the entry the emulator lays its viewport and 2D screen out at, so "Model 2 native"
				// is the arcade board's real 496x384 rather than the 1024x768 the module defaults to.
				const bool model2 = selected >= 0 && selected < static_cast<int>(games.size())
					&& HasDisplayModes(games[selected].info->id);
				if (!model2)
				{
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				}
				ImGui::SetNextItemWidth(260.0f);
				const char* preview = m2ftg::DISPLAY_MODES[displayMode].label;
				if (ImGui::BeginCombo("Model 2 render resolution", preview))
				{
					for (int i = 0; i < static_cast<int>(m2ftg::DISPLAY_MODE_COUNT); i++)
					{
						const bool isSelected = displayMode == i;
						if (ImGui::Selectable(m2ftg::DISPLAY_MODES[i].label, isSelected) && model2)
						{
							displayMode = i;
							SaveDisplayMode(i);
						}
						if (isSelected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				if (!model2)
				{
					ImGui::PopStyleVar();
					ImGui::SameLine();
					ImGui::TextDisabled("(Model 2 games only)");
				}

				if (ImGui::Checkbox("Match window to render resolution", &matchWindow) && model2)
				{
					SaveWindowMatchesRender(matchWindow);
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Sizes the window to the resolution above rather than the one in\n"
						"the settings, presented 1:1 with no letterboxing. Ignored in fullscreen.");
				}

				ImGui::Spacing();
				// The vendored ImGui predates BeginDisabled/EndDisabled; mirror ButtonToggleable's
				// dimming (YAMPUserInterface.cpp) for the disabled Play button.
				const bool canPlay = selected >= 0 && selected < static_cast<int>(games.size())
					&& games[selected].CanPlay();
				if (!canPlay)
				{
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
				}
				if (ImGui::Button("Play", { 120.0f, 0.0f }) && canPlay)
				{
					playRequested = true;
				}
				if (!canPlay)
				{
					ImGui::PopStyleVar();
				}
				ImGui::SameLine();
				if (ImGui::Button("Rescan"))
				{
					rescanRequested = true;
				}
				ImGui::SameLine();
				// A clickable way out, for the fullscreen case where the window is a borderless
				// WS_POPUP with no title bar to close (see RenderWindow's window styles). Escape
				// does the same thing; this one is also reachable by mouse and by pad, which the
				// launcher enables ImGui nav for. Both go through the confirmation prompt.
				if (ImGui::Button("Exit"))
				{
					quitRequested = true;
				}
				ImGui::SameLine();
				ImGui::TextDisabled("Esc - exit, F1 - settings.");

				// BUILT FROM THE REGISTRY, NOT TYPED OUT. The hand-written version of this line
				// listed nine of the thirteen switches - it was written when there were nine, and
				// -stf-gaiden, -mr-gaiden, -fv2 and -src2 arrived without anyone thinking to come
				// back to it. GameRegistry is the one table that has to be right for a switch to
				// work at all, so a row that boots is now a row that appears, and this cannot
				// drift again. Built once - the table is constexpr and never changes at runtime.
				static const std::string bootArgs = []
				{
					std::string list;
					size_t count = 0;
					const GameRegistry::Entry* entries = GameRegistry::Entries(count);
					for (size_t i = 0; i < count; ++i)
					{
						if (entries[i].bootArg == nullptr)
						{
							continue;   // the launcher's own row, which has no switch
						}
						if (!list.empty())
						{
							list += " / ";
						}
						list += WcharToUTF8(entries[i].bootArg);
					}
					return "Games can also be booted directly with " + list + ".";
				}();
				ImGui::TextDisabled("%s", bootArgs.c_str());
			}
			ImGui::End();
		}
	}

	bool Run(HINSTANCE instance, int cmdShow)
	{
		gGeneral.SetGameId(YAMPGeneral::GameId::Launcher);
		gGeneral.SetDLLName("None (launcher)");
		gGeneral.SetDLLTimestamp(0);
		gGeneral.SetDataPath();
		gGeneral.LoadSettings();

		Catalogue catalogue = DiscoverGames();
		// The same vector object survives a Rescan (the catalogue is assigned into, not replaced),
		// so this reference stays valid for the whole loop.
		std::vector<FoundGame>& games = catalogue.games;

		// Menu-only process: let the keyboard (and pad, via the Win32 backend) drive the UI.
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

		RenderWindow window(instance, instance, cmdShow);
		IDXGISwapChain* swapChain = window.GetSwapChain();

		// Start on the first game that can actually be played, falling back to the first one
		// that is merely present so a failed check is what the user sees first.
		int selected = 0;
		int displayMode = LoadDisplayMode();
		bool matchWindow = LoadWindowMatchesRender();
		for (size_t i = 0; i < games.size(); i++)
		{
			if (games[i].CanPlay())
			{
				selected = static_cast<int>(i);
				break;
			}
			if (games[i].found && !games[selected].found)
			{
				selected = static_cast<int>(i);
			}
		}

		bool booted = false;
		bool escWasDown = false;
		bool quitPromptOpen = false, quitPromptOpenPopup = false;
		while (!window.IsShuttingDown())
		{
			bool playRequested = false;
			bool rescanRequested = false;
			bool quitRequested = false;   // asks for the prompt...
			bool exitConfirmed = false;   // ...which is what actually ends the loop

			// Escape is how you leave the launcher. There is no game here to pause (what Escape
			// means in the hosts), and with the Fullscreen setting on the window is a borderless
			// WS_POPUP - no title bar, no close button, nothing to click - so without this the
			// process could only be killed. Three things it deliberately does NOT quit through:
			//   - the F1 settings window: Escape closes that first, so backing out of settings
			//     cannot drop the whole launcher.
			//   - the quit prompt itself: a second Escape cancels it (this ImGui build leaves
			//     Escape-on-modal to the caller, see DrawQuitPrompt).
			//   - any other open ImGui popup (the resolution combo, a settings modal): ImGui
			//     consumes Escape for those itself, and the key still reaches WndProc, so they are
			//     checked here too. The state read is last frame's, which is the frame the key
			//     went down on.
			const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
			if (escDown && !escWasDown)
			{
				if (window.GetUI().IsSettingsOpen())
				{
					window.GetUI().CloseSettings();
				}
				else if (quitPromptOpen)
				{
					quitPromptOpen = false;
				}
				else if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup))
				{
					quitRequested = true;
				}
			}
			escWasDown = escDown;

			window.BeginFrame();
			window.ClearBackbuffer();
			window.NewImGuiFrame();
			DrawLauncherUI(catalogue, selected, displayMode, matchWindow, playRequested, rescanRequested,
				quitRequested);
			if (quitRequested && !quitPromptOpen)
			{
				quitPromptOpen = quitPromptOpenPopup = true;
			}
			DrawQuitPrompt(quitPromptOpen, quitPromptOpenPopup, exitConfirmed);
			window.RenderImGui();
			window.EndFrame();
			if (FAILED(swapChain->Present(1, 0))) break;

			if (exitConfirmed) break;

			if (rescanRequested)
			{
				catalogue = DiscoverGames();
			}
			else if (playRequested && games[selected].CanPlay() && BootGame(games[selected]))
			{
				booted = true;
				break;
			}
		}
		return booted;
	}
}
