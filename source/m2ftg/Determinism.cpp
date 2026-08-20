#include "Determinism.h"

#include "Debug/DwGame.h"
#include "ModuleBuild.h"
#include "../YAMPGeneral.h"
#include "../YAMPSettings.h"
#include "../DebugLog.h"
#include "../net/NetPlugin.h"
#include "../net/StateHash.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>

using namespace m2ftg::dwdbg;

namespace
{
	// --- Deterministic texture-upload budget (see the header) --------------------------------
	// (The table, its length and the handler to repoint all come from DwGame - StF's budget
	// handler is 0x52FD0 across 4 records, FV's is 0x51C20 across 4 of its own.)
	struct HleEntry
	{
		uint32_t romOffset;
		uint32_t padding;
		uint64_t handler;
	};
	static_assert(sizeof(HleEntry) == 0x10, "HLE record is 16 bytes in the DLL");

	using HleHandlerFn = unsigned long long (*)(unsigned int);

	HleHandlerFn g_originalTexBudget = nullptr;
	uint8_t* g_texBudgetBase = nullptr;
	// Captured when the wrapper is installed, so the hot path needs no game lookup.
	uintptr_t g_texBudgetCtxRva = 0;

	// Runs the DLL's own handler first so its return value (the trapped instruction's length, 4 or
	// 8) stays exactly right, then replaces the wall-clock answer it wrote into g0.
	unsigned long long DeterministicTexBudget(unsigned int index)
	{
		const unsigned long long length = g_originalTexBudget(index);

		if (g_texBudgetBase != nullptr)
		{
			uint8_t* ctx = *reinterpret_cast<uint8_t**>(g_texBudgetBase + g_texBudgetCtxRva);
			if (ctx != nullptr)
			{
				// g0 = 0: "the budget has not expired", so the unpack loop runs to completion
				// instead of stopping wherever this machine happened to be after the deadline
				// (9 ms in StF; 12 ms, or 8 in some master states, in FV).
				*reinterpret_cast<uint32_t*>(ctx + 0x98) = 0;
			}
		}
		return length;
	}
}

bool m2ftg::SetTextureBudgetDeterministic(bool enable)
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (!BoardBooted(game, base))
	{
		return false;
	}

	g_texBudgetBase = base;
	g_texBudgetCtxRva = game->rvaCpuCtxPtr;
	const uint64_t original = reinterpret_cast<uint64_t>(base + game->rvaTexBudgetHandler);
	const uint64_t replacement = reinterpret_cast<uint64_t>(&DeterministicTexBudget);

	// The table lives in .data and the installer writes it at boot, so it is already writable -
	// no VirtualProtect dance needed.
	auto* table = reinterpret_cast<HleEntry*>(base + game->rvaHleTable);
	size_t patched = 0;
	for (size_t i = 0; i < game->hleCount; ++i)
	{
		if (enable && table[i].handler == original)
		{
			if (g_originalTexBudget == nullptr)
			{
				g_originalTexBudget = reinterpret_cast<HleHandlerFn>(original);
			}
			table[i].handler = replacement;
			++patched;
		}
		else if (!enable && table[i].handler == replacement)
		{
			table[i].handler = original;
			++patched;
		}
	}
}

namespace
{
	// Layout from the generator (StF DLL+0x8D40 / FV DLL+0x8D00, the same code): the holder
	// global points at a struct whose slots hold Mersenne Twister state objects.
	constexpr uintptr_t MT_OFF_STATE = 0x008;    // u32 [624]
	constexpr uintptr_t MT_OFF_INDEX = 0x9C8;    // circular index, 0..623
	constexpr uint32_t MT_N = 624;

	// Seeds one twister in place. Returns false without writing anything if the object fails
	// its sanity check, so a wrong address costs nothing rather than scribbling 2.5 KB.
	static bool SeedOneTwister(uint8_t* holder, uintptr_t slot, uint32_t seed) noexcept
	{
		__try
		{
			uint8_t* mt = *reinterpret_cast<uint8_t**>(holder + slot);
			if (mt == nullptr)
			{
				return false;
			}

			// A live twister always has its index inside the state array.
			uint32_t& index = *reinterpret_cast<uint32_t*>(mt + MT_OFF_INDEX);
			if (index >= MT_N)
			{
				return false;
			}

			// Standard init_genrand:
			//     mt[0] = seed; mt[i] = 1812433253 * (mt[i-1] ^ (mt[i-1]>>30)) + i.
			uint32_t* state = reinterpret_cast<uint32_t*>(mt + MT_OFF_STATE);
			state[0] = seed;
			for (uint32_t i = 1; i < MT_N; ++i)
			{
				const uint32_t prev = state[i - 1];
				state[i] = 1812433253u * (prev ^ (prev >> 30)) + i;
			}
			// This generator twists in place off a circular index rather than regenerating in
			// blocks, so a freshly seeded state starts at element 0.
			index = 0;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}

bool m2ftg::SeedHostRng(uint32_t seed)
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (!BoardBooted(game, base))
	{
		return false;
	}

	uint8_t* holder = nullptr;
	__try
	{
		holder = *reinterpret_cast<uint8_t**>(base + game->rvaRngHolder);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	if (holder == nullptr)
	{
		return false;
	}

	// EVERY stream the ROM draws from has to be seeded, and each one gets a DIFFERENT seed.
	//
	// FV has two: `rand` (hook 43) and the VS stage picker (hook 30), which is its own
	// generator object rather than a second draw from the first. Seeding only `rand` would
	// leave both peers agreeing on the fight and disagreeing on the stage - a desync that
	// happens before the first frame of the round and looks nothing like an RNG problem.
	//
	// Deriving each stream's seed from the shared one keeps a single value on the wire while
	// making sure two streams seeded from the same match do not run in lockstep with each
	// other, which would be a needless correlation between unrelated draws.
	// EVERY stream must seed, not merely one of them.
	//
	// This returned "any succeeded" until 2026-08-02, which is a silent desync generator: a game
	// whose second twister failed its sanity check still reported success, the round started, and
	// the two peers then agreed on the fight and disagreed on the stage - the exact failure the
	// per-stream list exists to prevent. A determinism precondition has to be all-or-nothing, and
	// refusing the round is always better than playing a diverging one.
	bool allSeeded = game->rngStreamCount != 0;
	for (size_t i = 0; i < game->rngStreamCount; ++i)
	{
		// Weyl-style decorrelation; any fixed odd stride would do.
		const uint32_t streamSeed = seed + static_cast<uint32_t>(i) * 0x9E3779B9u;
		const bool ok = SeedOneTwister(holder, game->rngStreams[i], streamSeed);
		if (!ok)
		{
			allSeeded = false;
		}
		// Logged per stream: "seeding failed" on its own does not say WHICH generator, and the
		// second one is invisible in-game until a stage is picked.
		DebugLogFile("[rng] stream %zu (holder+0x%02X) seed=0x%08X -> %s\n",
			i, static_cast<unsigned>(game->rngStreams[i]), streamSeed, ok ? "ok" : "FAILED");
	}
	return allSeeded;
}

bool m2ftg::ReadEmulatedRam32(uint32_t address, uint32_t& out)
{
	uint8_t* base = ModuleBase();
	if (base == nullptr)
	{
		return false;
	}
	return ReadEmulated32(base, address, out);
}

bool m2ftg::IsBoardBooted()
{
	return BoardBooted(CurrentDw(), ModuleBase());
}

bool m2ftg::ResetBoard()
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	// Calling the reset before the board has finished booting would re-init a CPU whose ROM is
	// still loading, so gate on the same boot dword the menu actions use.
	if (!BoardBooted(game, base))
	{
		return false;
	}

	// A LINKED-CABINET GAME LOSES ITS SECOND BOARD HERE unless we put it back. Sampled before the
	// reset rather than forced afterwards, so this preserves how many cabinets were running
	// instead of deciding it - `-von-1board` stays single-board, and a reset never silently
	// changes the shape of the machine.
	const uint8_t gateBefore = (game->rvaTwoBoardGate != 0)
		? *(base + game->rvaTwoBoardGate) : 0;

	// The DEBUG MENU's "RESET" item: writes run-state 0 and calls the DLL's real i960/board
	// init. Unlike STEP/GO/REGS it is NOT a stub in either retail build.
	InvokeDwAction(base + game->rvaResetHandler);

	// RESET zeroes the two-board gate and then calls the module's board reset, which reads that
	// gate to decide whether to reset the SECOND board - so board 1's CPU is left holding a stale
	// context while board 0 restarts. Restore the gate and run that same reset again; the second
	// pass takes the two-board path and brings both cabinets up exactly as boot does.
	//
	// Resetting board 0 twice is deliberate and harmless - a reset is idempotent, and doing it
	// through the module's own routine is what keeps both boards in the state its code expects.
	// The alternative, reaching in to reset board 1 by hand, would be YAMP inventing a bring-up
	// sequence the module already has.
	if (gateBefore != 0 && game->rvaBoardResetAll != 0)
	{
		*(base + game->rvaTwoBoardGate) = gateBefore;
		InvokeDwAction(base + game->rvaBoardResetAll);
	}
	return true;
}

void m2ftg::UpdateDamageAssignment()
{
	if (!gGeneral.IsSonicTheFighters())
	{
		return;
	}
	const YAMPSettings* settings = gGeneral.GetSettings();
	// NOT forced off during netplay, unlike the debug flag above - this one is meant to be on in
	// a match. What makes it safe there is that the value comes from the ROOM rather than from
	// this machine's settings, so both peers write the same bit; net::EffectiveRealDamage is the
	// single place that choice is made.
	const bool real = net::EffectiveRealDamage(settings != nullptr && settings->m_m2RealDamage);

	uint8_t* base = ModuleBase();
	if (base == nullptr)
	{
		return;
	}
	// Before the board is up there is no block to write, and the ROM's own init_game_assignments
	// runs during boot - a write landing before it would simply be overwritten. Through the
	// descriptor, not DW_STF: the boot-state RVA moved in Like a Dragon Gaiden's build.
	if (!BoardBooted(CurrentDw(), base))
	{
		return;
	}

	uint32_t value = 0;
	if (!ReadEmulated32(base, GAME_ASSIGN_FLAG_DWORD, value))
	{
		return;
	}

	// Enforced every frame, but only WRITTEN when it differs. Re-asserting is what makes this
	// survive the ROM reloading its assignments (a board reset, or leaving the service menu),
	// which is exactly what happens at the start of every netplay round.
	const uint32_t wanted = real ? (value | DAMAGE_REAL_BIT) : (value & ~DAMAGE_REAL_BIT);
	if (wanted != value)
	{
		WriteEmulated32(base, GAME_ASSIGN_FLAG_DWORD, wanted);
	}
}

// Writes one byte of the module's config block, but only after proving the block is where we
// think it is.
//
// The config setters poke the module's own .data directly - there is no API for it - so a wrong
// base does not fail, it corrupts. The block's first dword is `kind`, a small enum naming the
// game ({0=vf2, 1=fv, 2=stf}), which makes the base cheap to verify at the moment of use: if it
// does not read back as this game, nothing is written and the caller sees false. That turns a
// mis-transcribed RVA into a setting that visibly does not apply instead of a random byte flipped
// somewhere in the module.
static bool WriteConfigByte(uintptr_t fieldOffset, uint8_t value) noexcept
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (game == nullptr || base == nullptr || game->rvaConfigBase == 0)
	{
		return false;
	}
	__try
	{
		uint8_t* config = base + game->rvaConfigBase;
		if (*reinterpret_cast<const uint32_t*>(config) != game->configKind)
		{
			return false;
		}
		*reinterpret_cast<volatile uint8_t*>(config + fieldOffset) = value;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

bool m2ftg::SetVf2Version20(bool version20)
{
	// The config block is plain .data the module memcpy'd in at module_start; no VirtualProtect.
	return WriteConfigByte(CONFIG_IS_VF20, version20 ? 1u : 0u);
}

bool m2ftg::SetVsMode(bool vsMode)
{
	// Same mechanism and the same window as SetVf2Version20: re-read by the backup-RAM injector
	// every time the ROM initialises the board, so a write here takes effect at the NEXT board
	// init. That is exactly what the round-start ResetBoard() is - so this must be called BEFORE
	// that reset, never after.
	return WriteConfigByte(CONFIG_IS_VS_MODE, vsMode ? 1u : 0u);
}

bool m2ftg::StateCheckIsHash()
{
	const DwGame* game = CurrentDw();
	// Same test StateCheckValue branches on, deliberately read from the same field rather than
	// listed per game: a game that gains a hash range gains the right comparison with it.
	return game != nullptr && game->stateHashLen != 0;
}

uint32_t m2ftg::StateCheckValue()
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (game == nullptr || base == nullptr)
	{
		return 0;
	}

	// No hash range configured: the ROM's own frame counter, as StF and FV have always used.
	if (game->stateHashLen == 0)
	{
		uint32_t v = 0;
		ReadEmulatedRam32(game->romFrameCounter, v);
		return v;
	}

	// Hash the host buffer directly rather than through the memory-map dispatch, which costs a
	// function pointer per dword - this runs once per emulated frame.
	__try
	{
		const uint8_t* ram = *reinterpret_cast<uint8_t* const*>(base + game->rvaRamBasePtr);
		if (ram == nullptr)
		{
			return 0;
		}
		const uint8_t* p = ram + game->stateHashBase;
		// Only the 32-bit RESULT goes on the wire (submit_state_check takes a uint32), so this
		// costs no bandwidth over the single counter it replaces - it is purely local work, and
		// it runs once per EMULATED frame, not once per host frame. The dword-FNV rationale is
		// with net::Fnv1aWords.
		return net::Fnv1aWords(p, game->stateHashLen);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}




bool m2ftg::ReadI960Timers(I960Timers& out)
{
	out = {};
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (!BoardBooted(game, base))
	{
		return false;
	}

	// The context pointer is written by the CPU init and survives a board reset, but this runs
	// from the module thread while the emulator is between frames - keep the same structured
	// net every other reader here uses rather than trusting the boot phase alone.
	__try
	{
		const uint8_t* ctx = *reinterpret_cast<uint8_t* const*>(base + game->rvaCpuCtxPtr);
		if (ctx == nullptr)
		{
			return false;
		}
		for (size_t i = 0; i < CTX_TIMER_COUNT; ++i)
		{
			const uint8_t* channel = ctx + CTX_TIMER_BASE + i * CTX_TIMER_STRIDE;
			out.enabled[i] = *channel;
			out.count[i] = *reinterpret_cast<const int32_t*>(channel + 4);
		}
		out.pending = *reinterpret_cast<const uint32_t*>(ctx + CTX_IRQ_PENDING);
		out.mask = *reinterpret_cast<const uint32_t*>(ctx + CTX_IRQ_MASK);
		out.ip = *reinterpret_cast<const uint32_t*>(ctx + CTX_IP);
		out.yield = *(ctx + CTX_FRAME_YIELD);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out = {};
		return false;
	}
}

bool m2ftg::NetplaySupported()
{
	const DwGame* game = CurrentDw();
	if (game == nullptr || game->romFrameCounter == 0)
	{
		return false;
	}
	// The diagnostic override deliberately does NOT relax the frame-counter test above: without
	// a measured counter there is no anchor and no canary, so a round could not be set up at
	// all. What it relaxes is the per-game "this one is known to diverge" verdict, which is the
	// only thing standing between VF2 and an observable round.
	if (game->netplayReady)
	{
		return true;
	}
	const YAMPSettings* settings = gGeneral.GetSettings();
	return settings != nullptr && settings->m_netForceUnsupported;
}

uint32_t m2ftg::RomFrameCounterAddress()
{
	const DwGame* game = CurrentDw();
	return game != nullptr ? game->romFrameCounter : 0;
}
