#include "K2Host.h"
#include "../../ModuleLoad.h"

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
#include "../Determinism.h"           // RomFrameCounterAddress / ReadEmulatedRam32
#include "../VirtualClock.h"          // the module's timebase, driven by frames not real time
#include "../Debug/HleHooks.h"              // the HLE trap table, disabled by repointing its handlers
#include "../CommBoard/CommBoard.h"   // the Model 2 comm board's DPRAM model, shared with MrLink
#include "../../cabinet/Cabinet.h"    // the shared cabinet front panel (pause/coin/assign/switches)
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

#include "VonBoard.h"

namespace m2ftg
{
	namespace K2
	{
		using namespace pxd;

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
		uint32_t s_cabinetRole = 0;

		// Idempotent and callable at ANY time - which is what makes a live role change possible.
		// Returns false if the site did not look right, so callers can say so rather than assume.
		bool ApplyCabinetRole(void* dll, uint32_t role)
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
		bool DriveRoleResetSwitch()
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
		void DriveCabinetRoleChanges()
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
	}
}
