#pragma once

// The symbols YAMP resolves out of a pre3 module DLL. Same shape as ../m2ftg/ImportSymbols.h —
// only the container is shared (pxd::ImportsT) — but a separate enum and a separate pattern
// table, because a symbol that resolves in one family is not necessarily even PRESENT in the
// other (see IB_CREATE in Gaiden/ImportSymbols.cpp).
//
// Deliberately smaller than the m2ftg set: everything there that serves Sonic the Fighters' ROM
// work (HLE traps, the i960 fetch dispatcher, the frame-submit and live-execute_info globals)
// has no counterpart here — pre3 emulates a PowerPC 603e, not an i960, and none of that
// machinery has been ported. This is the pxd PLATFORM set plus the one emulator function the
// cabinet's TEST / SERVICE switches need.

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

		// ---- OPTIONAL — resolved when present, absent otherwise --------------------------
		// Neither is in RequiredButMissing: a build without them still boots, it just loses the
		// feature that hangs off it. See the notes in Gaiden/ImportSymbols.cpp.

		// M3EInput::read_port — the emulated Model 3 board's I/O read accessor, and the only
		// reader of its JAMMA register block. Wanted for the cabinet TEST / SERVICE switches,
		// which the module itself never drives. See SystemSwitches.h.
		IO_READ_PORT,

		IB_CREATE,

		// THE EMULATOR OBJECT ITSELF - `TaskM3E`, the static instance the frame step and the
		// savestate pump take as `this`, and the root of every board reach in Determinism.cpp and
		// CommBoard.cpp. Both addressed it by hardcoded RVA until 2026-08-07.
		//
		// Worth importing where the struct offsets under it are not, and the difference is the
		// point: an offset INSIDE a structure has no code to anchor a pattern on, but the ADDRESS
		// of the structure is referenced by code and can be found exactly the way
		// SL_CONTEXT_INSTANCE and D3DDEVICE already are. A stale RVA does not fail loudly - it
		// reads plausible qwords out of whatever moved into its place, which presents as "the
		// board never booted" rather than as a bad address. That mistake is already recorded in
		// Determinism.h, made against M2FTGAppModule.
		MACHINE_OBJECT,

		// The module's `CXComm` vtable - the emulated Model 3 network board's. CommBoard compares
		// the object at rom+0x588 against it before believing any field, so this is the
		// known-fixed value that whole self-check rests on; leaving it an RVA made the check only
		// as good as the constant it validated against.
		CXCOMM_VTABLE,
	};

	using Imports = pxd::ImportsT<ImportSymbol>;

	Imports BuildSymbolMap(void* dll);

	// "" when every symbol the sl file layer and the gs bring-up dereference resolved, otherwise
	// a comma-separated list of the ones that did not. The OPTIONAL symbols above are excluded —
	// a build legitimately without them is not a broken build.
	const char* RequiredButMissing(const Imports& symbols);
}
