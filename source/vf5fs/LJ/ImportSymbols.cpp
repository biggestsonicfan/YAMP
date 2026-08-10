#include "ImportSymbols.h"
#include "../../pxd/PatternHelpers.h"

#include "../../Utils/Patterns.h"
#include "../../Utils/MemoryMgr.h"

#include <string>

using namespace hook::txn;

namespace vf5fs
{
	namespace LJ
	{
		// Pattern resolution: the shared helpers in pxd/PatternHelpers.h, under this
		// namespace's names so the tables below read unchanged.
		using pxd::get_module_pattern;
		using pxd::immediate;
		using pxd::immediate8;
		using pxd::immediateImm8;

		// ---------------------------------------------------------------------------------------
		// How this table was built, and why the function patterns look the way they do
		//
		// This DLL is the same pxd engine build as Lost Judgment's m2ftg modules, so every engine
		// function YAMP needs exists here as the SAME CODE. Rather than hand-reverse each one, they
		// were transferred: resolve the symbol in stf-pxd-w64-d3d12_retail.dll with m2ftg's pattern,
		// then locate that function's body in this DLL. The pattern below is the function's own first
		// bytes with '?' wildcards at exactly the positions where the two builds differ — those are
		// the image-relative operands (call rel32, RIP-relative displacements) and nothing else. Each
		// pattern was checked to match exactly ONCE in this DLL and to be anchored at the function
		// entry, and each function body was byte-compared over 96 bytes to confirm identity.
		//
		// This matters: m2ftg's own patterns mostly match a CALL SITE, and the code around a call
		// site is game code that differs per module. Applying them here silently mis-resolved
		// SL_FILE_OPEN to sl::file_create (both are called from similar-looking code).
		//
		// Addresses in the comments are for the non-ASLR'd 0x180000000 image, for cross-referencing
		// in Ghidra; the DLL is DYNAMIC_BASE, so nothing may assume them at runtime.
		// ---------------------------------------------------------------------------------------
		Imports BuildSymbolMap(void* dll)
		{
			using S = ImportSymbol;

			Imports symbols{
				// ---- globals -----------------------------------------------------------------
				// Each independently confirmed against the DLL's own code:
				//   SL_CONTEXT_INSTANCE: FUN_18020AF10 does `PTR_DAT_180557B88 = &DAT_180B82B80`,
				//     i.e. it points the sl-context pointer global at this static instance.
				//   GS_CONTEXT_*: gs::initialize_module (0x18022AEB0) does
				//     `lea rax,[0x180B92140]; mov [0x180B92100],rax`.
				//   D3DDEVICE: FUN_18022CE10 (called from the gs init chain) is literally
				//     `PTR_DAT_180557D58 = *param_1` — that global is cdevice_common::g_pD3DDevice.
				{ S::SL_CONTEXT_INSTANCE, immediate(get_module_pattern(dll, "48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? 48 83 C4 28 E9 ? ? ? ? 4C 8D 0D ? ? ? ? ", 3)) }, // 0x180B82B80
				{ S::GS_CONTEXT_INSTANCE, immediate(get_module_pattern(dll, "48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 8)) },  // 0x180B92140
				{ S::GS_CONTEXT_PTR, immediate(get_module_pattern(dll, "48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 15)) },      // 0x180B92100
				{ S::D3DDEVICE, immediate(get_module_pattern(dll, "EB B5 48 89 1F", -0x46)) },                                          // 0x180557D58

				// ---- sl file layer -----------------------------------------------------------
				{ S::SL_FILE_CREATE, get_module_pattern(dll, "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 30 04 00 00 48 8B D9 C7 01 00 00 00 00 4C 8B C2 48 8D 4C 24 20 BA 10 04 00 00") },                    // 0x180211870
				{ S::SL_FILE_OPEN, get_module_pattern(dll, "48 89 5C 24 10 55 56 57 48 81 EC 30 04 00 00 48 8B 05 ? ? ? 00 48 8B D9 4C 8B C2 33 ED 89 29 BA 10 04 00 00 48 8D 4C") },                          // 0x180211990
				{ S::SL_FILE_READ, get_module_pattern(dll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 41 8B F0 48 8B EA 8B D9 E8 ? ? ? ? 48 8B F8 48 85 C0 0F") },                           // 0x180211AB0
				{ S::SL_FILE_CLOSE, get_module_pattern(dll, "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 8B D9 BD 05 40 00 80 E8 ? ? ? ? 48 8B F0 48 85 C0 74 7A 8B 80 58 04 00") },                          // 0x1802117C0
				{ S::SL_HANDLE_CREATE, get_module_pattern(dll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 3D ? ? ? 00 48 8B F1 45 33 FF 45 8B F0") },                      // 0x180217210
				{ S::SL_FILE_HANDLE_DESTROY, get_module_pattern(dll, "48 85 C9 0F 84 D8 00 00 00 57 48 83 EC 20 48 8B F9 48 89 5C 24 30 48 81 C1 BC 04 00 00 48 89 74 24 38 E8 ? ? ? ? 33") },                 // 0x1802176B0
				{ S::SL_KERNEL_CALLOC, get_module_pattern(dll, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B FA 48 8B F1 E8 67 FC FF FF 48 8B D8 48 85 C0 74 16 40 F6 C7 08 74 10 4C") },                   // 0x180206E10

				// ---- archive spinlock --------------------------------------------------------
				// wlock and wunlock are near-identical to each other and to the read-lock variants,
				// so wlock's pattern has to run deep enough into the body to be unique.
				{ S::ARCHIVE_LOCK_WLOCK, get_module_pattern(dll, "48 89 5C 24 18 55 56 57 48 83 EC 20 65 48 8B 04 25 58 00 00 00 48 8B F9 8B 15 ? ? ? 00 48 8B 1C D0 B8 14 00 00 00 80 3C 18 00 75 05 E8 ? ? 04 00 B8 04 00 00 00 8B 34 18 85 F6 75 07 E8 ? ? ? ? 8B F0 8B EE C1 E5 10") }, // 0x1802126C0
				{ S::ARCHIVE_LOCK_WUNLOCK, get_module_pattern(dll, "8B 01 89 44 24 08 8B 44 24 08 FF C8 0F B7 D0 8B 44 24 08 25 00 00 FF FF 0B C2 89 44 24 08 8B 44 24 08 66 85 C0 75 0A 0F") },               // 0x1802127C0

				// ---- gs ----------------------------------------------------------------------
				// vb_create and ib_create share a prologue; the wildcards fall where they differ
				// (45 8B F9 45 8B F0 vs 4D 8B F9 41 8B E8) so each pattern still resolves uniquely.
				{ S::DEVICE_CONTEXT_RESET_STATE_ALL, get_module_pattern(dll, "48 8B C4 53 41 54 41 55 48 81 EC 80 00 00 00 81 89 90 34 01 00 00 00 00 E0 45 33 E4 48 83 09 03 48 8B D9 C7 40 0C 00 00") },     // 0x180231240
				{ S::VB_CREATE, get_module_pattern(dll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 45 8B F9 45 8B F0 8B DA 48 8B E9 48 8B 05") },                                // 0x180236FD0
				{ S::IB_CREATE, get_module_pattern(dll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 30 4D 8B F9 41 8B E8 8B DA 44 8B F1 48 8B 05") },                                // 0x18023A2B0

				// ---- host cdevice ------------------------------------------------------------
				{ S::CDEVICE_CTOR, get_module_pattern(dll, "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 B8 00 00 00 00 00 00 FF FF 89 39 48 8B D9 48 89 79 08 33 D2 48 89 79 10 41 B8 00") },                       // 0x18022C880

				// ---- module glue (also a sanity check on this host's structure sizes) --------
				// These are the size gates the module itself enforces: module_main `cmp rcx, 0x690`
				// and the three initialize_module prologues checking sl 0xF000 / gs 0x388A00 / ct 0x30.
				{ S::MODULE_MAIN_SIZE_GATE, get_module_pattern(dll, "48 81 F9 90 06 00 00 0F 85") },                                     // 0x1801EDCCF
				{ S::SL_INITIALIZE_MODULE, get_module_pattern(dll, "48 83 39 10 75 ? 48 8B 41 08 81 78 08 00 F0 00 00") },               // 0x18020AF40
				{ S::GS_INITIALIZE_MODULE, get_module_pattern(dll, "48 83 39 58 75 ? 48 8B 49 08 81 79 08 00 8A 38 00") },               // 0x18022AEB0
				{ S::CT_INITIALIZE_MODULE, get_module_pattern(dll, "48 83 39 10 75 ? 48 8B 41 08 83 78 08 30") },                        // 0x180225620

				{ S::TRAP_ALLOC_INSTANCE_TBL, immediate8(get_module_pattern(dll, "73 ? 4C 8B 41 08", 1)) },                              // 0x18023AAAC
			};

			// PRJ_TRAP (the engine's trap/log entry point) is deliberately absent: the transfer found
			// no unique body match for it in this DLL, and its only user is m2ftg's optional trap
			// injection, which this host does not install. Do not "find" it with a short pattern —
			// a 16-byte candidate looked unique but was a generic variadic shadow-store fragment
			// shared by many functions.

			return symbols;
		}

		const char* RequiredButMissing(const Imports& symbols)
		{
			// Everything the sl file layer and the gs bring-up dereference.
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
				{ ImportSymbol::SL_KERNEL_CALLOC,       "SL_KERNEL_CALLOC" },
				{ ImportSymbol::ARCHIVE_LOCK_WLOCK,     "ARCHIVE_LOCK_WLOCK" },
				{ ImportSymbol::ARCHIVE_LOCK_WUNLOCK,   "ARCHIVE_LOCK_WUNLOCK" },
				{ ImportSymbol::DEVICE_CONTEXT_RESET_STATE_ALL, "DEVICE_CONTEXT_RESET_STATE_ALL" },
				{ ImportSymbol::VB_CREATE,              "VB_CREATE" },
				{ ImportSymbol::IB_CREATE,              "IB_CREATE" },
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
}
