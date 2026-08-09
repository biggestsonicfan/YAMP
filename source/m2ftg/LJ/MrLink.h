#pragma once

// Motor Raid's linked-cabinet comm board, over the RPCN session's P2P socket.
//
// Motor Raid's network is the ROM's own ring protocol (docs/mr-comm-packet.md): a 0x1C0-byte
// packet per node per frame through a dual-banked DPRAM window, a firmware status block the
// boot check reads, and a mod-3 alive counter each cabinet watches on its downstream
// neighbour. Nothing here is lockstep - no barrier, no seed, no determinism requirement -
// exactly Virtual On's architecture (K2Host.cpp), with one structural difference:
//
// THE MODULE'S OWN COMM BOARD NEVER RUNS ON THE RETAIL PATH. The mr DLL ships the same
// comm-board emulation as omg (blocks at 0x741E50, transfer/firmware in FUN_18005c030 /
// the 0x52DB0 write handler), but the retail frame driver steps the i960 through a task
// object and the linked-mode driver - the only caller of the transfer - is gated on the
// link flag 0x741CB7, which nothing on this path raises. So where Virtual On stages a
// packet and lets the module deliver it, Motor Raid's host performs the DPRAM exchange
// itself: firmware status bytes, peer packet into RX slot 0, own echo into RX slot 1,
// bit7/bit0 toggled on the flag register - a faithful replica of the module's own
// linked-mode transfer, one board wide.
//
// The role (MASTER CONTROLLER / SLAVE MACHINE) comes from the ROOM, not a setting: the ROM
// derives it from the eeprom-backed game assignment during boot, so the role bytes are
// pinned in guest RAM every frame and the board is REBOOTED into a role change through the
// module's own soft reset - Sega Racing Classic 2's DriveRoomRole model.

#include <cstdint>

#include "../m2ftg.h"

namespace m2ftg
{
	namespace MrLink
	{
		// Wire the link to the loaded module. Only latches for the Motor Raid module build the
		// RVAs below were read from (GameVerify pins it by SHA-256 before it can load, so this
		// is belt and braces, not the gate).
		void Configure(uint8_t* dllBase);

		// Once per host frame, BEFORE module_main: pumps the room role, pins the assignment
		// bytes, and runs the comm-board exchange (send our packet, deliver the peer's).
		// Inert unless Configure latched and a room has assigned a role.
		void PreFrame();

		// A role is applied and the link should hold the board at 60 Hz wall time (a linked
		// pair must run at the same rate - a single-frame handshake state is only safe between
		// boards running in step; see docs/mr-comm-packet.md properties).
		bool LinkActive();

		// The running game can host a cabinet link at all (the Motor Raid module is loaded and
		// recognised) - what the Netplay page asks BEFORE any room exists. LinkActive() above
		// is the after-a-room question.
		bool LinkedCabinetSupported();

		// Linked-cabinet status for the netplay overlay - same questions Virtual On and SRC2
		// answer, read from the ROM's own globals rather than YAMP's intentions.
		struct LinkedCabinet
		{
			uint32_t role;      // 1 = MASTER CONTROLLER, 2 = SLAVE MACHINE
			bool ringUp;        // peer datagrams arriving right now
			uint8_t nodeId;     // as the ROM adopted it from the firmware block
			uint8_t nodes;
			bool checkDone;     // the ROM's own network check completed (_LinkMode non-zero)
		};
		bool GetLinkedCabinet(LinkedCabinet& out);

		// "-mr-iotest": drives a deterministic pattern through pad[0]'s analogue fields (one
		// axis at a time) and logs the module's io block against the ROM's own calibrated
		// `_Volume` bytes - how the pad->ADC channel mapping was measured rather than assumed.
		// Called after the host fills the pads, before module_main. Inert without the flag.
		void IoTest(m2ftg_execute_info_t& info);
	}
}
