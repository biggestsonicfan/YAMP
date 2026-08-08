#include "ArcadeSettings.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstddef>

namespace pre3
{
	namespace ArcadeSettings
	{
		// The module's guest RAM base (DAT_18062AA98), which is what its own handlers use for
		// direct guest memory. NOT CM3Mem+0x18 - that is a different base and reads as zeroes at
		// addresses that provably hold code.
		inline constexpr uintptr_t RVA_GUEST_RAM = 0x62AA98;

		// The two copies of the settings block, same layout. The board loads NVRAM into the
		// working copy during boot, so writing only one of them is a race with the boot order;
		// writing both is not.
		inline constexpr uint32_t BASE_WORK = 0x100180;
		inline constexpr uint32_t BASE_NVRAM = 0x72629C;

		inline constexpr uint32_t OFF_COIN = 0x17;
		inline constexpr uint32_t OFF_COUNTRY = 0x18;
		inline constexpr uint32_t OFF_LINK_ID = 0x1E;
		inline constexpr uint32_t OFF_CAR_NUMBER = 0x1F;
		inline constexpr uint32_t OFF_CABINET_TYPE = 0x20;

		static Desired s_desired{};
		static bool s_applied = false;

		static const uint8_t* ModuleBase()
		{
			return reinterpret_cast<const uint8_t*>(
				GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
		}

		// Guest RAM is kept word-swizzled - the emulator stores each 32-bit word in host order -
		// so a BYTE at guest address A lives at `ram + (A ^ 3)` while a dword needs no fixup.
		// This is the module's own convention, straight out of its settings injector.
		static uint8_t* BytePtr(uint8_t* ram, uint32_t guestAddress)
		{
			return ram + (guestAddress ^ 3u);
		}

		void SetDesired(const Desired& desired)
		{
			s_desired = desired;
		}

		void SetLink(LinkId role, uint8_t carNumber)
		{
			s_desired.linkId = static_cast<uint8_t>(role);
			s_desired.carNumber = carNumber < 8 ? carNumber : 0;
		}

		void Reset()
		{
			s_applied = false;
		}

		void Update()
		{
			if (s_applied || gGeneral.GetGameId() != YAMPGeneral::GameId::SRC2)
			{
				return;
			}

			const uint8_t* const base = ModuleBase();
			if (base == nullptr) return;
			auto* const ram = *reinterpret_cast<uint8_t* const*>(base + RVA_GUEST_RAM);
			if (ram == nullptr) return;

			// WAIT FOR THE MODULE'S OWN INJECTOR, rather than racing it. It writes the coin
			// setting as 1 or 0x1B, never 0, so a zero there means the block has not been filled
			// in yet and anything written now would simply be overwritten.
			const uint8_t coin = *BytePtr(ram, BASE_NVRAM + OFF_COIN);
			if (coin == 0) return;

			struct { uint32_t offset; uint8_t value; const char* name; } writes[] = {
				{ OFF_COUNTRY,      s_desired.country,     "COUNTRY" },
				{ OFF_CABINET_TYPE, s_desired.cabinetType, "CABINET TYPE" },
				{ OFF_LINK_ID,      s_desired.linkId,      "LINK ID" },
				{ OFF_CAR_NUMBER,   s_desired.carNumber,   "CAR NUMBER" },
			};
			for (const auto& write : writes)
			{
				*BytePtr(ram, BASE_NVRAM + write.offset) = write.value;
				*BytePtr(ram, BASE_WORK + write.offset) = write.value;
			}

			// Only latch once the working copy reads back what was asked for. If the board is
			// still mid-copy the write lands and is then undone, and retrying next frame costs
			// nothing - whereas latching optimistically would leave the setting silently ignored.
			bool settled = true;
			for (const auto& write : writes)
			{
				settled = settled && *BytePtr(ram, BASE_WORK + write.offset) == write.value;
			}
			if (!settled) return;

			s_applied = true;
			DebugLogFile("[%s] game assignments applied: country=%u cabinet=%u link=%u car=%u\n",
				gGeneral.GetGameTag(), s_desired.country, s_desired.cabinetType,
				s_desired.linkId, s_desired.carNumber);
		}
	}
}
