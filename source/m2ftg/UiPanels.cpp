// The m2ftg family settings panels: the shared dip-switch page and the HLE ROM hooks page.
// Split out of YAMPUserInterface.cpp (2026-08-09); the class and its page-copy state stay
// in YAMPUserInterface.h - this file only defines the panel methods.

#include "../YAMPUserInterface.h"
#include "../ui/UiInternal.h"


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

	// FROZEN once a room exists, exactly as Damage is and for the same reason: it is published
	// when the room is CREATED, so from that moment it describes the match rather than this
	// machine. Letting it move could only mean a value that is silently ignored or one peer
	// switching to a different boot mid-match.
	if (net::SessionInProgress())
	{
		const net::Status vsStatus = net::GetStatus();
		ImGui::LabelText("Versus Mode", "%s", vsStatus.vs_mode ? "on" : "off");
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Set by the room's host and fixed for the match.\n"
				"Leave the room to change your own setting.");
		}
	}
	else if (ImGui::Checkbox("Versus Mode", &m_m2VersusMode))
	{
		m_pageModified = true;
	}
	if (!net::SessionInProgress() && ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Boots straight into a credited 2-player versus match, the way\n"
			"%s's minigame runs. Unchecked: authentic arcade boot\n"
			"(attract mode, single-player ladder).\n"
			"Adopted from the room's host during netplay; otherwise needs a restart.",
			gGeneral.GetParentGameName());
	}

	// The VF2 module's own extra config switch (m2ftg_config_t is_vf20;
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

		// A "Disable Pepsi logos" checkbox used to sit here, driving m2ftg_config_t's
		// is_disable_pepsi. Removed 2026-08-02: the module never reads that byte, so the option
		// did nothing at all. See m2ftg.h's config map at +0x08 for the evidence.
	}
}


void YAMPUserInterface::DrawHleHooks()
{
	namespace Hle = m2ftg::HleHooks;

	const size_t hookCount = Hle::Count();
	if (hookCount == 0)
	{
		// No hook table for this game - the panel would be an empty list with misleading prose.
		return;
	}

	if (!ImGui::CollapsingHeader("HLE ROM hooks"))
	{
		return;
	}

	// NOT m2ftg::CurrentGame().display_name: that is the LJ GameDesc table, which only knows
	// StF/FV/MR and silently answers "Sonic the Fighters" for anything else. This panel now
	// draws for VF2 as well, which the LJ table has no entry for.
	const char* gameName = gGeneral.GetArcadeGameName();

	ImGui::PushTextWrapPos();
	ImGui::Text("At board start-up the game DLL overwrites %zu individual i960 instructions in the program ROM "
		"with traps that run native code instead. Disabling a hook hands that address back to the ROM's own code - "
		"which is what a patched rom_code1.bin needs in order to take effect. Applied live.",
		hookCount);
	ImGui::Spacing();
	ImGui::TextUnformatted("How that is done depends on the module: most restore the original instruction into the ROM "
		"image, while Virtual On instead repoints the hook at its own \"execute original\" handler - the one 63 of its "
		"121 hooks already use. Same result, and neither touches anything the module did not already write.");
	ImGui::Spacing();
	ImGui::Text("A wholly different program ROM - homebrew rather than a patch - needs more than that: every "
		"offset below is a %s address, so the installer corrupts %zu unrelated instructions before the "
		"CPU runs any of them, which is too early to repair here. For that, the settings.ini [HleRetarget] section moves "
		"or drops each hook before the installer runs. See the 'At' column.", gameName, hookCount);
	ImGui::PopTextWrapPos();
	ImGui::Spacing();

	auto setMask = [this](unsigned kinds)
	{
		Hle::MaskForKinds(m_stfHleDisableMask, kinds);
		m_pageModified = true;
	};

	// Deliberately "the defaults" rather than "none": hooks 16 and 17 are disabled out of the box
	// because re-enabling either alongside the backup-RAM TIME fix cuts matches (16) or the
	// attract demo (17) to about two seconds - see HleHooks.h. A button that silently re-armed
	// those is the same trap the tooltips warn about. Re-enabling them is still one click away,
	// via their own checkboxes in the list below.
	if (ImGui::Button("Restore defaults"))
	{
		m_stfHleDisableMask[0] = Hle::DefaultDisableMask()[0];
		m_stfHleDisableMask[1] = Hle::DefaultDisableMask()[1];
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		// StF ships two hooks disabled; FV ships none, and saying "except the ones YAMP ships\n// disabled" in front of an empty exception list reads as though something is hidden.
		if (Hle::DefaultDisableMask()[0] != 0 || Hle::DefaultDisableMask()[1] != 0)
		{
			ImGui::SetTooltip("Every hook enabled except the ones YAMP ships disabled - currently hooks 16\n"
				"(GAME_INT+0x4) and 17 (ADV_REPLAY_WAIT1A+0x128). Both fight the backup-RAM TIME\n"
				"correction: 16 would cut matches to about two seconds, 17 the attract demo. Untick\n"
				"them below if you want the module's stock behaviour.");
		}
		else
		{
			ImGui::SetTooltip("Every hook enabled, which is this game's stock behaviour.");
		}
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
	for (size_t i = 0; i < hookCount; i++)
	{
		disabledCount += Hle::MaskTest(m_stfHleDisableMask, i) ? 1 : 0;
	}
	ImGui::Text("%zu of %zu hooks disabled", disabledCount, hookCount);
	ImGui::TextColored(WARNING_COLOUR, "Core hooks are session-only: they apply live but are never saved, so a restart always boots.");
	ImGui::Spacing();

	// Where the installer actually put each trap, which is only the same as the table above when
	// no [HleRetarget] entry moved it. Read live so it also shows a hand-edited ini taking effect.
	// Invocation counters. The rate is the useful number - "once per frame" is the shape a
	// correctly placed render/vsync hook should have - so hold a one-second baseline and show
	// hits/sec next to the running total.
	static uint32_t s_rateBaseline[Hle::MAX_COUNT] = {};
	static float s_ratePerSecond[Hle::MAX_COUNT] = {};
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
			for (size_t i = 0; i < hookCount; i++)
			{
				const uint32_t total = Hle::HitCount(i);
				s_ratePerSecond[i] = static_cast<float>(total - s_rateBaseline[i]) / elapsed;
				s_rateBaseline[i] = total;
			}
			s_lastRateSample = now;
		}
	}

	uint32_t installed[Hle::MAX_COUNT];
	const bool haveInstalled = Hle::GetInstalledOffsets(installed);
	if (haveInstalled)
	{
		size_t retargeted = 0;
		size_t suppressed = 0;
		for (size_t i = 0; i < hookCount; i++)
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
		for (size_t i = 0; i < hookCount; i++)
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

		for (size_t i = 0; i < hookCount; i++)
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
