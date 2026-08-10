// The About panel (build info + module verification verdicts).
// Split out of YAMPUserInterface.cpp (2026-08-09).

#include "../YAMPUserInterface.h"
#include "UiInternal.h"


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
