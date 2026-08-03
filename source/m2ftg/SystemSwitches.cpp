#include "SystemSwitches.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"
#include "../Utils/MemoryMgr.h"
#include "../Utils/Trampoline.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace m2ftg
{
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
			// Expected only on Motor Raid, which does not share this I/O core: the cabinet
			// switches just stay released. Every other module has it - the YLAD VF2 build
			// included, whose refresh differs from the LJ one by a single short-vs-near JZ.
			DebugLog("[%s] No emulated I/O board - Test / Service switches unavailable.\n",
				gGeneral.GetGameTag());
			return;
		}

		// A direct rel32 from the module to YAMP's own code is not guaranteed to be in range.
		SystemSwitches::orgIoRefresh = refresh;
		Trampoline* t = Trampoline::MakeTrampoline(dll);
		Memory::InjectHook(callSite, t->Jump(&SystemSwitches::IoRefresh));
	}

	void SetSystemSwitches(bool test, bool service)
	{
		const uint8_t mask = static_cast<uint8_t>((test ? SystemSwitches::BIT_TEST : 0)
			| (service ? SystemSwitches::BIT_SERVICE : 0));
		SystemSwitches::heldMask.store(mask, std::memory_order_relaxed);
	}
}
