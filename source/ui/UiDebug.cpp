// The Debug settings panel (patch toggles, D3D layer, draw isolation).
// Split out of YAMPUserInterface.cpp (2026-08-09).

#include "../YAMPUserInterface.h"
#include "UiInternal.h"


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

	// ---- Draw isolation ---------------------------------------------------------------------
	//
	// "Which of these 200 draws is the artifact?" answered without a graphics debugger. Every
	// module draw whose per-frame index is >= the limit is dropped, so sweeping the slider
	// assembles the scene one draw at a time and the value at which something first appears IS the
	// draw that produces it.
	//
	// DELIBERATELY NOT PART OF THE APPLY FLOW. This writes straight through to the live settings
	// rather than to a page copy, so dragging the slider changes the picture in real time - which
	// is the entire point. Bisecting 200 draws through Apply-and-relaunch is eight rebuilds of
	// your patience; bisecting it by dragging is about four seconds.
	{
		const unsigned int total = ModuleDrawsLastFrameNow();
		if (total != 0)
		{
			// Straight to the hook layer, NOT through the settings object: the mutable settings
			// accessor writes the ini file when its token goes out of scope, and a slider being
			// dragged would rewrite it once per frame.
			int limit = static_cast<int>(ModuleDrawLimitNow());
			// Max is the live draw count, so the slider always spans exactly the draws that exist.
			// It stays accurate while limiting, because the index advances for skipped draws too.
			if (ImGui::SliderInt("Draw limit", &limit, 0, static_cast<int>(total),
				limit == 0 ? "off (all %d draws)" : "%d"))
			{
				SetModuleDrawLimitNow(static_cast<unsigned int>(limit < 0 ? 0 : limit));
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"Draws only the first N of the module's draws this frame, live.\n\n"
					"Sweep it to find which draw produces a rendering artifact: the value at which\n"
					"the artifact first appears is the draw that makes it. 0 draws everything.\n\n"
					"The index is the module's own draw order, which is also the draw order in a\n"
					"PIX capture of the same frame - so the number is directly usable afterwards.\n\n"
					"Not saved by Apply; it is a probe, not a preference.");
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(%u draws this frame)", total);

			// The mode that works on a DEFERRED renderer. pre3 composites its offscreen targets in
			// its last two draws, so a cutoff shows nothing until the composite and then the whole
			// scene at once. Dropping one draw keeps the composite and removes one thing from the
			// picture, which is the question actually being asked.
			const unsigned int NO_SKIP = 0xFFFFFFFFu;
			const unsigned int skipNow = ModuleSkipDrawNow();
			int skip = (skipNow == NO_SKIP) ? -1 : static_cast<int>(skipNow);
			if (ImGui::SliderInt("Skip draw", &skip, -1, static_cast<int>(total) - 1,
				skip < 0 ? "off" : "%d"))
			{
				SetModuleSkipDrawNow(skip < 0 ? NO_SKIP : static_cast<unsigned int>(skip));
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
					"Drops exactly ONE of the module's draws and renders everything else.\n\n"
					"Use this rather than the limit above on this game: pre3 renders into offscreen\n"
					"targets and only composites them in its last two draws, so a cutoff shows a\n"
					"black screen until the composite and then the whole frame at once.\n\n"
					"Sweep it and watch for something to VANISH - that draw is what renders it.");
			}

		}
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
				"IMPORTANT: leave HLE ROM hooks 16 (GAME_INT+0x4) AND 17 (ADV_REPLAY_WAIT1A+0x128) DISABLED while\n"
				"this is on. Both read that same byte as raw seconds instead of as a table index: hook 16 cuts\n"
				"every MATCH to about two seconds, and hook 17 cuts the ATTRACT DEMO to two (it overrides the\n"
				"ROM's own hardcoded 30-second demo). All three off = the module's stock behaviour (working\n"
				"timers, but the menu page freezes).");
		}
	}

	// NOT inside the isStf block: every m2ftg game with a hook table gets this panel - StF, FV,
	// VF2, Virtual On and Motor Raid. It self-gates on HleHooks::Count(), so a game without a
	// table draws nothing rather than an empty list.
	DrawHleHooks();
	// The Model 3 boards' equivalent. Self-gating the same way, and mutually exclusive with the
	// panel above in practice - no game has both kinds of table.
	DrawPre3HleHooks();
}
