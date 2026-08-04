#pragma once

#include <filesystem>

#include "input/Input.h"
#include "m2ftg/HleHooks.h"

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
	// Master volume, 0..100 %. Applied through each module's OWN volume mechanism rather than
	// by scaling samples host-side, so the game's mixer does the attenuation it was built to do:
	// the m2ftg/Y6 protocols take a float at execute_info+0x1C, the LJ VF5FS module a 0..20 byte
	// at +0x663. 100 is each module's native full scale — what every host passed before this
	// setting existed, so the default changes nothing.
	uint32_t m_volumePercent = 100;

	// Sonic the Fighters (m2ftg module)
	// The module's OWN internal render resolution, index into m2ftg::DISPLAY_MODES
	// (m2ftg/DisplayModes.h). Distinct from m_resX/m_resY, which stay the window and swapchain
	// size: index 1 makes the emulator draw at the arcade board's 496x384 and YAMP presents that
	// upscaled into whatever window you chose. Selected through the module's own command-line
	// option parser, which it reads once during module_start (restart to change).
	uint32_t m_m2RenderMode = 0;
	// Size the window to the module's render resolution instead of [Graphics] ResolutionX/Y, and
	// present 1:1 with no letterboxing - a pixel-exact arcade window. Ignored in fullscreen and
	// for the VF5FS builds, which have no Model 2 render mode.
	bool m_m2WindowMatchesRender = false;
	// Aspect: 0 = 4:3 (original), 1 = 16:9 (stretched), 2 = fill window. Applied live.
	uint32_t m_m2Aspect = 0;

	// Model 3 (pre3 module) internal render scale. pre3 has NO display-mode table: its option
	// parser handles nine switches and not one of them is a resolution (the -vga/-svga/-wxga
	// lines in its usage text are dead, inherited from the arcade source, with no matching option
	// strings and no readers for anything the parser does set). The whole resolution story is
	// config+0x1008, the render HEIGHT, from which the module derives the width at a fixed
	// 496/384. So this is a multiple of the native 384: 0 = match the window height, which is
	// what Like a Dragon Gaiden itself feeds the module; 1..6 pin it to Nx native instead.
	// Read once at module_start (restart to change).
	uint32_t m_pre3RenderScale = 0;
	// Lost Judgment's own CRT shader (exact port), applied in the host blit. Applied live.
	bool m_m2CrtFilter = false;
	// m2ftg_config_t dip switches, read once at module_start (restart to change).
	uint32_t m_m2Difficulty = 1; // 0..3, 1 = arcade default
	// Region the board boots as: 0 = Japan, 1 = USA ("Sonic Championship"), 2 = Export.
	// Seeds the game-assignments country byte (SRAM 0x1D03352 / RAM 0x59C352).
	uint32_t m_m2Country = 0;
	bool m_m2Freeplay = true;
	// Sonic the Fighters' GAME ASSIGNMENTS -> DAMAGE item: false = NORMAL (the cabinet default),
	// true = REAL. Unlike the switches above this one is NOT part of the module's config block -
	// the ROM keeps it as bit 0x80 of game_assignments_flag in emulated RAM (0x59C353), which is
	// also why it applies live rather than needing a restart. StF only; the other m2ftg games have
	// no such assignment. See m2ftg::UpdateDamageAssignment.
	bool m_m2RealDamage = false;
	// is_vs_mode: LJ's 2P-quick-match boot (auto-credits both players, random stage).
	// Off = authentic arcade boot (attract mode, single-player ladder).
	bool m_m2VersusMode = false;
	// Per-player input bindings (see Input.h): a keyboard key and/or a controller button
	// per action, plus which controller each player reads (empty = keyboard only).
	// The game loop re-reads these every frame, so they apply live. The module-facing
	// execute_info.assign table is fixed (Input::MODULE_ASSIGN) - remapping is host-side.
	Input::KeyBinds m_m2KeyBinds = Input::DEFAULT_KEY_BINDS;
	Input::PadBinds m_m2PadBinds = Input::DEFAULT_PAD_BINDS;
	// Input::PadDevice::id, NOT a list index: "xinput:0".."xinput:3" or "dinput:{guid}".
	// Stored as an id so a pad keeps its bindings when other devices are plugged in or out.
	// Defaults to the first two XInput slots, which is what the old integer setting meant.
	std::string m_m2PadId[2] = { "xinput:0", "xinput:1" };

	// Virtua Fighter 2 (YLAD m2ftg module) — the module's own VF2-only config switch
	// (m2ftg_config_t is_vf20). Read at module_start, but also re-read by the backup-RAM injector
	// at every board init, which is how a netplay round adopts the room's version live.
	// (A DisablePepsi switch used to live here; the module never read it — see m2ftg.h +0x08.)
	bool m_vf2Version20 = false;

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
	// Corrects the module's injected backup-RAM TIME byte from a raw second count to the
	// table index the ROM expects, without which the service menu's GAME ASSIGNMENTS page
	// hangs the board (see m2ftg::FixBackupRamTimeIndex). Belongs with HLE hooks 16 and 17,
	// which are disabled by default for the same reason: both read that byte as raw seconds,
	// so this ON with either of them ON gives two-second matches (16) or a two-second attract
	// demo (17). Consumed once at module load.
	bool m_stfFixBackupTimeIndex = true;
	// Which of the module's HLE ROM hooks to keep disabled (bit i = hook i, in the DLL's
	// own table order - see m2ftg::HleHooks). A disabled hook has its original i960
	// instruction restored in the ROM image, so the ROM's own code runs there instead, which
	// is what makes a patched rom_code1.bin take effect. Enforced every frame; applied live.
	uint64_t m_stfHleDisableMask[2] = { 0, 0 };
	// Where each HLE hook should be installed, for program ROMs that are not Sonic the
	// Fighters. Kept as the raw ini text because an entry may name an ELF symbol, which
	// cannot be resolved until game.elf has been loaded - see HleHooks::ResolveRetarget for
	// the accepted forms. Read from the ini's [HleRetarget] section and applied once, before
	// module_start; never written back, so a hand-authored section survives Apply.
	std::string m_stfHleRetarget[m2ftg::HleHooks::MAX_COUNT];

	// --- Netplay (RPCN) ---------------------------------------------------------------------
	// Persisted so a session can be resumed without retyping. m_netToken is the account
	// password: RPCN's Login takes (npid, password, token) and the token is only used when the
	// server has email validation enabled, which it is not by default.
	//
	// NOTE the fingerprint: leave it EMPTY for a server with a real certificate on a real domain,
	// which the plugin then validates (chain + host name) like any HTTPS client. It exists for
	// SELF-SIGNED servers only: RPCN's own `--cert-gen` produces a certificate with CN="RPCN" and
	// no subjectAltName, which ordinary validation can never accept, so it is pinned by SHA-256
	// instead. Connecting to such a server unpinned fails with the fingerprint named in the error
	// (and in yampnet.log), which is where the value to paste here comes from. Never pin a
	// publicly issued certificate - it is reissued at every renewal and the pin would then reject
	// the very server it protects.
	std::string m_netServer;
	std::string m_netNpid;
	std::string m_netToken;
	std::string m_netCertFingerprint;
	// Title id the rooms are scoped to. Any well-formed comm id works (9 uppercase/digits, '_',
	// 2 digits) because the server auto-registers unknown titles when CreateMissing is on.
	std::string m_netComId = "NPWR02113_00";
	// Frames of input delay. Higher hides more latency at the cost of feel; the lockstep engine
	// stalls rather than desyncs if this is too low for the link.
	int m_netFrameDelay = 3;

	// pre3 (Model 3) only: start a round from the board's shipped VERSUS START state rather than
	// from its power-on state. Published with the room, so this is only the value THIS machine
	// offers when it hosts - a guest plays under whatever the host chose.
	//
	// Off by default because the power-on state is the more useful of the two: a round then plays
	// forward through the boot, the attract demo and the credit screen, which is where the AI runs
	// and therefore where a divergence shows itself. Turning it on skips straight into a match.
	bool m_netPre3VsStart = false;

	// ---- Divergence diagnostics -------------------------------------------------------------
	// Both are INI-only on purpose: they exist to run a two-machine measurement, not to be
	// discovered and flipped from the settings panel. See docs/vf2-hle-hooks.md.
	//
	// Writes one line per emulated netplay frame to yampnet.log carrying the emulated i960's
	// four timer channels and the instructions charged to them since the previous frame. That
	// count is the quantity VF2's one-frame divergence is a symptom of, so the two peers' logs
	// diff directly. Off by default: it is a line per frame, forever, on both machines.
	bool m_netTimerTrace = false;
	// Lets a game whose netplay is marked NOT ready still open a round. The only such game is
	// VF2, which is held back precisely because it diverges - so this exists to make that
	// divergence observable, and a match run under it is expected to be wrong.
	bool m_netForceUnsupported = false;

	// Misc
	uint32_t m_buildLastShowedDisclaimer = 0;
};