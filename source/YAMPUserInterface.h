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
	// ...and query/close it, so a host's Escape key can dismiss settings instead of doing its
	// own thing (the launcher's Escape quits, and must not quit out from under a user who was
	// only backing out of the settings window).
	bool IsSettingsOpen() const { return m_settingsOpen; }
	void CloseSettings() { m_settingsOpen = false; }

private:
	void DrawGraphics();
	void DrawGame();
	void DrawGameStF();
	// The two Model 3 games (pre3). See the comment on the definition.
	void DrawGamePre3();
	void DrawControls();
	void DrawControlsStF();
	void DrawControlsStFPlayer(int player);
	void DrawStfBindingCapture();
	void StartStfCapture(int player, bool wizard, uint32_t action, uint32_t deviceMask);
	void AssignStfKey(int player, uint32_t action, uint32_t vk);
	void AssignStfPadButton(int player, uint32_t action, uint32_t button, const std::string& padId);
	void DrawDebug();
	// Per-hook control over the module's HLE ROM patches, for every LJ m2ftg game that has a
	// hook table - StF (76 hooks) and FV (95). Self-gating: draws nothing when there is none.
	void DrawStfHleHooks();
	void DrawPre3HleHooks();
	// The netplay page: persisted RPCN account settings on top, the live lobby below.
	void DrawNetplay();
	// The lobby's status line as an in-game overlay, so a connecting/waiting session is visible
	// with the settings window closed. A stalled or pre-match frame renders nothing new, which
	// looks exactly like a hang without this.
	void DrawNetplayOverlay();
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
	// Netplay mode for the NEXT launch; see YAMPSettings::m_netEnabled.
	bool m_netEnabled = false;
	// Which cabinet of the linked pair this machine is; see YAMPSettings::m_vonCabinetRole.
	uint32_t m_vonCabinetRole = 0;
	// See YAMPSettings::m_vonLinkLog.
	bool m_vonLinkLog = false;
	// See YAMPSettings::m_vonHoldLink.
	bool m_vonHoldLink = false;

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
	// Model 3 (pre3) internal render scale; 0 = match the window. See YAMPSettings.h.
	uint32_t m_pre3RenderScale = 0;
	bool m_m2CrtFilter = false;
	uint32_t m_m2Difficulty = 1;
	// Sega Racing Classic 2's GAME ASSIGNMENTS rows, as the board's own option indices.
	int m_pre3Country = 1;
	int m_pre3CabinetType = 1;
	int m_pre3LinkId = 0;
	int m_pre3CarNumber = 0;
	uint32_t m_m2Country = 0;
	bool m_m2Freeplay = true;
	bool m_m2VersusMode = false;
	bool m_m2RealDamage = false;
	Input::KeyBinds m_m2KeyBinds = Input::DEFAULT_KEY_BINDS;
	Input::PadBinds m_m2PadBinds = Input::DEFAULT_PAD_BINDS;
	std::string m_m2PadId[2] = { "xinput:0", "xinput:1" };

	// Virtua Fighter 2 (see YAMPSettings for field semantics)
	bool m_vf2Version20 = false;

	// Netplay (see YAMPSettings for field semantics). Char buffers rather than std::string:
	// this ImGui build ships no std::string InputText helper, and the sizes double as the limits
	// the RPCN protocol imposes anyway.
	char m_netServer[64] = {};
	char m_netNpid[24] = {};
	char m_netToken[64] = {};
	char m_netFingerprint[72] = {};
	char m_netComId[16] = {};
	int m_netFrameDelay = 3;
	// pre3 only: what this machine publishes as the round-start state when it HOSTS.
	bool m_netPre3VsStart = false;
	// Lobby-only state: never persisted, never part of the Apply flow.
	bool m_netShowToken = false;
	char m_netJoinRoomId[24] = {};
	// RPCN passwords are a fixed 8 bytes (SceNpMatching2SessionPassword); anything longer is
	// truncated on the wire, so the field says so rather than silently losing characters.
	char m_netRoomPassword[9] = {};
	unsigned long long m_netSelectedRoom = 0;

	// Debug settings
	bool m_dontApplyPatches = false;
	bool m_useD3DDebugLayer = false;
	bool m_stfShowDebugFeatures = false;
	bool m_stfLooseRomFiles = false;
	bool m_stfGameDebugFlag = false;
	bool m_stfFixBackupTimeIndex = true;
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
	// Edge detectors for the capture popup, one per attached controller (Input::Devices()
	// order), so an input already held when the prompt opens is not read as a press.
	std::vector<uint64_t> m_stfCapturePrevPadButtons;

	// Volatile state
	bool m_settingsOpen = false, m_pageModified = false, m_showRestartWarning = false;
	float m_lastDisplayW = 0.0f, m_lastDisplayH = 0.0f; // re-center the window when these change
	std::optional<bool> m_debugInfoAccepted { false };
};