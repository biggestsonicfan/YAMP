#pragma once

// The symbols YAMP resolves out of Yakuza: Like a Dragon's VF5FS module
// (runtime/media/vf5fs/vf5fs-pxd-w64-retail.dll).
//
// This DLL is the same pxd engine build as YLAD's VF2 module — the two were compiled 13 minutes
// apart from the same tree (TimeDateStamp 0x601763D1 vs 0x601766BE) — so every engine function
// below is literally the same code in both, and the table was built by transferring each symbol
// from vf2-pxd-w64-retail.dll and byte-comparing the bodies. See ImportSymbols.cpp for the method
// and the trap it caught. The enum is per-DLL-family, hence its own copy rather than sharing
// m2ftg's or LJ's.
//
// Note this is a DIFFERENT engine generation from Lost Judgment's VF5FS (../LJ): gs context
// 0x3820C0 here vs 0x388A00 there, DX11 vs DX12. The generation that matches is VF2's.

#include "../../pxd/Imports.h"

namespace vf5fs
{
	namespace YLAD
	{
		enum class ImportSymbol
		{
			// ---- engine globals ----
			SL_CONTEXT_INSTANCE,   // the DLL's own embedded sl context (0x180B52340)
			SL_CONTEXT_PTR,        // the global that points at it (0x18056C800)
			GS_CONTEXT_INSTANCE,   // embedded gs context, 0x3820C0 bytes (0x180B61A40)
			GS_CONTEXT_PTR,        // 0x180B61A10

			// ---- sl ----
			SL_KERNEL_CALLOC,
			SL_FILE_OPEN,
			SL_FILE_READ,
			// file_write is implemented by us, this is not a mistake
			SL_FILE_CLOSE,
			SL_HANDLE_CREATE,
			SL_FILE_HANDLE_DESTROY,
			ARCHIVE_LOCK_WLOCK,
			ARCHIVE_LOCK_WUNLOCK,
		};

		using Imports = pxd::ImportsT<ImportSymbol>;

		// Adds every symbol whose pattern resolves.
		Imports BuildSymbolMap(void* dll);

		// The symbols the sl/gs bring-up cannot run without. Returns the ones missing from the map,
		// comma-separated ("" when all present) — Run() reports this instead of crashing.
		const char* RequiredButMissing(const Imports& symbols);
	}
}
