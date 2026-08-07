#pragma once

#include "ImportSymbols.h" // m2ftg::ImportSymbol + the Imports alias

namespace m2ftg
{
	// Wires the cabinet's TEST and SERVICE switches into the emulated Model 2 I/O board, so the
	// board's own service menu (and with it the arcade input test) is reachable.
	//
	// Shared by every m2ftg host: the I/O core is the same in the Lost Judgment modules and in
	// the YLAD Virtua Fighter 2 one, and nothing here is game-specific - the install decodes the
	// I/O board pointer out of the refresh function's own first instruction, so it needs no
	// per-game RVA and survives ASLR. All a host has to supply is the hook site, as
	// ImportSymbol::I960_IO_REFRESH_CALL in its symbol map.
	//
	// Install once at patch time; SetSystemSwitches is then the per-frame switch position, and
	// does nothing at all if the install found no hook site (which is the Motor Raid case).
	void InstallSystemSwitches(void* dll, const Imports& symbols);

	// Same thing for a host whose symbol map is not this enum, and whose refresh does not carry
	// the I/O board pointer in its prologue - the Kiwami 2 generation on both counts. The caller
	// supplies the CALL site to hook and the module global holding the I/O board pointer.
	//
	// That global is re-pointed per board by the bank switch in a linked-cabinet game, which is
	// exactly what is wanted: the refresh runs once per board with that board selected, so hooking
	// it puts the switches on whichever cabinet is being stepped.
	//
	// TEST and SERVICE are on the same bits 2 and 3 as the Lost Judgment modules - Virtual On's
	// refresh puts Start 1 on io[9] bit 4, which is the standard Model 2 system-port layout.
	//
	// `multiplexedSystemPort` when the module serves guest 0x01C00002 as `io[8] & 1 ? io[10] :
	// io[9]` - Virtual On does, and runs with that select bit SET, so the switches have to be
	// written into both banks or the ROM never sees them.
	// `callSiteBoard1` is the second board's refresh in a LINKED-CABINET module, and passing it
	// matters: the switches must reach both boards or the pair deadlocks - one board enters the
	// operator menu, stops driving the link, and the other spins in its network check while the
	// first blocks in `synch` waiting for it. Null for a single-board module.
	void InstallSystemSwitchesAt(void* dll, void* callSite, void* callSiteBoard1,
		uint8_t* const* ioStateGlobal, bool multiplexedSystemPort);

	void SetSystemSwitches(bool test, bool service);
}
