#include "YAMPUserInterface.h"

#include "m2ftg/DisplayModes.h"

#include "YAMPGeneral.h"
#include "GameVerify.h"
#include "m2ftg/ELF/ElfRom.h"
#include "m2ftg/LJ/DebugWindows.h"
#include "m2ftg/LJ/HleHooks.h"
#include "m2ftg/LJ/LJHost.h"

#include "net/NetPlugin.h"

#include "imgui/imgui.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cstdlib>

static const ImVec4 WARNING_COLOUR { 1.000f, 1.000f, 0.000f, 1.000f };

// The three Lost Judgment m2ftg modules share one hosting path, one settings section and one
// input layer, so they also share the Game/Controls panels.
static bool IsLJm2ftgGame()
{
	const auto id = gGeneral.GetGameId();
	return id == YAMPGeneral::GameId::StF
		|| id == YAMPGeneral::GameId::FV
		|| id == YAMPGeneral::GameId::MR;
}

// ...as do the Yakuza Kiwami 2 modules, which share the K2 host the same way. Kiwami 2 ships two
// (Virtua Fighter 2 and Virtua On), so this is deliberately a per-PARENT-GAME test rather than a
// per-game one — the second module needs nothing here beyond its GameId.
static bool IsKiwami2Game()
{
	return gGeneral.GetGameId() == YAMPGeneral::GameId::VF2_K2;
}

static bool IsM2ftgGame()
{
	return IsLJm2ftgGame() || IsKiwami2Game()
		|| gGeneral.GetGameId() == YAMPGeneral::GameId::VF2;
}

// Netplay hangs off the LJ m2ftg host loop, but holding two emulators in lockstep needs the
// determinism work as well (board reset, host-RNG seeding, the texture-budget pin) — and all
// three of those are reconstructed from StF's DLL. So StF is the only game that can currently
// sustain a session, and it is the only one that gets the page.
static bool IsNetplayGame()
{
	return gGeneral.GetGameId() == YAMPGeneral::GameId::StF;
}

// The Virtua Fighter 2 MODULE, in either of the two parent games that ship it. Separate from the
// test above because m2ftg_config_t's is_vf20 / is_disable_pepsi are VF2's own switches — they
// mean nothing to Virtua On, StF, FV or MR.
static bool IsVF2Module()
{
	const auto id = gGeneral.GetGameId();
	return id == YAMPGeneral::GameId::VF2 || id == YAMPGeneral::GameId::VF2_K2;
}

// "Custom" ImGui wrappers
namespace ImGuiCustom
{
	bool ButtonToggleable(const char* label, const bool enabled, const ImVec2& size = { 0, 0 })
	{
		bool result = false;
		if (!enabled)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
		}

		result = ImGui::Button(label, size);

		if (!enabled)
		{
			ImGui::PopStyleVar();
		}
		return enabled ? result : false;
	}

}

#define STRINGIZE(s) STRINGIZE2(s)
#define STRINGIZE2(s) #s

#define WIDEN(x) WIDEN2(x)
#define WIDEN2(x) L ##x

void YAMPUserInterface::Draw()
{
	// In the launcher no game is selected yet: the disclaimer names the parent game, so it
	// stays with the per-game boots (each game's own settings file tracks it separately).
	const bool launcherMode = gGeneral.GetGameId() == YAMPGeneral::GameId::Launcher;

	if (!launcherMode && gGeneral.GetSettings()->m_buildLastShowedDisclaimer < rsc_RevisionID)
	{
		DrawDisclaimer();
	}

	// The game DLL's own debug windows are independent of the F1 settings window.
	//
	// HIDDEN DURING NETPLAY: the reconstructed DEBUG MENU drives the DLL's own handlers, and one
	// of them is a REAL board reset (DLL+0x4C840 - the same call the round-start sequence uses).
	// Running it on one machine mid-match re-initialises that i960 and nothing else, which is an
	// instant, unrecoverable desync. Single-stepping and the other CPU controls are the same class
	// of hazard, so the whole set goes away while a session is up rather than being audited item
	// by item.
	if (!net::SessionInProgress())
	{
		m2ftg::DrawDebugWindows();
	}
	m2ftg::UpdateGameDebugFlag();
	m2ftg::HleHooks::Update();

	// Likewise the netplay status: it has to be visible while the settings window is CLOSED,
	// which is where a session spends all of its time once it is running.
	DrawNetplayOverlay();

	if (!ProcessF1Key())
	{
		return;
	}

	// Center the window on first appearance AND whenever the display size changes (window
	// resize / resolution change), so it never ends up off-center or off-screen.
	const ImVec2& displaySize = ImGui::GetIO().DisplaySize;
	ImGuiCond posCond = ImGuiCond_Appearing;
	if (displaySize.x != m_lastDisplayW || displaySize.y != m_lastDisplayH)
	{
		m_lastDisplayW = displaySize.x;
		m_lastDisplayH = displaySize.y;
		posCond = ImGuiCond_Always;
	}
	ImGui::SetNextWindowPos({ displaySize.x / 2.0f, displaySize.y / 2.0f }, posCond, { 0.5f, 0.5f });
	ImGui::SetNextWindowSize({ 600, 600 }, ImGuiCond_Once);

	if (ImGui::Begin("YAMP Settings", &m_settingsOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
	{
		// Helper variables
		int graphics_id, game_id, debug_id, about_id, controls_id, netplay_id;

		static int selectedTab = 0;
		static int delayedSelectedTab = 0; // For confirmation
		ImGui::BeginChild("##left", { 150, 0 }, true);
		{
			auto settingsSection = [this](const char* label, int index, bool confirm)
			{
				const bool selected = selectedTab == index;
				if (ImGui::Selectable(label, selected) && !selected)
				{
					if (!confirm)
					{
						selectedTab = index;
					}
					else
					{
						delayedSelectedTab = index;
						ImGui::OpenPopup("YAMP Settings##save");
					}
				}
				return index;
			};

			// The launcher hosts no game, so only the Graphics + About pages apply there.
			int index = 0;
			game_id = controls_id = debug_id = netplay_id = -1;
			if (!launcherMode) game_id = settingsSection("Game", index++, m_pageModified);
			graphics_id = settingsSection("Graphics", index++, m_pageModified);
			if (!launcherMode) controls_id = settingsSection("Controls", index++, m_pageModified);
			// Only the game that can actually hold a session gets the page — a Netplay tab on a
			// title with no netcode behind it would be a promise YAMP cannot keep.
			if (!launcherMode && IsNetplayGame()) netplay_id = settingsSection("Netplay", index++, m_pageModified);
			if (!launcherMode) debug_id = settingsSection("Debug", index++, m_pageModified);
			about_id = settingsSection("About", index++, m_pageModified);

			if (DrawSettingsConfirmation())
			{
				selectedTab = delayedSelectedTab;
			}

			ImGui::EndChild();
		}

		ImGui::SameLine();

		ImGui::BeginGroup();
		// About is informational; every hosted game's Controls page is now the editable
		// binding editor (see DrawControls), so it needs the Apply/Cancel flow too.
		const bool controlsEditable = gGeneral.GetGameId() != YAMPGeneral::GameId::Launcher;
		bool drawButtons = selectedTab != about_id && (selectedTab != controls_id || controlsEditable);

		float rightPanelHeight = 0.0f;
		if (drawButtons)
		{
			rightPanelHeight -= ImGui::GetFrameHeightWithSpacing();
			if (m_showRestartWarning)
			{
				rightPanelHeight -= ImGui::GetTextLineHeightWithSpacing() + ImGui::GetTextLineHeight();
			}
		}

		ImGui::BeginChild("##right", { 0, rightPanelHeight });
		{
			if (selectedTab == game_id) DrawGame();
			else if (selectedTab == graphics_id) DrawGraphics();
			else if (selectedTab == controls_id) DrawControls();
			else if (selectedTab == netplay_id) DrawNetplay();
			else if (selectedTab == debug_id) DrawDebug();
			else if (selectedTab == about_id) DrawAbout();
		}
		ImGui::EndChild();

		if (drawButtons)
		{
			if (m_showRestartWarning)
			{
				ImGui::PushTextWrapPos();
				ImGui::TextColored(WARNING_COLOUR, "YAMP needs to be restarted for the new settings to take effect.");
				ImGui::PopTextWrapPos();
			}

			if (ImGuiCustom::ButtonToggleable("Apply", m_pageModified))
			{
				ApplySettings();
			}

			ImGui::SameLine();
			if (ImGuiCustom::ButtonToggleable("Cancel", m_pageModified))
			{
				DiscardSettings();
			}
		}
		ImGui::EndGroup();

	}
	ImGui::End();
}

void YAMPUserInterface::AddResolution(uint32_t width, uint32_t height, float refreshRate)
{
	auto it = std::lower_bound(m_resolutions.begin(), m_resolutions.end(), std::make_pair(width, height), [](const Resolution& res, const auto& curr) {
		return std::make_pair(res.width, res.height) < curr;
		});
	if (it != m_resolutions.end() && it->width == width && it->height == height)
	{
	}
	else
	{
		char res[64];
		sprintf_s(res, "%u x %u", width, height);
		it = m_resolutions.insert(it, { width, height, res });
	}

	char refRate[64];
	sprintf_s(refRate, "%.2f Hz", refreshRate);
	it->refreshRates.push_back({ refreshRate, refRate });
}

void YAMPUserInterface::GetDefaultsFromSettings()
{
	const YAMPSettings* settings = gGeneral.GetSettings();

	auto res = std::find_if(m_resolutions.begin(), m_resolutions.end(), [settings](const auto& e) {
		return e.width == settings->m_resX && e.height == settings->m_resY;
		});
	if (res != m_resolutions.end())
	{
		m_currentResolutionIndex = std::distance(m_resolutions.begin(), res);
		auto refRate = std::find_if(res->refreshRates.begin(), res->refreshRates.end(), [settings](const auto& e) {
			return e.refreshRate == settings->m_refreshRate;
			});

		if (refRate != res->refreshRates.end())
		{
			m_currentRefRateIndex = std::distance(res->refreshRates.begin(), refRate);
		}
	}

	m_currentFullscreen = settings->m_fullscreen;
	m_enableFpsCap = settings->m_enableFpsCap;

	m_arcadeMode = settings->m_arcadeMode;
	m_circleConfirm = settings->m_circleConfirm;
	m_language = settings->m_language;
	m_volumePercent = static_cast<int>(settings->m_volumePercent);

	m_m2RenderMode = settings->m_m2RenderMode;
	m_m2WindowMatchesRender = settings->m_m2WindowMatchesRender;
	m_m2Aspect = settings->m_m2Aspect;
	m_m2CrtFilter = settings->m_m2CrtFilter;
	m_m2Difficulty = settings->m_m2Difficulty;
	m_m2Country = settings->m_m2Country;
	m_m2Freeplay = settings->m_m2Freeplay;
	m_m2VersusMode = settings->m_m2VersusMode;
	m_m2RealDamage = settings->m_m2RealDamage;
	m_m2KeyBinds = settings->m_m2KeyBinds;
	m_m2PadBinds = settings->m_m2PadBinds;
	for (int player = 0; player < 2; player++)
	{
		m_m2PadId[player] = settings->m_m2PadId[player];
	}

	m_vf2Version20 = settings->m_vf2Version20;
	m_vf2DisablePepsi = settings->m_vf2DisablePepsi;

	// Netplay: settings hold std::strings, the page edits fixed buffers (no std::string InputText
	// in this ImGui build), so the two are copied across at the page's edges.
	auto copyToBuffer = [](char* dst, size_t cap, const std::string& src)
	{
		strncpy_s(dst, cap, src.c_str(), _TRUNCATE);
	};
	copyToBuffer(m_netServer, sizeof(m_netServer), settings->m_netServer);
	copyToBuffer(m_netNpid, sizeof(m_netNpid), settings->m_netNpid);
	copyToBuffer(m_netToken, sizeof(m_netToken), settings->m_netToken);
	copyToBuffer(m_netFingerprint, sizeof(m_netFingerprint), settings->m_netCertFingerprint);
	copyToBuffer(m_netComId, sizeof(m_netComId), settings->m_netComId);
	m_netFrameDelay = settings->m_netFrameDelay;

	m_dontApplyPatches = settings->m_dontApplyPatches;
	m_useD3DDebugLayer = settings->m_useD3DDebugLayer;
	m_stfShowDebugFeatures = settings->m_stfShowDebugFeatures;
	m_stfLooseRomFiles = settings->m_stfLooseRomFiles;
	m_stfGameDebugFlag = settings->m_stfGameDebugFlag;
	m_stfFixBackupTimeIndex = settings->m_stfFixBackupTimeIndex;
	m_stfHleDisableMask[0] = settings->m_stfHleDisableMask[0];
	m_stfHleDisableMask[1] = settings->m_stfHleDisableMask[1];

	// In case non-default Debug options are present, don't nag about the consequences of Debug options for this session.
	// The hook mask is compared against its DEFAULT, not against zero: hook 16 is disabled out of
	// the box (HleHooks.h), and that alone must not read as "the user has been poking at things".
	if (m_dontApplyPatches || m_useD3DDebugLayer || m_stfShowDebugFeatures || m_stfLooseRomFiles || m_stfGameDebugFlag ||
		!m_stfFixBackupTimeIndex ||
		m_stfHleDisableMask[0] != m2ftg::HleHooks::DEFAULT_DISABLE_MASK[0] ||
		m_stfHleDisableMask[1] != m2ftg::HleHooks::DEFAULT_DISABLE_MASK[1])
	{
		m_debugInfoAccepted.reset();
	}
}

void YAMPUserInterface::DrawGraphics()
{
	ImGuiStyle& style = ImGui::GetStyle();
	float w = ImGui::CalcItemWidth();
	float spacing = style.ItemInnerSpacing.x;
	float button_sz = ImGui::GetFrameHeight();

	const auto& currentRes = m_resolutions[m_currentResolutionIndex];
	{
		ImGui::PushItemWidth(w - spacing * 2.0f - button_sz * 2.0f);

		if (ImGui::BeginCombo("##resolutions", currentRes.displayString.c_str(), ImGuiComboFlags_NoArrowButton))
		{
			size_t index = 0;
			for (const auto& it : m_resolutions)
			{
				const bool isSelected = index == m_currentResolutionIndex;
				if (ImGui::Selectable(it.displayString.c_str(), isSelected))
				{
					m_pageModified = true;
					m_currentResolutionIndex = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
				++index;
			}

			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();
		ImGui::SameLine(0, spacing);
		if (ImGui::ArrowButton("##resolutions##l", ImGuiDir_Left))
		{
			m_pageModified = true;
			if (m_currentResolutionIndex > 0) m_currentResolutionIndex--;
			else m_currentResolutionIndex = m_resolutions.size() - 1;
		}
		ImGui::SameLine(0, spacing);
		if (ImGui::ArrowButton("##resolutions##r", ImGuiDir_Right))
		{
			m_pageModified = true;
			if (++m_currentResolutionIndex >= m_resolutions.size())
				m_currentResolutionIndex = 0;
		}
		ImGui::SameLine(0, spacing);
		ImGui::Text("Resolution");
	}
	{
		ImGui::PushItemWidth(w - spacing * 2.0f - button_sz * 2.0f);

		if (m_currentRefRateIndex >= currentRes.refreshRates.size())
		{
			m_currentRefRateIndex = 0;
		}
		if (ImGui::BeginCombo("##refrates", currentRes.refreshRates[m_currentRefRateIndex].displayString.c_str(), ImGuiComboFlags_NoArrowButton))
		{
			size_t index = 0;
			for (const auto& it : currentRes.refreshRates)
			{
				const bool isSelected = index == m_currentRefRateIndex;
				if (ImGui::Selectable(it.displayString.c_str(), isSelected))
				{
					m_pageModified = true;
					m_currentRefRateIndex = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
				++index;
			}

			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();
		ImGui::SameLine(0, spacing);
		if (ImGui::ArrowButton("##refrates##l", ImGuiDir_Left))
		{
			m_pageModified = true;
			if (m_currentRefRateIndex > 0) m_currentRefRateIndex--;
			else m_currentRefRateIndex = currentRes.refreshRates.size() - 1;
		}
		ImGui::SameLine(0, spacing);
		if (ImGui::ArrowButton("##refrates##r", ImGuiDir_Right))
		{
			m_pageModified = true;
			if (++m_currentRefRateIndex >= currentRes.refreshRates.size())
				m_currentRefRateIndex = 0;
		}
		ImGui::SameLine(0, spacing);
		ImGui::Text("Refresh Rate");

	}

	if (ImGui::Checkbox("Fullscreen", &m_currentFullscreen))
	{
		m_pageModified = true;
	}
	if (ImGui::Checkbox("Enable 60 FPS cap", &m_enableFpsCap))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Gameplay bugs may occur on uncapped framerates.");
	}
}

// TODO: This will have to be subclassed once more games are added
void YAMPUserInterface::DrawGame()
{
	// Master volume. Every host converts this to whatever its own module's volume mechanism
	// wants, so the game's mixer attenuates rather than YAMP scaling samples afterwards. Drawn
	// before the dispatch below so it appears for every game, not just the VF5FS panel.
	if (ImGui::SliderInt("Volume", &m_volumePercent, 0, 100, "%d%%"))
	{
		m_pageModified = true;
	}

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Master volume. 100%% is the module's own full scale.");
	}

	// All the m2ftg-family arcade modules (StF/FV/MR from LJ, VF2 from YLAD) share the same
	// hosting protocol and dip switches, so they share the settings panel too.
	if (IsM2ftgGame())
	{
		DrawGameStF();
		return;
	}

	{
		const char* labels[] = { "Japanese", "English" };
		if (ImGui::BeginCombo("Language", labels[m_language]))
		{
			size_t index = 0;
			for (const auto* label : labels)
			{
				const bool isSelected = index == m_language;
				if (ImGui::Selectable(label, isSelected))
				{
					m_pageModified = true;
					m_language = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
				++index;
			}

			ImGui::EndCombo();
		}
	}

	{
		const char* labels[] = { "Cross (A)", "Circle (B)" };
		if (ImGui::BeginCombo("Confirmation button", labels[m_circleConfirm]))
		{
			size_t index = 0;
			for (const auto* label : labels)
			{
				const bool isSelected = index == m_circleConfirm;
				if (ImGui::Selectable(label, isSelected))
				{
					m_pageModified = true;
					m_circleConfirm = index != 0;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
				++index;
			}

			ImGui::EndCombo();
		}
	}

	if (ImGui::Checkbox("Arcade Machine Mode", &m_arcadeMode))
	{
		m_pageModified = true;
	}

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("When unchecked, the game runs in console mode.");
	}

}

void YAMPUserInterface::DrawGameStF()
{
	{
		if (m_m2RenderMode >= m2ftg::DISPLAY_MODE_COUNT)
		{
			m_m2RenderMode = 0;
		}
		if (ImGui::BeginCombo("Render resolution", m2ftg::DISPLAY_MODES[m_m2RenderMode].label))
		{
			for (uint32_t index = 0; index < m2ftg::DISPLAY_MODE_COUNT; index++)
			{
				const bool isSelected = index == m_m2RenderMode;
				if (ImGui::Selectable(m2ftg::DISPLAY_MODES[index].label, isSelected))
				{
					m_pageModified = true;
					m_m2RenderMode = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("The size the emulator itself renders at, chosen through the module's own\n"
				"command-line option - not the window size, which stays on the Graphics page.\n"
				"\"Model 2 native\" is the arcade board's real 496x384, presented upscaled.\n"
				"The module reads this once at startup, so it takes effect on the next launch.");
		}
	}

	if (ImGui::Checkbox("Match window to render resolution", &m_m2WindowMatchesRender))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Sizes the window to the resolution above instead of the one on the Graphics\n"
			"page, and presents it 1:1 with no letterboxing - a pixel-exact arcade window.\n"
			"\"Model 2 native\" gives a literal 496x384 window. Overrides the aspect ratio\n"
			"setting while it is on, and is ignored in fullscreen. Takes effect on the next launch.");
	}


	{
		const char* labels[] = { "4:3 (Original)", "16:9 (Stretched)", "Fill Window" };
		if (m_m2Aspect >= std::size(labels))
		{
			m_m2Aspect = 0;
		}
		if (ImGui::BeginCombo("Aspect ratio", labels[m_m2Aspect]))
		{
			for (uint32_t index = 0; index < std::size(labels); index++)
			{
				const bool isSelected = index == m_m2Aspect;
				if (ImGui::Selectable(labels[index], isSelected))
				{
					m_pageModified = true;
					m_m2Aspect = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s is a native 4:3 arcade game.\nApplies immediately.",
				gGeneral.GetArcadeGameName());
		}
	}

	if (ImGui::Checkbox("CRT filter", &m_m2CrtFilter))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		if (IsVF2Module())
		{
			ImGui::SetTooltip("Lost Judgment's CRT effect (scanlines + aperture grille), an exact port\n"
				"of the shader LJ draws its Model 2 arcade minigames with.\nApplies immediately.");
		}
		else
		{
			ImGui::SetTooltip("Lost Judgment's own CRT effect (scanlines + aperture grille),\n"
				"an exact port of the shader LJ draws this game with.\nApplies immediately.");
		}
	}

	ImGui::NewLine();
	ImGui::Separator();
	ImGui::TextUnformatted("ARCADE DIP SWITCHES:");

	{
		const char* labels[] = { "Easy", "Normal", "Hard", "Hardest" };
		if (m_m2Difficulty >= std::size(labels))
		{
			m_m2Difficulty = 1;
		}
		if (ImGui::BeginCombo("Difficulty", labels[m_m2Difficulty]))
		{
			for (uint32_t index = 0; index < std::size(labels); index++)
			{
				const bool isSelected = index == m_m2Difficulty;
				if (ImGui::Selectable(labels[index], isSelected))
				{
					m_pageModified = true;
					m_m2Difficulty = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Arcade difficulty dip switch (Normal is the arcade default).\nRequires a restart.");
		}
	}

	{
		const char* labels[] = { "Japan", "USA", "Export" };
		if (m_m2Country >= std::size(labels))
		{
			m_m2Country = 0;
		}
		if (ImGui::BeginCombo("Region", labels[m_m2Country]))
		{
			for (uint32_t index = 0; index < std::size(labels); index++)
			{
				const bool isSelected = index == m_m2Country;
				if (ImGui::Selectable(labels[index], isSelected))
				{
					m_pageModified = true;
					m_m2Country = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
		{
			if (gGeneral.GetGameId() == YAMPGeneral::GameId::StF)
			{
				ImGui::SetTooltip("Region the arcade board boots as. USA runs the game as\n"
					"Sonic Championship, its western release.\nRequires a restart.");
			}
			else
			{
				ImGui::SetTooltip("Region the arcade board boots as.\nRequires a restart.");
			}
		}
	}

	if (ImGui::Checkbox("Free Play", &m_m2Freeplay))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("When unchecked, the game asks for credits like a real cabinet:\n"
			"press Start (F key) on the coin screen to insert a coin.\nRequires a restart.");
	}

	// StF's own GAME ASSIGNMENTS page has a DAMAGE item; the other m2ftg games do not, and it is
	// not part of the module's config block either - it is a byte of the ROM's live game
	// assignments, which is why this one applies immediately (see m2ftg::UpdateDamageAssignment).
	if (gGeneral.GetGameId() == YAMPGeneral::GameId::StF)
	{
		const char* labels[] = { "Normal", "Real" };

		// FROZEN once a room exists. The setting is published when the room is CREATED, so from
		// that moment on it describes the match rather than this machine, and letting it move
		// would only ever mean one of two wrong things: a value that is ignored (confusing), or
		// one peer's emulator changing a damage rule mid-match (a desync). Showing the room's
		// live value read-only is the honest version of both. Everything before a room - offline,
		// connecting, or logged in and browsing - stays editable, which is where the choice
		// belongs.
		if (net::SessionInProgress())
		{
			const net::Status status = net::GetStatus();
			ImGui::LabelText("Damage", "%s", labels[status.real_damage ? 1 : 0]);
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Set by the room's host and fixed for the match.\n"
					"Leave the room to change your own setting.");
			}
		}
		else
		{
			const int current = m_m2RealDamage ? 1 : 0;
			if (ImGui::BeginCombo("Damage", labels[current]))
			{
				for (int index = 0; index < 2; index++)
				{
					const bool isSelected = index == current;
					if (ImGui::Selectable(labels[index], isSelected))
					{
						m_pageModified = true;
						m_m2RealDamage = index != 0;
					}
					if (isSelected)
						ImGui::SetItemDefaultFocus();
				}

				ImGui::EndCombo();
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("The cabinet's GAME ASSIGNMENTS -> DAMAGE setting. Real makes hits take\n"
					"considerably more health, which is the setting most competitive players use.\n"
					"Applies immediately - no restart needed.\n\n"
					"Online this is a property of the ROOM: it is published when the host creates one\n"
					"and cannot be changed while you are in a room. Everyone plays under the host's\n"
					"choice, whatever their own setting says.");
			}
		}
	}

	if (ImGui::Checkbox("Versus Mode", &m_m2VersusMode))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Boots straight into a credited 2-player versus match, the way\n"
			"%s's minigame runs. Unchecked: authentic arcade boot\n"
			"(attract mode, single-player ladder).\nRequires a restart.",
			gGeneral.GetParentGameName());
	}

	// The VF2 module's own extra config switches (m2ftg_config_t is_vf20 / is_disable_pepsi;
	// no readers in the StF/FV DLLs).
	if (IsVF2Module())
	{
		if (ImGui::Checkbox("Version 2.0", &m_vf2Version20))
		{
			m_pageModified = true;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("The module's own revision switch: boots the original Virtua Fighter 2.0\n"
				"instead of the 2.1 revision.\nRequires a restart.");
		}

		if (ImGui::Checkbox("Disable Pepsi logos", &m_vf2DisablePepsi))
		{
			m_pageModified = true;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("The module's own switch for removing the game's Pepsi product placement.\nRequires a restart.");
		}
	}
}

void YAMPUserInterface::DrawControls()
{
	// EVERY hosted game — the m2ftg titles AND all three VF5FS builds — fills its pads through
	// csl_pad::set_state (source/input/Pad.cpp), which reads nothing but the Input bindings.
	// So the real, editable bindings are the correct page for all of them.
	//
	// This used to fall through to a hardcoded list ("P = K, K = L, G = J, Movement = Arrow Keys /
	// WSAD", with a "TODO: Make these controls customizable"). That list predated the VF5FS hosts
	// moving onto Input and had become actively wrong: it told the player to press keys that are
	// not bound to anything, while the actual bindings were the shared ones (Punch = Z, Kick = X,
	// Guard = C, Start = 1 by default). It looked exactly like input being broken.
	DrawControlsStF();
}

void YAMPUserInterface::DrawControlsStF()
{
	ImGui::PushTextWrapPos();
	ImGui::TextUnformatted("Each player can play on the keyboard, an XInput controller, or both. "
		"Use Program All Inputs to set everything up in one go, or click a single binding to change it "
		"(right-click clears it). Applies when you press Apply - no restart needed.");
	ImGui::PopTextWrapPos();
	ImGui::NewLine();

	if (ImGui::BeginTabBar("##stfplayers"))
	{
		for (int player = 0; player < 2; player++)
		{
			char tab[16];
			sprintf_s(tab, "Player %d", player + 1);
			if (ImGui::BeginTabItem(tab))
			{
				DrawControlsStFPlayer(player);
				ImGui::EndTabItem();
			}
		}
		ImGui::EndTabBar();
	}

	ImGui::NewLine();
	ImGui::TextDisabled("The left stick always steers, like the cabinet lever. Escape pauses; F1 opens this window.");
	// The coin/start dance is the m2ftg hosts' arcade behaviour; the VF5FS hosts do not implement it.
	if (IsM2ftgGame())
	{
		ImGui::TextDisabled("With Free Play off, Start doubles as a coin insert at the coin screen.");
		ImGui::TextDisabled("Test and Service are the cabinet's service-panel switches: Test opens the board's\n"
			"own service menu (where the input test shows exactly what the game is reading),\n"
			"Service is the credit/navigate button beside it. Both are disabled during netplay.");
	}
	ImGui::TextDisabled("In the game's own menus, Punch confirms, Kick cancels and Back resets/shows controls.");

	DrawStfBindingCapture();
}

void YAMPUserInterface::DrawControlsStFPlayer(int player)
{
	// Keeps the connection indicators and capture edge detection fresh even before the
	// game loop's own per-frame poll has run (polling twice per frame is harmless).
	Input::PollPads();

	// Every attached controller, XInput and DirectInput alike. A configured but unplugged pad
	// still gets a row, so its bindings look intact rather than silently reassigned.
	const std::vector<Input::PadDevice>& devices = Input::Devices();
	const int selected = Input::FindDevice(m_m2PadId[player]);
	const bool selectedMissing = selected < 0 && !m_m2PadId[player].empty();

	std::string preview = "None (keyboard only)";
	if (selected >= 0)
	{
		preview = devices[selected].name;
	}
	else if (selectedMissing)
	{
		preview = m_m2PadId[player] + " (not connected)";
	}

	if (ImGui::BeginCombo("Controller", preview.c_str()))
	{
		if (ImGui::Selectable("None (keyboard only)", m_m2PadId[player].empty()))
		{
			m_pageModified = true;
			m_m2PadId[player].clear();
		}
		for (const Input::PadDevice& device : devices)
		{
			const bool isSelected = device.id == m_m2PadId[player];
			if (ImGui::Selectable(device.name.c_str(), isSelected))
			{
				m_pageModified = true;
				m_m2PadId[player] = device.id;
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		if (selectedMissing)
		{
			// Keep the absent pad selectable so opening the combo cannot silently drop it.
			ImGui::Selectable(preview.c_str(), true);
			ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	// Manual: scanning costs ~100 ms (see Input::RefreshDevices) and there is no cheap way to be
	// told about a new pad, so a button is the only way that does not stutter the game.
	if (ImGui::Button("Rescan"))
	{
		Input::RequestDeviceRescan();
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Look for controllers plugged in since YAMP started.\n"
			"Brief pause while it scans.");
	}
	ImGui::NewLine();

	if (ImGui::Button("Program All Inputs..."))
	{
		StartStfCapture(player, true, 0, 0);
	}
	ImGui::SameLine();
	if (ImGui::Button("Restore Defaults"))
	{
		m_pageModified = true;
		m_m2KeyBinds[player] = Input::DEFAULT_KEY_BINDS[player];
		m_m2PadBinds[player] = Input::DEFAULT_PAD_BINDS[player];
		// The pad defaults are Xbox buttons, so the matching device is the player's XInput slot.
		// A DirectInput pad is left selected - resetting it would just unplug the player.
		if (m_m2PadId[player].empty() || m_m2PadId[player].compare(0, 7, "xinput:") == 0)
		{
			m_m2PadId[player] = "xinput:" + std::to_string(player);
		}
	}

	if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
	{
		ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();

		for (uint32_t action = 0; action < Input::Action_Count; action++)
		{
			// Test / Service are switches on the arcade cabinet's service panel, which only the
			// m2ftg boards emulate — the VF5FS builds have no I/O board to wire them to, so
			// offering the bindings there would just be a row that does nothing.
			if ((action == Input::Action_Test || action == Input::Action_Service) && !IsM2ftgGame())
			{
				continue;
			}

			ImGui::PushID(static_cast<int>(action));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(Input::ActionName(action));

			ImGui::TableNextColumn();
			const std::string keyLabel = Input::KeyName(m_m2KeyBinds[player][action]);
			if (ImGui::Button((keyLabel + "##key").c_str(), ImVec2(-FLT_MIN, 0.0f)))
			{
				StartStfCapture(player, false, action, 1);
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && m_m2KeyBinds[player][action] != 0)
			{
				m_pageModified = true;
				m_m2KeyBinds[player][action] = 0;
			}

			ImGui::TableNextColumn();
			const std::string padButtonLabel = Input::PadButtonName(m_m2PadBinds[player][action]) + "##pad";
			if (ImGui::Button(padButtonLabel.c_str(), ImVec2(-FLT_MIN, 0.0f)))
			{
				StartStfCapture(player, false, action, 2);
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && m_m2PadBinds[player][action] != Input::Pad_None)
			{
				m_pageModified = true;
				m_m2PadBinds[player][action] = Input::Pad_None;
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

void YAMPUserInterface::StartStfCapture(int player, bool wizard, uint32_t action, uint32_t deviceMask)
{
	m_stfCapturePlayer = player;
	m_stfCaptureQueue.clear();
	m_stfCaptureStep = 0;
	if (wizard)
	{
		// The basics, prompted one by one: up, down, left, right, punch, kick, guard,
		// start, coin. Combos and Back stay on their per-row buttons.
		for (uint32_t a = 0; a < Input::WIZARD_ACTION_COUNT; a++)
		{
			m_stfCaptureQueue.push_back({ a, 3 });
		}
	}
	else
	{
		m_stfCaptureQueue.push_back({ action, deviceMask });
	}
	m_stfCaptureOpenPopup = true;

	// Prime the edge detectors with the current state so already-held inputs are ignored.
	m_stfCapturePrevKeys = gGeneral.GetPressedKeys();
	Input::PollPads();
	m_stfCapturePrevPadButtons.assign(Input::Devices().size(), 0);
	for (size_t i = 0; i < m_stfCapturePrevPadButtons.size(); i++)
	{
		m_stfCapturePrevPadButtons[i] = Input::GetPadState(static_cast<int>(i)).buttons;
	}
}

void YAMPUserInterface::AssignStfKey(int player, uint32_t action, uint32_t vk)
{
	m_pageModified = true;
	// The keyboard is shared hardware: a key can only mean one thing, so steal it from
	// wherever else it is bound (either player, any action).
	for (auto& binds : m_m2KeyBinds)
	{
		for (uint32_t& bind : binds)
		{
			if (bind == vk)
			{
				bind = 0;
			}
		}
	}
	m_m2KeyBinds[player][action] = vk;
}

void YAMPUserInterface::AssignStfPadButton(int player, uint32_t action, uint32_t button, const std::string& padId)
{
	m_pageModified = true;
	// Answering with a controller also claims that controller for this player - the
	// wizard's "press a button to join" moment, like Lost Judgment's pad picking.
	m_m2PadId[player] = padId;
	// Steal the button only from players reading the same physical controller; a player
	// on a different pad legitimately keeps identical bindings.
	for (int p = 0; p < 2; p++)
	{
		if (m_m2PadId[p] != padId)
		{
			continue;
		}
		for (uint32_t& bind : m_m2PadBinds[p])
		{
			if (bind == button)
			{
				bind = Input::Pad_None;
			}
		}
	}
	m_m2PadBinds[player][action] = button;
}

void YAMPUserInterface::DrawStfBindingCapture()
{
	if (m_stfCaptureQueue.empty())
	{
		return;
	}

	if (m_stfCaptureOpenPopup)
	{
		ImGui::OpenPopup("Assign Input");
		m_stfCaptureOpenPopup = false;
	}

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	// NoNav keeps captured presses (Enter, Space, arrows...) from also activating the
	// popup's own Skip/Cancel buttons through keyboard navigation.
	if (!ImGui::BeginPopupModal("Assign Input", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav))
	{
		// Closed from outside (e.g. the page changed) - abandon the remaining prompts.
		m_stfCaptureQueue.clear();
		return;
	}

	const StfCapturePrompt& prompt = m_stfCaptureQueue[m_stfCaptureStep];
	const char* deviceText = prompt.deviceMask == 1 ? "a key"
		: prompt.deviceMask == 2 ? "a controller button"
		: "a key or controller button";
	ImGui::Text("Player %d", m_stfCapturePlayer + 1);
	ImGui::Text("Press %s for:", deviceText);
	ImGui::SameLine();
	ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", Input::ActionName(prompt.action));
	if (m_stfCaptureQueue.size() > 1)
	{
		ImGui::TextDisabled("%zu of %zu", m_stfCaptureStep + 1, m_stfCaptureQueue.size());
	}

	Input::PollPads();
	const auto& keys = gGeneral.GetPressedKeys();

	bool advanced = false;
	if (prompt.deviceMask & 1)
	{
		// VKs below 8 are mouse buttons; Escape is the pause menu, F1 the settings toggle.
		for (uint32_t vk = 8; vk < 256 && !advanced; vk++)
		{
			if (vk == VK_ESCAPE || vk == VK_F1)
			{
				continue;
			}
			if (keys[vk] && !m_stfCapturePrevKeys[vk])
			{
				AssignStfKey(m_stfCapturePlayer, prompt.action, vk);
				advanced = true;
			}
		}
	}
	if (!advanced && (prompt.deviceMask & 2))
	{
		const std::vector<Input::PadDevice>& devices = Input::Devices();
		// A list that grew mid-prompt would index past the primed edges; treat new entries as
		// "nothing held".
		m_stfCapturePrevPadButtons.resize(devices.size(), 0);
		for (size_t i = 0; i < devices.size() && !advanced; i++)
		{
			const uint64_t pressed =
				Input::GetPadState(static_cast<int>(i)).buttons & ~m_stfCapturePrevPadButtons[i];
			for (uint32_t button = 1; button < Input::Pad_Count && !advanced; button++)
			{
				if (pressed & (1ull << button))
				{
					AssignStfPadButton(m_stfCapturePlayer, prompt.action, button, devices[i].id);
					advanced = true;
				}
			}
		}
	}

	ImGui::NewLine();
	if (ImGui::Button("Skip") && !advanced)
	{
		advanced = true; // keep the current binding, move on
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
	{
		m_stfCaptureQueue.clear();
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}

	if (advanced && ++m_stfCaptureStep >= m_stfCaptureQueue.size())
	{
		m_stfCaptureQueue.clear();
		ImGui::CloseCurrentPopup();
	}

	// Refresh the edge detectors for the next frame/prompt.
	m_stfCapturePrevKeys = keys;
	m_stfCapturePrevPadButtons.assign(Input::Devices().size(), 0);
	for (size_t i = 0; i < m_stfCapturePrevPadButtons.size(); i++)
	{
		m_stfCapturePrevPadButtons[i] = Input::GetPadState(static_cast<int>(i)).buttons;
	}
	ImGui::EndPopup();
}

void YAMPUserInterface::DrawDebug()
{
	// Everything on this page either alters the emulated ROM image (the HLE hook mask), writes
	// emulated RAM (the game debug flag) or exposes the DLL's own CPU controls. All of them change
	// what this machine simulates, so none of them may be touched while a peer is depending on
	// this machine simulating the same thing theirs does.
	if (net::SessionInProgress())
	{
		ImGui::PushTextWrapPos();
		ImGui::TextColored(WARNING_COLOUR, "Debug options are unavailable during a netplay session.");
		ImGui::TextUnformatted("These settings change what this machine simulates - the emulated ROM "
			"patches, the game's debug flag, and the game's own CPU/debug windows. Changing any of "
			"them while another player is in your room would desync the match immediately.");
		ImGui::NewLine();
		ImGui::TextUnformatted("Leave the room on the Netplay page to get them back.");
		ImGui::PopTextWrapPos();
		return;
	}

	if (m_debugInfoAccepted.has_value())
	{
		ImGui::TextDisabled("Graphics backend: DX11 on D3D12");

		bool accept = m_debugInfoAccepted.value();

		ImGui::PushTextWrapPos();
		ImGui::TextColored(WARNING_COLOUR, "WARNING: Do not change any of these options unless you really know what you are doing. "
			"They are intended ONLY for troubleshooting and no support is provided when any debug options are enabled. Use at your own risk.");
		if (!accept)
		{
			ImGui::TextColored(WARNING_COLOUR, "Tick the checkbox below to acknowledge.");
		}
		else
		{
			ImGui::NewLine();
		}
		ImGui::PopTextWrapPos();

		if (ImGui::Checkbox("I understand the consequences", &accept))
		{
			if (accept)
			{
				m_debugInfoAccepted = true;
			}
		}
	}

	if (!m_debugInfoAccepted.value_or(true))
	{
		return;
	}

	if (ImGui::Checkbox("Skip applying patches", &m_dontApplyPatches))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("When checked, no patches will be applied to the game DLL.");
	}

	if (ImGui::Checkbox("Use Debug D3D device", &m_useD3DDebugLayer))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Enables a debug D3D layer when supported by the system.");
	}

	// The DLL debug-menu windows and the emulated-RAM debug flag are reconstructed from
	// StF-specific DLL data/addresses; the loose-ROM bypass is generic across the LJ m2ftg
	// games (archive name and image list come from the GameDesc table).
	const bool isStf = gGeneral.GetGameId() == YAMPGeneral::GameId::StF;
	const bool isLJm2ftg = IsLJm2ftgGame();

	if (isStf)
	{
		if (ImGui::Checkbox("Display debugging features", &m_stfShowDebugFeatures))
		{
			m_pageModified = true;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Shows the game's own developer debug-menu windows (DEBUG MENU, CONFIG, PERFORMANCE, 960STAT),\n"
				"reconstructed from data inside the game DLL. Actions run the DLL's own handlers; some of them are\n"
				"stubs in the retail DLL and have no effect. Takes effect immediately after Apply.");
		}
	}

	if (isLJm2ftg)
	{
		if (ImGui::Checkbox("Load ROM files from a directory", &m_stfLooseRomFiles))
		{
			m_pageModified = true;
		}
		if (ImGui::IsItemHovered())
		{
			const auto& game = m2ftg::CurrentGame();
			ImGui::SetTooltip("Bypasses rom/%s and reads the ROM images directly from the matching rom subdirectory.\n"
				"Every image extracted from the archive (rom_code1.bin, rom_data.bin, the EP/POL/TEX ROMs)\n"
				"must be present, otherwise the archive is used as usual. Requires a restart.",
				game.rom_archive_name);
		}
	}

	if (isStf)
	{
		if (ImGui::Checkbox("Set the game's debug flag", &m_stfGameDebugFlag))
		{
			m_pageModified = true;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Flips the game's own debug flag in emulated RAM (the dword at 0x508000, XORed with 0x24).\n"
				"Re-applied automatically if the game rewrites that memory. Takes effect immediately after Apply.");
		}

		if (ImGui::Checkbox("Correct the module's backup-RAM TIME setting", &m_stfFixBackupTimeIndex))
		{
			m_pageModified = true;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("The module writes the round time into backup RAM +0x3351 as a raw second count (30),\n"
				"but the ROM stores an index 0-9 into its own table of times (10,20,30...99) there. Index 30 is\n"
				"off the end of the table. This writes the index the ROM expects instead. Requires a restart.\n"
				"\n"
				"TURN THIS OFF AND GAME ASSIGNMENTS STOPS WORKING: opening that page in the service menu freezes\n"
				"YAMP outright - not just the game - and the process has to be killed. That is the module's stock\n"
				"behaviour and the reason this option exists.\n"
				"\n"
				"IMPORTANT: leave HLE ROM hook 16 (GAME_INT+0x4) DISABLED while this is on. That hook copies the\n"
				"same byte straight into the round timer without the table lookup, so this ON plus that hook ON\n"
				"means every match lasts about two seconds. Both off = stock behaviour (working timer, but the\n"
				"menu page freezes); both on is the only broken combination.");
		}

		DrawStfHleHooks();
	}
}

void YAMPUserInterface::DrawStfHleHooks()
{
	namespace Hle = m2ftg::HleHooks;

	if (!ImGui::CollapsingHeader("HLE ROM hooks"))
	{
		return;
	}

	ImGui::PushTextWrapPos();
	ImGui::TextUnformatted("At board start-up the game DLL overwrites 76 individual i960 instructions in the program ROM "
		"with traps that run native code instead. Disabling a hook restores the ROM's original instruction, so the ROM's "
		"own code runs there again - which is what a patched rom_code1.bin needs in order to take effect. Applied live.");
	ImGui::Spacing();
	ImGui::TextUnformatted("A wholly different program ROM - homebrew rather than a patch - needs more than that: every "
		"offset below is a Sonic the Fighters address, so the installer corrupts 76 unrelated instructions before the "
		"CPU runs any of them, which is too early to repair here. For that, the settings.ini [HleRetarget] section moves "
		"or drops each hook before the installer runs. See the 'At' column.");
	ImGui::PopTextWrapPos();
	ImGui::Spacing();

	auto setMask = [this](unsigned kinds)
	{
		Hle::MaskForKinds(m_stfHleDisableMask, kinds);
		m_pageModified = true;
	};

	// Deliberately "the defaults" rather than "none": hook 16 is disabled out of the box because
	// re-enabling it alongside the backup-RAM TIME fix makes every match last about two seconds
	// (see HleHooks.h). A button that silently re-armed that is the same trap the tooltips warn
	// about. Re-enabling it is still one click away - its own checkbox in the list below.
	if (ImGui::Button("Restore defaults"))
	{
		m_stfHleDisableMask[0] = Hle::DEFAULT_DISABLE_MASK[0];
		m_stfHleDisableMask[1] = Hle::DEFAULT_DISABLE_MASK[1];
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Every hook enabled except the ones YAMP ships disabled - currently just hook 16\n"
			"(GAME_INT+0x4), which fights the backup-RAM TIME correction and would cut matches to\n"
			"about two seconds. Untick it below if you want the module's stock behaviour.");
	}
	ImGui::SameLine();
	if (ImGui::Button("Disable game-behaviour hooks"))
	{
		setMask(Hle::MODDING_KINDS);
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Disables every hook that stops the ROM's own code from running (Content and Removed),\n"
			"while leaving the emulator's own plumbing and host integration alone. This is the preset\n"
			"to use when modding the ROM.");
	}
	ImGui::SameLine();
	if (ImGui::Button("Disable all"))
	{
		ImGui::OpenPopup("Disable Core hooks?");
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Includes the Core hooks the emulator depends on (frame yield, vsync wait, self-test bypass).\n"
			"The game will hang. Provided for investigation only - restart YAMP to recover.");
	}

	if (ImGui::BeginPopupModal("Disable Core hooks?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
		ImGui::TextColored(WARNING_COLOUR, "This disables the Core hooks the emulator itself depends on - the frame "
			"yield, the vsync wait and the board self-test bypass. The emulated board will hang, and because the "
			"settings UI is drawn from the same loop, it will take this window with it.");
		ImGui::Spacing();
		ImGui::TextUnformatted("Core hooks are never written to settings.ini, so restarting YAMP always recovers.");
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		if (ImGui::Button("Disable all anyway"))
		{
			setMask(Hle::ALL_KINDS);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	size_t disabledCount = 0;
	for (size_t i = 0; i < Hle::COUNT; i++)
	{
		disabledCount += Hle::MaskTest(m_stfHleDisableMask, i) ? 1 : 0;
	}
	ImGui::Text("%zu of %zu hooks disabled", disabledCount, Hle::COUNT);
	ImGui::TextColored(WARNING_COLOUR, "Core hooks are session-only: they apply live but are never saved, so a restart always boots.");
	ImGui::Spacing();

	// Where the installer actually put each trap, which is only the same as the table above when
	// no [HleRetarget] entry moved it. Read live so it also shows a hand-edited ini taking effect.
	// Invocation counters. The rate is the useful number - "once per frame" is the shape a
	// correctly placed render/vsync hook should have - so hold a one-second baseline and show
	// hits/sec next to the running total.
	static uint32_t s_rateBaseline[Hle::COUNT] = {};
	static float s_ratePerSecond[Hle::COUNT] = {};
	static double s_lastRateSample = 0.0;
	{
		const double now = ImGui::GetTime();
		if (s_lastRateSample == 0.0)
		{
			s_lastRateSample = now;
		}
		else if (now - s_lastRateSample >= 1.0)
		{
			const float elapsed = static_cast<float>(now - s_lastRateSample);
			for (size_t i = 0; i < Hle::COUNT; i++)
			{
				const uint32_t total = Hle::HitCount(i);
				s_ratePerSecond[i] = static_cast<float>(total - s_rateBaseline[i]) / elapsed;
				s_rateBaseline[i] = total;
			}
			s_lastRateSample = now;
		}
	}

	uint32_t installed[Hle::COUNT];
	const bool haveInstalled = Hle::GetInstalledOffsets(installed);
	if (haveInstalled)
	{
		size_t retargeted = 0;
		size_t suppressed = 0;
		for (size_t i = 0; i < Hle::COUNT; i++)
		{
			if (installed[i] >= 0x200000)
			{
				suppressed++;
			}
			else if (installed[i] != Hle::Get(i).romOffset)
			{
				retargeted++;
			}
		}
		// ---- homebrew health checks ----------------------------------------------------------
		// Every hook misplacement found so far failed SILENTLY: hook 1 on no site is a black
		// screen with every counter healthy, hook 2 on an init-only site is a 98% emulated spin
		// at 2 fps with no error anywhere. The hit counters already measure both, so say it out
		// loud rather than leaving it to be bisected. Only while an ELF ROM is loaded - stock StF
		// legitimately leaves most of these alone.
		if (m2ftg::ElfRom::IsLoaded())
		{
			const bool yieldInstalled = installed[2] < 0x200000;
			if (Hle::HitCount(1) == 0)
			{
				ImGui::TextColored(WARNING_COLOUR,
					"HOOK 1 HAS NEVER FIRED - the composite is disabled, so the screen stays black.");
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("It is installed but its ROM site is never reached. Move it to an address the\n"
						"program executes once during start-up (the ROM can declare one as\n"
						"__yamp_hook_composite_enable).");
				}
			}
			if (yieldInstalled && s_ratePerSecond[2] > 0.0f && s_ratePerSecond[2] < 10.0f)
			{
				ImGui::TextColored(WARNING_COLOUR,
					"HOOK 2 IS FIRING AT ONLY %.1f/sec - the frame yield is not on a per-frame path.",
					s_ratePerSecond[2]);
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("The board spends its time spinning instead of handing frames back to the host.\n"
						"Expect a very low frame rate and high CPU. Move the yield to a site reached every\n"
						"frame (the ROM can declare one as __yamp_hook_frame_yield).");
				}
			}
			if (yieldInstalled && Hle::HitCount(2) == 0)
			{
				ImGui::TextColored(WARNING_COLOUR,
					"HOOK 2 HAS NEVER FIRED - no frame yield; the board will spin.");
			}
			if (Hle::UsedConvention())
			{
				ImGui::TextDisabled("Hook sites declared by the ROM (__yamp_hook_* symbols in game.elf).");
			}
		}

		// Hook 1 is the composite enable: without it the display target is bound and cleared every
		// frame but never drawn into, so the screen is black while every other counter looks
		// healthy. That failure is invisible from the outside and cost a long debugging session -
		// say it plainly rather than leaving it to the per-hook tooltip.
		if (installed[1] >= 0x200000)
		{
			ImGui::TextColored(WARNING_COLOUR,
				"HOOK 1 IS NOT INSTALLED - the screen will stay black no matter what the ROM draws.");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Hook 1 sets the module's composite-enable flag. Everything else can be working\n"
					"(geometry submitted, passes bound, draws recorded) and nothing will reach the display.\n"
					"For a non-StF ROM give it any address executed once at start-up: [HleRetarget] Hook1=<symbol>.");
			}
			ImGui::Spacing();
		}

		if (retargeted != 0 || suppressed != 0)
		{
			ImGui::TextColored(WARNING_COLOUR, "[HleRetarget]: %zu hook(s) moved, %zu never installed.",
				retargeted, suppressed);
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("The settings.ini [HleRetarget] section changed which ROM addresses the module's\n"
					"hook installer patches. Rows below show the address actually used.");
			}
			ImGui::Spacing();
		}
	}


	if (ImGui::Button("Reset hit counts"))
	{
		Hle::ResetHitCounts();
		for (size_t i = 0; i < Hle::COUNT; i++)
		{
			s_rateBaseline[i] = 0;
			s_ratePerSecond[i] = 0.0f;
		}
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Hits are counted in the i960 instruction fetch, so they show whether a hook's trap is\n"
			"actually reached - not just installed. A per-frame hook should sit near the frame rate.");
	}
	ImGui::Spacing();

	if (ImGui::BeginTable("hle_hooks", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableSetupColumn("Off");
		ImGui::TableSetupColumn("#");
		ImGui::TableSetupColumn("Kind");
		ImGui::TableSetupColumn("At");
		ImGui::TableSetupColumn("Hits");
		ImGui::TableSetupColumn("/sec");
		ImGui::TableSetupColumn("ROM site", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();

		for (size_t i = 0; i < Hle::COUNT; i++)
		{
			const Hle::Info& info = Hle::Get(i);
			ImGui::TableNextRow();
			ImGui::PushID(static_cast<int>(i));

			ImGui::TableNextColumn();
			bool disabled = Hle::MaskTest(m_stfHleDisableMask, i);
			if (ImGui::Checkbox("##off", &disabled))
			{
				Hle::MaskSet(m_stfHleDisableMask, i, disabled);
				m_pageModified = true;
			}

			ImGui::TableNextColumn();
			ImGui::Text("%zu", i);

			ImGui::TableNextColumn();
			if (info.kind == Hle::Kind::Core)
			{
				ImGui::TextColored(WARNING_COLOUR, "%s", Hle::KindName(info.kind));
			}
			else
			{
				ImGui::TextUnformatted(Hle::KindName(info.kind));
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", Hle::KindDescription(info.kind));
			}

			ImGui::TableNextColumn();
			if (!haveInstalled)
			{
				ImGui::Text("%06X", info.romOffset);
			}
			else if (installed[i] >= 0x200000)
			{
				ImGui::TextColored(WARNING_COLOUR, "%s", "-");
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Suppressed by [HleRetarget]: the installer skipped this hook, so the\n"
						"ROM word at 0x%06X was never overwritten.", info.romOffset);
				}
			}
			else if (installed[i] != info.romOffset)
			{
				ImGui::TextColored(WARNING_COLOUR, "%06X", installed[i]);
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Retargeted by [HleRetarget]: installed at 0x%06X instead of Sonic the\n"
						"Fighters' own 0x%06X.", installed[i], info.romOffset);
				}
			}
			else
			{
				ImGui::Text("%06X", installed[i]);
			}

			ImGui::TableNextColumn();
			const uint32_t hits = Hle::HitCount(i);
			if (hits == 0)
			{
				ImGui::TextDisabled("-");
			}
			else
			{
				ImGui::Text("%u", hits);
			}

			ImGui::TableNextColumn();
			if (s_ratePerSecond[i] <= 0.0f)
			{
				ImGui::TextDisabled("-");
			}
			else
			{
				ImGui::Text("%.1f", s_ratePerSecond[i]);
			}

			ImGui::TableNextColumn();
			ImGui::Text("%s", info.site);
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("ROM 0x%06X, native handler DLL+0x%X\n%s\n\n%s",
					info.romOffset, info.handlerRva,
					info.replacesInstruction ? "The original instruction never runs." : "The original instruction still runs after the handler.",
					info.note);
			}

			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

// The netplay page. Two halves that behave differently on purpose: the account block above is
// ordinary settings and goes through Apply/Cancel like every other page, while the lobby below
// acts the moment a button is pressed — it drives a live network session, which is not something
// that can sit pending until the user remembers to press Apply.
void YAMPUserInterface::DrawNetplay()
{
	ImGui::PushTextWrapPos();
	ImGui::TextColored(WARNING_COLOUR, "Netplay is experimental. It plays Sonic the Fighters against one "
		"other machine over an RPCN server using delay-based lockstep, the same scheme the PS3 port used.");
	ImGui::PopTextWrapPos();
	ImGui::Separator();

	if (!net::IsAvailable())
	{
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted("The netplay plugin (yampnet.dll) is not loaded, so netplay is unavailable. "
			"It is an optional DLL that sits next to YAMP.exe; builds that ship without it have no netcode at all.");
		const char* why = net::LoadError();
		if (why != nullptr && *why != '\0')
		{
			ImGui::TextColored(WARNING_COLOUR, "The plugin was found but rejected: %s", why);
		}
		ImGui::PopTextWrapPos();
		return;
	}

	const net::Status status = net::GetStatus();
	// A session started with -net-server drives itself end to end (it is the two-machine
	// regression harness); letting the lobby half-steer it would only produce states neither path
	// expects, so the controls go read-only instead.
	const bool commandLineSession = net::Config().enabled;

	// ---- Account ------------------------------------------------------------------------------
	ImGui::TextUnformatted("Account");
	ImGui::Separator();

	// Consumed at Connect, so these stay editable right up to that point and never need a restart.
	// Read-only rather than hidden once a session is up: the values still have to be readable, and
	// this ImGui build has no BeginDisabled. FAILED counts as editable on purpose — a rejected
	// login is exactly when a credential needs correcting.
	const bool accountLocked = commandLineSession
		|| (status.state != YAMPNET_STATE_IDLE && status.state != YAMPNET_STATE_FAILED);
	const ImGuiInputTextFlags lockFlag = accountLocked ? ImGuiInputTextFlags_ReadOnly : 0;
	if (accountLocked)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
	}

	ImGui::PushItemWidth(-180.0f);
	if (ImGui::InputText("Server", m_netServer, sizeof(m_netServer), lockFlag)) m_pageModified = true;
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Host name or address of the RPCN server, e.g. np.rpcs3.net.\n"
			"Both players must use the same one.");
	}

	if (ImGui::InputText("Account (NPID)", m_netNpid, sizeof(m_netNpid), lockFlag)) m_pageModified = true;

	const ImGuiInputTextFlags tokenFlags =
		lockFlag | (m_netShowToken ? 0 : ImGuiInputTextFlags_Password);
	if (ImGui::InputText("Password", m_netToken, sizeof(m_netToken), tokenFlags)) m_pageModified = true;
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Stored in plain text in the settings file, like every other setting.\n"
			"Use an account you do not mind being readable there.");
	}
	ImGui::PopItemWidth();
	ImGui::SameLine();
	ImGui::Checkbox("Show", &m_netShowToken);

	ImGui::PushItemWidth(-180.0f);
	if (ImGui::InputText("Communication ID", m_netComId, sizeof(m_netComId), lockFlag)) m_pageModified = true;
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("The title the rooms are scoped to. Both players must match.\n"
			"Any well-formed ID works (9 uppercase letters/digits, '_', 2 digits) -\n"
			"the server registers an unknown title on first use.");
	}

	if (ImGui::InputText("Certificate SHA-256", m_netFingerprint, sizeof(m_netFingerprint), lockFlag)) m_pageModified = true;
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Leave EMPTY for a server with a proper certificate - it is then validated\n"
			"the way a browser validates a website, and nothing needs to be pasted here.\n"
			"\n"
			"Fill it in only for a SELF-SIGNED server: those certificates carry no usable name, so\n"
			"ordinary validation can never accept them and the exact certificate is pinned instead.\n"
			"Connecting to one unpinned fails with its fingerprint in the message (and in\n"
			"yampnet.log) - that is the value to paste here.\n"
			"\n"
			"Do not pin a real certificate: it is reissued every renewal and the pin would then\n"
			"start rejecting the server.");
	}

	// The delay is read when a round starts, not when the session connects, so it stays editable
	// between matches — but not while one is running, where it would silently mean nothing.
	if (status.state == YAMPNET_STATE_SYNCING || status.state == YAMPNET_STATE_IN_MATCH)
	{
		ImGui::LabelText("Input delay", "%d frames", m_netFrameDelay);
	}
	else if (ImGui::SliderInt("Input delay", &m_netFrameDelay, 0, 10, "%d frames"))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Frames of delay applied to both players' inputs to hide network latency.\n"
			"Too low for the connection and the game stalls rather than desyncing; 2-4 suits most links.\n"
			"Read when a match starts, so a change applies to the next one.");
	}
	ImGui::PopItemWidth();

	if (accountLocked)
	{
		ImGui::PopStyleVar();
		ImGui::TextDisabled(commandLineSession
			? "Driven by the command line for this session."
			: "Disconnect to change these.");
	}

	// ---- Lobby --------------------------------------------------------------------------------
	ImGui::NewLine();
	ImGui::TextUnformatted("Session");
	ImGui::Separator();

	ImGui::PushTextWrapPos();
	ImGui::TextUnformatted(status.text);
	if (status.error != nullptr && *status.error != '\0')
	{
		ImGui::TextColored(WARNING_COLOUR, "%s", status.error);
	}
	// Latched for the rest of the session on purpose: this is the single most useful thing the
	// netplay UI can tell you, and it must not scroll away with the next status change.
	if (status.desynced)
	{
		ImGui::TextColored(WARNING_COLOUR,
			"Desync detected at frame %u (this machine %u, the other %u). The two emulators "
			"stopped simulating the same game there; see yampnet.log.",
			status.desync_frame, status.desync_local, status.desync_remote);
	}
	if (const char* actionError = net::LastActionError(); actionError != nullptr && *actionError != '\0')
	{
		ImGui::TextColored(WARNING_COLOUR, "%s", actionError);
	}
	ImGui::PopTextWrapPos();

	if (commandLineSession)
	{
		ImGui::TextDisabled("Started from the command line; the lobby controls are disabled.");
		return;
	}

	switch (status.state)
	{
	case YAMPNET_STATE_IDLE:
	case YAMPNET_STATE_FAILED:
	{
		const bool ready = m_netServer[0] != '\0' && m_netNpid[0] != '\0' && m_netToken[0] != '\0';
		if (ImGuiCustom::ButtonToggleable("Connect", ready))
		{
			// Deliberately the page's live buffers rather than the saved settings: connecting is
			// how you find out a credential is wrong, and having to Apply first would make fixing
			// it a two-step dance.
			net::Connect(m_netServer, m_netNpid, m_netToken, m_netFingerprint, m_netComId);
		}
		if (!ready && ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Fill in the server, account and password first.");
		}
		break;
	}

	case YAMPNET_STATE_CONNECTING:
		if (ImGui::Button("Cancel"))
		{
			net::Disconnect();
		}
		break;

	case YAMPNET_STATE_ONLINE:
	{
		if (ImGui::Button("Host a room"))
		{
			// The room takes this machine's cabinet settings with it. Read from the SAVED
			// settings, not the Game page's edit buffer: an unapplied combo change would
			// otherwise publish a value the local emulator is not running under.
			const YAMPSettings* set = gGeneral.GetSettings();
			net::HostRoom(m_netRoomPassword, set != nullptr && set->m_m2RealDamage);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("The room is created with your current Damage setting (Game page),\n"
				"and everyone who joins plays under it. It cannot be changed once the room exists.");
		}
		ImGui::SameLine();
		ImGui::PushItemWidth(160.0f);
		ImGui::InputText("Room password", m_netRoomPassword, sizeof(m_netRoomPassword));
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Optional. Leave empty for a room anyone with the ID can join.");
		}

		ImGui::PopItemWidth();

		// ---- Room browser --------------------------------------------------------------------
		ImGui::NewLine();
		if (ImGui::Button("Refresh room list"))
		{
			net::RefreshRooms();
			m_netSelectedRoom = 0;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Rooms are listed by the account hosting them.");

		net::RoomRow rooms[16];
		const unsigned int roomCount = net::GetRooms(rooms, static_cast<unsigned int>(std::size(rooms)));

		if (ImGui::BeginTable("##rooms", 5,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
			{ 0.0f, 130.0f }))
		{
			ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthFixed, 60.0f);
			ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, 60.0f);
			ImGui::TableSetupColumn("Locked", ImGuiTableColumnFlags_WidthFixed, 55.0f);
			ImGui::TableSetupColumn("Room ID", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableHeadersRow();

			for (unsigned int i = 0; i < roomCount; i++)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(static_cast<int>(i));
				const bool selected = m_netSelectedRoom == rooms[i].room_id;
				if (ImGui::Selectable(rooms[i].owner, selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					m_netSelectedRoom = rooms[i].room_id;
					sprintf_s(m_netJoinRoomId, "%llu", rooms[i].room_id);
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u/%u", rooms[i].players, rooms[i].max_players);
				ImGui::TableSetColumnIndex(2);
				// The host's DAMAGE assignment, published with the room. Worth a column of its
				// own because it is not a preference you keep on joining - it is how that match
				// will play, and the two settings are very different games.
				ImGui::TextUnformatted(rooms[i].real_damage ? "Real" : "Normal");
				ImGui::TableSetColumnIndex(3);
				// A locked room cannot be entered without the password at all: the server only
				// hands a password-less joiner a PUBLIC slot, and a locked room has none.
				ImGui::TextUnformatted(rooms[i].has_password ? "yes" : "");
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%llu", rooms[i].room_id);
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		if (roomCount == 0)
		{
			ImGui::TextDisabled("No rooms found. Press Refresh, or host one yourself.");
		}

		// Join by ID stays available: a room can be joined before it shows up in a search, and it
		// is the fallback when someone simply gives you a number.
		ImGui::PushItemWidth(160.0f);
		ImGui::InputText("Room ID", m_netJoinRoomId, sizeof(m_netJoinRoomId), ImGuiInputTextFlags_CharsDecimal);
		ImGui::PopItemWidth();
		ImGui::SameLine();
		if (ImGuiCustom::ButtonToggleable("Join room", m_netJoinRoomId[0] != '\0'))
		{
			net::JoinRoom(_strtoui64(m_netJoinRoomId, nullptr, 10), m_netRoomPassword);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("A locked room needs its password typed into the Room password box above.");
		}

		ImGui::NewLine();
		if (ImGui::Button("Disconnect"))
		{
			net::Disconnect();
		}
		break;
	}

	case YAMPNET_STATE_IN_ROOM:
	{
		if (status.room_id != 0)
		{
			ImGui::Text("Room ID: %llu", status.room_id);
			ImGui::SameLine();
			if (ImGui::Button("Copy"))
			{
				char idText[32];
				sprintf_s(idText, "%llu", status.room_id);
				ImGui::SetClipboardText(idText);
			}
		}
		// What this match will actually be played under, stated for BOTH players: the host has to
		// see what it published (the room is fixed now, so a later settings change is not it), and
		// the guest has to see what it has just adopted.
		ImGui::Text("Damage: %s%s", status.real_damage ? "Real" : "Normal",
			status.hosting ? "" : " (set by the host)");
		ImGui::PushTextWrapPos();
		if (status.hosting)
		{
			ImGui::TextUnformatted("Give this ID to the other player so they can join.");
		}
		// The room is not a presence channel: this machine finds out the other player exists only
		// when their first packet arrives, and that does not happen until they start the match.
		// So "waiting for them to join" is not something the lobby can honestly display.
		ImGui::TextUnformatted("Both players press Start match. Nothing happens until both have - "
			"the board is reset on both machines at that point so the round starts from the same state.");
		ImGui::PopTextWrapPos();

		if (ImGui::Button("Start match"))
		{
			net::RequestStartRound();
		}
		ImGui::SameLine();
		if (ImGui::Button("Leave room"))
		{
			net::LeaveRoom();
		}
		break;
	}

	case YAMPNET_STATE_SYNCING:
	case YAMPNET_STATE_IN_MATCH:
	{
		ImGui::Text("You are player %d", status.local_player + 1);
		ImGui::Text("Input delay: %d frames", m_netFrameDelay);
		// Every stall is a frame the emulator could not run because the other machine's input had
		// not arrived. A number that keeps climbing is the connection, not the game.
		ImGui::Text("Stalls: %u", status.stall_count);
		if (ImGui::Button("Leave match"))
		{
			net::LeaveRoom();
		}
		break;
	}

	default:
		break;
	}
}

void YAMPUserInterface::DrawNetplayOverlay()
{
	if (!IsNetplayGame() || !net::IsAvailable())
	{
		return;
	}

	const net::Status status = net::GetStatus();

	// The other player vanished. This takes over the screen rather than joining the small status
	// overlay: the match is over, the game is about to be reset, and the player needs to know why
	// their opponent stopped moving instead of being dropped back into attract mode unexplained.
	if (status.peer_lost)
	{
		const ImVec2& display = ImGui::GetIO().DisplaySize;
		ImGui::SetNextWindowPos({ display.x / 2.0f, display.y / 2.0f }, ImGuiCond_Always,
			{ 0.5f, 0.5f });
		ImGui::Begin("Netplay##peerlost", nullptr,
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
		ImGui::TextUnformatted(status.peer_lost_reason);
		ImGui::TextUnformatted("The session will be closed and the game reset.");
		ImGui::NewLine();
		if (ImGui::Button("OK", { 120.0f, 0.0f }))
		{
			// Leave RPCN entirely (not just the room) so nothing reconnects behind the player's
			// back, then put the board back to a clean attract mode.
			net::EndSession();
			m2ftg::ResetBoard();
		}
		ImGui::End();
		return;
	}

	// Nothing to say before a session is started, and nothing worth covering the game with once
	// one is running normally.
	if (!status.started || status.state == YAMPNET_STATE_IN_MATCH || status.state == YAMPNET_STATE_IDLE)
	{
		return;
	}

	ImGui::SetNextWindowPos({ 20, 20 }, ImGuiCond_Always);
	ImGui::Begin("Netplay", nullptr,
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
	ImGui::TextUnformatted(status.text);
	// Shown for as long as there is a room, not just while IN_ROOM: a command-line host opens the
	// barrier the same frame it gets the room, so it is SYNCING within a frame or two - and that
	// is exactly when the other player still needs to be told which room to join.
	if (status.room_id != 0)
	{
		ImGui::Text("Room ID: %llu", status.room_id);
	}
	// The round cannot start until this machine's board has booted (see m2ftg::IsBoardBooted).
	// Say so, or the wait looks like the session having stalled.
	if ((status.state == YAMPNET_STATE_IN_ROOM || status.state == YAMPNET_STATE_SYNCING)
		&& !m2ftg::IsBoardBooted())
	{
		ImGui::TextUnformatted("Waiting for the emulated board to finish booting...");
	}
	if (status.state == YAMPNET_STATE_FAILED && status.error != nullptr && *status.error != '\0')
	{
		ImGui::TextColored(WARNING_COLOUR, "%s", status.error);
	}
	if (status.desynced)
	{
		ImGui::TextColored(WARNING_COLOUR, "Desync at frame %u - round ended", status.desync_frame);
	}
	ImGui::TextDisabled("F1 -> Netplay");
	ImGui::End();
}

void YAMPUserInterface::DrawAbout()
{
	ImGui::PushTextWrapPos();

	// YAMP info
	ImGui::TextUnformatted("Yakuza Arcade Machines Player");
#if rsc_BuildID > 0
	ImGui::TextUnformatted("Build " STRINGIZE(rsc_RevisionID) " (Rev " STRINGIZE(rsc_BuildID) ")");
#else
	ImGui::TextUnformatted("Build " STRINGIZE(rsc_RevisionID));
#endif
	ImGui::TextUnformatted("Compiled on " __DATE__ " " __TIME__);
	if (ImGui::Button("Check for updates"))
	{
		ShellExecuteW(nullptr, L"open", WIDEN(rsc_UpdateURL), nullptr, nullptr, SW_SHOWNORMAL);
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("A GitHub page will open in the web browser.");
	}

	// Game info
	ImGui::Separator();
	ImGui::TextUnformatted("GAME INFORMATION:");

	const char* arcadeName = gGeneral.GetArcadeGameName();
	const char* baseGameName = gGeneral.GetParentGameName();
	ImGui::Text("Current arcade: %s", arcadeName);
	ImGui::Text("Base game: %s", baseGameName);
	ImGui::Text("DLL name: %s", gGeneral.GetDLLName().c_str());
	{
		const time_t timestamp = gGeneral.GetDLLTimestamp();
		tm time;
		if (gmtime_s(&time, &timestamp) == 0)
		{
			char timeStr[64];
			if (strftime(timeStr, std::size(timeStr), "%b %e %Y %T", &time) != 0)
			{
				ImGui::Text("DLL compiled on %s", timeStr);
			}
		}
	}

	// Integrity + ownership verdicts from the pre-load check (source/GameVerify.cpp). The
	// process only gets this far when neither of them blocked, so this is a record of what
	// was verified rather than a warning.
	{
		const Verify::ModuleResult& module = Verify::LastModuleResult();
		if (module.status == Verify::ModuleStatus::Verified)
		{
			ImGui::Text("DLL checksum: verified, %s", module.buildLabel);
			ImGui::TextDisabled("SHA-256: %s", module.sha256.c_str());
		}
		else if (module.status == Verify::ModuleStatus::NotChecked)
		{
			ImGui::TextUnformatted("DLL checksum: no reference for this game yet");
		}

		const Verify::ParentResult& parent = Verify::LastParentResult();
		if (parent.status == Verify::ParentStatus::Verified)
		{
			ImGui::Text("Base game: verified, %s", parent.buildLabel);
		}
		else if (parent.status == Verify::ParentStatus::UnknownBuild)
		{
			ImGui::TextColored(WARNING_COLOUR, "Base game: %s found, but its version is not one "
				"YAMP recognises.", parent.exeName);
		}
	}

	// Disclaimers
	ImGui::Separator();
	ImGui::TextUnformatted("ACKNOWLEDGEMENTS:");
	ImGui::TextColored(WARNING_COLOUR, "Yakuza Arcade Machines Player does not redistribute ANY copyrighted files. "
			"You must own an original Steam copy of %s to play this game via YAMP. "
			"Pirated game copies WILL NOT receive any support.", baseGameName);

	ImGui::NewLine();
	ImGui::Text("All rights to %s belong to SEGA.", arcadeName);

	ImGui::PopTextWrapPos();
}

bool YAMPUserInterface::DrawSettingsConfirmation()
{
	bool result = false;

	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal("YAMP Settings##save", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Save changes?\n\n");
		ImGui::Separator();

		if (ImGui::Button("Yes", ImVec2(120, 0)))
		{
			ApplySettings();
			ImGui::CloseCurrentPopup();
			result = true;
		}
		ImGui::SetItemDefaultFocus();
		ImGui::SameLine();
		if (ImGui::Button("No", ImVec2(120, 0)))
		{
			DiscardSettings();
			ImGui::CloseCurrentPopup();
			result = true;
		}

		ImGui::EndPopup();
	}

	return result;
}

void YAMPUserInterface::DrawDisclaimer()
{
	// Show disclaimer for 20 seconds from the time it's first called
	static const auto displayStartTime = std::chrono::system_clock::now();
	if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - displayStartTime) >= std::chrono::seconds(20))
	{
		auto token = gGeneral.GetSettingsUpdateToken();
		auto* settings = token.first;

		settings->m_buildLastShowedDisclaimer = rsc_RevisionID;
	}

	const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

	ImGui::SetNextWindowPos({ 10.0f, 10.0f }, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.75f);

	if (ImGui::Begin("##disclaimer", nullptr, windowFlags))
	{
		ImGui::TextUnformatted("Welcome to Yakuza Arcade Machines Player (Build " STRINGIZE(rsc_RevisionID) ").");
		ImGui::Separator();

		ImGui::TextColored(WARNING_COLOUR, "DISCLAIMER: Yakuza Arcade Machines Player does not redistribute ANY copyrighted files.\n"
			"You must own an original Steam copy of %s to play this game via YAMP.\n"
			"Pirated game copies WILL NOT receive any support.", gGeneral.GetParentGameName());

		ImGui::NewLine();
		ImGui::Text("All rights to %s belong to SEGA.", gGeneral.GetArcadeGameName());
		ImGui::Separator();
		ImGui::TextUnformatted("Press "); ImGui::SameLine(0, 0); ImGui::TextColored(WARNING_COLOUR, "F1"); ImGui::SameLine(0, 0); ImGui::TextUnformatted(" to open settings.");
	}
	ImGui::End();
}

bool YAMPUserInterface::ProcessF1Key()
{
	static bool keyDown = false;
	if (GetAsyncKeyState(VK_F1) & 0x8000)
	{
		if (!keyDown)
		{
			keyDown = true;
			m_settingsOpen = !m_settingsOpen;
		}
	}
	else
	{
		keyDown = false;
	}

	return m_settingsOpen;
}

void YAMPUserInterface::ApplySettings()
{
	auto token = gGeneral.GetSettingsUpdateToken();
	auto* settings = token.first;

	// StF's aspect ratio, CRT filter and button assignments are re-read by the game loop every
	// frame, so they take effect immediately; only warn about a restart when a setting that is
	// consumed once at startup actually changed.
	const bool needsRestart =
		settings->m_resX != m_resolutions[m_currentResolutionIndex].width ||
		settings->m_resY != m_resolutions[m_currentResolutionIndex].height ||
		settings->m_refreshRate != m_resolutions[m_currentResolutionIndex].refreshRates[m_currentRefRateIndex].refreshRate ||
		settings->m_fullscreen != m_currentFullscreen ||
		settings->m_enableFpsCap != m_enableFpsCap ||
		settings->m_arcadeMode != m_arcadeMode ||
		settings->m_circleConfirm != m_circleConfirm ||
		settings->m_language != m_language ||
		settings->m_volumePercent != static_cast<uint32_t>(m_volumePercent) ||
		settings->m_m2RenderMode != m_m2RenderMode ||
		settings->m_m2WindowMatchesRender != m_m2WindowMatchesRender ||
		settings->m_m2Difficulty != m_m2Difficulty ||
		settings->m_m2Country != m_m2Country ||
		settings->m_m2Freeplay != m_m2Freeplay ||
		settings->m_m2VersusMode != m_m2VersusMode ||
		settings->m_m2RealDamage != m_m2RealDamage ||
		settings->m_vf2Version20 != m_vf2Version20 ||
		settings->m_vf2DisablePepsi != m_vf2DisablePepsi ||
		settings->m_dontApplyPatches != m_dontApplyPatches ||
		settings->m_useD3DDebugLayer != m_useD3DDebugLayer ||
		settings->m_stfFixBackupTimeIndex != m_stfFixBackupTimeIndex ||
		settings->m_stfLooseRomFiles != m_stfLooseRomFiles;

	settings->m_resX = m_resolutions[m_currentResolutionIndex].width;
	settings->m_resY = m_resolutions[m_currentResolutionIndex].height;
	settings->m_refreshRate = m_resolutions[m_currentResolutionIndex].refreshRates[m_currentRefRateIndex].refreshRate;
	settings->m_fullscreen = m_currentFullscreen;
	settings->m_enableFpsCap = m_enableFpsCap;

	settings->m_arcadeMode = m_arcadeMode;
	settings->m_circleConfirm = m_circleConfirm;
	settings->m_language = m_language;
	settings->m_volumePercent = static_cast<uint32_t>(m_volumePercent);

	settings->m_m2RenderMode = m_m2RenderMode;
	settings->m_m2WindowMatchesRender = m_m2WindowMatchesRender;
	settings->m_m2Aspect = m_m2Aspect;
	settings->m_m2CrtFilter = m_m2CrtFilter;
	settings->m_m2Difficulty = m_m2Difficulty;
	settings->m_m2Country = m_m2Country;
	settings->m_m2Freeplay = m_m2Freeplay;
	settings->m_m2VersusMode = m_m2VersusMode;
	settings->m_m2RealDamage = m_m2RealDamage;
	// Bindings are re-read by the game loop every frame, so they apply live.
	settings->m_m2KeyBinds = m_m2KeyBinds;
	settings->m_m2PadBinds = m_m2PadBinds;
	for (int player = 0; player < 2; player++)
	{
		settings->m_m2PadId[player] = m_m2PadId[player];
	}

	// Consumed once by module_start (config.is_vf20 / is_disable_pepsi), hence the restart warning.
	settings->m_vf2Version20 = m_vf2Version20;
	settings->m_vf2DisablePepsi = m_vf2DisablePepsi;

	// Netplay: read when a session connects and when a round starts, so no restart is needed —
	// which is why none of these appear in the needsRestart test above.
	settings->m_netServer = m_netServer;
	settings->m_netNpid = m_netNpid;
	settings->m_netToken = m_netToken;
	settings->m_netCertFingerprint = m_netFingerprint;
	settings->m_netComId = m_netComId;
	settings->m_netFrameDelay = m_netFrameDelay;

	settings->m_dontApplyPatches = m_dontApplyPatches;
	settings->m_useD3DDebugLayer = m_useD3DDebugLayer;
	// The debug-window overlay is read every frame, so it applies live (no restart warning).
	settings->m_stfShowDebugFeatures = m_stfShowDebugFeatures;
	// Enforced every frame while the board is booted, so it also applies live.
	settings->m_stfGameDebugFlag = m_stfGameDebugFlag;
	// Read once when the module is patched, hence the restart warning above.
	settings->m_stfFixBackupTimeIndex = m_stfFixBackupTimeIndex;
	// Ditto: m2ftg::HleHooks::Update() reconciles the ROM image against this mask every frame.
	settings->m_stfHleDisableMask[0] = m_stfHleDisableMask[0];
	settings->m_stfHleDisableMask[1] = m_stfHleDisableMask[1];
	// Consumed once when module_start mounts the ROM archive, hence the restart warning above.
	settings->m_stfLooseRomFiles = m_stfLooseRomFiles;

	m_pageModified = false;
	if (needsRestart)
	{
		m_showRestartWarning = true;
	}
}

void YAMPUserInterface::DiscardSettings()
{
	GetDefaultsFromSettings();
	m_pageModified = false;
}
