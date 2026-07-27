#pragma once

#include <filesystem>

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
	// execute_info.assign button assignments, in MODULE SLOT order: A, B, Y, X, LT, LB, RT, RB
	// (slot->pad-button map from the DLL's slot template @0x180126770). Values are
	// m2ftg_execute_info_t::assign_t (1=none, 2=P, 3=K, 4=G, 5=PG, 6=PKG, 7=PK, 8=KG).
	// Defaults follow the module's own template except X, which we default to G so the
	// documented J key (X button) guards on keyboard. Applied live.
	uint32_t m_stfAssign[8] = { 2, 3, 4, 4, 5, 6, 7, 8 };

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

	// Misc
	uint32_t m_buildLastShowedDisclaimer = 0;
};