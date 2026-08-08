#include "ArcadeSettings.h"

#include "Determinism.h"

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

		// The ROM globals LogLinkGate reads. Guest addresses, byte-swizzled like everything else
		// here; each one is named by the routine that writes it rather than by a guess.
		//
		//   0x737B90  DAT_00737b90 - the mode FUN_00091FC0 latches from FUN_00093DB4's argument,
		//                            i.e. the ROM's own record of the LINK ID it was given.
		//   0x10062A  NODE COUNT - `(status >> 5) & 0x1F`, and the value the ROM multiplies by
		//              0x350 to index its ring-layout table at 0x728518.
		//   0x10062B  THIS CABINET'S ID, 1-based - `status & 0x1F`.
		//
		// Both are DAT_00100628's bytes 2 and 3, every exit path of the check writes both, and
		// the SINGLE early return writes 1 and 1. WHICH IS WHICH WAS MEASURED, NOT READ OFF THE
		// DECOMPILER: a linked shared-memory pair reports 2/1 on the master and 2/2 on the slave,
		// so the byte that is equal on both cabinets is the count and the one that differs is the
		// id. They were labelled the other way round until that run.
		inline constexpr uint32_t ADDR_LATCHED_MODE = 0x737B90;
		inline constexpr uint32_t ADDR_NODE_COUNT = 0x10062A;
		inline constexpr uint32_t ADDR_NODE_ID = 0x10062B;

		// A THIRD COPY OF THE SETTINGS, and the one that wins. FUN_00007434 - the routine HLE
		// hook 5 injects into - refills the working block from it:
		//
		//     if (FUN_0000769C(0x31) == 1) {
		//         src = 0x1001C6; dst = 0x10018C; n = 0x3A; do { *dst++ = *src++; } while (--n);
		//     }
		//
		// The working LINK ID at 0x10019E is 0x12 bytes into that destination, so its source is
		// 0x1001C6 + 0x12. Sampled here because a host write to the working copy that is later
		// overwritten looks identical to a host write that never happened.
		inline constexpr uint32_t ADDR_SOURCE_LINK_ID = 0x1001D8;

		// WHICH OF THE CHECK'S FOUR OUTCOMES APPLIES, once it is known to have run at all.
		//
		//   0x7379B9  DAT_007379b9. Bit 0x20 = network board present (set unconditionally by
		//             FUN_0000479C from the BIOS init). Bit 0x40 = comm firmware uploaded, set by
		//             FUN_00004820 after it copies the blob at 0xFFFF8000 into 0xC0020000 and
		//             programs 0xC0020800/04/08. Clear ⇒ "NETWORK BOARD NOT PRESENT" and
		//             "NETWORK BOARD HAS ANY PROBLEM" respectively.
		//   0x737B5C  DAT_00737b5c. Set to 0xFE by the failure/cancel tail (LAB_00094084) and by
		//             nothing else in the check, so it separates "gave up" from "agreed".
		inline constexpr uint32_t ADDR_NET_FLAGS = 0x7379B9;
		inline constexpr uint32_t ADDR_FAIL_MARK = 0x737B5C;

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

		// Read-only companion to BytePtr, same swizzle. Separate because the probe below takes the
		// validated base from Determinism, which hands out a const pointer.
		static uint8_t GuestByte(const uint8_t* ram, uint32_t guestAddress)
		{
			return ram[guestAddress ^ 3u];
		}

		static bool s_gateDone = false;
		static uint64_t s_gateLast = ~0ull;
		static uint8_t s_gateFail = 0xFF;
		static int s_gateFrame = -1;

		void Reset()
		{
			s_applied = false;
			s_gateDone = false;
			s_gateLast = ~0ull;
			s_gateFail = 0xFF;
			s_gateFrame = -1;
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
			// frame= is LogLinkGate's counter, deliberately: what matters about this line is
			// whether it lands before or after the boot network check, and that comparison is
			// only meaningful if both are stamped by the same clock.
			DebugLogFile("[%s] game assignments applied (frame=%d): country=%u cabinet=%u link=%u "
				"car=%u\n",
				gGeneral.GetGameTag(), s_gateFrame, s_desired.country, s_desired.cabinetType,
				s_desired.linkId, s_desired.carNumber);
		}

		void LogLinkGate()
		{
			if (gGeneral.GetGameId() != YAMPGeneral::GameId::SRC2) return;

			// Counted from the first call rather than from the first successful read, so the
			// numbers are comparable with the [pre3] frame= heartbeat and with the "game
			// assignments applied" line above - which is the comparison the probe exists to make.
			s_gateFrame++;
			if (s_gateDone) return;

			// The VALIDATED base (it refuses anything that does not decode guest 0x189EC to SRC2's
			// `bl 0x07BA18`), because a plausible-looking base that reads zeroes everywhere has
			// already produced a run of confident wrong readings on this board twice.
			const auto* const ram = static_cast<const uint8_t*>(BoardGuestRam());
			if (ram == nullptr) return;   // board not up yet; try again next frame

			const uint8_t work = GuestByte(ram, BASE_WORK + OFF_LINK_ID);
			const uint8_t nvram = GuestByte(ram, BASE_NVRAM + OFF_LINK_ID);
			const uint8_t coin = GuestByte(ram, BASE_NVRAM + OFF_COIN);
			const uint8_t mode = GuestByte(ram, ADDR_LATCHED_MODE);
			const uint8_t nodes = GuestByte(ram, ADDR_NODE_COUNT);
			const uint8_t nodeId = GuestByte(ram, ADDR_NODE_ID);
			const uint8_t source = GuestByte(ram, ADDR_SOURCE_LINK_ID);
			const uint8_t netFlags = GuestByte(ram, ADDR_NET_FLAGS);
			const uint8_t failMark = GuestByte(ram, ADDR_FAIL_MARK);

			static const char* const MODE_NAME[4] =
				{ "SINGLE", "MASTER CONTROLLER", "SLAVE", "LIVE" };
			const auto name = [](uint8_t value)
			{
				return value < 4 ? MODE_NAME[value] : "?";
			};

			const uint64_t key = static_cast<uint64_t>(work) | static_cast<uint64_t>(nvram) << 8
				| static_cast<uint64_t>(coin) << 16 | static_cast<uint64_t>(mode) << 24
				| static_cast<uint64_t>(nodeId) << 32 | static_cast<uint64_t>(nodes) << 40
				| static_cast<uint64_t>(source) << 48 | static_cast<uint64_t>(netFlags) << 56;
			if (key != s_gateLast || failMark != s_gateFail)
			{
				s_gateLast = key;
				s_gateFail = failMark;
				DebugLogFile("[%s linkgate] frame=%d LINK ID work=%u nvram=%u src=%u coin=0x%02X | "
					"ROM latched mode=%u (%s) id=%u nodes=%u | net=0x%02X%s%s fail=0x%02X\n",
					gGeneral.GetGameTag(), s_gateFrame, work, nvram, source, coin, mode,
					name(mode), nodeId, nodes, netFlags,
					(netFlags & 0x20) != 0 ? " present" : " NOT-PRESENT",
					(netFlags & 0x40) != 0 ? " fw" : " NO-FW", failMark);
			}

			// THE CHECK HAS RUN once the node COUNT is non-zero. Not the id: the BIOS gets there
			// first - FUN_00004820's `FUN_00012DA4(0, 1, 0x100)` stores the dword 1 at 0x100628,
			// whose low byte IS 0x10062B, so an id of 1 says nothing. 0x10062A is written only by
			// FUN_00093DB4, on every one of its exit paths.
			if (nodes == 0) return;

			s_gateDone = true;
			if (mode == 0)
			{
				DebugLogFile("[%s linkgate] VERDICT frame=%d: the boot network check ran with "
					"LINK ID = 0 (SINGLE), so FUN_00093DB4 returned before printing anything. "
					"No network status screen is drawn - by the ROM's design, not by a loss. "
					"Host LINK ID at this instant: work=%u nvram=%u src=%u (applied=%d, "
					"wanted=%u).\n",
					gGeneral.GetGameTag(), s_gateFrame, work, nvram, source,
					static_cast<int>(s_applied), s_desired.linkId);
			}
			else
			{
				DebugLogFile("[%s linkgate] VERDICT frame=%d: the boot network check ran as %s "
					"(LINK ID = %u) and settled on id=%u nodes=%u, net=0x%02X fail=0x%02X -> %s. "
					"The screen's text path WAS reached, so anything missing from here is "
					"downstream of the ROM.\n",
					gGeneral.GetGameTag(), s_gateFrame, name(mode), mode, nodeId, nodes,
					netFlags, failMark,
					(netFlags & 0x20) == 0 ? "\"NETWORK BOARD NOT PRESENT\""
					: (netFlags & 0x40) == 0 ? "\"NETWORK BOARD HAS ANY PROBLEM\""
					: failMark == 0xFE ? "the failure/cancel tail (an error was printed)"
					: "agreed on a ring");
			}
		}
	}
}
