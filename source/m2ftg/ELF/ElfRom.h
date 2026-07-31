#pragma once

#include <cstdint>
#include <string>

namespace m2ftg
{
	// Program-ROM override from an ELF executable.
	//
	// A homebrew Model 2B build produces an ELF; the flat .bin the module wants is that ELF
	// with its PT_LOAD segments laid out by load address and the rest padded to the full ROM
	// size. Producing the .bin is a separate objcopy step, and the symbol table that makes
	// HLE retargeting maintainable ([HleRetarget] Hook3=geo_wait) lives only in the ELF - so
	// keeping the two in sync is a manual chore that silently rots the moment a rebuild moves
	// an address.
	//
	// Dropping "game.elf" next to the loose ROM images instead lets YAMP do the flattening
	// itself, from the same artifact the symbols come from. The module still opens
	// rom_code1.bin exactly as before; file_access serves this image for that one path (see
	// RomOverride there), so nothing in the DLL's loader or the boot state machine changes.
	//
	// Verified against roms/bin/apps/pengo: flattening by p_paddr with 0xFF fill reproduces
	// the checked-in cart/rom_code1.bin byte for byte.
	namespace ElfRom
	{
		// Looked for in the loose-ROM directory, alongside rom_code1.bin.
		inline constexpr wchar_t OVERRIDE_FILE_NAME[] = L"game.elf";
		// The module's program-ROM slot. StF, FV and MR all give the i960 a 1 MB image, and
		// the DLL reads exactly that much, so the flattened ELF has to be padded to match.
		inline constexpr uint32_t PROGRAM_ROM_SIZE = 0x100000;

		// Parses the ELF and flattens it into a romSize-byte image. Returns false (and leaves
		// no override active) if the file is absent or is not a little-endian 32-bit i960 ELF.
		bool Load(const std::wstring& elfPath, uint32_t romSize);
		void Unload();

		bool IsLoaded();
		const uint8_t* Image();
		uint32_t ImageSize();
		const wchar_t* LoadedPath();
		size_t SymbolCount();

		// "symbol", "symbol+1c" or "symbol+0x1c". The offset is hex, like every other address
		// in the retarget section. False if the name is unknown or no ELF is loaded.
		bool ResolveSymbol(const std::string& expression, uint32_t& outAddress);

		// The reverse: nearest defined symbol at or below `address`, for symbolising an i960
		// address (the 960STAT call stack). False if no ELF is loaded, it has no symbols, or
		// the address sits below the first one. `outName` points into the loaded table and
		// stays valid until Unload().
		bool SymbolizeAddress(uint32_t address, const char*& outName, uint32_t& outOffset);
	}
}
