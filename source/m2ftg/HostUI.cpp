#include "HostUI.h"

#include "../imgui/imgui.h"

namespace m2ftg
{
	void ApplyAspectSetting(RenderWindow& window, uint32_t mode)
	{
		switch (mode)
		{
		case 1:
			window.SetGameStretchToFill(false);
			window.SetGameAspectRatio(16.0f / 9.0f);
			break;
		case 2:
			window.SetGameStretchToFill(true);
			break;
		default:
			window.SetGameStretchToFill(false);
			window.SetGameAspectRatio(4.0f / 3.0f);
			break;
		}
	}

	bool DrawPauseMenu(RenderWindow& window, bool& menuOpen)
	{
		bool keepRunning = true;

		const ImVec2& displaySize = ImGui::GetIO().DisplaySize;
		ImGui::SetNextWindowPos({ displaySize.x / 2.0f, displaySize.y / 2.0f }, ImGuiCond_Always, { 0.5f, 0.5f });
		if (ImGui::Begin("Paused", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove))
		{
			constexpr float BUTTON_WIDTH = 240.0f;
			if (ImGui::Button("Resume", { BUTTON_WIDTH, 0 }))
			{
				menuOpen = false;
			}
			if (ImGui::Button("View Controls / Settings", { BUTTON_WIDTH, 0 }))
			{
				window.GetUI().OpenSettings();
			}
			if (ImGui::Button("Quit", { BUTTON_WIDTH, 0 }))
			{
				keepRunning = false;
			}
		}
		ImGui::End();

		return keepRunning;
	}
}
