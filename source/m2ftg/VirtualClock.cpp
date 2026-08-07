#include "VirtualClock.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "../DebugLog.h"
#include "../Utils/MemoryMgr.h"

namespace
{
	// The value the patched wrapper returns. A plain file-static because the patch encodes its
	// ADDRESS as an immediate - it cannot live in the object, which would tie the patch to an
	// instance's lifetime.
	//
	// Starts at a large non-zero value rather than 0: the module takes differences between clock
	// reads, and a few of them are taken before the first Advance(), so a zero origin would make
	// those deltas read as "since the epoch" on the first frame. Any fixed constant does; this one
	// is far from both 0 and any plausible overflow.
	int64_t g_ticks = 0x0000'0100'0000'0000;
}

m2ftg::VirtualClock& m2ftg::ModuleClock()
{
	static VirtualClock s_clock;
	return s_clock;
}

bool m2ftg::VirtualClock::Install(void* wallClockFn)
{
	if (wallClockFn == nullptr)
	{
		// Not an error: only the games whose pattern resolved get a virtual clock, and a module
		// left on real time behaves exactly as it did before this file existed.
		return false;
	}

	// The step, in the units the module's own QueryPerformanceFrequency reports. Read locally
	// rather than assumed: on current Windows this is 10 MHz everywhere, but the correctness
	// argument only needs the step to be SHORTER than the board's frame period, and freq/120 is
	// that for any board slower than 120 Hz.
	//
	// Machines may disagree on the frequency, and that is fine - it changes how many calls a peer
	// needs per frame, not what the board computes. See the header.
	LARGE_INTEGER freq = {};
	if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0)
	{
		return false;
	}
	m_step = freq.QuadPart / 120;
	m_frequency = freq.QuadPart;
	if (m_step <= 0)
	{
		return false;
	}

	// MOV RAX, &g_ticks ; MOV RAX, [RAX] ; RET
	//
	// An absolute immediate rather than a RIP-relative load: YAMP.exe and the module are separate
	// images and nothing guarantees they land within the +-2GB a RIP displacement can reach.
	//
	// The wrapper is 34 bytes (its own frame, the indirect call and the return), so 14 bytes of
	// replacement fit inside it with room to spare, and nothing jumps into its middle - it is a
	// leaf whose only entry is its first byte.
	const uint64_t address = reinterpret_cast<uint64_t>(&g_ticks);
	const auto byteOf = [address](int i) { return static_cast<uint8_t>(address >> (i * 8)); };
	Memory::VP::Patch(wallClockFn, {
		0x48, 0xB8,                                                     // MOV RAX, imm64
		byteOf(0), byteOf(1), byteOf(2), byteOf(3),
		byteOf(4), byteOf(5), byteOf(6), byteOf(7),
		0x48, 0x8B, 0x00,                                               // MOV RAX, [RAX]
		0xC3,                                                           // RET
	});

	m_installed = true;
	DebugLogFile("[m2ftg] virtual clock installed at %p, step %lld ticks (freq %lld)\n",
		wallClockFn, static_cast<long long>(m_step), static_cast<long long>(freq.QuadPart));
	return true;
}

void m2ftg::VirtualClock::Advance()
{
	if (!m_installed)
	{
		return;
	}
	g_ticks += m_step;
}

int64_t m2ftg::VirtualClock::Ticks() const
{
	return g_ticks;
}

unsigned int m2ftg::VirtualClock::BoardFramesDue(bool linkLive)
{
	// Four board frames is 1/15 s of catch-up per host frame - enough to ride out a stutter,
	// far too little to let a two-second hitch turn into a two-second burst.
	constexpr unsigned int kMaxCatchUp = 4;

	if (!m_installed || !linkLive)
	{
		// Solo play keeps the old policy exactly: one frame per host frame, and a host that
		// cannot present at 60 Hz runs the game slow. Nobody is disadvantaged by that but the
		// player who chose the settings, and catching up costs a stutter they did not ask for.
		m_boardDue = 0;
		return 1;
	}

	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);
	const int64_t period = m_frequency / 60;

	if (m_boardDue == 0)
	{
		m_boardDue = now.QuadPart + period;
		return 1;
	}

	unsigned int due = 1;
	m_boardDue += period;
	while (m_boardDue <= now.QuadPart && due < kMaxCatchUp)
	{
		++due;
		m_boardDue += period;
	}
	if (m_boardDue <= now.QuadPart)
	{
		// Past the cap: drop the outstanding debt rather than carry it into the next frame,
		// where it would still be unpayable and would keep the board in permanent catch-up.
		m_boardDue = now.QuadPart + period;
	}
	return due;
}

void m2ftg::VirtualClock::PaceToVirtualTime()
{
	if (!m_installed)
	{
		return;   // the module still paces itself off real time; nothing to do
	}

	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);

	// One board frame per host frame, at 60 Hz.
	//
	// An earlier version paced against the virtual time the frame CONSUMED, on the reasoning that
	// this is the module's own period and so needs no hardcoded rate. It does not work: the
	// consumed time is quantised to the sub-frame step, so a period near 1/60 s reads as either
	// two steps (60 Hz) or one (120 Hz), and the average landed at 63.7 Hz. A signal quantised
	// coarser than the thing being measured cannot measure it.
	//
	// So the rate is stated rather than inferred, and 60 is the number the rest of YAMP uses -
	// every other host caps here with the same freq/60 idiom behind "Enable 60 FPS cap".
	const int64_t consumed = g_ticks - m_lastPacedTicks;
	m_lastPacedTicks = g_ticks;

	if (m_deadline == 0 || consumed <= 0)
	{
		// First frame, or a frame that advanced no board time at all (a lockstep stall, or the
		// boot phases before the ROM runs). Re-baseline instead of pacing: there is no frame here
		// to be slow or fast, and holding a deadline across a stall is what would make the game
		// sprint afterwards to repay time it never owed.
		m_deadline = now.QuadPart;
		return;
	}

	m_deadline += m_frequency / 60;

	// Behind already: run flat out and reset the deadline rather than accumulating a debt. A host
	// that cannot sustain the board's rate should run slow, not stutter and then sprint.
	if (m_deadline <= now.QuadPart)
	{
		m_deadline = now.QuadPart;
		return;
	}

	// Sleep the bulk of the wait, then spin the remainder. Sleep alone overshoots by up to a
	// scheduler tick, which at ~17 ms a frame is a visible stutter; spinning alone burns a core.
	for (;;)
	{
		QueryPerformanceCounter(&now);
		const int64_t remaining = m_deadline - now.QuadPart;
		if (remaining <= 0)
		{
			break;
		}
		const int64_t remainingMs = (remaining * 1000) / m_frequency;
		if (remainingMs > 1)
		{
			Sleep(static_cast<DWORD>(remainingMs - 1));
		}
	}
}
