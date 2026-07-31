#pragma once

// The symbols YAMP resolves out of Lost Judgment's VF5FS module
// (runtime/media/vf5fs/vf5fs-pxd-w64-d3d12_retail.dll).
//
// This DLL is the same pxd engine build as LJ's m2ftg modules, so every engine function below is the
// same code in both and was located by transferring it from stf-pxd-w64-d3d12_retail.dll and
// byte-comparing the bodies (96 bytes identical apart from image-relative operands). See the long
// comment in ImportSymbols.cpp for the method and its one trap. The enum is per-DLL-family, hence
// its own copy rather than sharing m2ftg's.

#include "../../pxd/Imports.h"

namespace vf5fs
{
	namespace LJ
	{
		enum class ImportSymbol
		{
			// ---- engine globals ----
			SL_CONTEXT_INSTANCE,
			GS_CONTEXT_INSTANCE,
			GS_CONTEXT_PTR,
			D3DDEVICE,                      // cdevice_common::g_pD3DDevice (holds the HOST cdevice)
			CDEVICE_CTOR,                   // pxd cdevice ctor, called on YAMP's own buffer

			// ---- sl ----
			SL_KERNEL_CALLOC,
			SL_FILE_CREATE,
			SL_FILE_OPEN,
			SL_FILE_READ,
			// file_write is implemented by us, this is not a mistake
			SL_FILE_CLOSE,
			SL_HANDLE_CREATE,
			SL_FILE_HANDLE_DESTROY,
			ARCHIVE_LOCK_WLOCK,
			ARCHIVE_LOCK_WUNLOCK,

			// ---- gs ----
			DEVICE_CONTEXT_RESET_STATE_ALL,
			VB_CREATE,
			IB_CREATE,

			// ---- module glue: the size gates the module enforces, which double as a check that
			// this host builds the structures the module expects ----
			MODULE_MAIN_SIZE_GATE,          // `cmp rcx, 0x690` in module_main — the execute_info size
			SL_INITIALIZE_MODULE,           // checks sl_module 0x10 / sl context 0xF000
			GS_INITIALIZE_MODULE,           // checks gs_module 0x58 / gs context 0x388A00
			CT_INITIALIZE_MODULE,           // checks ct_module 0x10 / ct context 0x30

			TRAP_ALLOC_INSTANCE_TBL,

			// Present in the m2ftg table but NOT resolved for this DLL, and not needed: its only
			// user is m2ftg's optional trap injection. See ImportSymbols.cpp.
			PRJ_TRAP,
		};

		using Imports = pxd::ImportsT<ImportSymbol>;

		// Adds every symbol whose pattern resolves. PRJ_TRAP is never added, so read that one with
		// TryGetSymbol if a future host wants it.
		Imports BuildSymbolMap(void* dll);

		// The symbols the sl/gs bring-up cannot run without. Returns the ones missing from the map,
		// comma-separated ("" when all present) — Run() reports this instead of crashing.
		const char* RequiredButMissing(const Imports& symbols);
	}
}
