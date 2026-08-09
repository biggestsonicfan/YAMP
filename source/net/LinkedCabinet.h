#pragma once

#include <cstdint>

namespace net
{
	// The linked-cabinet report, one shape for every board family - Virtual On (m2ftg/K2),
	// Motor Raid (m2ftg/LJ/MrLink) and Sega Racing Classic 2 (pre3/CommBoard) answer the same
	// questions about the same kind of link, and the overlay should not have to care which
	// emulator is running. Until 2026-08-09 this was three identical per-family structs kept in
	// step by comments pointing at each other; it lives in net/ because the transport these
	// links ride (the plugin's link channel) is the one layer all three already share.
	//
	// Every field is the cabinet's OWN answer rather than the host's intent - `checkDone` in
	// particular is the ROM's verdict from its boot network check, not "YAMP thinks the link is
	// up". A host that reported its own hopes would say the ring was fine while the board sat
	// in its check.
	struct LinkedCabinetStatus
	{
		uint32_t role;      // 1 = MASTER, 2 = SLAVE
		bool ringUp;        // the peer's packets are arriving right now
		uint8_t nodeId;     // as the ROM adopted it from the ring
		uint8_t nodes;      // cabinets on the ring, including this one
		bool checkDone;     // the ROM's own network check completed
	};
}
