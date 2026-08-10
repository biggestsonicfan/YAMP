// The Model 3 family settings panels: the pre3 game page and its HLE hooks page.
// Split out of YAMPUserInterface.cpp (2026-08-09); the class and its page-copy state stay
// in YAMPUserInterface.h - this file only defines the panel methods.

#include "../YAMPUserInterface.h"
#include "../ui/UiInternal.h"

// 496x384 is the thing people actually want from an arcade emulator.
void YAMPUserInterface::DrawGamePre3()
{
	{
		const char* labels[] = {
			"Match window (native behaviour)",
			"1x  (496x384)", "2x  (992x768)", "3x  (1488x1152)",
			"4x  (1984x1536)", "5x  (2480x1920)", "6x  (2976x2304)",
		};
		if (m_pre3RenderScale >= std::size(labels))
		{
			m_pre3RenderScale = 0;
		}
		if (ImGui::BeginCombo("Internal resolution", labels[m_pre3RenderScale]))
		{
			for (uint32_t index = 0; index < std::size(labels); index++)
			{
				const bool isSelected = index == m_pre3RenderScale;
				if (ImGui::Selectable(labels[index], isSelected))
				{
					m_pageModified = true;
					m_pre3RenderScale = index;
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("The emulator's own render resolution, upscaled into whatever\n"
				"window you chose. \"Match window\" is what Like a Dragon Gaiden\n"
				"itself does. Requires a restart.");
		}
	}

	if (ImGui::Checkbox("CRT filter", &m_m2CrtFilter))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Lost Judgment's own CRT effect (scanlines + aperture grille).\nApplies immediately.");
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

	// REGION IS NOT A SETTING VIRTUAL ON HAS, so do not offer a control that cannot do anything.
	// Three independent checks agree: the `omg` module never reads `m2ftg_config_t.country`
	// (config+5 has ZERO references, against one apiece for difficulty, free play and kind), the
	// ROM contains no region string at all, and the cabinet's own GAME ASSIGNMENTS page has no
	// region item - PLAY TIME, MATCH COUNT, NETWORK LINK ATTRIBUTE, WINNING BY DECISION, GAME
	// DIFFICULTY, ADVERTIZE SOUND, CONTINUE, REPLAY AND POSING, RANKING, VERSUS ALWAYS FINISH,
	// DISPLAY BRIGHTNESS, INITIALIZE, EXIT is the whole list.
	//
	// Difficulty, by contrast, IS wired and correct on this game, which is worth stating because
	// the two were suspected together: the module reads config+4, adds one, and indexes a
	// five-entry table (`{3,3,0,1,2}` at RVA 0x4500E0) to produce the ROM's backup byte
	// 0x1D00021, whose encoding is 0=NORMAL 1=HARD 2=VERY HARD 3=EASY. So YAMP's 0..3 lands on
	// EASY / NORMAL / HARD / VERY HARD in order - the four labels below, exactly.
	// Sega Racing Classic 2 gets the board's OWN country list instead of this three-entry one.
	// The module's settings injector can only reach three of its six values, so the other three
	// are written into the board directly (pre3::ArcadeSettings) - and once that is happening for
	// COUNTRY anyway there is no reason to show the folded version beside it.
	const bool isSrc2 = gGeneral.GetGameId() == YAMPGeneral::GameId::SRC2;
	if (gGeneral.GetGameId() != YAMPGeneral::GameId::VON_K2 && !isSrc2)
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
			ImGui::SetTooltip("Region the arcade board boots as.\nRequires a restart.");
		}
	}

	// ---- GAME ASSIGNMENTS, Sega Racing Classic 2 ----------------------------------------
	//
	// Every list here is the BOARD's own, read out of its service-menu table at guest 0x0E5548 -
	// each row of that table carries both the strings and the address of the byte it edits - so
	// what these combos offer is exactly what the cabinet's GAME ASSIGNMENTS screen offers, in
	// the same order. See pre3/ArcadeSettings.h.
	if (isSrc2)
	{
		auto boardCombo = [this](const char* label, int& value, const char* const* options,
			int count, const char* tooltip)
		{
			value = value < 0 || value >= count ? 0 : value;
			if (ImGui::BeginCombo(label, options[value]))
			{
				for (int index = 0; index < count; index++)
				{
					const bool isSelected = index == value;
					if (ImGui::Selectable(options[index], isSelected))
					{
						m_pageModified = true;
						value = index;
					}
					if (isSelected) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			if (tooltip != nullptr && ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", tooltip);
			}
		};

		static const char* const COUNTRY[] = { "International", "Japan", "USA", "Export",
			"Australia", "Korea" };
		static const char* const CABINET[] = { "Deluxe", "Twin", "Special" };
		static const char* const LINK_ID[] = { "Single", "Master", "Slave", "Live" };
		static const char* const CAR[] = { "1", "2", "3", "4", "5", "6", "7", "8" };

		static const char* const GAME_MODE[] = { "Normal (Sprint)", "Grand Prix", "100 Miles",
			"200 Miles", "300 Miles", "400 Miles", "500 Miles" };
		static const char* const MOTOR[] = { "50%", "60%", "70%", "80%", "90%", "100%" };
		static const char* const RANKING[] = { "Normal", "Campaign", "Internet" };

		boardCombo("Country", m_pre3Country, COUNTRY, static_cast<int>(std::size(COUNTRY)),
			"COUNTRY on the board's GAME ASSIGNMENTS screen.\nRequires a restart.");
		boardCombo("Cabinet Type", m_pre3CabinetType, CABINET, static_cast<int>(std::size(CABINET)),
			"CABINET TYPE on the board's GAME ASSIGNMENTS screen.\nRequires a restart.");
		boardCombo("Game Mode", m_pre3GameMode, GAME_MODE, static_cast<int>(std::size(GAME_MODE)),
			"GAME MODE on the board's GAME ASSIGNMENTS screen: the race being driven.\n"
			"A netplay room plays under the HOST's value - joining adopts it for the session.\n"
			"Requires a restart.");
		boardCombo("Motor Power", m_pre3MotorPower, MOTOR, static_cast<int>(std::size(MOTOR)),
			"MOTOR POWER on the board's GAME ASSIGNMENTS screen: feedback motor strength.\n"
			"A netplay room plays under the HOST's value - joining adopts it for the session.\n"
			"Requires a restart.");
		boardCombo("Ranking Mode", m_pre3RankingMode, RANKING, static_cast<int>(std::size(RANKING)),
			"RANKING MODE on the board's GAME ASSIGNMENTS screen.\n"
			"A netplay room plays under the HOST's value - joining adopts it for the session.\n"
			"Requires a restart.");

		// LINK ID and CAR NUMBER describe a LINKED cabinet, so netplay owns them once it can run
		// this game - it has to, because the two cabinets must agree on who is master and which
		// car each one is. They are editable now for single-cabinet experimentation and for
		// reaching the board's own linked-cabinet screens; nothing negotiates them yet.
		ImGui::Spacing();
		ImGui::TextDisabled("Linked cabinet (netplay will drive these):");
		boardCombo("Link ID", m_pre3LinkId, LINK_ID, static_cast<int>(std::size(LINK_ID)),
			"LINK ID on the board's GAME ASSIGNMENTS screen.\n"
			"Single is a standalone cabinet; Master/Slave are the two ends of a link.\n"
			"Netplay is not available for this game yet - see docs/src2-hle-hooks.md.\n"
			"Requires a restart.");
		boardCombo("Car Number", m_pre3CarNumber, CAR, static_cast<int>(std::size(CAR)),
			"CAR NUMBER on the board's GAME ASSIGNMENTS screen: which car this cabinet is.\n"
			"Requires a restart.");
		ImGui::Spacing();
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

	if (ImGui::Checkbox("Versus mode", &m_m2VersusMode))
	{
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Boots the board in its versus cabinet configuration.\nRequires a restart.");
	}
}



void YAMPUserInterface::DrawPre3HleHooks()
{
	namespace Hle = pre3::HleHooks;

	const size_t hookCount = Hle::Count();
	if (hookCount == 0)
	{
		return;
	}

	if (!ImGui::CollapsingHeader("HLE ROM hooks"))
	{
		return;
	}

	ImGui::PushTextWrapPos();
	ImGui::Text("The Model 3 board's PowerPC core is a decoded-trace interpreter, and the module hooks %zu "
		"addresses in it by substituting a native handler while that trace is built. Guest memory is never "
		"touched, so unlike the Model 2 boards nothing here is visible in the ROM image. Disabling a hook "
		"restores the trace entry to the instruction the board actually decoded, which hands the code back "
		"to the emulated CPU. Applied live, every frame.", hookCount);
	ImGui::Spacing();
	ImGui::TextUnformatted("There are no ROM symbols for this board, so each row is named by what its handler "
		"does rather than by the routine it replaces - which is knowable exactly, where the name is not.");
	if (net::SessionInProgress())
	{
		ImGui::Spacing();
		ImGui::TextColored(WARNING_COLOUR,
			"Netplay is running, so the board is held at the default mask below and these boxes do "
			"nothing until the session ends. The mask changes what the board does, and two players "
			"on different masks are not playing the same game.");
	}
	ImGui::PopTextWrapPos();
	ImGui::Spacing();

	// Both of these are per-game, and neither can be phrased once: FV2 ships two hooks disabled
	// (the start-up warning screen) and has twenty native routines, SRC2 ships none of either.
	// Asking the table rather than the GameId keeps the next board honest for free.
	const uint64_t* const hleDefault = Hle::DefaultDisableMask();
	const bool shipsAnyDisabled = hleDefault[0] != 0 || hleDefault[1] != 0;
	bool hasNativeRoutines = false;
	for (size_t i = 0; i < hookCount && !hasNativeRoutines; i++)
	{
		hasNativeRoutines = Hle::Get(i).kind == Hle::Kind::Speed;
	}

	if (ImGui::Button("Restore defaults"))
	{
		m_stfHleDisableMask[0] = hleDefault[0];
		m_stfHleDisableMask[1] = hleDefault[1];
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip(shipsAnyDisabled
			? "Every hook on except hooks 7 and 10, which between them skip the board's start-up\n"
			  "warning screen. Like a Dragon Gaiden skips it because the emulator is a minigame there;\n"
			  "YAMP is the cabinet, so the screen the real board shows on power-up is what you get."
			: "Every hook on - this board ships with nothing disabled, so the default is exactly the\n"
			  "module's own table.");
	}
	ImGui::SameLine();
	if (ImGui::Button("Enable all"))
	{
		m_stfHleDisableMask[0] = 0;
		m_stfHleDisableMask[1] = 0;
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip(shipsAnyDisabled
			? "Every hook on, including the boot-screen skip - i.e. exactly what the module\n"
			  "does inside Like a Dragon Gaiden."
			: "Every hook on - i.e. exactly what the module does inside Like a Dragon Gaiden.\n"
			  "For this board that is also the default.");
	}
	// Hidden rather than disabled when the game has no native routines: a greyed button invites
	// the question "why not?", and the honest answer is that there is nothing for it to turn off.
	if (hasNativeRoutines)
	{
		ImGui::SameLine();
		if (ImGui::Button("Disable native routines"))
		{
			Hle::MaskForKinds(m_stfHleDisableMask, Hle::NATIVE_ROUTINE_KINDS);
			m_pageModified = true;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Every whole-routine native reimplementation off, so the emulated PowerPC runs the\n"
				"ROM's own code instead. Slower, and the way to find out whether one of them is wrong.");
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Disable all"))
	{
		Hle::MaskForKinds(m_stfHleDisableMask, Hle::ALL_KINDS);
		m_pageModified = true;
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Everything off, including any row marked !. On this board that stops the\n"
			"boot outright - which is the point of the button, but it is not a state you can\n"
			"play in. The boot-critical rows are not saved, so a restart recovers on its own.");
	}

	size_t disabledCount = 0;
	size_t criticalDisabled = 0;
	for (size_t i = 0; i < hookCount; i++)
	{
		if (!Hle::MaskTest(m_stfHleDisableMask, i)) continue;
		disabledCount++;
		criticalDisabled += Hle::BootCritical(i) ? 1 : 0;
	}
	ImGui::Text("%zu of %zu hooks disabled", disabledCount, hookCount);
	if (criticalDisabled != 0)
	{
		// Said here as well as in the row tooltip, because this is the state where the board
		// stops and the reason is one scroll away.
		ImGui::TextColored(WARNING_COLOUR,
			"%zu of them are boot-critical (!) - the board will not start like this. Not saved, "
			"so restarting clears it.", criticalDisabled);
	}
	if (!Hle::Live())
	{
		// Before the board is up there is no trace to patch, and after a table mismatch the
		// module refuses to touch one. Say which, rather than letting the checkboxes look live.
		if (Hle::LiveRecordCount() != 0 && Hle::LiveRecordCount() != hookCount)
		{
			ImGui::TextColored(WARNING_COLOUR,
				"The module's live table has %zu records, not %zu - these descriptions are for a "
				"different build, so nothing is being applied.", Hle::LiveRecordCount(), hookCount);
		}
		else
		{
			ImGui::TextDisabled("Waiting for the board to start - changes apply as soon as it does.");
		}
	}
	ImGui::Spacing();

	if (ImGui::BeginTable("##pre3hle", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
		| ImGuiTableFlags_ScrollY, ImVec2(0.0f, 320.0f)))
	{
		ImGui::TableSetupColumn("Off", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("What the handler does", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupScrollFreeze(0, 1);
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

			const bool bootCritical = Hle::BootCritical(i);

			ImGui::TableNextColumn();
			if (bootCritical)
			{
				// The marker goes on the INDEX rather than the Kind, because Kind is exactly what
				// fails to predict this: SRC2's only Core hook is the safest in the table, and the
				// three that stop the boot are two Removed and one Host.
				ImGui::TextColored(WARNING_COLOUR, "%zu !", i);
			}
			else
			{
				ImGui::Text("%zu", i);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(bootCritical
					? "BOOT-CRITICAL, measured one hook at a time. Disable this and the board does\n"
					  "not start. You can still turn it off to experiment - it applies immediately -\n"
					  "but it is never written to settings.ini, so the next launch always boots."
					: "Position in the module's own hook table.");
			}

			ImGui::TableNextColumn();
			if (info.kind == Hle::Kind::Core || bootCritical)
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
			ImGui::Text("%06X", info.guestAddress);
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Guest address in the board's code.\nHandler: module + 0x%X",
					info.handlerRva);
			}

			ImGui::TableNextColumn();
			ImGui::TextUnformatted(info.note);

			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}
