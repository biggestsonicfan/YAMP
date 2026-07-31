#include "Patch.h"

#include "../../pxd/LJ/file_access.h"
#include "../../pxd/LJ/pxd_types.h"
#include "../../pxd/LJ/async_request.h"

#include "../../wil/common.h"
#include "../../DebugLog.h"
#include "../../Utils/MemoryMgr.h"
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

