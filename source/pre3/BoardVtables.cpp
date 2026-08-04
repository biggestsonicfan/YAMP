#include "BoardVtables.h"

#include "../Utils/MemoryMgr.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace pre3
{
	size_t FindBoardVtables(void* dll, const void* readPortFn, void** out, size_t max)
	{
		auto* const base = static_cast<uint8_t*>(dll);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

		size_t found = 0;
		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections && found < max; i++, section++)
		{
			if (strncmp(reinterpret_cast<const char*>(section->Name), ".rdata",
				IMAGE_SIZEOF_SHORT_NAME) != 0)
			{
				continue;
			}

			// Vtable entries are pointer-aligned, so step by the pointer size rather than by the
			// byte - a byte-wise scan could match an unaligned coincidence in const data.
			void** const first = reinterpret_cast<void**>(base + section->VirtualAddress);
			const size_t count = section->Misc.VirtualSize / sizeof(void*);
			for (size_t slot = 0; slot < count && found < max; slot++)
			{
				if (first[slot] == readPortFn)
				{
					out[found++] = &first[slot];
				}
			}
		}
		return found;
	}

	size_t RedirectBoardSlot(void* const* tables, size_t count, size_t slot,
		void* replacement, void** original)
	{
		if (count == 0)
		{
			return 0;
		}

		// Every table must agree on what is in the slot. They are all the same inherited method,
		// so a disagreement means the assumption behind this whole helper - one class hierarchy,
		// one method per slot - does not hold for this build, and writing anything would be
		// guesswork. Checked BEFORE writing, so a rejected build is left completely untouched.
		void* const current = static_cast<void**>(tables[0])[slot];
		if (current == nullptr || current == replacement)
		{
			return 0;
		}
		for (size_t i = 1; i < count; i++)
		{
			if (static_cast<void**>(tables[i])[slot] != current)
			{
				return 0;
			}
		}

		for (size_t i = 0; i < count; i++)
		{
			Memory::Patch(&static_cast<void**>(tables[i])[slot], replacement);
		}
		if (original != nullptr)
		{
			*original = current;
		}
		return count;
	}
}
