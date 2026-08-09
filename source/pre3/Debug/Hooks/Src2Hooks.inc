// Split out of HleHooks.cpp (2026-08-09), which #includes this inside namespace
// pre3::HleHooks - `Info`, `Kind` and MAX_COUNT come from HleHooks.h.

		// ---- Sega Racing Classic 2 -------------------------------------------------------
		//
		// Table index 12 (M3ERomSrc2 + 0x20600, set by the ROM factory at DLL 0x180038C59), which
		// is the table at DLL 0x180108A10: 26 records. Full derivation in docs/src2-hle-hooks.md.
		//
		// SRC2 HAS NO Speed HOOKS AT ALL - not one of the 26 is a native reimplementation of a
		// ROM routine, where FV2 has twenty. Its whole table is behaviour: eight deleted
		// instructions, thirteen surgical register/branch patches, four host callbacks and a
		// single idle-loop cut-out. So the "turn off every native routine" preset has nothing to
		// offer here, and disabling anything in this list changes what the board DOES.
		//
		// Register-file offsets in the notes are the ones the handlers use, relative to cpu+8:
		// r3 = +0x1C, r16 = +0x50, f0 = +0x98, f1 = +0xA0, LR = +0x1A0, next PC = +0x0C.
		//
		// Five of the handlers (0x2CAA0, 0x2CAD0, 0x2CB00, 0x2CB10, and 0x2C640/0x2CA40 which
		// SRC2 does NOT use) are exclusive to this family; the rest of the 0x2C6A0-0x2CB20 block
		// is shared with hook tables 10 and 11, the two Daytona USA 2 variants the ROM factory
		// never builds. That is a useful sanity check: SRC2 is the only D2-derived game Gaiden
		// actually ships.
		static constexpr Info SRC2_HOOKS[] = {
			{ 0x04CA40, 0x26780, Kind::Core,    "idle-loop cut-out: yields the rest of the CPU's timeslice, then runs the original instruction. SRC2 has ONE of these where FV2 has three" },
			// HOOKS 1 AND 2 ARE A MATCHED PAIR that excises the board's SECURITY-CHIP overlay -
			// 464 bytes of XOR-0x98 obfuscated PowerPC that talks to the Model 3 security board
			// at 0xF0180000/0xF01A0000 (via the 0xFE1x mirror) and executes what it reads back.
			//
			// The module removes it because pre3's security registers are a STUB THAT LIES:
			// M3ERomSrc2 vtable slot 2 (read) is `return 0` and slot 3 (write) is a bare `ret`,
			// so the overlay's busy-wait clears on its first poll, its 0x411A8 timeout never
			// fires, and it reports SUCCESS with a buffer full of zeros. The game ships a
			// perfectly good failure path - on timeout it jumps to a fallback the loader stashes
			// at 0x55DF00 - and the stub is what makes that path unreachable.
			//
			// Read the two together; docs/src2-hle-hooks.md has the full disassembly.
			{ 0x0189EC, 0x01DC0, Kind::Removed, "deletes `bl 0x07BA18` in the board's init chain - the LOADER that XOR-0x98 deobfuscates a 464-byte overlay from 0x0D98DC to 0x55D000 and relocates it to 0x55D800. BOOT-CRITICAL: disable this alone and the board never reaches frame 1" },
			{ 0x018B28, 0x01DC0, Kind::Removed, "deletes `bla 0x55D800` - the call INTO that overlay. Load-bearing only because hook 1 removed the loader, so 0x55D800 is all zeros: disable this alone and the board calls into empty memory and freezes on its 8th draw (still exactly 8 at frames 600, 1200 and 1800)" },
			{ 0x09A3FC, 0x01DC0, Kind::Removed, "deletes the PER-FRAME `bla 0x55D800` in the game's main loop (FUN_0009A39C) - the third call into the same security overlay hooks 1 and 2 excise. NOT harmless to disable, despite the blob decrypting to a stateless device-transfer routine: attract and races run, but the TEST switch crash-reboots the board into the MODEL3 SYSTEM PROGRAM screen and the menu never opens (measured 2026-08-08 - it cost the service menu for a day). BOOT-CRITICAL like its pair: the overlay it calls is never loaded" },
			{ 0x006754, 0x267E0, Kind::Host,    "whole routine replaced: appends r3 to a growable u32 array on the ROM object (data at rom+0x488, capacity +0x490, count +0x494), then returns to the link register. Same handler FV2 uses at its three sites" },
			{ 0x007480, 0x2C6A0, Kind::Host,    "arcade settings injection: writes the coin setting (free play -> 0x1B), region, difficulty and the linked-cabinet fields from the YAMP config into game RAM at 0x72629C, runs the original instruction (`lis r16, 0x73`), then stores 50.0f to guest 0xC4550, 0xC4590 and 0xC4610. BOOT-CRITICAL: disable it and the board never reaches frame 1 - SRC2 does not merely prefer configured settings, it will not start without them" },
			{ 0x01922C, 0x2C6E0, Kind::Patch,   "forces r3 = 0, then runs the original instruction - and that instruction is `cmpwi r3, 1` on the COUNTRY byte the boot chain just read from the settings working copy (guest 0x100198 = 0x100180+0x18). COUNTRY == 1 (JAPAN) is what makes the ROM draw the START-UP WARNING SCREEN, so forcing 0 (INTERNATIONAL) suppresses it. THE warning-screen switch, measured 2026-08-08 by cold-boot bisection: disabling this hook ALONE brings the screen back; disabling 16-21 without it does nothing. With a non-JAPAN COUNTRY the ROM skips the screen natively, hook or no hook" },
			{ 0x073CC4, 0x2C720, Kind::Patch,   "f0 += 0.05, f1 -= 0.05, then runs the original instruction" },
			{ 0x073CFC, 0x2C780, Kind::Patch,   "f0 += 0.02, f1 += 0.0, then runs the original instruction" },
			{ 0x073D70, 0x2C7E0, Kind::Patch,   "f0 += 0.16, f1 -= 0.1, then runs the original instruction" },
			{ 0x073DFC, 0x2C840, Kind::Patch,   "f0 += 0.17, f1 -= 0.1, then runs the original instruction" },
			{ 0x073E34, 0x2C8A0, Kind::Patch,   "f0 += 0.15, f1 -= 0.1, then runs the original instruction" },
			{ 0x069C60, 0x2C900, Kind::Patch,   "f0 += 0.0, f1 -= 0.1, then runs the original instruction" },
			{ 0x02D2F0, 0x2C960, Kind::Host,    "THE CPU CARS. Multiplies by the float at rom+0x374, then runs the original - which is `stfs f1, 0x1e4(r15)`, the store of the MOTION-INTEGRATION SCALAR at guest 0x1051E4 (guest 0x08DC98 multiplies all three velocity components by it before adding them to position). rom+0x374 is execute_info+0x1684, which YAMP used to fill with FV2's button-assign bytes - a float denormal ~1e-36 - so the scalar was zeroed and every AI car slowed to a stop a few seconds into a race. Host now writes 1.0f there (pre3_execute_info_t::src2_scalars), which makes this hook the identity" },
			{ 0x036E2C, 0x2C9D0, Kind::Host,    "runs the original instruction FIRST, then multiplies r3 by the float at rom+0x378 = execute_info+0x1688 - same story as hook 13, and the value it scales feeds `DAT_00105010 += DAT_00106254`, a bonus-time/score accumulator. Also 1.0f now, so also the identity" },
			{ 0x07BC4C, 0x2CB20, Kind::Host,    "settings-blob upload: when the host config's +0x07 flag is set, copies the 0x1000 bytes at config +0x08 into guest RAM at 0x2000 and clears the flag. YAMP leaves both zero, so it does nothing today. Then runs the original instruction" },
			// HOOKS 16-21 ARE THE BOOT PRESENTATION. Disabling all six brings back the start-up
			// WARNING screen and the Daytona 2 TITLE LOGO (measured as a group, user-confirmed).
			// Which hook belongs to which screen is read off the two routines they sit in, NOT
			// yet split by measurement - two runs with 0x10000 and 0x1E0000 would settle it.
			// Gaiden hides them because its emulator is a minigame inside a menu; YAMP is the
			// cabinet, so the board's own power-up sequence is the authentic behaviour - the same
			// argument DefaultDisableMask already makes for FV2's hooks 7 and 10.
			{ 0x091660, 0x2CAA0, Kind::Patch,   "forces a conditional branch ALWAYS TAKEN: next PC = PC + (sign-extended low halfword of the instruction, low two bits cleared). Sits in FUN_00091600, which draws message ids 0x1BC and 0x5A8 at locate(0,10,2) / locate(0x38,0x11,2) once the screen timer passes 0x11C and 0x54 - i.e. the START-UP WARNING SCREEN" },
			{ 0x091800, 0x2CAA0, Kind::Patch,   "forces a conditional branch always taken (second site). This one and the three below are in FUN_000917C0 - a 3D sequence that walks a table of position triples, draws ids 0x6D6/0x6D7 and indexes a 32-entry curve by the screen timer with a fade under it: the DAYTONA 2 TITLE LOGO" },
			{ 0x09185C, 0x01DC0, Kind::Removed, "instruction deleted (title logo sequence)" },
			{ 0x0918A4, 0x01DC0, Kind::Removed, "instruction deleted (title logo sequence)" },
			{ 0x0918D0, 0x01DC0, Kind::Removed, "instruction deleted (title logo sequence)" },
			{ 0x06A4C4, 0x2CAD0, Kind::Patch,   "forces a branch-to-link-register always taken - a conditional return made unconditional. Same boot-presentation cluster: the screen sequencer calls this routine at 0x91560, immediately before setting the screen timer to -1 and incrementing the screen index at r15+4, so it is plausibly the skip/advance check. Role unread" },
			{ 0x0420B8, 0x2CB00, Kind::Patch,   "instruction replaced by r16 = 0x53B; the original never runs" },
			{ 0x042134, 0x2CB10, Kind::Patch,   "instruction replaced by r3 = 0x53B; the original never runs" },
			{ 0x01B834, 0x01DC0, Kind::Removed, "instruction deleted" },
			{ 0x01B854, 0x01DC0, Kind::Removed, "instruction deleted" },
		};
		static_assert(sizeof(SRC2_HOOKS) / sizeof(SRC2_HOOKS[0]) <= MAX_COUNT);
