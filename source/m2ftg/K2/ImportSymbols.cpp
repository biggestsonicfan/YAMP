#include "ImportSymbols.h"

#include "../../Utils/Patterns.h"
#include "../../Utils/MemoryMgr.h"

#include <string>

using namespace hook::txn;

namespace m2ftg
{
	namespace K2
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

		// Addresses in the comments are for the non-ASLR'd 0x180000000 image of
		// vf2-pxd-w64-gog_retail.dll, for cross-referencing in Ghidra. BOTH the module DLL and
		// YakuzaKiwami2.exe are built DYNAMIC_BASE — verified live, the exe did NOT load at its
		// preferred base — so nothing may assume them at runtime.
		Imports BuildSymbolMap(void* dll)
		{
			using S = ImportSymbol;

			Imports symbols{
				// module_main's own prologue, anchored on the execute_info size gate that makes it
				// unambiguous: `MOV RDI,RDX` then `CMP RCX, 0x16E0`. Confirmed live for VF2:
				// entered with RCX = 0x16E0 and RDX = the execute_info.
				// The wildcarded byte is the RSI spill slot, the ONE difference between the two
				// Kiwami 2 modules (vf2 `[RSP+0x18]`, omg `[RSP+0x20]`); both share the 0x16E0 size,
				// which was read out of each module_stop's own copy of the same gate. Unique in
				// both images.
				{ S::MODULE_MAIN, get_module_pattern(dll,
					"48 89 74 24 ? 57 48 83 EC 20 83 CE FF 48 8B FA 48 81 F9 E0 16 00 00") },    // vf2 18005ebd0, omg 180081900

				// The sl context is constructed by a CRT static-init helper: `SUB RSP,0x28;
				// LEA RCX,[embedded ctx]; CALL ctor`. Three helpers in this DLL share that exact
				// shape, so the pattern reaches back over the preceding loop tail (`JNZ; RET`) to
				// pick the right one. Unique in BOTH Kiwami 2 modules (vf2 0x180001DAD, omg
				// 0x180001F3D); the LEA displacement sits at offset 10.
				{ S::SL_CONTEXT_INSTANCE, immediate(get_module_pattern(dll,
					"75 F1 C3 48 83 EC 28 48 8D 0D ? ? ? ? E8", 10)) },                           // 18017df80

				// handle_create_internal, reached through a call site (the YLAD pattern happens to
				// match this DLL unchanged). VERIFIED rather than assumed — the target FUN_180066710
				// takes (uint* out, ptr, type), zeroes the out-handle, works off the sl context
				// pointer PTR_DAT_180170010, pops the handle free-queue at sl+0xA80 and bumps the
				// per-type counter at sl+0x1BC0+type*4. Call-site patterns can resolve to the wrong
				// function, so never take one on faith.
				{ S::SL_HANDLE_CREATE, immediate(get_module_pattern(dll,
					"E8 ? ? ? ? 8B 08 89 4E 20", 1)) },                                           // 180066710

				// file_handle_destroy. Anchored well past the LJ pattern's `48 85 C9 0F 84 ? ? ? ?
				// 57` prologue and into the body's `LEA RCX,[RCX+0x4BC]` (the per-handle mutex it
				// destroys first), which is what makes it this function rather than any other
				// null-guarded prologue.
				{ S::SL_FILE_HANDLE_DESTROY, get_module_pattern(dll,
					"48 85 C9 0F 84 ? ? ? ? 57 48 83 EC 20 48 8B F9 48 89 5C 24 38 48 81 C1 BC 04 00 00") }, // 180064680

				// The rwspinlock read-lock: spin while the high 16 bits (the writer field) are set,
				// then CAS the low 16 (the reader count) up by one. Matched on its own prologue
				// through the first read of the lock word, so it is anchored on the function itself
				// rather than on a call site. Its partner is the two-instruction release thunk
				// `OR EAX,-1; LOCK XADD [RCX],EAX; RET`, distinctive enough to match directly.
				// Verified pair: FUN_18006C350 (the archive-registry lookup) takes the first at its
				// head and the second on its exit path.
				{ S::ARCHIVE_LOCK_RLOCK, get_module_pattern(dll,
					"48 89 5C 24 20 57 48 83 EC 20 48 8B F9 0F 1F 00 8B 07 33 D2") },             // 1800668f0
				{ S::ARCHIVE_LOCK_RUNLOCK, get_module_pattern(dll,
					"83 C8 FF F0 0F C1 01 C3") },                                                 // 1800669c0

				// gs self-init: `if (ctx == 0) { gs_ptr = &embedded_ctx; }`. Matches both Kiwami 2
				// modules unchanged (vf2 at 0x180097834, omg at 0x1800BA414).
				{ S::GS_CONTEXT_INSTANCE, immediate(get_module_pattern(dll,
					"48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 8)) },                    // 18018ed40
				{ S::GS_CONTEXT_PTR, immediate(get_module_pattern(dll,
					"48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 15)) },
			};

			return symbols;
		}

		const char* RequiredButMissing(const Imports& symbols)
		{
			static std::string missing;
			missing.clear();

			const std::pair<ImportSymbol, const char*> required[] = {
				{ ImportSymbol::MODULE_MAIN, "MODULE_MAIN" },
				{ ImportSymbol::SL_CONTEXT_INSTANCE, "SL_CONTEXT_INSTANCE" },
				{ ImportSymbol::SL_HANDLE_CREATE, "SL_HANDLE_CREATE" },
				{ ImportSymbol::SL_FILE_HANDLE_DESTROY, "SL_FILE_HANDLE_DESTROY" },
				{ ImportSymbol::ARCHIVE_LOCK_RLOCK, "ARCHIVE_LOCK_RLOCK" },
				{ ImportSymbol::ARCHIVE_LOCK_RUNLOCK, "ARCHIVE_LOCK_RUNLOCK" },
				{ ImportSymbol::GS_CONTEXT_INSTANCE, "GS_CONTEXT_INSTANCE" },
				{ ImportSymbol::GS_CONTEXT_PTR, "GS_CONTEXT_PTR" },
			};

			for (const auto& [symbol, name] : required)
			{
				if (symbols.TryGetSymbol(symbol) == nullptr)
				{
					if (!missing.empty()) missing += ", ";
					missing += name;
				}
			}
			return missing.c_str();
		}
	}
}
