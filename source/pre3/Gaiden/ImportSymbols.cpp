#include "../ImportSymbols.h"
#include "../../pxd/Imports.h"

#include "../../Utils/Patterns.h"
#include "../../Utils/MemoryMgr.h"

#include <string>

using namespace hook::txn;

// Byte patterns for Like a Dragon Gaiden's pre3 module (pre3-pxd-w64-d3d12_retail.dll,
// 2024-03-10, TimeDateStamp 0x65EE7212).
//
// HOW THESE WERE OBTAINED, because it matters for maintaining them:
//
// pre3 is built from the same m2ftg-pxd source tree as the Model 2 modules and links the SAME
// pxd generation as Gaiden's Sonic the Fighters build, so most of these functions are
// byte-identical between the two DLLs apart from rip-relative displacements. Ten of the m2ftg
// table's patterns matched pre3 unchanged and are reproduced verbatim below. The rest split
// into two groups:
//
//  * Patterns m2ftg anchors on a CALL SITE rather than the function body (SL_FILE_OPEN,
//    SL_FILE_READ, SL_FILE_CLOSE, VB_CREATE). Those call sites are Sonic the Fighters' code and
//    do not exist here, so each was re-derived by disassembling the SAME function out of the
//    Gaiden StF DLL, wildcarding every rip-relative displacement and rel32, and searching pre3
//    for the result. Each match below is UNIQUE in pre3's .text.
//
//  * ARCHIVE_LOCK_WLOCK, which was genuinely recompiled. See its note.
//
// Symbols that do NOT appear here at all are documented at the bottom — an absent symbol is a
// finding, not an oversight.

namespace pre3
{
	template<typename T = void>
	static auto get_module_pattern(void* module, std::string_view pattern_string, ptrdiff_t offset = 0)
	{
		return pattern(module, std::move(pattern_string)).get_first<T>(offset);
	}

	static void* immediate(void* addr)
	{
		void* val;
		Memory::ReadOffsetValue(addr, val);
		return val;
	}

	Imports BuildSymbolMap(void* dll)
	{
		using S = ImportSymbol;
		Imports symbols{
			// ---- unchanged from the m2ftg table ------------------------------------------
			// The pxd platform functions whose call sites survived into this module too.
			{ S::SL_CONTEXT_INSTANCE, immediate(get_module_pattern(dll, "48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? 48 83 C4 28 E9 ? ? ? ? 4C 8D 0D ? ? ? ? ", 3)) },
			// Both come out of gs::initialize_module (0x18006B270) — the same function that
			// gates on gs::context_t being 0x3898C0, which is what pins the pxd generation.
			{ S::GS_CONTEXT_INSTANCE, immediate(get_module_pattern(dll, "48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 8)) },
			{ S::GS_CONTEXT_PTR, immediate(get_module_pattern(dll, "48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 15)) },
			{ S::D3DDEVICE, immediate(get_module_pattern(dll, "EB B5 48 89 1F", -0x46)) },
			{ S::SL_FILE_CREATE, immediate(get_module_pattern(dll, "E8 ? ? ? ? 48 8B 56 20", 1)) },
			{ S::SL_HANDLE_CREATE, immediate(get_module_pattern(dll, "E8 ? ? ? ? 41 B8 05 00 00 00 48 8D 4C 24 38 48 8B D7 E8 ? ? ? ? 8B 44 24 38 48 8D 4F 44 48 2B F1 89 47 40", 20)) },
			{ S::SL_FILE_HANDLE_DESTROY, get_module_pattern(dll, "48 85 C9 0F 84 ? ? ? ? 57 ") },
			{ S::DEVICE_CONTEXT_RESET_STATE_ALL, get_module_pattern(dll, "48 8B C4 53 41 54") },
			{ S::CDEVICE_CTOR, get_module_pattern(dll, "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 B8 00 00 00 00 00 00 FF FF 89 39 48 8B D9") },
			// recursive_rwspinlock::wunlock. The branchless form the Gaiden-era rebuild uses,
			// and the one m2ftg pattern of the pair that still matches here.
			{ S::ARCHIVE_LOCK_WUNLOCK, get_module_pattern(dll, "8B 11 8D 42 FF 33 C2 0F B7 C0 33 D0 66 85 D2 75 0A 0F AE F0 C7 01 00 00 00 00 C3 89 11 C3") },

			// ---- re-derived from the function bodies -------------------------------------
			// m2ftg reaches these through Sonic the Fighters' own call sites; pre3 has none of
			// them, so they are matched on the prologue instead. Every wildcard here is a
			// rip-relative displacement or a rel32, nothing else.
			{ S::SL_FILE_OPEN, get_module_pattern(dll, "48 89 5C 24 10 55 56 57 48 81 EC 30 04 00 00 48 8B 05 ? ? ? ? 48 8B D9 4C 8B C2 33 ED 89 29 BA 10 04 00 00 48 8D 4C 24 20 48 8B B0 90 00 00 00") },
			{ S::SL_FILE_READ, get_module_pattern(dll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 41 8B F0 48 8B EA 8B D9 E8 ? ? ? ? 48 8B F8 48 85 C0 0F 84 ? ? ? ? 8B 88 58 04 00 00") },
			{ S::SL_FILE_CLOSE, get_module_pattern(dll, "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 8B D9 BD 05 40 00 80 E8 ? ? ? ? 48 8B F0 48 85 C0 74 76 F6 80 58 04 00 00 01 74 04 33 ED EB 61") },
			{ S::VB_CREATE, get_module_pattern(dll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 45 8B F9 45 8B F0 8B DA 48 8B E9 48 8B 05 ? ? ? ? 48 8B 88 B0 00 00 00") },
			// The sl kernel allocator — see the SL_KERNEL_ALLOC note below. Same derivation as
			// the four above (masked Gaiden StF body, unique match here).
			{ S::SL_KERNEL_ALLOC, get_module_pattern(dll, "48 89 5C 24 08 48 89 6C 24 10 56 57 41 54 41 56 41 57 48 81 EC 00 01 00 00 8B F2 48 8B E9 45 33 FF 41 8B DF 49 BC 00 00 00 00 00 00 FF FF F6 C2 08") },

			// ---- recursive_rwspinlock::wlock ---------------------------------------------
			//
			// Recompiled relative to every build m2ftg knows, and it has a NEAR-TWIN in the same
			// module: the READ lock shares this entire prologue. The two only diverge at the
			// test that decides what "free" means — the writer waits for the whole lock word to
			// be zero (`8B 07 33 DB 85 C0`, i.e. TEST EAX,EAX), the reader masks off the owner
			// field first. So this pattern deliberately runs 88 bytes, far enough to include
			// that test; anything shorter matches BOTH functions and silently picks whichever
			// sits lower in .text.
			//
			// Note also `B8 10 00 00 00` where the m2ftg builds have `B8 14 00 00 00` — the TLS
			// slot index differs, which is why m2ftg's wlock pattern finds nothing here even
			// before the reader/writer ambiguity comes into it.
			{ S::ARCHIVE_LOCK_WLOCK, get_module_pattern(dll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 65 48 8B 04 25 58 00 00 00 48 8B F9 8B 15 ? ? ? ? 48 8B 1C D0 B8 10 00 00 00 80 3C 18 00 75 05 E8 ? ? ? ? B8 08 00 00 00 8B 34 18 85 F6 75 07 E8 ? ? ? ? 8B F0 8B EE C1 E5 10 8B 07 33 DB 85 C0") },

			// ---- M3EInput::read_port ------------------------------------------------------
			//
			// The emulator side rather than the pxd side, so nothing in the m2ftg table relates
			// to it. Found from the M3EInput RTTI (the module carries full MSVC RTTI): it is
			// vtable slot 0 of M3EInput and of every M3ERom subclass, which is also how
			// InstallSystemSwitches finds the entries to redirect.
			//
			// Whole body, no wildcards at all — it is branch-and-return only, with no
			// rip-relative operand anywhere in it, and the pattern below is unique in the image:
			//
			//     port 0x04 -> ~[this+0x28]   SYSTEM  (coin, start, and the service panel)
			//     port 0x08 -> ~[this+0x29]   player 1
			//     port 0x0C -> ~[this+0x2A]   player 2
			//     port 0x3C -> the 8-byte ADC ring at [this+0x2B], indexed by the channel
			//                  register at [this+0x33], which post-increments mod 8
			//     anything else -> 0xFF
			{ S::IO_READ_PORT, get_module_pattern(dll, "4C 8B C1 80 FA 04 74 34 80 FA 08 74 28 80 FA 0C 74 1C 80 FA 3C 75 14") },
		};

		// ---- gs::ib_create: NOT PRESENT IN THIS MODULE ------------------------------------
		//
		// Not "not found yet" — absent. Both builds allocate their gs buffer objects through the
		// same idiom (`mov rax,[gs_ctx]; mov rcx,[rax+0xB0]; mov rax,[rcx]; mov edx,<size>;
		// mov r8d,0x10; call [rax+8]`), so listing every occurrence in each DLL and pairing them
		// up in order gives a 1:1 correspondence between Gaiden StF and pre3 — EXCEPT for StF's
		// two entries at 0x18009B3A3 (ib_create, allocating 0xC0) and 0x18009B520, which have no
		// counterpart here at all. No size-0xC0 allocation of that shape exists anywhere in
		// pre3's .text. Nothing in this module references index-buffer creation, so the linker
		// discarded the COMDAT.
		//
		// The consequence for the host is that gs::primitive_initialize's shared quad/fan index
		// buffers cannot be built the module's own way. That is consistent rather than alarming:
		// a module that never creates an index buffer never draws with one either.
		//
		// If a future pre3 build does reference it, resolve it the same way the four above were
		// (mask a Gaiden StF body, search here) rather than by anchoring on the allocation size.

		// ---- sl::kernel_calloc: ABSENT, and reconstructed instead --------------------------
		//
		// Gone for the same reason ib_create is: linked out because nothing in the module calls
		// it. That was established by enumerating its call sites in the Gaiden StF build — there
		// are exactly THREE, all of them `mov edx,8 / mov ecx,<size> / call`, and all three are
		// Sonic the Fighters' own code allocating its ROM and work buffers. pre3 allocates
		// through its own module-level wrapper (the pointer module_start installs at
		// DLL 0x180189C18) and never goes near sl::kernel_calloc, so the symbol is not there.
		//
		// It cannot simply be skipped the way ib_create can: the HOST's sl::initialize and
		// sl::handle_initialize call it to allocate p_temp_work and the handle buffer, so a null
		// here is a call to address zero during bring-up — which is exactly how this was found.
		//
		// The fix is to rebuild it rather than to substitute a host heap, so the memory still
		// comes from the module's own sl allocator. Its whole body in the StF build is:
		//
		//     rax = kernel_alloc(size, flags);            // SL_KERNEL_ALLOC above
		//     if (rax && (flags & 8)) memset(rax, 0, size);
		//     return rax;
		//
		// which the host reproduces verbatim (Pre3Host.cpp). Note the inner allocator takes the
		// SAME (size, flags) pair and tests bit 3 itself — the wrapper's zero-fill is on top of
		// whatever that does, not a substitute for it.

		return symbols;
	}

	const char* RequiredButMissing(const Imports& symbols)
	{
		// Everything the sl file layer and the gs bring-up dereference. IB_CREATE is deliberately
		// NOT here — see the note above.
		static constexpr struct { ImportSymbol sym; const char* name; } REQUIRED[] = {
			{ ImportSymbol::SL_CONTEXT_INSTANCE,    "SL_CONTEXT_INSTANCE" },
			{ ImportSymbol::GS_CONTEXT_INSTANCE,    "GS_CONTEXT_INSTANCE" },
			{ ImportSymbol::GS_CONTEXT_PTR,         "GS_CONTEXT_PTR" },
			{ ImportSymbol::D3DDEVICE,              "D3DDEVICE" },
			{ ImportSymbol::CDEVICE_CTOR,           "CDEVICE_CTOR" },
			{ ImportSymbol::SL_FILE_CREATE,         "SL_FILE_CREATE" },
			{ ImportSymbol::SL_FILE_OPEN,           "SL_FILE_OPEN" },
			{ ImportSymbol::SL_FILE_READ,           "SL_FILE_READ" },
			{ ImportSymbol::SL_FILE_CLOSE,          "SL_FILE_CLOSE" },
			{ ImportSymbol::SL_HANDLE_CREATE,       "SL_HANDLE_CREATE" },
			{ ImportSymbol::SL_FILE_HANDLE_DESTROY, "SL_FILE_HANDLE_DESTROY" },
			{ ImportSymbol::ARCHIVE_LOCK_WLOCK,     "ARCHIVE_LOCK_WLOCK" },
			{ ImportSymbol::ARCHIVE_LOCK_WUNLOCK,   "ARCHIVE_LOCK_WUNLOCK" },
			{ ImportSymbol::DEVICE_CONTEXT_RESET_STATE_ALL, "DEVICE_CONTEXT_RESET_STATE_ALL" },
			{ ImportSymbol::VB_CREATE,              "VB_CREATE" },
			{ ImportSymbol::SL_KERNEL_ALLOC,        "SL_KERNEL_ALLOC" },
		};

		static std::string missing;
		missing.clear();
		for (const auto& entry : REQUIRED)
		{
			if (symbols.TryGetSymbol(entry.sym) == nullptr)
			{
				if (!missing.empty()) missing += ", ";
				missing += entry.name;
			}
		}
		return missing.c_str();
	}
}
