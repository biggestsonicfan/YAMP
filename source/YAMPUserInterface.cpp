#include "YAMPUserInterface.h"

#include "YAMPGeneral.h"
#include "LJ/StfDebugWindows.h"
#include "LJ/m2ftg_host.h"

#include "imgui/imgui.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>

static const ImVec4 WARNING_COLOUR { 1.000f, 1.000f, 0.000f, 1.000f };

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
	if (gGeneral.GetSettings()->m_buildLastShowedDisclaimer < rsc_RevisionID)
	{
		DrawDisclaimer();
	}

	// The game DLL's own debug windows are independent of the F1 settings window.
	LJ::StF::DrawDebugWindows();
	LJ::StF::UpdateGameDebugFlag();

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
		int graphics_id, game_id, debug_id, about_id, controls_id;

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

			int index = 0;
			game_id = settingsSection("Game", index++, m_pageModified);
			graphics_id = settingsSection("Graphics", index++, m_pageModified);
			controls_id = settingsSection("Controls", index++, m_pageModified);
			debug_id = settingsSection("Debug", index++, m_pageModified);
			about_id = settingsSection("About", index++, m_pageModified);

			if (DrawSettingsConfirmation())
			{
				selectedTab = delayedSelectedTab;
			}

			ImGui::EndChild();
		}

		ImGui::SameLine();

		ImGui::BeginGroup();
		// About is informational; Controls is read-only except for the LJ m2ftg games (StF/FV),
		// whose button assignments are editable and go through the same Apply/Cancel flow as
		// the other pages.
		const bool controlsEditable = gGeneral.GetGameId() == YAMPGeneral::GameId::StF
			|| gGeneral.GetGameId() == YAMPGeneral::GameId::FV;
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

	m_stfAspect = settings->m_stfAspect;
	m_stfCrtFilter = settings->m_stfCrtFilter;
	m_stfDifficulty = settings->m_stfDifficulty;
	m_stfCountry = settings->m_stfCountry;
	m_stfFreeplay = settings->m_stfFreeplay;
	m_stfVersusMode = settings->m_stfVersusMode;
	m_stfKeyBinds = settings->m_stfKeyBinds;
	m_stfPadBinds = settings->m_stfPadBinds;
	for (int player = 0; player < 2; player++)
	{
		m_stfPadIndex[player] = settings->m_stfPadIndex[player];
	}

	m_dontApplyPatches = settings->m_dontApplyPatches;
	m_useD3DDebugLayer = settings->m_useD3DDebugLayer;
	m_stfShowDebugFeatures = settings->m_stfShowDebugFeatures;
	m_stfLooseRomFiles = settings->m_stfLooseRomFiles;
	m_stfGameDebugFlag = settings->m_stfGameDebugFlag;

	// In case non-default Debug options are present, don't nag about the consequences of Debug options for this session
	if (m_dontApplyPatches || m_useD3DDebugLayer || m_stfShowDebugFeatures || m_stfLooseRomFiles || m_stfGameDebugFlag)
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
	if (gGeneral.GetGameId() == YAMPGeneral::GameId::StF
		|| gGeneral.GetGameId() == YAMPGeneral::GameId::FV)
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
		const char* labels[] = { "4:3 (Original)", "16:9 (Stretched)", "Fill Window" };
		if (m_stfAspect >= std::size(labels))
		{
			m_stfAspect = 0;
		}
		if (ImGui::BeginCombo("Aspect ratio", labels[m_stfAspect]))
		{
			for (uint32_t index = 0; index < std::size(labels); index++)
			{
				const bool isSelected = index == m_stfAspect;
				if (ImGui::Selectable(labels[index], isSelected))
				{
					m_pageModified = true;
					m_stfAspect = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s is a native 4:3 arcade game.\nApplies immediately.",
				gGeneral.GetGameId() == YAMPGeneral::GameId::FV ? "Fighting Vipers" : "Sonic the Fighters");
		}
	}

	if (ImGui::Checkbox("CRT filter", &m_stfCrtFilter))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Lost Judgment's own CRT effect (scanlines + aperture grille),\n"
			"an exact port of the shader LJ draws this game with.\nApplies immediately.");
	}

	ImGui::NewLine();
	ImGui::Separator();
	ImGui::TextUnformatted("ARCADE DIP SWITCHES:");

	{
		const char* labels[] = { "Easy", "Normal", "Hard", "Hardest" };
		if (m_stfDifficulty >= std::size(labels))
		{
			m_stfDifficulty = 1;
		}
		if (ImGui::BeginCombo("Difficulty", labels[m_stfDifficulty]))
		{
			for (uint32_t index = 0; index < std::size(labels); index++)
			{
				const bool isSelected = index == m_stfDifficulty;
				if (ImGui::Selectable(labels[index], isSelected))
				{
					m_pageModified = true;
					m_stfDifficulty = index;
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
		if (m_stfCountry >= std::size(labels))
		{
			m_stfCountry = 0;
		}
		if (ImGui::BeginCombo("Region", labels[m_stfCountry]))
		{
			for (uint32_t index = 0; index < std::size(labels); index++)
			{
				const bool isSelected = index == m_stfCountry;
				if (ImGui::Selectable(labels[index], isSelected))
				{
					m_pageModified = true;
					m_stfCountry = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
		{
			if (gGeneral.GetGameId() == YAMPGeneral::GameId::FV)
			{
				ImGui::SetTooltip("Region the arcade board boots as.\nRequires a restart.");
			}
			else
			{
				ImGui::SetTooltip("Region the arcade board boots as. USA runs the game as\n"
					"Sonic Championship, its western release.\nRequires a restart.");
			}
		}
	}

	if (ImGui::Checkbox("Free Play", &m_stfFreeplay))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("When unchecked, the game asks for credits like a real cabinet:\n"
			"press Start (F key) on the coin screen to insert a coin.\nRequires a restart.");
	}

	if (ImGui::Checkbox("Versus Mode", &m_stfVersusMode))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Boots straight into a credited 2-player versus match, the way\n"
			"Lost Judgment's minigame runs. Unchecked: authentic arcade boot\n"
			"(attract mode, single-player ladder).\nRequires a restart.");
	}
}

void YAMPUserInterface::DrawControls()
{
	if (gGeneral.GetGameId() == YAMPGeneral::GameId::StF
		|| gGeneral.GetGameId() == YAMPGeneral::GameId::FV)
	{
		DrawControlsStF();
		return;
	}

	ImGui::PushItemWidth(ImGui::CalcItemWidth() / 2.0f);

	// TODO: Make these controls customizable
	ImGui::LabelText("Movement", "Arrow Keys / WSAD");
	ImGui::LabelText("P", "K");
	ImGui::LabelText("K", "L");
	ImGui::LabelText("G", "J");
	ImGui::LabelText("P + G", "M");
	ImGui::LabelText("P + K + G", "I");
	ImGui::LabelText("P + K", "U");
	ImGui::LabelText("K + G", "O");
	ImGui::NewLine();
	ImGui::LabelText("Confirm", "Enter");
	ImGui::LabelText("Back", "Escape");
	ImGui::LabelText("Start Game, Select Subcostume, Skip", "F");
	ImGui::LabelText("View Controls, Reset", "Tab");

	ImGui::NewLine();
	ImGui::LabelText("Open YAMP Settings", "F1");

	ImGui::PopItemWidth();
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
	ImGui::TextDisabled("With Free Play off, Start doubles as a coin insert at the coin screen.");
	ImGui::TextDisabled("In the game's own menus, Punch confirms, Kick cancels and Back resets/shows controls.");

	DrawStfBindingCapture();
}

void YAMPUserInterface::DrawControlsStFPlayer(int player)
{
	// Keeps the connection indicators and capture edge detection fresh even before the
	// game loop's own per-frame poll has run (polling twice per frame is harmless).
	StFInput::PollPads();

	auto controllerLabel = [](int padIndex, char (&buf)[64]) -> const char* {
		if (padIndex < 0)
		{
			return "None (keyboard only)";
		}
		sprintf_s(buf, "Controller %d%s", padIndex + 1,
			StFInput::GetPadState(padIndex).connected ? "" : " (not connected)");
		return buf;
	};

	char label[64];
	if (ImGui::BeginCombo("Controller", controllerLabel(m_stfPadIndex[player], label)))
	{
		for (int padIndex = -1; padIndex < 4; padIndex++)
		{
			const bool isSelected = padIndex == m_stfPadIndex[player];
			if (ImGui::Selectable(controllerLabel(padIndex, label), isSelected))
			{
				m_pageModified = true;
				m_stfPadIndex[player] = padIndex;
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
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
		m_stfKeyBinds[player] = StFInput::DEFAULT_KEY_BINDS[player];
		m_stfPadBinds[player] = StFInput::DEFAULT_PAD_BINDS[player];
		m_stfPadIndex[player] = player;
	}

	if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
	{
		ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();

		for (uint32_t action = 0; action < StFInput::Action_Count; action++)
		{
			ImGui::PushID(static_cast<int>(action));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(StFInput::ActionName(action));

			ImGui::TableNextColumn();
			const std::string keyLabel = StFInput::KeyName(m_stfKeyBinds[player][action]);
			if (ImGui::Button((keyLabel + "##key").c_str(), ImVec2(-FLT_MIN, 0.0f)))
			{
				StartStfCapture(player, false, action, 1);
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && m_stfKeyBinds[player][action] != 0)
			{
				m_pageModified = true;
				m_stfKeyBinds[player][action] = 0;
			}

			ImGui::TableNextColumn();
			const char* padLabel = StFInput::PadButtonName(m_stfPadBinds[player][action]);
			char padButtonLabel[64];
			sprintf_s(padButtonLabel, "%s##pad", padLabel);
			if (ImGui::Button(padButtonLabel, ImVec2(-FLT_MIN, 0.0f)))
			{
				StartStfCapture(player, false, action, 2);
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && m_stfPadBinds[player][action] != StFInput::Pad_None)
			{
				m_pageModified = true;
				m_stfPadBinds[player][action] = StFInput::Pad_None;
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
		for (uint32_t a = 0; a < StFInput::WIZARD_ACTION_COUNT; a++)
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
	StFInput::PollPads();
	for (int i = 0; i < 4; i++)
	{
		m_stfCapturePrevPadButtons[i] = StFInput::GetPadState(i).buttons;
	}
}

void YAMPUserInterface::AssignStfKey(int player, uint32_t action, uint32_t vk)
{
	m_pageModified = true;
	// The keyboard is shared hardware: a key can only mean one thing, so steal it from
	// wherever else it is bound (either player, any action).
	for (auto& binds : m_stfKeyBinds)
	{
		for (uint32_t& bind : binds)
		{
			if (bind == vk)
			{
				bind = 0;
			}
		}
	}
	m_stfKeyBinds[player][action] = vk;
}

void YAMPUserInterface::AssignStfPadButton(int player, uint32_t action, uint32_t button, int padIndex)
{
	m_pageModified = true;
	// Answering with a controller also claims that controller for this player - the
	// wizard's "press a button to join" moment, like Lost Judgment's pad picking.
	m_stfPadIndex[player] = padIndex;
	// Steal the button only from players reading the same physical controller; a player
	// on a different pad legitimately keeps identical bindings.
	for (int p = 0; p < 2; p++)
	{
		if (m_stfPadIndex[p] != padIndex)
		{
			continue;
		}
		for (uint32_t& bind : m_stfPadBinds[p])
		{
			if (bind == button)
			{
				bind = StFInput::Pad_None;
			}
		}
	}
	m_stfPadBinds[player][action] = button;
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
	ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", StFInput::ActionName(prompt.action));
	if (m_stfCaptureQueue.size() > 1)
	{
		ImGui::TextDisabled("%zu of %zu", m_stfCaptureStep + 1, m_stfCaptureQueue.size());
	}

	StFInput::PollPads();
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
		for (int padIndex = 0; padIndex < 4 && !advanced; padIndex++)
		{
			const uint32_t pressed = StFInput::GetPadState(padIndex).buttons & ~m_stfCapturePrevPadButtons[padIndex];
			for (uint32_t button = 1; button < StFInput::Pad_Count && !advanced; button++)
			{
				if (pressed & (1u << button))
				{
					AssignStfPadButton(m_stfCapturePlayer, prompt.action, button, padIndex);
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
	for (int i = 0; i < 4; i++)
	{
		m_stfCapturePrevPadButtons[i] = StFInput::GetPadState(i).buttons;
	}
	ImGui::EndPopup();
}

void YAMPUserInterface::DrawDebug()
{
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
	const bool isLJm2ftg = isStf || gGeneral.GetGameId() == YAMPGeneral::GameId::FV;

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
			const auto& game = LJ::StF::CurrentGame();
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
	}
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

	const char* arcadeName;
	const char* baseGameName;
	switch (gGeneral.GetGameId())
	{
	case YAMPGeneral::GameId::StF:
		arcadeName = "Sonic the Fighters";
		baseGameName = "Lost Judgment";
		break;
	case YAMPGeneral::GameId::FV:
		arcadeName = "Fighting Vipers";
		baseGameName = "Lost Judgment";
		break;
	case YAMPGeneral::GameId::VF2:
		arcadeName = "Virtua Fighter 2";
		baseGameName = "Yakuza: Like a Dragon";
		break;
	default:
		arcadeName = "Virtua Fighter 5: Final Showdown";
		baseGameName = "Yakuza 6";
		break;
	}
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

	// Disclaimers
	ImGui::Separator();
	ImGui::TextUnformatted("ACKNOWLEDGEMENTS:");
	ImGui::TextColored(WARNING_COLOUR, "Yakuza Arcade Machines Player does not redistribute ANY copyrighted files. "
			"You must own an original Steam copy of Yakuza 6: The Song of Life to play games via YAMP. "
			"Pirated game copies WILL NOT receive any support.");

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
			"You must own an original Steam copy of Yakuza 6: The Song of Life to play games via YAMP.\n"
			"Pirated game copies WILL NOT receive any support.");

		ImGui::NewLine();
		ImGui::TextUnformatted("All rights to Virtua Fighter 5: Final Showdown belong to SEGA.");
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
		settings->m_stfDifficulty != m_stfDifficulty ||
		settings->m_stfCountry != m_stfCountry ||
		settings->m_stfFreeplay != m_stfFreeplay ||
		settings->m_stfVersusMode != m_stfVersusMode ||
		settings->m_dontApplyPatches != m_dontApplyPatches ||
		settings->m_useD3DDebugLayer != m_useD3DDebugLayer ||
		settings->m_stfLooseRomFiles != m_stfLooseRomFiles;

	settings->m_resX = m_resolutions[m_currentResolutionIndex].width;
	settings->m_resY = m_resolutions[m_currentResolutionIndex].height;
	settings->m_refreshRate = m_resolutions[m_currentResolutionIndex].refreshRates[m_currentRefRateIndex].refreshRate;
	settings->m_fullscreen = m_currentFullscreen;
	settings->m_enableFpsCap = m_enableFpsCap;

	settings->m_arcadeMode = m_arcadeMode;
	settings->m_circleConfirm = m_circleConfirm;
	settings->m_language = m_language;

	settings->m_stfAspect = m_stfAspect;
	settings->m_stfCrtFilter = m_stfCrtFilter;
	settings->m_stfDifficulty = m_stfDifficulty;
	settings->m_stfCountry = m_stfCountry;
	settings->m_stfFreeplay = m_stfFreeplay;
	settings->m_stfVersusMode = m_stfVersusMode;
	// Bindings are re-read by the game loop every frame, so they apply live.
	settings->m_stfKeyBinds = m_stfKeyBinds;
	settings->m_stfPadBinds = m_stfPadBinds;
	for (int player = 0; player < 2; player++)
	{
		settings->m_stfPadIndex[player] = m_stfPadIndex[player];
	}

	settings->m_dontApplyPatches = m_dontApplyPatches;
	settings->m_useD3DDebugLayer = m_useD3DDebugLayer;
	// The debug-window overlay is read every frame, so it applies live (no restart warning).
	settings->m_stfShowDebugFeatures = m_stfShowDebugFeatures;
	// Enforced every frame while the board is booted, so it also applies live.
	settings->m_stfGameDebugFlag = m_stfGameDebugFlag;
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
