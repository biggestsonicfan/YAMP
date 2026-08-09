#include "MrLink.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>

#include "../../DebugLog.h"

#include "../m2ftg.h"
#include "../CommBoard/CommBoard.h"
#include "../ModuleBuild.h"
#include "../SystemSwitches.h"
#include "../../net/NetPlugin.h"
#include "../../YAMPGeneral.h"

namespace m2ftg
{
	namespace MrLink
	{
		// RVAs into mr-pxd-w64-d3d12_retail.dll (build LJ_MR), read in the 2026-08-09 reversing
		// pass - docs/mr-comm-packet.md 6 is the map these came from. Safe to hardcode only
		// because GameVerify pins the module by SHA-256 before it is ever loaded.
		namespace MrRva
		{
			// Board 0's comm block: the CommBoard model (two banks, then the register pair the
			// guest reaches at 0x1A14000/0x1A14002 - see ../CommBoard/CommBoard.h). Only WHERE
			// the blocks live is Motor Raid's own.
			constexpr size_t COMM_BLOCK  = 0x741E50;
			constexpr size_t COMM_BLOCK1 = 0x749E58;   // board 1's block (+0x8008)

			// Slot layout inside a bank (guest window 0x1A10000 + offset):
			//   +0x0000 firmware status (byte0 ring-up, byte2 node id, byte3 node count)
			//   +0x2000 own SEND slot, +0x21C0 RX slot 0 (the peer), +0x2380 RX slot 1 (echo)
			// `_clearPacket` (i960 0x4A210) is the slot-size proof: 0x70 word stores = 0x1C0.
			constexpr size_t SLOT_SEND   = 0x2000;
			constexpr size_t SLOT_RX0    = 0x21C0;
			constexpr size_t SLOT_RX1    = 0x2380;
			constexpr size_t PACKET      = 0x1C0;

			// The module's own soft reset (FUN_1800522e0): task/audio cleanup, then the full
			// comm + CPU reset (0x52400). The module calls it from its own reset command paths
			// (0x431D0 / 0x53E70), so it is safe between module_main calls on this thread.
			constexpr size_t SOFT_RESET  = 0x522E0;
			// The soft reset writes one dword through the current-board framebuffer pointer
			// (DAT_180bb6308 + 0x98897). Only the module's own linked-mode render select
			// (FUN_1800529c0) ever sets that pointer; the retail path leaves it null and the
			// reset AVs (measured 2026-08-09, DLL +0x523DC). Board 0's framebuffer is
			// 0x1810B6350 (+ board * 0x988A0), from the same function.
			constexpr size_t FRAME_PTR   = 0xBB6308;
			constexpr size_t FRAME_BASE  = 0x10B6350;

			// The per-frame mode-driver pointer (DAT_180618790): null until the module's boot
			// chain installs the game mode (FUN_180042ad0), which is also the test module_main's
			// own frame virtual makes. MR has no DwGame row, so m2ftg::IsBoardBooted() cannot
			// answer for it - this global is the module's own answer.
			constexpr size_t MODE_DRIVER = 0x618790;

			// Board 0's i960 0x500000 window, from the bank switch's own arm
			// (FUN_1800521a0: work-RAM base = &DAT_180fb6350 - 0x500000 for board 0).
			constexpr size_t RAM5_BASE   = 0xFB6350;
			constexpr uint32_t I960_R5   = 0x500000;

			// ROM globals, from the symbol table the DLL itself embeds (0x18017A700).
			constexpr uint32_t SYM_GAMEASSIGN_LINK = 0x518EFC; // _GAMEASSIGN+0xC: 0 single, 1 master, 2-8 slave, 0xF live
			constexpr uint32_t SYM_EEP_LINK        = 0x51900E; // _EEPData+0x1E: the eeprom-mirror source of the above (& 0xF)
			constexpr uint32_t SYM_LINK_MODE       = 0x519548; // _LinkMode: link_ID once the check adopted the ring
			constexpr uint32_t SYM_NODE_ID         = 0x519552; // _node_ID (byte)
			constexpr uint32_t SYM_TOTAL_NODES     = 0x518EE0; // _total_nodes (byte)
			constexpr size_t   PKT_HEARTBEAT       = 0x0E;     // _mainLoop's per-frame counter in the packet
		}

		static uint8_t* g_base = nullptr;

		// The APPLIED role (what the board was rebooted into), not the wanted one. 0 = no link.
		static uint32_t s_role = 0;

		// The newest peer packet, held resident, and its freshness - the shared DPRAM residency
		// model (CommBoard::LinkHealth; the watchdog rationale is documented there, and
		// docs/mr-comm-packet.md 5 is this ROM's own accounting of it).
		static uint8_t s_peer[MrRva::PACKET];
		static CommBoard::LinkHealth s_health;

		// The wire datagram: a tiny header in front of the raw slot. The sequence number exists
		// because the plugin de-dupes byte-identical link payloads (measured on SRC2), and a
		// cabinet parked on a static screen would otherwise starve its own heartbeat.
		struct WireHdr
		{
			uint32_t magic;    // 'KLRM'
			uint32_t seq;
		};
		constexpr uint32_t WIRE_MAGIC = 0x4D524C4B; // "KLRM" little-endian = "MRLK" on the wire

		static uint8_t* GuestRam(uint32_t i960Addr)
		{
			return g_base + MrRva::RAM5_BASE + (i960Addr - MrRva::I960_R5);
		}

		static bool BoardBooted()
		{
			return *reinterpret_cast<uint64_t*>(g_base + MrRva::MODE_DRIVER) != 0;
		}

		void Configure(uint8_t* dllBase)
		{
			if (CurrentModuleBuild() != build::LJ_MR)
			{
				net::Logf("mr-link: unknown module build 0x%08X - link disabled", CurrentModuleBuild());
				return;
			}
			g_base = dllBase;
		}

		static const char* RoleName(uint32_t role)
		{
			return role == 1 ? "MASTER CONTROLLER" : role == 2 ? "SLAVE MACHINE" : "STAND-ALONE";
		}

		// The role bytes the ROM derives its link_ID from, pinned every frame while a role is
		// applied. Both the derived assignment word and its eeprom-mirror source are written so
		// every consumer agrees: `_CheckMainMode` and the network check read _GAMEASSIGN+0xC
		// directly, `_InitGameSet` re-derives it from the eeprom mirror on any settings pass,
		// and `_eepromUpdate` writes the mirror back to the (emulated) part.
		static void PinRoleBytes()
		{
			const uint32_t assign = (s_role == 1) ? 1u : 2u;
			*reinterpret_cast<uint32_t*>(GuestRam(MrRva::SYM_GAMEASSIGN_LINK)) = assign;
			uint8_t* eep = GuestRam(MrRva::SYM_EEP_LINK);
			*eep = static_cast<uint8_t>((*eep & 0xF0u) | assign);
		}

		// THE ROLE COMES FROM THE ROOM (host = MASTER, guest = SLAVE), and the board is
		// REBOOTED into it: the ROM reads the assignment during its boot chain, long before an
		// RPCN room can exist, so a role decided later needs the boot chain re-run. The
		// module's own soft reset is that mechanism - the same one its service-menu reset
		// command uses - and PinRoleBytes above makes sure the re-run reads the new role.
		static void DriveRoomRole()
		{
			const net::Status status = net::GetStatus();
			if (status.local_player < 0)
			{
				return;   // no room (or no plugin) - nothing decides a role
			}
			const uint32_t wanted = (status.local_player == 0) ? 1u : 2u;
			if (s_role == wanted)
			{
				return;
			}
			if (!BoardBooted())
			{
				return;   // too early to reset a board that has not booted; retried next frame
			}
			s_role = wanted;
			net::Logf("mr-link: room says local player %d - this cabinet is the %s; rebooting the board",
				status.local_player, RoleName(wanted));
			PinRoleBytes();
			uint64_t* framePtr = reinterpret_cast<uint64_t*>(g_base + MrRva::FRAME_PTR);
			if (*framePtr == 0)
			{
				*framePtr = reinterpret_cast<uint64_t>(g_base + MrRva::FRAME_BASE);
			}
			reinterpret_cast<void(*)()>(g_base + MrRva::SOFT_RESET)();
		}

		bool LinkActive()
		{
			return g_base != nullptr && s_role != 0;
		}

		bool LinkedCabinetSupported()
		{
			return g_base != nullptr;
		}

		// THE MODULE'S OWN TRANSFER DELIVERS THE PACKET - Virtual On's architecture exactly,
		// and a correction to this file's first version. FUN_18005c030 is the RETAIL frame
		// driver (its gate 0x741CB7 means "machine running", not "linked mode"; settled when
		// the io probe showed its io refresh rebuilding io[9] every module_main), so the four
		// 0x1C0 copies and the bit7/bit0 flag discipline already run every frame. YAMP's job
		// is what a real ring's firmware would do and nothing more:
		//   - stage the PEER's packet into board 1's SEND buffer, in the bank the transfer is
		//     about to source (board 1's flag bit0, read here one call before module_main runs
		//     it) - the transfer then delivers it to board 0's RX slot 0, correctly banked;
		//   - write the firmware status bytes (ring-up / node id / node count) into both banks
		//     of board 0 - role-correct, overriding the boot response's hardcoded board+1 / 2;
		//   - carry our own SEND slot onto the wire.
		// The flag register is never touched: the module owns it, and driving it from here
		// double-toggles the data-ready bit against the module's own per-frame flip.
		static void ExchangeCommPayload()
		{
			uint8_t* const block = g_base + MrRva::COMM_BLOCK;

			// Nothing before the ROM's own release of the board (`_checkBoard` step: flag,
			// then reset=1, then poll) - the boot-flap rationale is with ReleasedFromReset.
			if (!CommBoard::ReleasedFromReset(block))
			{
				return;
			}

			const uint8_t* const sendCur = block + MrRva::SLOT_SEND + CommBoard::CurrentBank(block);

			// SEND: the slot the ROM wrote (current bank) onto the wire, once per host frame.
			// The header's sequence defeats the plugin's byte-identical de-dupe; the ROM's own
			// heartbeat at +0xE is inside the payload and is what the peer's watchdog reads.
			{
				static uint32_t s_txSeq = 0;
				uint8_t buf[sizeof(WireHdr) + MrRva::PACKET];
				WireHdr hdr{ WIRE_MAGIC, ++s_txSeq };
				memcpy(buf, &hdr, sizeof(hdr));
				memcpy(buf + sizeof(hdr), sendCur, MrRva::PACKET);
				net::LinkSend(buf, sizeof(buf));
			}

			// TAKE: one packet per board frame, in order - the entry handshake and the alive
			// counter are per-frame states, and consuming faster than the ROM reads them is
			// the same discard the plugin's queue exists to prevent (Virtual On, measured).
			{
				uint8_t buf[sizeof(WireHdr) + MrRva::PACKET];
				const unsigned int got = net::LinkTake(buf, sizeof(buf));
				if (got == sizeof(buf))
				{
					WireHdr hdr{};
					memcpy(&hdr, buf, sizeof(hdr));
					if (hdr.magic == WIRE_MAGIC)
					{
						memcpy(s_peer, buf + sizeof(WireHdr), MrRva::PACKET);
						s_health.NotePacket();
					}
				}
				else
				{
					// PreFrame runs once per host frame, which is the rate Age() wants.
					s_health.Age();
				}
			}

			// The firmware's answer, truthfully: ring up only while peer datagrams flow.
			const bool ringUp = net::LinkReady() && s_health.HavePacket() && s_health.Fresh();
			CommBoard::WriteFirmwareStatus(block, ringUp,
				static_cast<uint8_t>(s_role) /* MASTER -> 1, SLAVE -> 2 */, 2,
				"mr-link", "CHECKING NETWORK NOW");

			// Stage the peer's packet into board 1's SEND buffer, the bank the module's
			// transfer sources (`DAT_180751e59 & 1`, read one call before module_main runs
			// it). Residency is free: board 1's i960 never steps (the two-board gate stays
			// down), so nothing overwrites the staged packet between arrivals - a dropped
			// datagram costs nothing, exactly the DPRAM behaviour (Virtual On's model).
			if (s_health.HavePacket())
			{
				uint8_t* const block1 = g_base + MrRva::COMM_BLOCK1;
				memcpy(block1 + MrRva::SLOT_SEND + CommBoard::CurrentBank(block1),
					s_peer, MrRva::PACKET);
			}
		}

		void PreFrame()
		{
			if (g_base == nullptr)
			{
				return;
			}
			DriveRoomRole();
			if (s_role == 0)
			{
				return;
			}
			PinRoleBytes();
			ExchangeCommPayload();
		}

		void IoTest(m2ftg_execute_info_t& info)
		{
			if (g_base == nullptr)
			{
				return;
			}
			static int s_enabled = -1;
			if (s_enabled < 0)
			{
				s_enabled = wcsstr(GetCommandLineW(), L"-mr-iotest") != nullptr;
			}
			if (s_enabled == 0)
			{
				return;
			}

			// One axis moves at a time, so a channel that follows it is attributed rather than
			// guessed: RB pressure ramps first, then LB, then the stick sweeps.
			static uint32_t s_f = 0;
			const uint32_t f = s_f++;
			uint8_t rb = 0, lb = 0;
			float x1 = 0.0f;
			const uint32_t phase = f / 960;
			const uint32_t t = f % 960;
			if (phase == 0)      { rb = static_cast<uint8_t>(t / 4); }
			else if (phase == 1) { lb = static_cast<uint8_t>(t / 4); }
			else                 { x1 = (static_cast<float>(t) / 480.0f) - 1.0f; }
			info.pad[0].m_x1 = x1;
			info.pad[0].m_buttons[4] = lb;   // sl::BUTTON_LB
			info.pad[0].m_buttons[5] = rb;   // sl::BUTTON_RB

			// Past the analogue sweeps: hold TEST through the SystemSwitches hook (which clears
			// io[9] bit2 AFTER the module's refresh - a pre-module_main write never survives:
			// measured, entry io9 was always FF again). The ROM's edge logic wants a press, so
			// hold it 90 frames then release; _TestReq/_MainMode in the log show the ROM taking
			// it.
			uint8_t* const io = g_base + 0xBAE1C0;
			const uint8_t io9Entry = io[9];
			if (f >= 2880)
			{
				SetSystemSwitches(f < 2970, false);
			}

			if (f % 30 == 0)
			{
				// io[0..3]: the module io block the port handlers serve the ADC from
				// (0x1C0001E reads walk it). vol/raw: the ROM's own calibrated values
				// (_Volume: steer/accel/brake) and the raw channel latches (_IoData..).
				// sw: the ROM's main-loop digital latches (518FBC raw, 518FC0+4 the edge byte
				// _CheckMainMode's TEST gate reads) and the mode/req words the gate writes.
				const uint8_t* vol = GuestRam(0x518FA0);
				const uint8_t* raw = GuestRam(0x518FB0);
				const uint8_t* isw = GuestRam(0x519450);   // _NewISW/_OnISW (inverted io[9])
				const uint8_t* swRaw = GuestRam(0x518FBC);
				const uint8_t* swEdge = GuestRam(0x518FC0);
				DebugLogFile("[mr-io] f=%u in x1=%+.2f lb=%02X rb=%02X | io %02X %02X %02X %02X"
					" io9=%02X(entry %02X) | vol H=%02X A=%02X B=%02X"
					" | isw=%02X%02X svc=%02X | sw=%02X %02X %02X %02X e4=%02X eE=%02X"
					" | main=%u test=%u\n",
					f, x1, lb, rb, io[0], io[1], io[2], io[3], io[9], io9Entry,
					vol[0], vol[1], vol[2],
					isw[0], isw[1], *GuestRam(0x519070),
					swRaw[0], swRaw[1], swRaw[2], swRaw[3], swEdge[4], swEdge[0xE],
					*reinterpret_cast<uint32_t*>(GuestRam(0x518F7C)),
					*reinterpret_cast<uint32_t*>(GuestRam(0x519478)));
			}
		}

		bool GetLinkedCabinet(LinkedCabinet& out)
		{
			if (g_base == nullptr || s_role == 0 || !BoardBooted())
			{
				return false;
			}
			out.role = s_role;
			out.ringUp = s_health.HavePacket() && s_health.Fresh();
			out.nodeId = *GuestRam(MrRva::SYM_NODE_ID);
			out.nodes = *GuestRam(MrRva::SYM_TOTAL_NODES);
			out.checkDone = *reinterpret_cast<uint32_t*>(GuestRam(MrRva::SYM_LINK_MODE)) != 0;
			return true;
		}
	}
}
