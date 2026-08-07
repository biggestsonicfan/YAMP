// GENERATED from the module's own HLE table (RVA 0x476520, 120 records) cross-referenced
// against its ROM symbol table (RVA 0x4507E0, 202 entries), then CLASSIFIED 2026-08-05 by
// reading every handler in Ghidra and every hook site in IDA. See docs/von-hle-hooks.md.
//
// RENUMBERED IN THE SAME PASS. The table was read one record low (0x476510), which invented a
// hook 0 whose "handler" was the CRT's free() and pushed every real hook one index up. The
// installer settles the base: `FUN_1800048e0` walks `&DAT_180476520 + i * 0x10` for i in
// 0..0x77. So every index below is ONE LOWER than in notes written before 2026-08-05, and it
// now matches the index the module encodes in its own trap words (0x04 << 24 | i * 4).
//
// Two shared tails, and telling them apart is most of the classification:
//   +0x070FB0   jmp to the decoder - the ROM instruction runs unchanged. Provably Inert; 63 of
//               the 120 handlers are literally this, and they are debug probes whose bodies were
//               compiled out of the retail build.
//   +0x004A10   returns the instruction LENGTH without decoding it - the ROM instruction is
//               DELETED. Four hooks point straight at it.
// A handler that ends `FUN_180004980(index)` runs its own work and then the original; one that
// ends by returning a length has replaced the original outright; one that calls FUN_1800446A0
// and returns 0 has performed the i960 `ret` itself (whole-function HLE, as StF does for `rand`).
//
// THE BOOT PATH is hooks 0-3 and 17, but ONLY 1 AND 2 ARE LOAD-BEARING, and that distinction was
// MEASURED, not reasoned: each was disabled from before module_start with `-von-hleoff` and given
// 600 frames under cdb. 1 and 2 spin the host at 100% CPU with zero frames and zero audio; 0, 3
// and 17 reach the frame limit with the same 14 attract cues as an unmodified run, so they are
// Host, not Core.
//
// Worth recording because the static reading said otherwise: hook 0 sets the flag (0x6910DF) that
// the module's own bring-up state machine FUN_180072FB0 waits on in state 0x12, which reads as
// "the board cannot finish booting without it". The board boots anyway. That state machine gates
// the module's own presentation, not the emulated board - it is not what makes the ROM run.
constexpr m2ftg::HleHooks::Info VON_HOOKS[] = {
	{ 0x0187E0, 0x0707D0, "BlackOut+0x190", Kind::Host, false, "the bring-up handshake: sets the module flag (DLL 0x6910DF) that its own boot state 0x12 waits on, and clears 0x2000 bytes of text RAM. MEASURED with it off: the board still boots and runs attract for 600 frames, so the state machine it stalls is the module's presentation, not the emulated board" },
	{ 0x028E2C, 0x070800, "SyncGeometrizer.lf+0x44", Kind::Core, false, "supplies the VBLANK EDGE. The ROM spins on `ld (g6)` with g6 = 0x98000C until bit2 changes; this toggles bit2 of the module's per-board copy. MEASURED with it off: the spin never ends and never yields, so the host burns 100% CPU with zero frames" },
	{ 0x018AB4, 0x070840, "synch+0x4", Kind::Core, false, "THE FRAME YIELD, on the instruction after the vblank spin returns: raises the CPU stop flag, flips the double-buffered vblank record, then runs the original. Every wait in the ROM yields through synch. MEASURED with it off: same 100%-CPU stall as hook 1" },
	{ 0x002754, 0x004A10, "InitIO+0x24", Kind::Host, false, "deletes `be loc_275C`, which forces BoardType (0x5023E0) to 1 and skips InitIO_Basic (0x26E8). Restoring it hands cabinet detection back to an I/O read - which boots (measured, 600 frames), though what a BoardType of 0 changes later has not been chased" },
	{ 0x003464, 0x0708A0, "InitCoinSetting+0x34", Kind::Host, false, "arcade settings: writes backup 0x1D00034 (FREE PLAY) from the YAMP setting plus a coin-settings block, and 0x1D00024 when two-board is on, in place of the ROM's `stob g14, 0x1D00034`" },
	{ 0x01871C, 0x0709A0, "BlackOut+0xCC", Kind::Host, false, "SKIPS THE WARNING SCREEN, and is DISABLED BY DEFAULT so it does not. Guest 0x5024D4 is the `Warning` mode's already-shown flag; the ROM clears it here and this writes 1 instead, so MainMode 0 falls straight through to the SEGA screen. Measured: off, the notice holds for the ROM's own 564 frames. See HOOK_VON_WARNING_SKIP" },
	{ 0x018A10, 0x070A00, "InitNetwork", Kind::Host, false, "MASTER/SLAVE injector: backup 0x1D00028 = 2 standalone, else 1 on board 0 and 0 on board 1. The link handshake itself runs raw" },
	{ 0x0187E4, 0x070C60, "__mainloop__", Kind::Host, false, "service jump: after the original `call synch`, if the module's request flag is raised (set by its board reset), redirects the i960 to ClearBackup (0x2350) or 0xE37F0" },
	{ 0x019C30, 0x070A60, "Game_00", Kind::Content, false, "attract/demo control off the module's own guest counter 0x504D10; also clears four banks of text RAM" },
	{ 0x019C60, 0x070B00, "Game_00+0x30", Kind::Content, false, "attract/demo control (0x504D10), with an i960 `ret` on the early path" },
	{ 0x02DD30, 0x070B50, "Demoplay_00", Kind::Content, false, "demo entry: branches the i960 to 0x226B0, sets the demo timer 0x503A04 = 0x78 and the fade to 1.0" },
	{ 0x02DD34, 0x070BD0, "Demoplay_00+0x4", Kind::Content, false, "demo timer countdown and fade-out" },
	{ 0x02BB34, 0x070C30, "Advertize+0x154", Kind::Content, false, "attract: 0x504D10 = -1, clears the fade, replaces the original" },
	{ 0x0188B0, 0x004A10, "__mainloop__+0xCC", Kind::Removed, false, "deletes `stob 4, 0x1400000`, an I/O write the module does not model, once per mainloop pass" },
	{ 0x0AB9A4, 0x070CB0, "PutPhalanx+0x204", Kind::Content, false, "loop clamp: once the count register reaches 60, skip 0x130 bytes instead of running the body" },
	{ 0x034540, 0x070CD0, "Demoplay_10+0x6670", Kind::Content, false, "writes a computed double into the register pair the demo path reads" },
	{ 0x029D40, 0x070D50, "InitFade+0xA0", Kind::Content, false, "zeroes a register before the original runs" },
	{ 0x0CE68C, 0x070D70, "InitSelect+0x1C", Kind::Host, false, "the same render-request plus CPU-yield pair as hook 1, on the character-select init. Not required to boot (measured, 600 frames) - hook 2 already yields every frame - but the select screen itself was not reached in that test" },
	{ 0x0DBBFC, 0x070DA0, "Ending3+0x10C", Kind::Content, false, "ending pacing: clamps a register and replaces the original below a threshold" },
	{ 0x02BB2C, 0x070E10, "Advertize+0x14C", Kind::Host, false, "LINKED CABINETS: reads the emulated CommData+4 (0x1A12704) and sets the register to (value == 0x21); board 1 forces 1. Gated on the two-board gate and the game mode" },
	{ 0x018CDC, 0x070F00, "InitSelectV+0xDC", Kind::Host, false, "LINKED CABINETS: clears a register on the versus-select init" },
	{ 0x018D6C, 0x070F30, "InitSelectV+0x16C", Kind::Host, false, "LINKED CABINETS: VersusMode (0x503A7C) = 1 and cSend+4 (0x5032F4) = 0x21, replacing the original" },
	{ 0x0190BC, 0x070FB0, "WaitAnother+0x8C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CE108, 0x070FC0, "LinePut+0x8", Kind::Host, false, "LINKED CABINETS: clears a register before the original runs" },
	{ 0x01978C, 0x070FF0, "WaitChallenger+0xCC", Kind::Host, false, "LINKED CABINETS: on board 0, replaces the original" },
	{ 0x0197E0, 0x071060, "WaitChallenger+0x120", Kind::Host, false, "LINKED CABINETS: RoundnumberVS (0x503A84) takes the halfword at 0x5024F8 on board 1" },
	{ 0x086D68, 0x070FB0, "InitAiLearning+0x6A8", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x01B5AC, 0x0710C0, "Game_30+0x13C", Kind::Host, false, "LINKED CABINETS: compares SetWin/SetLose against NumOfSets and performs the i960 `ret` when the match is not over" },
	{ 0x01B9A4, 0x071130, "InitGameOverReal+0x24", Kind::Host, false, "LINKED CABINETS: raises a register flag and calls the module's own game-over routine" },
	{ 0x0190D8, 0x071180, "InitGameMain+0x8", Kind::Host, false, "LINKED CABINETS: replaces the original while linked" },
	{ 0x018ED8, 0x071180, "SelectV+0x138", Kind::Host, false, "LINKED CABINETS: replaces the original while linked" },
	{ 0x019054, 0x071180, "WaitAnother+0x24", Kind::Host, false, "LINKED CABINETS: replaces the original while linked" },
	{ 0x0CE78C, 0x070FB0, "InitSelect+0x11C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CE798, 0x070FB0, "InitSelect+0x128", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CD918, 0x070FB0, "SelectMove+0x368", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x02A4E0, 0x0711E0, "entrySoundCode", Kind::Host, false, "audio bridge: issues the register's sound command to the host mixer, then performs the i960 `ret` - whole-function HLE" },
	{ 0x02A580, 0x0711E0, "entrySoundCode+0xA0", Kind::Host, false, "audio bridge, as hook 35" },
	{ 0x02A5F0, 0x0711E0, "entrySoundCode+0x110", Kind::Host, false, "audio bridge, as hook 35" },
	{ 0x02A6B0, 0x071220, "entrySoundCode+0x1D0", Kind::Host, false, "audio bridge: command 0xA0010000 | reg, then the i960 `ret`" },
	{ 0x02A768, 0x071220, "entrySoundCode+0x288", Kind::Host, false, "audio bridge, as hook 38" },
	{ 0x02A7C0, 0x071260, "entrySoundCode+0x2E0", Kind::Host, false, "audio bridge: command 0xA0030000 | reg, then the i960 `ret`" },
	{ 0x02A810, 0x0712A0, "entrySoundCode+0x330", Kind::Host, false, "audio: STUBBED - returns to the caller without issuing anything" },
	{ 0x02A840, 0x0712A0, "entrySoundCode+0x360", Kind::Host, false, "audio: STUBBED - returns to the caller without issuing anything" },
	{ 0x02A870, 0x0712A0, "setSoundSlot", Kind::Host, false, "audio: STUBBED - returns to the caller without issuing anything" },
	{ 0x02BB50, 0x0712B0, "Advertize+0x170", Kind::Host, false, "attract audio: issues sound commands 0x1123/0x1147/0x1122/0x1146, then runs the original" },
	{ 0x0F5060, 0x070FB0, "rand.lf+0x8", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged. NOTE this is where StF and FV put whole-function RNG HLE; Virtual On's RNG runs in the ROM" },
	{ 0x034374, 0x071300, "Demoplay_10+0x64A4", Kind::Content, false, "demo: compares a float against 0x504B9C and skips 0x4CC bytes when it is short" },
	{ 0x034840, 0x070FB0, "Demoplay_10+0x6970", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x019C68, 0x071380, "Game_00+0x38", Kind::Host, false, "audio bridge: sound command 0xA00F0000 | halfword at 0x504CC0, then the original" },
	{ 0x0CD838, 0x070FB0, "SelectMove+0x288", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CD8BC, 0x070FB0, "SelectMove+0x30C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x019830, 0x070FB0, "InitGame", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x028D84, 0x0716C0, "InitTGP+0x4", Kind::Host, false, "builds the display ramp table natively and replaces the ROM's `call sub_28840`, which selects RGB gamma factors from backup 0x1D00027 and fills the same table a word at a time" },
	{ 0x0E1F10, 0x071850, "MakeRampIntensityTable.lf+0x108", Kind::Host, false, "builds the second ramp table natively, then runs the original" },
	{ 0x0E2150, 0x070FB0, "InitRampSet+0x20", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0E2354, 0x070FB0, "RampSet+0x24", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0D279C, 0x070FB0, "RoboPut+0x2C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x019E80, 0x070FB0, "Game_00+0x250", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0D3998, 0x070FB0, "waitInterDemo1+0x8", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0E455C, 0x070FB0, "InitNameEntry+0x30C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0E4920, 0x070FB0, "InitNameEntry2+0x200", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x02DD54, 0x070FB0, "Demoplay_00+0x24", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0DB028, 0x070FB0, "dispJunk+0xE8", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0DB364, 0x070FB0, "dispJunk+0x424", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0DB594, 0x070FB0, "dispJunk+0x654", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0C9B5C, 0x070FB0, "FixPutRoboData+0x15C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0C9B78, 0x070FB0, "FixPutRoboData+0x178", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CE19C, 0x070FB0, "LinePut+0x9C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CE450, 0x070FB0, "LinePut+0x350", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CDA10, 0x070FB0, "SelectMove+0x460", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CDA48, 0x070FB0, "SelectMove+0x498", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0DBCD4, 0x070FB0, "Ending3+0x1E4", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0E658C, 0x070FB0, "demoRanking2+0x3CC", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x075E84, 0x070FB0, "ai_init+0xF4", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x02771C, 0x070FB0, "initRobot+0x1CC", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CD638, 0x071870, "SelectMove+0x88", Kind::Content, false, "select shortcut: skips 0xD0 bytes when the input word 0x5024A4 has bits 0x804000 set" },
	{ 0x0E4ED4, 0x0718A0, "NameEntry+0x3F4", Kind::Content, false, "name entry shortcut: skips 0xAC bytes on the same input bits" },
	{ 0x0CD848, 0x0718D0, "SelectMove+0x298", Kind::Host, false, "raises the K2 progress-report flag for the select screen, and skips the original on the input bits" },
	{ 0x0CDADC, 0x0718D0, "SelectMove+0x52C", Kind::Host, false, "as hook 77" },
	{ 0x0E4F9C, 0x0718D0, "NameEntry+0x4BC", Kind::Host, false, "as hook 77" },
	{ 0x027618, 0x070FB0, "initRobot+0xC8", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0250D8, 0x070FB0, "makebutton+0x98", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x02632C, 0x070FB0, "makebutton+0x12EC", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x03383C, 0x070FB0, "Demoplay_10+0x596C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x076FBC, 0x070FB0, "ai_init+0x122C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CD8A8, 0x070FB0, "SelectMove+0x2F8", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0CD8D4, 0x071960, "SelectMove+0x324", Kind::Host, false, "reports the chosen VR (machine_P, 0x503A98) to the Kiwami 2 host block and raises its flag bit 0x100" },
	{ 0x01B470, 0x071990, "Game_30", Kind::Host, false, "reports the match result (0x503A80) to the Kiwami 2 host block and raises its flag bit 0x200, once the set is decided" },
	{ 0x01A2A4, 0x070FB0, "Game_01+0x24", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0D5EB0, 0x070FB0, "initEnding0", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0DB858, 0x070FB0, "dispJunk+0x918", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0DE638, 0x070FB0, "execCamera+0x8", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x09BAA0, 0x070FB0, "execObstacle", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x09BD7C, 0x070FB0, "execObstacle+0x2DC", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x09C050, 0x070FB0, "execObstacle2", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x09C2CC, 0x070FB0, "execObstacle2+0x27C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0C7BDC, 0x070FB0, "getCommStatus.lf+0x1F34", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0C7BF0, 0x070FB0, "getCommStatus.lf+0x1F48", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0C7F34, 0x070FB0, "getCommStatus.lf+0x228C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0C7F44, 0x070FB0, "getCommStatus.lf+0x229C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0C773C, 0x070FB0, "getCommStatus.lf+0x1A94", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0C7768, 0x070FB0, "getCommStatus.lf+0x1AC0", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0987A8, 0x070FB0, "Replay+0x10CE8", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x098A0C, 0x070FB0, "Replay+0x10F4C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x09844C, 0x0719D0, "Replay+0x1098C", Kind::Content, false, "replay: forces a register to 0x5800 before the original runs" },
	{ 0x098988, 0x071A30, "Replay+0x10EC8", Kind::Content, false, "replay: forces a register to 0x5800 before the original runs" },
	{ 0x0DA574, 0x070FB0, "Ending2+0x1234", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0DA5D8, 0x070FB0, "Ending2+0x1298", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0DADCC, 0x070FB0, "Ending2+0x1A8C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0DAE30, 0x070FB0, "Ending2+0x1AF0", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x071410, 0x004A10, "execRader+0x390", Kind::Removed, false, "the shared \"skip original\" tail: the ROM instruction is deleted and nothing runs in its place" },
	{ 0x071778, 0x004A10, "execRader+0x6F8", Kind::Removed, false, "the shared \"skip original\" tail: the ROM instruction is deleted and nothing runs in its place" },
	{ 0x024188, 0x070FB0, "ScrollANullSet+0x1510", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x02422C, 0x070FB0, "ScrollANullSet+0x15B4", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x0245D4, 0x070FB0, "ScrollANullSet+0x195C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x024690, 0x070FB0, "ScrollANullSet+0x1A18", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x024E20, 0x070FB0, "ScrollANullSet+0x21A8", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x024EB4, 0x070FB0, "ScrollANullSet+0x223C", Kind::Inert, false, "the shared \"execute original\" tail: the ROM instruction runs unchanged" },
	{ 0x01CD00, 0x071B30, "SetBackground.lf+0x8", Kind::Content, false, "scales the RGB555 background colour in the register by the module's brightness factor" },
	{ 0x01BC10, 0x071C00, "SetScrollcol+0x80", Kind::Content, false, "remaps the scroll colour index through the module's own table for a range of values" },
};
