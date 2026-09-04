#include "ImportSymbols.h"
#include "../../pxd/PatternHelpers.h"

#include "../../Utils/Patterns.h"
#include "../../Utils/MemoryMgr.h"

#include <string>

using namespace hook::txn;

namespace m2ftg
{
	namespace K2
	{
		// Pattern resolution: the shared helpers in pxd/PatternHelpers.h, under this
		// namespace's names so the tables below read unchanged.
		using pxd::get_module_pattern;
		using pxd::immediate;
		using pxd::immediate8;
		using pxd::immediateImm8;

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

			// The two-board gate - see the long note in ImportSymbols.h. Anchored on the frame
			// step's BOARD-1 TAIL, which is what makes this unambiguous: the `CMP byte [gate],0`
			// is followed by the short JZ, `MOV ECX,1` (board index 1) and the CALL to the bank
			// switch, then the two per-board step calls and the `XOR ECX,ECX` + CALL that selects
			// board 0 again. Nothing else in the image looks like that.
			//
			// Deliberately anchored on the TAIL rather than on the frame step's prologue: the
			// prologue is shared by every m2ftg module (YAMP already matches it as
			// I960_IO_REFRESH_CALL), and the two jz encodings - `0F 84` in StF/FV, `74` here -
			// move every later offset. The tail only exists where the second board does.
			//
			// OPTIONAL: one hit in omg (gate 0x1806910DE) and one in mr; zero in StF/FV/VF2.
			{
				hook::pattern boardTail(pxd::module_scan(dll),
					"80 3D ? ? ? ? 00 74 ? B9 01 00 00 00 E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? 33 C9 E8");
				if (boardTail.size() == 1)
				{
					symbols.Add(S::TWO_BOARD_GATE, immediateImm8(boardTail.get(0).get<void>(2)));
					// Same anchor, offset 29: the `XOR ECX,ECX` feeding the step's final
					// bank-switch call. 7 (cmp) + 2 (jz) + 5 (mov ecx,1) + 3*5 (calls) = 29.
					symbols.Add(S::RENDER_BOARD_SELECT, boardTail.get(0).get<void>(29));
					// The bank switch itself, resolved from the CALL the tail makes right after
					// `MOV ECX,1` (offset 14 in the pattern) rather than by matching its own
					// prologue - the call site is already uniquely anchored, so this cannot land
					// on a lookalike.
					symbols.Add(S::BOARD_SELECT,
						Memory::ReadCallFrom(boardTail.get(0).get<void>(14)));
					// Board 1's I/O refresh, the next call along: 14 is the bank switch, 19 the
					// refresh, 24 the CPU step. The cabinet switches have to reach this one too or
					// the pair deadlocks - see I960_IO_REFRESH_CALL_B1 in the header.
					symbols.Add(S::I960_IO_REFRESH_CALL_B1, boardTail.get(0).get<void>(19));
				}
			}

			// The frame driver's board-0 sequence: `XOR ECX,ECX / CALL bank_switch / CALL io_refresh
			// / CALL cpu_step / CMP byte [two_board_gate],0`. The trailing CMP is what makes it
			// unique - the same three-call shape appears in the board-1 tail, which is followed by
			// the bank switch back to 0 instead.
			{
				hook::pattern board0(pxd::module_scan(dll), "33 C9 E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? 80 3D");
				if (board0.size() == 1)
				{
					symbols.Add(S::I960_IO_REFRESH_CALL, board0.get(0).get<void>(7));
				}
			}

			// The frame driver's link-transfer call: `INC dword [link_frame_counter] / CALL
			// transfer / XOR ECX,ECX / CALL bank_switch`. The INC is what anchors it - the
			// board-0 pattern above starts at that same XOR, so the two agree on the same site.
			{
				hook::pattern linkXfer(pxd::module_scan(dll), "FF 05 ? ? ? ? E8 ? ? ? ? 33 C9 E8");
				if (linkXfer.size() == 1)
				{
					symbols.Add(S::LINK_TRANSFER_CALL, linkXfer.get(0).get<void>(6));
				}
			}

			// The QueryPerformanceCounter wrapper. Matched on its whole body rather than a prologue:
			// it is a tiny leaf, and `zero a local / LEA it / CALL [IAT] / return the local` in this
			// exact register allocation occurs once. The IAT displacement is wildcarded because it
			// is the one part that moves between builds.
			{
				hook::pattern clock(pxd::module_scan(dll),
					"48 83 EC 28 48 8D 4C 24 30 48 C7 44 24 30 00 00 00 00 FF 15 ? ? ? ? "
					"48 8B 44 24 30 48 83 C4 28 C3");
				if (clock.size() == 1)
				{
					symbols.Add(S::WALL_CLOCK, clock.get(0).get<void>(0));
				}
			}

			// (A PER_BOARD_INIT pattern lived here and has been removed - it matched boot phase 2's
			// `XOR ECX,ECX / CALL / MOV ECX,1 / CALL` pair, which is the per-board VIDEO init, not
			// the CPU one. See the note in ImportSymbols.h.)

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
