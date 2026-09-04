#pragma once

#include <cstddef>
#include <string_view>

#include "../Utils/Patterns.h"
#include "../Utils/MemoryMgr.h"

// The byte-pattern resolution helpers every host's ImportSymbols.cpp used to define for
// itself, byte-identical (2026-08-09 dedup; the seventh copy lived inline in the YLAD VF2
// host). They sit next to pxd/Imports.h because they are the other half of the same job:
// Imports is the container, these are how its entries are found.
namespace pxd
{
	// The scan range for a whole loaded module. ModUtils dropped the `pattern(module, ...)`
	// constructor in favour of an explicit section list, so every module-wide scan names its
	// range now. Readable sections are what upstream itself uses as the default range for the
	// process module, and they are a superset of the code-section range the old constructor
	// scanned - so every pattern that resolved before still resolves. The cost of the wider
	// range is that a pattern could now also match in .rdata/.data; the txn/assert policies
	// catch that as a count mismatch rather than silently picking the wrong one.
	inline hook::scan_segments module_scan(void* module)
	{
		return hook::get_all_readable_sections(module);
	}

	// One pattern, resolved inside the module image. Used by itself it returns the match
	// address (plus `offset`); wrapped in immediate() it resolves the instruction's
	// RIP-relative operand instead.
	template<typename T = void>
	inline auto get_module_pattern(void* module, std::string_view pattern_string, ptrdiff_t offset = 0)
	{
		return hook::txn::pattern(module_scan(module), std::move(pattern_string)).get_first<T>(offset);
	}

	// Resolve a RIP-relative disp32 operand (LEA/MOV/CALL rel32...).
	inline void* immediate(void* addr)
	{
		void* val;
		Memory::ReadOffsetValue(addr, val);
		return val;
	}

	// Resolve a rel8 branch target (short jumps).
	inline void* immediate8(void* addr)
	{
		intptr_t srcAddr = (intptr_t)addr;
		intptr_t dstAddr = srcAddr + 1 + *(int8_t*)srcAddr;
		return reinterpret_cast<void*>(dstAddr);
	}

	// Resolve a RIP-relative operand whose instruction carries ONE trailing immediate byte
	// after the disp32 - `CMP byte ptr [rip+disp32], imm8`. RIP is relative to the END of the
	// instruction, so the imm8 counts. Using the plain immediate() here resolves one byte
	// SHORT, which would not crash: it would quietly address the neighbouring byte and look
	// exactly like "the change did nothing".
	inline void* immediateImm8(void* addr)
	{
		void* val;
		Memory::ReadOffsetValue(addr, val, 1);
		return val;
	}
}
