#include "SystemSwitches.h"
#include "BoardVtables.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace pre3
{
	// ---- Cabinet TEST / SERVICE switches --------------------------------------------
	//
	// The emulated Model 3 board keeps its JAMMA inputs in a small register block inside the
	// M3EInput object that every M3ERom subclass derives from, and serves them through one
	// accessor — M3EInput::read_port, DLL 0x180033A30, vtable slot 0:
	//
	//     port 0x04 -> ~[this+0x28]   SYSTEM   port 0x08 -> ~[this+0x29]   player 1
	//     port 0x0C -> ~[this+0x2A]   player 2 port 0x3C -> the ADC ring at [this+0x2B]
	//
	// Active low, and the accessor is where the inversion happens: the registers themselves are
	// set-means-pressed and the guest sees the complement.
	//
	// The SYSTEM byte is the service panel, and the module drives exactly three of its bits. The
	// board's frame function (DLL 0x18003B0A0) clears +0x28..+0x2A, sets bit0 when the host's coin
	// latch at +0x35 is up — which is what execute_info status bit5 becomes — and then hands the
	// two pads to the game's own mapper (slot 24), which is where START becomes bit4 / bit5. That
	// is the whole of it: NOTHING in any of the six games ever touches the remaining bits, so TEST
	// and SERVICE have no host source in the pre3 protocol at all, exactly as on the m2ftg path.
	//
	// The layout of those remaining bits is the Sega I/O board's, which the ROM is arcade code and
	// therefore expects verbatim: coin 1/2 on bits 0-1 and start 1/2 on bits 4-5 are what the
	// module's own writes above establish, and the service panel is bits 2-3 (cabinet A) and 6-7
	// (cabinet B).
	//
	// Which of bits 2 and 3 is TEST was MEASURED rather than taken from a hardware reference,
	// because published Model 3 pinouts disagree on the order and getting it backwards produces a
	// switch that silently does nothing. Forcing one bit at a time on a headless
	// `-fv2 -frames 900` run and watching execute_info's game-mode pair (see pre3.h):
	//
	//     none / 0x02 / 0x08 / 0x40 -> 00,01,02,03/xx   the ordinary attract sequence
	//     0x04 / 0x80               -> 00, 14/00, 15/01  the board's service menu
	//
	// so bit2 is the TEST switch and bit7 is cabinet B's, and neither bit3 nor bit6 changes what
	// the board displays — which is exactly right for a SERVICE button, whose whole effect is a
	// service credit and menu navigation once TEST has opened the menu.
	//
	// Unlike m2ftg's I/O byte, this block does NOT have to be rewritten behind the module: since
	// the accessor is the only reader, a switch can be applied there. That is closer to the
	// hardware (the lines are sampled when the guest reads the port, not latched a frame earlier)
	// and it leaves the module's per-frame rebuild completely untouched.
	namespace SystemSwitches
	{
		constexpr uint8_t PORT_SYSTEM = 0x04;
		constexpr uint8_t BIT_TEST = 0x04;     // test switch, cabinet A
		constexpr uint8_t BIT_SERVICE = 0x08;  // service button, cabinet A

		// ---- The ADC, and why it is served here rather than written into the board ----------
		//
		// The same accessor's fourth branch, read straight off DLL 0x180033A30:
		//
		//     port 0x3C:  b = *(u8 *)(this + 0x2B + this[0x33]);
		//                 this[0x33] = (this[0x33] + 1) & 7;
		//                 return b;                       // RAW - not inverted, unlike 4/8/0xC
		//
		// So it is an EIGHT-channel ring at +0x2B..+0x32 with a self-advancing index at +0x33.
		// Nothing resets that index: the per-frame input build (DLL 0x18003B0A0) clears +0x28,
		// +0x29 and +0x2A and calls the game's mapper, and never touches +0x2B..+0x33 at all.
		// Neither does the constructor. **The ring is therefore all zeroes for the life of the
		// board**, which for a steering channel is not "centred", it is full lock - and it is why
		// Sega Racing Classic 2 has never been drivable.
		//
		// SERVING THE READ rather than filling the ring keeps this hook the only writer of
		// nothing: the index still advances in the module's own code, so if a future build changes
		// how it moves this follows automatically. It is also closer to the hardware, where the
		// converter is sampled at read time - the same argument the TEST/SERVICE path above makes.
		//
		// CHANNEL ASSIGNMENT IS THE MODEL 3 DRIVING CONVENTION AND IS NOT YET CONFIRMED. The
		// accessor names nothing, and the meaning lives in the game ROM rather than the module.
		// 0 steering / 1 accelerator / 2 brake is what the Model 3 driving games conventionally
		// use and what MAME's daytona2 ports assume; the remaining five read zero. If a car brakes
		// when it should steer, this is the table to swap - it is the only place the order exists.
		constexpr uint8_t PORT_ADC = 0x3C;
		constexpr size_t OFF_ADC_CHANNEL = 0x33;
		constexpr uint8_t ADC_STEER = 0;
		constexpr uint8_t ADC_THROTTLE = 1;
		constexpr uint8_t ADC_BRAKE = 2;

		// Steering rests CENTRED. A pedal rests at zero, but an 8-bit wheel axis is a full swing
		// and its neutral is the middle of it - which is the whole difference between a board that
		// drives straight and one that is welded left.
		constexpr uint8_t ADC_CENTRE = 0x80;

		using read_port_fn = uint8_t (*)(void* self, uint8_t port);

		static read_port_fn orgReadPort = nullptr;
		// Written by the host thread each frame, read by the module thread inside the hook.
		static std::atomic<uint8_t> heldMask{ 0 };
		// One word so a frame's three axes are picked up together: the module samples these from
		// its own thread and a torn read would be a wheel and pedals from different frames.
		// Packed steer | throttle << 8 | brake << 16.
		static std::atomic<uint32_t> axes{ static_cast<uint32_t>(ADC_CENTRE) };

		static uint8_t ReadPort(void* self, uint8_t port)
		{
			if (port == PORT_ADC)
			{
				// Sampled BEFORE the call, because the call is what advances it.
				const uint8_t channel = static_cast<uint8_t>(
					*(static_cast<const uint8_t*>(self) + OFF_ADC_CHANNEL) & 7);
				orgReadPort(self, port);

				const uint32_t packed = axes.load(std::memory_order_relaxed);
				switch (channel)
				{
				case ADC_STEER:    return static_cast<uint8_t>(packed & 0xFF);
				case ADC_THROTTLE: return static_cast<uint8_t>((packed >> 8) & 0xFF);
				case ADC_BRAKE:    return static_cast<uint8_t>((packed >> 16) & 0xFF);
				default:           return 0;
				}
			}

			const uint8_t value = orgReadPort(self, port);
			if (port != PORT_SYSTEM)
			{
				return value;
			}
			// Active low, and `value` has already been inverted by the accessor: a closed switch
			// is a CLEARED bit.
			return static_cast<uint8_t>(value & ~heldMask.load(std::memory_order_relaxed));
		}

	}

	void InstallSystemSwitches(void* const* vtables, size_t count)
	{
		// Slot 0 is both what identifies a board vtable and what this replaces - see the header
		// for why the caller locates them rather than this function.
		void* original = nullptr;
		const size_t patched = RedirectBoardSlot(vtables, count, 0,
			reinterpret_cast<void*>(&SystemSwitches::ReadPort), &original);
		if (patched == 0)
		{
			// The pattern matched a function nothing dispatches through, which would mean the
			// accessor has been inlined or moved out of the vtables. Fail loudly rather than leave
			// switches that silently do nothing.
			DebugLog("[%s] board I/O accessor is in no vtable - Test / Service unavailable.\n",
				gGeneral.GetGameTag());
			return;
		}
		SystemSwitches::orgReadPort = reinterpret_cast<SystemSwitches::read_port_fn>(original);
		DebugLogFile("[%s] Test / Service switches installed (%zu vtable entries)\n",
			gGeneral.GetGameTag(), patched);
	}

	void SetDrivingAxes(float steer, float throttle, float brake)
	{
		// -1..+1 about the centre, so full lock either way reaches 0x00 / 0xFF and neutral is
		// exactly 0x80. 127.0f rather than 128.0f keeps +1 inside the byte without a clamp.
		const auto toByte = [](float value, float centre, float scale)
		{
			const float raw = centre + value * scale;
			const int rounded = static_cast<int>(raw + 0.5f);
			return static_cast<uint32_t>(rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded));
		};

		const uint32_t packed = toByte(steer, static_cast<float>(SystemSwitches::ADC_CENTRE), 127.0f)
			| toByte(throttle, 0.0f, 255.0f) << 8
			| toByte(brake, 0.0f, 255.0f) << 16;
		SystemSwitches::axes.store(packed, std::memory_order_relaxed);
	}

	void SetSystemSwitches(bool test, bool service)
	{
		const uint8_t mask = static_cast<uint8_t>((test ? SystemSwitches::BIT_TEST : 0)
			| (service ? SystemSwitches::BIT_SERVICE : 0));
		SystemSwitches::heldMask.store(mask, std::memory_order_relaxed);
	}
}
