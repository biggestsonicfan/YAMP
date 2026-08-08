#include "HleHooks.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cstdio>

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
			// The interpreter's register file, as every HLE handler addresses it (the module's
			// own DAT_18052AA88). Layout, read off those handlers: +0x08 current PC, +0x0C next
			// PC, +0x10 + 4n GPRn (so r3 = +0x1C), +0x98 + 8n FPRn, +0x1A0 the link register.
			inline constexpr uintptr_t REG_PTR = 0x52AA88;
			// The CM3Mem object (the module's DAT_18052AA80).
			inline constexpr uintptr_t MEM_PTR = 0x52AA80;
			// THE guest RAM base - the module's DAT_18062AA98, which is what its own HLE handlers
			// use for direct guest memory: the SRC2 settings injector writes bytes at
			// `(guestAddr ^ 3) + DAT_18062AA98` and dwords at `DAT_18062AA98 + 0xC4550`.
			//
			// NOT CM3Mem+0x18, which is a different base and reads as zeroes at addresses that
			// provably hold code. That mistake produced a whole run of confident, wrong readings
			// before a peek at a known-fixed address (0x0189EC, which must be 0x4806302D) caught
			// it - which is why the validation is now written down here rather than assumed.
			inline constexpr uintptr_t RAM_PTR = 0x62AA98;
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

		// ---- Sega Racing Classic 2 -------------------------------------------------------
		//
		// Table index 12 (M3ERomSrc2 + 0x20600, set by the ROM factory at DLL 0x180038C59), which
		// is the table at DLL 0x180108A10: 26 records. Full derivation in docs/src2-hle-hooks.md.
		//
		// SRC2 HAS NO Speed HOOKS AT ALL - not one of the 26 is a native reimplementation of a
		// ROM routine, where FV2 has twenty. Its whole table is behaviour: eight deleted
		// instructions, thirteen surgical register/branch patches, four host callbacks and a
		// single idle-loop cut-out. So the "turn off every native routine" preset has nothing to
		// offer here, and disabling anything in this list changes what the board DOES.
		//
		// Register-file offsets in the notes are the ones the handlers use, relative to cpu+8:
		// r3 = +0x1C, r16 = +0x50, f0 = +0x98, f1 = +0xA0, LR = +0x1A0, next PC = +0x0C.
		//
		// Five of the handlers (0x2CAA0, 0x2CAD0, 0x2CB00, 0x2CB10, and 0x2C640/0x2CA40 which
		// SRC2 does NOT use) are exclusive to this family; the rest of the 0x2C6A0-0x2CB20 block
		// is shared with hook tables 10 and 11, the two Daytona USA 2 variants the ROM factory
		// never builds. That is a useful sanity check: SRC2 is the only D2-derived game Gaiden
		// actually ships.
		static constexpr Info SRC2_HOOKS[] = {
			{ 0x04CA40, 0x26780, Kind::Core,    "idle-loop cut-out: yields the rest of the CPU's timeslice, then runs the original instruction. SRC2 has ONE of these where FV2 has three" },
			// HOOKS 1 AND 2 ARE A MATCHED PAIR that excises the board's SECURITY-CHIP overlay -
			// 464 bytes of XOR-0x98 obfuscated PowerPC that talks to the Model 3 security board
			// at 0xF0180000/0xF01A0000 (via the 0xFE1x mirror) and executes what it reads back.
			//
			// The module removes it because pre3's security registers are a STUB THAT LIES:
			// M3ERomSrc2 vtable slot 2 (read) is `return 0` and slot 3 (write) is a bare `ret`,
			// so the overlay's busy-wait clears on its first poll, its 0x411A8 timeout never
			// fires, and it reports SUCCESS with a buffer full of zeros. The game ships a
			// perfectly good failure path - on timeout it jumps to a fallback the loader stashes
			// at 0x55DF00 - and the stub is what makes that path unreachable.
			//
			// Read the two together; docs/src2-hle-hooks.md has the full disassembly.
			{ 0x0189EC, 0x01DC0, Kind::Removed, "deletes `bl 0x07BA18` in the board's init chain - the LOADER that XOR-0x98 deobfuscates a 464-byte overlay from 0x0D98DC to 0x55D000 and relocates it to 0x55D800. BOOT-CRITICAL: disable this alone and the board never reaches frame 1" },
			{ 0x018B28, 0x01DC0, Kind::Removed, "deletes `bla 0x55D800` - the call INTO that overlay. Load-bearing only because hook 1 removed the loader, so 0x55D800 is all zeros: disable this alone and the board calls into empty memory and freezes on its 8th draw (still exactly 8 at frames 600, 1200 and 1800)" },
			{ 0x09A3FC, 0x01DC0, Kind::Removed, "deletes the PER-FRAME `bla 0x55D800` in the game's main loop (FUN_0009A39C) - the third call into the same security overlay hooks 1 and 2 excise. Harmless to disable, unlike that pair, and confirmed so: decrypting the 464-byte blob shows a pure device-transfer routine (byte-swapped stores to 0xFE180000, status poll at 0xFE1A001C) that holds no game state" },
			{ 0x006754, 0x267E0, Kind::Host,    "whole routine replaced: appends r3 to a growable u32 array on the ROM object (data at rom+0x488, capacity +0x490, count +0x494), then returns to the link register. Same handler FV2 uses at its three sites" },
			{ 0x007480, 0x2C6A0, Kind::Host,    "arcade settings injection: writes the coin setting (free play -> 0x1B), region, difficulty and the linked-cabinet fields from the YAMP config into game RAM at 0x72629C, runs the original instruction (`lis r16, 0x73`), then stores 50.0f to guest 0xC4550, 0xC4590 and 0xC4610. BOOT-CRITICAL: disable it and the board never reaches frame 1 - SRC2 does not merely prefer configured settings, it will not start without them" },
			{ 0x01922C, 0x2C6E0, Kind::Patch,   "forces r3 = 0, then runs the original instruction" },
			{ 0x073CC4, 0x2C720, Kind::Patch,   "f0 += 0.05, f1 -= 0.05, then runs the original instruction" },
			{ 0x073CFC, 0x2C780, Kind::Patch,   "f0 += 0.02, f1 += 0.0, then runs the original instruction" },
			{ 0x073D70, 0x2C7E0, Kind::Patch,   "f0 += 0.16, f1 -= 0.1, then runs the original instruction" },
			{ 0x073DFC, 0x2C840, Kind::Patch,   "f0 += 0.17, f1 -= 0.1, then runs the original instruction" },
			{ 0x073E34, 0x2C8A0, Kind::Patch,   "f0 += 0.15, f1 -= 0.1, then runs the original instruction" },
			{ 0x069C60, 0x2C900, Kind::Patch,   "f0 += 0.0, f1 -= 0.1, then runs the original instruction" },
			{ 0x02D2F0, 0x2C960, Kind::Host,    "THE CPU CARS. Multiplies by the float at rom+0x374, then runs the original - which is `stfs f1, 0x1e4(r15)`, the store of the MOTION-INTEGRATION SCALAR at guest 0x1051E4 (guest 0x08DC98 multiplies all three velocity components by it before adding them to position). rom+0x374 is execute_info+0x1684, which YAMP used to fill with FV2's button-assign bytes - a float denormal ~1e-36 - so the scalar was zeroed and every AI car slowed to a stop a few seconds into a race. Host now writes 1.0f there (pre3_execute_info_t::src2_scalars), which makes this hook the identity" },
			{ 0x036E2C, 0x2C9D0, Kind::Host,    "runs the original instruction FIRST, then multiplies r3 by the float at rom+0x378 = execute_info+0x1688 - same story as hook 13, and the value it scales feeds `DAT_00105010 += DAT_00106254`, a bonus-time/score accumulator. Also 1.0f now, so also the identity" },
			{ 0x07BC4C, 0x2CB20, Kind::Host,    "settings-blob upload: when the host config's +0x07 flag is set, copies the 0x1000 bytes at config +0x08 into guest RAM at 0x2000 and clears the flag. YAMP leaves both zero, so it does nothing today. Then runs the original instruction" },
			// HOOKS 16-21 ARE THE BOOT PRESENTATION. Disabling all six brings back the start-up
			// WARNING screen and the Daytona 2 TITLE LOGO (measured as a group, user-confirmed).
			// Which hook belongs to which screen is read off the two routines they sit in, NOT
			// yet split by measurement - two runs with 0x10000 and 0x1E0000 would settle it.
			// Gaiden hides them because its emulator is a minigame inside a menu; YAMP is the
			// cabinet, so the board's own power-up sequence is the authentic behaviour - the same
			// argument DefaultDisableMask already makes for FV2's hooks 7 and 10.
			{ 0x091660, 0x2CAA0, Kind::Patch,   "forces a conditional branch ALWAYS TAKEN: next PC = PC + (sign-extended low halfword of the instruction, low two bits cleared). Sits in FUN_00091600, which draws message ids 0x1BC and 0x5A8 at locate(0,10,2) / locate(0x38,0x11,2) once the screen timer passes 0x11C and 0x54 - i.e. the START-UP WARNING SCREEN" },
			{ 0x091800, 0x2CAA0, Kind::Patch,   "forces a conditional branch always taken (second site). This one and the three below are in FUN_000917C0 - a 3D sequence that walks a table of position triples, draws ids 0x6D6/0x6D7 and indexes a 32-entry curve by the screen timer with a fade under it: the DAYTONA 2 TITLE LOGO" },
			{ 0x09185C, 0x01DC0, Kind::Removed, "instruction deleted (title logo sequence)" },
			{ 0x0918A4, 0x01DC0, Kind::Removed, "instruction deleted (title logo sequence)" },
			{ 0x0918D0, 0x01DC0, Kind::Removed, "instruction deleted (title logo sequence)" },
			{ 0x06A4C4, 0x2CAD0, Kind::Patch,   "forces a branch-to-link-register always taken - a conditional return made unconditional. Same boot-presentation cluster: the screen sequencer calls this routine at 0x91560, immediately before setting the screen timer to -1 and incrementing the screen index at r15+4, so it is plausibly the skip/advance check. Role unread" },
			{ 0x0420B8, 0x2CB00, Kind::Patch,   "instruction replaced by r16 = 0x53B; the original never runs" },
			{ 0x042134, 0x2CB10, Kind::Patch,   "instruction replaced by r3 = 0x53B; the original never runs" },
			{ 0x01B834, 0x01DC0, Kind::Removed, "instruction deleted" },
			{ 0x01B854, 0x01DC0, Kind::Removed, "instruction deleted" },
		};
		static_assert(sizeof(SRC2_HOOKS) / sizeof(SRC2_HOOKS[0]) <= MAX_COUNT);

		// The three SRC2 hooks the board cannot start without, from the one-at-a-time sweep
		// (400 frames each against a 676-draw baseline, the failures re-confirmed at 2000):
		//
		//   1  deletes `bl 0x07BA18`  - never reaches frame 1. Diagnosed: re-enabling this call
		//                               alone re-enters boot-phase frame-sync code (guest
		//                               0x001E78) after its device-address table at 0x0BB918 has
		//                               been reused, so it kicks a bogus address and spins on a
		//                               status bit that can never clear.
		//   2  deletes `bla 0x55D800` - freezes on its 8th draw, calling into empty memory.
		//   5  arcade settings         - never reaches frame 1. SRC2 will not start without its
		//                               settings written into game RAM.
		//
		// FV2 has NO entry here on purpose: the sweep has not been run on it, and guessing from
		// SRC2's indices would be the same class of error as handing FV2's default mask to SRC2.
		static constexpr size_t SRC2_BOOT_CRITICAL[] = { 1, 2, 5 };

		static const Info* CurrentTable(size_t& count)
		{
			switch (gGeneral.GetGameId())
			{
			case YAMPGeneral::GameId::FV2:
				count = sizeof(FV2_HOOKS) / sizeof(FV2_HOOKS[0]);
				return FV2_HOOKS;
			case YAMPGeneral::GameId::SRC2:
				count = sizeof(SRC2_HOOKS) / sizeof(SRC2_HOOKS[0]);
				return SRC2_HOOKS;
			default:
				break;
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

		bool BootCritical(size_t index)
		{
			if (gGeneral.GetGameId() != YAMPGeneral::GameId::SRC2) return false;
			for (const size_t critical : SRC2_BOOT_CRITICAL)
			{
				if (critical == index) return true;
			}
			return false;
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

		// ---- guest-code dump -------------------------------------------------------------
		//
		// The decoded trace is the ONLY practical way to read this board's PowerPC. The hook
		// addresses are RAM addresses the board has copied its program into, and eprom.bin in
		// image/src2.par is the raw EPROM set - a different interleave with no simple mapping to
		// them, so disassembling the archive does not answer "what instruction sits at 0x018B28".
		// The trace does: the pre-decoder writes {originalWord, handlerIndex} for EVERY word of
		// the region, and the original word is kept precisely so a hook can be undone. Reading it
		// back costs nothing and needs no debugger.
		//
		// Env-gated rather than a command-line flag, matching YAMP_PRE3_DIAG:
		//   YAMP_PRE3_DUMP=18B28,7480:40   - comma-separated <hexAddr>[:<wordCount>], default 64.
		//
		// SELF-TIMING, and it has to be. On the first frame the table verifies, every word in the
		// trace is still ZERO: the board has not yet copied its program into RAM, so the
		// pre-decoder has not run over this region. A dump there prints nothing but zeroes and
		// handler indices that Update itself has just written. So it retries each frame and only
		// fires once the FIRST requested address has a non-zero word, i.e. once that region has
		// actually been decoded.
		//
		// (The same emptiness is why Update reconciles every frame rather than once - see the
		// note at its loop. Writing a natural index derived from a zero word is harmless only
		// because the pre-decoder overwrites the whole entry when it finally runs.)
		static void DumpGuestCode(const TraceEntry* trace, uint32_t entries)
		{
			static bool done = false;
			if (done) return;

			char spec[256];
			size_t specLen = 0;
			if (getenv_s(&specLen, spec, sizeof(spec), "YAMP_PRE3_DUMP") != 0 || specLen == 0)
			{
				done = true;
				return;
			}

			{
				const unsigned long first = strtoul(spec, nullptr, 16);
				const uint32_t index = static_cast<uint32_t>(first) / 4;
				if (index >= entries || trace[index].word == 0) return;   // not decoded yet
			}
			done = true;

			for (const char* p = spec; *p != '\0'; )
			{
				char* end = nullptr;
				const unsigned long addr = strtoul(p, &end, 16);
				if (end == p) break;
				unsigned long words = 64;
				if (*end == ':')
				{
					words = strtoul(end + 1, &end, 10);
				}
				DebugLogFile("[%s dump] guest 0x%06lX, %lu words\n",
					gGeneral.GetGameTag(), addr, words);
				for (unsigned long i = 0; i < words; i++)
				{
					const uint32_t at = static_cast<uint32_t>(addr) + i * 4;
					const uint32_t index = at / 4;
					if (index >= entries) break;
					DebugLogFile("[%s dump] %06X  %08X  idx=%04X\n", gGeneral.GetGameTag(),
						at, trace[index].word, trace[index].handler_index);
				}
				p = (*end == ',') ? end + 1 : end;
				while (*p == ',') p++;
			}
		}

		// ---- whole-region image dump -----------------------------------------------------
		//
		// Reconstructs the board's ENTIRE code region as a flat big-endian image, straight out of
		// the decoded trace, so it can be loaded into a real disassembler:
		//
		//   YAMP_PRE3_DUMPBIN=src2_guest.bin
		//
		// Import into Ghidra as a RAW BINARY, language PowerPC:BE:32:default, base address 0 -
		// the trace is indexed by guestAddress/4, so byte i of this file IS guest address i, and
		// no rebasing is needed. Auto-analysis then gives functions, xrefs and a decompiler over
		// the guest, which is a different class of tool from disassembling 200-word windows.
		//
		// Two honest caveats. The trace is a snapshot of DECODE time, not live RAM: regions the
		// board writes later (the security overlay's output buffer at 0x55D800 is the one that
		// matters here) read as zero. And a hooked address holds the ORIGINAL word - which is the
		// point, since that is the instruction the module replaced.
		static void DumpGuestImage(const TraceEntry* trace, uint32_t entries)
		{
			static bool done = false;
			if (done) return;

			char path[MAX_PATH];
			size_t pathLen = 0;
			if (getenv_s(&pathLen, path, sizeof(path), "YAMP_PRE3_DUMPBIN") != 0 || pathLen == 0)
			{
				done = true;
				return;
			}
			// Same self-timing rule as DumpGuestCode, for the same reason - on the first verified
			// frame the whole trace is still zero. Anchor on a hook site, which is by definition
			// a real instruction in this game's code.
			size_t count = 0;
			const Info* table = CurrentTable(count);
			if (table == nullptr || count == 0) { done = true; return; }
			const uint32_t anchor = table[0].guestAddress / 4;
			if (anchor >= entries || trace[anchor].word == 0) return;
			done = true;

			FILE* f = nullptr;
			if (fopen_s(&f, path, "wb") != 0 || f == nullptr)
			{
				DebugLogFile("[%s dump] could not open %s\n", gGeneral.GetGameTag(), path);
				return;
			}
			for (uint32_t i = 0; i < entries; i++)
			{
				const uint32_t w = trace[i].word;
				const unsigned char be[4] = {
					static_cast<unsigned char>(w >> 24), static_cast<unsigned char>(w >> 16),
					static_cast<unsigned char>(w >> 8),  static_cast<unsigned char>(w)
				};
				fwrite(be, 1, 4, f);
			}
			fclose(f);
			DebugLogFile("[%s dump] wrote %s - %u bytes of guest code, base 0, PowerPC BE 32\n",
				gGeneral.GetGameTag(), path, entries * 4);
		}

		// ---- guest PC probe --------------------------------------------------------------
		//
		// "The board does not advance" and "the board is spinning at address X" look identical
		// from the host: the frame loop runs at full speed either way, because the emulator has
		// its own thread and never stops handing back frames. The only way to tell them apart is
		// to look at the guest's own program counter.
		//
		//   YAMP_PRE3_PC=1     - log PC and LR every frame
		//   YAMP_PRE3_PC=30    - ...every 30th frame
		//
		// A guest stuck in a tight loop prints the same one or two addresses forever, and that
		// address is directly disassemblable with YAMP_PRE3_DUMP.
		static void SampleGuestPc(const uint8_t* base)
		{
			char spec[32];
			size_t specLen = 0;
			if (getenv_s(&specLen, spec, sizeof(spec), "YAMP_PRE3_PC") != 0 || specLen == 0)
			{
				return;
			}
			const unsigned long every = (std::max)(1UL, strtoul(spec, nullptr, 10));

			const auto* regs = *reinterpret_cast<const uint8_t* const*>(base + rva::REG_PTR);
			if (regs == nullptr) return;

			static unsigned long frame = 0;
			if (frame++ % every != 0) return;

			// LIVE guest RAM, on the same cadence:
			//   YAMP_PRE3_PEEK=BB910:8   - <hexGuestAddr>[:<wordCount>]
			//
			// Distinct from YAMP_PRE3_DUMP and needed alongside it: the trace holds what the
			// PRE-DECODER saw, so it answers "what instruction is at X" but not "what does the
			// board's data at X say NOW". Anything the board writes after decode - tables it
			// fills in, buffers it populates - is invisible there and visible here.
			//
			// Guest word at A is the host dword at RAM_PTR + A: the emulator keeps RAM
			// word-swizzled, which is why BYTE access in the module's handlers goes through
			// `addr ^ 3` while dword access does not.
			{
				char peek[128];
				size_t peekLen = 0;
				if (getenv_s(&peekLen, peek, sizeof(peek), "YAMP_PRE3_PEEK") == 0 && peekLen != 0)
				{
					const auto* ram = *reinterpret_cast<const uint8_t* const*>(base + rva::RAM_PTR);
					if (ram != nullptr)
					{
						char* end = nullptr;
						const unsigned long at = strtoul(peek, &end, 16);
						const unsigned long words = (*end == ':') ? strtoul(end + 1, nullptr, 10) : 4;
						for (unsigned long i = 0; i < words; i++)
						{
							DebugLogFile("[%s peek] %06lX = %08X\n", gGeneral.GetGameTag(),
								at + i * 4,
								*reinterpret_cast<const uint32_t*>(ram + at + i * 4));
						}
					}
				}
			}

			// r3..r6 rather than r3 alone: the interesting spins on this board poll a device
			// address held in a register, and the address is the whole question.
			DebugLogFile("[%s pc] frame=%lu pc=%06X next=%06X lr=%06X r3=%08X r4=%08X r5=%08X r6=%08X\n",
				gGeneral.GetGameTag(), frame - 1,
				*reinterpret_cast<const uint32_t*>(regs + 0x08),
				*reinterpret_cast<const uint32_t*>(regs + 0x0C),
				*reinterpret_cast<const uint32_t*>(regs + 0x1A0),
				*reinterpret_cast<const uint32_t*>(regs + 0x1C),
				*reinterpret_cast<const uint32_t*>(regs + 0x20),
				*reinterpret_cast<const uint32_t*>(regs + 0x24),
				*reinterpret_cast<const uint32_t*>(regs + 0x28));
		}

		bool MaskStripBootCritical(uint64_t mask[2])
		{
			// Deliberate escape hatch for diagnosis: YAMP_PRE3_ALLOW_CRITICAL=1 lets a
			// boot-critical hook actually stay disabled, ini and all.
			//
			// It exists because the strip is otherwise INVISIBLE and silently invalidates any
			// experiment on these three hooks - a settings.ini asking for hook 1 off simply comes
			// back with it on, and a run that looks like a clean bisect is really the default
			// mask. That cost a whole round of measurements here before it was spotted. Nothing
			// in the UI sets this; it is for the person holding a debugger.
			static const bool allow = []
			{
				char v[8];
				size_t n = 0;
				return getenv_s(&n, v, sizeof(v), "YAMP_PRE3_ALLOW_CRITICAL") == 0 && n != 0
					&& v[0] != '0';
			}();
			if (allow) return false;

			bool changed = false;
			const size_t count = Count();
			for (size_t i = 0; i < count; i++)
			{
				if (BootCritical(i) && MaskTest(mask, i))
				{
					MaskSet(mask, i, false);
					changed = true;
				}
			}
			return changed;
		}

		const uint64_t* DefaultDisableMask()
		{
			// PER-GAME, and it has to be: the indices below are per-title. Handing FV2's mask to
			// SRC2 would silently patch the wrong instructions in a different game.
			//
			// THIS IS ALSO THE NETPLAY ORACLE. A session forces every peer onto this value and
			// ignores settings.ini entirely (see YAMPUserInterface's Update call), because the
			// mask changes what the board DOES - Patch hooks rewrite instructions and Removed
			// hooks delete them - so two machines running different masks are running different
			// games. Anything added here therefore has to be right for a match, not just pleasant
			// for a single player.
			static constexpr uint64_t NONE[2] = { 0, 0 };

			if (gGeneral.GetGameId() == YAMPGeneral::GameId::SRC2)
			{
				// SEGA RACING CLASSIC 2, established 2026-08-08 by bisecting the live table - which
				// only became testable once the game was drivable at all (its ADC ring had never
				// been filled; see pre3::SetDrivingAxes). Three groups, all user-confirmed on
				// screen:
				//
				//   3       the per-frame `bla 0x55D800` into the security overlay. Hooks 1 and 2
				//           already excise that overlay's loader and boot call and are BOOT-
				//           CRITICAL, so the third call has to go with them - left enabled it
				//           calls into memory the loader never filled.
				//   13, 14  the two `rom+0x374` / `+0x378` multiplies. Hook 13 zeroes the board's
				//           MOTION-INTEGRATION SCALAR (guest 0x1051E4), which is what froze every
				//           CPU car a few seconds into a race; 14 does the same to a bonus-time
				//           accumulator. Redundant with the 1.0f the host now writes into those
				//           fields (pre3_execute_info_t::src2_scalars) - the two are behaviourally
				//           identical, and both are kept because the scalar is the right VALUE
				//           whatever the mask says, and the mask is the thing a match agrees on.
				//   16-21   the boot presentation: the start-up WARNING screen and the Daytona 2
				//           title logo. Same argument as FV2's pair below - Gaiden hides them
				//           because its emulator is a minigame inside a menu, and YAMP is the
				//           cabinet.
				//
				// NOT hooks 1, 2 or 5: those three are the ones the board cannot start without.
				static constexpr uint64_t SRC2_DEFAULT[2] = {
					(1ull << 3) | (1ull << 13) | (1ull << 14)
					| (1ull << 16) | (1ull << 17) | (1ull << 18)
					| (1ull << 19) | (1ull << 20) | (1ull << 21),
					0
				};
				static_assert(SRC2_DEFAULT[0] == 0x3F6008);
				return SRC2_DEFAULT;
			}

			if (gGeneral.GetGameId() != YAMPGeneral::GameId::FV2)
			{
				return NONE;
			}

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

			DumpGuestCode(trace, entries);
			DumpGuestImage(trace, entries);
			SampleGuestPc(base);
			s_live = true;
		}
	}
}
