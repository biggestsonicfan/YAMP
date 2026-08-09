// Split out of HleHooks.cpp (2026-08-09), which #includes this inside its anonymous
// namespace - `Info` and `Kind` are the using-aliases declared there, like MrHooks.inc.

	// ---- Virtua Fighter 2 (Yakuza: Like a Dragon) ----------------------------------------
	//
	// All 67 hooks (installer FUN_180047A70, whose trap loop is factored out into
	// FUN_180003CD0; table DLL+0x185640). Sites are symbolised with VF2's own 301-entry ROM
	// symbol table (DLL+0x15DD40).
	//
	// VF2 is the ANCESTOR of the other two - Fighting Vipers and Sonic the Fighters are both
	// built on this engine - which is why records 0-7 line up item for item with theirs (board
	// init, composite enable, frame yield, the interrupt handshake, the vsync trio) despite
	// the games having nothing else in common.
	//
	// Kinds are confirmed by decompilation for the tails, the inert stub, every multi-site
	// handler and hooks 0-13 / 31 / 48-52. The remaining single-site handlers have their
	// replaces-flag derived from the tail they end in - which is mechanical and reliable - but
	// are classified Content provisionally rather than from reading each one; their notes say so.
	constexpr Info VF2_HOOKS[] =
	{
		{ 0x011BD4, 0x4E960, "init_fix+0x8C", Kind::Core,     true,   "Forces g0 = 0 in the board hardware-init check." },
		{ 0x009FAC, 0x4E9B0, "siw_loop+0x8", Kind::Core,     true,   "REQUIRED FOR ANY PICTURE. Sets the composite-enable flag (DLL+0x51F0CF) and clears the 0x2000-byte host tile/sprite buffer at [DLL+0x880038]." },
		{ 0x009FB0, 0x4EA20, "main_loop", Kind::Core,     true,   "Per-frame yield, plus the master-state-dependent tile-buffer mode words at [DLL+0x880038]+0x98898." },
		{ 0x000F7C, 0x4EAD0, "interrupt_wait", Kind::Core,     true,   "Interrupt handshake: raises the pending-interrupt bit (ctx+0x188) when ctx+0x18C bit 0 is armed." },
		{ 0x000F84, 0x4EB30, "interrupt_wait+0x8", Kind::Core,     true,   "Second half of the interrupt handshake: host yield (FUN_18004C690) plus ctx+0x1B0, then skips the original." },
		// The vsync wait. NOT a host clock, despite what these three said until 2026-08-02: the
		// value all three read, DLL+0xA80050, is the host storage of GUEST 0x500000 (board init
		// sets the guest-0x500000 bank pointer ctx+0x18 to it and the RAM base DLL+0x880030 to
		// 0x580050, and 0x580050 + 0x500000 = 0xA80050). It is the ROM's own vblank byte, which
		// the ROM's timer ISR increments at ROM 0xCE0-0xCEC. So the spin below exits on an
		// emulated interrupt, entirely inside the instruction stream - which is why the
		// divergence hunt has to be about how many INSTRUCTIONS a frame takes, not about a race
		// with a host thread. See docs/vf2-hle-hooks.md.
		{ 0x010F90, 0x4EB60, "interrupt_wait_b+0x88", Kind::Core,     true,   "Vsync wait: loads the ROM's vblank byte (guest 0x500000, host DLL+0xA80050) into g0 (ctx+0x98) and raises the pending-interrupt bit." },
		{ 0x010F98, 0x4EBE0, "interrupt_wait_b+0x90", Kind::Core,     true,   "Vsync wait: re-loads the same vblank byte into r3 (ctx+0x64). This is the top of the spin." },
		{ 0x010FA0, 0x4EC40, "interrupt_wait_b+0x98", Kind::Core,     true,   "Vsync wait loop: returns -8 to re-execute the load above until the vblank byte changes, i.e. until the ROM's timer interrupt bumps it." },
		{ 0x06E1C8, 0x4ECA0, "check_sram_all+0x47C", Kind::Host,     true,   "Injects the backup-RAM / DIP block from the module config (gate DLL+0x6263FB) into board SRAM at [DLL+0x880020]+0x91. This is VF2's GAME ASSIGNMENTS source." },
		{ 0x011378, 0x4EE20, "variable_diff_calc+0x80", Kind::Content,  true,   "Forces ctx+0x68 = 7 in the difficulty calculation, then runs the original." },
		{ 0x011348, 0x03DF0, "variable_diff_calc+0x50", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3DF0), so nothing runs in its place." },
		{ 0x064808, 0x4EE40, "osage_dsp+0x57C", Kind::Core,     true,   "Forces r3 = 1 to pass a self-test / checksum path. A modified ROM will not boot without these." },
		{ 0x002EAC, 0x03DF0, "chg_scr_color_req+0x360", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3DF0), so nothing runs in its place (site 2)." },
		{ 0x00A80C, 0x4EE40, "WARNING_INT+0x8", Kind::Core,     true,   "Self-test / checksum bypass: forces r3 = 1 (site 2)." },
		{ 0x024588, 0x4EE90, "Calc_pos+0x20", Kind::Content,  false,  "Runs the original afterwards. Not yet read in detail." },
		{ 0x0245AC, 0x4EEB0, "Calc_pos+0x44", Kind::Content,  false,  "Runs the original afterwards. Not yet read in detail." },
		{ 0x011138, 0x03DF0, "debug_sw_check+0x44", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3DF0), so nothing runs in its place (site 3)." },
		{ 0x011B44, 0x4EED0, "init_scroll+0xB8", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x0438EC, 0x4EEF0, "enemy_control+0x6434", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x00D030, 0x4EF20, "GAME_INT+0x1C", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x001358, 0x4EF40, "player_entry+0x14", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build." },
		{ 0x00B0D8, 0x4EF50, "ADV_SEGA_PIC_INT", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x00B57C, 0x4EF70, "ADV_MOVIE_DSP+0x8C", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x00AD2C, 0x4EF90, "ADV_DSP+0x34", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x0013E8, 0x4EFB0, "pushed_st1+0x64", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x001548, 0x4EFB0, "pushed_st2+0x70", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail (site 2)." },
		{ 0x001460, 0x4F000, "vs_mode1+0x28", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x0015C0, 0x4F000, "vs_mode2+0x28", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail (site 2)." },
		{ 0x00C474, 0x4F020, "SEL_INT", Kind::Content,  false,  "Runs the original afterwards. Not yet read in detail." },
		{ 0x00C7FC, 0x4EF40, "SEL_DSP+0x10", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 2)." },
		{ 0x00CB64, 0x4EF40, "SEL_DSP+0x378", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 3)." },
		{ 0x00CF48, 0x4F0E0, "SEL_DSP+0x75C", Kind::Content,  false,  "Stage select: when config+0xA (DLL+0x6263FA) is set, picks one of 11 stages with the SECOND host twister at [DLL+0x623788 + 0x08], then runs the original. NETPLAY-CRITICAL: seeding only `rand` leaves this free to disagree between peers." },
		{ 0x00CDD4, 0x4EF40, "SEL_DSP+0x5E8", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 4)." },
		{ 0x00DD48, 0x4EF40, "SET_INT+0x1B0", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 5)." },
		{ 0x00EB38, 0x4EF40, "JUDGE_DSP_INT+0x590", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 6)." },
		{ 0x00F61C, 0x4EF40, "VIC_INT", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 7)." },
		{ 0x00F894, 0x4EF40, "VIC_INT+0x278", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 8)." },
		{ 0x00F904, 0x03DF0, "VIC_INT+0x2E8", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3DF0), so nothing runs in its place (site 4)." },
		{ 0x00F910, 0x4F150, "VIC_INT+0x2F4", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x00F438, 0x4F1A0, "next_program+0x230", Kind::Content,  false,  "Runs the original afterwards. Not yet read in detail." },
		{ 0x00FF6C, 0x4EF40, "vs_conti+0x48", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 9)." },
		{ 0x00C734, 0x4F400, "SEL_INT+0x2C0", Kind::Content,  false,  "Runs the original afterwards. Not yet read in detail." },
		{ 0x00C898, 0x4F500, "SEL_DSP+0xAC", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x00C8C8, 0x4F570, "SEL_DSP+0xDC", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x00CA0C, 0x4F5F0, "SEL_DSP+0x220", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x00C8E0, 0x03DF0, "SEL_DSP+0xF4", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3DF0), so nothing runs in its place (site 5)." },
		{ 0x00CA24, 0x03DF0, "SEL_DSP+0x238", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3DF0), so nothing runs in its place (site 6)." },
		{ 0x00CB18, 0x4F670, "SEL_DSP+0x32C", Kind::Content,  true,   "Replaces the original instruction. Not yet read in detail." },
		{ 0x04CAE4, 0x4F740, "send_beta_data+0x108", Kind::Core,     true,   "TEXTURE-UPLOAD BUDGET, AND THE ONLY HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us > 7499), a flat ~7.5 ms with no master-state variant. Times FUN_180061530() against DLL+0x625D60. This is the divergence SetTextureBudgetDeterministic pins." },
		{ 0x04CC44, 0x4F740, "send_lod_data+0xE0", Kind::Core,     true,   "Texture-upload budget check (site 2)." },
		{ 0x04CEDC, 0x4F740, "send_lod_data_q_sub_norm+0x54", Kind::Core,     true,   "Texture-upload budget check (site 3)." },
		{ 0x04D018, 0x4F740, "send_lod_data_q_sub_anim+0x54", Kind::Core,     true,   "Texture-upload budget check (site 4)." },
		{ 0x0094D0, 0x4F7D0, "rand", Kind::Host,     true,   "WHOLE-FUNCTION HLE of the ROM's rand: draws a 16-bit value from the host Mersenne Twister at [DLL+0x623788 + 0x20] (generator FUN_180008DA0), writes it into g0 and performs the i960 ret itself. NETPLAY-CRITICAL: primary RNG stream." },
		{ 0x00F744, 0x4F800, "VIC_INT+0x128", Kind::Content,  false,  "Runs the original afterwards. Not yet read in detail." },
		{ 0x00E9C0, 0x4EF40, "JUDGE_DSP_INT+0x418", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 10)." },
		{ 0x010BCC, 0x4EF40, "RANK_INT+0xC", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 11)." },
		{ 0x029674, 0x4EF40, "chk_ai_switch+0x2C", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 12)." },
		{ 0x00CDD0, 0x4F850, "SEL_DSP+0x5E4", Kind::Content,  false,  "Runs the original afterwards. Not yet read in detail." },
		{ 0x00D94C, 0x4EF40, "ROUND_INT", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 13)." },
		{ 0x00DB98, 0x4EF40, "SET_INT", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 14)." },
		{ 0x054C4C, 0x4EF40, "name_entry+0xF94", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 15)." },
		{ 0x054CC0, 0x4EF40, "name_entry+0x1008", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 16)." },
		{ 0x054D00, 0x4EF40, "name_entry+0x1048", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 17)." },
		{ 0x04BD58, 0x03DF0, "unp_send_tex_para_sub+0x84", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3DF0), so nothing runs in its place (site 7)." },
		{ 0x010DB0, 0x03DF0, "event_loop+0x5C", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3DF0), so nothing runs in its place (site 8)." },
		{ 0x010E60, 0x03DF0, "event_loop+0x10C", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3DF0), so nothing runs in its place (site 9)." },
		{ 0x0313EC, 0x4EF40, "chk_input+0x14", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe whose body was compiled out of the retail build (site 18)." },	};
