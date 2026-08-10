// The shell of the settings UI - the tab frame, Draw(), the reconciliation tick, apply/
// discard and the disclaimer. Everything else moved to the per-panel TUs (ui/, m2ftg/, pre3/,
// net/, input/). All the shared includes, predicates, helpers and the draw-isolation shims
// live in ui/UiInternal.h, which every panel TU pulls in - this file just uses them.
#include "ui/UiInternal.h"

// Once-per-frame reconciliation of the emulated board against the settings - the debug
// windows, both families' HLE hook masks and the pre3 arcade-settings injection. NOT UI:
// it must run whether or not the settings window is open, which is why Draw() calls it
// before the F1 gate. (Lived inline in Draw() until the 2026-08-09 split.)
void YAMPUserInterface::ReconcileEmulatorState()
{
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
	// The pre3 boards keep their hooks in the PowerPC core's decoded trace rather than in the
	// ROM image, so this reconciles that instead - same once-a-frame, applies-live contract.
	//
	// FORCED TO THE SHIPPED DEFAULT DURING NETPLAY, which is how the two peers are made to agree
	// about it. The mask changes what the board DOES - Patch hooks rewrite instructions, Removed
	// hooks delete them, and the default itself disables two (the start-up warning screen) - so
	// two machines running different masks are running different games. It is a uint64_t[2], which
	// does not fit in the room's flag word, and negotiating it would mean a new wire field for a
	// setting nobody wants to vary mid-match anyway. Pinning it to one value both sides compute
	// locally is the same answer for none of the cost, and it matches how every other board-facing
	// control behaves while a session is up: switched off for the duration.
	// The board's GAME ASSIGNMENTS rows the module cannot carry. Latches itself once applied, so
	// the game's own service menu owns them afterwards.
	{
		pre3::ArcadeSettings::Desired desired;
		const auto* settings = gGeneral.GetSettings();
		desired.country = static_cast<uint8_t>(settings->m_pre3Country);
		desired.cabinetType = static_cast<uint8_t>(settings->m_pre3CabinetType);
		desired.linkId = static_cast<uint8_t>(settings->m_pre3LinkId);
		desired.carNumber = static_cast<uint8_t>(settings->m_pre3CarNumber);
		desired.gameMode = static_cast<uint8_t>(settings->m_pre3GameMode);
		desired.motorPower = static_cast<uint8_t>(settings->m_pre3MotorPower);
		desired.ranking = static_cast<uint8_t>(settings->m_pre3RankingMode);

		// A LIVE LINK OWNS THE LINK ID ROW, on the same rule every other board-facing control
		// follows during a session: switched off for the duration.
		//
		// A linked pair is one MASTER and one SLAVE, exactly as two real cabinets are wired, and
		// the row has to track the comm board's node id because that is what decides which
		// machine is which - it comes from the harness or from a room, and a setting cannot know
		// what the other side ended up as. Leaving them independent lets a cabinet run as node 1
		// while telling its own ROM it is the MASTER.
		if (pre3::CommBoard::CurrentMode() != pre3::CommBoard::Mode::Off)
		{
			desired.linkId = static_cast<uint8_t>(pre3::CommBoard::NodeId() == 0
				? pre3::ArcadeSettings::LinkId::Master
				: pre3::ArcadeSettings::LinkId::Slave);

			// CAR NUMBER FOLLOWS JOIN ORDER, for the same reason the role does: it is a
			// per-cabinet identity in a ring, and two cabinets that pick it independently can
			// pick the same one. The node id already IS join order - the room hands out 0 to the
			// host and 1 to the first guest - so car N belongs to node N, and the row's option
			// index is 0-based over cars 1..8.
			desired.carNumber = pre3::CommBoard::NodeId();
		}

		pre3::ArcadeSettings::SetDesired(desired);
		pre3::ArcadeSettings::Update();
	}

	pre3::HleHooks::Update(net::SessionInProgress()
		? pre3::HleHooks::DefaultDisableMask()
		: gGeneral.GetSettings()->m_stfHleDisableMask);
}

void YAMPUserInterface::Draw()
{
	// In the launcher no game is selected yet: the disclaimer names the parent game, so it
	// stays with the per-game boots (each game's own settings file tracks it separately).
	const bool launcherMode = gGeneral.GetGameId() == YAMPGeneral::GameId::Launcher;

	if (!launcherMode && gGeneral.GetSettings()->m_buildLastShowedDisclaimer < rsc_RevisionID)
	{
		DrawDisclaimer();
	}

	// The emulator-facing per-frame work, before any UI gate - see ReconcileEmulatorState.
	ReconcileEmulatorState();

	// The netplay status has to be visible while the settings window is CLOSED, which is
	// where a session spends all of its time once it is running.
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
	m_netEnabled = settings->m_netEnabled;
	m_vonCabinetRole = settings->m_vonCabinetRole;
	m_vonLinkLog = settings->m_vonLinkLog;
	m_vonHoldLink = settings->m_vonHoldLink;

	m_arcadeMode = settings->m_arcadeMode;
	m_circleConfirm = settings->m_circleConfirm;
	m_language = settings->m_language;
	m_volumePercent = static_cast<int>(settings->m_volumePercent);

	m_m2RenderMode = settings->m_m2RenderMode;
	m_m2WindowMatchesRender = settings->m_m2WindowMatchesRender;
	m_m2Aspect = settings->m_m2Aspect;
	m_m2CrtFilter = settings->m_m2CrtFilter;
	m_pre3RenderScale = settings->m_pre3RenderScale;
	m_m2Difficulty = settings->m_m2Difficulty;
	m_pre3Country = settings->m_pre3Country;
	m_pre3CabinetType = settings->m_pre3CabinetType;
	m_pre3LinkId = settings->m_pre3LinkId;
	m_pre3CarNumber = settings->m_pre3CarNumber;
	m_pre3GameMode = settings->m_pre3GameMode;
	m_pre3MotorPower = settings->m_pre3MotorPower;
	m_pre3RankingMode = settings->m_pre3RankingMode;
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
	m_netPre3VsStart = settings->m_netPre3VsStart;

	m_dontApplyPatches = settings->m_dontApplyPatches;
	m_useD3DDebugLayer = settings->m_useD3DDebugLayer;
	m_stfShowDebugFeatures = settings->m_stfShowDebugFeatures;
	m_stfLooseRomFiles = settings->m_stfLooseRomFiles;
	m_stfGameDebugFlag = settings->m_stfGameDebugFlag;
	m_stfFixBackupTimeIndex = settings->m_stfFixBackupTimeIndex;
	m_stfHleDisableMask[0] = settings->m_stfHleDisableMask[0];
	m_stfHleDisableMask[1] = settings->m_stfHleDisableMask[1];

	// In case non-default Debug options are present, don't nag about the consequences of Debug options for this session.
	// The hook mask is compared against its DEFAULT, not against zero: StF's hook 16 and the pre3
	// boards' boot-screen trio are disabled out of the box (each family's HleHooks.h), and that
	// alone must not read as "the user has been poking at things".
	const uint64_t* hleDefault = pre3::HleHooks::Supported()
		? pre3::HleHooks::DefaultDisableMask()
		: m2ftg::HleHooks::DefaultDisableMask();
	if (m_dontApplyPatches || m_useD3DDebugLayer || m_stfShowDebugFeatures || m_stfLooseRomFiles || m_stfGameDebugFlag ||
		!m_stfFixBackupTimeIndex ||
		m_stfHleDisableMask[0] != hleDefault[0] ||
		m_stfHleDisableMask[1] != hleDefault[1])
	{
		m_debugInfoAccepted.reset();
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

	if (IsPre3Game())
	{
		DrawGamePre3();
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
	// THE RULE: a setting belongs here only if nothing re-reads it while the game runs.
	//
	// Two were listed that do not qualify, and both told the player to restart for a change they
	// could already hear or see:
	//
	//   m_volumePercent - every host writes it into execute_info from the settings struct on EVERY
	//     frame of its game loop (the m2ftg/pre3 float at +0x1C, the VF5FS 0..20 byte). Moving the
	//     slider and pressing Apply is audible immediately.
	//   m_m2RealDamage  - not part of any config block; it is a byte of the ROM's live game
	//     assignments that m2ftg::UpdateDamageAssignment writes once per EMULATED frame. The
	//     comment on its combo box has said so all along.
	//
	// Everything left is read once, at module_start or window creation, and genuinely does need
	// the game relaunching.
	const bool needsRestart =
		settings->m_resX != m_resolutions[m_currentResolutionIndex].width ||
		settings->m_resY != m_resolutions[m_currentResolutionIndex].height ||
		settings->m_refreshRate != m_resolutions[m_currentResolutionIndex].refreshRates[m_currentRefRateIndex].refreshRate ||
		settings->m_fullscreen != m_currentFullscreen ||
		settings->m_enableFpsCap != m_enableFpsCap ||
		settings->m_netEnabled != m_netEnabled ||
		settings->m_vonCabinetRole != m_vonCabinetRole ||
		settings->m_vonLinkLog != m_vonLinkLog ||
		settings->m_vonHoldLink != m_vonHoldLink ||
		settings->m_arcadeMode != m_arcadeMode ||
		settings->m_circleConfirm != m_circleConfirm ||
		settings->m_language != m_language ||
		settings->m_m2RenderMode != m_m2RenderMode ||
		settings->m_m2WindowMatchesRender != m_m2WindowMatchesRender ||
		settings->m_m2Difficulty != m_m2Difficulty ||
		settings->m_pre3Country != m_pre3Country ||
		settings->m_pre3CabinetType != m_pre3CabinetType ||
		settings->m_pre3LinkId != m_pre3LinkId ||
		settings->m_pre3CarNumber != m_pre3CarNumber ||
		settings->m_pre3GameMode != m_pre3GameMode ||
		settings->m_pre3MotorPower != m_pre3MotorPower ||
		settings->m_pre3RankingMode != m_pre3RankingMode ||
		settings->m_m2Country != m_m2Country ||
		settings->m_m2Freeplay != m_m2Freeplay ||
		settings->m_m2VersusMode != m_m2VersusMode ||
		settings->m_vf2Version20 != m_vf2Version20 ||
		settings->m_dontApplyPatches != m_dontApplyPatches ||
		settings->m_useD3DDebugLayer != m_useD3DDebugLayer ||
		settings->m_stfFixBackupTimeIndex != m_stfFixBackupTimeIndex ||
		settings->m_stfLooseRomFiles != m_stfLooseRomFiles;

	settings->m_resX = m_resolutions[m_currentResolutionIndex].width;
	settings->m_resY = m_resolutions[m_currentResolutionIndex].height;
	settings->m_refreshRate = m_resolutions[m_currentResolutionIndex].refreshRates[m_currentRefRateIndex].refreshRate;
	settings->m_fullscreen = m_currentFullscreen;
	settings->m_enableFpsCap = m_enableFpsCap;
	settings->m_netEnabled = m_netEnabled;
	settings->m_vonCabinetRole = m_vonCabinetRole;
	settings->m_vonLinkLog = m_vonLinkLog;
	settings->m_vonHoldLink = m_vonHoldLink;

	settings->m_arcadeMode = m_arcadeMode;
	settings->m_circleConfirm = m_circleConfirm;
	settings->m_language = m_language;
	settings->m_volumePercent = static_cast<uint32_t>(m_volumePercent);

	settings->m_m2RenderMode = m_m2RenderMode;
	settings->m_m2WindowMatchesRender = m_m2WindowMatchesRender;
	settings->m_m2Aspect = m_m2Aspect;
	settings->m_pre3RenderScale = m_pre3RenderScale;
	settings->m_m2CrtFilter = m_m2CrtFilter;
	settings->m_m2Difficulty = m_m2Difficulty;
	settings->m_pre3Country = m_pre3Country;
	settings->m_pre3CabinetType = m_pre3CabinetType;
	settings->m_pre3LinkId = m_pre3LinkId;
	settings->m_pre3CarNumber = m_pre3CarNumber;
	settings->m_pre3GameMode = m_pre3GameMode;
	settings->m_pre3MotorPower = m_pre3MotorPower;
	settings->m_pre3RankingMode = m_pre3RankingMode;
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

	// Consumed by module_start (config.is_vf20), hence the restart warning.
	settings->m_vf2Version20 = m_vf2Version20;

	// Netplay: read when a session connects and when a round starts, so no restart is needed —
	// which is why none of these appear in the needsRestart test above.
	settings->m_netServer = m_netServer;
	settings->m_netNpid = m_netNpid;
	settings->m_netToken = m_netToken;
	settings->m_netCertFingerprint = m_netFingerprint;
	settings->m_netComId = m_netComId;
	settings->m_netFrameDelay = m_netFrameDelay;
	settings->m_netPre3VsStart = m_netPre3VsStart;

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
