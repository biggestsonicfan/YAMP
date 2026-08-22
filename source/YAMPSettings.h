#pragma once

#include <filesystem>

#include "input/Input.h"
#include "m2ftg/Debug/HleHooks.h"

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

	// Virtual On (Kiwami 2 "omg" module) — which of the module's five pad-to-twin-stick control
	// schemes to play with. The module reads execute_info+0x15E4 (assign[0][4]) as an index into
	// its own 0xC0-byte mapping table at DLL+0x12ABB0 EVERY frame, so this applies live. 0-4;
	// 3 is the full discrete twin-stick mapping (the cabinet's actual control set), 0/1 are the
	// beginner schemes where one button expands into a pre-composed two-stick gesture. See the
	// assign[0][4] note in K2Host's frame loop for the per-entry decode.
	uint32_t m_vonControlScheme = 3;

	// Virtual On — the SEGA SATURN TWIN STICK OVERRIDE. On, and with a stick attached to a
	// Bliss-Box, a player's pad is filled from the real levers instead of from their key/pad
	// bindings, and m_vonControlScheme is forced to the full twin-stick entry for that frame. It
	// is an override rather than a sixth control scheme because it does not go through the
	// module's mapping table in the sense the other five do: the stick has one switch per cabinet
	// input, so there is nothing to map. See m2ftg/K2/VonTwinStick.cpp.
	//
	// The port per player: -1 = Auto (first capable port to P1, the next to P2), otherwise a
	// Bliss-Box port index 0..3. Explicit picks are honoured even when that port is empty — see
	// TwinStick::PortForPlayer for why sliding a player onto another port would be worse.
	bool m_vonTwinStick = false;
	int32_t m_vonTwinStickPort[2] = { -1, -1 };

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
	// [Debug] AllowBootCriticalHle - advanced, hand-authored, and deliberately not a checkbox, on
	// the same footing as HleRetarget. Lets a pre3 boot-critical hook stay disabled in the ini
	// instead of being stripped on the way in and out. The strip exists so the panel cannot brick
	// the game; this is how someone with a debugger opts out of it, and it is the only way to do so
	// under x64dbg, which cannot pass the YAMP_PRE3_ALLOW_CRITICAL environment variable through.
	bool m_allowBootCriticalHle = false;

	// Sega Racing Classic 2's GAME ASSIGNMENTS, for the rows the module's settings injector cannot
	// reach. Values are the BOARD's own option indices, so the UI can offer its strings verbatim -
	// see pre3/ArcadeSettings.h for where each list comes from. Defaults match what the board
	// itself boots with (JAPAN / TWIN / SINGLE / car 1 / SPRINT / 80% / NORMAL ranking).
	int m_pre3Country = 1;
	int m_pre3CabinetType = 1;
	int m_pre3LinkId = 0;
	int m_pre3CarNumber = 0;
	// The three rows a netplay room also publishes and adopts (see net::Src2Assignments); these
	// are the LOCAL preferences the board boots with outside a session.
	int m_pre3GameMode = 0;      // 0..6  NORMAL(SPRINT) / GRAND PRIX / 100..500 MILES
	int m_pre3MotorPower = 3;    // 0..5  50% .. 100%; 3 = the board's own 80% default
	int m_pre3RankingMode = 0;   // 0..2  NORMAL / CAMPAIGN / INTERNET
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
	// NETPLAY MODE FOR THIS LAUNCH. Read once at start-up, never mid-session.
	//
	// It exists because a LINKED-CABINET game cannot change its mind about how many Model 2 boards
	// it is running. Virtual On brings up its second cabinet only when this is set, because an idle
	// second board is not neutral - it is a peer the ROM waits for, and the operator's menu
	// deadlocks against it. Bringing one up on a live board is the transition that has to be
	// avoided, so the answer is fixed for the process and changing it means restarting the game.
	//
	// Off by default: the common case is one person at one cabinet.
	bool m_netEnabled = false;

	// WHICH CABINET OF THE LINKED PAIR THIS MACHINE IS. Virtual On only; read once at start-up.
	//
	//     0  NOLINK   the module's own answer - a standalone cabinet
	//     1  MASTER
	//     2  SLAVE
	//
	// These are YAMP's numbers, chosen to read in the obvious order; the module's backup-RAM byte
	// uses a different encoding (see K2Host::ApplyCabinetRole) and the ROM a third one again
	// (link_ID: 1 = master site, 2 = slave site, 3 = standalone). Do not assume any two agree.
	//
	// Why a setting and not a command-line flag: the role belongs to the MACHINE, so it is set once
	// per install and wanted on every ordinary launch - including one started from the game
	// launcher, which passes only the game's boot argument and so cannot carry a flag at all.
	// Restart-scoped for the same reason as m_netEnabled above: the ROM reads the byte once, in
	// InitNetwork, several hundred frames before anyone can open this page.
	uint32_t m_vonCabinetRole = 0;

	// Log the linked-cabinet state - both boards' link_ID, net_flag, MainMode, GlobCntr, comm
	// flags and payload counts - every 200 frames. This is what says whether the cabinet role
	// above actually took, and whether a link came up.
	//
	// A SETTING RATHER THAN THE `-von-2probe` FLAG IT REPLACES, because the game launcher passes
	// only the game's boot argument: anything behind a command-line flag is invisible to a run
	// started the normal way, which is how the first two-machine tests silently ran with no
	// diagnostics at all on either peer (docs/von-netplay-recon.md records the cost). A
	// diagnostic that cannot be turned on in the situation you need it is not a diagnostic.
	bool m_vonLinkLog = false;

	// Hold the comm board's "ring up" byte at 0, so the ROM's own link check waits instead of
	// succeeding against the fake healthy ring the module leaves in the status bytes. The board
	// sits in "Checking Network Now", yielding every frame, until something releases it - which
	// is the state a pair of cabinets is in before the other one is switched on.
	//
	// Off by default: with nothing to release it, the game waits forever. See K2Host::HoldCommRing.
	bool m_vonHoldLink = false;

	// ---- Draw isolation (rendering bugs) ----------------------------------------------------
	// Drop every module draw whose per-frame index is >= this; 0 draws everything. Sweeping it
	// assembles the scene one draw at a time, so the value at which an artifact first appears is
	// the draw that produces it - see the note on g_drawLimit in pxd/LJ/HostCdevice.cpp. The index
	// matches the draw order in a PIX capture of the same frame, which is what makes the answer
	// useful afterwards: it names the draw whose textures and constants to go and read.
	//
	// INI-only, like the netplay diagnostics above: it is a probe, not a preference.
	uint32_t m_drawLimit = 0;

	// Misc
	uint32_t m_buildLastShowedDisclaimer = 0;
};