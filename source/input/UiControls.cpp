// The Controls page: per-player binding editor and the capture flow (game-neutral despite the Stf names).
// Split out of YAMPUserInterface.cpp (2026-08-09); the class and its page-copy state stay
// in YAMPUserInterface.h - this file only defines the panel methods.

#include "../YAMPUserInterface.h"
#include "../ui/UiInternal.h"


void YAMPUserInterface::DrawControls()
{
	// EVERY hosted game — the m2ftg titles AND all three VF5FS builds — fills its pads through
	// csl_pad::set_state (source/input/Pad.cpp), which reads nothing but the Input bindings.
	// So the real, editable bindings are the correct page for all of them.
	//
	// This used to fall through to a hardcoded list ("P = K, K = L, G = J, Movement = Arrow Keys /\n// WSAD", with a "TODO: Make these controls customizable"). That list predated the VF5FS hosts
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
			// emulated boards have — the m2ftg Model 2 ones and the pre3 Model 3 ones. The VF5FS
			// builds have no I/O to wire them to, so offering the bindings there would just be a
			// row that does nothing.
			if ((action == Input::Action_Test || action == Input::Action_Service)
				&& !IsM2ftgGame() && !IsPre3Game())
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
