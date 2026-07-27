#pragma once

#include <optional>
#include <vector>
#include <string>
#include <tuple>

class YAMPUserInterface
{
public:
	YAMPUserInterface() = default;

	void Draw();
	void AddResolution(uint32_t width, uint32_t height, float refreshRate);
	void GetDefaultsFromSettings();

	// Open the settings window programmatically (same window F1 toggles).
	void OpenSettings() { m_settingsOpen = true; }

private:
	void DrawGraphics();
	void DrawGame();
	void DrawGameStF();
	void DrawControls();
	void DrawControlsStF();
	void DrawDebug();
	void DrawAbout();
	bool DrawSettingsConfirmation();

	void DrawDisclaimer();

	bool ProcessF1Key();

	void ApplySettings();
	void DiscardSettings();

private:
	struct Resolution
	{
		struct RefreshRate
		{
			float refreshRate;
			std::string displayString;
		};

		uint32_t width, height;
		std::string displayString;
		std::vector<RefreshRate> refreshRates;
	};

	std::vector<Resolution> m_resolutions;

	// Current settings
	// Graphics
	size_t m_currentResolutionIndex = 0;
	size_t m_currentRefRateIndex = 0;
	bool m_currentFullscreen = false;
	bool m_enableFpsCap = true;

	// Game settings
	// TODO: Subclass once more games are added
	bool m_arcadeMode = false;
	bool m_circleConfirm = false;
	uint32_t m_language = 1;

	// Sonic the Fighters (see YAMPSettings for field semantics)
	uint32_t m_stfAspect = 0;
	bool m_stfCrtFilter = false;
	uint32_t m_stfDifficulty = 1;
	uint32_t m_stfCountry = 0;
	bool m_stfFreeplay = true;
	bool m_stfVersusMode = false;
	uint32_t m_stfAssign[8] = { 2, 3, 4, 4, 5, 6, 7, 8 };

	// Debug settings
	bool m_dontApplyPatches = false;
	bool m_useD3DDebugLayer = false;
	bool m_stfShowDebugFeatures = false;
	bool m_stfLooseRomFiles = false;

	// Volatile state
	bool m_settingsOpen = false, m_pageModified = false, m_showRestartWarning = false;
	float m_lastDisplayW = 0.0f, m_lastDisplayH = 0.0f; // re-center the window when these change
	std::optional<bool> m_debugInfoAccepted { false };
};