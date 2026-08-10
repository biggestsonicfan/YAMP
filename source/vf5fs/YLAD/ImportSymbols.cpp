#include "ImportSymbols.h"
#include "../../pxd/PatternHelpers.h"

#include "../../Utils/Patterns.h"
#include "../../Utils/MemoryMgr.h"

#include <string>

using namespace hook::txn;

namespace vf5fs
{
	namespace YLAD
	{
		// Pattern resolution: the shared helpers in pxd/PatternHelpers.h, under this
		// namespace's names so the tables below read unchanged.
		using pxd::get_module_pattern;
		using pxd::immediate;
		using pxd::immediate8;
		using pxd::immediateImm8;

		// ---------------------------------------------------------------------------------------
		// How this table was built
		//
		// YLAD ships two pxd modules — vf2-pxd-w64-retail.dll and this one — built 13 minutes apart
		// from the same tree. The whole platform layer is therefore the SAME CODE in both, so each
		// symbol was transferred rather than hand-reversed: resolve it in the VF2 DLL with the VF2
		// host's pattern, find that function's body here, and build the pattern from THIS DLL's own
		// first bytes with '?' at exactly the positions where the two builds differ (the
		// image-relative operands, and nothing else). Every pattern below was then checked to match
		// exactly ONCE in this DLL and to be anchored at a function entry.
		//
		// THE TRAP THIS CAUGHT — worth repeating because it is silent: 8 of the VF2 host's 10
		// patterns matched here unchanged, but SL_FILE_OPEN's matched UNIQUELY AND WRONG. That
		// pattern ("E8 ? ? ? ? 8B 08 85 C9") anchors on a CALL SITE, and code around a call site is
		// game code that differs per module; here it landed on a neighbouring sl function whose body
		// shares only 7 of 96 bytes with the real file_open. A unique match is NOT a correct match.
		// Every symbol below was confirmed by byte-comparing 96 bytes of its body against the VF2
		// DLL's known-good function: the genuine ones score 85-93/96, differing only in 3-4 byte
		// runs. The real file_open was found by ranking all 10,919 functions in this DLL by body
		// similarity — 85/96 against a runner-up of 19/96.
		//
		// Addresses in the comments are for the non-ASLR'd 0x180000000 image, for cross-referencing
		// in Ghidra; the DLL is DYNAMIC_BASE, so nothing may assume them at runtime.
		// ---------------------------------------------------------------------------------------
		Imports BuildSymbolMap(void* dll)
		{
			using S = ImportSymbol;

			Imports symbols{
				// ---- globals -----------------------------------------------------------------
				// FUN_1802121D0 is sl's self-init accessor: `if (ptr != 0) { ptr = &embedded_ctx; }`.
				// It names both globals in one place, so one pattern yields the pair — the same
				// shape (and the same certainty) as the gs one below.
				{ S::SL_CONTEXT_INSTANCE, immediate(get_module_pattern(dll,
					"48 83 3D ? ? ? ? 00 74 11 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? 33 C0 C3", 13)) },   // 180b52340
				{ S::SL_CONTEXT_PTR, immediate(get_module_pattern(dll,
					"48 83 3D ? ? ? ? 00 74 11 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? 33 C0 C3", 20)) },   // 18056c800
				// gs::initialize_module's null path: `if (module == 0) { gs_ptr = &embedded_ctx; }`.
				// Confirmed by the decompile of FUN_180235900, which also enforces gs ctx 0x3820C0.
				{ S::GS_CONTEXT_INSTANCE, immediate(get_module_pattern(dll,
					"48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 8)) },                        // 180b61a40
				{ S::GS_CONTEXT_PTR, immediate(get_module_pattern(dll,
					"48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 15)) },                       // 180b61a10

				// ---- sl (all body-anchored; see the note above about call-site patterns) ------
				{ S::SL_KERNEL_CALLOC, get_module_pattern(dll,
					"48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B FA 48 8B F1 E8 77 FC FF FF 48 8B D8 48 85 C0 74 16 40") }, // 18020ded0
				{ S::SL_FILE_OPEN, get_module_pattern(dll,
					"48 89 5C 24 10 55 56 57 48 81 EC 30 04 00 00 48 8B 05 ? ? ? ? 48 8B D9 4C 8B C2 33 ED 89 29 BA 10 04 00 00") }, // 180216900
				{ S::SL_FILE_READ, get_module_pattern(dll,
					"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 41 8B F0 48 8B EA 8B D9 E8 ? ? ? ? 48") },     // 180216a20
				{ S::SL_FILE_CLOSE, get_module_pattern(dll,
					"48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 8B D9 BD 05 40 00 80 E8 ? ? ? ? 48 8B F0 48 85 C0 74") },     // 180216730
				{ S::SL_HANDLE_CREATE, get_module_pattern(dll,
					"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 3D ? ? ? 00 48 8B F1") },    // 18021c010
				{ S::SL_FILE_HANDLE_DESTROY, get_module_pattern(dll,
					"48 85 C9 0F 84 D5 00 00 00 57 48 83 EC 20 48 8B F9 48 89 5C 24 38 48 81 C1 BC 04 00 00 48 89 74 24 40") }, // 18021c4a0
				// 34 bytes was not enough here: a sibling lock function shares this prologue, so the
				// pattern runs to 64 bytes (through the `8B 34 01 / 85 F6` archive-slot load).
				{ S::ARCHIVE_LOCK_WLOCK, get_module_pattern(dll,
					"48 89 5C 24 18 55 56 57 48 83 EC 20 8B 15 ? ? ? 00 48 8B F9 65 48 8B 04 25 58 00 00 00 B9 04 00 00 00 "
					"48 8B 04 D0 8B 34 01 85 F6 75 07 E8 9D ? ? ? 8B F0 8B EE C1 E5 10 66 0F 1F 44 00 00") },                   // 1802174a0
				{ S::ARCHIVE_LOCK_WUNLOCK, get_module_pattern(dll,
					"8B 01 89 44 24 08 8B 44 24 08 FF C8 0F B7 D0 8B 44 24 08 25 00 00 FF FF 0B C2 89 44 24 08 8B 44 24 08") }, // 1802175a0
			};

			return symbols;
		}

		const char* RequiredButMissing(const Imports& symbols)
		{
			static std::string missing;
			missing.clear();

			const std::pair<ImportSymbol, const char*> required[] = {
				{ ImportSymbol::SL_CONTEXT_INSTANCE, "SL_CONTEXT_INSTANCE" },
				{ ImportSymbol::SL_CONTEXT_PTR, "SL_CONTEXT_PTR" },
				{ ImportSymbol::GS_CONTEXT_INSTANCE, "GS_CONTEXT_INSTANCE" },
				{ ImportSymbol::GS_CONTEXT_PTR, "GS_CONTEXT_PTR" },
				{ ImportSymbol::SL_KERNEL_CALLOC, "SL_KERNEL_CALLOC" },
				{ ImportSymbol::SL_FILE_OPEN, "SL_FILE_OPEN" },
				{ ImportSymbol::SL_FILE_READ, "SL_FILE_READ" },
				{ ImportSymbol::SL_FILE_CLOSE, "SL_FILE_CLOSE" },
				{ ImportSymbol::SL_HANDLE_CREATE, "SL_HANDLE_CREATE" },
				{ ImportSymbol::SL_FILE_HANDLE_DESTROY, "SL_FILE_HANDLE_DESTROY" },
				{ ImportSymbol::ARCHIVE_LOCK_WLOCK, "ARCHIVE_LOCK_WLOCK" },
				{ ImportSymbol::ARCHIVE_LOCK_WUNLOCK, "ARCHIVE_LOCK_WUNLOCK" },
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
