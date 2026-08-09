// GameLauncher.cpp
// The no-argument boot menu: discovers every arcade module YAMP can host and lets the user
// pick one. Discovery checks the folders the per-game LoadDLL implementations already accept
// (next to YAMP.exe / the documented subfolder) plus every game install root Verify::
// GameInstallRoots() knows about: each Steam library's steamapps/common/* (from the registry
// SteamPath + steamapps/libraryfolders.vdf, so renamed install folders still match by DLL
// layout), every GOG game from the registry, and — for installs no store knows about — each
// folder sitting beside YAMP.exe.
//
// "Play" relaunches YAMP.exe as a child process with the game's command-line switch and the
// working directory set to the discovered game folder, so the per-game boot paths (which
// resolve the DLL and its rom/sound assets relative to the CWD) run completely unchanged.

#include "GameLauncher.h"

#include "YAMPGeneral.h"
#include "GameVerify.h"
#include "RenderWindow.h"
#include "DebugLog.h"
#include "imgui/imgui.h"
#include "m2ftg/DisplayModes.h"

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
			// Lost Judgment does, so the candidate list already finds it — the two titles differ
			// only in which BUILD of the DLL is there, which the host resolves at load.
			{ YAMPGeneral::GameId::StF, "Sonic the Fighters", "Lost Judgment or Like a Dragon Gaiden", L"-stf",
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

		std::vector<FoundGame> DiscoverGames()
		{
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
			return games;
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

		void DrawLauncherUI(const std::vector<FoundGame>& games, int& selected, int& displayMode,
			bool& matchWindow, bool& playRequested, bool& rescanRequested, bool& quitRequested)
		{
			const ImVec2& displaySize = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos({ 0.0f, 0.0f });
			ImGui::SetNextWindowSize(displaySize);
			if (ImGui::Begin("##launcher", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoBringToFrontOnFocus))
			{
				ImGui::Text("Yakuza Arcade Machines Player");
				ImGui::TextDisabled("Select an arcade game. Games are located automatically: next to "
					"YAMP.exe, in any folder beside it, and in your Steam and GOG installs of the "
					"parent games.");
				ImGui::Separator();

				// Reserve room for the details block (path, source, checksum verdict, parent
				// game verdict — the wrapped ones can take two lines) + buttons below the table.
				// ... plus the resolution combo, which sits between the details block and the buttons.
				const float footerHeight = 7.0f * ImGui::GetTextLineHeightWithSpacing()
					+ 2.0f * ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 3.0f;
				if (ImGui::BeginTable("##games", 3,
					ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
					{ 0.0f, -footerHeight }))
				{
					// Status carries the longest strings now ("Lost Judgment not found",
					// "Unrecognised build"), so it gets a share to match.
					ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 0.40f);
					ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 0.32f);
					ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.28f);
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableHeadersRow();

					for (int i = 0; i < static_cast<int>(games.size()); i++)
					{
						const FoundGame& game = games[i];
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::PushID(i);
						if (ImGui::Selectable(game.info->name, selected == i,
							ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
						{
							selected = i;
							if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && game.CanPlay())
							{
								playRequested = true;
							}
						}
						ImGui::PopID();

						ImGui::TableSetColumnIndex(1);
						ImGui::TextUnformatted(game.info->parent);

						ImGui::TableSetColumnIndex(2);
						if (!game.found)
						{
							ImGui::TextDisabled("Not found");
						}
						else if (game.module.Blocks())
						{
							ImGui::TextColored({ 0.95f, 0.35f, 0.35f, 1.0f }, "%s",
								Verify::Describe(game.module.status));
						}
						else if (game.parent.Blocks())
						{
							// Name the parent game: "not found" on its own reads as if the
							// arcade module were missing, which is the one thing that IS present.
							ImGui::TextColored({ 0.95f, 0.35f, 0.35f, 1.0f }, "%s not found",
								game.info->parent);
						}
						else if (game.module.status == Verify::ModuleStatus::Verified)
						{
							ImGui::TextColored({ 0.3f, 0.9f, 0.3f, 1.0f }, "Verified");
						}
						else
						{
							// No checksum table for this game yet — found, but unverified.
							ImGui::TextColored({ 0.9f, 0.8f, 0.35f, 1.0f }, "Found");
						}
					}
					ImGui::EndTable();
				}

				if (selected >= 0 && selected < static_cast<int>(games.size()))
				{
					const FoundGame& game = games[selected];
					if (!game.found)
					{
						ImGui::TextWrapped("%s was not found. Install %s on Steam, or place the game "
							"files next to YAMP.exe.", game.info->name, game.info->parent);
					}
					else
					{
						const ImVec4 BAD { 0.95f, 0.35f, 0.35f, 1.0f };
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
								"Update the game through Steam.", game.info->parent);
							break;
						case Verify::ModuleStatus::Unreadable:
							ImGui::TextColored(BAD, "The module file could not be read.");
							break;
						case Verify::ModuleStatus::UnknownBuild:
							ImGui::TextColored(BAD, "Checksum mismatch - this is not a build of %s "
								"that YAMP supports. Restore the original DLL from your own copy of "
								"the game.", game.info->name);
							break;
						default:
							ImGui::TextDisabled("Checksum: no reference for this game yet.");
							break;
						}

						switch (game.parent.status)
						{
						case Verify::ParentStatus::Verified:
							ImGui::TextDisabled("%s: %s", game.info->parent, game.parent.buildLabel);
							break;
						case Verify::ParentStatus::UnknownBuild:
							ImGui::TextDisabled("%s: found, unrecognised version.", game.info->parent);
							break;
						case Verify::ParentStatus::NotFound:
							ImGui::TextColored(BAD, "No installation of %s was found. You must own "
								"it to play its arcade games.", game.info->parent);
							break;
						default:
							break;
						}
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
				ImGui::TextDisabled("Esc - exit, F1 - settings. Games can also be booted directly with "
					"-stf / -fv / -mr / -vf2 / -vf2-k2 / -von-k2 / -vf5fs / -vf5fs-lj / -vf5fs-ylad.");
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

		std::vector<FoundGame> games = DiscoverGames();

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
			DrawLauncherUI(games, selected, displayMode, matchWindow, playRequested, rescanRequested,
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
				games = DiscoverGames();
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
