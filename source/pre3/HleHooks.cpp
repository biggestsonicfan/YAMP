#include "HleHooks.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>

namespace pre3
{
	namespace HleHooks
	{
		// ---- The module's own globals ----------------------------------------------------
		//
		// RVAs rather than byte patterns, on the same justification the pxd system-shader loader
		// in Pre3Host.cpp uses: GameVerify has already pinned this exact build by SHA-256 before
		// any of this runs, and these are DATA - there is no code to pattern-match. Every one was
		// read out of the functions named beside it.
		namespace rva
		{
			// Written by CPpc603e::reset (DLL 0x180030840) with the CPU object, so it is null
			// until the board has been built.
			inline constexpr uintptr_t CPU_PTR = 0x62AA90;
			// The 19 per-game hook tables (DLL 0x1800307C5 indexes this).
			inline constexpr uintptr_t HOOK_TABLES = 0x1084A0;
		}

		// Offsets inside the CPpc603e object, all from CPpc603e::init (DLL 0x1800306B0) and the
		// pre-decoder (DLL 0x180015E50).
		namespace cpu
		{
			inline constexpr size_t REGION_SIZE = 0x2F0;  // bytes of guest code the trace covers
			inline constexpr size_t TRACE_ARRAY = 0x2F8;  // {u32 word, u32 handlerIndex}[]
			inline constexpr size_t HOOK_TABLE = 0x300;   // the table init selected
		}

		// One decoded instruction. The interpreter reads these 8 bytes and calls
		// dispatch[handler_index]; `word` is the instruction the pre-decoder saw, kept so a
		// handler can still get at it - and so a hook can be undone.
		struct TraceEntry
		{
			uint32_t word;
			uint32_t handler_index;
		};

		// One record of a per-game hook table.
		struct Record
		{
			uint32_t guest_address;
			uint32_t pad;
			void* handler;
		};

		// Primary opcode 1 - unassigned on PowerPC, which is why the module can use its 2048
		// dispatch slots for hooks. Hook i lives at index HOOK_INDEX_BASE + i.
		inline constexpr uint32_t HOOK_INDEX_BASE = 0x800;

		// What the pre-decoder computes for an ordinary instruction: primary opcode into bits
		// 11..16, extended opcode in bits 0..10.
		static uint32_t NaturalIndex(uint32_t word)
		{
			return ((word >> 15) & 0x1F800u) | (word & 0x7FFu);
		}

		// ---- Fighting Vipers 2 -----------------------------------------------------------
		//
		// Table index 3 (M3ERomFv2 + 0x5A8, set by the ROM factory at DLL 0x180038720), which is
		// the table at DLL 0x180108FE0: 36 records.
		//
		// There are no ROM symbols for this board - nothing in the module names a single guest
		// address, and unlike Sonic the Fighters there is no symbolised disassembly to borrow
		// from. So the notes below describe what each HANDLER does, which is knowable exactly,
		// rather than guessing at what the ROM routine was called. Five handler shapes cover the
		// whole table and give the Kind column its meaning:
		//
		//   0x26780  burn the rest of the CPU timeslice, then run the original instruction
		//   0x267E0  call the board object (r3 passed through), then return to the link register
		//   0x26B30  set r4 = 2, then run the original instruction
		//   0x19080  return to the link register immediately - the routine is stubbed out
		//   0x01DC0  a bare `ret` - the instruction is simply deleted
		//
		// Everything from 0x28390 up is a large native routine ending in the link-register
		// return, i.e. a whole ROM function reimplemented in x64. The byte size in each note is
		// that handler's own size, which is the honest measure of how much work it replaces.
		static constexpr Info FV2_HOOKS[] = {
			{ 0x00A218, 0x26780, Kind::Core,    "idle-loop cut-out: yields the rest of the CPU's timeslice, then runs the original instruction" },
			{ 0x009F4C, 0x26780, Kind::Core,    "idle-loop cut-out (second site)" },
			{ 0x009FB4, 0x26780, Kind::Core,    "idle-loop cut-out (third site)" },
			{ 0x0073F8, 0x267E0, Kind::Host,    "routine replaced by a call into the board object with r3, then return" },
			{ 0x007428, 0x267E0, Kind::Host,    "routine replaced by the same board call (second site)" },
			{ 0x007480, 0x267E0, Kind::Host,    "routine replaced by the same board call (third site)" },
			{ 0x00ABF4, 0x26B30, Kind::Patch,   "forces r4 = 2, then runs the original instruction" },
			{ 0x0380E4, 0x19080, Kind::Removed, "routine stubbed out - the handler returns to the link register and does nothing else. WITH HOOK 10 THIS IS THE START-UP WARNING SCREEN: disable both and the board plays it, which is what YAMP ships" },
			{ 0x00B330, 0x28150, Kind::Patch,   "executes the instruction word minus 0x20 instead of the one in the ROM" },
			{ 0x0567B4, 0x01DC0, Kind::Removed, "instruction deleted" },
			{ 0x0567E8, 0x01DC0, Kind::Removed, "instruction deleted. WITH HOOK 7 THIS IS THE START-UP WARNING SCREEN - see hook 7" },
			{ 0x038B8C, 0x28280, Kind::Host,    "fills a guest table at 0x214942 from the halfword at 0x21495E" },
			{ 0x038B84, 0x28180, Kind::Host,    "arcade settings injection: writes the coin setting (free play -> 0x1B) and the difficulty from the YAMP config into the game's own RAM at 0x1205xx" },
			{ 0x02B434, 0x28350, Kind::Patch,   "executes the instruction with its low nibble decremented" },
			{ 0x02BE34, 0x28350, Kind::Patch,   "executes the instruction with its low nibble decremented" },
			{ 0x02C558, 0x28350, Kind::Patch,   "executes the instruction with its low nibble decremented" },
			{ 0x022D60, 0x28390, Kind::Speed,   "native routine, 1276 bytes - heavy double-precision maths over the FPU file" },
			{ 0x022C34, 0x28890, Kind::Speed,   "native routine, 597 bytes - guest memory walk" },
			{ 0x022360, 0x28AF0, Kind::Speed,   "native routine, 339 bytes - square root over guest floats" },
			{ 0x022C70, 0x28C50, Kind::Speed,   "native routine, 1851 bytes - the largest FPU block in the table" },
			{ 0x00266C, 0x29390, Kind::Speed,   "native routine, 343 bytes - FPU only" },
			{ 0x0026A0, 0x294F0, Kind::Speed,   "native routine, 307 bytes - FPU only" },
			{ 0x0026D4, 0x29630, Kind::Speed,   "native routine, 355 bytes - FPU only" },
			{ 0x001FC0, 0x297A0, Kind::Speed,   "native routine, 897 bytes - mixed integer and FPU" },
			{ 0x00201C, 0x29B30, Kind::Speed,   "native routine, 826 bytes - mixed integer and FPU" },
			{ 0x008B04, 0x29E70, Kind::Speed,   "native routine, 617 bytes" },
			{ 0x008BFC, 0x2A0E0, Kind::Speed,   "native routine, 629 bytes" },
			{ 0x002F60, 0x2A360, Kind::Speed,   "native routine, 260 bytes" },
			{ 0x002754, 0x2A470, Kind::Speed,   "native routine, 331 bytes" },
			{ 0x002ECC, 0x2A5C0, Kind::Speed,   "native routine, 405 bytes" },
			{ 0x002E18, 0x2A760, Kind::Speed,   "native routine, 378 bytes" },
			{ 0x002E64, 0x2A8E0, Kind::Speed,   "native routine, 1358 bytes" },
			{ 0x0584A0, 0x2AE30, Kind::Speed,   "native routine, 1976 bytes - the largest handler in the table" },
			{ 0x058914, 0x2B5F0, Kind::Speed,   "native routine, 950 bytes" },
			{ 0x058804, 0x2B9B0, Kind::Speed,   "native routine, 680 bytes" },
			{ 0x058850, 0x2BC60, Kind::Speed,   "native routine, 529 bytes" },
		};
		static_assert(sizeof(FV2_HOOKS) / sizeof(FV2_HOOKS[0]) <= MAX_COUNT);

		// Sega Rally 2 uses a different table (M3ERomSrc2 + 0x20600 selects it) which has not
		// been enumerated, so its panel stays hidden rather than showing FV2's addresses.
		static const Info* CurrentTable(size_t& count)
		{
			if (gGeneral.GetGameId() == YAMPGeneral::GameId::FV2)
			{
				count = sizeof(FV2_HOOKS) / sizeof(FV2_HOOKS[0]);
				return FV2_HOOKS;
			}
			count = 0;
			return nullptr;
		}

		static bool s_live = false;
		static size_t s_liveRecords = 0;
		// Set once, when the live table has been checked against the descriptions above.
		static bool s_verified = false;
		static bool s_mismatch = false;

		static const uint8_t* ModuleBase()
		{
			return reinterpret_cast<const uint8_t*>(GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
		}

		size_t Count()
		{
			size_t count = 0;
			CurrentTable(count);
			return count;
		}

		bool Supported() { return Count() != 0; }
		bool Live() { return s_live; }
		size_t LiveRecordCount() { return s_liveRecords; }

		const Info& Get(size_t index)
		{
			size_t count = 0;
			const Info* table = CurrentTable(count);
			if (table == nullptr)
			{
				static constexpr Info EMPTY{ 0, 0, Kind::Core, "" };
				return EMPTY;
			}
			return table[index < count ? index : 0];
		}

		const char* KindName(Kind kind)
		{
			switch (kind)
			{
			case Kind::Core:    return "Core";
			case Kind::Host:    return "Host";
			case Kind::Speed:   return "Speed";
			case Kind::Patch:   return "Patch";
			case Kind::Removed: return "Removed";
			}
			return "?";
		}

		const char* KindDescription(Kind kind)
		{
			switch (kind)
			{
			case Kind::Core:
				return "Emulator plumbing: the handler spots the board spinning and gives the rest of\n"
					"the CPU's timeslice back. Disabling one is safe but costs frame rate - the board\n"
					"spins out the whole slice instead.";
			case Kind::Host:
				return "Host integration: arcade settings from YAMP's own config written into the game's\n"
					"RAM, a table clear, and the routines that reach the host through the board object.\n"
					"Disabling one boots, but loses whatever it provided.";
			case Kind::Speed:
				return "A whole ROM routine reimplemented in native x64 - the handler returns to the link\n"
					"register, so the ROM's own body never runs. Disabling one hands the work back to\n"
					"the interpreted PowerPC code: slower, and otherwise identical. This is the way to\n"
					"test whether one of them is wrong.";
			case Kind::Patch:
				return "The handler alters the instruction word before executing it, or pokes a register\n"
					"and then lets the original run. Small deliberate changes to the ROM's behaviour.";
			case Kind::Removed:
				return "The ROM's code is deleted here - either the one instruction, or the whole routine\n"
					"when the handler returns to the link register straight away.";
			}
			return "";
		}

		bool MaskTest(const uint64_t mask[2], size_t index)
		{
			return index < 128 && (mask[index >> 6] & (1ull << (index & 63))) != 0;
		}

		void MaskSet(uint64_t mask[2], size_t index, bool disabled)
		{
			if (index >= 128) return;
			const uint64_t bit = 1ull << (index & 63);
			if (disabled) mask[index >> 6] |= bit;
			else          mask[index >> 6] &= ~bit;
		}

		void MaskForKinds(uint64_t mask[2], unsigned kinds)
		{
			mask[0] = mask[1] = 0;
			const size_t count = Count();
			for (size_t i = 0; i < count; i++)
			{
				if ((kinds & KindBit(Get(i).kind)) != 0)
				{
					MaskSet(mask, i, true);
				}
			}
		}

		bool MaskStripKinds(uint64_t mask[2], unsigned kinds)
		{
			bool changed = false;
			const size_t count = Count();
			for (size_t i = 0; i < count; i++)
			{
				if ((kinds & KindBit(Get(i).kind)) != 0 && MaskTest(mask, i))
				{
					MaskSet(mask, i, false);
					changed = true;
				}
			}
			return changed;
		}

		const uint64_t* DefaultDisableMask()
		{
			// THE START-UP WARNING SCREEN PLAYS BY DEFAULT, which means hooks 7 and 10 ship
			// disabled. Between them they skip the screen every Sega arcade board shows on
			// power-up. Like a Dragon Gaiden wants it gone - there the emulator is a minigame
			// inside a menu, and nobody wants a hardware warning in front of it. YAMP is the
			// cabinet, so the board's own power-up sequence is the authentic behaviour.
			//
			// A CORRECTION WORTH KEEPING, because the measurement that produced the wrong answer
			// looked sound. This was first derived from the board's own game-mode byte over a
			// 900-frame headless run: mode 01 (start-up) holds for one sample with every hook on,
			// three with hook 6 off and four with 9+10 off, which reads exactly like a screen
			// being shown for longer - so 6, 9 and 10 were named as the trio. They are not; the
			// pair is 7 and 10, established by WATCHING THE SCREEN. Two lessons. The mode byte
			// does not distinguish "the screen is up" from "the board is between states", so it
			// cannot answer a question about what is displayed. And a 900-frame window is shorter
			// than the thing being measured - the screen has a real dwell time, so "it has not
			// advanced yet" and "it will never advance" look identical inside it.
			//
			// NB an existing settings.ini carries an explicit mask, and an explicit value beats a
			// default - so a profile written before this keeps the screen skipped until "Restore
			// defaults" is pressed.
			static constexpr uint64_t DEFAULT[2] = { (1ull << 7) | (1ull << 10), 0 };
			return DEFAULT;
		}

		// Checks the module's live table against the descriptions once. If the addresses and
		// handlers do not line up exactly, the descriptions belong to some other build and acting
		// on them would disable the wrong instructions - so Update stops instead.
		static bool Verify(const uint8_t* base, const Record* records, size_t liveCount)
		{
			size_t count = 0;
			const Info* table = CurrentTable(count);
			if (table == nullptr) return false;
			if (liveCount != count) return false;
			for (size_t i = 0; i < count; i++)
			{
				if (records[i].guest_address != table[i].guestAddress) return false;
				if (records[i].handler != base + table[i].handlerRva) return false;
			}
			return true;
		}

		void Update(const uint64_t mask[2])
		{
			s_live = false;

			size_t count = 0;
			const Info* table = CurrentTable(count);
			if (table == nullptr) return;

			const uint8_t* const base = ModuleBase();
			if (base == nullptr) return;

			auto* const cpuObject = *reinterpret_cast<uint8_t* const*>(base + rva::CPU_PTR);
			if (cpuObject == nullptr) return;   // board not built yet

			const auto* records = *reinterpret_cast<const Record* const*>(cpuObject + cpu::HOOK_TABLE);
			auto* const trace = *reinterpret_cast<TraceEntry* const*>(cpuObject + cpu::TRACE_ARRAY);
			const uint32_t regionSize = *reinterpret_cast<const uint32_t*>(cpuObject + cpu::REGION_SIZE);
			if (records == nullptr || trace == nullptr || regionSize == 0) return;

			// The table is null-handler terminated, exactly as CPpc603e::init walks it.
			size_t liveCount = 0;
			while (liveCount < MAX_COUNT && records[liveCount].handler != nullptr)
			{
				liveCount++;
			}
			s_liveRecords = liveCount;

			if (!s_verified)
			{
				s_verified = true;
				s_mismatch = !Verify(base, records, liveCount);
				if (s_mismatch)
				{
					DebugLogFile("[%s hle] live table (%zu records) does not match the %zu described "
						"hooks - leaving them alone\n", gGeneral.GetGameTag(), liveCount, count);
				}
				else
				{
					DebugLogFile("[%s hle] %zu hooks verified against the module's own table\n",
						gGeneral.GetGameTag(), count);
				}
			}
			if (s_mismatch) return;

			// Reconciled EVERY frame rather than once, and the reason is measurable: the board
			// re-runs the pre-decoder after it has copied its code into RAM, which puts every
			// hook back. A one-shot apply is silently undone a few frames into the boot.
			const uint32_t entries = regionSize / 4;
			for (size_t i = 0; i < count; i++)
			{
				const uint32_t word_index = table[i].guestAddress / 4;
				if (word_index >= entries) continue;

				TraceEntry& entry = trace[word_index];
				const uint32_t wanted = MaskTest(mask, i)
					? NaturalIndex(entry.word)
					: static_cast<uint32_t>(HOOK_INDEX_BASE + i);
				if (entry.handler_index != wanted)
				{
					entry.handler_index = wanted;
				}
			}
			s_live = true;
		}
	}
}
