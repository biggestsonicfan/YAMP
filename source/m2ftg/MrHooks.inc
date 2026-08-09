// GENERATED 2026-08-09 from the Motor Raid module's own HLE table (RVA 0x276D80, 64 records,
// installer FUN_180052680) cross-referenced against the ROM symbol table the DLL embeds
// (RVA 0x17A700, 3987 entries — AM2's own names, source tree `am1_users2_yasuda_mb_src`).
//
// Two shared tails, same scheme as every other m2ftg module:
//   +0x059B70   `jmp +0x38C0` (the execute-original tail) — provably Inert; 20 handlers are
//               literally this: debug probes whose bodies were compiled out of retail.
//   +0x003960   returns the instruction LENGTH without decoding — the ROM instruction is
//               DELETED (the skip tail).
//
// THE INSTALLER ONLY STAMPS OFFSETS BELOW 0x200000. Twenty records target the RAM2BASE
// overlay (0x21xxxx — code the ROM copies to RAM at boot), and the LJ build's installer
// skips every one of them: no trap is ever written, no saved word exists, the hook NEVER
// FIRES. They are listed because they are in the module's table (and were presumably live
// on the PS3, where e.g. deleting the three `addr` doublings at _NetGameDataTransRun1+0x74/
// 84/94 halves the bike marshal's velocity extrapolation), and the Update() reconciler's
// `romOffset >= 0x200000` guard — the installer's own — already leaves them alone.
//
// Classification is PARTIAL, deliberately: only handlers actually read in Ghidra carry a
// meaningful Kind; the rest are Unclassified, which the settings UI treats as session-only
// (see HleHooks.h) — live toggling allowed, persistence not, exactly because a wrong guess
// here could leave the game unbootable behind a saved mask.
//
// Network-relevant rows for the linked-cabinet work (docs/mr-comm-packet.md): 21/22 delete
// the "CHECKING NETWORK NOW"/"Ver.5.00" prints, 34/35 post an i960 interrupt through the
// check's wait states, 36/37 are the two-board attract sync (inert with one board), and
// 40/41/32/42-44 are network sites that are either Inert or never installed.

	constexpr Info MR_HOOKS[] =
	{
		/*  0 */ { 0x03CAEC, 0x59A50, "main+0x10c",                Kind::Host,        false, "Sets the module bring-up flag (DLL+0x741CB5, what the boot state machine's state 0x12 waits on) and clears 0x2000 bytes of the text layer, then runs the original. Same shape as Virtual On's hook 0." },
		/*  1 */ { 0x030204, 0x59A80, "SyncGeometrizer+0x34",      Kind::Core,        false, "Geometry-processor handshake: raises the render/display-list flags the host renderer consumes, then runs the original." },
		/*  2 */ { 0x02F178, 0x59B70, "InitTGP+0x68",              Kind::Inert,       false, "" },
		/*  3 */ { 0x091C1C, 0x59B80, "soundPortWait+0x1c",        Kind::Unclassified, false, "Writes 7 into an i960 context field, then runs the original — reads as a sound-port wait pump; not yet confirmed." },
		/*  4 */ { 0x01F5C4, 0x59BA0, "eepRead+0xc4",              Kind::Unclassified, false, "The serial-eeprom read bit-bang (game assignment storage). Handler not yet reversed." },
		/*  5 */ { 0x01F4F8, 0x03960, "eepWrite+0x118",            Kind::Removed,     true,  "Instruction deleted — part of the eeprom write wait." },
		/*  6 */ { 0x037B90, 0x59BC0, "MakeVolumeTable+0x40",      Kind::Unclassified, false, "" },
		/*  7 */ { 0x01A4D4, 0x59C50, "InitCredit+0xd4",           Kind::Unclassified, false, "Credit/coin bridge, most likely the LJ coin protocol's injection point." },
		/*  8 */ { 0x09E520, 0x59D30, "WarningDisp+0x20",          Kind::Unclassified, false, "The boot warning screen. If Motor Raid appears to skip it, this is where." },
		/*  9 */ { 0x03CA44, 0x59D90, "main+0x64",                 Kind::Unclassified, false, "Early main-loop site; likely the per-frame yield." },
		/* 10 */ { 0x21C4F4, 0x59B70, "ScrollWindowBit+0x84",      Kind::Inert,       false, "NEVER INSTALLED: targets the RAM overlay (>= 0x200000), which the installer skips." },
		/* 11 */ { 0x21C7F4, 0x59B70, "ScrollWindowBitFlash+0xe4", Kind::Inert,       false, "NEVER INSTALLED (overlay target)." },
		/* 12 */ { 0x078B64, 0x59E10, "replayWindowRun+0x94",      Kind::Unclassified, false, "" },
		/* 13 */ { 0x2199F4, 0x59B70, "turboMarkPut+0x144",        Kind::Inert,       false, "NEVER INSTALLED (overlay target)." },
		/* 14 */ { 0x219A08, 0x5A080, "turboMarkPut+0x158",        Kind::Unclassified, false, "NEVER INSTALLED (overlay target)." },
		/* 15 */ { 0x219B74, 0x5A0F0, "instturboMarkPut+0x134",    Kind::Unclassified, false, "NEVER INSTALLED (overlay target)." },
		/* 16 */ { 0x2198B8, 0x5A0B0, "turboMarkPut+0x8",          Kind::Unclassified, false, "NEVER INSTALLED (overlay target)." },
		/* 17 */ { 0x09BD6C, 0x59B70, "instructPutTask+0x13c",     Kind::Inert,       false, "" },
		/* 18 */ { 0x09BFA0, 0x59B70, "instructPutTask+0x370",     Kind::Inert,       false, "" },
		/* 19 */ { 0x219A88, 0x5A110, "instturboMarkPut+0x48",     Kind::Unclassified, false, "NEVER INSTALLED (overlay target)." },
		/* 20 */ { 0x079A04, 0x59B70, "DoubleWindowObjPut+0x244",  Kind::Inert,       false, "" },
		/* 21 */ { 0x04A618, 0x03960, "startComCheck+0x68",        Kind::Removed,     true,  "Deletes the `call puts` that prints \"CHECKING NETWORK NOW\" during the network check." },
		/* 22 */ { 0x04A624, 0x03960, "startComCheck+0x74",        Kind::Removed,     true,  "Deletes the `call puts` that prints \"      Ver.5.00      \"." },
		/* 23 */ { 0x09B004, 0x5A130, "titleLogoFadeinRun+0x134",  Kind::Unclassified, false, "" },
		/* 24 */ { 0x09B4B4, 0x5A1C0, "TopoPutConst+0x14",         Kind::Unclassified, false, "" },
		/* 25 */ { 0x091980, 0x5A280, "SoundRequest_main",         Kind::Host,        false, "Sound-request bridge into the host's CRI stack (whole-function shape, like StF's sound hooks)." },
		/* 26 */ { 0x091B30, 0x5A280, "SoundRequestDirect",        Kind::Host,        false, "Same handler as 25." },
		/* 27 */ { 0x091B80, 0x5A2B0, "SoundRequestCoin",          Kind::Host,        false, "Coin-chime variant of the sound bridge." },
		/* 28 */ { 0x028F74, 0x5A2E0, "gameoverTaskRun+0x24",      Kind::Unclassified, false, "" },
		/* 29 */ { 0x019A48, 0x5A340, "AdvtiseModeRun+0x18",       Kind::Unclassified, false, "" },
		/* 30 */ { 0x09AAE0, 0x5A3A0, "MotorRaidTitleInit",        Kind::Unclassified, false, "" },
		/* 31 */ { 0x00F5BC, 0x5A410, "SoundCtrlConst+0xec",       Kind::Unclassified, false, "" },
		/* 32 */ { 0x21D8A0, 0x59B70, "rand.lf+0x8",               Kind::Inert,       false, "NEVER INSTALLED (overlay target) — and Inert anyway. The RNG is NOT substituted in this build; the netplay RNG lessons from VF2 do not have a hook to act through here." },
		/* 33 */ { 0x013CDC, 0x59B70, "InitGameSet+0x12c",         Kind::Inert,       false, "Probe point inside the game-assignment derivation (docs/mr-comm-packet.md 2)." },
		/* 34 */ { 0x04AAF0, 0x5A470, "checkNetwork+0x260",        Kind::Host,        false, "Posts i960 interrupt (ctx+0xA0 = 3) each pass through the network check's node wait, then runs the original — keeps the board serviced while it polls the ring." },
		/* 35 */ { 0x04AE20, 0x5A490, "waitCommunication+0x250",   Kind::Host,        false, "Same as 34 for the all-cabinets-present barrier (ctx+0xCC = 3)." },
		/* 36 */ { 0x0045B4, 0x5A4B0, "AdvtiseMode+0x54",          Kind::Host,        false, "TWO-BOARD attract sync: posts an event (ctx+0xB0 = 5) to the second local board — a no-op unless the two-board gate (DLL+0x741CB6) is up, which YAMP's one-board-per-machine link never raises." },
		/* 37 */ { 0x004B54, 0x5A4E0, "AdvtiseSynch+0x94",         Kind::Host,        false, "Second half of 36 (ctx+0xB4 = 5)." },
		/* 38 */ { 0x03CFD0, 0x59B70, "StartButtonCheck+0x40",     Kind::Inert,       false, "" },
		/* 39 */ { 0x023428, 0x59B70, "EntryWaitModeEnd+0x38",     Kind::Inert,       false, "" },
		/* 40 */ { 0x04C9DC, 0x59B70, "NetAllGameOverChk+0x7c",    Kind::Inert,       false, "Probe on the packet's game-over flag store (_SendPacket+0x17)." },
		/* 41 */ { 0x04BC80, 0x59B70, "NetWorkGameOut",            Kind::Inert,       false, "Probe on the linked-race teardown entry." },
		/* 42 */ { 0x21D624, 0x03960, "NetGameDataTransRun1+0x74", Kind::Removed,     true,  "NEVER INSTALLED (overlay target). Would delete the `addr` that doubles vel.x in the bike marshal's pos+2*vel extrapolation — on PS3 this halved the dead-reckon lead." },
		/* 43 */ { 0x21D634, 0x03960, "NetGameDataTransRun1+0x84", Kind::Removed,     true,  "NEVER INSTALLED (overlay target); vel.y twin of 42." },
		/* 44 */ { 0x21D644, 0x03960, "NetGameDataTransRun1+0x94", Kind::Removed,     true,  "NEVER INSTALLED (overlay target); vel.z twin of 42." },
		/* 45 */ { 0x216BE8, 0x5A510, "MapPut2+0x178",             Kind::Unclassified, false, "NEVER INSTALLED (overlay target)." },
		/* 46 */ { 0x00A750, 0x5A510, "ArtObjput",                 Kind::Unclassified, false, "" },
		/* 47 */ { 0x023D00, 0x5A510, "emCalcTexPoint",            Kind::Unclassified, false, "Same handler as 46/48 — an envmap/texture-coordinate helper HLE." },
		/* 48 */ { 0x08D884, 0x5A510, "spBikePut+0x114",           Kind::Unclassified, false, "Same handler as 46/47." },
		/* 49 */ { 0x00DDDC, 0x5A540, "otherSpeedSet+0x6c",        Kind::Unclassified, false, "" },
		/* 50 */ { 0x00DDE4, 0x5A5E0, "otherSpeedSet+0x74",        Kind::Unclassified, false, "" },
		/* 51 */ { 0x00DDF4, 0x59B70, "otherSpeedSet+0x84",        Kind::Inert,       false, "" },
		/* 52 */ { 0x0264F8, 0x59B70, "GameTimeLapInit+0xc8",      Kind::Inert,       false, "" },
		/* 53 */ { 0x0271EC, 0x59B70, "checkChkPointNormal+0x9c",  Kind::Inert,       false, "" },
		/* 54 */ { 0x219BA0, 0x59B70, "turboCounterAdd",           Kind::Inert,       false, "NEVER INSTALLED (overlay target)." },
		/* 55 */ { 0x211F40, 0x5A6A0, "gearchange_calc",           Kind::Unclassified, false, "NEVER INSTALLED (overlay target)." },
		/* 56 */ { 0x21274C, 0x5A700, "enginTorque+0x4bc",         Kind::Unclassified, false, "NEVER INSTALLED (overlay target)." },
		/* 57 */ { 0x08B1E8, 0x59B70, "changeAttIDRun+0x68",       Kind::Inert,       false, "" },
		/* 58 */ { 0x08B210, 0x59B70, "changeAttIDRun+0x90",       Kind::Inert,       false, "" },
		/* 59 */ { 0x026F20, 0x5A750, "goalInGameOver",            Kind::Unclassified, false, "" },
		/* 60 */ { 0x026FF0, 0x5A820, "goalInChampionShip",        Kind::Unclassified, false, "" },
		/* 61 */ { 0x01B21C, 0x5A8F0, "CreditDisplay+0x3c",        Kind::Unclassified, false, "" },
		/* 62 */ { 0x01B0C8, 0x5A910, "InsertDisplay+0x18",        Kind::Unclassified, false, "" },
		/* 63 */ { 0x03CD5C, 0x5A930, "CheckMainMode+0x2c",        Kind::Host,        false, "When DLL flag +0x27B969 is clear, ORs 0x14 into execute_info.status — mode reporting back to the host — then runs the original." },
	};
