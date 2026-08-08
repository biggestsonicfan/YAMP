#pragma once

#include <cstddef>

namespace pre3
{
	// The Model 3 SECURITY BOARD, and specifically the fact that pre3's model of it lies.
	//
	// Real hardware puts the protection device's RAM at 0xF0180000 and its registers at
	// 0xF01A0000 (MAME: `model3.cpp`), both mirrored at 0xFE18/0xFE1A. pre3 maps both windows
	// and routes them into the ROM object's vtable — RAM through slots 4/5, REGISTERS through
	// slots 2/3 — and then implements the registers as `return 0` / `ret`, identically in all
	// eight board vtables. No game gets a working device.
	//
	// A device that is simply ABSENT would be survivable, because Sega's code degrades on its
	// own. Sega Racing Classic 2's protection routine is a 464-byte XOR-0x98 obfuscated overlay
	// (see docs/src2-hle-hooks.md) that polls a busy bit with a 0x411A8 timeout and, on timeout,
	// returns 0 so its caller jumps to a fallback address the loader stashed at 0x55DF00.
	//
	// The stub defeats exactly that. Reads return zero, the guest's `lwbrx` byte-swaps zero to
	// zero, the busy test clears on the FIRST poll, the timeout is never reached, and the routine
	// reports SUCCESS with a result buffer full of zeros. The board then acts on the garbage - in
	// SRC2 it goes on to overwrite its own video-register table at 0x0BB910 with ASCII "0000",
	// after which the frame-sync wait spins on a pointer that is not a device address.
	//
	// So the cheap, faithful repair is not to emulate the chip: it is to stop lying. Hold the
	// busy bit SET and the guest takes the failure path it already ships.
	//
	// Off unless asked for, because it is only reachable at all with the game's protection hooks
	// disabled (SRC2 hooks 1 and 2, which are boot-critical and need YAMP_PRE3_ALLOW_CRITICAL):
	//
	//   YAMP_PRE3_SECURITY=fail    hold the busy bit set, so the guest times out and falls back
	//   YAMP_PRE3_SECURITY=off     (or unset) leave the module's own stub alone
	//
	// Takes the board vtables ALREADY LOCATED, for the same reason InstallSystemSwitches does:
	// they are identified by slot 0, so every installer must be handed the same list rather than
	// finding its own.
	void InstallSecurityBoard(void* const* vtables, size_t count);
}
