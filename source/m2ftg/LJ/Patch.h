#pragma once

#include "../../RenderWindow.h"
#include "../../pxd/LJ/sl.h"
#include "../../pxd/LJ/gs.h"
#include "../../pxd/LJ/PatchGs.h"

#include "../ImportSymbols.h" // m2ftg::Imports (pxd::ImportsT + this family's symbol enum)

namespace m2ftg
{
	// The pxd platform layer this host is built on (source/pxd): sl/gs/cgs_device_context,
	// the host cdevice, Imports/ImportSymbol. Shared with the LJ VF5FS host.
	using namespace pxd;
		// (PatchSl lives in ../../pxd/LJ/sl.h and PatchGs/ResetCbvSrvRingCursors in
		// ../../pxd/LJ/PatchGs.h — both shared with every other LJ-era host.)
		void ReinstateLogging(void* dll, const Imports& symbols);
		void InjectTraps(const Imports& symbols);
		// Replaces the DLL's i960 instruction fetch with a region-aware version so code in
		// emulated work RAM (the ROM debug menu's trampoline at 0x59F270) can execute.
		void InstallRamExecFetch(void* dll, const Imports& symbols);

		// Wires the cabinet's TEST and SERVICE switches into the emulated Model 2 I/O board, so
		// the board's own service menu (and with it the arcade input test) is reachable. Install
		// once at patch time; SetSystemSwitches is then the per-frame switch position, and does
		// nothing at all if the install found no hook site.
		void InstallSystemSwitches(void* dll, const Imports& symbols);
		void SetSystemSwitches(bool test, bool service);

		// Corrects the one byte of the module's injected backup-RAM block that holds a raw
		// value where the ROM stores a table index - the service menu's GAME ASSIGNMENTS page
		// hangs the board on it. See the definition for the full chain.
		void FixBackupRamTimeIndex(void* dll);

		void Patch_SysUtil(void* dll, const Imports& symbols);
		void Patch_CsGame(void* dll, const Imports& symbols);
		void Patch_Misc(void* dll, const Imports& symbols);
	}

