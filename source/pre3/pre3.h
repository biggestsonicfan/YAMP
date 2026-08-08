#pragma once

// Shared pre3 ("Model 3 emulator") module-hosting structures.
//
// pre3 is Like a Dragon Gaiden's SECOND arcade emulator, sitting beside the Model 2 m2ftg
// modules in runtime/media/pre3/. It is not a separate engine: the module's own PDB path is
//
//     S:\Project\pre3\m2ftg_pxd\m2ftg-pxd\.out\pre3-pxd\w64\d3d12_Retail\pre3-pxd-w64-d3d12_Retail.pdb
//
// so it is a sibling target inside the same m2ftg-pxd source tree, and its app-module class is
// still called M2FTGAppModule (the name survives in the module's RTTI). Consequently the whole
// host-facing protocol — module_start/module_stop, the params block, the sl/gs/ct module
// descriptors, the icri pointer, the per-frame execute callback — is the one ../m2ftg/m2ftg.h
// already describes, and the pxd platform layer underneath is the SAME GENERATION as Like a
// Dragon Gaiden's Sonic the Fighters module (sl::context_t 0xF140, gs::context_t 0x3898C0, both
// re-read out of this module's own initialize_module size checks). That is why this header
// borrows pxd/LJ wholesale rather than describing a new platform.
//
// What genuinely differs from m2ftg, and therefore lives here:
//   * the config blob is 0x1028 rather than 0x100C, with an entirely different field set;
//   * execute_info is 0x1780 rather than 0x1760, with FOUR pad slots and a save-data region;
//   * one module hosts SIX games, selected by a config byte, instead of one game per DLL.
//
// Unlike the m2ftg modules there are no game symbols to lean on, but the module carries full
// MSVC RTTI (M3ERom / M3ERomFv2 / CImgFv2 / CPpc603e / CScr / CM3Mem / CXComm / M3EInput / ...),
// which is what made the emulator side legible.

#include <cstdint>
#include <cstddef>

// The per-pad block is the shared LJ-era pad layout (pxd::lj_pad_t), unchanged here.
#include "../pxd/LJ/sl.h"

namespace pxd
{
	class cgs_device_context;
}

namespace pre3
{
	using pxd::cgs_device_context;

	// Which game the module boots, from config.game. module_start indexes a six-entry name
	// table with it (guarded `< 6`) to build "%s/image/%s.par", and the ROM factory
	// (DLL 0x180038720) switches on the same byte to construct the matching M3ERom subclass:
	//
	//   0 vf3tb   M3ERomVf3tb   Virtua Fighter 3 Team Battle
	//   1 vf3a    M3ERomVf3a    Virtua Fighter 3 (version A)
	//   2 getbass M3ERomBass    Get Bass / Sega Bass Fishing
	//   3 spkfe   M3ERomSpkfe   Spikeout Final Edition
	//   4 fv2     M3ERomFv2     Fighting Vipers 2
	//   5 src2    M3ERomSrc2    Sega Racing Classic 2
	//
	// Like a Dragon Gaiden only ships image/fv2.par and image/src2.par, so only those two are
	// playable from a stock install; the other four are compiled in but have no data.
	enum class Game : uint8_t
	{
		Vf3tb = 0,
		Vf3a = 1,
		GetBass = 2,
		Spkfe = 3,
		Fv2 = 4,
		Src2 = 5,
	};

	// module_start copies 0x1028 bytes from params+0x38 into its config global (DLL 0x18018A050).
	//
	// Every field below is a BYTE — confirmed from the readers' encodings (`movzx eax, byte ptr`
	// / `cmp byte ptr`), not assumed from the m2ftg layout, which packs its settings the same way
	// but means different things by them.
	//
	// The settings bytes are consumed by the board's backup-RAM initialiser (DLL 0x180026840),
	// which writes them into emulated NVRAM at 0xF00DE0xx through the memory-access interface.
	// Names below are read off THOSE writes; where the arcade meaning of an NVRAM address is not
	// certain the field says so rather than guessing a nicer name.
	struct pre3_config_t
	{
		//   +0x00  which of the six games to boot (see Game above).
		uint8_t game = static_cast<uint8_t>(Game::Fv2);

		//   +0x01  difficulty. Folded 0/1 -> 0, 2 -> 1, 3 -> 2, anything else -> 3 before being
		//          written to NVRAM 0xF00DE005, so it is a four-step scale with 0 and 1 aliased.
		uint8_t difficulty = 1;

		//   +0x02  passed through 0/1/2 (anything else -> 0) to NVRAM 0xF00DE01D. Position in the
		//          initialiser matches m2ftg's country/region byte; not yet confirmed against the
		//          board's own service menu, so it keeps a descriptive name.
		uint8_t region = 0;

		//   +0x03  bypasses the "%s/image/%s.par" archive load in module_start entirely when
		//          non-zero, leaving the ROM images to be served as loose files — the same shape
		//          as Sonic the Fighters' loose-ROM debug toggle. YAMP leaves it clear.
		uint8_t loose_rom_files = 0;

		//   +0x04  free play. Selects the coin setting written to NVRAM 0xF00DE030 (1 when clear,
		//          0x1B when set) and drives the companion flag at 0xF00DE038.
		uint8_t is_freeplay = 1;

		//   +0x05  linked-cabinet / VS mode, NVRAM 0xF00DE01C. The initialiser FORCES this on
		//          whenever the optional link interface (params+0x1070) is present, so the host
		//          only gets a say when it passes no link interface — which YAMP currently does.
		uint8_t is_vs_mode = 0;

		uint8_t reserved_06 = 0;
		uint8_t reserved_07 = 0;

		//   +0x08  0x1000-byte block, address-taken by the module rather than read field by
		//          field. Left zero, which is what a fresh config looks like.
		std::byte settings[0x1008 - 0x08]{};

		//   +0x1008  RENDER HEIGHT in pixels, and the field that makes the module usable at all.
		//          The app-init step (DLL 0x180006D60) reads it, converts it to float and derives
		//          the width as `height * 1.291666…` — that constant (DLL 0x18010D4D8) is
		//          496/384, the Model 3 display aspect — then creates its six render targets at
		//          that size. Zero here means a 0x0 B8G8R8A8 target, which D3D12 rejects; the
		//          module's failure path for that is an int3, so it takes the process with it.
		//
		//          The width is DERIVED, so this is the only resolution control there is: 384
		//          gives the native 496x384, and larger values supersample.
		//
		//          A PIX capture of Like a Dragon Gaiden running Fighting Vipers 2 shows the
		//          module's colour target and depth buffer both at 1162x900, and
		//          900 * (496/384) = 1162.5 -> 1162 — the formula confirmed end to end. That 900
		//          was Gaiden's own OUTPUT height at capture time (a windowed session), not a
		//          constant the module cares about, so the host derives this from its window
		//          rather than hardcoding it. The default here is only the fallback.
		int32_t render_height = 900;

		//   +0x100C  THIS CABINET'S NODE ID in the link ring — 0 is the master. Named
		//          `link_reserved` until 2026-08-07, when the comm board was traced: the field has
		//          no writer inside the module because it is simply a config byte module_start
		//          copies, and the emulated network board latches it as its node id in state 1 of
		//          its transfer machine (config global DLL 0x18018A050 + 0x100C = DAT_18018B05C,
		//          read by FUN_180035BA0). The backup-RAM initialiser's "takes the link path only
		//          when this is zero" is the same field being tested for master.
		int32_t link_node_id = 0;

		//   +0x1010  peer count — how many cabinets are in the ring. The backup-RAM initialiser
		//          only engages the link interface when this is greater than 1, and the comm
		//          board's transfer machine takes the stand-alone branch on the same test.
		int32_t link_peer_count = 0;

		std::byte tail[0x1028 - 0x1014]{};
	};
	static_assert(sizeof(pre3_config_t) == 0x1028);
	static_assert(offsetof(pre3_config_t, is_freeplay) == 0x04);
	static_assert(offsetof(pre3_config_t, settings) == 0x08);
	static_assert(offsetof(pre3_config_t, render_height) == 0x1008);
	static_assert(offsetof(pre3_config_t, link_peer_count) == 0x1010);

	// What the module's own aspect constant works out to — the native Model 3 display.
	inline constexpr int NATIVE_HEIGHT = 384;
	inline constexpr int NATIVE_WIDTH = 496;

	// Same pad block as every other LJ-era module — see ../pxd/LJ/sl.h.
	using pre3_pad_t = pxd::lj_pad_t;
	static_assert(sizeof(pre3_pad_t) == 0x190);

	// pre3 execute_info. The module rejects any size but 0x1780 (`CMP RCX,0x1780` at the top of
	// all three entry points: the per-frame callback 0x1800423C0 and the two optional ones at
	// 0x180042690 / 0x1800427E0), and the header fields are the m2ftg ones unchanged:
	//
	//   +0x00 size, +0x08 device context, +0x10 status, +0x14 result, +0x18 output_texid,
	//   +0x1C sound volume.
	//
	// status bits behave as in m2ftg: host->module bit0 = pause and bit5 (0x20) = coin inserted;
	// module->host bit6 (0x40) = "insert coin / press start" active. The frame callback also
	// tests bit7 alongside bit0 (`status & 0x81`) when deciding whether to pause the audio
	// channels, so 0x80 is a second host->module suspend input; YAMP does not drive it.
	//
	// MUST PERSIST ACROSS FRAMES — the module keeps state in it, exactly like m2ftg's.
	struct alignas(16) pre3_execute_info_t
	{
		size_t size_of_struct;
		cgs_device_context* p_device_context;
		int status;
		int result;
		unsigned int output_texid;
		float sound_volume;

		// FOUR pads, not m2ftg's two. Model 3 hosts four-player cabinets (Sega Rally 2's linked
		// seats, Spikeout's four-player mode), and the arithmetic is exact: the pads run from
		// +0x20 up to the save-data region the module memcpys into at +0x660, and
		// 0x660 - 0x20 = 0x640 = 4 * 0x190. Fighting Vipers 2 only ever reads the first two.
		pre3_pad_t pad[4];

		// +0x660: save data. The module WRITES here — 0x1000 bytes from its NVRAM image
		// (DLL 0x1800379A0) and a 0x750-byte block elsewhere (0x1800358F0) — so this is the
		// board's backup RAM on its way back out to the host, not something the host fills in.
		// Sized to reach the work block; the two observed writes both fit inside it.
		std::byte save_data[0x1660 - 0x660];

		// +0x1660: the "work" block, at the same offset m2ftg puts its own. The first three
		// qwords are NULLABLE POINTERS the module reads and null-checks (0x180035BA0), so they
		// are host-provided rather than module-owned.
		//
		// THEY ARE THE LINKED-CABINET DATA PATH, and the function that null-checks them is the
		// comm board's own transfer machine. Traced 2026-08-07; see CommBoard.h and
		// docs/src2-netplay-recon.md:
		//
		//   [0]  TX ARRAY   the emulated network board copies its outgoing packet INTO it, at
		//                   slot [config.link_node_id]
		//   [1]  RX ARRAY   it copies every node's packet OUT of it, indexed by node id
		//   [2]  points at a u64 RENDEZVOUS WORD shared by every cabinet: bits 0..n-1 are
		//        "node i's packet is present" (the module only ever WAITS on these), bit
		//        nodeId+0x20 is the guest's 0xF000 acknowledgement, bit nodeId+0x30 is
		//        "node i has finished booting" and gates the machine's own boot barrier.
		//
		// Both arrays are `link_peer_count * packetSize`, where packetSize is a register the GUEST
		// programs at 0xC0020808 — so a host that fills these in does not get to choose the
		// length. YAMP leaves all three null unless CommBoard is running, which is the path the
		// module already handles and is what every non-linked game gets.
		//
		// +0x167C and +0x1680 are the two volume scalars the frame
		// callback multiplies sound_volume by when it sets the six audio channels (channel 5
		// takes +0x167C, channels 0-4 take +0x1680), and +0x1694 is a byte the module writes.
		void* p_work_ptr[3];
		std::byte gap_1678[0x167C - 0x1678];
		float volume_channel5;
		float volume_channels0_4;

		// +0x1684: the BUTTON ASSIGN TABLE, one row of eight per player, and the same thing
		// m2ftg's execute_info.assign is — same assign_t values, same eight module slots, in the
		// same order (see Input::MODULE_ASSIGN, which transfers verbatim).
		//
		// It reaches the board as part of one block copy: M3EInput's per-frame update (DLL
		// 0x1800336D0) memcpys 0x100 bytes from execute_info+0x1678 into the board object at
		// +0x368, which is why the two volume scalars above sit immediately in front of it. The
		// game's own button mapper (FV2: DLL 0x180037090) then walks the eight slot masks the
		// M3EInput constructor set up —
		//
		//     slot 0..7 mask = 0x08, 0x02, 0x04, 0x01, 0x40, 0x10, 0x80, 0x20
		//
		// tested against the pad's m_now UNPERMUTED, so in sl terms the slots are Y, B, X, A, LT,
		// LB, RT, RB — exactly the order Input::MODULE_ASSIGN is written in — and ORs the assigned
		// P/K/G combination into the player's input register.
		//
		// A ZERO row matches none of the mapper's cases, so leaving this unset does not produce a
		// default layout: it produces a board on which no attack button exists at all, while the
		// lever and START (which the shared base mapper handles) keep working. Directly visible in
		// the board's own input test.
		uint8_t assign[2][8];

		// +0x1694: the module->host TELEMETRY block, which is how Like a Dragon Gaiden tracks its
		// arcade minigame (rank reached, character used, whether a match finished). Written by
		// each game's own per-frame input override — FV2's is DLL 0x180036BF0 — from guest RAM.
		// YAMP has no use for most of it, but the two BYTES below are worth naming because they
		// are the only cheap window the host has into what the board is doing.

		// The board's current game mode and sub-mode, read straight out of guest RAM every frame
		// (FV2: 0x58FD98 / 0x58FD99). Purely informational — nothing the host does depends on
		// them — but they change the instant the ROM switches screens, which makes them the
		// headless way to tell that an input actually reached the board.
		uint8_t game_mode;
		uint8_t game_sub_mode;

		std::byte tail[0x1780 - 0x1696];
	};
	static_assert(sizeof(pre3_execute_info_t) == 0x1780);
	static_assert(offsetof(pre3_execute_info_t, sound_volume) == 0x1C);
	static_assert(offsetof(pre3_execute_info_t, pad) == 0x20);
	static_assert(offsetof(pre3_execute_info_t, pad[1]) == 0x1B0);
	static_assert(offsetof(pre3_execute_info_t, save_data) == 0x660);
	static_assert(offsetof(pre3_execute_info_t, p_work_ptr) == 0x1660);
	static_assert(offsetof(pre3_execute_info_t, volume_channel5) == 0x167C);
	static_assert(offsetof(pre3_execute_info_t, volume_channels0_4) == 0x1680);
	static_assert(offsetof(pre3_execute_info_t, assign) == 0x1684);
	static_assert(offsetof(pre3_execute_info_t, game_mode) == 0x1694);

	// The module drives six audio channels (the frame callback loops 0..5 calling its own
	// per-channel volume and pause setters), against m2ftg's single master. Nothing in the
	// protocol exposes them individually — the two scalars above are the only host input.
	inline constexpr int AUDIO_CHANNEL_COUNT = 6;
}
