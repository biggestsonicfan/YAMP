#include "Bench.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "psapi")

namespace Bench
{
	bool g_enabled = false;

	namespace
	{
		using Frame = std::array<int64_t, static_cast<size_t>(Mark::Count)>;

		std::wstring g_path;
		// Frames before this are boot (ROM load, first PSO compiles) and are left out of every
		// statistic, so a change that only moves boot cost does not read as a per-frame one.
		uint32_t g_warmup = 120;
		std::vector<Frame> g_frames;
		int64_t g_qpcFreq = 1;

		// Taken when the first post-warmup frame starts: CPU and wall time are reported for the
		// measured window only, so boot does not dilute them.
		bool g_windowOpen = false;
		int64_t g_windowQpc = 0;
		uint64_t g_windowProcessCpu = 0;
		uint64_t g_windowThreadCpu = 0;
		int64_t g_firstFrameQpc = 0;
		int64_t g_anchorQpc = 0;

		int64_t Now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		uint64_t FileTime100ns(const FILETIME& ft)
		{
			return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
		}

		uint64_t ProcessCpu()
		{
			FILETIME c, e, k, u;
			GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
			return FileTime100ns(k) + FileTime100ns(u);
		}

		// The host frame loop runs on the main thread, and Finish is called from it too.
		uint64_t ThreadCpu()
		{
			FILETIME c, e, k, u;
			GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
			return FileTime100ns(k) + FileTime100ns(u);
		}

		double Ms(int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_qpcFreq); }

		const wchar_t* ArgAfter(const wchar_t* cmdLine, const wchar_t* flag)
		{
			const wchar_t* p = wcsstr(cmdLine, flag);
			if (p == nullptr) return nullptr;
			p += wcslen(flag);
			while (*p == L' ') ++p;
			return p;
		}

		// One statistic over the measured frames. A negative sample means a mark was not stamped
		// that frame (a paused or stalled frame skips module_main), and is left out.
		template<typename F>
		void WriteStat(FILE* f, const char* name, F&& sample)
		{
			std::vector<double> v;
			v.reserve(g_frames.size());
			for (size_t i = g_warmup; i < g_frames.size(); ++i)
			{
				const double s = sample(i);
				if (s >= 0.0) v.push_back(s);
			}
			if (v.empty())
			{
				fprintf(f, "%s_n=0\n", name);
				return;
			}
			double sum = 0.0;
			for (double s : v) sum += s;
			std::sort(v.begin(), v.end());
			auto pct = [&v](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))]; };
			fprintf(f, "%s_n=%zu\n%s_avg=%.4f\n%s_p50=%.4f\n%s_p95=%.4f\n%s_p99=%.4f\n%s_max=%.4f\n",
				name, v.size(), name, sum / static_cast<double>(v.size()), name, pct(0.50),
				name, pct(0.95), name, pct(0.99), name, v.back());
		}

		double Span(size_t i, Mark from, Mark to)
		{
			const int64_t a = g_frames[i][static_cast<size_t>(from)];
			const int64_t b = g_frames[i][static_cast<size_t>(to)];
			return (a != 0 && b != 0) ? Ms(b - a) : -1.0;
		}
	}

	void Configure(const wchar_t* cmdLine)
	{
		const wchar_t* path = ArgAfter(cmdLine, L"-bench ");
		if (path == nullptr || *path == L'\0' || *path == L'-') return;

		// Quoted or bare, up to the next space.
		const wchar_t end = (*path == L'"') ? L'"' : L' ';
		if (*path == L'"') ++path;
		const wchar_t* stop = wcschr(path, end);
		g_path.assign(path, stop != nullptr ? stop : path + wcslen(path));

		if (const wchar_t* w = ArgAfter(cmdLine, L"-bench-warmup ")) g_warmup = static_cast<uint32_t>(_wtoi(w));

		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		g_qpcFreq = freq.QuadPart;
		g_frames.reserve(1 << 16);
		g_enabled = true;
	}

	void Anchor()
	{
		if (!g_enabled) return;
		g_anchorQpc = Now();
		g_frames.clear();
		g_windowOpen = false;
	}

	void StampImpl(Mark mark)
	{
		const int64_t t = Now();
		if (mark == Mark::FrameStart)
		{
			if (g_firstFrameQpc == 0) g_firstFrameQpc = t;
			if (!g_windowOpen && g_frames.size() == g_warmup)
			{
				g_windowOpen = true;
				g_windowQpc = t;
				g_windowProcessCpu = ProcessCpu();
				g_windowThreadCpu = ThreadCpu();
			}
			g_frames.push_back(Frame{});
		}
		if (!g_frames.empty()) g_frames.back()[static_cast<size_t>(mark)] = t;
	}

	void Finish(uint32_t stateHash, uint32_t romFrame, const void* ram, size_t ramSize)
	{
		if (!g_enabled) return;
		g_enabled = false;
		const int64_t endQpc = Now();
		const uint64_t processCpu = ProcessCpu();
		const uint64_t threadCpu = ThreadCpu();

		FILE* f = nullptr;
		if (_wfopen_s(&f, g_path.c_str(), L"w") != 0 || f == nullptr) return;

		const size_t measured = g_frames.size() > g_warmup ? g_frames.size() - g_warmup : 0;
		fprintf(f, "frames=%zu\nwarmup=%u\nmeasured=%zu\n", g_frames.size(), g_warmup, measured);
		fprintf(f, "state_hash=0x%08X\nrom_frame=%u\n", stateHash, romFrame);

		// Boot: process creation to the first host frame, and on to the anchor (the ROM loaded
		// and the board up).
		{
			FILETIME c, e, k, u, now;
			GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
			GetSystemTimeAsFileTime(&now); // (the Precise one is Win8+; the project targets Win7)
			const double sinceCreate = static_cast<double>(FileTime100ns(now) - FileTime100ns(c)) / 1e7;
			fprintf(f, "boot_s=%.4f\n", sinceCreate - Ms(endQpc - g_firstFrameQpc) / 1000.0);
			if (g_anchorQpc != 0) fprintf(f, "anchor_s=%.4f\n", sinceCreate - Ms(endQpc - g_anchorQpc) / 1000.0);
		}

		if (g_windowOpen && measured > 0)
		{
			const double wall = Ms(endQpc - g_windowQpc) / 1000.0;
			fprintf(f, "wall_s=%.4f\nfps=%.3f\n", wall, static_cast<double>(measured) / wall);
			// Percent of ONE core over the measured window.
			fprintf(f, "cpu_process_pct=%.2f\n", static_cast<double>(processCpu - g_windowProcessCpu) / 1e5 / wall);
			fprintf(f, "cpu_mainthread_pct=%.2f\n", static_cast<double>(threadCpu - g_windowThreadCpu) / 1e5 / wall);
		}

		PROCESS_MEMORY_COUNTERS_EX mem{};
		if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem)))
		{
			fprintf(f, "peak_working_set_mb=%.2f\nprivate_mb=%.2f\npeak_private_mb=%.2f\n",
				static_cast<double>(mem.PeakWorkingSetSize) / 1048576.0,
				static_cast<double>(mem.PrivateUsage) / 1048576.0,
				static_cast<double>(mem.PeakPagefileUsage) / 1048576.0);
		}
		DWORD handles = 0;
		GetProcessHandleCount(GetCurrentProcess(), &handles);
		fprintf(f, "handles=%lu\n", handles);

		const size_t last = g_frames.size();
		// frame: start to the next start - everything, including the limiter and vsync.
		WriteStat(f, "frame_ms", [last](size_t i) {
			return i + 1 < last ? Ms(g_frames[i + 1][0] - g_frames[i][0]) : -1.0; });
		// work: start to Present - the CPU cost of a frame, before any waiting.
		WriteStat(f, "work_ms", [](size_t i) { return Span(i, Mark::FrameStart, Mark::PresentStart); });
		WriteStat(f, "module_ms", [](size_t i) { return Span(i, Mark::ModuleStart, Mark::ModuleEnd); });
		WriteStat(f, "submit_ms", [](size_t i) { return Span(i, Mark::SubmitStart, Mark::SubmitEnd); });
		WriteStat(f, "present_ms", [](size_t i) { return Span(i, Mark::PresentStart, Mark::PresentEnd); });
		// host: the work that is YAMP's own, i.e. not module_main.
		WriteStat(f, "host_ms", [](size_t i) {
			const double work = Span(i, Mark::FrameStart, Mark::PresentStart);
			const double mod = Span(i, Mark::ModuleStart, Mark::ModuleEnd);
			return (work >= 0.0 && mod >= 0.0) ? work - mod : -1.0; });
		fclose(f);

		if (ram != nullptr && ramSize != 0 && _wfopen_s(&f, (g_path + L".ram").c_str(), L"wb") == 0 && f != nullptr)
		{
			fwrite(ram, 1, ramSize, f);
			fclose(f);
		}

		// Per-frame detail for digging into a regression the summary only hints at.
		if (_wfopen_s(&f, (g_path + L".csv").c_str(), L"w") == 0 && f != nullptr)
		{
			fprintf(f, "frame,frame_ms,work_ms,module_ms,submit_ms,present_ms\n");
			for (size_t i = 0; i < last; ++i)
			{
				fprintf(f, "%zu,%.4f,%.4f,%.4f,%.4f,%.4f\n", i,
					i + 1 < last ? Ms(g_frames[i + 1][0] - g_frames[i][0]) : -1.0,
					Span(i, Mark::FrameStart, Mark::PresentStart), Span(i, Mark::ModuleStart, Mark::ModuleEnd),
					Span(i, Mark::SubmitStart, Mark::SubmitEnd), Span(i, Mark::PresentStart, Mark::PresentEnd));
			}
			fclose(f);
		}
	}
}
