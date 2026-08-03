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

		// Reads 32 bits of emulated i960 memory through the DLL's own memory-map dispatch.
		// Returns false (leaving `out` untouched) if the module is not loaded or the address
		// has no reader in the map.
		bool ReadEmulatedRam32(uint32_t address, uint32_t& out);

		// Holds Sonic the Fighters' GAME ASSIGNMENTS -> DAMAGE item at the value the YAMP dip
		// switch (or, during netplay, the ROOM) selects, by writing bit 0x80 of the ROM's
		// game_assignments_flag in emulated RAM. Call once per EMULATED frame, from the module
		// thread, before module_main - not from the UI thread, whose timing relative to the
		// emulator differs between machines. No-op unless the game is StF and the board has booted.
		void UpdateDamageAssignment();

		// True once the emulated board has finished booting (the DLL's own phase dword
		// +0x6B9300 reaches 2, which only happens after module_main has run for a while).
		//
		// EVERY determinism helper below is gated on this and quietly returns false before it,
		// so a netplay round MUST NOT begin until it is true. A guest that joins an already
		// waiting host reaches the barrier within a couple of hundred milliseconds of launching
		// - long before its board is up - and would then start a match with no reset, no shared
		// RNG seed and no texture-budget pin, while the host (which had been waiting, booted)
		// applied all three. The result is an emulated CPU rolling its own random numbers on one
		// side only: an AI desync that looks like a network fault and is not one.
		bool IsBoardBooted();

		// Re-runs the DLL's own i960 CPU/board initialisation - the DEBUG MENU's "RESET" item
		// (handler at DLL+0x4C840), which unlike STEP/GO is NOT a stub in retail.
		//
		// Netplay uses this to give both machines an identical starting state. Lockstep keeps two
		// emulators in step only if they START in the same state; merely beginning to exchange
		// inputs at the same moment does nothing if one side has already been running attract mode
		// for thirty seconds. Resetting both at the barrier makes "frame 0" mean the same thing.
		//
		// Returns false if the game is not StF or the board has not booted (dword +0x6B9300 != 2),
		// in which case nothing was called.
		bool ResetBoard();

		// Seeds the host RNG that the ROM's `rand` HLE hook draws from, so both machines in a
		// netplay match produce identical "random" values.
		//
		// The ROM's rand is HLE'd: handler DLL+0x53070 calls the DLL's own generator
		// (DLL+0x8D40) and writes the result into i960 g0. That generator is a MERSENNE TWISTER
		// (N=624, M=397, the standard tempering shifts) whose state object lives at
		// *(*(DLL+0x68BB88) + 0x20): u32 state[624] at +0x08, circular index at +0x9C8.
		// Normally it is seeded per-process, so two machines roll different numbers - which is
		// exactly what makes the CPU behave differently on each client in a netplay match.
		//
		// This re-runs the standard MT init_genrand with the shared match seed. Returns false if
		// the game is not StF, the board has not booted, or the state object fails a sanity check
		// (in which case nothing was written).
		bool SeedHostRng(uint32_t seed);

		// Makes the ROM's texture-upload budget deterministic, which netplay requires.
		//
		// Four Core HLE hooks (all sharing handler DLL+0x52FD0) answer the ROM's "have 9 ms
		// elapsed?" question by reading a WALL CLOCK and writing the boolean into i960 g0. The
		// unpack loop yields on that answer, so a fast machine and a slow one do different amounts
		// of work in the same emulated frame and their states diverge - a desync that no amount of
		// input synchronisation can fix, because the divergence is not in the inputs.
		//
		// Enabling this repoints those table entries at a wrapper that runs the original handler
		// (for its instruction-length return value) and then overwrites g0 with a constant "budget
		// not expired", so the loop always completes the same work on both machines. The cost is
		// smoothness during big uploads; the benefit is a reproducible simulation.
		//
		// Returns false if the game is not StF, the board has not booted, or no entries matched.
		bool SetTextureBudgetDeterministic(bool enable);

		// The address in EMULATED RAM of the ROM's own per-frame counter - the value a netplay
		// round anchors frame 0 to and desync-checks against every frame. 0 means this game has
		// no measured counter, which disables netplay for it rather than anchoring on a value
		// that is not a frame count.
		uint32_t RomFrameCounterAddress();

		// Whether this game can currently sustain a netplay session.
		//
		// Separate from RomFrameCounterAddress() on purpose: VF2's counter IS measured and
		// correct, but VF2 still diverges by one emulated frame (see docs/vf2-hle-hooks.md), so
		// it must not offer netplay while that is unresolved. Tying the two together would mean
		// either shipping a known-diverging game or throwing away a measured fact to express
		// "not yet".
		bool NetplaySupported();

		// Sets the module's is_vf20 config byte, which selects Virtua Fighter 2 version 2.0 or
		// 2.1. The two are different games mechanically, so netplay has to agree on it.
		//
		// Deliberately NOT a live setting: nothing re-reads it per frame. The module's
		// backup-RAM injector (HLE hook 8) reads it each time the ROM initialises the board, so
		// a write here takes effect on the NEXT board init - which is exactly what a netplay
		// round already does before frame 0. Writing it without a reset changes nothing.
		//
		// Returns false, having written nothing, for a game with no such byte.
		bool SetVf2Version20(bool version20);

		// The value this peer submits to the desync canary for the current emulated frame.
		//
		// StF and FV submit the ROM's frame_counter, which advances exactly once per
		// module_main in those games. VF2 hashes a slice of work RAM instead: its counter is
		// bumped by an interrupt that can land either side of the module_main boundary, so it
		// jitters by a frame even when the two simulations are identical - measured, and it was
		// producing false desyncs on a game that was in sync. See the DwGame comment for the
		// evidence. 0 if no game is running.
		uint32_t StateCheckValue();

	}

