#pragma once

// The symbols YAMP resolves out of an m2ftg module DLL (stf/fv/mr from Lost Judgment, vf2 from
// Yakuza: Like a Dragon — the four share one pattern table, in LJ/ImportSymbols.cpp). The enum and
// BuildSymbolMap are per-DLL-family and so live here rather than in the shared pxd layer; only the
// container is shared (pxd::ImportsT, see ../pxd/Imports.h).

#include "../pxd/Imports.h"

namespace m2ftg
{
		enum class ImportSymbol
		{
			SL_CONTEXT_INSTANCE,
			GS_CONTEXT_INSTANCE,
			GS_CONTEXT_PTR,
			D3DDEVICE,

			// Needed for semaphore_create
			SL_KERNEL_CALLOC,
			//MEMSET,
			
			SL_FILE_CREATE,
			SL_FILE_OPEN,
			SL_FILE_READ,
			// file_write is implemented by us, this is not a mistake
			SL_FILE_CLOSE,

			SL_HANDLE_CREATE,
			SL_FILE_HANDLE_DESTROY,
			PRJ_TRAP,

			ARCHIVE_LOCK_WLOCK,
			ARCHIVE_LOCK_WUNLOCK,

			DEVICE_CONTEXT_RESET_STATE_ALL,
			VB_CREATE,
			IB_CREATE,

			TRAP_ALLOC_INSTANCE_TBL,

			// pxd cdevice (cdevice_common::g_pD3DDevice) constructor. We build the host
			// device object ourselves but let the DLL's own ctor initialize its fields
			// (incl. the real cd3d12_mem_allocator resource factory at +0x17b0).
			CDEVICE_CTOR,

			// M2FTGAppModule's per-frame render-system submit (FUN_18003b530 = FUN_18003a1e0 + tail-jmp
			// FUN_18003a540). module_main only RECORDS; in handler mode (installed at init) its inline
			// submit stage is a no-op, and THIS is the real submit — normally driven by the engine's
			// render-system loop, which YAMP doesn't run, so the host must call it each frame.
			STF_FRAME_SUBMIT,
			// The "live execute_info" global (DAT_1801ee4a0). module_main sets it on entry and clears it
			// to 0 on return; STF_FRAME_SUBMIT dereferences it, so the host restores it around the call.
			STF_RENDER_EXECINFO,

			// The i960 CPU core's fetch/decode dispatcher (FUN_1800255F0). Its instruction fetch is
			// hard-wired to the program-ROM host buffer (ctx->codeBase + IP, no memory map), so code
			// running from emulated RAM — which the ROM's own debug menu does via a trampoline at
			// 0x59F270 — reads past the 1MB ROM image and crashes. YAMP replaces it with a
			// region-aware reimplementation (see InstallRamExecFetch).
			I960_FETCH_EXEC,
		};

		using Imports = pxd::ImportsT<ImportSymbol>;

		Imports BuildSymbolMap(void* dll);
	}

