#include "ElfRom.h"

#include "../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace
{
	// ELF32, little-endian, EM_960. Only the handful of fields the flattening needs; the ELF
	// is produced by the homebrew's own toolchain, so this is a reader, not a validator.
	constexpr uint8_t ELF_MAGIC[4] = { 0x7F, 'E', 'L', 'F' };
	constexpr uint8_t ELFCLASS32 = 1;
	constexpr uint8_t ELFDATA2LSB = 1;
	constexpr uint16_t EM_960 = 19;
	constexpr uint32_t PT_LOAD = 1;
	constexpr uint32_t SHT_SYMTAB = 2;
	constexpr uint16_t SHN_UNDEF = 0;
	// Unwritten ROM reads back as 0xFF, which is what the toolchain's own objcopy pads with.
	constexpr uint8_t ROM_FILL = 0xFF;

	struct Elf32Header
	{
		uint8_t  e_ident[16];
		uint16_t e_type, e_machine;
		uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
		uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
	};
	struct Elf32ProgramHeader
	{
		uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
	};
	struct Elf32SectionHeader
	{
		uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
		uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
	};
	struct Elf32Symbol
	{
		uint32_t st_name, st_value, st_size;
		uint8_t  st_info, st_other;
		uint16_t st_shndx;
	};
	static_assert(sizeof(Elf32Header) == 52, "ELF32 header is 52 bytes");
	static_assert(sizeof(Elf32ProgramHeader) == 32, "ELF32 program header is 32 bytes");
	static_assert(sizeof(Elf32SectionHeader) == 40, "ELF32 section header is 40 bytes");
	static_assert(sizeof(Elf32Symbol) == 16, "ELF32 symbol is 16 bytes");

	// Symbol types worth keeping: FILE and SECTION entries carry no address of interest and
	// would only add noise to a symbolised call stack.
	constexpr uint8_t STT_SECTION = 3;
	constexpr uint8_t STT_FILE = 4;

	constexpr uint8_t STT_FUNC = 2;

	struct SortedSymbol
	{
		uint32_t address;
		uint8_t rank;       // higher wins when several symbols share an address
		std::string name;
	};

	std::vector<uint8_t> s_image;
	std::unordered_map<std::string, uint32_t> s_symbols;
	// Same set, ordered by address, for the address -> name direction.
	std::vector<SortedSymbol> s_sorted;
	std::wstring s_path;

	bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		bool ok = false;
		LARGE_INTEGER size;
		if (GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 && size.QuadPart < (256 << 20))
		{
			out.resize(static_cast<size_t>(size.QuadPart));
			DWORD read = 0;
			ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != FALSE
				&& read == out.size();
		}
		CloseHandle(file);
		if (!ok)
		{
			out.clear();
		}
		return ok;
	}

	// Bounds-checked view of a fixed-size record at a byte offset.
	template <typename T>
	const T* At(const std::vector<uint8_t>& data, size_t offset)
	{
		if (offset > data.size() || data.size() - offset < sizeof(T))
		{
			return nullptr;
		}
		return reinterpret_cast<const T*>(data.data() + offset);
	}
}

bool m2ftg::ElfRom::Load(const std::wstring& elfPath, uint32_t romSize)
{
	Unload();

	std::vector<uint8_t> file;
	if (!ReadWholeFile(elfPath, file))
	{
		return false;
	}

	const Elf32Header* header = At<Elf32Header>(file, 0);
	if (header == nullptr || memcmp(header->e_ident, ELF_MAGIC, sizeof(ELF_MAGIC)) != 0)
	{
		DebugLog("[elf] '%ls' is not an ELF file - ignored\n", elfPath.c_str());
		return false;
	}
	if (header->e_ident[4] != ELFCLASS32 || header->e_ident[5] != ELFDATA2LSB)
	{
		DebugLog("[elf] '%ls' is not 32-bit little-endian (class=%u data=%u) - ignored\n",
			elfPath.c_str(), header->e_ident[4], header->e_ident[5]);
		return false;
	}
	if (header->e_machine != EM_960)
	{
		DebugLog("[elf] '%ls' targets machine %u, not i960 (%u) - ignored\n",
			elfPath.c_str(), header->e_machine, EM_960);
		return false;
	}

	// Flatten by LOAD ADDRESS, not virtual address: .data lives at 0x502000 at run time but
	// its initial image is stored in ROM at its p_paddr, and the program copies it into RAM
	// during boot (pengo's copy_rom_to_main_ram). Using p_vaddr would scatter it outside the
	// ROM entirely.
	std::vector<uint8_t> image(romSize, ROM_FILL);
	size_t loaded = 0;
	for (uint16_t i = 0; i < header->e_phnum; i++)
	{
		const auto* ph = At<Elf32ProgramHeader>(file,
			header->e_phoff + static_cast<size_t>(i) * header->e_phentsize);
		if (ph == nullptr || ph->p_type != PT_LOAD || ph->p_filesz == 0)
		{
			continue;
		}
		if (ph->p_paddr > romSize || romSize - ph->p_paddr < ph->p_filesz)
		{
			DebugLog("[elf] segment %u (paddr 0x%X, 0x%X bytes) does not fit in the 0x%X program ROM - ignored\n",
				i, ph->p_paddr, ph->p_filesz, romSize);
			return false;
		}
		if (ph->p_offset > file.size() || file.size() - ph->p_offset < ph->p_filesz)
		{
			DebugLog("[elf] segment %u is truncated in the file - ignored\n", i);
			return false;
		}
		memcpy(image.data() + ph->p_paddr, file.data() + ph->p_offset, ph->p_filesz);
		DebugLog("[elf] segment %u -> ROM 0x%06X..0x%06X\n", i, ph->p_paddr, ph->p_paddr + ph->p_filesz);
		loaded++;
	}

	if (loaded == 0)
	{
		DebugLog("[elf] '%ls' has no loadable segments - ignored\n", elfPath.c_str());
		return false;
	}

	// Symbols are optional: a stripped ELF still produces a perfectly good ROM image, it just
	// cannot be retargeted by name.
	for (uint16_t i = 0; i < header->e_shnum; i++)
	{
		const auto* sh = At<Elf32SectionHeader>(file,
			header->e_shoff + static_cast<size_t>(i) * header->e_shentsize);
		if (sh == nullptr || sh->sh_type != SHT_SYMTAB || sh->sh_entsize != sizeof(Elf32Symbol))
		{
			continue;
		}
		const auto* strtab = At<Elf32SectionHeader>(file,
			header->e_shoff + static_cast<size_t>(sh->sh_link) * header->e_shentsize);
		if (strtab == nullptr || strtab->sh_offset > file.size())
		{
			continue;
		}
		const char* strings = reinterpret_cast<const char*>(file.data()) + strtab->sh_offset;
		const size_t stringsSize = file.size() - strtab->sh_offset < strtab->sh_size
			? file.size() - strtab->sh_offset : strtab->sh_size;

		for (uint32_t s = 0; s < sh->sh_size / sizeof(Elf32Symbol); s++)
		{
			const auto* sym = At<Elf32Symbol>(file, sh->sh_offset + static_cast<size_t>(s) * sizeof(Elf32Symbol));
			if (sym == nullptr || sym->st_shndx == SHN_UNDEF || sym->st_name == 0 || sym->st_name >= stringsSize)
			{
				continue;
			}
			const uint8_t type = sym->st_info & 0xF;
			if (type == STT_SECTION || type == STT_FILE)
			{
				continue;
			}
			const char* name = strings + sym->st_name;
			if (*name == '\0')
			{
				continue;
			}
			// First definition wins, so a later local alias cannot shadow the real symbol.
			if (s_symbols.emplace(name, sym->st_value).second)
			{
				s_sorted.push_back({ sym->st_value, static_cast<uint8_t>(type == STT_FUNC ? 1 : 0), name });
			}
		}
	}

	// Several symbols can share an address - pengo has both `start` and `system_address_table`
	// at 0 - and picking arbitrarily makes a symbolised stack read wrong. Sort by address, then
	// by descending rank, and keep only the first of each address so a function name wins.
	std::sort(s_sorted.begin(), s_sorted.end(), [](const SortedSymbol& a, const SortedSymbol& b)
		{
			if (a.address != b.address) { return a.address < b.address; }
			return a.rank > b.rank;
		});
	s_sorted.erase(std::unique(s_sorted.begin(), s_sorted.end(),
		[](const SortedSymbol& a, const SortedSymbol& b) { return a.address == b.address; }),
		s_sorted.end());

	s_image = std::move(image);
	s_path = elfPath;
	DebugLog("[elf] '%ls' loaded: %zu segment(s), 0x%X-byte ROM image, %zu symbol(s)\n",
		elfPath.c_str(), loaded, romSize, s_symbols.size());
	return true;
}

void m2ftg::ElfRom::Unload()
{
	s_image.clear();
	s_image.shrink_to_fit();
	s_symbols.clear();
	s_sorted.clear();
	s_sorted.shrink_to_fit();
	s_path.clear();
}

bool m2ftg::ElfRom::IsLoaded()
{
	return !s_image.empty();
}

const uint8_t* m2ftg::ElfRom::Image()
{
	return s_image.empty() ? nullptr : s_image.data();
}

uint32_t m2ftg::ElfRom::ImageSize()
{
	return static_cast<uint32_t>(s_image.size());
}

const wchar_t* m2ftg::ElfRom::LoadedPath()
{
	return s_path.c_str();
}

size_t m2ftg::ElfRom::SymbolCount()
{
	return s_symbols.size();
}

bool m2ftg::ElfRom::ResolveSymbol(const std::string& expression, uint32_t& outAddress)
{
	if (s_symbols.empty() || expression.empty())
	{
		return false;
	}

	std::string name = expression;
	uint32_t addend = 0;
	if (const size_t plus = expression.find('+'); plus != std::string::npos)
	{
		name = expression.substr(0, plus);
		std::string offset = expression.substr(plus + 1);
		if (offset.size() > 2 && offset[0] == '0' && (offset[1] == 'x' || offset[1] == 'X'))
		{
			offset = offset.substr(2);
		}
		if (offset.empty())
		{
			return false;
		}
		char* end = nullptr;
		addend = static_cast<uint32_t>(strtoul(offset.c_str(), &end, 16));
		if (end == nullptr || *end != '\0')
		{
			return false;
		}
	}

	while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
	{
		name.pop_back();
	}

	const auto it = s_symbols.find(name);
	if (it == s_symbols.end())
	{
		return false;
	}
	outAddress = it->second + addend;
	return true;
}

bool m2ftg::ElfRom::SymbolizeAddress(uint32_t address, const char*& outName, uint32_t& outOffset)
{
	if (s_sorted.empty() || address < s_sorted.front().address)
	{
		return false;
	}

	// upper_bound then step back: the last symbol whose address is <= the query.
	const auto it = std::upper_bound(s_sorted.begin(), s_sorted.end(), address,
		[](uint32_t value, const SortedSymbol& sym) { return value < sym.address; });
	const SortedSymbol& sym = *(it - 1);
	outName = sym.name.c_str();
	outOffset = address - sym.address;
	return true;
}
