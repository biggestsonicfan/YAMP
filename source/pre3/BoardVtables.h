#pragma once

#include <cstddef>

namespace pre3
{
	// Finding, and redirecting, the emulated board's own vtables.
	//
	// Every M3ERom subclass derives from M3EInput and inherits its whole method set, so the six
	// games' vtables all carry the same functions in the same slots for everything M3EInput
	// declares. That is what makes this worth sharing: a host patch aimed at one of those methods
	// has to go into EVERY one of those vtables, because which subclass the ROM factory builds
	// depends on the configured game, and there is no point duplicating the factory's switch to
	// find out.
	//
	// The vtables are located by a slot that is never overridden - M3EInput::read_port, slot 0 -
	// so one resolved symbol anchors all of them. Redirecting a slot is then a pointer write into
	// .rdata, which the caller must already have unprotected.
	//
	// Eight vtables carry it in the shipped build (six M3ERom subclasses, one secondary base, and
	// M3EInput's own); nothing else in the image points at the function.
	inline constexpr size_t MAX_BOARD_VTABLES = 16;

	// Vtables whose slot 0 is `readPortFn`. Returns how many were found and fills `out`.
	size_t FindBoardVtables(void* dll, const void* readPortFn, void** out, size_t max);

	// Points `slot` at `replacement` in every vtable in `tables`, and reports the function that
	// was there through `original` (they all hold the same one - see above; a build where they
	// did not would be a different class hierarchy and is rejected by returning 0).
	//
	// Returns the number of entries written, or 0 if the slot did not hold one consistent
	// function across every table - in which case nothing is written at all.
	size_t RedirectBoardSlot(void* const* tables, size_t count, size_t slot,
		void* replacement, void** original);
}
