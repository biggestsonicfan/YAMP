#pragma once

#include <cstddef>
#include <cstdint>

namespace pre3
{
	// The pre3 module does not run its Model 3 program unmodified either - but it goes about it
	// very differently from the m2ftg boards, and the difference is what makes this worth
	// describing rather than copying.
	//
	// m2ftg overwrites i960 instructions in the ROM image with trap words. pre3 leaves guest
	// memory completely alone: its PowerPC core is a DECODED-TRACE interpreter, and the hook is
	// applied while that trace is built.
	//
	//   * CPpc603e::init (DLL 0x180306B0) picks one of NINETEEN hook tables out of
	//     DLL 0x1801084A0, indexed by a per-game constant the ROM object supplies through its
	//     own vtable slot 35. Fighting Vipers 2 asks for index 3 - the table at DLL 0x180108FE0,
	//     36 records of {u32 guestAddress, u32 pad, u64 handler}, terminated by a null handler.
	//   * That same function copies each record's handler into the interpreter's dispatch table
	//     at DLL 0x52AA90, at index `i | 0x800` - i.e. into the slots that PowerPC primary
	//     opcode 1 would occupy. Opcode 1 is unassigned on PowerPC, so those slots are free.
	//   * The pre-decoder (DLL 0x180015E50) walks the whole code region once, and for every
	//     instruction writes an 8-byte trace entry {u32 originalWord, u32 handlerIndex} into the
	//     array at cpu+0x2F8. The index is normally derived from the word
	//     (`(word >> 15) & 0x1F800 | word & 0x7FF`, i.e. primary << 11 | extended); when the
	//     address matches a record it writes `i | 0x800` instead.
	//   * The execute loop (DLL 0x1800164D0) reads a trace entry and calls
	//     dispatch[entry.handlerIndex]. So the hook lives entirely in the trace.
	//
	// Two consequences the m2ftg path does not have. The guest's own memory is never modified,
	// so a hook cannot be seen (or undone) by looking at RAM. And because the trace entry keeps
	// the ORIGINAL instruction word alongside the index, restoring a hook's entry to the index
	// that word decodes to is a complete, reversible disable - which is what this module does,
	// live, every frame.
	//
	// (Disabling loses the pre-decoder's peephole specialisation for that one instruction: it
	// picks a faster equivalent handler for some words, and only for entries it did not hook.
	// The generic handler for the opcode is by definition correct, so this costs a little speed
	// on one instruction and changes nothing else.)
	namespace HleHooks
	{
		enum class Kind : uint8_t
		{
			// Emulator plumbing. The three idle-loop cut-outs: the handler notices the board is
			// spinning, abandons the rest of the CPU's timeslice and then runs the original
			// instruction anyway. Disabling one does not break the board, it makes it burn the
			// whole slice spinning - the frame rate goes with it.
			Core,
			// Host integration: arcade settings injected into the game's own RAM copy, a table
			// clear, and the routine calls that reach the host through the board object.
			// Disabling these boots but loses the feature.
			Host,
			// A native x64 reimplementation of a whole ROM routine - the handler ends by
			// returning to the link register, so the ROM's body never runs. These are pure speed:
			// FV2's are the matrix/vector maths and the block memory operations. Disabling one
			// hands the work back to the interpreted PowerPC code, which is slower and otherwise
			// identical - and is the way to check whether one of them is wrong.
			Speed,
			// The handler rewrites the instruction word and then executes it, or pokes a register
			// before letting the original run. Small, surgical changes to what the ROM does.
			Patch,
			// The ROM code at that address is deleted: either the single instruction (the handler
			// is a bare return) or the whole routine (the handler immediately returns to the link
			// register without doing anything first).
			Removed,
		};

		struct Info
		{
			uint32_t guestAddress;   // where in the board's code the hook sits
			uint32_t handlerRva;     // the native handler, RVA in the module
			Kind kind;
			const char* note;
		};

		// Upper bound across every game described here, and the size of every per-hook array.
		// The live count is Count(); this only sizes storage. Must stay <= 128 - the disable mask
		// is a uint64_t[2].
		inline constexpr size_t MAX_COUNT = 64;
		static_assert(MAX_COUNT <= 128, "the disable mask is a uint64_t[2]");

		// Hooks in the running game's table, or 0 when this game has no description here.
		size_t Count();
		bool Supported();
		const Info& Get(size_t index);
		const char* KindName(Kind kind);
		const char* KindDescription(Kind kind);

		// Bit i disables hook i, in the module's own table order - the order the settings list
		// shows them in.
		bool MaskTest(const uint64_t mask[2], size_t index);
		void MaskSet(uint64_t mask[2], size_t index, bool disabled);
		void MaskForKinds(uint64_t mask[2], unsigned kinds);
		bool MaskStripKinds(uint64_t mask[2], unsigned kinds);
		inline constexpr unsigned KindBit(Kind kind) { return 1u << static_cast<unsigned>(kind); }
		inline constexpr unsigned ALL_KINDS =
			KindBit(Kind::Core) | KindBit(Kind::Host) | KindBit(Kind::Speed)
			| KindBit(Kind::Patch) | KindBit(Kind::Removed);
		// The preset that answers "is one of the native routines wrong?" - every whole-routine
		// replacement off, everything the board needs left alone.
		inline constexpr unsigned NATIVE_ROUTINE_KINDS = KindBit(Kind::Speed);

		// Unlike m2ftg's, none of pre3's hooks are load-bearing enough to hang the board when
		// disabled - the Core ones only cost frame rate - so nothing here is session-only and the
		// whole mask persists.
		const uint64_t* DefaultDisableMask();

		// Reconciles every hook with the mask. Call once per frame; no-op until the board's CPU
		// exists and its trace has been decoded. Works live and is fully reversible, because the
		// trace entry carries the instruction word the hook replaced.
		void Update(const uint64_t mask[2]);

		// True once Update has found a live CPU and trace - i.e. the checkboxes are doing
		// something. False before the board is up.
		bool Live();

		// Diagnostics for the settings panel: how many of the described records actually matched
		// the module's live table. A mismatch means this build's table is not the one the
		// descriptions were written against, and Update refuses to touch anything.
		size_t LiveRecordCount();
	}
}
