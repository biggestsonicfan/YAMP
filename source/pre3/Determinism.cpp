#include "Determinism.h"
#include "BoardVtables.h"

#include "../net/StateHash.h"
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

	// Is this game's BOOT survivable with the clock pinned? Measured, per game, not reasoned.
	//
	// The pinned clock returns a CONSTANT, and Sega Racing Classic 2's boot does not survive one:
	// with the pin in from install it never leaves its start-up sequence. The board reaches its
	// running phase and then sits there for as long as you care to wait - it creates none of the
	// 496x384 Real3D targets, issues no draws, and never produces an output texture, so the host
	// presents nothing and the game looks frozen on a loading screen. Bisected to the commit that
	// introduced this pin, then narrowed to the pin itself by suppressing each of that commit's
	// four unconditional pokes at the board in turn: only the clock changes anything.
	//
	// The module's own deterministic path does NOT freeze the clock. When the linked-cabinet
	// interface at params+0x1070 is present, get_time returns *(rom+0x470) - a fixed epoch that
	// slot 18 resets and THE BOARD THEN ADVANCES. A clock that ticks predictably and a clock that
	// does not tick at all are both reproducible, and only the first is something arcade code
	// expects; a game is perfectly entitled to wait for the second hand to move.
	//
	// So this is deliberately a whitelist rather than "everything except SRC2": a game earns the
	// pin by having been run with it. Fighting Vipers 2 has (it boots, and it is the only game
	// here with netplay). Anything else gets its real clock, which is what it had before the pin
	// existed and what it boots with.
	//
	// FOR WHOEVER BRINGS UP SRC2 NETPLAY: do not simply add it here - it will stop booting again.
	// The pin has to grow a clock that ADVANCES deterministically first (the module's own
	// mechanism above is the shape to copy), and then both games can use it.
	static bool ClockPinBootSafe()
	{
		return gGeneral.GetGameId() == YAMPGeneral::GameId::FV2;
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

		// The redirect goes in for every game, because it costs nothing when it is not pinned -
		// the hook tail-calls the module's own accessor - and because leaving it out would make
		// "can this game be pinned at all" depend on start-up order rather than on the module.
		//
		// WHETHER TO PIN IT NOW is a different question, and the answer is per-game. See
		// ClockPinBootSafe: a pinned clock does not tick, and one of the two games does not boot
		// with a clock that does not tick.
		if (!ClockPinBootSafe())
		{
			DebugLogFile("[%s] board clock redirect installed but NOT pinned - this game's boot "
				"needs a ticking clock (%zu vtable entries, host clock %p)\n",
				gGeneral.GetGameTag(), patched, original);
			return;
		}

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

		// machine+0x90, the ROM object - the same global module_main independently reads for the
		// coin latch, which is the free corroboration named above. Everything game-specific hangs
		// off it, so BoardRomObject() publishes it rather than each caller re-deriving the machine.
		inline constexpr size_t ROM_OBJECT = 0x90;

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

		// machine+0xA0 is a DIFFERENT object from MEM_OBJECT above, and the two are easy to
		// conflate: this is the CM3Mem the machine build constructs with FUN_180013D90 and stores at
		// +0xA0, i.e. the guest ADDRESS DECODE, while +0xA8 is the one whose bank descriptors the
		// canary reads. The frame step's own interrupt raise goes through +0xA0
		// (DLL 0x18003B1B0, `mov rcx,[rbx+0xA0]` then vtable +0x98), which is what pins it.
		//
		// +0x36 on it is the Model 3 INTERRUPT STATE byte: the 0xF0 window's sub-address 0x18
		// returns it verbatim (DLL 0x18001355A), so it is exactly what the guest polls at
		// 0xF0100018 and dispatches its ISRs from.
		inline constexpr size_t MEM_DECODE = 0xA0;
		inline constexpr size_t IRQ_RAISE_SLOT = 0x98 / 8;
		// THE guest RAM base - the module's own DAT_18062AA98, the one its memory handlers use.
		// Same constant HleHooks.cpp calls RAM_PTR, and it carries the same warning: a different
		// plausible base reads zeroes at addresses that provably hold code. BoardGuestRam validates
		// whatever it gets against a known-fixed instruction before returning it.
		inline constexpr uintptr_t RVA_RAM_PTR = 0x62AA98;

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

		// Filled by SetBoardSymbols before module_start; null until then, and null for good if the
		// pattern did not resolve - in which case Object() falls back to RVA_MACHINE.
		static uint8_t* importedMachine = nullptr;
		static const void* importedCommVtable = nullptr;

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
			// The RESOLVED address wins. The RVA below is right for the build GameVerify pins and
			// silently wrong for any other, which is the whole reason the pattern exists.
			if (importedMachine != nullptr) return importedMachine;
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

	void SetBoardSymbols(void* machineObject, const void* cxcommVtable)
	{
		Machine::importedMachine = static_cast<uint8_t*>(machineObject);
		Machine::importedCommVtable = cxcommVtable;

		const uint8_t* base = Machine::ModuleBase();
		const void* rvaMachine = (base != nullptr) ? base + Machine::RVA_MACHINE : nullptr;

		if (machineObject == nullptr)
		{
			DebugLogFile("[%s] TaskM3E did not resolve by pattern - falling back to the RVA (%p). "
				"Correct for the pinned build, wrong for any other.\n",
				gGeneral.GetGameTag(), rvaMachine);
		}
		else if (machineObject != rvaMachine)
		{
			// Available AND disagreeing is the case the hardcoded RVA could never report: the
			// module's layout has moved, and every offset expressed against it is now suspect.
			DebugLogFile("[%s] TaskM3E resolved to %p but the RVA says %p - the module layout has "
				"MOVED. Using the resolved address.\n",
				gGeneral.GetGameTag(), machineObject, rvaMachine);
		}
		else
		{
			DebugLogFile("[%s] board symbols resolved: TaskM3E %p, CXComm vftable %p\n",
				gGeneral.GetGameTag(), machineObject, cxcommVtable);
		}
	}

	const void* BoardGuestRam()
	{
		// VALIDATED AGAINST A KNOWN-FIXED VALUE BEFORE IT IS RETURNED, which is not optional here.
		// The identical mistake is already written up in docs/src2-hle-hooks.md: a plausible-looking
		// base (CM3Mem+0x18 that time, the canary's bank descriptor this time) reads ZEROES
		// everywhere, including at addresses that provably hold code, and a probe on it reports
		// "this counter never moves" about counters that move every frame. It cost a run there and
		// it cost one here.
		//
		// Guest 0x0189EC must decode to 0x4806302D - SRC2's `bl 0x07BA18`, the instruction HLE hook
		// 1 deletes. Any base that does not satisfy that is refused rather than returned.
		constexpr uintptr_t KNOWN_ADDR = 0x0189EC;
		constexpr uint32_t KNOWN_WORD = 0x4806302D;

		const auto validated = [](const uint8_t* base) -> const uint8_t*
		{
			if (base == nullptr) return nullptr;
			__try
			{
				return (*reinterpret_cast<const uint32_t*>(base + KNOWN_ADDR) == KNOWN_WORD)
					? base : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		};

		// The base the module's OWN memory handlers use - its DAT_18062AA98. This is the one that
		// is right; the bank descriptor below is kept only as a fallback because the canary already
		// reaches RAM that way and a future build could move the global.
		if (const uint8_t* base = Machine::ModuleBase())
		{
			if (const auto* ram = *reinterpret_cast<const uint8_t* const*>(base + Machine::RVA_RAM_PTR))
			{
				if (const uint8_t* ok = validated(ram)) return ok;
			}
		}

		const uint8_t* machine = Machine::Running();
		if (machine == nullptr) return nullptr;
		const auto* mem = *reinterpret_cast<uint8_t* const*>(machine + Machine::MEM_OBJECT);
		if (mem == nullptr) return nullptr;
		const auto* desc = *reinterpret_cast<uint8_t* const* const*>(mem + Machine::MEM_BANK0_DESC);
		if (desc == nullptr) return nullptr;
		return validated(*reinterpret_cast<const uint8_t* const*>(desc));
	}

	const void* BoardCommVtable()
	{
		return Machine::importedCommVtable;
	}

	int BoardPhase()
	{
		const uint8_t* machine = Machine::Object();
		if (machine == nullptr) return -1;
		return *reinterpret_cast<const int*>(machine + Machine::BOOT_STATE);
	}

	void* BoardMemObject()
	{
		uint8_t* machine = Machine::Running();
		if (machine == nullptr) return nullptr;
		return *reinterpret_cast<void* const*>(machine + Machine::MEM_DECODE);
	}

	void* BoardRomObject()
	{
		uint8_t* machine = Machine::Running();
		if (machine == nullptr) return nullptr;
		return *reinterpret_cast<void* const*>(machine + Machine::ROM_OBJECT);
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

	uint32_t StateCheckCombine(uint32_t cpu, uint32_t ram)
	{
		// FNV-1a's own combining step, so the value on the wire is exactly what one pass over both
		// regions would have produced - the split is a diagnostic, not a different canary.
		const uint32_t h = net::Fnv1aWord(net::Fnv1aWord(net::FNV1A_BASIS, cpu), ram);
		// A hash that happened to land on zero would read as "no canary" to the caller.
		return h != 0 ? h : 1u;
	}

	uint32_t StateCheckValue()
	{
		uint32_t cpu = 0, ram = 0;
		if (!StateCheckParts(cpu, ram))
		{
			return 0;
		}
		return StateCheckCombine(cpu, ram);
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
			uint32_t h = net::FNV1A_BASIS;
			if (const auto* cpuObject =
				*reinterpret_cast<uint8_t* const*>(base + Machine::RVA_CPU_PTR))
			{
				const uint8_t* regs = cpuObject + Machine::CPU_REGISTER_FILE;
				for (size_t off = Machine::REGS_FIRST; off < Machine::REGS_LAST; off += 4)
				{
					h = net::Fnv1aWord(h, *reinterpret_cast<const uint32_t*>(regs + off));
				}
			}
			cpu = h;

			h = net::FNV1A_BASIS;
			for (size_t off = 0; off < Machine::RAM_SIZE; off += Machine::HASH_STRIDE)
			{
				h = net::Fnv1aWord(h, *reinterpret_cast<const uint32_t*>(ramBase + off));
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
		// The clock has to be pinnable AND pinned. Availability alone is not enough: the redirect
		// is installed for both games but only pinned for one, and an unpinned board boots
		// differently on every machine - which is precisely the divergence a round cannot recover
		// from. Stated as a condition rather than as a comment so that whitelisting another game
		// for netplay without also making its clock pinnable fails closed.
		return gGeneral.GetGameId() == YAMPGeneral::GameId::FV2
			&& DeterministicClockAvailable() && ClockPinBootSafe();
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
