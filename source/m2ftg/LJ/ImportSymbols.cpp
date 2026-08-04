#include "../ImportSymbols.h"
#include "../../pxd/Imports.h"

#include "../../Utils/Patterns.h"
#include "../../Utils/MemoryMgr.h"

using namespace hook::txn;

namespace m2ftg
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

		static void* immediate8(void* addr)
		{
			intptr_t srcAddr = (intptr_t)addr;
			intptr_t dstAddr = srcAddr + 1 + *(int8_t*)srcAddr;
			return reinterpret_cast<void*>(dstAddr);
		}

		Imports BuildSymbolMap(void* dll)
		{
			using S = ImportSymbol;
			Imports symbols{

				//get_module_pattern - if used by itself, returns the address offset
				//if used with immediate, it returns an instance use of the offset which resolves
				/** SONIC THE FIGHTERS **/
				{ S::SL_CONTEXT_INSTANCE, immediate(get_module_pattern(dll, "48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? 48 83 C4 28 E9 ? ? ? ? 4C 8D 0D ? ? ? ? ", 3)) }, //180001977
				//{ S::GS_CONTEXT_INSTANCE, immediate(get_module_pattern(dll, "48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? 48 83 C4 28 E9 ? ? ? ? C5 F9 EF C0", 3)) }, //180001B647
				//{ S::GS_CONTEXT_PTR, immediate(get_module_pattern(dll, "48 8B 05 ? ? ? ? 48 8B 88 B0 00 00 00 48 8B 01 FF 50 18 48 8B 74 24 60", 3)) }, //
				{ S::GS_CONTEXT_INSTANCE, immediate(get_module_pattern(dll,	"48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 8)) },
				{ S::GS_CONTEXT_PTR, immediate(get_module_pattern(dll, "48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 15)) },
				//{ S::GS_CONTEXT_PTR, immediate(get_module_pattern(dll, "48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? 48 83 C4 28 E9 ? ? ? ? C5 F9 EF C0", 3)) }, //1802358E3
				//{ S::GS_CONTEXT_PTR, immediate(get_module_pattern(dll, "46 8D 34 06", 7)) }, //18009809A
				//{ S::GS_CONTEXT_PTR, immediate(get_module_pattern(dll, "75 17 48 89 0D ? ? ? ?", 5)) }, //180093015
				{ S::D3DDEVICE, immediate(get_module_pattern(dll, "EB B5 48 89 1F", -0x46)) }, //180086F56
				{ S::SL_KERNEL_CALLOC, get_module_pattern(dll, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B FA 48 8B F1 E8 ? ? ? ? 48 8B D8 48 85 C0 74 ? 40 F6 C7 08") }, //180065E40
				//{ S::MEMSET, immediate(get_module_pattern(dll,"48 03 C8 E8 ? ? ? ? C5 F8 10 44 24 20", 4)) },
				{ S::SL_FILE_CREATE, immediate(get_module_pattern(dll, "E8 ? ? ? ? 48 8B 56 20", 1)) }, //1800A2A15
				{ S::SL_FILE_OPEN, immediate(get_module_pattern(dll, "E8 ? ? ? ? 8B 08 85 C9", 1)) }, //180024404
				{ S::SL_FILE_READ, immediate(get_module_pattern(dll, "49 89 46 18", 5)) }, //18002451E
				{ S::SL_FILE_CLOSE, immediate(get_module_pattern(dll, "E8 ? ? ? ? 4E 89 7C 23 ?", 1)) }, //180024652
				//{ S::SL_HANDLE_CREATE, immediate(get_module_pattern(dll, "E8 ? ? ? ? 8B 08 89 4E 20", 1)) }, //1800734AB
				{ S::SL_HANDLE_CREATE, immediate(get_module_pattern(dll, "E8 ? ? ? ? 41 B8 05 00 00 00 48 8D 4C 24 38 48 8B D7 E8 ? ? ? ? 8B 44 24 38 48 8D 4F 44 48 2B F1 89 47 40", 20)) },
				{ S::SL_FILE_HANDLE_DESTROY, get_module_pattern(dll, "48 85 C9 0F 84 ? ? ? ? 57 ") }, //18006BBF0
				{ S::PRJ_TRAP, immediate(get_module_pattern(dll, "48 8D 4C 24 ? E8 ? ? ? ? 90 48 89 3D ? ? ? ? ", 6)) }, //180039BE5
				// (archive spinlock pair — resolved below, it is the one place the two module
				// builds needed different patterns)
				{ S::DEVICE_CONTEXT_RESET_STATE_ALL, get_module_pattern(dll, "48 8B C4 53 41 54") }, //18008A7A0 - pxd::cgs_device_context::reset_state_all
				{ S::VB_CREATE, immediate(get_module_pattern(dll, "E8 ? ? ? ? 4C 8B B4 24 ? ? ? ? 8D 7E 20 ", 1)) }, //18005D7FC
				{ S::IB_CREATE,immediate(get_module_pattern(dll, "E8 ? ? ? ? 49 89 87 ? ? ? ?", 1)) }, //18005D586
				//{ S::SHIFT_NEXT_MODE, immediate(get_module_pattern(dll, "E8 ? ? ? ? 33 C9 83 7B 58 01", 1)) }, //1801BC669
				//{ S::SHIFT_NEXT_MODE_SUB, immediate(get_module_pattern(dll, "E8 ? ? ? ? EB 0E 84 C0", 1)) }, //1801C3AB4
				{ S::TRAP_ALLOC_INSTANCE_TBL, immediate8(get_module_pattern(dll, "73 ? 4C 8B 41 08", 1)) }, //180010812
				{ S::CDEVICE_CTOR, get_module_pattern(dll, "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 B8 00 00 00 00 00 00 FF FF 89 39 48 8B D9") }, //18008D5F0 pxd cdevice ctor
				// FUN_18003b530 (M2FTGAppModule per-frame submit): SUB RSP,0x28 / CALL / ADD RSP,0x28 / JMP.
				{ S::STF_FRAME_SUBMIT, get_module_pattern(dll, "48 83 EC 28 E8 ? ? ? ? 48 83 C4 28 E9 ? ? ? ?") }, //18003B530
				// DAT_1801ee4a0 (live execute_info): module_main's `CMP RCX,0x1760 / JNZ / MOV [rip],RDX`.
				{ S::STF_RENDER_EXECINFO, immediate(get_module_pattern(dll, "48 81 F9 60 17 00 00 0F 85 ? ? ? ? 48 89 15 ? ? ? ?", 16)) }, //1801EE4A0
			};

			// ---- archive spinlock (recursive_rwspinlock wlock/wunlock) ------------------------
			//
			// The ONLY two symbols in this table that the Like a Dragon Gaiden rebuild moved out
			// from under their patterns. Everything else here still matches both builds byte for
			// byte; these two functions were genuinely recompiled:
			//
			//   wunlock   LJ 0x18006EA00 decrements through a stack temp; Gaiden 0x18006ECC0 does
			//             it branchlessly (`lea eax,[rdx-1]; xor eax,edx; movzx eax,ax; xor edx,eax`).
			//   wlock     same TLS-owner algorithm in both (owner thread id in the high 16 bits,
			//             recursion count in the low 16), but LJ 0x18006EE20 keeps the lock word in
			//             RDI/ESI where Gaiden 0x18006F160 uses RDI/EBX and saves RBP too.
			//
			// Both are matched on the FUNCTION BODY rather than on a call site. The old wlock
			// pattern anchored on `E8 ? ? ? ? 8B 43 10 83 E8 01`, a caller that no longer exists in
			// the Gaiden build at all, and the old wunlock pattern ended in `48 8B C7` — a register
			// choice, which is exactly the kind of thing a recompile changes. Bodies are stabler.
			//
			// Semantics are identical in both, so they go into the same two slots and nothing
			// downstream cares which build supplied them.
			{
				struct LockPattern { const char* wlock; const char* wunlock; };
				static constexpr LockPattern LOCK_PATTERNS[] = {
					// Lost Judgment / Yakuza: Like a Dragon (2021-2022 builds).
					{ "48 89 5C 24 18 55 56 57 48 83 EC 20 65 48 8B 04 25 58 00 00 00 48 8B F9 8B 15 ? ? ? ? 48 8B 1C D0 B8 14 00 00 00 80 3C 18 00 75 05 E8 ? ? ? ? B8 04 00 00 00 8B 34 18 85 F6 75 07 E8 ? ? ? ? 8B F0 8B EE C1 E5 10 66 0F 1F 44 00 00 8B 0F 33 DB 89 4C 24 40 8B 44 24 40 85 C0",
					  "8B 01 89 44 24 08 8B 44 24 08 FF C8 0F B7 D0 8B 44 24 08 25 00 00 FF FF 0B C2 89 44 24 08" },
					// Like a Dragon Gaiden (2023-11-27).
					{ "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 65 48 8B 04 25 58 00 00 00 48 8B F9 8B 15 ? ? ? ? 48 8B 1C D0 B8 14 00 00 00 80 3C 18 00 75 05 E8 ? ? ? ? B8 04 00 00 00 8B 34 18 85 F6 75 07 E8 ? ? ? ? 8B F0 8B EE C1 E5 10 8B 07 33 DB 85 C0",
					  "8B 11 8D 42 FF 33 C2 0F B7 C0 33 D0 66 85 D2 75 0A 0F AE F0 C7 01 00 00 00 00 C3 89 11 C3" },
				};

				for (const LockPattern& candidate : LOCK_PATTERNS)
				{
					hook::pattern wlock(dll, candidate.wlock);
					hook::pattern wunlock(dll, candidate.wunlock);
					if (wlock.size() == 1 && wunlock.size() == 1)
					{
						symbols.Add(S::ARCHIVE_LOCK_WLOCK, wlock.get(0).get<void>());
						symbols.Add(S::ARCHIVE_LOCK_WUNLOCK, wunlock.get(0).get<void>());
						break;
					}
				}
			}

			// i960 fetch/decode dispatcher: `mov rax,[ctx]; lea r10,[base]; mov r9d,[rax+8]; add r9,[rax]; movzx eax,[r9+3]`.
			// OPTIONAL — StF (0x1800255F0) and FV ship this as a standalone single-instruction step, but
			// Motor Raid's CPU core inlines fetch+decode into its execution loop (FUN_18002CAC0), so no
			// function of this shape exists there. Absent symbol => InstallRamExecFetch does nothing.
			{
				hook::pattern fetchExec(dll, "48 83 EC 28 48 8B 05 ? ? ? ? 4C 8D 15 ? ? ? ? 44 8B 48 08 4C 03 08 41 0F B6 41 03");
				if (fetchExec.size() == 1)
				{
					symbols.Add(S::I960_FETCH_EXEC, fetchExec.get(0).get<void>());
				}
			}

			// Frame step, anchored on its whole prologue; the payload is match + 0x25, the CALL
			// to the I/O refresh. OPTIONAL and unique where it exists: StF (0x180055760) and FV
			// (0x180053DC0) share this shape, Motor Raid and the YLAD VF2 build do not. Absent
			// symbol => InstallSystemSwitches does nothing and the switches stay released.
			{
				hook::pattern frameStep(dll,
					"40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 80 3D ? ? ? ? 00 48 89 43 60 0F 84 ? ? ? ? FF 05 ? ? ? ? E8");
				if (frameStep.size() == 1)
				{
					symbols.Add(S::I960_IO_REFRESH_CALL, frameStep.get(0).get<void>(0x25));
				}
			}

			return symbols;
		}
	}

