#pragma once

#include <array>
#include <optional>
#include <vector>
#include <string>
#include <tuple>

#include "input/Input.h"

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
	void DrawControlsStFPlayer(int player);
	void DrawStfBindingCapture();
	void StartStfCapture(int player, bool wizard, uint32_t action, uint32_t deviceMask);
	void AssignStfKey(int player, uint32_t action, uint32_t vk);
	void AssignStfPadButton(int player, uint32_t action, uint32_t button, int padIndex);
	void DrawDebug();
	// StF only: per-hook control over the module's 76 HLE ROM patches (see m2ftg::HleHooks).
	void DrawStfHleHooks();
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
	int m_volumePercent = 100;

	// Sonic the Fighters (see YAMPSettings for field semantics)
	uint32_t m_m2RenderMode = 0;
	bool m_m2WindowMatchesRender = false;
	uint32_t m_m2Aspect = 0;
	bool m_m2CrtFilter = false;
	uint32_t m_m2Difficulty = 1;
	uint32_t m_m2Country = 0;
	bool m_m2Freeplay = true;
	bool m_m2VersusMode = false;
	Input::KeyBinds m_m2KeyBinds = Input::DEFAULT_KEY_BINDS;
	Input::PadBinds m_m2PadBinds = Input::DEFAULT_PAD_BINDS;
	int32_t m_m2PadIndex[2] = { 0, 1 };

	// Virtua Fighter 2 (see YAMPSettings for field semantics)
	bool m_vf2Version20 = false;
	bool m_vf2DisablePepsi = false;

	// Debug settings
	bool m_dontApplyPatches = false;
	bool m_useD3DDebugLayer = false;
	bool m_stfShowDebugFeatures = false;
	bool m_stfLooseRomFiles = false;
	bool m_stfGameDebugFlag = false;
	uint64_t m_stfHleDisableMask[2] = { 0, 0 };

	// StF binding-capture state ("press a key for X"). The queue holds (action, device mask
	// 1=keyboard 2=controller) prompts; non-empty = the capture popup is live. The prev
	// snapshots make capture edge-triggered so held inputs are not picked up.
	struct StfCapturePrompt
	{
		uint32_t action;
		uint32_t deviceMask;
	};
	std::vector<StfCapturePrompt> m_stfCaptureQueue;
	size_t m_stfCaptureStep = 0;
	int m_stfCapturePlayer = 0;
	bool m_stfCaptureOpenPopup = false;
	std::array<bool, 256> m_stfCapturePrevKeys{};
	uint32_t m_stfCapturePrevPadButtons[4] = {};

	// Volatile state
	bool m_settingsOpen = false, m_pageModified = false, m_showRestartWarning = false;
	float m_lastDisplayW = 0.0f, m_lastDisplayH = 0.0f; // re-center the window when these change
	std::optional<bool> m_debugInfoAccepted { false };
};