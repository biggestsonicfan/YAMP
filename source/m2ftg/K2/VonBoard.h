#pragma once

// The Virtual On / Kiwami 2 board internals shared by the split K2 host files (2026-08-09):
// the OmgRva symbol map, the guest-RAM accessor, the -von-* diagnostic flags and the small
// hashes, plus the cross-file state and entry points. Internal to m2ftg/K2 - nothing outside
// the K2 host family includes this.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstddef>
#include <cstdint>

// Self-sufficient on purpose: the flag helpers below read gGeneral/YAMPSettings, and without
// this the header compiled only because every K2 TU happened to include it last.
#include "../../YAMPGeneral.h"

namespace m2ftg
{
	namespace K2
	{
		inline bool WantTwoBoardMode()
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
		inline bool WantTwoBoardProbe()
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
		inline bool WantWorkRamMap()
		{
			static const bool wanted = WantTwoBoardMode()
				|| wcsstr(GetCommandLineW(), L"-von-2probe") != nullptr;
			return wanted;
		}

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
			constexpr size_t COMM_BANK      = CommBoard::BANK;   // the second 16 KB bank

			// THE DPRAM SLOT LAYOUT, read out of the module's own per-frame link transfer
			// (`FUN_18006A310`, the function this file hooks). Per comm block, per bank:
			//
			//     +0x2000  SEND      the board's outgoing packet, written by its i960
			//     +0x2700  RECV[0]   the OTHER board's packet
			//     +0x2E00  RECV[1]   this board's own echo
			//
			// and the transfer does exactly four 0x700 copies to fill them - each one into the bank
			// the destination is NOT currently reading - before toggling bit7 (data ready) and bit0
			// (bank select) on both flag registers. So the slot layout, the double-buffering and the
			// data-ready flag are the MODULE'S protocol, not something YAMP has to reproduce: put a
			// packet in a SEND buffer and the module delivers it, correctly banked, on its own.
			//
			// Only RECV[0] is named: it is the one YAMP reads back to check the module's work.
			// SEND already has its two absolute forms above, and nothing here touches RECV[1].
			constexpr size_t COMM_RECV0     = 0x2700;
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
			// THE REST OF THE PACKET, decoded 2026-08-09 - full field map with per-field i960
			// evidence in docs/von-comm-packet.md. Everything rides the 0x700 window YAMP already
			// carries verbatim, so none of it needs handling here; this is so the next
			// stage-desync or missing-event hunt starts from names instead of raw offsets.
			//   +0x0C          own character id (published by InitWaitChallenger, adopted by
			//                  WaitChallenger into 0x503A9C)
			//   +0x0E/+0x10    the game clock pair - the MASTER runs it, the slave adopts
			//   +0x14/+0x18    time limit / round-count setting (InitGame, from backup RAM)
			//   +0x1A..+0x20   round trio + match counter
			//   +0x22          sound-cue byte (sendBB): one byte per frame to the local sound
			//                  port, mirrored here; the peer EDGE-DETECTS it when its own queue
			//                  is empty - that is how both cabinets' announcers stay in step
			//   +0x28..+0x114  robot state marshal (loose-code writer at i960 0x26980)
			//   +0x514/+0x515  P1/P2 robot ids - comfire adopts them on the non-master, slot
			//                  picked by comm-flag bit0 (the Advertize scan's memo; if char
			//                  select ever misbehaves in linked play, look there first)
			//   +0x516..+0x52C spawn block (position xyz + heading)
			//   +0x530..+0x540 SendNetRobot's ONE-SHOT event block - the sender CLEARS the
			//                  source after a single transmission, which is the ROM-side reason
			//                  the receive path queues packets in order instead of overwriting
			//   +0x542..+0x555 operator-settings mirror (memcpy of backup 0x1D00016, 0x14 bytes,
			//                  every GameMain frame; the SLAVE adopts the master's time/rounds -
			//                  settings authority belongs to the master cabinet)
			//   +0x556         the stamp check (PKT_CHECK above); +0x558.. unused
		}

		// Native address of an i960 global in a given board's work RAM.
		inline uint8_t* I960At(uint8_t* base, int board, uint32_t i960Addr)
		{
			const size_t bias = (board != 0) ? OmgRva::BOARD1_R5 : OmgRva::BOARD0_R5;
			return base + bias + (i960Addr - OmgRva::I960_R5_BASE);
		}

		// "-von-padtest": hold a distinctive button pattern on pad[1] ONLY, so the two boards can be
		// told apart by their inputs. Without it both pads are idle under -frames and the boards are
		// expected to look identical - which would prove nothing either way.
		inline bool WantPadTest()
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
		inline int AutoStartFrame()
		{
			static const int frame = []() {
				const wchar_t* arg = wcsstr(GetCommandLineW(), L"-von-autostart=");
				return arg != nullptr ? _wtoi(arg + wcslen(L"-von-autostart=")) : 0;
			}();
			return frame;
		}
		inline constexpr int AUTOSTART_HOLD_FRAMES = 8;

		// "-von-render1": force drawing board 1 (a two-board debug aid). See SetRenderBoard.
		inline bool WantRenderBoard1()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-render1") != nullptr;
			return wanted;
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
		inline bool WantFindCounter()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-findctr") != nullptr;
			return wanted;
		}
		inline uint32_t NonZeroCount(const uint8_t* p, size_t n)
		{
			uint32_t hits = 0;
			for (size_t i = 0; i < n; ++i) hits += (p[i] != 0);
			return hits;
		}

		// FNV-1a over a sample, so "are these two regions the same" is one comparison.
		inline uint32_t Hash32(const uint8_t* p, size_t n)
		{
			uint32_t h = 2166136261u;
			for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
			return h;
		}


		// ---- Shared state and cross-file entry points (2026-08-09 split of K2Host.cpp into
		// the LJ-family shape: VonRole.cpp, VonLink.cpp, VonProbes.cpp). Definitions live in
		// the file named beside each; everything above is header-inline and per-TU.
		using board_select_t = void(*)(int);
		extern uint8_t* g_dllBase;            // K2Host.cpp
		extern uint8_t* g_twoBoardGate;       // K2Host.cpp
		extern board_select_t g_boardSelect;  // K2Host.cpp
		extern uint32_t s_cabinetRole;        // VonRole.cpp - the APPLIED role, 0 = none
		bool ApplyCabinetRole(void* dll, uint32_t role);   // VonRole.cpp
		bool DriveRoleResetSwitch();                       // VonRole.cpp
		void DriveCabinetRoleChanges();                    // VonRole.cpp
		bool LinkIsLive();                                 // VonLink.cpp
		void DriveCommFirmware();                          // VonLink.cpp
		void DriveNetSession();                            // VonLink.cpp
		void LogLinkState(int frame);                      // VonLink.cpp
		void LinkTransferWithResetSemantics();             // VonLink.cpp
		extern void (*g_origLinkTransfer)();               // VonLink.cpp
		void LogTwoBoardState(int frame, uint32_t texid);  // VonProbes.cpp
		namespace FindCtr
		{
			void Sample(uint8_t* dllBase, int board);
			void SnapGlobals(uint8_t* dllBase);
			void Report(int board);
			void ReportGlobals(uint8_t* dllBase);
		}
	}
}
