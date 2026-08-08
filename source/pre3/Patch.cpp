#include "Patch.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"
#include "../Utils/ScopedUnprotect.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstdlib>

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
			ScopedUnprotect::Section unprotect(GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"),
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
