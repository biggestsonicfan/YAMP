#include "Determinism.h"
#include "BoardVtables.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace pre3
{
	namespace Determinism
	{
		// M3EInput::get_time. Never overridden by any M3ERom subclass, so one slot covers all six
		// games - the same property FindBoardVtables relies on for slot 0.
		inline constexpr size_t SLOT_GET_TIME = 17;

		using get_time_fn = long long (*)(void* self);

		static get_time_fn orgGetTime = nullptr;
		static std::atomic<bool> pinned{ false };
		static std::atomic<long long> epoch{ DETERMINISTIC_EPOCH };

		static long long GetTime(void* self)
		{
			if (!pinned.load(std::memory_order_relaxed))
			{
				return orgGetTime(self);
			}
			// Returning a CONSTANT rather than a counter is deliberate: the board advances its
			// own copy when it wants elapsed time, and a host-side counter would reintroduce the
			// very host dependence this removes. The default is the module's own reset value, so
			// a pinned board reports what a linked board would at the moment of reset.
			return epoch.load(std::memory_order_relaxed);
		}
	}

	void InstallDeterministicClock(void* const* vtables, size_t count)
	{
		void* original = nullptr;
		const size_t patched = RedirectBoardSlot(vtables, count, Determinism::SLOT_GET_TIME,
			reinterpret_cast<void*>(&Determinism::GetTime), &original);
		if (patched == 0)
		{
			DebugLog("[%s] board clock accessor not consistent across %zu vtables - not pinned.\n",
				gGeneral.GetGameTag(), count);
			return;
		}

		Determinism::orgGetTime = reinterpret_cast<Determinism::get_time_fn>(original);

		// PINNED IMMEDIATELY, not left for a round to turn on. This install runs before
		// module_start, and it has to, because the board reads the clock while it BOOTS - and the
		// power-on snapshot a round restores is captured at the end of that boot.
		//
		// Measured, and it is the reason this is not still "off by default" as first written: two
		// runs of the same build with the clock unpinned produced different canaries at the same
		// point, and pinning it made every frame of the boot reproducible across processes. An
		// unpinned boot therefore gives the two peers different snapshots, which is a divergence
		// baked into the state a round starts from - the one place lockstep can never recover.
		//
		// A round re-pins this to the shared match seed (see SetDeterministicClock), which is a
		// change of value at a moment both peers are held at the same emulated frame.
		SetDeterministicClock(true);
		DebugLogFile("[%s] deterministic clock pinned (%zu vtable entries, host clock %p)\n",
			gGeneral.GetGameTag(), patched, original);
	}

	void SetDeterministicClock(bool pin, long long clockEpoch)
	{
		// Epoch first, so a board that reads the clock between these two stores can never see
		// "pinned" paired with the previous session's value.
		Determinism::epoch.store(clockEpoch, std::memory_order_relaxed);
		Determinism::pinned.store(pin, std::memory_order_relaxed);
	}

	bool DeterministicClockAvailable()
	{
		return Determinism::orgGetTime != nullptr;
	}

	// ---- The emulated machine ------------------------------------------------------------
	//
	// See Determinism.h for what each of these is and where it was read from. Grouped here rather
	// than spread through the functions so the whole of what the host reaches into the module's
	// private state is one short list that can be re-derived against a future build.
	namespace Machine
	{
		// THE EMULATOR OBJECT ITSELF, not a pointer to one. `TaskM3E` (the name survives in the
		// module's RTTI) is a STATIC instance in .data, constructed by the module's own C++
		// initialiser at DLL 0x180001140 - which is the write that identifies it: that function
		// stores TaskM3E::vftable (DLL 0x18010C3B8, whose slot 2 is the frame step at
		// DLL 0x1800393D0) into DLL 0x1801895D0.
		//
		// Worth stating because the obvious candidate is wrong and was tried first: the global
		// module_start fills in at DLL 0x180527BF8 is `M2FTGAppModule`, an 0x18-BYTE wrapper whose
		// own slot 2 module_main calls. Reading TaskM3E's fields off that lands in three unrelated
		// qwords, and reads as "the board never booted" rather than as a bad offset.
		//
		// The corroboration is free and exact: every other offset below is expressed relative to
		// the object that the frame step and the savestate pump take as `this`, and
		// 0x1801895D0 + 0x90 = 0x180189660 - the ROM-object global module_main independently reads
		// to set the coin latch and to ask whether the "press start" screen is up.
		inline constexpr uintptr_t RVA_MACHINE = 0x1895D0;

		// Bring-up phase. The frame step switches on it; 0x10 is the terminal "running" case, and
		// the savestate pump (DLL 0x180037E50) refuses to do anything until it is reached.
		inline constexpr size_t BOOT_STATE = 0x88;
		inline constexpr int BOOT_STATE_RUNNING = 0x10;

		// The CM3Mem object, and through it the guest's memory. Offsets from the savestate
		// writer/reader pair (DLL 0x180037E50 / 0x1800382F0) and cross-checked against the memory
		// clear (DLL 0x180008B10), which zeroes the same three buffers at the same sizes:
		//
		//     mem+0x60 -> bank descriptor -> +0x00 = 0xC00000 main RAM   (also reachable at mem+0x08)
		//     mem+0x68 -> the second bank, restored from the same bytes
		//     mem+0x48 =  0x800000 VRAM
		inline constexpr size_t MEM_OBJECT = 0xA8;
		inline constexpr size_t MEM_BANK0_DESC = 0x60;
		inline constexpr size_t RAM_SIZE = 0xC00000;

		// The preloaded-state vector: {void* bytes, size_t length} pairs. The module's own vector
		// header is {data, capacity, count} at +0x00 / +0x08 / +0x0C (DLL 0x18003B7E0), so for the
		// vector based at machine+0x108 the count is machine+0x114.
		inline constexpr size_t PRELOAD_DATA = 0x108;
		inline constexpr size_t PRELOAD_COUNT = 0x114;

		// The request bitfield the frame step drains - see ResetBoard's comment in the header.
		inline constexpr size_t REQUEST = 0x120;
		inline constexpr uint8_t REQ_SAVE = 0x01;           // push the machine onto the save vector
		inline constexpr uint8_t REQ_RESTORE_SAVE = 0x02;   // restore its newest entry (no pop)
		inline constexpr uint8_t REQ_RESTORE_PRELOAD = 0x10;
		inline constexpr uint8_t REQ_PRELOAD_SLOT = 0x60;   // bits 5-6 select which state

		// The save vector YAMP's own snapshot lives in, laid out exactly like the preloaded one:
		// {data, capacity, count} at +0x00 / +0x08 / +0x0C from its base.
		inline constexpr size_t SAVE_DATA = 0xF8;
		inline constexpr size_t SAVE_COUNT = 0x104;

		// One dword per 256 bytes across the bank - see StateCheckValue in the header.
		inline constexpr size_t HASH_STRIDE = 0x100;

		// The worker-thread handshake - see WaitForEmulatedFrame in the header for why the host
		// has to respect it. All three read out of the "m3e_ctrl" thread body (DLL 0x18003B270)
		// and corroborated by the savestate handler, which refuses to touch the machine while
		// either busy flag is set (DLL 0x1800382F0).
		inline constexpr size_t BUSY_CTRL = 0x16C;   // inside the frame step
		inline constexpr size_t BUSY_DISP = 0x16D;   // inside the memory object
		inline constexpr size_t FRAME_MARKER = 0x148;

		// The PowerPC core's architectural state, which is the other half of the canary.
		//
		// The CPU object is the same one the HLE hook table walks (CPpc603e::reset writes it to
		// DLL 0x18062AA90), and its register file is at cpu+8 - confirmed at runtime rather than
		// assumed, by reading the module's own register-file global at DLL 0x18052AA88 and finding
		// it equal to cpu+8 on a live board.
		//
		// Hashed from the GPRs through LR and no further. The first 16 bytes of the file are the
		// program counter and one unidentified qword, and they are deliberately SKIPPED: an
		// unnamed field is the one place a host-derived value could hide, and a canary that
		// disagreed for that reason would end every round with a false desync - a far worse
		// failure than a canary that is merely slow. Everything in the range that IS hashed has a
		// name: GPR0 at +0x10, FPR0 at +0x98, LR at +0x1A0.
		inline constexpr uintptr_t RVA_CPU_PTR = 0x62AA90;
		inline constexpr size_t CPU_REGISTER_FILE = 8;
		inline constexpr size_t REGS_FIRST = 0x10;    // GPR0
		inline constexpr size_t REGS_LAST = 0x1A8;    // one past LR

		static const uint8_t* ModuleBase()
		{
			// Named literally, as HleHooks does: the module is ASLR'd, there is exactly one pre3
			// DLL, and this file is not part of the host's start-up path that already has a handle.
			return reinterpret_cast<const uint8_t*>(
				GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
		}

		// Null only when the module is not loaded: the object is static, so it exists (zeroed)
		// from DLL load onwards. The phase word below is what says whether it means anything yet.
		static uint8_t* Object()
		{
			const uint8_t* base = ModuleBase();
			if (base == nullptr) return nullptr;
			return const_cast<uint8_t*>(base + RVA_MACHINE);
		}

		static uint8_t* Running()
		{
			uint8_t* machine = Object();
			if (machine == nullptr) return nullptr;
			return *reinterpret_cast<const int*>(machine + BOOT_STATE) == BOOT_STATE_RUNNING
				? machine : nullptr;
		}
	}

	bool IsBoardBooted()
	{
		return Machine::Running() != nullptr;
	}

	bool ResetBoard()
	{
		uint8_t* machine = Machine::Running();
		if (machine == nullptr)
		{
			return false;
		}

		// No preloaded state means the request would be dropped on the floor: the handler tests
		// the slot against this count and, finding nothing, clears the bit and returns. Saying so
		// here is the difference between "netplay is unavailable for this game" and a round that
		// starts from two different states and looks like a network fault.
		if (*reinterpret_cast<const uint32_t*>(machine + Machine::PRELOAD_COUNT) == 0
			|| *reinterpret_cast<uint8_t* const*>(machine + Machine::PRELOAD_DATA) == nullptr)
		{
			DebugLogFile("[%s] no preloaded machine state - cannot reset the board\n",
				gGeneral.GetGameTag());
			return false;
		}

		// Slot 0, which is the only one Fighting Vipers 2 has (image/fv2/vs_start.bin). Written
		// exactly the way the module writes it for itself at the end of bring-up: clear the slot
		// bits, set the restore bit, leave the rest of the field alone.
		uint8_t& request = *(machine + Machine::REQUEST);
		request = static_cast<uint8_t>(
			(request & ~(Machine::REQ_RESTORE_PRELOAD | Machine::REQ_PRELOAD_SLOT))
			| Machine::REQ_RESTORE_PRELOAD);
		return true;
	}

	bool BoardResetPending()
	{
		uint8_t* machine = Machine::Object();
		return machine != nullptr
			&& (*(machine + Machine::REQUEST) & Machine::REQ_RESTORE_PRELOAD) != 0;
	}

	bool SaveResetSnapshot()
	{
		uint8_t* machine = Machine::Running();
		if (machine == nullptr)
		{
			return false;
		}
		// One is all a round needs, and the handler allocates the whole machine every time it is
		// asked - so asking twice would leak ~22 MB per ask for a state identical to the one
		// already held.
		if (ResetSnapshotTaken())
		{
			return true;
		}
		*(machine + Machine::REQUEST) |= Machine::REQ_SAVE;
		return true;
	}

	bool RestoreResetSnapshot()
	{
		uint8_t* machine = Machine::Running();
		if (machine == nullptr || !ResetSnapshotTaken())
		{
			DebugLogFile("[%s] no power-on snapshot to restore\n", gGeneral.GetGameTag());
			return false;
		}
		*(machine + Machine::REQUEST) |= Machine::REQ_RESTORE_SAVE;
		return true;
	}

	bool ResetSnapshotTaken()
	{
		uint8_t* machine = Machine::Object();
		return machine != nullptr
			&& *reinterpret_cast<const uint32_t*>(machine + Machine::SAVE_COUNT) != 0
			&& *reinterpret_cast<uint8_t* const*>(machine + Machine::SAVE_DATA) != nullptr;
	}

	bool BoardRestorePending()
	{
		uint8_t* machine = Machine::Object();
		return machine != nullptr
			&& (*(machine + Machine::REQUEST)
				& (Machine::REQ_RESTORE_PRELOAD | Machine::REQ_RESTORE_SAVE)) != 0;
	}

	uint32_t StateCheckValue()
	{
		uint32_t cpu = 0, ram = 0;
		if (!StateCheckParts(cpu, ram))
		{
			return 0;
		}
		// FNV-1a's own combining step, so the value on the wire is exactly what one pass over both
		// regions would have produced - the split below is a diagnostic, not a different canary.
		const uint32_t h = (((2166136261u ^ cpu) * 16777619u) ^ ram) * 16777619u;
		// A hash that happened to land on zero would read as "no canary" to the caller.
		return h != 0 ? h : 1u;
	}

	bool StateCheckParts(uint32_t& cpu, uint32_t& ram)
	{
		cpu = 0;
		ram = 0;

		uint8_t* machine = Machine::Running();
		if (machine == nullptr)
		{
			return false;
		}

		// Structured net rather than trust: this runs once per emulated frame from the module
		// thread while the board is between frames, and the pointer chain is three loads into the
		// module's private state. A fault here must cost the canary, not the process.
		const uint8_t* const base = Machine::ModuleBase();
		if (base == nullptr)
		{
			return false;
		}

		__try
		{
			const auto* mem = *reinterpret_cast<uint8_t* const*>(machine + Machine::MEM_OBJECT);
			if (mem == nullptr) return false;
			const auto* desc = *reinterpret_cast<uint8_t* const* const*>(
				mem + Machine::MEM_BANK0_DESC);
			if (desc == nullptr) return false;
			const auto* ramBase = *reinterpret_cast<const uint8_t* const*>(desc);
			if (ramBase == nullptr) return false;

			// FNV-1a stepped a dword at a time, as the m2ftg canary is: it only has to agree
			// between two peers and to be sensitive to a changed byte, and both peers are x86-64
			// so the word load needs no endian conversion. Only the 32-bit combined result goes on
			// the wire, so the sampling density costs bandwidth nothing - it is purely local work.

			// The CPU's registers, which are what make this sensitive from one frame to the next.
			// Measured on a 400-frame run: the RAM half alone repeats its value for five frames at a
			// time on a static screen (a 256-byte stride simply does not land on the handful of words
			// a title screen touches), while the register half came out different on every frame.
			uint32_t h = 2166136261u;
			if (const auto* cpuObject =
				*reinterpret_cast<uint8_t* const*>(base + Machine::RVA_CPU_PTR))
			{
				const uint8_t* regs = cpuObject + Machine::CPU_REGISTER_FILE;
				for (size_t off = Machine::REGS_FIRST; off < Machine::REGS_LAST; off += 4)
				{
					h = (h ^ *reinterpret_cast<const uint32_t*>(regs + off)) * 16777619u;
				}
			}
			cpu = h;

			h = 2166136261u;
			for (size_t off = 0; off < Machine::RAM_SIZE; off += Machine::HASH_STRIDE)
			{
				h = (h ^ *reinterpret_cast<const uint32_t*>(ramBase + off)) * 16777619u;
			}
			ram = h;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			cpu = 0;
			ram = 0;
			return false;
		}
	}

	unsigned long long BoardFrameMarker()
	{
		uint8_t* machine = Machine::Running();
		if (machine == nullptr)
		{
			return 0;
		}
		return *reinterpret_cast<volatile const unsigned long long*>(
			machine + Machine::FRAME_MARKER);
	}

	bool WaitForEmulatedFrame(unsigned long long previous, unsigned int timeoutMs)
	{
		uint8_t* machine = Machine::Running();
		if (machine == nullptr)
		{
			return false;
		}

		const auto* marker = reinterpret_cast<volatile const unsigned long long*>(
			machine + Machine::FRAME_MARKER);
		const auto* ctrl = reinterpret_cast<volatile const uint8_t*>(machine + Machine::BUSY_CTRL);
		const auto* disp = reinterpret_cast<volatile const uint8_t*>(machine + Machine::BUSY_DISP);

		// Spin rather than sleep: the wait is bounded by one emulated frame, and it is not extra
		// work - the module's own update stage would have waited for exactly this at the top of
		// the next call. All this does is move the wait to where the state can be read.
		const ULONGLONG deadline = GetTickCount64() + timeoutMs;
		for (;;)
		{
			if (*marker != previous && *ctrl == 0 && *disp == 0)
			{
				return true;
			}
			if (GetTickCount64() > deadline)
			{
				return false;
			}
			YieldProcessor();
		}
	}

	bool NetplaySupported()
	{
		return gGeneral.GetGameId() == YAMPGeneral::GameId::FV2
			&& DeterministicClockAvailable();
	}

	void LogBoardStateOnce()
	{
		static bool s_logged = false;
		if (s_logged)
		{
			return;
		}

		uint8_t* machine = Machine::Object();
		if (machine == nullptr)
		{
			return;   // module_start has not built it yet; try again next frame
		}
		const int phase = *reinterpret_cast<const int*>(machine + Machine::BOOT_STATE);
		if (phase != Machine::BOOT_STATE_RUNNING)
		{
			return;
		}

		s_logged = true;
		// The canary is taken here rather than merely tested for: a value proves the whole
		// mem -> bank descriptor -> RAM chain resolved, which nothing else in a stand-alone run
		// would ever exercise.
		DebugLogFile("[%s net] machine=%p phase=0x%X preloaded states=%u request=0x%02X "
			"canary=0x%08X clock pin=%d netplay=%d\n",
			gGeneral.GetGameTag(), static_cast<void*>(machine), phase,
			*reinterpret_cast<const uint32_t*>(machine + Machine::PRELOAD_COUNT),
			*(machine + Machine::REQUEST), StateCheckValue(),
			static_cast<int>(DeterministicClockAvailable()),
			static_cast<int>(NetplaySupported()));
	}
}
