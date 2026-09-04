#include "Patch.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"
#include "../Utils/ScopedUnprotect.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace pre3
{
	namespace TilemapPatch
	{
		// CM3Mem's address decode: 17 records of {topByte, read8, write8, read16, write16, read32,
		// write32}, stride 0x38. The slot ORDER is not assumed - it was read off the 0xC0 entry,
		// whose six handlers tail-call the device's vtable at +0x40, +0x48, +0x50, +0x58, +0x60 and
		// +0x68 in exactly that sequence.
		inline constexpr uintptr_t RVA_WINDOW_TABLE = 0x1077C0;
		inline constexpr size_t WINDOW_COUNT = 17;
		inline constexpr size_t WINDOW_STRIDE = 7;   // in qwords
		inline constexpr uint64_t WINDOW_TOP_TILEMAP = 0xF1;

		// The tilemap RAM the 32-bit accessors serve. Past this the window is video REGISTERS, and
		// those keep the module's own behaviour - see Patch.h.
		inline constexpr uint32_t TILEMAP_LIMIT = 0xF1120000;

		using read8_fn = uint8_t (*)(uint32_t address);
		using write8_fn = void (*)(uint32_t address, uint8_t value);
		using read16_fn = uint16_t (*)(uint32_t address);
		using write16_fn = void (*)(uint32_t address, uint16_t value);
		using read32_fn = uint32_t (*)(uint32_t address);
		using write32_fn = void (*)(uint32_t address, uint32_t value);

		static read8_fn orgRead8 = nullptr;
		static write8_fn orgWrite8 = nullptr;
		static read16_fn orgRead16 = nullptr;
		static write16_fn orgWrite16 = nullptr;
		static read32_fn orgRead32 = nullptr;
		static write32_fn orgWrite32 = nullptr;

		static uint64_t narrowReads = 0;
		static uint64_t narrowWrites = 0;
		// The WIDE path is counted too, and it is the more informative number: if the board never
		// writes tilemap RAM at ANY width, the text is not being lost in this window at all and
		// the search moves elsewhere.
		static uint64_t wideReads = 0;
		static uint64_t wideWrites = 0;
		static uint64_t registerWrites = 0;
		static uint32_t firstWriteAddr = 0;
		static uint32_t lastWriteAddr = 0;

		// Both 32-bit accessors bswap, so what they carry is the guest's word value. The byte at
		// guest address A therefore occupies bits [24 - 8*(A & 3)] of the word at A & ~3, and the
		// halfword bits [16 - 8*(A & 2)].
		static unsigned ByteShift(uint32_t address) { return 24u - 8u * (address & 3u); }
		static unsigned HalfShift(uint32_t address) { return 16u - 8u * (address & 2u); }

		static uint8_t Read8(uint32_t address)
		{
			if (address >= TILEMAP_LIMIT || orgRead32 == nullptr) return orgRead8(address);
			narrowReads++;
			const uint32_t word = orgRead32(address & ~3u);
			return static_cast<uint8_t>((word >> ByteShift(address)) & 0xFFu);
		}

		static void Write8(uint32_t address, uint8_t value)
		{
			if (address >= TILEMAP_LIMIT || orgRead32 == nullptr || orgWrite32 == nullptr)
			{
				orgWrite8(address, value);
				return;
			}
			narrowWrites++;
			const uint32_t aligned = address & ~3u;
			const unsigned shift = ByteShift(address);
			const uint32_t word = (orgRead32(aligned) & ~(0xFFu << shift))
				| (static_cast<uint32_t>(value) << shift);
			orgWrite32(aligned, word);
		}

		static uint16_t Read16(uint32_t address)
		{
			if (address >= TILEMAP_LIMIT || orgRead32 == nullptr) return orgRead16(address);
			narrowReads++;
			const uint32_t word = orgRead32(address & ~3u);
			return static_cast<uint16_t>((word >> HalfShift(address)) & 0xFFFFu);
		}

		static void Write16(uint32_t address, uint16_t value)
		{
			if (address >= TILEMAP_LIMIT || orgRead32 == nullptr || orgWrite32 == nullptr)
			{
				orgWrite16(address, value);
				return;
			}
			narrowWrites++;
			const uint32_t aligned = address & ~3u;
			const unsigned shift = HalfShift(address);
			const uint32_t word = (orgRead32(aligned) & ~(0xFFFFu << shift))
				| (static_cast<uint32_t>(value) << shift);
			orgWrite32(aligned, word);
		}

		// Counted pass-throughs. They exist only to answer "is this window used at all", which the
		// narrow counters cannot: a board that writes tilemap RAM entirely with word stores would
		// show zero narrow accesses and be indistinguishable from one that never touches it.
		static uint32_t Read32(uint32_t address)
		{
			wideReads++;
			return orgRead32(address);
		}

		static void Write32(uint32_t address, uint32_t value)
		{
			// SPLIT BY RANGE, because the totals alone were misleading: a first/last pair that
			// both landed on video registers said nothing about whether tilemap RAM was touched
			// in between, and tilemap RAM is the whole question.
			if (address < TILEMAP_LIMIT)
			{
				if (wideWrites == 0) firstWriteAddr = address;
				lastWriteAddr = address;
				wideWrites++;
			}
			else
			{
				registerWrites++;
			}
			orgWrite32(address, value);
		}

		static bool Wanted()
		{
			char value[8];
			size_t length = 0;
			if (getenv_s(&length, value, sizeof(value), "YAMP_PRE3_TILEMAP") != 0 || length == 0)
			{
				return true;   // on by default: discarding a write is not a behaviour worth keeping
			}
			return value[0] != '0';
		}
	}

	void InstallTilemapAccess()
	{
		using namespace TilemapPatch;

		if (!Wanted()) return;

		const auto* base = reinterpret_cast<const uint8_t*>(
			GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
		if (base == nullptr) return;

		auto* table = reinterpret_cast<uint64_t*>(
			const_cast<uint8_t*>(base) + RVA_WINDOW_TABLE);

		// FOUND BY ITS TOP BYTE, not by a hardcoded entry index. The table is ordered by the
		// module's own layout and an index would be one more constant to be silently wrong about;
		// the top byte is the thing being looked for.
		uint64_t* entry = nullptr;
		for (size_t i = 0; i < WINDOW_COUNT; ++i)
		{
			uint64_t* candidate = table + i * WINDOW_STRIDE;
			if (candidate[0] == WINDOW_TOP_TILEMAP)
			{
				entry = candidate;
				break;
			}
		}
		if (entry == nullptr)
		{
			DebugLog("[%s] no 0xF1 window in the CM3Mem table - tilemap accessors NOT patched.\n",
				gGeneral.GetGameTag());
			return;
		}

		orgRead8 = reinterpret_cast<read8_fn>(entry[1]);
		orgWrite8 = reinterpret_cast<write8_fn>(entry[2]);
		orgRead16 = reinterpret_cast<read16_fn>(entry[3]);
		orgWrite16 = reinterpret_cast<write16_fn>(entry[4]);
		orgRead32 = reinterpret_cast<read32_fn>(entry[5]);
		orgWrite32 = reinterpret_cast<write32_fn>(entry[6]);

		// SELF-CHECK. The narrow slots are expected to be the two stubs and the guard thunk; if
		// this build implements them for real, replacing them would be a regression rather than a
		// fix, and the right move is to do nothing and say so.
		const bool narrowWritesAreOneThunk =
			reinterpret_cast<void*>(orgWrite8) == reinterpret_cast<void*>(orgWrite16);
		if (orgRead32 == nullptr || orgWrite32 == nullptr || !narrowWritesAreOneThunk)
		{
			DebugLog("[%s] 0xF1 window does not look like the stubbed layout "
				"(w8=%p w16=%p r32=%p) - tilemap accessors NOT patched.\n",
				gGeneral.GetGameTag(), reinterpret_cast<void*>(orgWrite8),
				reinterpret_cast<void*>(orgWrite16), reinterpret_cast<void*>(orgRead32));
			return;
		}

		{
			// The table is in .rdata.
			const auto unprotect = ScopedUnprotect::Section(GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"),
				".rdata");
			entry[1] = reinterpret_cast<uint64_t>(&Read8);
			entry[2] = reinterpret_cast<uint64_t>(&Write8);
			entry[3] = reinterpret_cast<uint64_t>(&Read16);
			entry[4] = reinterpret_cast<uint64_t>(&Write16);
			entry[5] = reinterpret_cast<uint64_t>(&Read32);
			entry[6] = reinterpret_cast<uint64_t>(&Write32);
		}

		DebugLogFile("[%s] tilemap narrow accessors installed over the 0xF1 window "
			"(r32=%p w32=%p). 8/16-bit writes were being discarded.\n",
			gGeneral.GetGameTag(), reinterpret_cast<void*>(orgRead32),
			reinterpret_cast<void*>(orgWrite32));
	}

	namespace BootRender
	{
		// FOUND BY SCANNING .rdata FOR THE PREDICATE, not by naming a vtable.
		//
		// The first version hardcoded M3ERomSrc2's vftable (0x18010C3F0, slot 42 at +0x150) which
		// is right for SRC2 and useless for anything else - each of the module's ROM classes has
		// its own vtable and they all inherit this same accessor. Scanning for every qword in
		// .rdata that equals FUN_180037950 patches slot 42 of ALL of them in one pass, needs no
		// per-game RVA, and cannot mis-target: the only thing a vtable slot holding that address
		// can be IS this predicate.
		inline constexpr uintptr_t RVA_GATE_FN = 0x37950;

		using gate_fn = char (*)(void* rom, void* arg);
		static gate_fn original = nullptr;
		static uint64_t forced = 0;
		static uint64_t passed = 0;

		// The guest frame counter, as the original predicate would have read it - so the log can
		// say how many frames were forced rather than just that the patch is installed.
		static char Hook(void* rom, void* arg)
		{
			const char real = original != nullptr ? original(rom, arg) : 1;
			if (real != '\0') { passed++; return real; }
			forced++;
			return 1;
		}
	}

	void InstallBootRender()
	{
		using namespace BootRender;

		// DEFAULT IS SRC2 ONLY, and that is a scope decision rather than a technical one. The
		// counter this overrides is Daytona 2's; whether Fighting Vipers 2 hides its boot screens
		// the same way is a separate measurement, so it takes an explicit YAMP_PRE3_BOOTRENDER=1
		// rather than arriving as a side effect of a fix for another game.
		//
		//   unset -> SRC2 only      0 -> off everywhere      1 -> every pre3 game
		char value[8];
		size_t length = 0;
		const bool haveEnv = getenv_s(&length, value, sizeof(value), "YAMP_PRE3_BOOTRENDER") == 0
			&& length != 0;
		if (haveEnv && value[0] == '0')
		{
			DebugLogFile("[%s] boot-render override disabled by YAMP_PRE3_BOOTRENDER=0\n",
				gGeneral.GetGameTag());
			return;
		}
		if (!(haveEnv && value[0] == '1')
			&& gGeneral.GetGameId() != YAMPGeneral::GameId::SRC2)
		{
			return;
		}

		const auto base = reinterpret_cast<uintptr_t>(
			GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
		if (base == 0) return;

		// Walk the PE headers for .rdata rather than assuming its bounds.
		const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto* section = IMAGE_FIRST_SECTION(nt);
		const uint64_t expected = base + RVA_GATE_FN;
		unsigned patched = 0;

		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			if (memcmp(section->Name, ".rdata", 6) != 0) continue;

			auto* const start = reinterpret_cast<uint64_t*>(base + section->VirtualAddress);
			const size_t count = section->Misc.VirtualSize / sizeof(uint64_t);
			for (size_t at = 0; at < count; ++at)
			{
				if (start[at] != expected) continue;
				start[at] = reinterpret_cast<uint64_t>(&Hook);
				patched++;
			}
		}

		if (patched == 0)
		{
			// Refused rather than forced. No slot holding the predicate means this is not the
			// build the override was written against, and there is nothing safe to guess at.
			DebugLogFile("[%s] boot-render override REFUSED: no .rdata slot holds %p "
				"(FUN_180037950). Not patching.\n",
				gGeneral.GetGameTag(), reinterpret_cast<void*>(expected));
			return;
		}

		original = reinterpret_cast<gate_fn>(expected);
		DebugLogFile("[%s] boot-render override installed in %u ROM vtable(s): the scene submit "
			"no longer waits for the game's frame counter at guest 0x737978\n",
			gGeneral.GetGameTag(), patched);
	}

	void LogBootRender()
	{
		using namespace BootRender;
		if (original == nullptr) return;
		DebugLogFile("[%s] boot-render override: %llu frames forced, %llu the game asked for "
			"itself\n",
			gGeneral.GetGameTag(), static_cast<unsigned long long>(forced),
			static_cast<unsigned long long>(passed));
	}

	namespace ScrollProbe
	{
		// Both resolved against the pinned build, like the rest of this file. GameVerify has
		// already refused any other module.
		inline constexpr uintptr_t RVA_SCR_PTR = 0x189680;     // CScr* (a pointer variable)
		inline constexpr uintptr_t RVA_BOARD_STATE = 0x189658;  // 0x10 once the board is running

		// THE GATE OVER THE WHOLE SCENE SUBMIT, tilemap included. FUN_18003AC20 runs only when
		// the board state is 0x10 AND the ROM object's vtable slot 42 (+0x150) returns non-zero,
		// and that slot is `FUN_180037950: return *(int *)(this + 0x205B8) != 0`. Everything the
		// module draws - the Real3D scene, FUN_180014DC0's tilemap render, and the blit of the
		// tilemap texture - is behind it.
		//
		// The ROM object is the machine's +0x90, mirrored to this global; its vftable for SRC2 is
		// M3ERomSrc2 at 0x18010C3F0 (verified: +0x118 is FUN_180037A90, the HLE table index
		// accessor docs/src2-hle-hooks.md already pins).
		inline constexpr uintptr_t RVA_ROM_PTR = 0x189660;
		inline constexpr size_t OFF_ROM_RENDER_GATE = 0x205B8;
		inline constexpr size_t OFF_ROM_MODE = 0x205BC;   // slot 43 tests (u8)(this[0x205BC]-2) < 6

		inline constexpr size_t OFF_LIVE_REGS = 0x88;
		inline constexpr size_t OFF_LATCHED_REGS = 0x90;
		inline constexpr size_t OFF_DIRTY = 0x98;
		inline constexpr size_t OFF_RING_COUNT = 0x9C;
		inline constexpr size_t OFF_PRODUCER = 0xA0;
		inline constexpr size_t OFF_CONSUMER = 0xA4;
		inline constexpr size_t OFF_TEX0 = 0x100;
		inline constexpr size_t OFF_TEX1 = 0x104;
		inline constexpr size_t OFF_LAYER_MASK = 0x110;
		inline constexpr size_t OFF_CONTROL = 0x38;   // within either register struct

		static uint64_t lastKey = ~0ull;
		static uint64_t frame = 0;
		static uint64_t uploads = 0;
		static bool announced = false;
		static uint8_t preDirty = 0xFF;
		static uint32_t preProducer = 0;
		static uint32_t preConsumer = 0;

		// The shadow is the guest's tilemap VRAM as the module keeps it: 0x000000-0x0FFFFF is
		// tilemap RAM (character data low, the four name tables high) and 0x100000-0x11FFFF is
		// the palette. Guest halfwords are stored big-endian, so a tile code has to be byte
		// swapped to read.
		inline constexpr size_t SHADOW_PALETTE = 0x100000;
		inline constexpr size_t SHADOW_SIZE = 0x120000;
		inline constexpr size_t NAME_TABLE_LOW = 0xF0000;

		// A guest HALFWORD out of the shadow, and the swizzle is `offset ^ 2` and NOTHING ELSE.
		// Both wide accessors bswap, so the host dword at a guest word address already holds the
		// guest's value as an integer; the guest's HIGH halfword is therefore the host's upper
		// two bytes, which is the whole of the correction. Bytes within the halfword are already
		// in host order.
		//
		// Worth the comment because both wrong answers are readable. With no swizzle the text
		// dumps as "T_IH_SSIM" - every adjacent pair transposed. With the swizzle AND a byte swap
		// it dumps in the right ORDER but every code shifted a byte (0x5400 for 'T'), which looks
		// like a plausible "tile number 0x400, palette 5" encoding rather than the bare ASCII it
		// actually is. The palette read has the same trap and no text to catch it with.
		static uint16_t GuestHalf(const uint8_t* shadow, size_t offset)
		{
			return *reinterpret_cast<const uint16_t*>(shadow + (offset ^ 2u));
		}

		static void DumpShadow(const uint8_t* scr, uint64_t frame)
		{
			const auto* const regs = *reinterpret_cast<const uint8_t* const*>(scr + OFF_LIVE_REGS);
			if (regs == nullptr) return;
			const auto* const shadow = *reinterpret_cast<const uint8_t* const*>(regs);
			if (shadow == nullptr) return;

			// Per-0x2000 census over the name-table half. A name table is 64x48 halfwords =
			// 0x1800, so a block that holds text is a block with a few hundred non-zero entries -
			// and the census locates it without assuming which of the four layers the BIOS uses.
			size_t bestBlock = 0;
			unsigned bestCount = 0;
			char census[256];
			int used = 0;
			for (size_t block = NAME_TABLE_LOW; block < SHADOW_PALETTE; block += 0x2000)
			{
				unsigned count = 0;
				for (size_t i = 0; i < 0x2000; i += 2)
				{
					if (*reinterpret_cast<const uint16_t*>(shadow + block + i) != 0) count++;
				}
				if (count > bestCount) { bestCount = count; bestBlock = block; }
				if (used < static_cast<int>(sizeof(census)) - 16)
				{
					used += _snprintf_s(census + used, sizeof(census) - used, _TRUNCATE,
						"%s%u", used != 0 ? "/" : "", count);
				}
			}

			// THE PALETTE, WITH INDICES. The name-table entries turn out to be bare ASCII with
			// zero attribute bits, so the font draws out of palette BANK 0 - and a bank 0 of
			// black is black text on a black screen, which is indistinguishable from a layer
			// that was never drawn. So bank 0 is printed verbatim rather than counted.
			unsigned paletteNonZero = 0;
			unsigned firstIndex[6] = {};
			uint16_t firstValue[6] = {};
			unsigned sampled = 0;
			const unsigned entries = static_cast<unsigned>((SHADOW_SIZE - SHADOW_PALETTE) / 2);
			for (unsigned i = 0; i < entries; ++i)
			{
				const uint16_t entry = GuestHalf(shadow, SHADOW_PALETTE + static_cast<size_t>(i) * 2);
				if (entry == 0) continue;
				paletteNonZero++;
				if (sampled < 6) { firstIndex[sampled] = i; firstValue[sampled] = entry; sampled++; }
			}

			char bank0[16 * 5 + 8];
			int bankAt = 0;
			for (unsigned i = 0; i < 16; ++i)
			{
				bankAt += _snprintf_s(bank0 + bankAt, sizeof(bank0) - bankAt, _TRUNCATE, "%04X ",
					GuestHalf(shadow, SHADOW_PALETTE + static_cast<size_t>(i) * 2));
			}

			DebugLogFile("[%s scroll] shadow dump frame=%llu base=%p  name-table census "
				"0xF0000+ [%s]  palette non-zero %u/%u first@ %u=%04X %u=%04X %u=%04X\n"
				"[%s scroll]   palette bank 0 (what a zero-attribute tile draws with): %s\n",
				gGeneral.GetGameTag(), static_cast<unsigned long long>(frame),
				static_cast<const void*>(shadow), census, paletteNonZero, entries,
				firstIndex[0], firstValue[0], firstIndex[1], firstValue[1],
				firstIndex[2], firstValue[2],
				gGeneral.GetGameTag(), bank0);

			if (bestCount == 0) return;

			// THE ROWS THE ROM ACTUALLY WRITES TO. FUN_00093DB4 calls locate(0x13, 0x2C) for the
			// mode string and locate(0x0F, 0x26) / locate(0x0F, 0x28) for the messages, so those
			// three rows are where the network text has to be if it is anywhere. A row of one
			// repeated value is blank; text is a run of distinct codes.
			//
			// All four name tables, because which layer the BIOS text service uses is not known
			// and guessing it is how the last three theories in this file went wrong.
			// ALL 48 ROWS, not the three the network check happens to use. Narrowing to those was
			// right while the question was "is the network text there"; it is the wrong shape for
			// "does this screen say anything at all", which is what a hook bisect needs to see.
			// Flat rows are skipped below, so a blank screen still costs nothing.
			static constexpr size_t NAME_TABLES[4] = { 0xF8000, 0xFA000, 0xFC000, 0xFE000 };
			for (size_t table : NAME_TABLES)
			{
				for (int textRow = 0; textRow < 48; ++textRow)
				{
					const size_t rowOffset = table + static_cast<size_t>(textRow) * 64 * 2;
					if (rowOffset + 128 > SHADOW_PALETTE) continue;

					// Columns 8..47, which covers every x the check's locate calls use.
					// Printed as TEXT as well as codes: the entries are bare ASCII, so the row
					// reads directly and there is no interpretation step to get wrong.
					char row[40 * 5 + 8];
					char text[48];
					int at = 0;
					unsigned distinct = 0;
					uint16_t previous = 0xFFFF;
					for (int column = 8; column < 48; ++column)
					{
						const uint16_t code = GuestHalf(
							shadow, rowOffset + static_cast<size_t>(column) * 2);
						if (code != previous) { distinct++; previous = code; }
						at += _snprintf_s(row + at, sizeof(row) - at, _TRUNCATE, "%04X ", code);
						const auto ch = static_cast<char>(code & 0xFF);
						text[column - 8] = (ch >= 0x20 && ch < 0x7F) ? ch : '.';
					}
					text[40] = '\0';
					// Only the rows that are NOT a flat fill: a blank row says nothing and four
					// tables x three rows of blanks is noise that would bury the one that matters.
					if (distinct <= 1) continue;
					DebugLogFile("[%s scroll]   nt 0x%zX row 0x%02X: \"%s\"\n     %s\n",
						gGeneral.GetGameTag(), table, textRow, text, row);
				}
			}
		}

		static const uint8_t* Object()
		{
			const auto* const base = reinterpret_cast<const uint8_t*>(
				GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
			if (base == nullptr) return nullptr;
			return *reinterpret_cast<const uint8_t* const*>(base + RVA_SCR_PTR);
		}
	}

	void SampleTilemapPipelinePreFrame()
	{
		using namespace ScrollProbe;
		const uint8_t* const scr = Object();
		if (scr == nullptr) return;
		preDirty = scr[OFF_DIRTY];
		preProducer = *reinterpret_cast<const uint32_t*>(scr + OFF_PRODUCER);
		preConsumer = *reinterpret_cast<const uint32_t*>(scr + OFF_CONSUMER);
	}

	void LogTilemapPipeline()
	{
		using namespace ScrollProbe;

		const auto* const base = reinterpret_cast<const uint8_t*>(
			GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
		if (base == nullptr) return;

		frame++;

		const auto* const scr = *reinterpret_cast<const uint8_t* const*>(base + RVA_SCR_PTR);
		if (scr == nullptr) return;

		const auto u32 = [](const uint8_t* p, size_t off)
		{
			return *reinterpret_cast<const uint32_t*>(p + off);
		};
		const auto ptr = [](const uint8_t* p, size_t off)
		{
			return *reinterpret_cast<const uint8_t* const*>(p + off);
		};

		const uint32_t producer = u32(scr, OFF_PRODUCER);
		const uint32_t consumer = u32(scr, OFF_CONSUMER);
		const uint8_t dirty = scr[OFF_DIRTY];
		const uint32_t mask = u32(scr, OFF_LAYER_MASK);
		const uint8_t* const live = ptr(scr, OFF_LIVE_REGS);
		const uint8_t* const latched = ptr(scr, OFF_LATCHED_REGS);
		const uint32_t liveCtrl = live != nullptr ? u32(live, OFF_CONTROL) : 0;
		const uint32_t latchedCtrl = latched != nullptr ? u32(latched, OFF_CONTROL) : 0;

		const auto* const rom = *reinterpret_cast<const uint8_t* const*>(base + RVA_ROM_PTR);
		const uint32_t gate = rom != nullptr ? u32(rom, OFF_ROM_RENDER_GATE) : 0;
		const uint8_t romMode = rom != nullptr ? rom[OFF_ROM_MODE] : 0;

		if (!announced)
		{
			announced = true;
			DebugLogFile("[%s scroll] CScr=%p ring=%u textures=%u/%u board=0x%X\n",
				gGeneral.GetGameTag(), static_cast<const void*>(scr), u32(scr, OFF_RING_COUNT),
				u32(scr, OFF_TEX0), u32(scr, OFF_TEX1),
				*reinterpret_cast<const int*>(base + RVA_BOARD_STATE));
		}

		// COUNT THE PRODUCER ADVANCING, not producer != consumer. The first version tested the
		// latter, which is render_begin's own condition and is therefore ALWAYS false by the time
		// the host looks - render_begin equalises them inside the frame - so it read zero on a
		// pipeline that was working perfectly and printed a confident verdict saying so.
		static uint32_t s_lastProducer = ~0u;
		if (producer != s_lastProducer)
		{
			s_lastProducer = producer;
			uploads++;
		}

		const uint64_t key = static_cast<uint64_t>(producer) | static_cast<uint64_t>(consumer) << 16
			| static_cast<uint64_t>(dirty) << 32 | static_cast<uint64_t>(mask) << 40
			| static_cast<uint64_t>(preDirty) << 44
			| static_cast<uint64_t>(liveCtrl & 0xFFFF) << 48
			| static_cast<uint64_t>(gate != 0 ? 1 : 0) << 63
			| static_cast<uint64_t>(romMode) << 55;
		if (key == lastKey && frame % 600 != 0) return;
		lastKey = key;

		// IS THE TEXT ACTUALLY IN THE BUFFER THE GPU IS FED? Three one-shot dumps of the CPU-side
		// 0x120000 shadow - the exact bytes render_begin uploads - because "the ROM composed the
		// text" and "the text is in the buffer the GPU reads" are different claims and only the
		// second one matters here. Bounded: three frames, a per-block census and one row.
		if (frame == 200 || frame == 400 || frame == 700) DumpShadow(scr, frame);

		DebugLogFile("[%s scroll] frame=%llu producer=%u->%u consumer=%u->%u dirty pre=%u post=%u "
			"uploads=%llu mask=0x%X ctrl live=0x%08X latched=0x%08X gate=%u mode=%u%s\n",
			gGeneral.GetGameTag(), static_cast<unsigned long long>(frame),
			preProducer, producer, preConsumer, consumer, preDirty, dirty,
			static_cast<unsigned long long>(uploads), mask, liveCtrl, latchedCtrl, gate, romMode,
			(frame > 300 && uploads == 0)
				? "  <== THE PRODUCER NEVER MOVES: no upload, no dirty flag, no tilemap render"
				: (frame > 300 && dirty != 0)
					? "  <== DIRTY IS NEVER CONSUMED: FUN_180014DC0 is not running"
					: "");
	}

	void LogTilemapAccess()
	{
		using namespace TilemapPatch;
		if (orgRead32 == nullptr) return;
		DebugLogFile("[%s] tilemap window: narrow %llu reads / %llu writes, "
			"wide %llu reads / TILEMAP %llu writes (first=%08X last=%08X) / registers %llu writes%s\n",
			gGeneral.GetGameTag(),
			static_cast<unsigned long long>(narrowReads),
			static_cast<unsigned long long>(narrowWrites),
			static_cast<unsigned long long>(wideReads),
			static_cast<unsigned long long>(wideWrites),
			firstWriteAddr, lastWriteAddr, static_cast<unsigned long long>(registerWrites),
			(narrowWrites == 0 && wideWrites == 0)
				? "  <== THE BOARD NEVER WRITES THIS WINDOW AT ALL"
				: (narrowWrites == 0 ? "  <== no narrow use; the narrow patch fixes nothing" : ""));
	}
}
