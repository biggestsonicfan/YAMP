#pragma once

// Symbols YAMP resolves out of a Yakuza Kiwami 2 m2ftg module
// (m2ftg/vf2-pxd-w64-gog_retail.dll, and later omg-pxd-w64-gog_retail.dll).
//
// This is a THIRD pxd generation, older than both the Lost Judgment and the Like a Dragon ones:
// sl context 0xF3C0 and gs context 0x202140, versus 0xF000/0x388A00 (LJ) and 0xF000/0x3820C0
// (YLAD). Only 4 of the 10 YLAD VF2 patterns survive here, so this family gets its own table.
//
// The notable difference from every other generation: **module_main is not exported and is not
// returned through params**. The real host keeps it in its own wrapper object (verified live:
// `call qword ptr [rdi+0x2A40]` from YakuzaKiwami2.exe+0x84B85C) and module_start never writes
// params+0x30. YAMP does not need to replicate that handoff — it pattern-scans module_main like
// any other symbol, which is what MODULE_MAIN below is for.

#include "../../pxd/Imports.h"

namespace m2ftg
{
	namespace K2
	{
		enum class ImportSymbol
		{
			// The per-frame entry, found by its `CMP RCX, 0x16E0` execute_info size gate.
			MODULE_MAIN,

			// The DLL's own embedded sl context (0xF3C0), built by its CRT static init. The host
			// must hand this back in params.sl_module — sl's initialize_module dereferences the
			// block unconditionally, so a null there is an instant crash.
			SL_CONTEXT_INSTANCE,

			// sl::handle_create_internal(&out, ptr, type). This ONE import unblocks the whole file
			// subsystem: PatchSl's objects reach it via file_handle_internal_t::_afterConstruct ->
			// sl::handle_create, and via semaphore_create/thread_create (which are YAMP's own Win32
			// code but still funnel through handle_create).
			SL_HANDLE_CREATE,

			// sl::file_handle_destroy — returns a file handle object to the pool at sl+0x1FC8
			// (tail index +0x1FDC, wrap at +0x1FD0, count +0x1FD4). YAMP's async request worker
			// calls it on every completed close, so a null pointer here is a call through zero on
			// the first one — which is what faulted during teardown.
			SL_FILE_HANDLE_DESTROY,

			// The archive lock pair. This generation predates the recursive rwspinlock the LJ/YLAD
			// hosts import: its archive registry is guarded by a plain **read** lock (waits while
			// the high 16 bits hold a writer, then bumps the low-16 reader count) and released by
			// an atomic decrement. Those are exactly what YAMP's csl_archive::create_instance
			// needs — a lookup plus an add_ref — so they go into sl::archive_lock_wlock/wunlock.
			// Leaving them null is a call through a null pointer on the first archive read, from
			// inside YAMP's own async worker thread.
			ARCHIVE_LOCK_RLOCK,
			ARCHIVE_LOCK_RUNLOCK,

			// The DLL's own embedded gs context (0x202140) and the global that points at it.
			// Both come from gs's self-init path, the same shape as the YLAD generation — this
			// is one of the few patterns that transfers unchanged, and it matches BOTH Kiwami 2
			// modules (vf2 and omg).
			GS_CONTEXT_INSTANCE,
			GS_CONTEXT_PTR,
		};

		using Imports = pxd::ImportsT<ImportSymbol>;

		Imports BuildSymbolMap(void* dll);

		// Comma-separated names of any required symbol that failed to resolve ("" when all present).
		const char* RequiredButMissing(const Imports& symbols);
	}
}
