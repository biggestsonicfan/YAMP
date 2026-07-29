#pragma once

#include <cstddef>
#include <cstdint>

namespace m2ftg
{
		// Sampling profiler for the emulated i960.
		//
		// The call-stack pane can only read the machine between frames, and once the module
		// yields per frame the board is always parked in the yield handler - so there is never
		// a live call frame to walk and the pane is honest but useless. The work happens
		// *inside* module_main, where nothing outside can observe it.
		//
		// RamExecFetch::FetchExec runs for every instruction the CPU executes, which is exactly
		// the inside view. Sampling the IP there and bucketing it gives a profile of where the
		// program actually spends its frame - which is what identifies a per-frame code path
		// (a geometry submit, say) that a between-frames sample can never see.
		namespace I960Profile
		{
			// 64-byte buckets over the 1 MB program ROM: fine enough to separate small
			// functions, small enough to keep the table at 64 KB and the hot path to an
			// increment.
			inline constexpr uint32_t BUCKET_SHIFT = 6;
			inline constexpr uint32_t ROM_SIZE = 0x100000;
			inline constexpr uint32_t BUCKET_COUNT = ROM_SIZE >> BUCKET_SHIFT;
			// One sample every 1024 instructions. At a few million instructions a frame that is
			// still thousands of samples a second, and the cost is a masked compare.
			inline constexpr uint32_t SAMPLE_MASK = 0x3FF;

			namespace detail
			{
				extern uint32_t g_buckets[BUCKET_COUNT];
				extern uint32_t g_counter;
				extern uint64_t g_samples;
			}

			inline void NoteFetch(uint32_t ip)
			{
				if ((++detail::g_counter & SAMPLE_MASK) != 0)
				{
					return;
				}
				if (ip < ROM_SIZE)
				{
					detail::g_buckets[ip >> BUCKET_SHIFT]++;
					detail::g_samples++;
				}
			}

			void Reset();
			uint64_t TotalSamples();
			uint32_t Bucket(size_t index);
		}

		// Draws the StF DLL's own "dw" debug-menu windows (DEBUG MENU / CONFIG / PERFORMANCE /
		// 960STAT) as ImGui windows. The window layout, item labels, bound variables and action
		// handlers all come from the descriptor tree inside the game DLL itself; YAMP only
		// interprets that data. No-op unless the game is StF, the DLL is loaded and booted, and
		// the "Display debugging features" debug setting is enabled.
		void DrawDebugWindows();

		// Keeps the game's own debug flag in emulated RAM (the dword at 0x508000, flipped by
		// XOR with 0x24) in sync with the "Set the game's debug flag" debug setting, writing
		// through the DLL's own memory-map dispatch. Call once per frame; no-op unless the
		// game is StF and the board has booted.
		void UpdateGameDebugFlag();
	}

