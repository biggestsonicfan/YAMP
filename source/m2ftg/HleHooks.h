#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace m2ftg
{
	// The LJ m2ftg modules do not run their arcade ROM unmodified. During board bring-up each
	// one overwrites individual i960 instructions in the program ROM image with a trap word,
	// each of which dispatches to a native x64 handler inside the DLL - "HLE hooks". Some of
	// them are what makes the emulator work at all (the frame yield, the vsync wait, the
	// self-test bypass); the rest re-implement or delete game logic, which is what makes the
	// ROM's own behaviour unmoddable: patching rom_code1.bin at a hooked address has no
	// effect, because the instruction there is never executed.
	//
	// This module enumerates them and can disable any subset at runtime, restoring the ROM's
	// original instruction so the (possibly modified) ROM code runs instead.
	//
	// Two games are described here, and the mechanism is identical in both - same installer
	// contract, same 16-byte record, same two shared handler tails, same reversible restore.
	// Only the addresses and the hook set differ:
	//
	//     Sonic the Fighters   76 hooks   table DLL+0x1E8870
	//     Fighting Vipers      95 hooks   table DLL+0x1E5840
	//     Virtual Fighter 2    (YLAD)     table DLL+0x185640
	//     Virtual On          120 hooks   table DLL+0x476520
	//
	// Virtual On is the exception to "restore the original instruction": its traps are disabled by
	// repointing the TABLE's handler column at the module's own execute-original tail instead
	// (GameHooks::Disable::HandlerTail), because that column is static .data no part of the module
	// ever writes. That is also why its mask reaches the boot path - see Update().
	//
	// Everything game-specific lives in the GameHooks table in the .cpp; the API below is
	// game-agnostic and reads the current game's descriptor through Count()/Get().
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
			// Enumerated but not yet reversed: listed from a module's own table and named from its
			// ROM symbol table, with nobody having read the handlers, so which of them are emulator
			// plumbing is unknown. NO GAME USES THIS TODAY - Virtual On's 120 were classified on
			// 2026-08-05 - but it is what a newly enumerated game's table should start as.
			//
			// Treated as SESSION-ONLY for that reason (see SESSION_ONLY_KINDS): a hook that turns
			// out to be Core could otherwise be ticked, saved, and leave the game unbootable with
			// the settings UI hung behind it. Live toggling is the point; persistence is what is
			// unsafe while the kind is a guess.
			Unclassified,
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

		// Upper bound across every game described here, and the size of every per-hook array
		// (masks, hit counts, retarget strings). The LIVE count is Count(), which depends on
		// the game that is running - iterate with that, size with this.
		//
		// It must stay <= 128, because the disable mask is a uint64_t[2].
		inline constexpr size_t MAX_COUNT = 128;   // Virtual On has 120
		static_assert(MAX_COUNT <= 128, "the disable mask is a uint64_t[2]");

		// Hooks in the running game's table, or 0 if this game has no HLE hook table (which
		// is every game but StF and FV). Supported() is the same test, spelled for clarity.
		size_t Count();
		bool Supported();

		// Valid for index < Count(). Out-of-range returns the first record rather than
		// reading past the table.
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
		inline constexpr unsigned ALL_KINDS = KindBit(Kind::Core) | KindBit(Kind::Host) | MODDING_KINDS
			| KindBit(Kind::Inert) | KindBit(Kind::Unclassified);

		// Disabling a Core hook hangs the emulated board, and the hang takes the settings UI
		// with it - so a Core bit that survived into settings.ini would make YAMP unbootable
		// with no way back. Core bits are therefore session-only: they can be ticked and take
		// effect live, but they are stripped when settings are saved and again when they are
		// loaded, so a restart always recovers.
		inline constexpr unsigned SESSION_ONLY_KINDS = KindBit(Kind::Core) | KindBit(Kind::Unclassified);

		// ---- Hooks YAMP disables by default ------------------------------------------------
		//
		// TWO of Sonic the Fighters' hooks read backup RAM +0x3351 as RAW SECONDS, and both are
		// wrong for the same reason. That byte is `time_var_array_num`, an INDEX 0..9 into
		// time_vars[] at ROM 0x8F3C4 (10,20,30,...,99): the ROM's own init_game_assignments
		// stores a literal 2 there (`mov 2, g14; stob` to backup 0x1D03351 AND work RAM
		// 0x59C351 - read out of the ROM image, so this is the board's own convention, not an
		// inference), the ROM converts with `time = time_vars[idx]`, and the service menu edits
		// and displays it as an index throughout.
		//
		// The module's injector wrote SECONDS (30) into it instead. That is the bug that hangs
		// GAME ASSIGNMENTS - an index of 30 runs off the end of a 10-entry table - and
		// FixBackupRamTimeIndex in Patch.cpp corrects the injector to write the ROM's own 2.
		// Which leaves these two hooks reading an index as if it were seconds, so they go with
		// it. Treat the three as a package: fix the byte, disable both hooks.
		//
		// Hook 16 (GAME_INT+0x4) copies the byte into `time` (RAM 0x500090) every game
		// interrupt. With the byte fixed it forces `time` to 2 - two-second MATCHES. Its handler
		// is that single store plus the module's "run the original" tail, so disabling it costs
		// nothing and hands the round length back to the ROM's own table conversion.
		//
		// Hook 17 (ADV_REPLAY_WAIT1A+0x128) sets the ATTRACT DEMO's timer:
		// game_timer = min(byte, 30) * 64 (RAM 0x500028, a 16-bit 1/64-second counter). With the
		// byte fixed that is a two-second demo - both fighters still at full health and 01.73 on
		// the clock, which is how it was spotted. It REPLACES the ROM instruction at 0x96AC,
		// which is `shlo 7, 0xF, r15; stis r15, game_timer` - a hardcoded 1920 = 30 seconds. So
		// the arcade board always runs a 30-second demo regardless of the operator's TIME
		// setting, and this hook exists only to make the demo follow that setting instead.
		// Disabling it restores the board's own constant, which is the authentic behaviour.
		//
		// Disabled by DEFAULT rather than forced, so both stay visible and re-enableable in the
		// HLE ROM hooks list like every other hook. NOTE that a settings.ini written before this
		// changed carries an explicit mask with only bit 16 set, and an explicit value always
		// wins over the default - so an existing profile keeps the two-second demo until
		// "Restore defaults" is pressed (or the DisabledHleHooks* lines are deleted).
		//
		// Fighting Vipers has no counterpart to hook 16: its GAME_INT+0x8 hook (19) is Inert, so
		// FV's default mask is empty. Whether FV's attract path has an equivalent of hook 17 has
		// NOT been checked - if FV's demo ever shows the same symptom, that is the first place to
		// look. DefaultDisableMask() returns the running game's mask, always two words, and never
		// null - an unsupported game gets a zero mask.
		inline constexpr size_t HOOK_STF_GAME_INT_TIME = 16;
		inline constexpr size_t HOOK_STF_ATTRACT_TIMER = 17;

		// VIRTUAL ON SKIPS ITS WARNING SCREEN, and hook 5 is the whole of why.
		//
		// `Warning` (i960 0x3C40) is MainMode 0 - literally the first entry of the mainloop's mode
		// table at 0x18680, with Advertize (the SEGA screen) at [1]. It is guarded by an
		// "already shown" flag at guest 0x5024D4:
		//
		//     3C6C  ld   0x5024D4, g4     ; the flag
		//     3C74  lda  0x234, g2        ; 564 frames ~ 9.4 s
		//     3C7C  st   g2, 0x503A04     ; the mode timer
		//     3C84  bne  loc_3CFC         ; SET -> skip the text and MainMode++ immediately
		//           ...print the notice, hold for the timer...
		//     3D44  st   g2, 0x5024D4     ; ...then mark it shown and MainMode++
		//
		// The ROM clears that flag during BlackOut (`st g14, 0x5024D4` at 0x1871C). Hook 5 replaces
		// that instruction with a write of ONE - pre-marking the notice as already seen - so mode 0
		// falls straight through to mode 1 and the board appears to boot directly to the SEGA
		// screen. Disabling it restores the arcade boot, which is both what the hardware did and
		// what a linked pair needs: the boot sequence is where the cabinets find each other.
		//
		// MEASURED (MASTER, 1200 frames): enabled, MainMode is 1 by the first sample; disabled, it
		// stays 0 for ~564 frames and then advances - the ROM's own 0x234, to the frame. Confirmed
		// on screen by the user: the warning is displayed.
		//
		// Disabled by DEFAULT rather than forced, so it stays visible and re-enableable like every
		// other hook. NOTE the same trap as StF's: a settings.ini written before this change
		// carries an explicit `DisabledHleHooksLo.VON-K2=0`, and an explicit value always beats a
		// default - such a profile keeps skipping the warning until "Restore defaults" is pressed
		// (or the two DisabledHleHooks*.VON-K2 lines are deleted).
		inline constexpr size_t HOOK_VON_WARNING_SKIP = 5;
		const uint64_t* DefaultDisableMask();

		// Restores or re-applies each hook to match the "Disable DLL HLE ROM hooks" setting.
		// Call once per frame; no-op unless the running game has a hook table and its board
		// has booted. Works live - the original instruction words come from the DLL's own save
		// area, which the installer fills before it writes the traps.
		void Update();

		// ---- Pre-install retargeting (for program ROMs that are not Sonic the Fighters) ----
		//
		// Every offset in the table above is an address in that game's own program ROM. Point
		// the module at a different program - a homebrew Model 2B build, say - and the
		// installer still stamps every trap at those offsets, corrupting that many unrelated
		// instructions before the i960 executes a single one. Update() cannot undo that in
		// time: it runs from the UI draw, which is a full module_main call behind the first
		// frame of emulation.
		//
		// The fix is to change what the installer is told to patch, before it runs. The table
		// is in .data (writable, no VirtualProtect needed) and the installer
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
		//
		// The hook INDICES below are per-game - the two tables happen to agree on 1-7 but not
		// on `rand`, which is hook 33 in StF and 43 in FV - so the site list lives in the
		// GameHooks descriptor and is reached through Convention().
		struct ConventionSite
		{
			const char* symbol;
			uint8_t hook;
			uint8_t byteOffset;
		};
		// The running game's convention sites. Returns null and sets count to 0 when the game
		// has no hook table.
		const ConventionSite* Convention(size_t& count);

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
		size_t ApplyRetarget(const std::string retarget[MAX_COUNT]);

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
			extern uint32_t g_hitCounts[MAX_COUNT];
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
			if (index < MAX_COUNT)
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
		bool GetInstalledOffsets(uint32_t out[MAX_COUNT]);
	}
}
