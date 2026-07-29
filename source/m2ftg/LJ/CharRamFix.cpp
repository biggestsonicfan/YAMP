#include "CharRamFix.h"

#include "../../YAMPGeneral.h"
#include "../../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
	// The module's own memory-map dispatch table: 64 records of 0x70 at
	// DLL+0x172660, six {read, write} pointer pairs by access width. Region 0x10 covers guest
	// 0x1000000..0x10FFFFF (tilemap / palette / char RAM). The table lives in .rdata.
	constexpr uintptr_t RVA_MEMMAP_TBL = 0x172660;
	constexpr size_t RECORD_SIZE = 0x70;
	constexpr size_t REGION = 0x10;

	constexpr uint32_t CHAR_LO = 0x1080000;
	constexpr uint32_t CHAR_HI = 0x1100000;

	using ReadFn = void(*)(void* out, uint32_t address);
	using WriteFn = void(*)(uint32_t address, const void* source);

	ReadFn g_originalRead8 = nullptr;
	WriteFn g_originalWrite8 = nullptr;
	bool g_installed = false;

	inline uint8_t NibbleSwap(uint8_t v)
	{
		return static_cast<uint8_t>((v << 4) | (v >> 4));
	}

	// The w16 ingest transform, restated per byte: internal[i] = nibswap(guest[i^1]). A byte
	// write to guest offset g therefore belongs at the handler's offset g^1, nibble-swapped.
	inline uint32_t LaneAddress(uint32_t address)
	{
		return CHAR_LO + ((address - CHAR_LO) ^ 1u);
	}

	void Write8Fixed(uint32_t address, const void* source)
	{
		if (address >= CHAR_LO && address < CHAR_HI)
		{
			const uint8_t swapped = NibbleSwap(*static_cast<const uint8_t*>(source));
			g_originalWrite8(LaneAddress(address), &swapped);
			return;
		}
		g_originalWrite8(address, source);
	}

	// tfb_putpixel is a read-modify-write; without the inverse here the guest would read back
	// transformed bytes and re-fold the transform into its own data.
	void Read8Fixed(void* out, uint32_t address)
	{
		if (address >= CHAR_LO && address < CHAR_HI)
		{
			uint8_t raw = 0;
			g_originalRead8(&raw, LaneAddress(address));
			*static_cast<uint8_t*>(out) = NibbleSwap(raw);
			return;
		}
		g_originalRead8(out, address);
	}
}

void m2ftg::CharRamFix::Install()
{
	if (g_installed)
	{
		return;
	}

	uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"stf-pxd-w64-d3d12_retail.dll"));
	if (base == nullptr)
	{
		return;
	}

	void** record = reinterpret_cast<void**>(base + RVA_MEMMAP_TBL + REGION * RECORD_SIZE);
	DWORD previousProtect = 0;
	if (VirtualProtect(record, RECORD_SIZE, PAGE_READWRITE, &previousProtect) == FALSE)
	{
		DebugLogFile("[charramfix] VirtualProtect failed (%lu) - not installed\n", GetLastError());
		return;
	}

	// Pair 0 = 8-bit: {read, write}.
	g_originalRead8 = reinterpret_cast<ReadFn>(record[0]);
	g_originalWrite8 = reinterpret_cast<WriteFn>(record[1]);
	record[0] = reinterpret_cast<void*>(&Read8Fixed);
	record[1] = reinterpret_cast<void*>(&Write8Fixed);

	DWORD ignored = 0;
	VirtualProtect(record, RECORD_SIZE, previousProtect, &ignored);

	g_installed = true;
	DebugLogFile("[charramfix] 8-bit char-RAM lane/nibble fix installed (region 0x10, 0x%07X..0x%07X)\n",
		CHAR_LO, CHAR_HI);
}

bool m2ftg::CharRamFix::IsInstalled()
{
	return g_installed;
}
