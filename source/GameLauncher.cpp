// GameLauncher.cpp
// The no-argument boot menu: discovers every arcade module YAMP can host and lets the user
// pick one. Discovery checks the folders the per-game LoadDLL implementations already accept
// (next to YAMP.exe / the documented subfolder) plus the parent games' own Steam installs —
// every library from the registry SteamPath + steamapps/libraryfolders.vdf, scanning all of
// steamapps/common so renamed install folders still match by DLL layout.
//
// "Play" relaunches YAMP.exe as a child process with the game's command-line switch and the
// working directory set to the discovered game folder, so the per-game boot paths (which
// resolve the DLL and its rom/sound assets relative to the CWD) run completely unchanged.

#include "GameLauncher.h"

#include "YAMPGeneral.h"
#include "RenderWindow.h"
#include "DebugLog.h"
#include "imgui/imgui.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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

		constexpr GameInfo GAMES[] = {
			{ YAMPGeneral::GameId::StF, "Sonic the Fighters", "Lost Judgment", L"-stf",
				STF_CANDIDATES, std::size(STF_CANDIDATES) },
			{ YAMPGeneral::GameId::FV, "Fighting Vipers", "Lost Judgment", L"-fv",
				FV_CANDIDATES, std::size(FV_CANDIDATES) },
			{ YAMPGeneral::GameId::MR, "Motor Raid", "Lost Judgment", L"-mr",
				MR_CANDIDATES, std::size(MR_CANDIDATES) },
			{ YAMPGeneral::GameId::VF2, "Virtua Fighter 2", "Yakuza: Like a Dragon", L"-vf2",
				VF2_CANDIDATES, std::size(VF2_CANDIDATES) },
			{ YAMPGeneral::GameId::VF5FS, "Virtua Fighter 5: Final Showdown", "Yakuza 6: The Song of Life", L"-vf5fs",
				VF5FS_CANDIDATES, std::size(VF5FS_CANDIDATES) },
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
		};

		// Steam base install + any secondary library folders from libraryfolders.vdf.
		std::vector<fs::path> SteamLibraryRoots()
		{
			std::vector<fs::path> bases;
			auto add = [&bases](const fs::path& raw) {
				std::error_code ec;
				fs::path p = fs::weakly_canonical(raw, ec);
				if (ec || p.empty()) p = raw;
				if (!fs::is_directory(p, ec) || ec) return;
				for (const fs::path& existing : bases)
				{
					if (existing == p) return;
				}
				bases.push_back(std::move(p));
			};

			wchar_t steamPath[512];
			DWORD steamPathSize = sizeof(steamPath);
			if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
				RRF_RT_REG_SZ, nullptr, steamPath, &steamPathSize) == ERROR_SUCCESS)
			{
				add(steamPath);
			}
			add(L"C:\\Program Files (x86)\\Steam");
			add(L"C:\\Program Files\\Steam");

			// Secondary libraries: every "path" value in each base's libraryfolders.vdf.
			const size_t primaryCount = bases.size();
			for (size_t i = 0; i < primaryCount; i++)
			{
				std::ifstream vdf(bases[i] / L"steamapps" / L"libraryfolders.vdf");
				if (!vdf) continue;
				std::stringstream buffer;
				buffer << vdf.rdbuf();
				const std::string text = buffer.str();

				size_t pos = 0;
				while ((pos = text.find("\"path\"", pos)) != std::string::npos)
				{
					pos += 6;
					const size_t open = text.find('"', pos);
					if (open == std::string::npos) break;
					const size_t close = text.find('"', open + 1);
					if (close == std::string::npos) break;

					std::string value = text.substr(open + 1, close - open - 1);
					std::string unescaped;
					unescaped.reserve(value.size());
					for (size_t j = 0; j < value.size(); j++)
					{
						if (value[j] == '\\' && j + 1 < value.size() && value[j + 1] == '\\')
						{
							unescaped += '\\';
							j++;
						}
						else
						{
							unescaped += value[j];
						}
					}
					add(fs::u8path(unescaped));
					pos = close + 1;
				}
			}
			return bases;
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

			// Every install under each Steam library's steamapps/common: matching by DLL
			// layout instead of by folder name keeps renamed installs discoverable.
			for (const fs::path& lib : SteamLibraryRoots())
			{
				std::error_code ec;
				for (const fs::directory_entry& entry : fs::directory_iterator(lib / L"steamapps" / L"common", ec))
				{
					std::error_code entryEc;
					if (!entry.is_directory(entryEc) || entryEc) continue;
					addRoot(entry.path(), "Steam: " + WcharToUTF8(entry.path().filename().wstring()));
				}
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
				for (const SearchRoot& root : roots)
				{
					for (size_t c = 0; c < info.candidateCount && !result.found; c++)
					{
						std::error_code ec;
						fs::path dll = root.path / info.candidates[c].dll;
						if (!fs::is_regular_file(dll, ec) || ec) continue;

						result.found = true;
						result.dllPath = std::move(dll);
						result.bootDir = fs::weakly_canonical(root.path / info.candidates[c].bootDir, ec);
						if (ec || result.bootDir.empty()) result.bootDir = root.path;
						result.dllPathUtf8 = WcharToUTF8(result.dllPath.wstring());
						result.sourceLabel = root.label;
					}
					if (result.found) break;
				}
				DebugLog("[launcher] %s: %s\n", info.name,
					result.found ? result.dllPathUtf8.c_str() : "not found");
				games.push_back(std::move(result));
			}
			return games;
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

		void DrawLauncherUI(const std::vector<FoundGame>& games, int& selected,
			bool& playRequested, bool& rescanRequested)
		{
			const ImVec2& displaySize = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowPos({ 0.0f, 0.0f });
			ImGui::SetNextWindowSize(displaySize);
			if (ImGui::Begin("##launcher", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
				ImGuiWindowFlags_NoBringToFrontOnFocus))
			{
				ImGui::Text("Yakuza Arcade Machines Player");
				ImGui::TextDisabled("Select an arcade game. Games are located automatically, next to "
					"YAMP.exe and in your Steam installs of the parent games.");
				ImGui::Separator();

				// Reserve room for the details block + buttons below the table.
				const float footerHeight = 4.0f * ImGui::GetTextLineHeightWithSpacing()
					+ ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
				if (ImGui::BeginTable("##games", 3,
					ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
					{ 0.0f, -footerHeight }))
				{
					ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 0.45f);
					ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 0.40f);
					ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 0.15f);
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
							if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && game.found)
							{
								playRequested = true;
							}
						}
						ImGui::PopID();

						ImGui::TableSetColumnIndex(1);
						ImGui::TextUnformatted(game.info->parent);

						ImGui::TableSetColumnIndex(2);
						if (game.found)
						{
							ImGui::TextColored({ 0.3f, 0.9f, 0.3f, 1.0f }, "Found");
						}
						else
						{
							ImGui::TextDisabled("Not found");
						}
					}
					ImGui::EndTable();
				}

				if (selected >= 0 && selected < static_cast<int>(games.size()))
				{
					const FoundGame& game = games[selected];
					if (game.found)
					{
						ImGui::TextWrapped("Module: %s", game.dllPathUtf8.c_str());
						ImGui::TextDisabled("Located: %s", game.sourceLabel.c_str());
					}
					else
					{
						ImGui::TextWrapped("%s was not found. Install %s on Steam, or place the game "
							"files next to YAMP.exe.", game.info->name, game.info->parent);
					}
				}

				ImGui::Spacing();
				// The vendored ImGui predates BeginDisabled/EndDisabled; mirror ButtonToggleable's
				// dimming (YAMPUserInterface.cpp) for the disabled Play button.
				const bool canPlay = selected >= 0 && selected < static_cast<int>(games.size())
					&& games[selected].found;
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
				ImGui::TextDisabled("F1 - settings. Games can also be booted directly with "
					"-stf / -fv / -vf2 / -vf5fs.");
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

		int selected = 0;
		for (size_t i = 0; i < games.size(); i++)
		{
			if (games[i].found)
			{
				selected = static_cast<int>(i);
				break;
			}
		}

		bool booted = false;
		while (!window.IsShuttingDown())
		{
			bool playRequested = false;
			bool rescanRequested = false;

			window.BeginFrame();
			window.ClearBackbuffer();
			window.NewImGuiFrame();
			DrawLauncherUI(games, selected, playRequested, rescanRequested);
			window.RenderImGui();
			window.EndFrame();
			if (FAILED(swapChain->Present(1, 0))) break;

			if (rescanRequested)
			{
				games = DiscoverGames();
			}
			else if (playRequested && games[selected].found && BootGame(games[selected]))
			{
				booted = true;
				break;
			}
		}
		return booted;
	}
}
