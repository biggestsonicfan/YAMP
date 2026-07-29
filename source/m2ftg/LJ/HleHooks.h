#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace m2ftg
{
	// Sonic the Fighters' m2ftg module does not run the arcade ROM unmodified. During board
	// bring-up it overwrites 76 individual i960 instructions in the program ROM image with a
	// trap word, each of which dispatches to a native x64 handler inside the DLL - "HLE hooks".
	// Some of them are what makes the emulator work at all (the frame yield, the vsync wait,
	// the self-test bypass); the rest re-implement or delete game logic, which is what makes
	// the ROM's own behaviour unmoddable: patching rom_code1.bin at a hooked address has no
	// effect, because the instruction there is never executed.
	//
	// This module enumerates all 76 and can disable any subset of them at runtime, restoring
	// the ROM's original instruction so the (possibly modified) ROM code runs instead.
	namespace HleHooks
	{
		enum class Kind : uint8_t
		{
			// Emulator plumbing - frame yield, vsync wait, render sync, self-test bypass,
			// texture-upload timeout. Disabling these hangs or black-screens the game.
			Core,
			// Host integration - audio bridging, input, backup RAM / arcade settings from the
			// Lost Judgment config, progress reporting back to the host. Disabling these
			// generally boots but loses the feature (silent sound, default DIP settings, ...).
			Host,
			// Changes what the game does: hidden character select, Honey's portraits and
			// head tilt, VS-mode rules, attract-mode timings. Safe to disable; disabling
			// gives you the plain arcade behaviour, which is what a ROM mod usually wants.
			Content,
			// Deletes the ROM instruction outright - the handler is the bare "skip original"
			// tail, so nothing runs in its place.
			Removed,
			// The handler is a bare jump to the "execute original" tail: the instruction still
			// runs exactly as the ROM wrote it. These are debug probe points whose bodies were
			// compiled out of the retail build. Disabling one changes nothing observable.
			Inert,
		};

		struct Info
		{
			uint32_t romOffset;        // program-ROM offset of the patched i960 instruction
			uint32_t handlerRva;       // native handler, RVA in the module
			const char* site;          // ROM symbol + offset, from the DLL's own symbol table
			Kind kind;
			bool replacesInstruction;  // true: original never runs; false: handler runs, then it does
			const char* note;
		};

		inline constexpr size_t COUNT = 76;

		const Info& Get(size_t index);
		const char* KindName(Kind kind);
		const char* KindDescription(Kind kind);

		// Bit i of the mask disables hook i. Bit indices match the DLL's own table order,
		// which is also the order the "HLE ROM hooks" settings list displays them in.
		bool MaskTest(const uint64_t mask[2], size_t index);
		void MaskSet(uint64_t mask[2], size_t index, bool disabled);
		// Every hook whose kind is in `kinds` (bitmask of 1 << (int)Kind).
		void MaskForKinds(uint64_t mask[2], unsigned kinds);
		// Clears the bits of every hook whose kind is in `kinds`. Returns true if it changed
		// anything. Used to keep Core hooks out of the saved mask - see Update().
		bool MaskStripKinds(uint64_t mask[2], unsigned kinds);
		inline constexpr unsigned KindBit(Kind kind) { return 1u << static_cast<unsigned>(kind); }
		// The preset a ROM modder normally wants: every hook that stops the ROM's own code at
		// that address from running, and nothing the emulator needs to keep running.
		inline constexpr unsigned MODDING_KINDS = KindBit(Kind::Content) | KindBit(Kind::Removed);
		// Every hook, including the ones the emulator depends on. Expect a hang.
		inline constexpr unsigned ALL_KINDS = KindBit(Kind::Core) | KindBit(Kind::Host) | MODDING_KINDS | KindBit(Kind::Inert);

		// Disabling a Core hook hangs the emulated board, and the hang takes the settings UI
		// with it - so a Core bit that survived into settings.ini would make YAMP unbootable
		// with no way back. Core bits are therefore session-only: they can be ticked and take
		// effect live, but they are stripped when settings are saved and again when they are
		// loaded, so a restart always recovers.
		inline constexpr unsigned SESSION_ONLY_KINDS = KindBit(Kind::Core);

		// Restores or re-applies each hook to match the "Disable DLL HLE ROM hooks" setting.
		// Call once per frame; no-op unless the game is StF and the board has booted. Works
		// live - the original instruction words come from the DLL's own save area, which the
		// installer fills before it writes the traps.
		void Update();

		// ---- Pre-install retargeting (for program ROMs that are not Sonic the Fighters) ----
		//
		// Every offset in the table above is a Sonic the Fighters address. Point the module at
		// a different program - a homebrew Model 2B build, say - and the installer still stamps
		// all 76 traps at those offsets, corrupting 76 unrelated instructions before the i960
		// executes a single one. Update() cannot undo that in time: it runs from the UI draw,
		// which is a full module_main call behind the first frame of emulation.
		//
		// The fix is to change what the installer is told to patch, before it runs. The table
		// at DLL+0x1E8870 is in .data (writable, no VirtualProtect needed) and the installer
		// skips any record whose romOffset is >= 0x200000 - so rewriting romOffset ahead of
		// module_start either drops a hook entirely or moves it to wherever the new program
		// keeps its equivalent code. Nothing is corrupted and there is no race, because the
		// trap for a suppressed hook is never written at all.
		//
		// Retarget entries are per-hook:
		//     0                   leave the DLL's own offset alone (the default)
		//     RETARGET_SUPPRESS   never install this hook; the ROM word stays untouched
		//     anything else       install the trap at this program-ROM offset instead
		// Offset 0 doubles as "no change" because it is the i960 initial boot record, which is
		// never a hook site.
		inline constexpr uint32_t RETARGET_SUPPRESS = 0xFFFFFFFF;

		// Interprets one [HleRetarget] ini value:
		//     ""                    -> 0                  (leave it alone)
		//     "off" / "none" / "-"  -> RETARGET_SUPPRESS
		//     "1A2B" / "0x1A2B"     -> that ROM offset
		//     "geo_wait" / "geo_wait+8" -> the ELF symbol's address, plus a hex offset
		// Symbol forms need game.elf loaded (ElfRom); an unknown name is reported and treated
		// as "leave it alone", because guessing an address would corrupt the ROM.
		uint32_t ResolveRetarget(const std::string& text, size_t hookIndex);

		// ---- Convention: a ROM that declares its own hook sites ----------------------------
		//
		// Hand-written [HleRetarget] offsets are how this started, and they are a poor contract:
		// they rot on every relink, and getting one subtly wrong fails SILENTLY - hook 1 on no
		// site at all is a black screen with every counter healthy, hook 2 on an init-only site
		// is a 98%-spin at 2 fps with no error anywhere. A homebrew ELF can instead NAME its hook
		// sites, and YAMP places them itself.
		//
		// The offsets below are not decoration; they are the handlers' wire contract:
		//   frame_yield  replaces its instruction and returns the real length, so it wants a
		//                sacrificial 4-byte instruction on a site reached EVERY FRAME.
		//   geo_wait     hook 4 returns a hardcoded 8, so it must be followed by exactly two
		//                4-byte instructions it can skip (+0 = the 8-byte MEMB load for hook 3).
		//   vblank       hooks 5/6 write g0 and r3 specifically, 8-byte MEMB loads apiece, with
		//                hook 7 on the 4-byte compare that closes the spin.
		// composite_enable is ADDITIVE (the original still runs), so it needs no sacrificial
		// slot at all - any instruction executed once at the end of init will do.
		//
		// `rand` is a different class again: WHOLE-FUNCTION HLE. Its handler writes the host RNG
		// into g0 (the i960 return register) and then performs the i960 `ret` itself - unwinding
		// the register frame through PFP/FP exactly as the instruction would - before returning 0
		// as its IP delta, because it has already set the IP. So its site must be the FIRST
		// instruction of a REAL, CALLED function; a bare `ret` body is enough, and no sacrificial
		// slot is wanted because the handler never falls through. It must not be inlined away:
		// with no `call` there is no frame for the handler's return to unwind.
		struct ConventionSite
		{
			const char* symbol;
			uint8_t hook;
			uint8_t byteOffset;
		};
		inline constexpr ConventionSite CONVENTION[] = {
			{ "__yamp_hook_composite_enable", 1, 0 },
			{ "__yamp_hook_frame_yield",      2, 0 },
			{ "__yamp_hook_geo_wait",         3, 0 },
			{ "__yamp_hook_geo_wait",         4, 8 },
			{ "__yamp_hook_vblank",           5, 0 },
			{ "__yamp_hook_vblank",           6, 8 },
			{ "__yamp_hook_vblank",           7, 16 },
			{ "__yamp_hook_rand",            33, 0 },
		};
		inline constexpr size_t CONVENTION_COUNT = sizeof(CONVENTION) / sizeof(CONVENTION[0]);

		// Rewrites the installer's table. Must be called with the game DLL loaded and BEFORE
		// module_start, which is what runs the installer. No-op unless the game is StF.
		// Returns the number of records changed.
		//
		// Resolution order per hook: an explicit [HleRetarget] line always wins; otherwise the
		// convention symbol, if game.elf declares one. When the ELF declares ANY convention
		// symbol it is taken as opting in, and every hook it did not name is SUPPRESSED rather
		// than left at Sonic the Fighters' own address - which for a different program ROM would
		// stamp a trap into unrelated code. An ELF with no convention symbols behaves exactly as
		// before, so nothing that works today changes.
		size_t ApplyRetarget(const std::string retarget[COUNT]);

		// True if the last ApplyRetarget placed hooks from ELF symbols rather than the ini.
		bool UsedConvention();

		// ---- Invocation counters ----------------------------------------------------------
		//
		// Whether a retargeted hook actually RUNS is the thing that is hardest to tell from the
		// outside: the flags its handler sets are consumed and cleared within the frame, so
		// sampling them between frames says nothing. Counting the trap words as they are fetched
		// answers it directly - "hook 3 fires once per frame" becomes an observation instead of
		// an inference.
		//
		// The count is taken in the instruction fetch path (RamExecFetch::FetchExec), which is
		// YAMP's own reimplementation of the DLL's fetch/decode dispatcher, so nothing in the
		// module is patched to get it. Only a trap word reaches the counter, and trap words are
		// only ever fetched when the CPU really executes one - a 0x04 byte sitting in a data
		// table is never fetched as an instruction, so it cannot inflate the count.
		//
		// NB Motor Raid inlines fetch+decode into its execution loop and therefore has no
		// dispatcher to reimplement (GameDesc i960 RVAs are zero); counts stay at 0 there.
		namespace detail
		{
			extern uint32_t g_hitCounts[COUNT];
		}

		// Called for every fetched instruction word - keep it trivial.
		inline void NoteFetchedWord(uint32_t word)
		{
			if ((word >> 24) != 0x4)
			{
				return;
			}
			const uint32_t operand = word & 0x00FFFFFF;
			if ((operand & 3) != 0)
			{
				return;
			}
			const uint32_t index = operand >> 2;
			if (index < COUNT)
			{
				detail::g_hitCounts[index]++;
			}
		}

		uint32_t HitCount(size_t index);
		void ResetHitCounts();

		// The offset each hook was actually installed at, straight out of the DLL's live table,
		// so it reflects what really happened rather than what was asked for. RETARGET_SUPPRESS
		// (or any value >= 0x200000) means the installer skipped it. False if the module is not
		// loaded yet.
		bool GetInstalledOffsets(uint32_t out[COUNT]);
	}
}
