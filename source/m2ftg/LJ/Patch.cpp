#include "Patch.h"
#include "../SystemSwitches.h" // InstallSystemSwitches (moved out; shared with the YLAD host)

#include "../../pxd/LJ/file_access.h"
#include "../../pxd/LJ/pxd_types.h"
#include "../../pxd/LJ/async_request.h"

#include "../../wil/common.h"
#include "../../DebugLog.h"
#include "../../YAMPSettings.h"
#include "../../Utils/MemoryMgr.h"
#include "../../Utils/Patterns.h"
#include "../../Utils/Trampoline.h"

#include "../../pxd/LJ/sys_util.h"
#include "../../pxd/LJ/cs_game.h"
#include "../Debug/HleHooks.h"
#include "../Debug/DebugWindows.h"

#include "../ImportSymbols.h"
#include "../../pxd/Imports.h"
#include "../../pxd/LJ/HostCdevice.h"
#include "LJHost.h" // GameDesc / CurrentGame()

#include <d3d12.h>
#include <atomic>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include "../../wil/com.h"
#include "../../RenderWindow.h"

namespace m2ftg
{
	// The pxd platform layer this host is built on (source/pxd): sl/gs/cgs_device_context,
	// the host cdevice, Imports/ImportSymbol. Shared with the LJ VF5FS host.
	using namespace pxd;
		// The generic pxd DX12 gs bring-up (PatchGs / ResetCbvSrvRingCursors and the descriptor-heap
		// setup behind them) moved to ../../pxd/PatchGs.cpp — it is engine wiring with no m2ftg in it,
		// and the LJ VF5FS host needs the same. What stays below is m2ftg's own patching.

		static void SilentInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
			unsigned int, uintptr_t) {}

		static void prj_trap(const char* format, ...)
		{
			// The pxd engine's trap/log uses custom format directives (e.g. "%~", "%(") that the
			// standard CRT formatter rejects — in a Debug build vsprintf_s would raise the
			// "Incorrect format specifier" assertion and kill the process. The game calls this
			// during the normal GameLoop, so it must NOT crash or halt. Format leniently (swallow
			// invalid-parameter asserts, fall back to the raw string) and never __debugbreak.
			char buf[512] = {};
			va_list vl;
			va_start(vl, format);
			const _invalid_parameter_handler prev =
				_set_thread_local_invalid_parameter_handler(&SilentInvalidParameter);
			const int n = format ? _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, format, vl) : -1;
			_set_thread_local_invalid_parameter_handler(prev);
			va_end(vl);

			if (n < 0)
			{
				// Formatting failed (custom directive / arg mismatch) — emit the raw format text.
				// The raw text may contain % directives, so pass it as an argument, never as the format.
				DebugLog("[%s prj_trap] ", gGeneral.GetGameTag());
				if (format) DebugLog("%s", format);
				DebugLog("\n");
			}
			else
			{
				DebugLog("%s", buf);
			}
		}

		void ReinstateLogging(void* dll, const Imports& symbols)
		{
			Trampoline* t = Trampoline::MakeTrampoline(dll);
			for (const auto& [key, func] : symbols.GetSymbolRange(ImportSymbol::PRJ_TRAP))
			{
				Memory::InjectHook(func, t->Jump(&prj_trap), Memory::HookType::Jump);
			}
		}

		void InjectTraps(const Imports& symbols)
		{
#ifdef _DEBUG
			for (const auto& [key, ptr] : symbols.GetSymbolRange(ImportSymbol::TRAP_ALLOC_INSTANCE_TBL))
			{
				Memory::Patch<uint8_t>(ptr, 0xCC);
			}
#endif
		}

		// ---- RAM-executable i960 instruction fetch --------------------------
		// The ROM's dw debug menu (enabled via the RAM 0x508000 debug flag) dispatches menu
		// items by copying an i960 instruction pair into a trampoline at RAM 0x59F270 and
		// `callx`-ing into it — legal on hardware, but the retail DLL's CPU core fetches
		// instructions linearly from the program-ROM host buffer only (ctx->codeBase + IP),
		// so a RAM IP reads past the 1MB ROM image and crashes the host. This reimplements
		// the DLL's fetch/decode dispatcher byte-for-byte in behaviour, with one change:
		// the host base is chosen per region, exactly like the DLL's own data accessors.
		namespace RamExecFetch
		{
			// DLL globals by fixed RVA (same layout StfDebugWindows relies on): the i960
			// context pointer, the 0x1000-entry opcode dispatch table the original indexes,
			// and the work-RAM host base (the emulated address is added directly to it).
			// Per-game — the StF and FV DLLs place them differently; values live in the
			// GameDesc table (LJHost.h) and are latched at install time.
			static uintptr_t RVA_CPU_CTX_PTR = 0;
			static uintptr_t RVA_OPCODE_TABLE = 0;
			static uintptr_t RVA_RAM_BASE_PTR = 0;
			// Work-RAM window the trampoline lives in (memory-map region 5).
			constexpr uint32_t RAM_START = 0x500000;
			constexpr uint32_t RAM_SIZE = 0x100000;

			static uint8_t* dllBase = nullptr;

			static void FetchExec()
			{
				uint8_t* const ctx = *reinterpret_cast<uint8_t* const*>(dllBase + RVA_CPU_CTX_PTR);
				const uint32_t ip = *reinterpret_cast<const uint32_t*>(ctx + 8);
				const uint8_t* instr;
				if (ip - RAM_START < RAM_SIZE)
				{
					instr = *reinterpret_cast<uint8_t* const*>(dllBase + RVA_RAM_BASE_PTR) + ip;
				}
				else
				{
					instr = *reinterpret_cast<uint8_t* const*>(ctx) + ip;
				}

				// Dispatch-table index, exactly as the original computes it: REG formats
				// (opcode 0x5x-0x7x) pack the sub-opcode from bits 7-10; the 0x8x-0xCx MEM
				// group packs bits 10-13, with bit 12 selecting the wider mask.
				const uint32_t word = *reinterpret_cast<const uint32_t*>(instr);
				// Every executed instruction passes through here, so this is where an HLE trap
				// can be counted and the IP sampled without patching anything in the module -
				// and it is the only vantage point that sees inside a frame.
				HleHooks::NoteFetchedWord(word);
				I960Profile::NoteFetch(ip);
				const uint32_t opcode = instr[3];
				uint32_t index = opcode << 4;
				const uint32_t group = opcode >> 4;
				if (group >= 5 && group <= 7)
				{
					index |= (word >> 7) & 0xF;
				}
				else if (group >= 8 && group <= 0xC)
				{
					index |= (((word >> 12) & 1) != 0 ? 0xFu : 0x8u) & (word >> 10);
				}

				const auto handler = *reinterpret_cast<int32_t(* const*)(const void*)>(
					dllBase + RVA_OPCODE_TABLE + static_cast<size_t>(index) * 8);
				*reinterpret_cast<int32_t*>(ctx + 8) += handler(instr);
			}
		}

		void InstallRamExecFetch(void* dll, const Imports& symbols)
		{
			// Per module BUILD, not per game: Sonic the Fighters ships in both Lost Judgment and
			// Like a Dragon Gaiden and the two DLLs put these globals 0xB9C0 apart.
			const I960Build& i960 = CurrentI960();
			// Motor Raid inlines fetch+decode into its execution loop, so it has neither the
			// single-step dispatcher to hook nor the i960 RVAs this reimplementation needs.
			// It also has no ROM debug menu to reach, so simply leave its CPU core alone.
			void* const fetchExec = symbols.TryGetSymbol(ImportSymbol::I960_FETCH_EXEC);
			if (fetchExec == nullptr || i960.rva_cpu_ctx_ptr == 0)
			{
				return;
			}

			RamExecFetch::RVA_CPU_CTX_PTR = i960.rva_cpu_ctx_ptr;
			RamExecFetch::RVA_OPCODE_TABLE = i960.rva_opcode_table;
			RamExecFetch::RVA_RAM_BASE_PTR = i960.rva_ram_base_ptr;
			RamExecFetch::dllBase = static_cast<uint8_t*>(dll);
			Trampoline* t = Trampoline::MakeTrampoline(dll);
			Memory::InjectHook(fetchExec, t->Jump(&RamExecFetch::FetchExec), Memory::HookType::Jump);
		}


		// ---- GAME ASSIGNMENTS lockup -----------------------------------------------------
		// Selecting GAME ASSIGNMENTS in the board's service menu froze the whole app. The i960
		// spins forever inside print_mes (ROM 0x5680-0x569C), so module_main never returns and
		// the host frame pump never gets control back - the freeze is the emulator's, but the
		// cause is one bad byte in the backup RAM the module injects.
		//
		// The menu's value column is drawn by game_assign_bk_ram_thing (ROM 0x637C8). For an
		// item whose flag word has bit 8 set and bit 2 clear, it uses the item's backup-RAM byte
		// as a RAW INDEX into a ROM array of string pointers, with no bounds check:
		//     r9 = item[0x10]; g0 = r9[value * 4]; print_mes(g0)
		// The TIME item (backup +0x3351, "time_var_array_num") indexes a 10-entry table at ROM
		// 0x5E088 holding "10".."99" - the display strings for time_vars[] at ROM 0x8F3C4
		// (0A 14 1E 28 32 3C 46 50 5A 63). The ROM's own default, written by
		// init_game_assignments (ROM 0x626A4), is 2 - index of "30" - and the menu's own edit
		// handler ga_TIME_sub (ROM 0x5D648) clamps the field to 0..9.
		//
		// The module writes 0x1E: the time in SECONDS, not the index. Index 30 reads 80 bytes
		// past the end of a 40-byte table - into i960 code - and yields the pointer 0x8C703000.
		// That guest address is unmapped, and an unmapped read in this emulator is a bare `ret`
		// stub that leaves the destination register untouched, so print_mes's `ldib` returns the
		// same non-zero byte forever and its "stop at NUL" loop can never end. On real hardware
		// the same read would eventually hit a zero (or the watchdog would reset the board);
		// here it is a guaranteed hang. TIME is the only item of the eighteen whose injected
		// value is out of range - every other byte in the block matches the ROM's own defaults
		// exactly, which is why only this one page locks up.
		//
		// Fixed at the source, by correcting the constant in the module's injector rather than
		// policing the backup RAM afterwards: the byte is the operator's setting, so the service
		// menu must stay free to change it, and re-asserting it per frame would fight the menu.
		// Paired with HLE hooks 16 (GAME_INT+0x4) and 17 (ADV_REPLAY_WAIT1A+0x128), which YAMP
		// disables by default: both read this same byte as RAW SECONDS rather than as the table
		// index it is, so with this fix applied and either hook restored a timer collapses to 2
		// seconds - hook 16 the match round timer, hook 17 the attract demo. See HleHooks.h.
		void FixBackupRamTimeIndex(void* dll)
		{
			const YAMPSettings* settings = gGeneral.GetSettings();
			if (settings != nullptr && !settings->m_stfFixBackupTimeIndex)
			{
				DebugLog("[%s] Backup-RAM TIME index: correction disabled in settings - the module's own "
					"0x1E is left in place, and the service menu's GAME ASSIGNMENTS page will hang the board.\n",
					gGeneral.GetGameTag());
				return;
			}

			// The tail of the injector (StF's set_window_data+0x564 handler, module +0x529D0)
			// that fills the +0x3340 half of the block. Anchored on the TST_*_ADD / TST_*_MUL
			// constants either side of it, which are distinctive and are themselves confirmed
			// correct against init_game_assignments:
			//     mov dword [rbp+0x20], 0x36161616   ; +0x3359 TST_RED/GRN/BLUE_ADD, RED_MUL
			//     lea  rax, [rbx+0x91]
			//     mov  word [rbp+0x24], 0x3636       ; +0x335D TST_GRN/BLUE_MUL
			//     mov  byte [rbp+0x26], 0x1F         ; +0x335F
			//     mov  byte [rbp+0x18], 0x1E         ; +0x3351 <- the one that is wrong
			// The stack buffer is copied out with `vmovups [rax+0x3340], ymm0` from [rbp+0x07],
			// which is what fixes the +0x07 <-> 0x3340 correspondence used above.
			constexpr ptrdiff_t IMMEDIATE_OFFSET = 27;  // the 0x1E, from the pattern's first byte
			constexpr uint8_t TIME_INDEX_30S = 0x02;    // init_game_assignments' own default

			hook::txn::pattern injector(dll,
				"C7 45 20 16 16 16 36 48 8D 83 ? ? ? ? 66 C7 45 24 36 36 C6 45 26 1F C6 45 18 1E");
			// size() is the non-throwing accessor - a module that does not match (a different
			// m2ftg build, or a future one where SEGA fixed this) simply keeps its own value.
			if (injector.size() != 1)
			{
				DebugLog("[%s] Backup-RAM TIME index: injector not matched (%zu hits) - left alone.\n",
					gGeneral.GetGameTag(), injector.size());
				return;
			}

			auto* const immediate = injector.get(0).get<uint8_t>(IMMEDIATE_OFFSET);
			if (*immediate != 0x1E)
			{
				DebugLog("[%s] Backup-RAM TIME index: expected 0x1E, found 0x%02X - left alone.\n",
					gGeneral.GetGameTag(), *immediate);
				return;
			}

			Memory::VP::Patch<uint8_t>(immediate, TIME_INDEX_30S);
			DebugLog("[%s] Backup-RAM TIME index: 0x1E (30s, a raw value) -> 0x%02X (the ROM's own "
				"index for \"30\"); GAME ASSIGNMENTS is safe to open.\n",
				gGeneral.GetGameTag(), TIME_INDEX_30S);
		}


		static void assign_helper_enable_shared_from_this(...)
		{
		}

		static float get_frame_speed_pause_stub()
		{
			return 1.0f;
		}

		static void* (*orgVF5AppCtor)(void* obj, int argc, char** argv);
		static void* VF5AppCtor_arguments(void* obj, int /*argc*/, char** /*argv*/)
		{
			return orgVF5AppCtor(obj, __argc, __argv);
		}
	}

