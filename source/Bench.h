#pragma once

#include <cstddef>
#include <cstdint>

// Frame-timing recorder for A/B performance runs: "-bench <file>" on the command line.
//
// Built into EVERY configuration on purpose. The numbers that matter are Release ones - Debug
// has the logging and the unoptimised emulator on top - and Release compiles DebugLog away, so
// this cannot ride on it. Off (one predictable branch per stamp) unless the switch is given.
//
// A host stamps a handful of points in its frame (see Mark); Finish() turns them into a
// key=value summary at <file> plus one CSV row per frame at <file>.csv. tools/ab/ab.py runs two
// builds against each other with it and compares the summaries - including `state_hash`, a hash
// of emulated work RAM on the last frame, which is what says an optimisation left the simulation
// untouched rather than merely faster.
namespace Bench
{
	enum class Mark : uint8_t
	{
		FrameStart,    // top of the host frame; also closes the previous frame
		ModuleStart,   // around module_main
		ModuleEnd,
		SubmitStart,   // around the host's per-frame GPU submit/flush
		SubmitEnd,
		PresentStart,  // around the swap chain Present
		PresentEnd,
		Count
	};

	// Parses "-bench <file>". Call once, before any host runs.
	void Configure(const wchar_t* cmdLine);

	extern bool g_enabled;
	inline bool Enabled() { return g_enabled; }

	void StampImpl(Mark mark);
	inline void Stamp(Mark mark)
	{
		if (g_enabled) StampImpl(mark);
	}

	// Called by the host once it has put the simulation in its reproducible starting state (see
	// LJHost's Run). Frames before it are discarded - their count depends on how fast the ROM
	// loaded - so the warmup and every statistic count from here.
	void Anchor();

	// Writes the report. stateHash/romFrame are whatever the host can say about the simulation's
	// end state (0 when it cannot); two runs of the same inputs must agree on them.
	// `ram`/`ramSize`, when given, are written raw to <file>.ram so a hash mismatch between two
	// builds can be narrowed to the bytes that differ (tools/ab/ab.py does this per 64 KB chunk).
	void Finish(uint32_t stateHash, uint32_t romFrame, const void* ram = nullptr, size_t ramSize = 0);
}
