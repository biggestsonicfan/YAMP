#pragma once

#include <filesystem>

#include "StFInput.h"

class YAMPSettings
{
public:
	void LoadSettings(const std::filesystem::path& dirPath);
	void SaveSettings(const std::filesystem::path& dirPath);

public:
	// Graphics
	uint32_t m_resX = 1280;
	uint32_t m_resY = 720;
	float m_refreshRate = 60.0f;
	bool m_fullscreen = false;
	bool m_enableFpsCap = true;

	// Game
	// TODO: Subclass once more games are added
	bool m_arcadeMode = false;
	bool m_circleConfirm = false;
	uint32_t m_language = 1; // English

	// Sonic the Fighters (m2ftg module)
	// Aspect: 0 = 4:3 (original), 1 = 16:9 (stretched), 2 = fill window. Applied live.
	uint32_t m_stfAspect = 0;
	// Lost Judgment's own CRT shader (exact port), applied in the host blit. Applied live.
	bool m_stfCrtFilter = false;
	// m2ftg_config_t dip switches, read once at module_start (restart to change).
	uint32_t m_stfDifficulty = 1; // 0..3, 1 = arcade default
	// Region the board boots as: 0 = Japan, 1 = USA ("Sonic Championship"), 2 = Export.
	// Seeds the game-assignments country byte (SRAM 0x1D03352 / RAM 0x59C352).
	uint32_t m_stfCountry = 0;
	bool m_stfFreeplay = true;
	// is_vs_mode: LJ's 2P-quick-match boot (auto-credits both players, random stage).
	// Off = authentic arcade boot (attract mode, single-player ladder).
	bool m_stfVersusMode = false;
	// Per-player input bindings (see StFInput.h): a keyboard key and/or an XInput button
	// per action, plus which XInput controller each player reads (-1 = keyboard only).
	// The game loop re-reads these every frame, so they apply live. The module-facing
	// execute_info.assign table is fixed (StFInput::MODULE_ASSIGN) - remapping is host-side.
	StFInput::KeyBinds m_stfKeyBinds = StFInput::DEFAULT_KEY_BINDS;
	StFInput::PadBinds m_stfPadBinds = StFInput::DEFAULT_PAD_BINDS;
	int32_t m_stfPadIndex[2] = { 0, 1 };

	// Debug settings
	bool m_dontApplyPatches = false;
	bool m_useD3DDebugLayer = false;
	// Shows the StF DLL's own dw debug-menu windows (DEBUG MENU / CONFIG / PERFORMANCE /
	// 960STAT, reconstructed from descriptor data inside the game DLL) as ImGui windows.
	bool m_stfShowDebugFeatures = false;
	// Bypasses rom/stf_rom.par: YAMP hides the archive from the DLL so its mount fails and
	// the engine's archive-miss fallback opens rom/stf_rom/*.bin as plain files instead.
	// Only honoured when all five extracted ROM images are present on disk.
	bool m_stfLooseRomFiles = false;
	// Sets the game's own debug flag in emulated RAM (the dword at 0x508000, flipped by
	// XOR with 0x24), enforced every frame while the board is booted. Applied live.
	bool m_stfGameDebugFlag = false;

	// Misc
	uint32_t m_buildLastShowedDisclaimer = 0;
};