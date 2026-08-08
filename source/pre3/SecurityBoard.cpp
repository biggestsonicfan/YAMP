#include "SecurityBoard.h"
#include "BoardVtables.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <cstdlib>

namespace pre3
{
	namespace SecurityBoard
	{
		// ROM vtable slot 2 - the security REGISTER read, reached from the 0x?01Axxxx sub-window's
		// 32-bit accessor (DLL 0x180013A00, which masks the address with 0xFFFFFF3F and calls
		// slot 2). Slot 3 is the matching write, and slots 4/5 are the security RAM window at
		// 0x?018xxxx - deliberately untouched here.
		inline constexpr size_t SLOT_REGISTER_READ = 2;

		// The register file is 6 address bits wide, which is what the module's own mask leaves.
		inline constexpr uint32_t REGISTER_MASK = 0x3F;
		// Offset 0 is the status register the overlay polls; every other offset is left alone.
		inline constexpr uint32_t REGISTER_STATUS = 0x00;

		// THE BUSY BIT, derived rather than guessed. The guest reads the status with `lwbrx`, a
		// BYTE-REVERSED load, and tests it with `rlwinm. r0, r3, 31, 0, 0` - which rotates left by
		// 31 and keeps PowerPC bit 0, i.e. it extracts the LOW bit of the loaded word. `bne` then
		// loops while that bit is set.
		//
		// Byte-reversing means the guest's low bit is bit 24 of what this handler returns, so
		// "busy" here is 0x01000000, not 0x00000001. Getting that backwards yields a value the
		// guest reads as ready, which is precisely the bug being fixed.
		inline constexpr uint32_t STATUS_BUSY = 0x01000000;

		using register_read_fn = uint32_t (*)(void* self, uint32_t address);

		static register_read_fn orgRegisterRead = nullptr;

		// First few accesses get logged. "Is my replacement even being called, and with what?" is
		// the question that separates a wrong VALUE from a wrong SLOT, and guessing between those
		// two costs a build and a run each time.
		static unsigned logged = 0;

		static uint32_t RegisterRead(void* self, uint32_t address)
		{
			const uint32_t offset = address & REGISTER_MASK;
			const uint32_t value = (offset == REGISTER_STATUS)
				? STATUS_BUSY
				: orgRegisterRead(self, address);
			if (logged < 12)
			{
				logged++;
				DebugLogFile("[%s security] read addr=%08X offset=%02X -> %08X\n",
					gGeneral.GetGameTag(), address, offset, value);
			}
			return value;
		}

		// Slot 3, the register WRITE. Redirected only to observe: the overlay writes the command
		// word before it reads anything, so if the reads never fire this says whether the guest
		// is touching the security board at all or not reaching it in the first place.
		inline constexpr size_t SLOT_REGISTER_WRITE = 3;

		using register_write_fn = void (*)(void* self, uint32_t address, uint32_t value);

		static register_write_fn orgRegisterWrite = nullptr;
		static unsigned loggedWrites = 0;

		static void RegisterWrite(void* self, uint32_t address, uint32_t value)
		{
			if (loggedWrites < 12)
			{
				loggedWrites++;
				DebugLogFile("[%s security] write addr=%08X offset=%02X value=%08X\n",
					gGeneral.GetGameTag(), address, address & REGISTER_MASK, value);
			}
			orgRegisterWrite(self, address, value);
		}

		enum class Mode { Off, Fail };

		static Mode ConfiguredMode()
		{
			char value[16];
			size_t length = 0;
			if (getenv_s(&length, value, sizeof(value), "YAMP_PRE3_SECURITY") != 0 || length == 0)
			{
				return Mode::Off;
			}
			return (value[0] == 'f' || value[0] == 'F') ? Mode::Fail : Mode::Off;
		}
	}

	void InstallSecurityBoard(void* const* vtables, size_t count)
	{
		if (SecurityBoard::ConfiguredMode() != SecurityBoard::Mode::Fail)
		{
			return;
		}

		void* original = nullptr;
		const size_t patched = RedirectBoardSlot(vtables, count,
			SecurityBoard::SLOT_REGISTER_READ,
			reinterpret_cast<void*>(&SecurityBoard::RegisterRead), &original);
		if (patched == 0)
		{
			// Every board vtable holds the same `return 0` stub in this slot, so a mismatch means
			// this build's class hierarchy is not the one the offsets were read from. Say so -
			// silently leaving the stub in place would look like the experiment had run.
			DebugLog("[%s] security register slot is not consistent across the board vtables - "
				"leaving the module's stub alone.\n", gGeneral.GetGameTag());
			return;
		}
		SecurityBoard::orgRegisterRead =
			reinterpret_cast<SecurityBoard::register_read_fn>(original);

		void* originalWrite = nullptr;
		if (RedirectBoardSlot(vtables, count, SecurityBoard::SLOT_REGISTER_WRITE,
			reinterpret_cast<void*>(&SecurityBoard::RegisterWrite), &originalWrite) != 0)
		{
			SecurityBoard::orgRegisterWrite =
				reinterpret_cast<SecurityBoard::register_write_fn>(originalWrite);
		}
		DebugLogFile("[%s security] status register now reports BUSY (%zu vtable entries). The "
			"guest's protection routine should time out and take its own fallback.\n",
			gGeneral.GetGameTag(), patched);
	}
}
