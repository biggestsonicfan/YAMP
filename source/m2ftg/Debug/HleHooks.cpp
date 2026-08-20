#include "HleHooks.h"

#include "../ModuleBuild.h"
#include "../../YAMPGeneral.h"
#include "../../YAMPSettings.h"
#include "../../DebugLog.h"
#include "../ELF/ElfRom.h"
#include "../../net/NetPlugin.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
	using m2ftg::HleHooks::Info;
	using Kind = m2ftg::HleHooks::Kind;

	// The installer's input table is, in both games, N records of {uint32 romOffset, pad,
	// uint64 handler}. It lives in .data, not .rdata, so it is writable without
	// VirtualProtect - but this code never needs to touch it for enabling/disabling, because
	// it works on the ROM image instead.
	//
	// The installer (StF FUN_18004B070 / FV FUN_180049C50, board bring-up stage 2) does, for
	// every record whose romOffset is below 0x200000:
	//     savedWords[i] = *(uint64*)(romBase + romOffset);          // original instruction(s)
	//     *(uint32*)(romBase + romOffset) = 0x4000000 | (i * 4);    // trap word
	// The trap word's low bits are the record index, so the emulator's opcode dispatch can
	// find the handler and the saved original again. Restoring the ROM word from savedWords
	// therefore un-does a hook completely and reversibly, at any time.
	constexpr uint32_t ROM_SIZE = 0x100000;
	constexpr uint32_t TRAP_OPCODE = 0x4000000;

	struct HleTableEntry
	{
		uint32_t romOffset;
		uint32_t padding;
		uint64_t handler;
	};
	static_assert(sizeof(HleTableEntry) == 0x10, "HLE record is 16 bytes in the DLL");

	// Every pxd module ships with ASLR, so this is a real lookup every time rather than a
	// constant - see SetGameDllRange in HostCdevice for the same lesson learned the hard way.
	//
	// The name comes from the game's own descriptor rather than from an LJ GameDesc lookup:
	// this file serves three different hosts now (LJ, YLAD, and K2 next), and only the LJ one
	// has a GameDesc table to ask.
	uint8_t* ModuleBase(const wchar_t* dllName)
	{
		return reinterpret_cast<uint8_t*>(GetModuleHandleW(dllName));
	}

	// The two shared tails every handler ends in, and the whole reason each table splits so
	// cleanly into "the original still runs" and "the original is gone":
	//   StF 0x39D0 / FV 0x39E0   executes the saved original instruction (recomputes the
	//                            opcode-table index exactly like the CPU's fetch path and
	//                            tail-jumps its handler)
	//   StF 0x3A70 / FV 0x3A80   returns only the original's length (4 or 8) - skipped
	// A handler that ends by returning a length inline is doing the same thing as the skip
	// tail; a handler that ends in a call or jump to the exec tail runs ADDITIVELY. That is
	// exactly what Info::replacesInstruction records, and it is why a bare jump to the exec
	// tail (StF 0x52D70, FV 0x51880) classifies as Inert.

#include "Hooks/StfHooks.inc"

#include "Hooks/FvHooks.inc"

#include "Hooks/Vf2Hooks.inc"

	// ---- Per-game descriptor -------------------------------------------------------------
	//
	// Everything above this line is data; everything below is game-agnostic and reads the
	// running game's descriptor. A game with no entry here simply has no HLE hook feature -
	// Update() and ApplyRetarget() become no-ops rather than misfiring on another DLL's
	// addresses, which is what the old `GetGameId() != StF` early-outs bought.
	// The four .data globals the installer works through. They are per module BUILD: the hook SET
	// is a property of the game (all 76 of Sonic the Fighters' records have identical romOffsets
	// in the Lost Judgment and Like a Dragon Gaiden DLLs, in the same order), but where the table
	// and its ROM live is a property of the compile. See ../ModuleBuild.h.
	struct HookBuild
	{
		uint32_t timestamp;
		uintptr_t rvaTable;       // installer input, .data
		uintptr_t rvaSavedWords;  // uint64[] - originals saved by the installer
		uintptr_t rvaRomBase;     // 1 MB i960 program ROM (slot 0)
		uintptr_t rvaBootState;   // ROM/CPU boot phase; 2 = board booted
	};

	struct GameHooks
	{
		// HOW A HOOK IS TURNED OFF. Two mechanisms, because the modules give us two.
		//
		// RomWord is the original: rewrite the i960 instruction in the ROM image, choosing between
		// the installer's saved original and the trap word. It needs the installer's saved-word
		// array and the ROM base, both reversed per module.
		//
		// HandlerTail is Virtual On's, and needs neither. Its table records are
		// `{rom_offset, handler}` in writable .data, and 63 of its 121 hooks already point at the
		// shared "execute original" tail - so pointing a handler there is not a trick, it is what
		// the module's own inert hooks ARE. The trap still fires and the tail runs the instruction
		// the ROM wrote.
		enum class Disable { RomWord, HandlerTail };

		const wchar_t* dllName;   // resolved with GetModuleHandleW; every module is ASLR'd
		Disable disable;
		uintptr_t rvaInertTail;   // HandlerTail only: the shared "execute original" handler
		const Info* hooks;
		size_t count;
		const HookBuild* builds;
		size_t buildCount;
		uint64_t defaultDisable[2];
		const m2ftg::HleHooks::ConventionSite* convention;
		size_t conventionCount;
	};

	using m2ftg::HleHooks::ConventionSite;

	// Homebrew-ROM convention sites. The wire contract behind each symbol is documented in
	// HleHooks.h; only the hook INDEX differs between the games, and only for `rand`.
	constexpr ConventionSite STF_CONVENTION[] = {
		{ "__yamp_hook_composite_enable", 1, 0 },
		{ "__yamp_hook_frame_yield",      2, 0 },
		{ "__yamp_hook_geo_wait",         3, 0 },
		{ "__yamp_hook_geo_wait",         4, 8 },
		{ "__yamp_hook_vblank",           5, 0 },
		{ "__yamp_hook_vblank",           6, 8 },
		{ "__yamp_hook_vblank",           7, 16 },
		{ "__yamp_hook_rand",            33, 0 },
	};
	// FV agrees on 1-7 (its 3/4 are interrupt_wait rather than geo_func, but they are the same
	// two-instruction handshake the geo_wait contract describes) and puts `rand` at 43.
	constexpr ConventionSite FV_CONVENTION[] = {
		{ "__yamp_hook_composite_enable", 1, 0 },
		{ "__yamp_hook_frame_yield",      2, 0 },
		{ "__yamp_hook_geo_wait",         3, 0 },
		{ "__yamp_hook_geo_wait",         4, 8 },
		{ "__yamp_hook_vblank",           5, 0 },
		{ "__yamp_hook_vblank",           6, 8 },
		{ "__yamp_hook_vblank",           7, 16 },
		{ "__yamp_hook_rand",            43, 0 },
	};

	// VF2 agrees with the other two on 1-7 and puts `rand` at 52.
	constexpr ConventionSite VF2_CONVENTION[] = {
		{ "__yamp_hook_composite_enable", 1, 0 },
		{ "__yamp_hook_frame_yield",      2, 0 },
		{ "__yamp_hook_geo_wait",         3, 0 },
		{ "__yamp_hook_geo_wait",         4, 8 },
		{ "__yamp_hook_vblank",           5, 0 },
		{ "__yamp_hook_vblank",           6, 8 },
		{ "__yamp_hook_vblank",           7, 16 },
		{ "__yamp_hook_rand",            52, 0 },
	};

	// Sonic the Fighters in both titles that ship it. The Gaiden row was read straight out of
	// that build's installer: the trap loop is
	//     uVar10 = *(uint *)(&DAT_1801f31c0 + i * 0x10);                       <- table
	//     (&DAT_180699f00)[n] = *(undefined8 *)(&DAT_180a03690 + uVar10);      <- savedWords, romBase
	//     *(uint *)(&DAT_180a03690 + uVar10) = n * 4 | 0x4000000;
	// and DAT_1806c4cc0 is the boot-phase counter it switches on. Every one of the four is
	// +0xB9C0 from its Lost Judgment position, which is the uniform .data shift between the two
	// builds and an independent check on all four numbers.
	constexpr HookBuild STF_BUILDS[] = {
		{ m2ftg::build::LJ_STF,     0x1E8870, 0x68E540, 0x9F7CD0, 0x6B9300 },
		{ m2ftg::build::GAIDEN_STF, 0x1F31C0, 0x699F00, 0xA03690, 0x6C4CC0 },
	};
	constexpr HookBuild FV_BUILDS[] = {
		{ m2ftg::build::LJ_FV, 0x1E5840, 0x690B40, 0x9FA2D0, 0x6BB900 },
	};
	constexpr HookBuild VF2_BUILDS[] = {
		{ m2ftg::build::YLAD_VF2, 0x185640, 0x51D180, 0x980040, 0x641890 },
	};

	constexpr GameHooks GAME_STF = {
		L"stf-pxd-w64-d3d12_retail.dll",
		GameHooks::Disable::RomWord, 0,
		STF_HOOKS, std::size(STF_HOOKS),
		STF_BUILDS, std::size(STF_BUILDS),
		{ (1ull << m2ftg::HleHooks::HOOK_STF_GAME_INT_TIME)
		| (1ull << m2ftg::HleHooks::HOOK_STF_ATTRACT_TIMER), 0 },
		STF_CONVENTION, std::size(STF_CONVENTION),
	};
	constexpr GameHooks GAME_FV = {
		L"fv-pxd-w64-d3d12_retail.dll",
		GameHooks::Disable::RomWord, 0,
		FV_HOOKS, std::size(FV_HOOKS),
		FV_BUILDS, std::size(FV_BUILDS),
		{ 0, 0 },   // FV's GAME_INT hook is Inert - nothing to disable by default
		FV_CONVENTION, std::size(FV_CONVENTION),
	};

	constexpr GameHooks GAME_VF2 = {
		L"vf2-pxd-w64-retail.dll",
		GameHooks::Disable::RomWord, 0,
		VF2_HOOKS, std::size(VF2_HOOKS),
		VF2_BUILDS, std::size(VF2_BUILDS),
		{ 0, 0 },   // nothing to disable by default
		VF2_CONVENTION, std::size(VF2_CONVENTION),
	};

#include "Hooks/VonHooks.inc"

	// Virtual On needs only the table RVA and the boot state: HandlerTail disabling writes the
	// table itself, so there is no saved-word array or ROM base to reverse. Boot state matches
	// DwGame::rvaBootState for the same module.
	//
	// The table starts at 0x476520, NOT the 0x476510 this used to say. The installer's own loop
	// settles it - `FUN_1800048e0` walks `&DAT_180476520 + i * 0x10` for i in 0..0x77, i.e. 120
	// records beginning there, and the 16 bytes below that base are {0, &free_thunk}: a neighbour,
	// not a hook. Reading the table one record low made every hook one index high AND invented a
	// 121st whose "handler" was the CRT's free() - and because the disable path writes the handler
	// column, ticking that phantom in the settings list stored 0x180070FB0 over a live data
	// pointer with many readers. Both are fixed by the base; the hook table below is renumbered to
	// match, so INDICES IN NOTES WRITTEN BEFORE 2026-08-05 ARE ONE HIGHER THAN THEY ARE NOW.
	constexpr HookBuild VON_BUILDS[] = {
		{ 0, 0x476520, 0, 0, 0x7ADCA8 },
	};

	constexpr GameHooks GAME_VON = {
		L"omg-pxd-w64-gog_retail.dll",
		GameHooks::Disable::HandlerTail, 0x070FB0,
		VON_HOOKS, std::size(VON_HOOKS),
		VON_BUILDS, std::size(VON_BUILDS),
		// Hook 5 off by default: it pre-marks the ROM's warning screen as already seen, which is
		// what makes Virtual On appear to boot straight to the SEGA screen. See the block on
		// HOOK_VON_WARNING_SKIP in HleHooks.h - this is the arcade boot, and it is also the
		// sequence a linked pair needs.
		{ 1ull << m2ftg::HleHooks::HOOK_VON_WARNING_SKIP, 0 },
		nullptr, 0, // no homebrew convention sites
	};

#include "Hooks/MrHooks.inc"

	// Motor Raid, enumerated 2026-08-09 alongside the linked-cabinet work (docs/
	// mr-comm-packet.md). RVAs from the module's own installer, FUN_180052680: the trap loop is
	//     uVar10 = *(uint *)(&DAT_180276d80 + i * 0x10);                      <- table (0x40)
	//     (&DAT_180727490)[i] = *(u64 *)(&DAT_180eb6310 + uVar10);            <- savedWords, romBase
	//     *(uint *)(&DAT_180eb6310 + uVar10) = i * 4 | 0x4000000;
	// guarded by `if (uVar10 < 0x200000)` — the overlay-targeting records never install (see
	// MrHooks.inc). The boot state is the installer's own stage counter DAT_180741e30, which
	// reaches 2 in the stage that stamps the traps and stays there — the same contract
	// rvaBootState documents.
	constexpr HookBuild MR_BUILDS[] = {
		{ m2ftg::build::LJ_MR, 0x276D80, 0x727490, 0xEB6310, 0x741E30 },
	};

	constexpr GameHooks GAME_MR = {
		L"mr-pxd-w64-d3d12_retail.dll",
		GameHooks::Disable::RomWord, 0,
		MR_HOOKS, std::size(MR_HOOKS),
		MR_BUILDS, std::size(MR_BUILDS),
		{ 0, 0 },   // nothing to disable by default
		nullptr, 0, // no homebrew convention sites
	};

	static_assert(std::size(VON_HOOKS) <= m2ftg::HleHooks::MAX_COUNT, "raise MAX_COUNT");
	static_assert(std::size(STF_HOOKS) <= m2ftg::HleHooks::MAX_COUNT, "raise MAX_COUNT");
	static_assert(std::size(FV_HOOKS) <= m2ftg::HleHooks::MAX_COUNT, "raise MAX_COUNT");
	static_assert(std::size(VF2_HOOKS) <= m2ftg::HleHooks::MAX_COUNT, "raise MAX_COUNT");
	static_assert(std::size(MR_HOOKS) <= m2ftg::HleHooks::MAX_COUNT, "raise MAX_COUNT");

	const GameHooks* CurrentHooks()
	{
		switch (gGeneral.GetGameId())
		{
		// One hook SET for both Sonic the Fighters builds - all 76 records have identical
		// romOffsets in the same order - with CurrentBuildRvas below picking where the table
		// lives in whichever DLL is loaded.
		case YAMPGeneral::GameId::StF:
		case YAMPGeneral::GameId::StF_GAIDEN: return &GAME_STF;
		case YAMPGeneral::GameId::FV:  return &GAME_FV;
		case YAMPGeneral::GameId::VF2: return &GAME_VF2;
		case YAMPGeneral::GameId::VON_K2: return &GAME_VON;
		case YAMPGeneral::GameId::MR:  return &GAME_MR;
		default:                       return nullptr;
		}
	}

	// The .data globals for the module build actually loaded. Falls back to the game's first row
	// for an unrecognised build - GameVerify blocks those before LoadLibrary, so reaching the
	// fallback means a table here is missing a row rather than a user having an odd file.
	const HookBuild& CurrentBuildRvas(const GameHooks& game)
	{
		const uint32_t running = m2ftg::CurrentModuleBuild();
		for (size_t i = 0; i < game.buildCount; i++)
		{
			if (game.builds[i].timestamp == running) return game.builds[i];
		}
		return game.builds[0];
	}

	// Used wherever a caller asks for a mask and the game has no table.
	constexpr uint64_t NO_MASK[2] = { 0, 0 };
}

size_t m2ftg::HleHooks::Count()
{
	const GameHooks* game = CurrentHooks();
	return game != nullptr ? game->count : 0;
}

bool m2ftg::HleHooks::Supported()
{
	return CurrentHooks() != nullptr;
}

const m2ftg::HleHooks::Info& m2ftg::HleHooks::Get(size_t index)
{
	// A caller with no game running still gets a readable record rather than a null deref;
	// the settings UI asks for Count() first, so it never lands here.
	static constexpr Info NONE = { 0, 0, "-", Kind::Inert, false, "" };
	const GameHooks* game = CurrentHooks();
	if (game == nullptr)
	{
		return NONE;
	}
	return game->hooks[index < game->count ? index : 0];
}

const uint64_t* m2ftg::HleHooks::DefaultDisableMask()
{
	const GameHooks* game = CurrentHooks();
	return game != nullptr ? game->defaultDisable : NO_MASK;
}

const m2ftg::HleHooks::ConventionSite* m2ftg::HleHooks::Convention(size_t& count)
{
	const GameHooks* game = CurrentHooks();
	count = game != nullptr ? game->conventionCount : 0;
	return game != nullptr ? game->convention : nullptr;
}

const char* m2ftg::HleHooks::KindName(Kind kind)
{
	switch (kind)
	{
	case Kind::Core:    return "Core";
	case Kind::Host:    return "Host";
	case Kind::Content: return "Content";
	case Kind::Removed: return "Removed";
	case Kind::Inert:   return "Inert";
	case Kind::Unclassified: return "Unclassified";
	}
	return "?";
}

const char* m2ftg::HleHooks::KindDescription(Kind kind)
{
	switch (kind)
	{
	case Kind::Core:    return "Emulator plumbing (frame yield, vsync, render sync, self-test bypass). Disabling these hangs or black-screens the game.";
	case Kind::Host:    return "Host integration: audio, input, arcade settings, progress reporting. Disabling these usually boots but loses the feature.";
	case Kind::Content: return "Changes what the game does: hidden characters, Honey's art and animation, VS-mode rules, attract timings. Safe to disable.";
	case Kind::Removed: return "The ROM instruction is deleted outright and nothing runs in its place. Safe to disable.";
	case Kind::Inert:   return "The trap runs the original instruction unchanged - a debug probe whose body was compiled out of the retail build. Disabling changes nothing but removes the trap overhead.";
	case Kind::Unclassified: return "Enumerated from the module's table and named from its ROM symbols, but the handler has not been read - it may be anything, including emulator plumbing. Toggle freely to find out; the setting is deliberately NOT saved, so a restart always recovers.";
	}
	return "";
}

bool m2ftg::HleHooks::MaskTest(const uint64_t mask[2], size_t index)
{
	return index < MAX_COUNT && (mask[index >> 6] & (1ull << (index & 63))) != 0;
}

void m2ftg::HleHooks::MaskSet(uint64_t mask[2], size_t index, bool disabled)
{
	if (index >= MAX_COUNT)
	{
		return;
	}
	const uint64_t bit = 1ull << (index & 63);
	if (disabled)
	{
		mask[index >> 6] |= bit;
	}
	else
	{
		mask[index >> 6] &= ~bit;
	}
}

void m2ftg::HleHooks::MaskForKinds(uint64_t mask[2], unsigned kinds)
{
	mask[0] = 0;
	mask[1] = 0;
	const size_t count = Count();
	for (size_t i = 0; i < count; i++)
	{
		if ((kinds & KindBit(Get(i).kind)) != 0)
		{
			MaskSet(mask, i, true);
		}
	}
}

bool m2ftg::HleHooks::MaskStripKinds(uint64_t mask[2], unsigned kinds)
{
	bool changed = false;
	const size_t count = Count();
	for (size_t i = 0; i < count; i++)
	{
		if ((kinds & KindBit(Get(i).kind)) != 0 && MaskTest(mask, i))
		{
			MaskSet(mask, i, false);
			changed = true;
		}
	}
	return changed;
}

void m2ftg::HleHooks::Update()
{
	const GameHooks* game = CurrentHooks();
	if (game == nullptr)
	{
		return;
	}

	uint8_t* base = ModuleBase(game->dllName);
	if (base == nullptr)
	{
		return;
	}

	const HookBuild& rvas = CurrentBuildRvas(*game);

	const YAMPSettings* settings = gGeneral.GetSettings();
	if (settings == nullptr)
	{
		return;
	}
	// DURING NETPLAY THE MASK IS IGNORED and every hook is restored, whatever the ini says.
	//
	// This mask rewrites the emulated ROM IMAGE. Two machines with different masks are running
	// different programs, so lockstep faithfully feeds identical inputs into two different games -
	// a guaranteed desync that no amount of input synchronisation can reach, and one that would be
	// invisible until someone wondered why their opponent's fighter did something impossible.
	// Rather than exchange masks and negotiate, both peers simply run the UNMODIFIED ROM, which is
	// a state both are guaranteed to agree on without any protocol at all.
	//
	// It costs nothing to reverse: the reconciler runs every frame, so leaving a room restores the
	// player's own mask on the next one.
	//
	// The game's default mask is the one exception, and it does not weaken the argument: it is
	// a compiled-in constant, not a setting, so every peer on the same build already agrees on
	// it without exchanging anything - exactly the property the paragraph above is buying.
	// Restoring StF's hook 16 here would instead make both peers agree on a two-second round.
	// (FV's default mask is empty, so for FV this is simply "restore everything".)
	const uint64_t* netplayMask = game->defaultDisable;
	const bool netplayLocked = net::SessionInProgress();
	const uint64_t* wanted = netplayLocked ? netplayMask : settings->m_stfHleDisableMask;

	// HANDLER-TAIL GAMES (Virtual On) rewrite the table's own handler pointers instead of the
	// emulated ROM. Same contract - enforced every frame, so it survives the installer re-running
	// at a board reset and needs no record of what was applied when - and the same netplay rule,
	// because a repointed handler changes the simulation exactly as a rewritten ROM word does.
	//
	// DELIBERATELY AHEAD OF THE BOOT-STATE GATE BELOW, which is what makes a disabled hook take
	// effect during BOOT rather than one frame into it. The handler column is static .data that
	// nothing in the module ever writes - the installer (`FUN_1800048e0`) reads only romOffset,
	// stamps the trap word into the ROM image and saves the original instruction; it never touches
	// a handler pointer. So this table is already meaningful the moment the DLL is mapped, long
	// before bring-up reaches the state that sets the boot flag, and waiting for that flag only
	// guaranteed that Virtual On's boot-path hooks (1-4, 18) had ALREADY FIRED by the time the
	// mask was applied. Nothing here reads emulated RAM, so running it early is safe.
	if (game->disable == GameHooks::Disable::HandlerTail)
	{
		// "-von-hleoff=3,7,21": disable these hook indices FROM BOOT. Kept now that the settings
		// mask reaches the boot path too, because Core hooks are still session-only by design
		// (SESSION_ONLY_KINDS) - and Virtual On's Core hooks are precisely the boot-path ones, so
		// this flag stays the only way to experiment with them. It survives nothing and defaults
		// to nothing, which is what preserves the restart-always-recovers guarantee.
		static uint64_t s_cmdOff[2] = {};
		static bool s_cmdParsed = false;
		if (!s_cmdParsed)
		{
			s_cmdParsed = true;
			if (const wchar_t* arg = wcsstr(GetCommandLineW(), L"-von-hleoff="))
			{
				const wchar_t* p = arg + wcslen(L"-von-hleoff=");
				char listed[256] = "";
				size_t n = 0;
				while (*p >= L'0' && *p <= L'9')
				{
					const unsigned long idx = wcstoul(p, const_cast<wchar_t**>(&p), 10);
					if (idx < game->count)
					{
						s_cmdOff[idx / 64] |= 1ull << (idx % 64);
						n += snprintf(listed + n, sizeof(listed) - n, "%s%lu",
							n != 0 ? "," : "", idx);
					}
					if (*p == L',') ++p;
				}
				net::Logf("-von-hleoff: hooks [%s] disabled from boot", listed);
			}
		}

		struct Entry { uint64_t romOffset; uint64_t handler; };
		auto* entries = reinterpret_cast<Entry*>(base + rvas.rvaTable);
		const uint64_t inert = reinterpret_cast<uint64_t>(base + game->rvaInertTail);
		static uint64_t s_original[MAX_COUNT] = {};
		for (size_t i = 0; i < game->count; i++)
		{
			// Latch what the installer put there the first time it is seen as something other
			// than the tail, so "off" is reversible without a saved-word array.
			if (s_original[i] == 0 && entries[i].handler != inert)
			{
				s_original[i] = entries[i].handler;
			}
			const bool off = MaskTest(wanted, i) || MaskTest(s_cmdOff, i);
			const uint64_t desired = (s_original[i] != 0 && off)
				? inert : (s_original[i] != 0 ? s_original[i] : entries[i].handler);
			if (entries[i].handler != desired)
			{
				entries[i].handler = desired;
			}
		}
		return;
	}

	// ROM-WORD GAMES restore the emulated instruction out of the module's own save area, which the
	// installer fills as it writes the traps - so unlike the handler table above, there is nothing
	// to read until the installer has run. It does so in board bring-up stage 2, which is also when
	// the boot state reaches 2.
	if (*reinterpret_cast<const uint32_t*>(base + rvas.rvaBootState) != 2)
	{
		return;
	}

	const auto* table = reinterpret_cast<const HleTableEntry*>(base + rvas.rvaTable);
	const auto* savedWords = reinterpret_cast<const uint64_t*>(base + rvas.rvaSavedWords);
	auto* rom = reinterpret_cast<uint8_t*>(base + rvas.rvaRomBase);

	// Enforced every frame rather than applied once. It is only a few dozen aligned dword
	// compares, and it means the setting can be toggled live, survives a board reset
	// re-installing the traps, and needs no bookkeeping about what was applied when.
	const size_t count = game->count;
	for (size_t i = 0; i < count; i++)
	{
		const uint32_t romOffset = table[i].romOffset;
		// Same guard the installer uses; anything outside the ROM image was never hooked.
		if (romOffset >= 0x200000 || romOffset + sizeof(uint32_t) > ROM_SIZE)
		{
			continue;
		}

		const uint32_t trapWord = TRAP_OPCODE | static_cast<uint32_t>(i * 4);
		const uint32_t originalWord = static_cast<uint32_t>(savedWords[i]);
		// The installer saves the original before writing the trap, so a saved word that is
		// itself a trap means the save area has not been filled yet (or something else has
		// already rewritten the ROM). Leave that entry alone rather than bake a trap in as
		// if it were the ROM's own instruction.
		if (originalWord >= TRAP_OPCODE && originalWord < TRAP_OPCODE + count * 4)
		{
			continue;
		}

		const uint32_t desired = MaskTest(wanted, i) ? originalWord : trapWord;
		auto* word = reinterpret_cast<volatile uint32_t*>(rom + romOffset);
		if (*word != desired)
		{
			*word = desired;
		}
	}
}

uint32_t m2ftg::HleHooks::detail::g_hitCounts[MAX_COUNT] = {};

// Set by ApplyRetarget when game.elf declared any convention symbol; the settings UI uses it to
// decide whether the homebrew health checks apply (they would false-alarm on stock StF).
static bool g_usedConvention = false;

uint32_t m2ftg::HleHooks::HitCount(size_t index)
{
	return index < MAX_COUNT ? detail::g_hitCounts[index] : 0;
}

void m2ftg::HleHooks::ResetHitCounts()
{
	for (size_t i = 0; i < MAX_COUNT; i++)
	{
		detail::g_hitCounts[i] = 0;
	}
}

uint32_t m2ftg::HleHooks::ResolveRetarget(const std::string& text, size_t hookIndex)
{
	if (text.empty())
	{
		return 0;
	}

	if (_stricmp(text.c_str(), "off") == 0 || _stricmp(text.c_str(), "none") == 0 || text == "-")
	{
		return RETARGET_SUPPRESS;
	}

	// A leading digit means a literal offset; anything else is a symbol name. This is why an
	// ELF symbol may not start with a digit - which the C identifier rules already guarantee.
	if (isdigit(static_cast<unsigned char>(text[0])) != 0)
	{
		const char* digits = text.c_str();
		if (text.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
		{
			digits += 2;
		}
		char* end = nullptr;
		const unsigned long value = strtoul(digits, &end, 16);
		if (end != nullptr && *end == '\0')
		{
			return static_cast<uint32_t>(value);
		}
		DebugLogFile("[HleHooks] hook %zu: '%s' is not a valid ROM offset - left alone\n", hookIndex, text.c_str());
		return 0;
	}

	uint32_t address = 0;
	if (ElfRom::ResolveSymbol(text, address))
	{
		DebugLogFile("[HleHooks] hook %zu: '%s' -> ROM 0x%06X\n", hookIndex, text.c_str(), address);
		return address;
	}

	if (!ElfRom::IsLoaded())
	{
		DebugLogFile("[HleHooks] hook %zu: '%s' names a symbol but no %ls is loaded - left alone\n",
			hookIndex, text.c_str(), ElfRom::OVERRIDE_FILE_NAME);
	}
	else
	{
		DebugLogFile("[HleHooks] hook %zu: symbol '%s' is not in %ls - left alone\n",
			hookIndex, text.c_str(), ElfRom::LoadedPath());
	}
	return 0;
}

bool m2ftg::HleHooks::UsedConvention()
{
	return g_usedConvention;
}

size_t m2ftg::HleHooks::ApplyRetarget(const std::string iniRetarget[MAX_COUNT])
{
	const GameHooks* game = CurrentHooks();
	if (game == nullptr)
	{
		return 0;
	}
	const size_t count = game->count;

	uint8_t* base = ModuleBase(game->dllName);
	if (base == nullptr)
	{
		DebugLogFile("[HleHooks] retarget skipped: module not loaded\n");
		return 0;
	}

	// Fold the ROM's own declarations in underneath the ini, so an explicit line still wins.
	std::string retarget[MAX_COUNT];
	for (size_t i = 0; i < count; i++)
	{
		retarget[i] = iniRetarget[i];
	}

	g_usedConvention = false;
	size_t declared = 0;
	for (size_t c = 0; c < game->conventionCount; c++)
	{
		const ConventionSite& site = game->convention[c];
		uint32_t address = 0;
		if (!ElfRom::ResolveSymbol(site.symbol, address))
		{
			continue;
		}
		g_usedConvention = true;
		if (!retarget[site.hook].empty())
		{
			DebugLogFile("[HleHooks] hook %u: '%s' declared by the ROM, but [HleRetarget] overrides it\n",
				site.hook, site.symbol);
			continue;
		}
		// Formatted as a literal so ResolveRetarget's normal path validates alignment and range.
		// MUST carry the 0x prefix: ResolveRetarget decides "hex or symbol name" on the first
		// character, so a bare "DEF0" reads as a symbol, fails to resolve and silently leaves the
		// hook unplaced - which for hook 1 is a black screen.
		char literal[16];
		_snprintf_s(literal, sizeof(literal), _TRUNCATE, "0x%X", address + site.byteOffset);
		retarget[site.hook] = literal;
		declared++;
		DebugLogFile("[HleHooks] hook %u <- %s+0x%X = ROM 0x%06X (declared by the ROM)\n",
			site.hook, site.symbol, site.byteOffset, address + site.byteOffset);
	}

	if (g_usedConvention)
	{
		// Opting in means the ROM owns the hook map: anything it did not name must NOT stay
		// pointed at the host game's own offsets, which in a different program ROM are
		// arbitrary instructions.
		size_t suppressed = 0;
		for (size_t i = 0; i < count; i++)
		{
			if (retarget[i].empty()) { retarget[i] = "off"; suppressed++; }
		}
		DebugLogFile("[HleHooks] ROM declared %zu hook site(s); %zu unnamed hook(s) suppressed\n",
			declared, suppressed);
	}

	auto* table = reinterpret_cast<HleTableEntry*>(base + CurrentBuildRvas(*game).rvaTable);
	size_t changed = 0;
	for (size_t i = 0; i < count; i++)
	{
		const uint32_t want = ResolveRetarget(retarget[i], i);
		if (want == 0)
		{
			continue;
		}

		if (want != RETARGET_SUPPRESS)
		{
			// The installer reads a full 8 bytes at the site (an i960 MEMB instruction with a
			// displacement is two words) and the trap it writes is a single aligned word, so a
			// misaligned or out-of-range offset would corrupt the ROM rather than hook it.
			if ((want & 3) != 0 || want + sizeof(uint64_t) > ROM_SIZE)
			{
				DebugLogFile("[HleHooks] hook %zu: retarget offset 0x%X is misaligned or outside the "
					"1 MB program ROM - ignored\n", i, want);
				continue;
			}
		}

		if (table[i].romOffset == want)
		{
			continue;
		}

		DebugLogFile("[HleHooks] hook %zu (%s): ROM 0x%06X -> %s\n", i, game->hooks[i].site, table[i].romOffset,
			want == RETARGET_SUPPRESS ? "not installed" : "retargeted");
		table[i].romOffset = want;
		changed++;
	}

	if (changed != 0)
	{
		DebugLogFile("[HleHooks] retargeted %zu of %zu hook records before module_start\n", changed, count);
	}
	return changed;
}

bool m2ftg::HleHooks::GetInstalledOffsets(uint32_t out[MAX_COUNT])
{
	const GameHooks* game = CurrentHooks();
	if (game == nullptr)
	{
		return false;
	}

	const uint8_t* base = ModuleBase(game->dllName);
	if (base == nullptr)
	{
		return false;
	}

	const auto* table = reinterpret_cast<const HleTableEntry*>(base + CurrentBuildRvas(*game).rvaTable);
	for (size_t i = 0; i < game->count; i++)
	{
		out[i] = table[i].romOffset;
	}
	return true;
}
