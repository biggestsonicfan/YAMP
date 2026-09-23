#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>

// Paces a host loop to a fixed period without burning a core for the whole wait.
//
// The hosts used to spin on QueryPerformanceCounter until the period elapsed, which pinned the
// main thread at ~80% of a core for a game whose frame is ~6 ms of work in 16.7 (measured with
// tools/ab). This sleeps on a HIGH-RESOLUTION waitable timer until shortly before the deadline
// and spins only the last stretch, so the deadline is met exactly as before.
//
// High-resolution timers need Windows 10 1803+. Without one there is no sleep that is safe to
// take: an ordinary timer wakes on the system tick (up to 15.6 ms late), so the limiter falls
// back to the old pure spin rather than dropping frames.
class FrameLimiter
{
public:
	FrameLimiter()
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		m_freq = freq.QuadPart;
		// (Spelled out: the SDK only defines it for _WIN32_WINNT >= Win10, and this targets Win7.)
		constexpr DWORD CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_ = 0x00000002;
		m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_, TIMER_ALL_ACCESS);
		QueryPerformanceCounter(&m_last);
	}
	~FrameLimiter()
	{
		if (m_timer != nullptr) CloseHandle(m_timer);
	}
	FrameLimiter(const FrameLimiter&) = delete;
	FrameLimiter& operator=(const FrameLimiter&) = delete;

	int64_t Frequency() const { return m_freq; }

	// Returns once `periodTicks` (QueryPerformanceCounter ticks) have passed since the previous
	// return. 0 = no limit. The period is measured from the previous RETURN, not accumulated, so
	// a slow frame is not followed by a burst of fast ones - the behaviour of the spin it replaces.
	void Wait(int64_t periodTicks)
	{
		const int64_t deadline = m_last.QuadPart + periodTicks;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);

		if (m_timer != nullptr)
		{
			// Timer wake-up latency on a high-resolution timer is well under this.
			const int64_t margin = m_freq / 1000;
			const int64_t sleepTicks = deadline - now.QuadPart - margin;
			if (sleepTicks > 0)
			{
				// Relative due time, in 100 ns units (negative = relative).
				LARGE_INTEGER due;
				due.QuadPart = -(sleepTicks * 10000000 / m_freq);
				if (due.QuadPart < 0 && SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE))
				{
					WaitForSingleObject(m_timer, INFINITE);
				}
				QueryPerformanceCounter(&now);
			}
		}

		while (now.QuadPart < deadline)
		{
			YieldProcessor();
			QueryPerformanceCounter(&now);
		}
		m_last = now;
	}

private:
	int64_t m_freq = 1;
	HANDLE m_timer = nullptr;
	LARGE_INTEGER m_last{};
};
