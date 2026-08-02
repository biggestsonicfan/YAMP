#include "Patch.h"

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
#include "HleHooks.h"
#include "DebugWindows.h"

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
			const GameDesc& game = CurrentGame();
			// Motor Raid inlines fetch+decode into its execution loop, so it has neither the
			// single-step dispatcher to hook nor the i960 RVAs this reimplementation needs.
			// It also has no ROM debug menu to reach, so simply leave its CPU core alone.
			void* const fetchExec = symbols.TryGetSymbol(ImportSymbol::I960_FETCH_EXEC);
			if (fetchExec == nullptr || game.rva_cpu_ctx_ptr == 0)
			{
				return;
			}

			RamExecFetch::RVA_CPU_CTX_PTR = game.rva_cpu_ctx_ptr;
			RamExecFetch::RVA_OPCODE_TABLE = game.rva_opcode_table;
			RamExecFetch::RVA_RAM_BASE_PTR = game.rva_ram_base_ptr;
			RamExecFetch::dllBase = static_cast<uint8_t*>(dll);
			Trampoline* t = Trampoline::MakeTrampoline(dll);
			Memory::InjectHook(fetchExec, t->Jump(&RamExecFetch::FetchExec), Memory::HookType::Jump);
		}

		// ---- Cabinet TEST / SERVICE switches --------------------------------------------
		// The emulated Model 2 I/O board serves guest 0x01C00002 - the SYSTEM input port - out of
		// one byte of its own state (io[9], the bank-0 copy; io[0x0A] is the DIP bank, hard-wired
		// to 0xFF). Active low, laid out like the hardware:
		//
		//     bit0 coin 1   bit1 coin 2   bit2 TEST   bit3 SERVICE   bit4 start 1   bit5 start 2
		//
		// read_sw folds that byte into the low 8 bits of the ROM's flag longs at RAM 0x500700
		// (held) / 0x500704 (momentary), inverted - which is why the DLL's ADV_DSP handler fakes
		// "both players pressed Start" by writing 0x30 straight to 0x500704.
		//
		// The DLL rebuilds io[9] from scratch once per emulated frame and drives only coin 1
		// (from the execute_info coin bit, via a one-shot flag at io+0x4098) and the two starts;
		// TEST and SERVICE have no host source in the module protocol at all. Since nothing about
		// io[9] survives a frame, a host write between module_main calls is simply overwritten -
		// so intercept the CALL to the refresh and pull the two lines low immediately after it,
		// before the frame's first i960 instruction runs.
		namespace SystemSwitches
		{
			constexpr size_t IO_SYSTEM_PORT = 9;
			constexpr uint8_t BIT_TEST = 0x04;
			constexpr uint8_t BIT_SERVICE = 0x08;

			static void (*orgIoRefresh)() = nullptr;
			static uint8_t* const* ioState = nullptr;   // the DLL's own I/O board pointer
			// Written by the host thread each frame, read by the module thread inside the hook.
			static std::atomic<uint8_t> heldMask{ 0 };

			static void IoRefresh()
			{
				orgIoRefresh();
				uint8_t* const io = *ioState;
				if (io != nullptr)
				{
					// Active low: a closed switch is a CLEARED bit.
					io[IO_SYSTEM_PORT] &= static_cast<uint8_t>(~heldMask.load(std::memory_order_relaxed));
				}
			}

			// The refresh function's first act is `MOV R10, [rip+disp32]` loading the I/O board
			// pointer (StF +0x2B -> 0x1806C9B88, FV +0x2B -> 0x1806CC188). Decoding the global
			// out of that instruction avoids a per-DLL RVA and survives ASLR, which FV needs.
			static uint8_t* const* FindIoState(const uint8_t* refresh)
			{
				constexpr uint8_t MOV_R10_RIP[] = { 0x4C, 0x8B, 0x15 };
				constexpr size_t SEARCH_BYTES = 0x40;
				for (size_t i = 0; i + sizeof(MOV_R10_RIP) + 4 <= SEARCH_BYTES; i++)
				{
					if (memcmp(refresh + i, MOV_R10_RIP, sizeof(MOV_R10_RIP)) != 0)
					{
						continue;
					}
					uint8_t* const* global = nullptr;
					Memory::ReadOffsetValue(refresh + i + sizeof(MOV_R10_RIP), global);
					return global;
				}
				return nullptr;
			}
		}

		void InstallSystemSwitches(void* dll, const Imports& symbols)
		{
			void* const callSite = symbols.TryGetSymbol(ImportSymbol::I960_IO_REFRESH_CALL);
			void (*refresh)() = nullptr;
			if (callSite != nullptr)
			{
				Memory::ReadCall(callSite, refresh);
				SystemSwitches::ioState =
					SystemSwitches::FindIoState(reinterpret_cast<const uint8_t*>(refresh));
			}
			if (SystemSwitches::ioState == nullptr)
			{
				// Expected on the modules that do not share this I/O core (MR, the YLAD VF2
				// build): the cabinet switches just stay released.
				DebugLog("[%s] No emulated I/O board - Test / Service switches unavailable.\n",
					gGeneral.GetGameTag());
				return;
			}

			// A direct rel32 from the module to YAMP's own code is not guaranteed to be in range.
			SystemSwitches::orgIoRefresh = refresh;
			Trampoline* t = Trampoline::MakeTrampoline(dll);
			Memory::InjectHook(callSite, t->Jump(&SystemSwitches::IoRefresh));
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
		// Paired with HLE hook 16 (GAME_INT+0x4), which YAMP disables by default: that hook copies
		// the same byte into `time` raw, so with this fix applied and that hook restored the round
		// timer becomes 2 seconds. See HleHooks.h for that half.
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

		void SetSystemSwitches(bool test, bool service)
		{
			const uint8_t mask = static_cast<uint8_t>((test ? SystemSwitches::BIT_TEST : 0)
				| (service ? SystemSwitches::BIT_SERVICE : 0));
			SystemSwitches::heldMask.store(mask, std::memory_order_relaxed);
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

