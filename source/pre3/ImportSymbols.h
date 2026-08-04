#pragma once

// The symbols YAMP resolves out of a pre3 module DLL. Same shape as ../m2ftg/ImportSymbols.h —
// only the container is shared (pxd::ImportsT) — but a separate enum and a separate pattern
// table, because a symbol that resolves in one family is not necessarily even PRESENT in the
// other (see IB_CREATE in Gaiden/ImportSymbols.cpp).
//
// Deliberately smaller than the m2ftg set: everything there that serves Sonic the Fighters' ROM
// work (HLE traps, the i960 fetch dispatcher, the I/O-refresh call site, the frame-submit and
// live-execute_info globals) has no counterpart here — pre3 emulates a PowerPC 603e, not an
// i960, and none of that machinery has been ported. This is the pxd PLATFORM set plus nothing.

#include "../pxd/Imports.h"

namespace pre3
{
	enum class ImportSymbol
	{
		SL_CONTEXT_INSTANCE,
		GS_CONTEXT_INSTANCE,
		GS_CONTEXT_PTR,
		D3DDEVICE,

		SL_FILE_CREATE,
		SL_FILE_OPEN,
		SL_FILE_READ,
		// file_write is implemented by us, this is not a mistake
		SL_FILE_CLOSE,

		SL_HANDLE_CREATE,
		SL_FILE_HANDLE_DESTROY,

		ARCHIVE_LOCK_WLOCK,
		ARCHIVE_LOCK_WUNLOCK,

		DEVICE_CONTEXT_RESET_STATE_ALL,
		VB_CREATE,

		CDEVICE_CTOR,

		// The sl kernel ALLOCATOR, not the calloc wrapper the m2ftg hosts import. pre3 has no
		// sl::kernel_calloc — nothing in the module calls it, so it was linked out — but the
		// host's own sl::initialize / sl::handle_initialize need it. It is a two-line wrapper
		// over this, so the host reconstructs it instead. See Gaiden/ImportSymbols.cpp.
		SL_KERNEL_ALLOC,

		// OPTIONAL — resolved when present, absent otherwise. See the notes in
		// Gaiden/ImportSymbols.cpp.
		IB_CREATE,
	};

	using Imports = pxd::ImportsT<ImportSymbol>;

	Imports BuildSymbolMap(void* dll);

	// "" when every symbol the sl file layer and the gs bring-up dereference resolved, otherwise
	// a comma-separated list of the ones that did not. The OPTIONAL symbols above are excluded —
	// a build legitimately without them is not a broken build.
	const char* RequiredButMissing(const Imports& symbols);
}
