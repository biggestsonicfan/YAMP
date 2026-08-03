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
	void SetSystemSwitches(bool test, bool service);
}
