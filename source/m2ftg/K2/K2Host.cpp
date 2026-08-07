#include "K2Host.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ImportSymbols.h"

#include "../../criware/Cri.h"
#include "../../pxd/LJ/sl.h"            // pxd::sl — the head of the context, and the primitives
#include "../../pxd/K2/sl.h"            // pxd::K2 — THIS generation's context layout (0xF3C0)
#include "../../pxd/LJ/sl_internal.h"   // handle_internal_buffer_t (the 8-byte queue node)
#include "../m2ftg.h"                 // m2ftg_config_t (0x100C) — unchanged in this generation
#include "../DebugWindows.h"          // RomFrameCounterAddress / ReadEmulatedRam32
#include "../VirtualClock.h"          // the module's timebase, driven by frames not real time
#include "../HleHooks.h"              // the HLE trap table, disabled by repointing its handlers
#include "../SystemSwitches.h"        // cabinet TEST / SERVICE on the emulated I/O board
#include "../../net/NetPlugin.h"      // net::Logf — the diagnostic sink both peers share
#include "../ModuleArgs.h"
#include "../DisplayModes.h"
#include "../../input/Input.h"
#include "../HostUI.h"
#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../Utils/MemoryMgr.h"       // VP::Patch — protection-aware writes into module .text
#include "../../Utils/ScopedUnprotect.hpp"
#include "../../Utils/Trampoline.h"
#include "../../wil/resource.h"

namespace m2ftg
{
	namespace K2
	{
		// The sl half of the pxd platform layer. This generation's sl context is the LJ-era layout
		// with 0x3C0 bytes inserted before handle_free_queue (0xF3C0 vs 0xF000), so the head fields
		// come from pxd::sl and everything past sz_fs_root from pxd::K2 — see pxd/K2/sl.h. The gs
		// half does NOT carry over (0x202140 vs 0x388A00): it stays an opaque block, with only the
		// individual fields the module reads filled in below.
		using namespace pxd;

		const GameDesc& CurrentGame()
		{
			// Kiwami 2's two modules are the same engine build (their TimeDateStamps are one second
			// apart) and speak the same protocol — same 0x16E0 execute_info, same m2ftg_config_t,
			// same gs layout — so they differ only by GameDesc.
			return gGeneral.GetGameId() == YAMPGeneral::GameId::VON_K2 ? GAME_OMG : GAME_VF2;
		}

		// The per-frame block. Size gate verified BOTH statically (`CMP RCX, 0x16E0` at the top of
		// module_main) and live under x64dbg (entered with RCX = 0x16E0).
		//
		// The 0x20-byte header is the shared pxd one, and the live capture read back exactly the
		// values the host had just written before calling:
		//   +0x00 size = 0x16E0   +0x08 p_device_context   +0x10 status (bit0 pause, bit5 coin)
		//   +0x14 result = 0x80004005 preset   +0x18 output_texid   +0x1C sound_volume = 1.0f
		//
		// THE VOLUME IS THE +0x1C FLOAT here — the m2ftg/Y6 mechanism, not Lost Judgment's +0x663
		// byte. Getting that wrong mutes every cue while the rest of the audio path works.
		//
		// The INPUT half, read out of this module's own pad reader FUN_18005E410 rather than
		// carried over from Lost Judgment (whose execute_info is 0x1760 with a 0x190 pad stride):
		//   pad[player] at **+0x20 with a 0x170 stride** — and 0x170 is exactly sizeof(csl_pad),
		//   i.e. this generation embeds the plain engine pad, not LJ's extended variant. Every
		//   field the reader touches lines up: +0x00 m_now (`*(uint*)(exec + p*0x170 + 0x20)`),
		//   +0x10..+0x1C the four analog floats (read at 0x30/0x34/0x38/0x3C), and
		//   m_buttons[4]/[5] the analog triggers (read at 0xC4/0xC5).
		//
		//   assign[player][8] at **+0x15E0** — FUN_180004A80 indexes it as
		//   `exec + 0x15E0 + player*8` and looks each byte up in the module's own combo table.
		struct alignas(16) execute_info_t
		{
			size_t size_of_struct;
			void* p_device_context;
			int status;
			int result;
			unsigned int output_texid;
			float sound_volume;
			pxd::csl_pad pad[2];                       // +0x20, stride 0x170
			std::byte unmapped[0x15E0 - 0x300];
			uint8_t assign[2][8];                      // +0x15E0
			std::byte unmapped_tail[0x16E0 - 0x15F0];
		};
		static_assert(sizeof(execute_info_t) == 0x16E0);
		static_assert(offsetof(execute_info_t, sound_volume) == 0x1C);
		static_assert(offsetof(execute_info_t, pad) == 0x20);
		static_assert(offsetof(execute_info_t, pad[1]) == 0x190);
		static_assert(offsetof(execute_info_t, assign) == 0x15E0);

		// ============================== host gs pieces =============================
		//
		// The gs context is the DLL's own embedded 0x202140 template. Everything below was
		// CAPTURED from a live Yakuza Kiwami 2 (host gs context found by searching memory for
		// 'LBgs' + version + size 0x202140), and it differs from the YLAD generation in three
		// ways that would each have cost a debugging session to guess:
		//
		//   gs+0x20 slot[0] = ID3D11Device                     (vtable confirmed in d3d11.dll)
		//   gs+0x20 slot[1] = slot[2] = gs+0xC0                <- an EMBEDDED sub-object, and the
		//                                                         SAME pointer twice; YLAD used two
		//                                                         separate external blocks
		//   gs+0x20 slot[3] = 1                                 (a value, not a pointer)
		//   gs+0x20 slot[4] = pointer to a dword that read 0
		//   gs+0x20 slot[5] = 0                                 <- NO allocator; YLAD put one here
		//   gs+0xB0        = the cgs_device_context             <- the SAME pointer as
		//                                                         execute_info+0x08. In YLAD this
		//                                                         slot is the allocator vtable.
		//   gs+0xC0 sub-object: +0x00 IDXGISwapChain (dxgi.dll vtable),
		//                       +0x08/+0x10/+0x18 three d3d11.dll objects, then packed dwords.
		static uint8_t* g_gsContext = nullptr;
		static void* g_slContext = nullptr;
		static int s_zeroDword = 0;

		// The module's two-board gate, or null in a module that has no second board - see the note
		// on ImportSymbol::TWO_BOARD_GATE. Written once, after module_start.
		static uint8_t* g_twoBoardGate = nullptr;

		// "-von-2board" on YAMP's command line. EXPERIMENT, off by default: nothing about normal
		// Virtual On play changes unless it is passed. Virtual On is a linked-cabinet game and the
		// module carries the whole second-board implementation with the switch never thrown; this
		// throws it, so the first question - does board 1 step at all? - can be answered by running
		// it rather than by reading more disassembly, which on this DLL has a poor track record.
		static bool WantTwoBoardMode()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-2board") != nullptr;
			return wanted;
		}

		// Logs the same state WITHOUT throwing the switch, so the two-board run has a control to be
		// compared against. Measuring only the experimental arm is how you talk yourself into
		// believing a number that was always going to be there.
		//
		// NOW A SETTING ("Log linked-cabinet state"), because a diagnostic reachable only from the
		// command line is unreachable from a launcher-started run - the launcher passes the game's
		// boot argument and nothing else. `-von-2probe` still works and is still what a cdb harness
		// uses, but it is no longer the only way in.
		static bool WantTwoBoardProbe()
		{
			const YAMPSettings* settings = gGeneral.GetSettings();
			static const bool wanted = WantTwoBoardMode()
				|| (settings != nullptr && settings->m_vonLinkLog)
				|| wcsstr(GetCommandLineW(), L"-von-2probe") != nullptr;
			return wanted;
		}

		// The per-chunk work-RAM map is a DESYNC tool - 32 CRC32s over 64 KB each, twice per frame,
		// which is 2 MB hashed and two log lines for every frame of the run. It belongs to the
		// lockstep era, where the question was "which 64 KB chunk diverged first". The current link
		// has no determinism requirement and nothing to diff, so it must NOT ride in on the "Log
		// linked-cabinet state" setting: a 1.3 MB log of hashes is how the ROM-level link lines got
		// lost last session. Command line only.
		static bool WantWorkRamMap()
		{
			static const bool wanted = WantTwoBoardMode()
				|| wcsstr(GetCommandLineW(), L"-von-2probe") != nullptr;
			return wanted;
		}

		// ---- two-board diagnostics -----------------------------------------------------------
		//
		// Only reachable behind -von-2board, and only meaningful for `omg`. These are RVAs into
		// omg-pxd-w64-gog_retail.dll, taken from the reversing in docs/von-netplay-recon.md; they
		// are safe to hardcode ONLY because GameVerify pins that module by SHA-256 before it is
		// ever loaded, so a different build cannot reach this code. They exist to answer one
		// question - does board 1's ROM actually run? - and should go away once it is answered.
		//
		// THE TEST: the frame step runs the link transfer BEFORE the two-board gate, so both
		// boards' CommData fill in either way and prove nothing. CommSend is different: a board's
		// SEND buffer is written by that board's own ROM. So a non-zero P2.CommSend is the one
		// signal that means board 1 executed.
		namespace OmgRva
		{
			constexpr size_t COMM_P1        = 0x7C2730;   // comm block 0 base
			constexpr size_t COMM_P1_SEND   = 0x7C4730;   // P1 + 0x2000
			constexpr size_t COMM_P1_FLAG   = 0x7CA731;   // P1 + 0x8001, bit0 = bank select
			constexpr size_t COMM_P2_SEND   = 0x7CC738;   // P2 + 0x2000, P2 = P1 + 0x8008
			constexpr size_t COMM_P2_FLAG   = 0x7D2739;
			constexpr size_t BOARD_INDEX    = 0x6911B4;   // written by the bank switch
			constexpr size_t COMM_PAYLOAD   = 0x700;      // bytes the link moves per board per frame
			constexpr size_t COMM_P2        = 0x7CA738;   // P1 + 0x8008
			constexpr size_t COMM_BANK      = 0x4000;     // the second 16 KB bank
			// THE FIRMWARE STATUS BYTES, at the base of whichever bank the flag selects. The ROM's
			// link check polls these and NEVER writes them (they are read-only to it): byte0 is
			// "ring up", byte2 the node id, byte3 the node count. On hardware the comm board's own
			// firmware fills them in once the serial ring is healthy; the module has no firmware, so
			// whatever it leaves here is what the ROM's `Net_check` believes about the wire.
			//
			// Logged because "does the check pass, and why" is otherwise unanswerable from outside:
			// a check that passes with no peer attached and a check that passes because a peer
			// answered look identical in link_ID.
			constexpr size_t COMM_STATUS_LEN = 4;

			// Per-board work RAM. The bank switch at 0x180069D30 does NOT store region bases - it
			// stores BIAS pointers, `region - i960_base`, so the emulator can index them with a
			// raw i960 address. Its two arms give, for the i960 0x500000 window where all of
			// Virtual On's game globals live (`LEA RAX,[...]` then `SUB RAX,0x500000`):
			//     board 0:  0x181337020 - 0x500000   ->  i960 0x500000 sits at RVA 0x1337020
			//     board 1:  0x180E37010 - 0x500000   ->  i960 0x500000 sits at RVA 0x0E37010
			// (The other pair, 0x180C37010 / 0x180F37010 less 0x200000, is the 0x200000 window.)
			// The module's own copy of m2ftg_config_t, the 0x100C bytes module_start memcpy'd from
			// params+0x38 into &DAT_1807A7FB0. +0x00 is `kind`, which must read 3 for omg - that
			// is the check that makes a wrong base fail loudly instead of corrupting .data.
			constexpr size_t CONFIG_BASE    = 0x7A7FB0;

			constexpr size_t BOARD0_R5      = 0x1337020;
			constexpr size_t BOARD1_R5      = 0x0E37010;
			constexpr uint32_t I960_R5_BASE = 0x500000;

			// The per-board blocks the bank switch (0x180069D30) re-points, base + board*stride.
			//
			// BACKUP/IO is the one that answers input routing: `backup_write` writes through this,
			// backup RAM starts at +0x91 (0x4000 bytes) and the I/O ports sit just past it - which
			// is where `module_main` puts the coin byte, at +0x4098. If player N's pad reaches
			// board N, the two boards' I/O bytes must differ when only pad[1] is held.
			constexpr size_t BACKUP_IO_BASE = 0x180C2EEC0;
			constexpr size_t BACKUP_IO_STEP = 0x409C;
			// THE I/O PORTS ARE AT THE FRONT OF THE BLOCK, NOT PAST THE BACKUP RAM. Corrected
			// 2026-08-04 from the module's own read handler FUN_18006CBE0, which serves the guest
			// ports straight off this block:
			//
			//     0x1C00002 SYSTEM  ->  io[8] & 1 ? io[10] : io[9]   (bank-0 copy / DIP bank)
			//     0x1C00004 P1      ->  io[0xB]
			//     0x1C00006 P2      ->  io[0xC]
			//     0x1C0001E analogue ->  io[0..7], cycled by io[0x10] & 7
			//
			// and by the backup-RAM injector, which maps guest 0x1D00000 to `block + 0x91` - so the
			// 0x4000 of backup RAM STARTS at 0x91 and everything before it is I/O.
			//
			// The old window (0x4091, "just past the 0x4000 of backup RAM") therefore watched bytes
			// no input ever touches, which is why the pad-routing probe showed both boards frozen at
			// the same value no matter what was held. It proved nothing, and reading it as "pads do
			// not reach the board" would have been wrong.
			constexpr size_t IO_PORTS       = 0x00;
			constexpr size_t IO_PORTS_LEN   = 0x11;   // through the analogue mux index at 0x10
			// The module global holding the pointer to that block. Re-pointed per board by the bank
			// switch, so anything reading through it sees the currently selected cabinet.
			constexpr size_t IO_STATE_PTR   = 0x6912A0;

			// The ~2 MB per-board region - the presentation candidate (video/framebuffer sized).
			constexpr size_t VIDEO_BASE     = 0x1807DADC0;
			constexpr size_t VIDEO_STEP     = 0x20A080;
			constexpr size_t VIDEO_SAMPLE   = 0x1000;

			// Named ROM globals, straight out of the module's own symbol table (RVA 0x4507E0) and
			// identical in the PS3 build. THIS is the real test, and unlike CommSend it needs no
			// match to be running: if the gate makes the frame step call board 1's step functions,
			// board 1 boots its own ROM from frame 0 and these fill in. All still zero after a
			// thousand frames means board 1 never executed.
			constexpr uint32_t SYM_BOARDTYPE = 0x5023E0;
			constexpr uint32_t SYM_SYNCHFLAG = 0x5024E0;   // the ROM's link handshake (`synch`)
			constexpr uint32_t SYM_SYNCHTIME = 0x5024E4;
			constexpr uint32_t SYM_GLOBCNTR  = 0x5024E8;   // the ROM's own shared frame counter
			constexpr uint32_t SYM_MAINMODE  = 0x5039F4;
			constexpr uint32_t SYM_VERSUSMODE= 0x503A7C;
			constexpr uint32_t SYM_NET_FLAG  = 0x5770B0;
			constexpr uint32_t SYM_LINK_ID   = 0x5770B1;

			// THE RUNTIME EXCHANGE, as the ROM itself stages it - decoded from the vsync path at
			// i960 0x14A0 and settled here because the packet's shape was the open question.
			//
			//   14B4  seq = [cSend+2]; seq = 1823 * (seq + 3)
			//   14F8  [cSend+2]   = seq                     ; stamped into the OUTGOING packet
			//   1500  [cSend+0x556] = seq ^ 0xAE5E          ; and its twin, same packet
			//   1508  memcpy(CommSend <- cSend, 0x700)      ; ROM -> comm RAM
			//   150C  memcpy(0x501CE0 <- CommData, 0x700)   ; comm RAM -> staging
			//   1524  accept iff staged[+0x556] == (staged[+2] ^ 0xAE5E)
			//   1550  memcpy(cRecn <- staging, 0x700)       ; only a validated packet lands
			//
			// So the ROM does BOTH halves of the cSend/cRecn staging itself and YAMP must not touch
			// either: the whole of YAMP's job is the 0x700 comm-RAM window in between. The check is
			// packet-internal (see the decode above), so it imposes no ordering on the transport.
			//
			// cRecn is an array of 0x700 node slots and cSend sits immediately past slot 1, which is
			// what made "we never write the peer's echo into slot 1" look like a bug. It is not: the
			// vsync path writes slot 0 unconditionally and NOTHING in the ROM ever writes slot 1 -
			// scanned, and 0x502BF0 appears in the image only as slot 1's state field being preset
			// to 0xFE when the link is down. The node-scan loop at 0x2BADC walks slots
			// `nodes-1 .. 0`, so on the two-node ring this game insists on it reads a permanently
			// empty slot 1 and then the peer in slot 0. There is no echo to write.
			constexpr uint32_t SYM_CSEND       = 0x5032F0;   // outgoing packet, 0x700
			constexpr uint32_t SYM_CRECN       = 0x5024F0;   // received packet, slot 0 of the array
			constexpr uint32_t SYM_TOTAL_NODES = 0x5770D0;   // written by Net_check step 12
			constexpr uint32_t SYM_NODE_ID     = 0x5770D1;
			constexpr size_t   PKT_SEQ         = 0x002;      // running counter
			constexpr size_t   PKT_STATE       = 0x004;      // the sender's link state (0x10/0x20/..)
			// THE STAGE, carried in the payload. The cabinet that rolls it publishes the value
			// here alongside state 0x24 at +4 (i960 0xCDB5C); the other adopts it from the same
			// offset of the packet it received (0x19770) and `InitGame` turns it into FieldNo.
			constexpr size_t   PKT_STAGE       = 0x008;
			// Where the adopted/rolled stage lives locally, and what the match actually loads.
			constexpr uint32_t SYM_STAGE_SEL   = 0x503A84;
			constexpr uint32_t SYM_FIELDNO     = 0x5770F0;
			constexpr size_t   PKT_CHECK       = 0x556;      // seq ^ PKT_CHECK_XOR
			constexpr uint16_t PKT_CHECK_XOR   = 0xAE5E;
			// What the ROM leaves in cRecn's state field when no packet landed: 0xFE if net_flag is
			// clear (i960 0x1488), 0xFF if the ring is down or the packet was rejected (0x1594 /
			// 0x15A0). Any OTHER value came out of a validated peer packet, which makes this one
			// byte the ROM-level proof that the link is carrying data.
			constexpr uint8_t  PKT_STATE_NONET = 0xFE;
			constexpr uint8_t  PKT_STATE_NOPKT = 0xFF;
		}

		// Native address of an i960 global in a given board's work RAM.
		static uint8_t* I960At(uint8_t* base, int board, uint32_t i960Addr)
		{
			const size_t bias = (board != 0) ? OmgRva::BOARD1_R5 : OmgRva::BOARD0_R5;
			return base + bias + (i960Addr - OmgRva::I960_R5_BASE);
		}

		// "-von-padtest": hold a distinctive button pattern on pad[1] ONLY, so the two boards can be
		// told apart by their inputs. Without it both pads are idle under -frames and the boards are
		// expected to look identical - which would prove nothing either way.
		static bool WantPadTest()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-padtest") != nullptr;
			return wanted;
		}

		// "-von-autostart=<frame>": press START on pad[0] at a given host frame, and hold it for the
		// length of a real press.
		//
		// It exists because "pressing START crashes the cabinet" is not reproducible any other way:
		// a `-frames` run never presses anything, and a human pressing a button under a debugger is
		// not a measurement you can repeat, bisect, or run on both cabinets at a known frame. The
		// press is injected where a real one lands - before the coin/start dance - so freeplay,
		// the coin insert and everything downstream see exactly what they would normally see.
		static int AutoStartFrame()
		{
			static const int frame = []() {
				const wchar_t* arg = wcsstr(GetCommandLineW(), L"-von-autostart=");
				return arg != nullptr ? _wtoi(arg + wcslen(L"-von-autostart=")) : 0;
			}();
			return frame;
		}
		constexpr int AUTOSTART_HOLD_FRAMES = 8;

		// "-von-render1": force drawing board 1 (a two-board debug aid). See SetRenderBoard.
		static bool WantRenderBoard1()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-render1") != nullptr;
			return wanted;
		}

		static uint8_t* g_renderBoardSelect = nullptr;

		// Choose which cabinet the render draws, by patching the two bytes that decide what the
		// frame step leaves selected: `XOR ECX,ECX` (board 0) <-> `MOV CL,1` (board 1).
		//
		// NOTE the VP::Patch. This writes into the module's .text, and the ScopedUnprotect that made
		// .text writable lives in ResolveSymbolsAndPatch and is long out of scope by the time the
		// game loop runs - so a bare store here faults. VP::Patch wraps the write in VirtualProtect
		// and restores the old protection.
		// SAY WHAT IT DID. This used to be entirely silent: null symbol, or bytes that did not match
		// the expected encoding, and it returned having done nothing at all - which made "the patch
		// is not being applied" indistinguishable from "the patch is applied and does not change
		// what is drawn", two completely different faults.
		static void SetRenderBoard(int board)
		{
			const bool wantOne = board != 0;
			static int s_reported = -1;
			const auto report = [&](const char* what) {
				if (s_reported != board)
				{
					s_reported = board;
					net::Logf("render board -> %d (%s)", board, what);
				}
			};

			if (g_renderBoardSelect == nullptr)
			{
				report("NO RENDER_BOARD_SELECT SYMBOL - the render always draws board 0");
				return;
			}
			const uint8_t* p = g_renderBoardSelect;
			// Verify before writing - never patch blind, and never write if it already reads right.
			if (wantOne && p[0] == 0x33 && p[1] == 0xC9)
			{
				Memory::VP::Patch(g_renderBoardSelect, { 0xB1, 0x01 });   // MOV CL,1  -> render board 1
				report("patched MOV CL,1");
			}
			else if (!wantOne && p[0] == 0xB1 && p[1] == 0x01)
			{
				Memory::VP::Patch(g_renderBoardSelect, { 0x33, 0xC9 });   // XOR ECX,ECX -> render board 0
				report("patched XOR ECX,ECX");
			}
			else if ((wantOne && p[0] == 0xB1 && p[1] == 0x01)
				|| (!wantOne && p[0] == 0x33 && p[1] == 0xC9))
			{
				report("already selected");
			}
			else
			{
				// Neither encoding: the symbol is not pointing at the instruction we think it is,
				// and every "patch" so far has silently done nothing.
				net::Logf("render board %d REFUSED: bytes at the select site are %02X %02X, "
					"expected 33 C9 or B1 01 - the symbol is wrong", board, p[0], p[1]);
			}
		}

		// ---- Cabinet role: NOLINK / MASTER / SLAVE ------------------------------------------
		//
		// THREE ENCODINGS ARE IN PLAY AND NO TWO AGREE. Getting them straight is most of this:
		//
		//   YAMPSettings::m_vonCabinetRole   0 NOLINK   1 MASTER   2 SLAVE   (YAMP's own, ordered)
		//   backup RAM 0x1D00028             2 NOLINK   1 MASTER   0 SLAVE   (what the ROM reads)
		//   link_ID    (guest 0x5770B1)      3 NOLINK   1 MASTER   2 SLAVE   (what the ROM derives)
		//
		// straight out of the ROM's InitNetwork (i960 0x18A10), which is the authority:
		//
		//     ldob 0x1D00028, g4 ; == 0 ? -> link_ID = 2, 0x503A08 = 1     (the SLAVE site)
		//     and  0xFF, g4      ; == 1 ? -> link_ID = 1, 0x503A08 = 0     (the MASTER site)
		//                        ; else   -> link_ID = 3, 0x503A08 = 0     (standalone)
		//     call Net_check     ; 0xC5870 - and standalone leaves early
		//
		// (0x503A08, which docs/von-netplay-recon.md lists as an unnamed "I am the slave" flag with
		// a warning not to rely on it, is exactly that: set on the byte==0 arm and cleared on both
		// others. The symbol table's `TempTimer+0x4` is the wrong name for it.)
		//
		// AND IT IS VISIBLE, which is the first tell this game has offered for any of this. Sampling
		// the video region every 200 frames across three runs each:
		//
		//     NOLINK  76EFDDC5 AFAC5EEF F4F45280 EB5B179F 9B508466
		//     MASTER  76EFDDC5 AFAC5EEF F4F45280 EB5B179F 9B508466    identical to NOLINK
		//     SLAVE   76EFDDC5 AFAC5EEF 6CDF31A6 6080D65D B2A5D5D0    diverges, reproducibly
		//
		// The user's independent report was "the colour of the characters in attract mode changed",
		// on a SLAVE run. So the slave arm's one distinguishing act - setting 0x503A08 - reaches the
		// picture, and that flag's long-suspected meaning is now checked against behaviour rather
		// than read off a disassembly. NOTE THE ASYMMETRY before using this to verify a two-machine
		// setup: only the SLAVE looks different. A master is pixel-identical to a standalone
		// cabinet, so "it looks the same" does not mean the role failed to apply - read the log.
		//
		// HOW IT IS SET, and why this way. The module already injects that byte, in the handler for
		// HLE hook 6 (`InitNetwork`), which runs on the very instruction that reads it:
		//
		//     mov  byte ptr [rsp+0x20], 2        <-- the standalone default: ONE IMMEDIATE
		//     cmp  byte ptr [two_board_gate], 0
		//     je   inject
		//     cmp  dword ptr [board_index], 0
		//     sete byte ptr [rsp+0x20]           ; two boards: board 0 = MASTER, board 1 = SLAVE
		//  inject:
		//     lea  rdx, [rsp+0x20]
		//     mov  ecx, 0x1D00028
		//     call backup_inject
		//
		// So the whole knob is that one immediate. Patching it is better than the alternatives:
		// writing the byte from YAMP would mean disabling hook 6 and then racing the ROM's read
		// (and the backup CRC the ROM computes during boot), while this hands the value to the
		// module's own injector at the module's own moment. Reversible, verified before writing,
		// and it leaves the two-board arm alone - with the gate on, the module still assigns
		// MASTER/SLAVE per board and this setting does not apply, which is correct: the role is a
		// property of a CABINET, and in two-board mode this machine is both of them.
		// The backup byte for a role, in the ROM's encoding (see the table above).
		static uint8_t CabinetRoleByte(uint32_t role)
		{
			return role == 1 ? 1 : (role == 2 ? 0 : 2);   // MASTER 1, SLAVE 0, NOLINK 2
		}

		static const char* CabinetRoleName(uint32_t role)
		{
			return role == 1 ? "MASTER" : (role == 2 ? "SLAVE" : "NOLINK");
		}

		// THE ROLE THIS CABINET IS ACTUALLY RUNNING AS - one source of truth, deliberately.
		// The setting is only an input to it: a room join will call SoftResetIntoRole directly,
		// and if the firmware kept reading the setting instead it would go on reporting a
		// standalone cabinet while the ROM had already been told it was a slave. Caught exactly
		// that way in the NOLINK -> MASTER proof: link_ID went to 1 but the ring stayed up,
		// because the harness moved the patched byte without moving the setting.
		static uint32_t s_cabinetRole = 0;

		// Idempotent and callable at ANY time - which is what makes a live role change possible.
		// Returns false if the site did not look right, so callers can say so rather than assume.
		static bool ApplyCabinetRole(void* dll, uint32_t role)
		{
			// Hook 6 is the InitNetwork trap. Assert that rather than trust the index: the table was
			// renumbered on 2026-08-05 and a stale index would patch some unrelated handler's stack.
			const HleHooks::Info& hook = HleHooks::Get(6);
			if (!HleHooks::Supported() || hook.romOffset != 0x18A10 || hook.handlerRva != 0x070A00)
			{
				net::Logf("cabinet role: hook 6 is not the InitNetwork injector "
					"(rom 0x%06X, handler 0x%06X) - role NOT applied", hook.romOffset, hook.handlerRva);
				return false;
			}

			// `C6 44 24 20 02` = mov byte ptr [rsp+0x20], 2. Verify all five bytes; the immediate is
			// the fifth. Never patch blind - see SetRenderBoard, which learned the same lesson.
			auto* site = reinterpret_cast<uint8_t*>(dll) + hook.handlerRva + 0x1E;
			const uint8_t expect[4] = { 0xC6, 0x44, 0x24, 0x20 };
			// site[4] is allowed to be 0/1/2 rather than strictly 2, so a second call is idempotent
			// rather than a refusal that reports the previous patch as a corrupt site.
			if (memcmp(site, expect, sizeof(expect)) != 0 || site[4] > 0x02)
			{
				net::Logf("cabinet role REFUSED: bytes at the injector default are "
					"%02X %02X %02X %02X %02X, expected C6 44 24 20 <=02",
					site[0], site[1], site[2], site[3], site[4]);
				return false;
			}

			const uint8_t wanted = CabinetRoleByte(role);
			s_cabinetRole = role;
			if (site[4] != wanted)
			{
				Memory::VP::Patch(site + 4, { wanted });
				net::Logf("cabinet role: %s - backup 0x1D00028 = %u", CabinetRoleName(role), wanted);
			}
			return true;
		}

		// "-von-findctr": hunt for the ROM's per-frame counter.
		//
		// NetSession needs a value that advances exactly once per emulated frame: it is both the
		// round-start anchor and the desync canary, and getting it wrong is precisely what put
		// VF2's two peers one frame apart. Rather than guess from the symbol table (GlobCntr was
		// the obvious candidate and measured only ~0.37 ticks per module_main, so it is not one),
		// sweep the whole work-RAM window and let the data name it.
		//
		// For every dword in the i960 0x500000 window, classify each module_main call as
		// delta == +1, delta == 0, or anything else. A true frame counter is "+1 or 0, never
		// anything else": the zero frames are calls where the board genuinely did not advance,
		// which VF2 proved really happen (~5% there) and which EndFrame already handles.
		static bool WantFindCounter()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-findctr") != nullptr;
			return wanted;
		}

		// The module's board-bank switch, or null in a module with only one board.
		using board_select_t = void(*)(int);
		static board_select_t g_boardSelect = nullptr;

		static uint8_t* g_dllBase = nullptr;

		// ---- Comm-board RESET semantics ------------------------------------------------------
		//
		// The one piece of comm-board firmware the module does not model, and the cause of the
		// hard hang on leaving the debug/test menu with the link running.
		//
		// The ROM's link check (`Net_check`, i960 0xC5870 - decoded in docs/von-netplay-recon.md)
		// drives the comm board through its two registers exactly as the hardware manual would
		// have it: zero both registers (hold the board in reset), wait a second, write
		// CommBoardReset = 1 (release it), then poll CommFlagReg until bit7 - data ready - reads
		// 0, which on real hardware is the firmware acknowledging its boot. Only then does it
		// wait for bit7 to SET (the first transfer), write its link_ID into comm RAM, and poll
		// the firmware's status bytes (byte0 = ring up, byte3 = node count == 2).
		//
		// The module's link transfer sets bit7 on BOTH flag registers every frame regardless of
		// the reset registers - the retail Kiwami 2 cabinet is standalone and nothing ever ran
		// this path - so the "wait for bit7 to clear" poll can never terminate once the link is
		// enabled. And that poll is a tight four-instruction i960 loop with no frame yield, so
		// the CPU step never returns, module_main never returns, and the whole host spins.
		//
		// Cold boot escapes by ORDERING alone: Net_check runs before the module enables the link,
		// so no transfer interferes and the check completes (which is why net_flag reads 1 in a
		// healthy linked run). The debug menu's exit re-runs the check with the link live and
		// hung the machine - MEASURED 2026-08-04 on a live process: both boards frozen, GlobCntr
		// static, board 0 spinning at Net_check+0x64 with reset=01/flag=81. Clearing bit7 by
		// hand un-froze it, "THIS IS MASTER SITE" / "SLAVE SITE" printed, and the pair
		// re-linked (net_flag -> 1 on both boards, MainMode -> 1).
		//
		// So YAMP models the missing semantics at the link transfer, the module's own once-per-
		// frame comm-board tick: a board whose reset register the ROM has dropped to 0 stops
		// driving its flag register until the ROM releases it. Armed only after the register has
		// been SEEN at 1 (the ROM's own release during its boot-time check), so a cold boot - or
		// any run where the check never re-runs - behaves exactly as before this existed.
		static void (*g_origLinkTransfer)() = nullptr;
		// Defined with the rest of the link, driven from the shim below - the module's own transfer
		// is a writer into the same window, so the peer's packet has to be laid down after it.
		static void DeliverCommPayload();
		namespace CommReset
		{
			// Register RVAs per comm block: reset at +0x8000, flag at +0x8001 (see OmgRva).
			constexpr size_t RESET[2] = { OmgRva::COMM_P1_FLAG - 1, OmgRva::COMM_P2_FLAG - 1 };
			constexpr size_t FLAG[2]  = { OmgRva::COMM_P1_FLAG,     OmgRva::COMM_P2_FLAG };
			static bool s_seenOn[2];
			static bool s_armed[2];
			// How many times the module's OWN link transfer ran. It is called from the frame driver
			// only while linking is enabled, so "does it run at all on a one-board cabinet" is a
			// question with two very different answers - and if it does, it is a writer into the
			// same CommData window YAMP fills from the wire.
			static uint32_t s_transfers;
		}

		static void LinkTransferWithResetSemantics()
		{
			// Reassert the two-board gate HERE, not just once per host frame. This shim runs
			// inside the frame driver before either board's CPU steps, and every consumer of the
			// gate - the board-1 branch of the driver, and hook 7's MASTER/SLAVE assignment that
			// InitNetwork reads - executes after it in the same call. A per-host-frame reassert
			// is not enough on its own: StepOneBoardFrame can run many module_main calls per
			// host frame while the counter holds (all of boot), and the module's own bring-up
			// zeroes the gate mid-burst - if the ROM's InitNetwork then runs before the next
			// host frame, it reads gate=0 and assigns NOLINK, and the machine comes up
			// standalone. Measured live 2026-08-04.
			if (g_twoBoardGate != nullptr && WantTwoBoardMode())
			{
				*g_twoBoardGate = 1;
			}

			// PRE: sample the reset registers before the transfer touches the flags. The arm /
			// disarm transitions are logged - they happen a handful of times per link check, and
			// a silent device model is how the original bug survived a whole session.
			for (int b = 0; b < 2; b++)
			{
				const bool released = (*(g_dllBase + CommReset::RESET[b]) & 1) != 0;
				if (released) CommReset::s_seenOn[b] = true;
				const bool arm = CommReset::s_seenOn[b] && !released;
				if (arm != CommReset::s_armed[b])
				{
					CommReset::s_armed[b] = arm;
					net::Logf("comm board %d %s", b,
						arm ? "held in RESET by the ROM - flag register pinned low"
						    : "released from reset");
				}
			}

			CommReset::s_transfers++;
			g_origLinkTransfer();

			// The peer's packet goes down AFTER the module's transfer and BEFORE the CPU steps.
			// This is the only window in the frame where it survives to be read.
			DeliverCommPayload();

			// POST: a comm board held in reset drives nothing, so undo what this frame's
			// transfer did to its registers. The ROM's release (writing 1) happens inside the
			// following CPU step - after this ran - which is exactly what lets its "bit7 must
			// read 0 after release" poll pass on the first read instead of spinning forever.
			for (int b = 0; b < 2; b++)
			{
				if (CommReset::s_armed[b]) *(g_dllBase + CommReset::FLAG[b]) = 0;
			}
		}

		// Non-zero byte count, so "did anything ever land here" is one number.
		static uint32_t NonZeroCount(const uint8_t* p, size_t n)
		{
			uint32_t hits = 0;
			for (size_t i = 0; i < n; ++i) hits += (p[i] != 0);
			return hits;
		}

		// FNV-1a over a sample, so "are these two regions the same" is one comparison.
		static uint32_t Hash32(const uint8_t* p, size_t n)
		{
			uint32_t h = 2166136261u;
			for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
			return h;
		}

		// The counter sweep. One pass per module_main call, over the i960 0x500000 window.
		namespace FindCtr
		{
			// The named globals run to at least 0x577590 (`r_num`), so sweep the whole 512 KB
			// rather than the first 32 KB - the first pass found nothing but flags.
			constexpr uint32_t BASE  = 0x500000;
			constexpr uint32_t WORDS = 0x80000 / 4;

			struct Slot { uint32_t prev; uint32_t inc; uint32_t same; uint32_t other; };
			static std::vector<Slot> s_slots;
			static bool s_primed = false;

			static void Sample(uint8_t* dllBase, int board)
			{
				const uint8_t* ram = I960At(dllBase, board, BASE);
				if (s_slots.empty()) s_slots.resize(WORDS);
				for (size_t i = 0; i < WORDS; ++i)
				{
					uint32_t v;
					memcpy(&v, ram + i * 4, sizeof(v));
					if (s_primed)
					{
						const uint32_t d = v - s_slots[i].prev;
						if (d == 1)      ++s_slots[i].inc;
						else if (d == 0) ++s_slots[i].same;
						else             ++s_slots[i].other;
					}
					s_slots[i].prev = v;
				}
				s_primed = true;
			}

			// ---- boot-state hunt ---------------------------------------------------------
			//
			// `DwGame.rvaBootState` is a MODULE global (not emulated RAM) that reads 2 once the
			// board is up; BoardBooted() gates every debug-menu action and ResetBoard on it.
			// Rather than guess, snapshot the region where the module's other known globals live
			// - the gate 0x6910DE, board index 0x6911B4, RNG holder 0x690810 and run-state
			// 0x7D2B10 are all in here - and report what climbed to exactly 2 and stayed.
			constexpr size_t GLOB_LO = 0x680000;
			constexpr size_t GLOB_HI = 0x7E0000;
			static std::vector<uint32_t> s_globFirst;

			static void SnapGlobals(uint8_t* dllBase)
			{
				const size_t n = (GLOB_HI - GLOB_LO) / 4;
				s_globFirst.resize(n);
				memcpy(s_globFirst.data(), dllBase + GLOB_LO, n * 4);
			}

			static void ReportGlobals(uint8_t* dllBase)
			{
				size_t shown = 0;
				DebugLogFile("[findboot] dwords in %06zX..%06zX that are 2 now and were <2 at frame 1\n",
					GLOB_LO, GLOB_HI);
				for (size_t i = 0; i < s_globFirst.size(); ++i)
				{
					uint32_t now;
					memcpy(&now, dllBase + GLOB_LO + i * 4, sizeof(now));
					if (now != 2 || s_globFirst[i] >= 2) continue;
					DebugLogFile("[findboot]   RVA %06zX  %u -> 2\n",
						GLOB_LO + i * 4, s_globFirst[i]);
					if (++shown >= 40) { DebugLogFile("[findboot]   ...truncated\n"); break; }
				}
				DebugLogFile("[findboot] %zu candidate(s)\n", shown);
			}

			// Report the dwords that only ever held still or stepped by one, best first.
			static void Report(int board)
			{
				std::vector<size_t> hits;
				for (size_t i = 0; i < s_slots.size(); ++i)
				{
					if (s_slots[i].other == 0 && s_slots[i].inc > 0) hits.push_back(i);
				}
				std::sort(hits.begin(), hits.end(),
					[](size_t a, size_t b) { return s_slots[a].inc > s_slots[b].inc; });
				DebugLogFile("[findctr] board %d: %zu monotone dwords (+1 or 0, never else)\n",
					board, hits.size());
				for (size_t n = 0; n < hits.size() && n < 16; ++n)
				{
					const size_t i = hits[n];
					DebugLogFile("[findctr]   i960 0x%06X  +1 x%u  hold x%u  (now %u)\n",
						BASE + static_cast<uint32_t>(i * 4),
						s_slots[i].inc, s_slots[i].same, s_slots[i].prev);
				}

				// Near-misses matter as much as clean hits: a counter that mostly steps by one but
				// occasionally jumps is still the ROM's frame counter, it just is not safe as the
				// anchor. Listing them is what tells the two cases apart instead of reporting
				// "nothing found" and leaving it ambiguous.
				std::vector<size_t> nearMiss;
				for (size_t i = 0; i < s_slots.size(); ++i)
				{
					if (s_slots[i].other > 0 && s_slots[i].inc > 0) nearMiss.push_back(i);
				}
				std::sort(nearMiss.begin(), nearMiss.end(),
					[](size_t a, size_t b) { return s_slots[a].inc > s_slots[b].inc; });
				DebugLogFile("[findctr] board %d: top near-misses (step by one, but not always)\n", board);
				for (size_t n = 0; n < nearMiss.size() && n < 16; ++n)
				{
					const size_t i = nearMiss[n];
					DebugLogFile("[findctr]   i960 0x%06X  +1 x%u  hold x%u  other x%u  (now %u)\n",
						BASE + static_cast<uint32_t>(i * 4),
						s_slots[i].inc, s_slots[i].same, s_slots[i].other, s_slots[i].prev);
				}
			}
		}

		static void LogTwoBoardState(int frame, uint32_t texid)
		{
			if (g_dllBase == nullptr) return;
			const auto at = [](size_t rva) { return g_dllBase + rva; };
			// These two are absolute VAs in the static image, so rebase them like everything else.
			const auto io = [&](int b) {
				return g_dllBase + (OmgRva::BACKUP_IO_BASE - 0x180000000)
					+ b * OmgRva::BACKUP_IO_STEP + OmgRva::IO_PORTS;
			};
			const auto video = [&](int b) {
				return g_dllBase + (OmgRva::VIDEO_BASE - 0x180000000) + b * OmgRva::VIDEO_STEP;
			};

			// Two hex chars per port byte plus the terminator - 0x11 ports is 35 bytes, and the
			// [32] this used to be overran the frame on the first -von-2probe run after the port
			// window was widened (caught by RTC as an int3 at frame 0, presenting as a crash).
			char io0[OmgRva::IO_PORTS_LEN * 2 + 1], io1[OmgRva::IO_PORTS_LEN * 2 + 1];
			for (size_t i = 0; i < OmgRva::IO_PORTS_LEN; ++i)
			{
				snprintf(io0 + i * 2, 3, "%02X", io(0)[i]);
				snprintf(io1 + i * 2, 3, "%02X", io(1)[i]);
			}
			// The control-mapping scheme the module actually latched, and the first two pairs it
			// copied out of the table. Valid data is {mask, code} pairs with small codes; an index
			// past the end of the five-entry table copies POINTERS (0x18045xxxx) instead, which is
			// what decoded every pad as nothing. See the assign[0][4] note in the frame loop.
			DebugLogFile("[2board] scheme=%u map=%08X,%08X,%08X,%08X\n",
				*reinterpret_cast<uint32_t*>(at(0x6917F8)),
				*reinterpret_cast<uint32_t*>(at(0x6916B0)), *reinterpret_cast<uint32_t*>(at(0x6916B4)),
				*reinterpret_cast<uint32_t*>(at(0x6916B8)), *reinterpret_cast<uint32_t*>(at(0x6916BC)));
			DebugLogFile("[2board] texid=%u io0=%s io1=%s vid0=%08X vid1=%08X\n",
				texid, io0, io1,
				Hash32(video(0), OmgRva::VIDEO_SAMPLE), Hash32(video(1), OmgRva::VIDEO_SAMPLE));
			const auto sym = [](int board, uint32_t a) { return I960At(g_dllBase, board, a); };
			const auto u32 = [&](int b, uint32_t a) { return *reinterpret_cast<uint32_t*>(sym(b, a)); };

			// Read the gate BACK every sample. The module's own mode machine has a state (0x10)
			// that zeroes it along with the board index, so a write made once after module_start
			// is not necessarily still there - and "we set it" is not the same claim as "it is
			// set".
			{
				const uint8_t* c = at(OmgRva::CONFIG_BASE);
				DebugLogFile("[cfg] kind=%u diff=%u country=%u acf=%u vf20=%u free=%u vs=%u sram=%u\n",
					*reinterpret_cast<const uint32_t*>(c), c[4], c[5], c[6], c[7], c[9], c[10], c[11]);
			}
			// WHICH BOARD IS THE CANARY ACTUALLY HASHING?
			//
			// DwGame.rvaRamBasePtr is 0xC37000, which the bank switch RE-POINTS per board, so it
			// names whichever board is selected at the moment StateCheckValue runs - not board 0 by
			// construction. If that selection differs between the two peers, the desync check
			// compares one machine's MASTER against the other's SLAVE and reports a divergence that
			// is not one.
			//
			// So log the live board index next to per-board hashes of the three chunks that
			// actually diverged (0x510000 / 0x520000 / 0x590000). Read across the two peers' logs:
			//   * peer A h0 == peer B h0  -> the boards agree; the canary is sampling the wrong one
			//   * h0 differs across peers -> a real divergence, and the board index is a red herring
			{
				const auto chunk = [&](int b, uint32_t a) {
					return Hash32(I960At(g_dllBase, b, a), 0x10000);
				};
				DebugLogFile("[board] idx=%d  B0 %08X/%08X/%08X  B1 %08X/%08X/%08X\n",
					*reinterpret_cast<int32_t*>(at(OmgRva::BOARD_INDEX)),
					chunk(0, 0x510000), chunk(0, 0x520000), chunk(0, 0x590000),
					chunk(1, 0x510000), chunk(1, 0x520000), chunk(1, 0x590000));
			}
			// The comm board as the ROM's link check sees it: the two registers, then the firmware
			// status bytes in BOTH banks (bit0 of the flag says which one the i960 is looking at).
			{
				// One buffer per argument, deliberately. A rotating static would be evaluated four
				// times before DebugLogFile ran and print the same two values twice.
				char st[4][OmgRva::COMM_STATUS_LEN * 2 + 1];
				const size_t bases[4] = {
					OmgRva::COMM_P1, OmgRva::COMM_P1 + OmgRva::COMM_BANK,
					OmgRva::COMM_P2, OmgRva::COMM_P2 + OmgRva::COMM_BANK,
				};
				for (size_t b = 0; b < 4; ++b)
				{
					for (size_t i = 0; i < OmgRva::COMM_STATUS_LEN; ++i)
					{
						snprintf(st[b] + i * 2, 3, "%02X", *at(bases[b] + i));
					}
				}
				DebugLogFile("[comm] B0 reset=%02X flag=%02X bank0=%s bank1=%s | "
					"B1 reset=%02X flag=%02X bank0=%s bank1=%s | brought-up=%u bootstate=%u\n",
					*at(OmgRva::COMM_P1_FLAG - 1), *at(OmgRva::COMM_P1_FLAG), st[0], st[1],
					*at(OmgRva::COMM_P2_FLAG - 1), *at(OmgRva::COMM_P2_FLAG), st[2], st[3],
					// The flag hook 0 sets at BlackOut+0x190, and the module's own boot state. Both
					// are downstream of Net_check RETURNING, so a board parked in the link check
					// never reaches either - which is the suspect for the blank screen.
					*at(0x6910DF), *reinterpret_cast<uint32_t*>(at(0x7ADCA8)));
			}
			DebugLogFile("[2board] gate=%d ", g_twoBoardGate != nullptr ? *g_twoBoardGate : -1);
			DebugLogFile("[2board] frame=%d flags=%02X/%02X send=%u/%u | "
				"B0 type=%u glob=%u main=%u vs=%u synch=%u/%u net=%u id=%u | "
				"B1 type=%u glob=%u main=%u vs=%u synch=%u/%u net=%u id=%u\n",
				frame,
				*at(OmgRva::COMM_P1_FLAG), *at(OmgRva::COMM_P2_FLAG),
				NonZeroCount(at(OmgRva::COMM_P1_SEND), OmgRva::COMM_PAYLOAD),
				NonZeroCount(at(OmgRva::COMM_P2_SEND), OmgRva::COMM_PAYLOAD),
				u32(0, OmgRva::SYM_BOARDTYPE), u32(0, OmgRva::SYM_GLOBCNTR),
				u32(0, OmgRva::SYM_MAINMODE), u32(0, OmgRva::SYM_VERSUSMODE),
				u32(0, OmgRva::SYM_SYNCHFLAG), u32(0, OmgRva::SYM_SYNCHTIME),
				*sym(0, OmgRva::SYM_NET_FLAG), *sym(0, OmgRva::SYM_LINK_ID),
				u32(1, OmgRva::SYM_BOARDTYPE), u32(1, OmgRva::SYM_GLOBCNTR),
				u32(1, OmgRva::SYM_MAINMODE), u32(1, OmgRva::SYM_VERSUSMODE),
				u32(1, OmgRva::SYM_SYNCHFLAG), u32(1, OmgRva::SYM_SYNCHTIME),
				*sym(1, OmgRva::SYM_NET_FLAG), *sym(1, OmgRva::SYM_LINK_ID));
		}

		// The gs-context allocator, at **gs+0xA0** in this generation (YLAD puts its equivalent at
		// gs+0xB0 — here 0xB0 is the cgs_device_context instead, so do NOT carry that over).
		// Shape read straight out of the resource creator FUN_18009D120:
		//     alloc: (**(code **)(*obj + 0x08))(obj, size, align)     -> vtable slot 1
		//     free:  (**(code **)(*obj + 0x18))(obj, ptr)             -> vtable slot 3
		// The module allocates every shader/resource object through it, so it runs hundreds of
		// times per boot and must stay silent.
		static void* __fastcall GsAlloc(void* /*self*/, size_t size, size_t align)
		{
			void* p = _aligned_malloc(size, align != 0 ? align : 16);
			if (p != nullptr) memset(p, 0, size);
			return p;
		}
		static void __fastcall GsFree(void* /*self*/, void* p) { _aligned_free(p); }
		static void __fastcall GsAllocNoop(void*) {}

		static void* s_gsAllocatorVtbl[4] = {
			reinterpret_cast<void*>(&GsAllocNoop),   // slot 0
			reinterpret_cast<void*>(&GsAlloc),       // slot 1 (+0x08) — alloc(this, size, align)
			reinterpret_cast<void*>(&GsAllocNoop),   // slot 2
			reinterpret_cast<void*>(&GsFree),        // slot 3 (+0x18) — free(this, ptr)
		};
		static void* s_gsAllocator[2] = { s_gsAllocatorVtbl, nullptr };

		// The cgs_device_context YAMP hands the module (and what execute_info+0x08 points at).
		// A zeroed block plus four fields, each read out of the DLL and filled in FillSharedSymbols:
		// +0x18 the D3D11 context, +0x28/+0x30 the cb/up pools, +0x38 the render-state block.
		// The rest is scratch the module manages itself.
		static uint8_t* s_deviceContext = nullptr;

		// Mirrors config.is_freeplay, which decides whether the coin/start dance runs at all.
		static bool s_isFreeplay = true;

		// ----------------------- primitive_initialize (host-provided) -----------------------
		//
		// The shared immediate-mode index buffers. `pxd::gs::primitive_initialize` lives in the
		// HOST in every generation (YakuzaKiwami2.exe here), so the module's gs constructor only
		// zeroes these slots and nothing inside the DLL ever fills them.
		//
		// The 2D/immediate primitive pusher FUN_180059020 selects between exactly two of them:
		//   kind 0xF -> gs+0x1418, then DrawIndexed(verts * 3/2)   = a QUAD list  (4 verts -> 6)
		//   kind 0xE -> gs+0x1420, then DrawIndexed((verts-2) * 3) = a triangle FAN
		// both bound with IASetIndexBuffer(..., DXGI_FORMAT_R16_UINT) and TRIANGLELIST topology.
		// A null slot faults immediately at DLL+0x59082 reading the wrapper's +0x10.
		//
		// The wrapper shape is the same cgs buffer object the YLAD generation uses (self-pointer at
		// +0x10 is the entire cache "id" the device context compares at devctx+0xF98; +0x18 points
		// at the embedded sub-object, whose +0x10 is the real ID3D11Buffer).
		struct alignas(16) gs_buffer_t
		{
			uint64_t alive;              // +0x00
			uint32_t flags;              // +0x08  0x201 = index buffer
			uint32_t byte_size;          // +0x0C
			gs_buffer_t* self;           // +0x10  its OWN address — the state-cache id
			void* sub;                   // +0x18  = (uint8_t*)this + 0x30
			uint32_t element_count;      // +0x20
			uint32_t reserved;           // +0x24
			std::byte gap[0x30 - 0x28] {};
			uint64_t sub_zero0;          // +0x30
			uint64_t sub_zero8;          // +0x38
			ID3D11Buffer* resource;      // +0x40  (sub+0x10) — what IASetIndexBuffer receives
			uint32_t sub_flags;          // +0x48
			uint32_t sub_size;           // +0x4C
			std::byte tail[0x80 - 0x50] {};
		};
		static_assert(sizeof(gs_buffer_t) == 0x80);
		static_assert(offsetof(gs_buffer_t, self) == 0x10);
		static_assert(offsetof(gs_buffer_t, sub) == 0x18);
		static_assert(offsetof(gs_buffer_t, resource) == 0x40);

		static gs_buffer_t s_primitiveBuffers[2];

		static gs_buffer_t* CreateIndexBuffer(ID3D11Device* device, gs_buffer_t& out,
			const uint16_t* indices, uint32_t count, const char* name)
		{
			const uint32_t byteSize = count * sizeof(uint16_t);

			D3D11_BUFFER_DESC desc {};
			desc.ByteWidth = byteSize;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_INDEX_BUFFER;

			D3D11_SUBRESOURCE_DATA initial {};
			initial.pSysMem = indices;

			ID3D11Buffer* buffer = nullptr;
			const HRESULT hr = device->CreateBuffer(&desc, &initial, &buffer);
			if (FAILED(hr) || buffer == nullptr)
			{
				DebugLogFile("[m2ftg::K2] %s index buffer creation FAILED (hr=0x%08X)\n", name, hr);
				return nullptr;
			}

			out.alive = 1;
			out.flags = 0x201;
			out.byte_size = byteSize;
			out.self = &out;
			out.sub = reinterpret_cast<uint8_t*>(&out) + 0x30;
			out.element_count = count;
			out.reserved = 0xFFFFFFFF;
			out.resource = buffer;
			out.sub_flags = out.flags;
			out.sub_size = byteSize;

			DebugLogFile("[m2ftg::K2] %s: %u indices (%u bytes), wrapper=%p buffer=%p\n",
				name, count, byteSize, static_cast<void*>(&out), static_cast<void*>(buffer));
			return &out;
		}

		static void PrimitiveInitialize(const RenderWindow& window)
		{
			ID3D11Device* device = window.GetD3D11Device();
			if (device == nullptr || g_gsContext == nullptr) return;
			if (*reinterpret_cast<void**>(g_gsContext + 0x1418) != nullptr) return;   // re-entry

			// p_ib_quad — {4i, 4i+1, 4i+2, 4i, 4i+2, 4i+3}. The pusher batches at most 0xC0
			// vertices (48 quads) per draw, so 192 quads is well clear of what it can ask for.
			{
				constexpr uint32_t NUM_PRIMITIVES = 192;
				static uint16_t indices[NUM_PRIMITIVES * 6];
				for (uint32_t prim = 0; prim < NUM_PRIMITIVES; prim++)
				{
					const uint16_t base = static_cast<uint16_t>(4 * prim);
					uint16_t* v = &indices[prim * 6];
					v[0] = base; v[1] = base + 1; v[2] = base + 2;
					v[3] = base; v[4] = base + 2; v[5] = base + 3;
				}
				*reinterpret_cast<void**>(g_gsContext + 0x1418) =
					CreateIndexBuffer(device, s_primitiveBuffers[0], indices,
						NUM_PRIMITIVES * 3 * 2, "p_ib_quad");
			}

			// p_ib_fan — {i+2, 0, i+1}, hub at vertex 0, drawn as a triangle list.
			{
				constexpr uint32_t NUM_PRIMITIVES = 512;
				static uint16_t indices[NUM_PRIMITIVES * 3];
				for (uint32_t prim = 0; prim < NUM_PRIMITIVES; prim++)
				{
					uint16_t* v = &indices[prim * 3];
					v[0] = static_cast<uint16_t>(prim + 2);
					v[1] = 0;
					v[2] = static_cast<uint16_t>(prim + 1);
				}
				*reinterpret_cast<void**>(g_gsContext + 0x1420) =
					CreateIndexBuffer(device, s_primitiveBuffers[1], indices,
						NUM_PRIMITIVES * 3, "p_ib_fan");
			}
		}

		static void FillSharedSymbols(const RenderWindow& window)
		{
			if (g_gsContext == nullptr) return;

			if (s_deviceContext == nullptr) s_deviceContext = new uint8_t[0x40000]();
			// +0x18 mp_sbgl_context — the RAW ID3D11DeviceContext (immediate). FUN_180093090 hands
			// exactly this to the render-target setter.
			*reinterpret_cast<void**>(s_deviceContext + 0x18) = window.GetD3D11DeviceContext();
			// +0x28 mp_cb_pool / +0x30 mp_up_pool — size-bucketed buffer tables. FUN_18009F1D0
			// indexes one as `[bucket*0x20 + sizeclass]` (three size classes: <=0x20 inline,
			// <=0x120 at +0xC02, larger at +0x1802) and lazily creates the D3D11 buffer in an empty
			// slot, so the table itself only has to exist and start zeroed. Reached on the first
			// constant-buffer upload of frame 1: a null table faults at DLL+0x9F23C reading
			// [null + index*8].
			if (*reinterpret_cast<void**>(s_deviceContext + 0x28) == nullptr)
			{
				*reinterpret_cast<void**>(s_deviceContext + 0x28) = new uint8_t[0x40000]();
				*reinterpret_cast<void**>(s_deviceContext + 0x30) = new uint8_t[0x40000]();
			}
			// +0x38 — the render-state block. `reset_state_all` (FUN_180091E40) is called as
			// `FUN_180091E40(*(devctx + 0x38))` at the end of every device-context reset and writes
			// straight through it (0x670, 0x3670, 0x3700.., up to 0x4340), so a null here is an
			// instant AV WRITE at addr=0x670. Same field as the YLAD generation's devctx.
			if (*reinterpret_cast<void**>(s_deviceContext + 0x38) == nullptr)
			{
				*reinterpret_cast<void**>(s_deviceContext + 0x38) = new uint8_t[0x8000]();
			}

			// The embedded display sub-object at gs+0xC0 — this generation's cswap_chain. The
			// render-target setter FUN_18009E1E0 reads it through DAT_1803914E8 (= shared symbol
			// slot[2]) when a caller asks for the DEFAULT targets (index -1): colour object at
			// +0x08, depth object at +0x10, and the packed dimensions at +0x20 / +0x40 as
			// `(w-1) | (h-1)<<14`. Same layout as the YLAD generation's cswap_chain.
			// The target objects themselves stay null (binding falls back gracefully); the dims
			// are seeded because they become the viewport width/height.
			uint8_t* sub = g_gsContext + 0xC0;
			*reinterpret_cast<void**>(sub + 0x00) = window.GetSwapChain();
			const uint32_t packedDims = (1280 - 1) | ((720 - 1) << 14);
			*reinterpret_cast<uint32_t*>(sub + 0x20) = packedDims;
			*reinterpret_cast<uint32_t*>(sub + 0x40) = packedDims;

			// sbgl keeps its per-device-context shadow state in a block attached to the
			// ID3D11DeviceContext with a pxd GUID, and fetches it with
			// GetPrivateData(guid, 8, &blockPtr) — ID3D11DeviceChild vtable slot 4 (+0x20). The
			// render-target setter then writes the bound targets and the viewport straight into
			// that block, so an unattached (null) block is an immediate AV WRITE at DLL+0x9E34E.
			// In the real game the host's device-start attaches it; here YAMP does.
			{
				static const GUID kPxdCtxGuid =
					{ 0xa84b07f7, 0x3bd0, 0x4c4e, { 0x89, 0x90, 0xe9, 0xd5, 0xaf, 0x6e, 0xfb, 0xa2 } };
				static uint8_t s_ctxShadowState[0x1000] {};
				void* blockPtr = s_ctxShadowState;
				window.GetD3D11DeviceContext()->SetPrivateData(kPxdCtxGuid, sizeof(blockPtr), &blockPtr);
			}

			void** p = reinterpret_cast<void**>(g_gsContext + 0x20);
			p[0] = window.GetD3D11Device();
			p[1] = sub;
			p[2] = sub;                                        // yes, the same pointer twice
			p[3] = reinterpret_cast<void*>(uintptr_t(1));      // a value, not a pointer
			p[4] = &s_zeroDword;
			p[5] = nullptr;                                    // no allocator in this generation

			*reinterpret_cast<void**>(g_gsContext + 0xA0) = s_gsAllocator;
			*reinterpret_cast<void**>(g_gsContext + 0xB0) = s_deviceContext;

			// Handle (instance) tables — the same bitmap allocator the YLAD host builds, but at
			// **gs+0x1808** with a 0x20 stride, NOT YLAD's gs+0x101718.
			//
			// Structure read straight out of the allocator FUN_180093CC0: it takes `free_top`
			// (+0x18) as a WORD index into the `free_tbl` bitmap (+0x08), requires
			// `free_top < free_words` (+0x1C), pulls the highest set bit, stores the object into
			// `tbl` (+0x00) and returns a 1-BASED id. A zeroed table therefore has
			// `0 < 0 == false` and can NEVER allocate — which is exactly why GSVS resource
			// creation returned 0 and nulled the handle that crashed FUN_18005C050.
			//
			// Offsets in use, from the ADD RCX,imm before every call to that allocator:
			// 0x1808, 0x1828 (the shader/GSVS one), 0x1848, 0x1868, 0x1888, 0x18A8, 0x18E8, 0x1908.
			// A contiguous run of ten covers those plus the 0x18C8 gap (which the lookup/free
			// siblings use). Capacity is uniform and generous because the per-slot meaning is not
			// mapped for this generation — YLAD's order (mesh/tex/vs/ps/...) must NOT be assumed.
			{
				struct instance_tbl
				{
					void** tbl;
					uint64_t* free_tbl;
					uint32_t status;
					uint32_t max;
					uint32_t free_top;
					uint32_t free_words;
				};
				static_assert(sizeof(instance_tbl) == 0x20);

				constexpr uint32_t kCapacity = 16384;
				constexpr uint32_t kWords = (kCapacity + 63) / 64;
				for (int i = 0; i < 10; i++)
				{
					auto* t = reinterpret_cast<instance_tbl*>(g_gsContext + 0x1808 + i * 0x20);
					if (t->tbl != nullptr) continue;   // already built (re-entry safety)
					t->tbl = new void* [kCapacity]();
					t->free_tbl = new uint64_t[kWords];
					for (uint32_t w = 0; w < kWords; w++) t->free_tbl[w] = ~0ull;
					t->status = 0;
					t->max = kCapacity;
					t->free_top = 0;
					t->free_words = kWords;
				}
				DebugLogFile("[m2ftg::K2] instance tables: 10 x %u entries at gs+0x1808\n", kCapacity);
			}

			PrimitiveInitialize(window);

			DebugLogFile("[m2ftg::K2] gs=%p device=%p swapchain=%p devctx=%p\n",
				static_cast<void*>(g_gsContext), static_cast<void*>(window.GetD3D11Device()),
				static_cast<void*>(window.GetSwapChain()), static_cast<void*>(s_deviceContext));
		}

		// ================================== boot ==================================

		static std::filesystem::path gamePath;
		static wil::unique_hmodule gameDll;
		static module_func_t g_moduleMain = nullptr;
		// What module_start writes through params.module_main; cross-checked against the
		// pattern-scanned MODULE_MAIN so a bad pattern cannot go unnoticed.
		static module_func_t g_moduleMainFromParams = nullptr;

		// Applied immediately before EVERY module_main call. A per-HOST-FRAME assert is not
		// enough, and each of the two times that was assumed it cost a two-machine run: one host
		// frame runs a BURST of module_main calls while the counter holds (all of boot), the
		// module's own bring-up zeroes the two-board gate mid-burst, and if the ROM's InitNetwork
		// runs later in the same burst it reads gate=0 and assigns NOLINK - the machine comes up
		// standalone. Asserting here means the gate is only ever down inside the one call that
		// zeroed it, and the read that matters is always calls later.
		static void AssertTwoBoardGate()
		{
			if (g_twoBoardGate != nullptr && WantTwoBoardMode())
			{
				*g_twoBoardGate = 1;
			}
		}

		// ---- HOLD THE RING DOWN until a linked cabinet is actually there ----------------------
		//
		// WHY THIS IS NOT AN HLE HOOK, which is the question that led here. The ROM's link check
		// (`Net_check`, i960 0xC5870) runs from `BlackOut+0x13C`, BEFORE the mainloop and therefore
		// before the warning screen - and NOTHING hooks it. Every hook in that address range is
		// Inert. The check nevertheless succeeds on a machine with one board and no peer, and the
		// comm board says why:
		//
		//     [comm] B0 reset=01 flag=00 bank0=01000102 bank1=01010102
		//                                       ^^  ^^^^
		//                                    byte0=1  byte2=1 byte3=2
		//
		// Those are the FIRMWARE status bytes - ring up, node id, node count. The ROM only ever
		// READS them; on hardware the comm board's own firmware writes them once the serial ring is
		// healthy. The module has no firmware, and what it leaves there is a fake healthy TWO-NODE
		// ring. So `Net_check` polls "is the ring up?" (yes), "are there two nodes?" (yes) and
		// declares success against a wire that does not exist.
		//
		// WHAT THE MODULE'S "FIRMWARE" ACTUALLY IS, found with a hardware write watchpoint on the
		// byte (there are no xrefs - the block is reached through a pointer). `FUN_18006A790` is the
		// comm board's REGISTER-WRITE handler, and it responds to the ROM releasing the board from
		// reset by faking a firmware boot:
		//
		//     6A7D1  cmp   dword [r8+0x8004], 0    ; "already booted" latch
		//     6A7DF  test  byte  [r8+0x8000], 1    ; CommBoardReset bit0 - released?
		//     6A7ED  movzx ecx, byte [0x6911B4]    ; the BOARD INDEX
		//     6A7FF  inc   cl                      ; node id = board index + 1
		//     6A818  mov   byte [rax+r8], 1        ; byte0 = 1  RING UP      ] and again for
		//     6A81D  mov   byte [rax+r8+2], cl     ; byte2 = node id         ] the other bank
		//     6A822  mov   byte [rax+r8+3], 2      ; byte3 = 2  NODE COUNT   ]
		//
		// Ring-up and node count are IMMEDIATES: it declares a healthy two-node ring with nothing
		// attached. That is the whole reason the boot-time link check has always succeeded.
		//
		// TWO CONSEQUENCES SHAPE THIS FUNCTION.
		//
		// 1. It writes the status ONCE, at release, and never again - the latch at +0x8004 sees to
		//    that. Real firmware reports continuously, because a ring can go DOWN. So this runs
		//    every call rather than seeding once, and that is measured, not defensive: writing the
		//    zero only on the first call leaves `bank0=01000102` by the first sample and the board
		//    boots straight through, because the ROM resets and releases the comm board on every
		//    `Net_check` and every `BlackOut` re-runs `Net_check`.
		//
		// 2. NODE ID COMES FROM THE BOARD INDEX, which is wrong for us. That was fine for the
		//    module's own two-local-boards arrangement, but one board per machine means the index
		//    is 0 on BOTH peers, so both would claim node id 1 - and step 12 of the check copies
		//    byte 2 into guest 0x5770D1. Node id here comes from the CABINET ROLE instead. It is a
		//    different value from `link_ID` (0x5770B1, which the cabinet-role backup byte drives)
		//    and the two have to agree.
		//
		// The ROM's reaction to a ring that is down is exactly what a pair of cabinets needs: it
		// sits in the check's ring-up poll showing "Checking Network Now", which YIELDS THROUGH
		// `synch` every frame - so module_main keeps returning and the host stays responsive. Do
		// NOT express "no link" by withholding flag bit7 instead: that poll is four instructions
		// with no frame yield and hangs the process (it is what the reset-semantics shim exists to
		// avoid), which is also why this deliberately leaves the flag register alone.
		//
		// Both banks, because bit0 of the flag register decides which one the i960 is reading and
		// it flips under us. Board 0 only: board 1, when it exists, is the local second cabinet and
		// its ring is the module's own business.
		// ---- SOFT-RESET A RUNNING CABINET INTO ANOTHER ROLE -----------------------------------
		//
		// Joining a room has to be able to turn an already-booted cabinet into the SLAVE without
		// restarting YAMP, and the ROM makes that easy because it RE-HANDSHAKES ON EVERY PASS
		// THROUGH `BlackOut`, by design:
		//
		//     18784  call sub_186C0     ; link_ID = 3, net_flag = 0   <- re-arms the check
		//     18788  call sub_18960     ; InitAll
		//     1878C  call sub_18A10     ; InitNetwork -> Net_check    <- re-reads the role byte
		//
		// and `BlackOut` is MainMode 2 in the mainloop's own mode table at 0x18680. So driving the
		// cabinet back through it is two guest words: MainMode = 2, and net_flag = 0 so
		// `Net_check`'s step-1 early-out ("already checked") does not skip the whole thing. The ROM
		// then zeroes SubMode / MainMode / VersusMode / FieldNo on entry and boots through normally
		// from the Warning screen with the new role - which is what a soft reset should look like.
		//
		// The role byte itself is just re-patched: `ApplyCabinetRole` is one verified `VP::Patch`
		// and is idempotent, so it can be called at any point.
		//
		// THIS FILE'S OLD WARNING ABOUT SOFT RESTARTS DOES NOT APPLY. `docs/von-netplay-recon.md`
		// records that the test-exit restart "WIPES NOTHING" and desynced peers at frame 0 - but
		// that was under LOCKSTEP, which needed bit-identical RAM on both machines. The link now
		// carries the ROM's own protocol and has no determinism requirement, so a restart that
		// leaves attract-mode leftovers in work RAM is simply what a real cabinet does.
		// *** THIS DOES NOT WORK YET - DO NOT TRUST IT. Measured 2026-08-06: booted NOLINK, fired
		// this at frame 600. The role byte patched correctly, but the ROM never re-ran its link
		// check - `link_ID` stayed 3 (standalone) and the cabinet jumped FORWARD into character
		// select (MainMode 4) instead of back through BlackOut. If `InitNetwork` had re-run,
		// link_ID would read 1.
		//
		// So `MainMode = 2` is the wrong lever. It is a REQUEST the mainloop only acts on between
		// mode handlers, and the handler already running sets MainMode itself on the way out - so
		// the write either loses the race or lands as a forward transition. The next thing to try
		// is the cabinet's TEST switch in and out: that is the ROM's own re-handshake path, YAMP
		// already has the switch plumbing (I960_IO_REFRESH_CALL / SystemSwitches), and the ROM
		// chooses when to act on it, so it cannot race a mode handler.
		// TEST IS A MOMENTARY PUSH, NOT A HOLD: one press enters the operator menu, a second press
		// leaves it. An earlier version simply held the switch down for 90 frames and did appear to
		// work - almost certainly by auto-repeating through menu items until it happened to land on
		// exit, which is the kind of thing that works once and then quietly stops.
		//
		// So: press, wait until the ROM is ACTUALLY in the menu, press again. Keyed off MainMode
		// rather than off timings, because the entry is the ROM's decision and a fixed delay is a
		// guess. Modes 5-7 are the test menu (mode table 0x18680: 0xF3F00 / 0xF3FE0 / 0xF3D30, all
		// in the test-menu region); the run that proved this observed MainMode 6.
		enum class RoleReset { Idle, PressIn, WaitMenu, PressOut, Settle };
		constexpr int ROLE_RESET_PRESS_FRAMES = 6;     // a press the I/O refresh cannot miss
		constexpr int ROLE_RESET_MENU_TIMEOUT = 600;   // give up rather than push TEST forever
		static RoleReset s_roleReset = RoleReset::Idle;
		static int s_roleResetTimer = 0;

		static bool InTestMenu()
		{
			if (g_dllBase == nullptr)
			{
				return false;
			}
			const uint32_t mode =
				*reinterpret_cast<uint32_t*>(I960At(g_dllBase, 0, OmgRva::SYM_MAINMODE));
			return mode >= 5 && mode <= 7;
		}

		// Returns whether TEST should be asserted this frame. Called once per HOST frame from the
		// switch position, so the counters are frames, not module_main calls.
		static bool DriveRoleResetSwitch()
		{
			switch (s_roleReset)
			{
			case RoleReset::PressIn:
				if (--s_roleResetTimer > 0) return true;
				s_roleReset = RoleReset::WaitMenu;
				s_roleResetTimer = ROLE_RESET_MENU_TIMEOUT;
				return false;                       // release: the ROM acts on the edge
			case RoleReset::WaitMenu:
				if (InTestMenu())
				{
					s_roleReset = RoleReset::PressOut;
					s_roleResetTimer = ROLE_RESET_PRESS_FRAMES;
					return true;                    // second press: leave the menu
				}
				if (--s_roleResetTimer <= 0)
				{
					s_roleReset = RoleReset::Idle;
					net::Logf("soft reset ABANDONED - the cabinet never entered the test menu");
				}
				return false;
			case RoleReset::PressOut:
				if (--s_roleResetTimer > 0) return true;
				s_roleReset = RoleReset::Settle;
				return false;
			case RoleReset::Settle:
				if (!InTestMenu())
				{
					s_roleReset = RoleReset::Idle;
					net::Logf("soft reset: left the test menu - the ROM's exit path has cleared "
						"net_flag and will re-run its link check");
				}
				return false;
			default:
				return false;
			}
		}

		static void SoftResetIntoRole(uint32_t role)
		{
			if (g_dllBase == nullptr || !m2ftg::IsBoardBooted())
			{
				return;
			}
			if (!ApplyCabinetRole(g_dllBase, role))
			{
				return;   // the injector site did not verify; ApplyCabinetRole has already said so
			}
			// Pulse the cabinet's TEST switch instead of writing MainMode, for the reason above:
			// the ROM samples the switch and decides for itself when to act, so this cannot race a
			// mode handler. Holding TEST takes it into the operator menu; RELEASING it runs the
			// exit routine (i960 0xF3C80: `net_flag = 0; SubMode = 0; MainMode = g4+1`), and the
			// cleared net_flag is what forces `Net_check` to run in full again - which is where the
			// re-patched cabinet byte gets read.
			//
			// SetSystemSwitches drives BOTH boards, which is not optional: driving TEST into board
			// 0 alone deadlocks a linked pair (board 0 stops servicing the link, board 1 spins in
			// Net_check, board 0 blocks in synch waiting for board 1).
			s_roleReset = RoleReset::PressIn;
			s_roleResetTimer = ROLE_RESET_PRESS_FRAMES;
			net::Logf("soft reset into %s - pressing TEST to enter the operator menu",
				CabinetRoleName(role));
		}

		// THE ROOM DECIDES THE ROLE: host = MASTER, guest = SLAVE.
		//
		// The role belongs to the MACHINE, and in an RPCN session the machine's identity is
		// already settled by who created the room - so making the player also pick MASTER/SLAVE
		// in the settings would be an invitation to two masters and no link. The room therefore
		// overrides the setting for as long as it lasts, and `local_player` (0 = host, 1 = guest)
		// is the same value everything else in netplay routes by.
		//
		// A change here is a real soft reset of a running board, so it is driven exactly once per
		// transition rather than reasserted: `s_cabinetRole` is what was actually applied, and
		// SoftResetIntoRole is a no-op path if the board is not booted yet - it will be tried
		// again next frame.
		static bool DriveRoomCabinetRole()
		{
			const net::Status status = net::GetStatus();
			if (status.local_player < 0)
			{
				return false;   // connected, perhaps, but no room yet - the setting still rules
			}
			const uint32_t wanted = (status.local_player == 0) ? 1u : 2u;
			if (s_cabinetRole != wanted)
			{
				net::Logf("room: local player %d -> this cabinet is the %s",
					status.local_player, CabinetRoleName(wanted));
				SoftResetIntoRole(wanted);
			}
			return true;
		}

		// Watches the setting and drives the reset when it changes under a running board. Separate
		// from the firmware below because it must run whether or not the link is being reported.
		static void DriveCabinetRoleChanges()
		{
			// In a room the role is not the player's to choose - see DriveRoomCabinetRole. Note
			// this also stops the setting watcher below from fighting the room for the role.
			if (DriveRoomCabinetRole())
			{
				return;
			}
			const YAMPSettings* settings = gGeneral.GetSettings();
			if (settings == nullptr)
			{
				return;
			}
			// Seeded from the value Run() applied before module_start, so the first frame is not
			// mistaken for a change.
			static uint32_t s_applied = 0xFFFFFFFF;
			if (s_applied == 0xFFFFFFFF)
			{
				s_applied = settings->m_vonCabinetRole;
				return;
			}
			if (s_applied == settings->m_vonCabinetRole)
			{
				return;
			}
			s_applied = settings->m_vonCabinetRole;
			SoftResetIntoRole(s_applied);
		}

		struct CommLink
		{
			bool peerUp;        // are the peer's datagrams arriving right now?
			uint8_t nodes;      // how many cabinets are on the ring, including this one
		};

		// THE LINK RUNS OVER THE RPCN SESSION'S P2P SOCKET, and that is the only transport.
		//
		// A direct address:port harness lived here while the comm-board firmware model was being
		// brought up, because RPCN cannot do loopback - it hardcodes 3658 in the address it hands a
		// peer - so two instances on one machine were the only way to exercise the link at all. It
		// did that job and is gone: RPCN is the protocol this ships on, and a second transport that
		// nothing tests is a second set of behaviours to keep true.
		static bool LinkAvailable()
		{
			return net::LinkReady();
		}

		constexpr int RPCN_LINK_TIMEOUT_FRAMES = 30;
		static int s_rpcnSinceRecv = RPCN_LINK_TIMEOUT_FRAMES + 1;

		// "Is a linked partner exchanging with us RIGHT NOW?" Cheap and side-effect free, unlike
		// ObserveLink, which also opens the harness transport - so this is what callers outside
		// the firmware path (the frame pacer) ask.
		static bool LinkIsLive()
		{
			if (s_cabinetRole == 0)
			{
				return false;
			}
			return LinkAvailable() && s_rpcnSinceRecv <= RPCN_LINK_TIMEOUT_FRAMES;
		}

		// THE ONE FUNCTION THE TRANSPORT HAS TO REPLACE. Everything else below is the firmware's
		// side of the wire and does not care where the answer comes from.
		static CommLink ObserveLink()
		{
			const bool up = LinkIsLive();
			return CommLink{ up, static_cast<uint8_t>(up ? 2 : 1) };
		}

		// The ROM's own acceptance test, applied to a packet off the wire. OBSERVABILITY, not a
		// filter: an unstamped packet is passed to the comm RAM exactly like a stamped one, because
		// it is not corruption. It is what a cabinet sends before its `net_flag` goes up - the ROM
		// does not stamp anything until the vsync path at 0x14A0 runs, so a zeroed CommSend fails
		// this (0x0000 against 0xAE5E) throughout both cabinets' link checks. That traffic is what
		// brings the RING up; only its content is meaningless, and the peer's ROM discards it for
		// itself one frame later.
		//
		// What the count is FOR: it separates "datagrams are arriving" from "the peer's ROM is past
		// its link check and stamping real packets", and that distinction is the one the last
		// two-machine run could not make.
		static bool LinkPacketStamped(const uint8_t* packet)
		{
			uint16_t seq = 0, check = 0;
			memcpy(&seq, packet + OmgRva::PKT_SEQ, sizeof(seq));
			memcpy(&check, packet + OmgRva::PKT_CHECK, sizeof(check));
			return check == static_cast<uint16_t>(seq ^ OmgRva::PKT_CHECK_XOR);
		}

		static uint32_t s_pktStamped = 0;    // packets off the wire the ROM will accept
		static uint32_t s_pktUnstamped = 0;  // ...and ones it will not (pre-link-up traffic)

		// The newest packet off the wire, held resident. NOT consumed on delivery: the module's own
		// link transfer overwrites this cabinet's CommData every frame (with board 1's send buffer,
		// which on a one-board machine is 0x700 zero bytes), so a packet written once and left
		// alone survives exactly until the next `module_main`. Keeping the last one and re-laying it
		// after every transfer is what a real DPRAM does between arrivals, and it is what makes a
		// dropped datagram cost nothing.
		static uint8_t s_peerPacket[OmgRva::COMM_PAYLOAD];
		static bool s_peerHave = false;

		// Counts datagrams actually put on the wire, so the send-rate change is measurable rather
		// than asserted. Reset per host frame by LogLinkState.
		static uint32_t s_linkSends = 0;
		static bool s_sentThisHostFrame = false;
		static void LinkSendMark() { ++s_linkSends; s_sentThisHostFrame = true; }

		// The SEND half: our CommSend onto the wire. Runs before module_main, which is where the
		// bank it reads comes from - MEASURED, not assumed. Both banks of CommSend hold a valid
		// stamped packet at any time, and the one matching the counter the ROM stamped into cSend
		// is bank `flag & 1`:
		//
		//   flag=01  send0=113D/BF63  send1=DD60/733E  tx=DD60      <- bank 1
		//   flag=00  send0=FAE0/54BE  send1=F060/5E3E  tx=FAE0      <- bank 0
		//
		// which is simply the ROM's vsync path doing its send and receive memcpys back to back
		// through the same guest window, so both land in whichever bank is current.
		static void ExchangeCommPayload()
		{
			if (g_dllBase == nullptr)
			{
				return;
			}
			uint8_t* const block = g_dllBase + OmgRva::COMM_P1;
			uint8_t* const flag = g_dllBase + OmgRva::COMM_P1_FLAG;
			const size_t front = (*flag & 1) ? OmgRva::COMM_BANK : 0;

			// ONLY TRANSMIT ONCE OUR OWN COMM BOARD HAS BEEN RELEASED FROM RESET.
			//
			// Without this, "the peer is up" means no more than "the other YAMP process is sending
			// datagrams", which is true from its first emulated frame - long before its ROM reaches
			// `Net_check`. Observed: the instant the slave booted, the MASTER's ring came up, its
			// check completed and it walked off into the warning screen while the slave was still
			// booting. The two cabinets end up at completely different points in their boot
			// sequences, and every flap of that signal re-runs somebody's check.
			//
			// `CommBoardReset` bit0 is the ROM's own "I have released the board" - step 5 of the
			// check, and the same edge the module's firmware stub keys its fake boot off. Gating
			// transmission on it makes peer liveness mean what it means on hardware: the other
			// cabinet's CPU has brought its comm board up and is participating in the ring.
			if ((*(g_dllBase + OmgRva::COMM_P1_FLAG - 1) & 1) == 0)
			{
				return;
			}
			const uint8_t* const outgoing = block + 0x2000 + front;

			// ONE PACKET OUT AND ONE PACKET IN PER BOARD FRAME, NOT PER module_main CALL.
			//
			// `StepOneBoardFrame` calls this before every call it makes, which is one to three per
			// board frame in the steady state and up to sixteen during boot - so the same packet
			// went out two or three times over. The ROM stamps a fresh counter at +2 exactly once
			// per board frame (i960 0x14F8), which makes "is this a packet the peer has not seen"
			// free to answer and needs no state of our own beyond the last value sent.
			//
			// The heartbeat is NOT optional. Before `net_flag` goes up the ROM stamps nothing, so
			// the counter is frozen at zero and a pure change-detector would transmit nothing at
			// all - and it is exactly that traffic which brings the peer's ring up in the first
			// place. So: act on a new stamp, or once per host frame when there is no stamp yet.
			//
			// THE TAKE IS RATE-LIMITED FOR A SHARPER REASON THAN THE SEND. The plugin queues
			// received packets in order so no one-frame state is discarded on the way in - but
			// `DeliverCommPayload` lays down whatever `s_peerPacket` holds, and the ROM reads the
			// comm window ONCE per board frame. Taking a packet per module_main call therefore
			// overwrote s_peerPacket two or three times between reads and showed the ROM only the
			// last of them, which is the same discard the queue exists to prevent, just moved
			// downstream. Measured: with the queue in and the take unthrottled, the peer's 0x24
			// still never reached the ROM and the cabinets still loaded different stages.
			//
			// One in, one out, per board frame - matching the rate the peer produces them.
			uint16_t seq = 0;
			memcpy(&seq, outgoing + OmgRva::PKT_SEQ, sizeof(seq));
			static uint16_t s_lastSentSeq = 0;
			const bool stampIsNew = (seq != s_lastSentSeq);
			if (!stampIsNew && s_sentThisHostFrame)
			{
				return;
			}
			s_lastSentSeq = seq;
			LinkSendMark();

			// Nothing arrived: keep the packet we already have. Delivery is DeliverCommPayload's
			// job, and it happens after the module's transfer rather than here - see there.
			net::LinkSend(outgoing, OmgRva::COMM_PAYLOAD);
			if (net::LinkTake(s_peerPacket, OmgRva::COMM_PAYLOAD) != OmgRva::COMM_PAYLOAD)
			{
				return;
			}
			s_rpcnSinceRecv = 0;
			s_peerHave = true;
			(LinkPacketStamped(s_peerPacket) ? s_pktStamped : s_pktUnstamped)++;
		}

		// THE RECEIVE HALF, and its position in the frame is the whole point.
		//
		// Called from the link-transfer shim IMMEDIATELY AFTER the module's own transfer and before
		// either board's CPU steps, because the module's transfer is a writer into the very window
		// this fills. On a one-board cabinet it copies board 1's send buffer - never executed, all
		// zeros - straight over the peer's packet, and the ROM then stages 0x700 zero bytes and
		// rejects them (0x0000 against 0xAE5E). That is precisely what was measured: valid stamped
		// packets sitting in comm RAM frame after frame, `stage=0000/0000`, `rx=FF`, and cRecn
		// never written once in a 1200-frame run on either cabinet.
		//
		// BOTH BANKS, deliberately, and no flag poke at all. The bank the i960 reads is the module's
		// business - its transfer toggles the selector on its own schedule, and YAMP's attempts to
		// drive bit0 were simply overwritten - so writing both removes the question instead of
		// answering it. Nothing is lost by doing so: the two banks exist to stop a DMA tearing
		// against a CPU read, and YAMP writes between emulated instructions where no tear is
		// possible. bit7 (data ready) is left to the module's transfer, which sets it every frame
		// and is what `Net_check` step 9 is waiting for.
		static void DeliverCommPayload()
		{
			if (!s_peerHave || g_dllBase == nullptr || s_cabinetRole == 0)
			{
				return;
			}
			uint8_t* const block = g_dllBase + OmgRva::COMM_P1;
			memcpy(block + 0x2700, s_peerPacket, OmgRva::COMM_PAYLOAD);
			memcpy(block + OmgRva::COMM_BANK + 0x2700, s_peerPacket, OmgRva::COMM_PAYLOAD);
		}

		static void DriveCommFirmware()
		{
			// A LIVE ROOM IMPLIES THE HOLD. "Wait for the other cabinet at boot" is a local
			// preference for a standalone board; joining a room is an explicit statement that
			// there IS another cabinet, and a player who joined one and left the box unticked
			// would get a session where nothing links and nothing says why.
			const YAMPSettings* settings = gGeneral.GetSettings();
			if (g_dllBase == nullptr || settings == nullptr
				|| (!settings->m_vonHoldLink && !LinkAvailable()))
			{
				return;
			}
			// A standalone cabinet never runs the check (link_ID 3 exits it early), so there is no
			// ring to report on and nothing here should touch the module's own state. The APPLIED
			// role, not the setting - see s_cabinetRole.
			const uint32_t role = s_cabinetRole;
			if (role == 0)
			{
				return;
			}

			const CommLink link = ObserveLink();
			const uint8_t ringUp = link.peerUp ? 1 : 0;
			const uint8_t nodeId = static_cast<uint8_t>(role);   // MASTER -> 1, SLAVE -> 2
			// Truthful rather than convenient. While the ring is down the ROM is parked at the
			// ring-up poll and never reads this, but if a future change raises byte 0 without a peer
			// the ROM will take its own "Illegal Nodes: %d" path - reset the comm board and restart
			// the check. That is correct behaviour for a mis-reported ring, not a bug to paper over.
			const uint8_t nodeCount = link.nodes;

			static int s_reported = -1;
			if (s_reported != ringUp)
			{
				s_reported = ringUp;
				net::Logf("comm firmware: ring %s, node id %u of %u%s",
					ringUp ? "UP" : "DOWN", nodeId, nodeCount,
					ringUp ? "" : " - the ROM will wait in \"Checking Network Now\"");
			}

			for (size_t bank = 0; bank <= OmgRva::COMM_BANK; bank += OmgRva::COMM_BANK)
			{
				uint8_t* status = g_dllBase + OmgRva::COMM_P1 + bank;
				status[0] = ringUp;
				status[2] = nodeId;
				status[3] = nodeCount;
			}

			// After the status, because the exchange is what raises "data ready" and the ROM reads
			// the two together.
			ExchangeCommPayload();

			// AND FINISH THE MODULE'S OWN BRING-UP, which a ring that never comes up would starve.
			//
			// `FUN_180072FB0` sits in state 0x12 until DLL flag 0x6910DF goes non-zero, and the only
			// thing that sets it is hook 0's handler - which the emulated ROM reaches at
			// `BlackOut+0x190`, i.e. AFTER `Net_check` RETURNS. Parking the board inside the link
			// check therefore parks the module one state short of finished, for as long as the hold
			// lasts, and the board is drawing frames nobody composites: measured `brought-up=0` and
			// a black window across a whole run while `GlobCntr` advanced normally.
			//
			// So say it ourselves. This is not a shortcut past anything - it is the same write the
			// hook makes, at the only moment it can be made, because the ROM is deliberately not
			// going to get there until a peer answers. The hook's other job (clearing 0x2000 bytes
			// of text RAM) is precisely what must NOT happen here: that clear is what wipes the site
			// screen the moment the check completes.
			// Only while the ring is DOWN: once a peer answers, the ROM completes the check and
			// reaches BlackOut+0x190 on its own, and hook 0 does the write for real.
			if (!ringUp && g_dllBase[0x6910DF] == 0)
			{
				g_dllBase[0x6910DF] = 1;
				net::Logf("bring-up flag forced - the ROM is parked in Net_check waiting for the "
					"ring and cannot set it itself");
			}
		}

		// PUMP THE RPCN SESSION. Once per host frame, and Virtual On is the only game that has to
		// do this for itself. (Declaration order: the definition follows the link helpers it uses.)
		// do this for itself.
		//
		// Every other title reaches poll() through `NetSession::Drive`, which also runs the whole
		// lockstep round flow - barrier, seed, frame numbering, state checks. Virtual On uses NONE
		// of that: the ROM's own link protocol does the synchronising, so K2Host never calls
		// NetSession at all. That is the right architecture and it left one thing dangling - the
		// session's connect, its RPCN signalling, its transport update and its socket drain all
		// hang off poll(), so without this the room would never form and link_ready would never
		// go true, on the lobby path as well as the command-line one.
		static void DriveNetSession()
		{
			if (!net::IsAvailable())
			{
				return;
			}
			net::Api()->poll(net::Session());
			net::DriveSession();   // -net-host / -net-join; idempotent, inert for a lobby session
		}

		// Reported to the netplay overlay - see K2Host.h. Reads the ROM's own answers rather than
		// YAMP's intentions: node id/count are what `Net_check` copied out of the ring, and
		// `checkDone` is net_flag, so an overlay saying "linked" is saying what the CABINET
		// thinks, not what the transport hopes.
		bool GetLinkedCabinet(LinkedCabinet& out)
		{
			if (s_cabinetRole == 0 || g_dllBase == nullptr || !m2ftg::IsBoardBooted())
			{
				return false;
			}
			out.role = s_cabinetRole;
			out.ringUp = LinkIsLive();
			out.nodeId = *I960At(g_dllBase, 0, OmgRva::SYM_NODE_ID);
			out.nodes = *I960At(g_dllBase, 0, OmgRva::SYM_TOTAL_NODES);
			out.checkDone = *I960At(g_dllBase, 0, OmgRva::SYM_NET_FLAG) == 1;
			return true;
		}

		// ---- THE ROM-LEVEL PROOF, in yampnet.log -----------------------------------------------
		//
		// Called once per HOST frame. Two jobs, and the first one is not cosmetic: this is what
		// ages the link's peer timeout, and it has to happen here rather than in the exchange
		// because a host frame is one to sixteen `module_main` calls.
		//
		// The second is the diagnostic this work has been missing. Everything the link produced so
		// far was TRANSPORT-level - `ring UP, node id 1 of 2` says our own firmware model answered,
		// not that the ROM believed it - and the `[comm]`/`net=`/`id=` probe goes through
		// DebugLogFile, which needs a debugger attached to read. So a two-machine run could show
		// traffic flowing and leave "did the cabinet actually link?" to somebody watching a screen.
		// These fields are the ROM's own verdict, in the log both peers already write:
		//
		//   net=1                  Net_check completed - "Network Check Success" (step 12)
		//   id=1 node=1/2          link_ID, and the node id/count the check copied out of the ring
		//   rx=<state>             cRecn+4: 0xFE no net_flag, 0xFF ring down or packet REJECTED,
		//                          anything else came out of a peer packet the ROM ACCEPTED
		//   tx/rx seq              the stamped counters - both moving means both ROMs are past
		//                          their checks and exchanging real state
		//
		// Logged on change of the discrete fields plus a slow heartbeat, so a whole run is a few
		// dozen lines and the transition that matters is never lost between samples.
		static void LogLinkState(int frame)
		{
			// The no-stamp heartbeat is once per HOST frame; this is where that frame turns over.
			s_sentThisHostFrame = false;

			// Age the peer timeout - see s_rpcnSinceRecv. HOST frames, because a host frame is one
			// to sixteen module_main calls and a timeout counted down there would be of unknown,
			// machine-dependent length.
			if (s_rpcnSinceRecv <= RPCN_LINK_TIMEOUT_FRAMES)
			{
				++s_rpcnSinceRecv;
			}

			// s_cabinetRole is the game gate as well as the role gate: it is only ever non-zero
			// after ApplyCabinetRole verified hook 6's InitNetwork injector, which exists in `omg`
			// alone. The RVAs below are Virtual On work-RAM addresses and mean nothing in VF2.
			const YAMPSettings* settings = gGeneral.GetSettings();
			if (g_dllBase == nullptr || settings == nullptr || !settings->m_vonLinkLog
				|| s_cabinetRole == 0 || !m2ftg::IsBoardBooted())
			{
				return;
			}
			const auto u8 = [](uint32_t a) { return *I960At(g_dllBase, 0, a); };
			const auto u16 = [](uint32_t a) {
				uint16_t v = 0; memcpy(&v, I960At(g_dllBase, 0, a), sizeof(v)); return v;
			};

			const uint8_t netFlag = u8(OmgRva::SYM_NET_FLAG);
			const uint8_t linkId  = u8(OmgRva::SYM_LINK_ID);
			const uint8_t nodeId  = u8(OmgRva::SYM_NODE_ID);
			const uint8_t nodes   = u8(OmgRva::SYM_TOTAL_NODES);
			const uint8_t rxState = u8(OmgRva::SYM_CRECN + OmgRva::PKT_STATE);
			const uint16_t txSeq  = u16(OmgRva::SYM_CSEND + OmgRva::PKT_SEQ);
			const uint16_t rxSeq  = u16(OmgRva::SYM_CRECN + OmgRva::PKT_SEQ);
			const auto u32 = [](uint32_t a) {
				return *reinterpret_cast<uint32_t*>(I960At(g_dllBase, 0, a));
			};
			const uint32_t mode   = u32(OmgRva::SYM_MAINMODE);

			// THE FIELDS THAT NAME A STAGE DESYNC'S CAUSE.
			//
			// `rx=` alone cannot: it is the PEER's state, and the two ways the cabinets can end up
			// on different stages differ only in what THIS cabinet did. Either both rolled (both
			// had VersusMode != 0 at i960 0xCDB0C, each published its own stage and adopted the
			// other's), or the adopter never saw the single-frame 0x24 that carries the roll. So:
			//
			//   tx=      this cabinet's own published state (cSend+4) - 0x24 means "I have the
			//            stage", and BOTH cabinets showing 0x24 having each rolled is the first
			//            case, caught red-handed
			//   stage=   the value in the payload, out (cSend+8) and in (cRecn+8)
			//   sel=     0x503A84, what this cabinet will actually load
			//   vs=      VersusMode (0x503A7C), the flag that decides roll-vs-adopt
			//   field=   FieldNo (0x5770F0), the stage the match is running
			//
			// Compare `sel=` across the two logs and the disagreement is visible directly; compare
			// `tx=`/`vs=` at the frame it appears and the cause is visible too.
			const uint8_t txState  = u8(OmgRva::SYM_CSEND + OmgRva::PKT_STATE);
			const uint16_t txStage = u16(OmgRva::SYM_CSEND + OmgRva::PKT_STAGE);
			const uint16_t rxStage = u16(OmgRva::SYM_CRECN + OmgRva::PKT_STAGE);
			const uint32_t stageSel = u32(OmgRva::SYM_STAGE_SEL);
			const uint32_t versus   = u32(OmgRva::SYM_VERSUSMODE);
			const uint32_t fieldNo  = u32(OmgRva::SYM_FIELDNO);

			// The discrete state, packed so one comparison decides whether anything moved. The
			// sequence counters are deliberately OUT of it: they change every frame by design, and
			// including them would turn "log on change" into "log every frame".
			const uint32_t key = netFlag | (linkId << 8) | (nodeId << 16) | (nodes << 20)
				| (rxState << 24);
			// The stage handshake gets its own key rather than being squeezed into the one above,
			// because every field in it has to be able to trigger a line ON ITS OWN - the whole
			// point is to catch the one frame a value changes.
			const uint64_t stageKey = static_cast<uint64_t>(txState)
				| (static_cast<uint64_t>(versus) << 8)
				| (static_cast<uint64_t>(stageSel) << 24)
				| (static_cast<uint64_t>(fieldNo) << 40);
			static uint32_t s_lastKey = 0xFFFFFFFF;
			static uint32_t s_lastMode = 0xFFFFFFFF;
			static uint64_t s_lastStageKey = 0xFFFFFFFFFFFFFFFFull;
			const bool changed = key != s_lastKey || mode != s_lastMode
				|| stageKey != s_lastStageKey;
			s_lastStageKey = stageKey;
			if (!changed && (frame % 120) != 0)
			{
				return;
			}
			s_lastKey = key;
			s_lastMode = mode;


			// WHERE THE PEER'S PACKET IS AT THE END OF A HOST FRAME. Three points along the path it
			// takes, so a failure names its own cause instead of leaving a choice of suspects:
			//
			//   data0/data1  the emulated comm RAM's receive window, BOTH banks - what YAMP wrote
			//   stage        guest 0x501CE0, what the ROM's own memcpy picked up out of it
			//   cRecn        only written if the ROM accepted it
			//
			// Each is the packet's <seq>/<check> pair, and the pair is self-validating: a live
			// packet has check == seq ^ 0xAE5E, so a glance says whether what is sitting there is a
			// real packet, a stale one, or zeros.
			const auto pair = [](const uint8_t* p, char* out, size_t n) {
				uint16_t seq = 0, check = 0;
				memcpy(&seq, p + OmgRva::PKT_SEQ, sizeof(seq));
				memcpy(&check, p + OmgRva::PKT_CHECK, sizeof(check));
				snprintf(out, n, "%04X/%04X%s", seq, check,
					check == static_cast<uint16_t>(seq ^ OmgRva::PKT_CHECK_XOR) ? "*" : "");
			};
			char data0[16], data1[16], stage[16], send0[16], send1[16];
			pair(g_dllBase + OmgRva::COMM_P1 + 0x2700, data0, sizeof(data0));
			pair(g_dllBase + OmgRva::COMM_P1 + OmgRva::COMM_BANK + 0x2700, data1, sizeof(data1));
			pair(I960At(g_dllBase, 0, 0x501CE0), stage, sizeof(stage));
			// And the OUTGOING window, both banks. `tx` above is what the ROM stamped into cSend;
			// these are where that landed once its own memcpy put it in comm RAM, which is what
			// decides whether the bank YAMP transmits from is the one the ROM just wrote.
			pair(g_dllBase + OmgRva::COMM_P1_SEND, send0, sizeof(send0));
			pair(g_dllBase + OmgRva::COMM_P1_SEND + OmgRva::COMM_BANK, send1, sizeof(send1));
			const char* verdict =
				netFlag != 1                        ? "link check has NOT completed" :
				rxState == OmgRva::PKT_STATE_NONET  ? "checked, but the ROM sees no net_flag" :
				rxState == OmgRva::PKT_STATE_NOPKT  ? "checked - no packet accepted this frame" :
				                                      "LINKED - accepting the peer's packets";
			net::Logf("[von] f=%d %s | net=%u id=%u node=%u/%u main=%u "
				"tx=%02X rx=%02X vs=%u sel=%u field=%u stage tx=%04X rx=%04X | "
				"seq tx=%04X rx=%04X | stamped=%u unstamped=%u | "
				"flag=%02X send0=%s send1=%s data0=%s data1=%s stg=%s xfer=%u",
				frame, verdict,
				netFlag, linkId, nodeId, nodes, mode,
				txState, rxState, versus, stageSel, fieldNo, txStage, rxStage,
				txSeq, rxSeq, s_pktStamped, s_pktUnstamped,
				*(g_dllBase + OmgRva::COMM_P1_FLAG), send0, send1, data0, data1, stage,
				CommReset::s_transfers);
		}

		// ADVANCE THE BOARD BY EXACTLY ONE EMULATED FRAME.
		//
		// One module_main call is NOT one emulated frame - the board is paced by the module's
		// clock, so a fast host gets calls that advance nothing and a slow one gets calls that
		// advance two or three frames at once. See VirtualClock.h; the short version is that
		// netplay cannot number frames it does not control, and three board frames sharing one pad
		// is a real divergence rather than a bookkeeping one.
		//
		// With the clock virtual, YAMP owns the pacing: tick it forward in sub-frame steps and stop
		// the moment the ROM's own counter moves. Every peer therefore runs exactly one emulated
		// frame per netplay frame no matter how fast it renders, and a lockstep stall (which simply
		// does not call this) freezes the board rather than letting it accumulate time to catch up
		// on later.
		//
		// Returns module_main's result from the call that produced the frame.
		int StepOneBoardFrame(execute_info_t& info)
		{
			// No virtual clock (symbol unresolved, or a module that never had one): behave exactly
			// as before, one call per host frame, real time still driving the board.
			if (!ModuleClock().Installed())
			{
				AssertTwoBoardGate();
				DriveCabinetRoleChanges();
				DriveCommFirmware();
				return g_moduleMain(sizeof(info), &info);
			}

			const uint32_t counterAddr = RomFrameCounterAddress();
			uint32_t before = 0;
			const bool haveCounter = counterAddr != 0 && ReadEmulatedRam32(counterAddr, before);

			// Without a readable counter there is nothing to stop on. One tick, one call - still an
			// improvement on real time, because the tick is the same on every machine.
			if (!haveCounter)
			{
				ModuleClock().Advance();
				AssertTwoBoardGate();
				DriveCabinetRoleChanges();
				DriveCommFirmware();
				return g_moduleMain(sizeof(info), &info);
			}

			// Bounded so a board that has stopped advancing (a crash, a halted CPU, the boot phases
			// before the ROM runs at all) cannot spin the host forever. SMALL, deliberately: the
			// frozen retries below exist to catch a service the module skipped, which lasts a call
			// or two - while a counter that simply is not moving yet (boot) should hand the host
			// frame back rather than burn hundreds of no-time calls per frame, which distorts the
			// module's per-call bring-up against its now-slower virtual time. A frame that gives
			// up here advanced nothing, which EndFrame's stall test already handles.
			constexpr int kMaxCalls = 16;

			// AND time itself is bounded, which is the two-machine fix (2026-08-04, desync at
			// frame 503): only the first three iterations tick the clock; the rest call with time
			// FROZEN. Without the cap, a call the module internally declines to service (VF2
			// measured ~5% of calls run nothing, and WHICH ones is machine-local) still got a tick,
			// so virtual time accrued across the skipped calls - and the call that finally serviced
			// ran EVERY accrued frame at once. Measured on two machines at netplay frame 503, off
			// a timer-rearm boundary: one peer's call ran rom 82->83 with 7,800 instructions, the
			// other's ran 82->84 with 15,648 - two board frames against one pad, unstoppable
			// mid-call, exactly the fault class the virtual clock exists to prevent.
			//
			// Three ticks is derived, not tuned: 3/120 s of accrued time guarantees AT LEAST one
			// frame is due for any board slower than 40 Hz, and LESS than two are due for any
			// board faster than 80 Hz - Model 2 runs ~57.5-60 Hz, comfortably inside both bounds.
			// A skipped service now just gets retried with no further time flowing, so the module
			// can never owe two frames, whatever it does internally.
			constexpr int kMaxTicks = 3;
			int result = 0;
			for (int i = 0; i < kMaxCalls; ++i)
			{
				if (i < kMaxTicks) ModuleClock().Advance();
				AssertTwoBoardGate();
				DriveCabinetRoleChanges();
				DriveCommFirmware();
				result = g_moduleMain(sizeof(info), &info);

				uint32_t after = 0;
				if (ReadEmulatedRam32(counterAddr, after) && after != before)
				{
					break;
				}
			}
			return result;
		}

		HMODULE LoadDLL()
		{
			const GameDesc& game = CurrentGame();

			{
				DWORD dwSize = GetCurrentDirectoryW(0, nullptr);
				auto buf = std::make_unique<wchar_t[]>(dwSize);
				GetCurrentDirectoryW(dwSize, buf.get());
				gamePath.assign(buf.get());
			}

			// Locate the DLL as a FILE first, so the integrity check runs before LoadLibrary.
			// Kiwami 2 keeps both modules in <install>/m2ftg/ next to rom/ and w64/.
			std::filesystem::path dllFile = gamePath / game.dll_name;
			if (!std::filesystem::is_regular_file(dllFile))
			{
				gamePath.append(game.subdir);
				dllFile = gamePath / game.dll_name;
			}

			if (!std::filesystem::is_regular_file(dllFile))
			{
				const std::wstring str(L"Could not load " + std::wstring(game.dll_name) +
					L"!\n\nMake sure that YAMP.exe is located next to the DLL file, or that its \"" +
					game.subdir + L"\" subdirectory contains it.");
				MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
				return nullptr;
			}

			gGeneral.SetDLLName(WcharToUTF8(game.dll_name));

			if (!Verify::CheckBeforeLoad(gGeneral.GetGameId(), dllFile))
			{
				return nullptr;
			}

			gameDll.reset(LoadLibraryW(dllFile.c_str()));
			if (!gameDll)
			{
				const std::wstring str(L"Could not load " + std::wstring(game.dll_name) +
					L"!\n\nThe file exists but Windows refused to load it.");
				MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
			}
			return gameDll.get();
		}

		void PreInitialize()
		{
			gGeneral.SetDataPath();
			gGeneral.LoadSettings();
		}

		static bool ResolveSymbolsAndPatch(void* dll, const RenderWindow& window) try
		{
			const Imports symbolMap = BuildSymbolMap(dll);

			if (const char* missing = RequiredButMissing(symbolMap); missing[0] != '\0')
			{
				DebugLogFile("[m2ftg::K2] unresolved symbols: %s\n", missing);
				return false;
			}

			const ScopedUnprotect::Section text(static_cast<HMODULE>(dll), ".text");
			const ScopedUnprotect::Section rdata(static_cast<HMODULE>(dll), ".rdata");

			// Lets YAMP's command line reach the module's own option parser, which module_start
			// otherwise calls with an empty argv. Must be before module_start - the parse happens
			// inside it.
			ModuleArgs::Install(dll);

			g_moduleMain = reinterpret_cast<module_func_t>(
				symbolMap.GetSymbol(ImportSymbol::MODULE_MAIN));
			g_gsContext = static_cast<uint8_t*>(symbolMap.GetSymbol(ImportSymbol::GS_CONTEXT_INSTANCE));
			g_slContext = symbolMap.GetSymbol(ImportSymbol::SL_CONTEXT_INSTANCE);
			sl::handle_create_internal = reinterpret_cast<decltype(sl::handle_create_internal)>(
				symbolMap.GetSymbol(ImportSymbol::SL_HANDLE_CREATE));
			// Returns file handle objects to the pool. YAMP's async worker calls it on every
			// completed close; leaving it null faults the worker during teardown.
			sl::file_handle_destroy = reinterpret_cast<decltype(sl::file_handle_destroy)>(
				symbolMap.GetSymbol(ImportSymbol::SL_FILE_HANDLE_DESTROY));
			// This generation's plain rwspinlock stands in for the LJ/YLAD recursive one — see
			// ImportSymbols.h. csl_archive::create_instance calls these on every archive read, from
			// YAMP's own async worker thread.
			sl::archive_lock_wlock = reinterpret_cast<decltype(sl::archive_lock_wlock)>(
				symbolMap.GetSymbol(ImportSymbol::ARCHIVE_LOCK_RLOCK));
			sl::archive_lock_wunlock = reinterpret_cast<decltype(sl::archive_lock_wunlock)>(
				symbolMap.GetSymbol(ImportSymbol::ARCHIVE_LOCK_RUNLOCK));

			// Optional: only the linked-cabinet modules (omg, mr) have a second board at all.
			g_twoBoardGate = static_cast<uint8_t*>(symbolMap.TryGetSymbol(ImportSymbol::TWO_BOARD_GATE));
			g_boardSelect = reinterpret_cast<board_select_t>(
				symbolMap.TryGetSymbol(ImportSymbol::BOARD_SELECT));
			g_dllBase = static_cast<uint8_t*>(dll);

			// Cabinet TEST / SERVICE, so the board's own service menu - and with it the operator's
			// INPUT TEST - is reachable. Same I/O core and the same io[9] bits 2/3 as the Lost
			// Judgment modules; only the way the I/O pointer is found differs, because this
			// generation's refresh loads the board INDEX in its prologue rather than the board
			// pointer, so the global is named outright instead of decoded.
			InstallSystemSwitchesAt(dll, symbolMap.TryGetSymbol(ImportSymbol::I960_IO_REFRESH_CALL),
				symbolMap.TryGetSymbol(ImportSymbol::I960_IO_REFRESH_CALL_B1),
				reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(dll) + OmgRva::IO_STATE_PTR),
				/*multiplexedSystemPort=*/true);

			// Comm-board reset semantics, wrapped around the module's own link transfer - see
			// LinkTransferWithResetSemantics. Gated on the two-board gate as well as the pattern:
			// the same engine ships in the vf2 module, which has no comm board for the registers
			// to describe (measured: the pattern matches only omg across all six module DLLs, but
			// the gate makes that a guarantee rather than an observation). Installed here because
			// this pass holds the .text unprotect the call-site injection needs.
			if (void* xferCall = symbolMap.TryGetSymbol(ImportSymbol::LINK_TRANSFER_CALL);
				xferCall != nullptr && g_twoBoardGate != nullptr)
			{
				Memory::ReadCall(xferCall, g_origLinkTransfer);
				Trampoline* trampoline = Trampoline::MakeTrampoline(dll);
				Memory::InjectHook(xferCall, trampoline->Jump(&LinkTransferWithResetSemantics));
				DebugLogFile("[m2ftg::K2] comm-board reset semantics installed "
					"(call site %p, transfer %p)\n",
					xferCall, reinterpret_cast<void*>(g_origLinkTransfer));
			}

			// Take the module off the host's wall clock. Done here because this is the pass that
			// holds the .text unprotect, and done for local play too: the board advancing with real
			// time rather than with frames is what makes a slow host run two emulated frames in one
			// call, and that is a correctness problem before it is a netplay one.
			//
			// `-von-realclock` puts the module back on QueryPerformanceCounter and back to one
			// module_main call per host frame - i.e. exactly the behaviour before the virtual clock
			// existed. It is the A/B for "did the clock change break this?", which matters because
			// the clock moves the frame loop under everything else the host does. Netplay will
			// desync with it on; that is the point of it being a flag.
			if (wcsstr(GetCommandLineW(), L"-von-realclock") == nullptr)
			{
				ModuleClock().Install(symbolMap.TryGetSymbol(ImportSymbol::WALL_CLOCK));
			}
			else
			{
				DebugLogFile("[m2ftg::K2] -von-realclock: module left on the host wall clock\n");
			}

			// Which board the render draws. Applied per frame from the netplay role rather than
			// once at load from a command line - see SetRenderBoard.
			g_renderBoardSelect =
				static_cast<uint8_t*>(symbolMap.TryGetSymbol(ImportSymbol::RENDER_BOARD_SELECT));

			// The gs context pointer global self-initialises lazily; point it up front.
			auto** ppGsCtx = static_cast<uint8_t**>(symbolMap.GetSymbol(ImportSymbol::GS_CONTEXT_PTR));
			*ppGsCtx = g_gsContext;

			DebugLogFile("[m2ftg::K2] module_main=%p gsCtx=%p (size field 0x%X)\n",
				reinterpret_cast<void*>(g_moduleMain), static_cast<void*>(g_gsContext),
				g_gsContext != nullptr ? *reinterpret_cast<uint32_t*>(g_gsContext + 8) : 0);

			// ---- sl bring-up -----------------------------------------------------------------
			// The module's embedded sl context is CONSTRUCTED but never INITIALISED — verified on a
			// live Kiwami 2, where the host's own sl context has 18 populated qwords in
			// +0x10..+0x100 and this embedded one has exactly one. Handing the module a dead sl
			// context makes every file/handle lookup fail.
			//
			// pxd::sl's initialize() does the head of the context (all at the LJ offsets in this
			// generation), but the handle queue and the file-handle pool live 0x3C0 bytes further
			// down here — see pxd/K2/sl.h — so those come from pxd::K2 instead.
			pxd::K2::set_context(g_slContext);

			// pxd::sl allocates through this hook. Rather than hunt this DLL's own kernel_calloc
			// (its body has diverged too far from the LJ/YLAD builds for pattern transfer — a
			// similarity sweep tops out at noise), YAMP supplies its own zeroing allocator, which
			// mirrors the real host: its sl context's allocator slot at +0x158 points at an object
			// inside YakuzaKiwami2.exe, i.e. the host's allocator, not the module's.
			// CAVEAT: if the module ever frees these buffers through its own allocator, that
			// mismatch would surface at teardown — the first thing to suspect if module_stop faults.
			if (sl::kernel_calloc_internal == nullptr)
			{
				sl::kernel_calloc_internal = [](uint64_t bytes, uint32_t) -> void* {
					void* p = _aligned_malloc(static_cast<size_t>(bytes), 16);
					if (p != nullptr) memset(p, 0, static_cast<size_t>(bytes));
					return p;
				};
			}

			if (pxd::K2::context()->handles.p_handle_buffer == nullptr)
			{
				constexpr uint32_t kHandleCapacity = 0x100000;
				if (const int rc = sl::initialize(pxd::K2::kContextSize); rc != 0)
				{
					DebugLogFile("[m2ftg::K2] sl::initialize FAILED (0x%X)\n", rc);
					return false;
				}
				// NOT sl::handle_initialize: it builds the free queue at LJ's 0x6C0, and this
				// generation's handle_create_internal pops it at sl+0xA80.
				if (!pxd::K2::InitHandles(kHandleCapacity))
				{
					DebugLogFile("[m2ftg::K2] K2::InitHandles FAILED\n");
					return false;
				}
				// The FILE half of sl. Without it the module can only reach resources embedded in
				// the DLL; anything it loads from disk (notably m2ftg/w64/shader_pxd_w64.farc)
				// comes back as a null blob and faults far downstream. The live host's sl context
				// has all of these populated (+0x88 p_file_handle_tbl, +0x90 p_file_access,
				// +0x98 p_file_async_request, +0x118/+0x120 archive access).
				pxd::K2::PatchSl();

				DebugLogFile("[m2ftg::K2] sl initialised: ctx=%p size=0x%X handles=%p access=%p\n",
					static_cast<void*>(pxd::K2::context()),
					pxd::K2::context()->size_of_struct,
					static_cast<void*>(pxd::K2::context()->handles.p_handle_buffer),
					static_cast<void*>(pxd::K2::context()->p_file_access));
			}

			FillSharedSymbols(window);
			return true;
		}
		catch (...)
		{
			const std::wstring str(L"Failed to resolve imports and/or patch " +
				std::wstring(CurrentGame().dll_name) +
				L"!\n\nIt's either not a valid arcade module DLL from Yakuza Kiwami 2, or the game "
				L"has been updated and YAMP is not forward compatible with that new version.");
			MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
			return false;
		}

		static bool GameLoop(RenderWindow& window)
		{
			static execute_info_t execute_info {};
			static int s_frame = 0;
			static int s_lastLogged = -1;

			execute_info.size_of_struct = sizeof(execute_info);
			execute_info.p_device_context = s_deviceContext;
			execute_info.status = 0;
			execute_info.result = 0x80004005;   // the module writes S_OK over this on success
			execute_info.output_texid = 0;
			execute_info.sound_volume =
				static_cast<float>(gGeneral.GetSettings()->m_volumePercent) / 100.0f;

			// NETWORKING IS DELIBERATELY ABSENT FROM THIS HOST. A full VON netplay stack was
			// built here (lockstep rounds, a handshake-as-barrier round start, host-forced HLE
			// sets) and STRIPPED on 2026-08-04 after four two-machine runs in a row failed in
			// four different ways - the retrospective and everything learned is in
			// docs/von-netplay-recon.md. The next attempt starts from that document's
			// "fresh start" notes (one board per machine, the link over the wire), not from
			// code archaeology here.

			static bool s_pauseMenuOpen = false;
			{
				static bool s_escWasDown = false;
				const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
				if (escDown && !s_escWasDown) s_pauseMenuOpen = !s_pauseMenuOpen;
				s_escWasDown = escDown;
			}
			if (s_pauseMenuOpen) execute_info.status |= 1;

			// Input: refresh the shared XInput snapshot, then evaluate each player's bindings via
			// csl_pad — the same Controls page every other m2ftg host uses. This generation embeds
			// the plain csl_pad in execute_info, so the whole struct goes across rather than LJ's
			// 0xE0 prefix.
			Input::PollPads();
			static pxd::csl_pad s_pads[2];
			s_pads[0].set_state(0);
			s_pads[1].set_state(1);
			for (int p = 0; p < 2; p++)
			{
				execute_info.pad[p] = s_pads[p];
				execute_info.pad[p].m_port = static_cast<unsigned int>(p);
				execute_info.pad[p].m_user_id = p;
				execute_info.pad[p].m_is_connected = true;
			}

			// The FIXED module-facing assignment table (slot order A, B, Y, X, LT, LB, RT, RB).
			// Remapping happens host-side in csl_pad::set_state, which routes each player's bound
			// inputs onto the bit carrying the wanted combo, so this must be Input::MODULE_ASSIGN —
			// the table those routes assume. Using the raw template instead shifts every combo
			// button by one slot (the bug that was found and fixed in the YLAD VF2 host).
			for (int p = 0; p < 2; p++)
			{
				for (int i = 0; i < 8; i++) execute_info.assign[p][i] = Input::MODULE_ASSIGN[i];
			}

			// VIRTUAL ON DOES NOT USE assign[0][4] AS A BUTTON ASSIGNMENT. It reads that one byte -
			// execute_info + 0x15E4 - as an INDEX into its own control-mapping table, and that is
			// why no input of any kind worked: not a button, not Start, not the service switch.
			//
			// omg 0x180004BE0 is the per-frame input update. Its first act is
			// `scheme = *(u8*)(execute_info + 0x15E4)` followed by a 0xC0-byte copy out of the table
			// at 0x18012ABB0 into the globals the pad decoder reads; only then does it call the
			// execute_info pad reader (0x180081140). The table holds {host button mask -> Model 2
			// button code} pairs, twelve to an entry.
			//
			// **The table has five entries, 0 to 4.** Entries 0-4 all begin with the same valid
			// pairs (0x10->0x07, 0x20->0x09, 0x40->0x08, 0x80->0x0A); entry 5 onwards is an array of
			// POINTERS belonging to something else. Input::MODULE_ASSIGN[4] is 6 - the P+K+G slot
			// value, correct for the fighting games and two entries off the end here - so Virtual On
			// was decoding every pad through whatever those pointers happen to be.
			//
			// SCHEME 3 IS THE FULL TWIN-STICK MAPPING. Virtual On is a twin-lever cabinet, and the
			// five entries are five ways of driving those levers from a pad. Decoded from the table
			// itself (0x6E is the "unmapped" filler):
			//
			//   entry 0/1  face buttons and D-pad produce COMBO codes (0x70-0x77) - one button
			//              expands into a two-stick gesture. Confirmed against the board's own
			//              input test: Punch read as "right on the left stick + left on the right"
			//              (both inward = crouch), Guard as both outward (jump), Kick as a turbo.
			//   entry 2/4  directions only partly routed; most masks are the filler.
			//   entry 3    every direction DISCRETE - D-pad -> left stick (codes 3/6/4/5), face
			//              buttons -> right stick (codes B/E/C/D), shoulders -> the triggers and
			//              turbos (0x10/0x20/0x40/0x80 -> 7/9/8/A), which every entry shares.
			//
			// So 0 is a beginner scheme that can only ever reach the gestures someone chose to
			// pre-compose, and 3 is the cabinet's actual control set. Choosing 3 means a player can
			// produce every stick position, which is the difference between playing the game and
			// operating a shortcut menu.
			if (gGeneral.GetGameId() == YAMPGeneral::GameId::VON_K2)
			{
				execute_info.assign[0][4] = 3;
			}

			const bool advanceFrame = true;

			// Before the board is stepped, so the link the exchange asks about is this frame's.
			DriveNetSession();

			// Synthetic START, before the coin/start dance so it takes the same path a real press
			// does. Logged, because a run that crashes has to say what it did first.
			if (const int at = AutoStartFrame();
				at > 0 && s_frame >= at && s_frame < at + AUTOSTART_HOLD_FRAMES)
			{
				execute_info.pad[0].m_now |= 0x100;
				if (s_frame == at)
				{
					execute_info.pad[0].m_push |= 0x100;
					net::Logf("[von] AUTOSTART: pressing START on pad 0 at frame %d", s_frame);
				}
			}

			// Dedicated coin binding -> the coin status bit (host->module bit5). Meaningless in
			// freeplay and swallowed while the pause menu is open.
			{
				static bool s_coinWasDown[2] = {};
				for (int p = 0; p < 2; p++)
				{
					const bool down = Input::ActionDown(p, Input::Action_Coin);
					if (down && !s_coinWasDown[p] && !s_isFreeplay && !s_pauseMenuOpen)
					{
						execute_info.status |= 0x20;
					}
					s_coinWasDown[p] = down;
				}
			}

			// The arcade coin/start dance. ONLY meaningful with the freeplay dip switch OFF: with
			// is_freeplay = 1 the module takes START directly and there is no coin to insert, so
			// running the dance would just swallow the player's first press.
			static bool s_startScreen = false;
			static bool s_coinPending = false;
			static bool s_startToggle = false;
			if (!s_isFreeplay && !s_pauseMenuOpen)
			{
				if (s_coinPending && s_startScreen)
				{
					if (!s_startToggle)
					{
						execute_info.pad[0].m_now |= 0x100;
						execute_info.pad[0].m_push |= 0x100;
					}
					s_startToggle = !s_startToggle;
				}
				else
				{
					s_coinPending = false;
					if (s_startScreen && (execute_info.pad[0].m_now & 0x100) != 0)
					{
						execute_info.status |= 0x20;
						execute_info.pad[0].m_now &= ~0x100u;
						execute_info.pad[0].m_push &= ~0x100u;
						s_coinPending = true;
						s_startToggle = false;
					}
				}
			}

			window.BeginFrame();
			window.NewImGuiFrame();
			if (s_pauseMenuOpen && !DrawPauseMenu(window, s_pauseMenuOpen))
			{
				return false;   // Quit picked
			}

			// The two-board gate is a LAUNCH-TIME choice (-von-2board), never a live toggle: an
			// idle second cabinet is a peer the ROM waits for, and flipping the gate on a running
			// board is the transition every crash in the netplay era traced back to. Per-frame
			// reassert because the module's own mode machine zeroes the gate during bring-up (and
			// the per-CALL assert in StepOneBoardFrame covers the bursts - see AssertTwoBoardGate).
			const bool gateOn = WantTwoBoardMode();
			if (g_twoBoardGate != nullptr && gateOn) *g_twoBoardGate = 1;

			// Which cabinet the render draws - a debug flag only (-von-render1).
			SetRenderBoard(WantRenderBoard1() ? 1 : 0);

			// Cabinet service panel: TEST opens the board's own service menu - the operator's input
			// test, which is how you find out which controls the ROM actually sees - and SERVICE is
			// the credit / navigate button beside it. Held lines; the ROM decides what latches.
			{
				bool test = false;
				bool service = false;
				if (!s_pauseMenuOpen)
				{
					for (int p = 0; p < 2; p++)
					{
						test = test || Input::ActionDown(p, Input::Action_Test);
						service = service || Input::ActionDown(p, Input::Action_Service);
					}
				}
				// A role change drives TEST itself - see SoftResetIntoRole. ORed in rather than
				// overriding, so a user holding the real switch is never fought.
				test = test || DriveRoleResetSwitch();
				SetSystemSwitches(test, service);
			}

			// Input-routing probe: hold a pattern on pad[1] and nothing on pad[0]. If player N's
			// input reaches board N, the two boards' I/O bytes have to diverge; if both boards read
			// pad[0], they stay identical no matter what pad[1] does.
			if (WantPadTest())
			{
				execute_info.pad[1].m_now = 0x000F;   // all four directions, unmistakable
				execute_info.pad[1].m_push = 0x000F;
				execute_info.pad[0].m_now = 0;
				execute_info.pad[0].m_push = 0;
			}

			static unsigned int s_lastOutputTexId = 0;
			int funcResult = 0;
			if (advanceFrame)
			{
				// KEEP THE BOARD AT 60 Hz WHILE A LINK IS LIVE, however slowly we present.
				//
				// One board frame per host frame makes game SPEED a function of the display, and
				// against a linked partner that is a competitive advantage rather than a cosmetic
				// difference - the cabinet running at 32 Hz has a pilot that moves and fires at
				// half the rate of the one at 60, and loses for a reason that is not the match.
				// See VirtualClock::BoardFramesDue; normally 1, capped at 4.
				const unsigned int due = ModuleClock().BoardFramesDue(LinkIsLive());
				for (unsigned int i = 0; i < due; ++i)
				{
					funcResult = StepOneBoardFrame(execute_info);
				}
				if (execute_info.output_texid != 0) s_lastOutputTexId = execute_info.output_texid;

				// Per-chunk work-RAM map behind the probe flag: sixteen 0x10000 chunks across the
				// i960 0x500000 window, per board - how a divergence between the two boards (or
				// between two runs) gets localised to a 64 KB chunk.
				if (g_dllBase != nullptr && WantWorkRamMap())
				{
					char line[16 * 9 + 1];
					for (int b = 0; b < 2; ++b)
					{
						int n = 0;
						for (int c = 0; c < 16; ++c)
						{
							n += snprintf(line + n, sizeof(line) - n, "%08X ",
								Hash32(I960At(g_dllBase, b, 0x500000 + c * 0x10000), 0x10000));
						}
						net::Logf("[vonmap] b%d %s", b, line);
					}
				}
			}
			else
			{
				execute_info.output_texid = s_lastOutputTexId;
			}

			const int interesting = execute_info.status | (funcResult << 16);
			if (interesting != s_lastLogged && s_frame < 5000)
			{
				DebugLogFile("[m2ftg::K2] frame=%d status=0x%X result=0x%X texid=%u ret=0x%X\n",
					s_frame, execute_info.status, execute_info.result,
					execute_info.output_texid, funcResult);
				s_lastLogged = interesting;
			}
			// Sample AFTER module_main, so each delta covers exactly one call.
			if (WantFindCounter() && g_dllBase != nullptr)
			{
				FindCtr::Sample(g_dllBase, 0);
				if (s_frame == 1)   FindCtr::SnapGlobals(g_dllBase);
				if (s_frame == 900) { FindCtr::Report(0); FindCtr::ReportGlobals(g_dllBase); }
			}
			if (WantTwoBoardProbe() && (s_frame % 200) == 0)
			{
				LogTwoBoardState(s_frame, execute_info.output_texid);
			}
			// Every host frame, unconditionally: it ages the link's peer timeout as well as
			// logging, and the log half gates itself on the setting.
			LogLinkState(s_frame);
			s_frame++;
			// status bit6 = the attract/insert-coin screen, the state the coin dance needs.
			s_startScreen = (execute_info.status & 0x40) != 0;

			// Display blit. execute_info.output_texid is a 1-BASED index into the FIRST instance
			// table (gs+0x1808, capacity at gs+0x181C) — the same table the render-target setter
			// FUN_180093090 indexes with `(texid - 1)` to resolve a colour target, which is what
			// identifies it as the texture table.
			//
			// From there the D3D11 view lives inside the sbgl sub-object at tex+0x20: the module's
			// own view getter FUN_180099EF0 reads an array of 0x40-byte per-subresource records at
			// sub+0x30 (dimensions at sub+0x14/+0x18). The exact field inside a record is not
			// pinned, so probe COM pointers once with a guarded QueryInterface and cache the hit —
			// the same approach the YLAD VF2 host uses.
			if (execute_info.output_texid != 0)
			{
				static ID3D11ShaderResourceView* s_displaySrv = nullptr;
				static bool s_srvProbed = false;
				if (!s_srvProbed && s_frame >= 60)
				{
					s_srvProbed = true;
					void** tbl = *reinterpret_cast<void***>(g_gsContext + 0x1808);
					uint8_t* tex = tbl != nullptr
						? static_cast<uint8_t*>(tbl[execute_info.output_texid - 1]) : nullptr;
					auto tryQiSrv = [](void* cand) -> ID3D11ShaderResourceView*
					{
						if (cand == nullptr || (reinterpret_cast<uintptr_t>(cand) & 0x7) != 0) return nullptr;
						ID3D11ShaderResourceView* srv = nullptr;
						__try
						{
							IUnknown* unk = static_cast<IUnknown*>(cand);
							if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&srv)))) return srv;
						}
						__except (EXCEPTION_EXECUTE_HANDLER) {}
						return nullptr;
					};
					if (tex != nullptr)
					{
						for (size_t texOff : { size_t(0x20), size_t(0x28) })
						{
							uint8_t* sub = *reinterpret_cast<uint8_t**>(tex + texOff);
							if (sub == nullptr) continue;
							for (size_t subOff = 0; subOff <= 0x40 && s_displaySrv == nullptr; subOff += 8)
								s_displaySrv = tryQiSrv(*reinterpret_cast<void**>(sub + subOff));
							// Then the per-subresource view records at sub+0x30, stride 0x40.
							uint8_t* records = *reinterpret_cast<uint8_t**>(sub + 0x30);
							for (size_t recOff = 0; records != nullptr && recOff < 0x40 &&
								s_displaySrv == nullptr; recOff += 8)
								s_displaySrv = tryQiSrv(*reinterpret_cast<void**>(records + recOff));
							if (s_displaySrv != nullptr)
							{
								DebugLogFile("[m2ftg::K2] display SRV found via tex+0x%zX (srv=%p)\n",
									texOff, static_cast<void*>(s_displaySrv));
								break;
							}
						}
					}
					if (s_displaySrv == nullptr)
					{
						DebugLogFile("[m2ftg::K2] display SRV NOT found (texid=%u tbl=%p tex=%p)\n",
							execute_info.output_texid, static_cast<void*>(tbl), static_cast<void*>(tex));
					}
				}
				if (s_displaySrv != nullptr)
				{
					window.BlitGameFrame(s_displaySrv);
				}
			}

			window.RenderImGui();
			window.EndFrame();
			if (FAILED(window.GetSwapChain()->Present(1, 0))) return false;
			return true;
		}

		void Run(RenderWindow& window)
		{
			const auto module_start = reinterpret_cast<module_func_t>(
				GetProcAddress(gameDll.get(), "module_start"));
			THROW_LAST_ERROR_IF_NULL(module_start);
			const auto module_stop = reinterpret_cast<module_func_t>(
				GetProcAddress(gameDll.get(), "module_stop"));
			THROW_LAST_ERROR_IF_NULL(module_stop);

			if (!ResolveSymbolsAndPatch(gameDll.get(), window))
			{
				DebugLogFile("[m2ftg::K2] ResolveSymbolsAndPatch FAILED\n");
				return;
			}

			Cri criware;

			// module_start's parameter block. It looks at first read as though module_start only
			// touches +0x20 and +0x38, but the rest is consumed by the helper it calls first
			// (FUN_18005E800), which does `*params.module_main = module_main` and hands the sl/ct/gs
			// blocks to their initialize_module functions. Sizes those enforce, read from each:
			//   sl (FUN_1800656D0): module 0x10, context 0xF3C0     — dereferenced unconditionally
			//   ct (FUN_1800A87A0): module 0x10, context 0x30       — dereferenced unconditionally
			//   gs (FUN_180097830): module **0x50**, context 0x202140, and NULL-SAFE (a null block
			//                       makes it fall back to its own embedded context)
			// Note gs's module block is 0x50 here, not the 0x58 of the LJ/YLAD generations.
			struct sl_module_t
			{
				size_t size = sizeof(sl_module_t);
				void* context;
			} sl_module;
			static_assert(sizeof(sl_module_t) == 0x10);
			sl_module.context = g_slContext;

			// ct: nothing in YAMP reads it and the module only size-checks it, so an opaque block
			// of the right size is enough (same as every other host).
			struct ct_context_t
			{
				uint32_t tag_id;
				uint32_t version;
				uint32_t size_of_struct = 0x30;
				std::byte unknown[0x30 - 0x0C] {};
			};
			static_assert(sizeof(ct_context_t) == 0x30);
			static ct_context_t s_ctContext;

			const struct ct_module_t
			{
				size_t size = sizeof(ct_module_t);
				ct_context_t* context = &s_ctContext;
			} ct_module;
			static_assert(sizeof(ct_module_t) == 0x10);

			// gs's module block. Passing NULL here is legal — initialize_module falls back to the
			// embedded context, which is the very one YAMP fills — but it SKIPS
			// FUN_1800A1010(ctx+0x20), and that function is not an initialiser: it READS the
			// shared-symbol block and copies the values into the DLL's own globals
			// (device -> DAT_1803914D8, the gs+0xC0 sub-object -> DAT_1803914E0/E8, slot[3] as a
			// DWORD -> DAT_1803914D4, slot[4] -> PTR_DAT_180171068). Skip it and those globals stay
			// null, which is what produced the null deref in FUN_1800A1880. So pass the block.
			struct gs_module_t
			{
				size_t size = 0x50;
				void* context;
				uint8_t pad[0x50 - 0x10] {};
			} gs_module;
			static_assert(sizeof(gs_module_t) == 0x50);
			gs_module.context = g_gsContext;

			struct params_t
			{
				size_t size = sizeof(params_t);
				const sl_module_t* sl_module = nullptr;   // +0x08
				const gs_module_t* gs_module = nullptr;   // +0x10
				const ct_module_t* ct_module = nullptr;   // +0x18
				const icri* cri_ptr = nullptr;            // +0x20
				const char* root_path = nullptr;          // +0x28
				module_func_t* module_main = nullptr;     // +0x30 — the module writes THROUGH this
				m2ftg_config_t config {};                 // +0x38, 0x100C bytes, copied wholesale
			} params;
			static_assert(offsetof(params_t, config) == 0x38);

			const std::string utf8Path = gamePath.u8string();
			params.sl_module = &sl_module;
			params.gs_module = &gs_module;
			params.ct_module = &ct_module;
			params.cri_ptr = &criware;
			params.root_path = utf8Path.c_str();
			// A real slot for the module to write into. Leaving this null is an immediate
			// null-deref inside FUN_18005E800. The pattern-scanned MODULE_MAIN should agree with
			// whatever lands here — Run() checks that below.
			params.module_main = &g_moduleMainFromParams;

			const auto* settings = gGeneral.GetSettings();
			params.config.kind = CurrentGame().kind;   // 0 = "vf2" in this DLL's own name table
			params.config.difficulty = settings->m_m2Difficulty <= 3
				? static_cast<uint8_t>(settings->m_m2Difficulty) : 1;
			params.config.country = settings->m_m2Country <= 2
				? static_cast<uint8_t>(settings->m_m2Country) : 0;
			params.config.is_freeplay = settings->m_m2Freeplay ? 1 : 0;
			s_isFreeplay = settings->m_m2Freeplay;
			params.config.is_vs_mode = settings->m_m2VersusMode ? 1 : 0;
			// The VF2 module's own two extra switches. Same m2ftg_config_t as every other
			// generation — the live capture decoded this DLL's copied config against it exactly —
			// so the YLAD VF2 settings apply here unchanged.
			params.config.is_vf20 = settings->m_vf2Version20 ? 1 : 0;

			ApplyAspectSetting(window, settings->m_m2Aspect);

			// MUST PRECEDE module_start, and this is the only call that can reach Virtual On's boot
			// path. The per-frame reconcile in the UI draw is a whole module_main behind the first
			// frame of emulation, so hooks 0-3 - the bring-up handshake, the vblank edge, the frame
			// yield and the cabinet-type branch - have all fired before it runs once, and turning
			// any of them off there tests nothing. Legal this early because the handler column is
			// static .data the installer never touches (see HleHooks::Update), so it is already the
			// live table before bring-up has read a byte of it.
			m2ftg::HleHooks::Update();

			// Same window, and for the same reason: the ROM reads the cabinet byte in InitNetwork,
			// which is hundreds of frames before anything in the UI can run.
			ApplyCabinetRole(gameDll.get(), settings->m_vonCabinetRole);

			DebugLogFile("[m2ftg::K2] module_start(size=%zu, root='%s', kind=%u)\n",
				sizeof(params), params.root_path, params.config.kind);
			const int startResult = module_start(sizeof(params), &params);
			// The module renders into a fixed 1024x768 texture whatever its own resolution option
			// selected, so tell the compositor which sub-rect of it actually holds the frame. Read
			// after module_start: that is when the module's parse ran, and a switch on YAMP's command
			// line can differ from the setting.
			{
				uint32_t srcW = 0, srcH = 0;
				if (ModuleArgs::ResolvedRenderSize(srcW, srcH))
				{
					const_cast<RenderWindow&>(window).SetModuleSourceRect(srcW, srcH);
				}
			}
			DebugLogFile("[m2ftg::K2] module_start -> 0x%X, module_main(params)=%p\n",
				startResult, reinterpret_cast<void*>(g_moduleMainFromParams));
			if (g_moduleMainFromParams != nullptr && g_moduleMainFromParams != g_moduleMain)
			{
				// The module has just told us where module_main really is. Trust that over the
				// pattern and say so loudly — a silent disagreement means the pattern needs work.
				DebugLogFile("[m2ftg::K2] WARNING: pattern gave %p but the module returned %p\n",
					reinterpret_cast<void*>(g_moduleMain),
					reinterpret_cast<void*>(g_moduleMainFromParams));
				g_moduleMain = g_moduleMainFromParams;
			}

			// Throw the second-board switch, AFTER module_start: state 0x10 of the module's own
			// mode machine zeroes this byte (along with the board index and the link-enabled
			// gate), so setting it earlier would simply be undone during bring-up.
			//
			// The gate lives in .data, which is already writable - no ScopedUnprotect needed, and
			// the ones taken during symbol resolution are long out of scope by here.
			if (g_twoBoardGate != nullptr && WantTwoBoardMode())
			{
				*g_twoBoardGate = 1;
				DebugLogFile("[m2ftg::K2] two-board mode ENABLED (gate %p = 1)\n",
					static_cast<void*>(g_twoBoardGate));
			}
			else if (WantTwoBoardMode())
			{
				DebugLogFile("[m2ftg::K2] -von-2board asked for, but this module has no two-board gate\n");
			}

			if (startResult == 0 && g_moduleMain != nullptr)
			{
				const uint32_t frameLimit = gGeneral.GetFrameLimit();
				uint32_t framesRun = 0;
				while (!window.IsShuttingDown())
				{
					if (!GameLoop(window)) break;
					if (frameLimit != 0 && ++framesRun >= frameLimit)
					{
						DebugLogFile("[m2ftg::K2] frame limit %u reached, shutting down\n", frameLimit);
						break;
					}

					// PACE THE LOOP TO THE BOARD, not to a fixed 60. Every other host caps here with
					// the "Enable 60 FPS cap" setting; this one never read it, which is why Virtual
					// On has always run uncapped - and why two peers at different frame rates fed
					// the module's real-time pacing wildly different numbers of emulated frames per
					// call. That is the desync, upstream of anything netplay did.
					//
					// The cap setting is deliberately not consulted. With the module's clock now
					// driven by frames, the loop rate IS the game speed, so an uncapped loop does
					// not render more of the same game - it runs Virtual On three times too fast.
					// There is nothing here left to turn off. See VirtualClock::PaceToVirtualTime.
					ModuleClock().PaceToVirtualTime();
				}

				const int stopResult = module_stop(0, nullptr);
				DebugLogFile("[m2ftg::K2] module_stop -> 0x%X\n", stopResult);
			}
		}
	}
}
