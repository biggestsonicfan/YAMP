#include "HleHooks.h"

#include "ModuleBuild.h"
#include "../YAMPGeneral.h"
#include "../YAMPSettings.h"
#include "../DebugLog.h"
#include "ELF/ElfRom.h"
#include "../net/NetPlugin.h"

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

	// ---- Sonic the Fighters --------------------------------------------------------------
	//
	// All 76 hooks, in the DLL's own table order. Sites are symbolised with the module's
	// 800-entry ROM symbol table (DLL+0x1742D0), which is AM2's own naming.
	constexpr Info STF_HOOKS[] =
	{
		{ 0x0122F4, 0x526A0, "init_fix+0x74",                     Kind::Core,    true,  "Forces g0 = 0 in the board hardware-init check." },
		{ 0x00735C, 0x52700, "main+0xa34",                        Kind::Core,    false, "REQUIRED FOR ANY PICTURE. Sets the module's composite-enable flag (DLL+0x6B9172), which gates the whole pass that draws into the presented 1024x768 texture - with this hook off, that pass binds and clears the display target every frame but never draws into it, so the screen stays black even though 2D and 3D render correctly into their own targets. Also clears the host tile/sprite buffer, then runs the original. Its ROM site is the `cmpobe 0,r11,main_loop` that enters the main loop only after init passes, so it fires exactly once, on a clean boot." },
		{ 0x0073A8, 0x52730, "main_loop",                         Kind::Core,    true,  "Per-frame yield: hands the module thread back to the host every main-loop iteration." },
		{ 0x001768, 0x527C0, "geo_func+0x218",                    Kind::Core,    true,  "Geometry-processor handshake; raises the render-ready flag instead of polling hardware." },
		{ 0x001770, 0x52830, "geo_func+0x220",                    Kind::Core,    true,  "Second half of the geometry handshake; flushes the display list to the host renderer." },
		{ 0x011608, 0x52860, "interrupt_wait_b+0x88",             Kind::Core,    true,  "Vsync wait: supplies the host frame counter in g0 and raises the pending-interrupt bit." },
		{ 0x011610, 0x528F0, "interrupt_wait_b+0x90",             Kind::Core,    true,  "Vsync wait: supplies the host frame counter in r3." },
		{ 0x011618, 0x52960, "interrupt_wait_b+0x98",             Kind::Core,    true,  "Vsync wait loop: returns -8 to re-execute until the host frame counter advances." },
		{ 0x003B44, 0x529D0, "set_window_data+0x564",             Kind::Host,    true,  "Injects the backup-RAM / DIP block (difficulty, country, free play, VS mode) from the module config." },
		{ 0x007C88, 0x52B70, "test_sw_chk+0xe4",                  Kind::Core,    true,  "Forces r3 = 1 to pass the board self-test. A modified ROM will not boot without this." },
		{ 0x00351C, 0x03A70, "chg_pol_color_send+0xc8",           Kind::Removed, true,  "Instruction deleted - polygon-colour write to hardware that does not exist here." },
		{ 0x068D28, 0x52BE0, "calc_kaze+0x284",                   Kind::Content, true,  "Returns 0x28, skipping ten instructions of the wind/cloth term." },
		{ 0x0001CC, 0x52B70, "start_ip+0x11c",                    Kind::Core,    true,  "Forces r3 = 1 in the boot ROM checksum path." },
		{ 0x00725C, 0x52B70, "main+0x934",                        Kind::Core,    true,  "Forces r3 = 1 in main's power-on test path." },
		{ 0x03F45C, 0x52BF0, "sound_queue_output+0x10",           Kind::Host,    true,  "Routes the sound queue to the host mixer instead of the i960 sound board." },
		{ 0x03F268, 0x52C70, "sound_request_special",             Kind::Host,    true,  "Special (voice / announcer) sound request -> host mixer." },
		{ 0x00B0F8, 0x52CA0, "GAME_INT+0x4",                      Kind::Host,    false, "DISABLED BY DEFAULT - re-enable it and matches last about two seconds. Copies backup RAM +0x3351 into `time` (RAM 0x500090) every game interrupt, then runs the original. The two are different units: `time` is the round length in SECONDS, while +0x3351 is time_var_array_num, an INDEX 0-9 into time_vars[] (ROM 0x8F3C4 = 10,20,30...99). The ROM converts between them (`time = time_vars[+0x3351]`), and the service menu edits that byte as an index. The module only gets away with the raw copy because its own injector wrote seconds into that byte as well - which is the bug that hung GAME ASSIGNMENTS, and which YAMP's \"Correct the module's backup-RAM TIME setting\" option fixes. With that option on (the default) this hook forces `time` to 2. Turn BOTH off together for the module's stock behaviour; leaving the fix on and this hook on is the one combination that gives two-second rounds. Disabling costs nothing else: the store is the handler's only side effect." },
		{ 0x0096AC, 0x52CD0, "ADV_REPLAY_WAIT1A+0x128",           Kind::Host,    true,  "DISABLED BY DEFAULT - re-enable it and the ATTRACT DEMO FIGHT lasts about two seconds. Sets the demo's round timer: game_timer = min(backup RAM +0x3351, 30) * 64 (RAM 0x500028, a 16-bit 1/64-second counter). It replaces the ROM's own instruction at 0x96AC, `shlo 7, 0xF, r15; stis r15, game_timer` - a HARDCODED 1920 = 30 seconds - so the arcade board always runs a 30-second demo and this hook exists only to make the demo follow the operator's TIME setting instead. It reads that byte as RAW SECONDS, exactly like hook 16, and is wrong for the same reason: with the backup-RAM TIME fix applied the byte is the ROM's own index (2), and 2*64 gives a two-second demo with both fighters at full health. Disabled, the ROM writes its own 1920 and the demo is 30 seconds." },
		{ 0x0019BC, 0x52D70, "player_entry+0x14",                 Kind::Inert,   false, "Trap runs the original unchanged - a probe point whose body was compiled out of the retail build." },
		{ 0x0083F4, 0x52D80, "ADV_DSP+0x108",                     Kind::Host,    false, "Clears the emulated input word during the host's attract context, then runs the original." },
		{ 0x00A218, 0x52DA0, "SEL_INT",                           Kind::Host,    false, "Flushes queued host sound commands on entry to character select, then runs the original." },
		{ 0x00A9B8, 0x52D70, "pl1_skp+0x6c",                      Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00AF84, 0x52E50, "set_vs_cnt_and_stage_num_sel+0x58", Kind::Content, false, "In VS mode, picks the stage with the host RNG (0-8) instead of the ROM's own sequence." },
		{ 0x00DC3C, 0x52D70, "jdi_set_sound+0xb0",                Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00E6EC, 0x52D70, "VIC_INT",                           Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00E93C, 0x52D70, "VIC_DSP+0x3c",                      Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00E9DC, 0x52D70, "VIC_DSP+0xdc",                      Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00E584, 0x52EC0, "next_round+0x1a4",                  Kind::Content, true,  "In VS mode only, redirects the round transition and forces both hidden-character flags set." },
		{ 0x00F57C, 0x52D70, "vs_conti+0x58",                     Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x04BDA8, 0x52FD0, "unp_send_tex_req+0x1190",           Kind::Core,    true,  "Texture-upload budget: reports whether 9 ms have elapsed so the unpack loop yields." },
		{ 0x04BF20, 0x52FD0, "unp_send_tex_req+0x1308",           Kind::Core,    true,  "Texture-upload budget check (second site)." },
		{ 0x04C1B4, 0x52FD0, "unp_send_tex_req+0x159c",           Kind::Core,    true,  "Texture-upload budget check (third site)." },
		{ 0x04C2F0, 0x52FD0, "unp_send_tex_req+0x16d8",           Kind::Core,    true,  "Texture-upload budget check (fourth site)." },
		{ 0x0066B0, 0x53070, "rand",                              Kind::Host,    true,  "WHOLE-FUNCTION HLE, not an instruction replacement: the handler writes a 16-bit host random value into g0 (the i960 return register) and then performs the i960 `ret` itself - unwinding the register frame via PFP/FP - so it returns 0 as its IP delta. Its site must therefore be the FIRST instruction of a real, CALLED function (a lone `ret` body is enough); it must not be inlined, or there is no frame for the return to unwind. Disable to restore the ROM's own sequence. NB the arcade RNG folds the free-running board timers at 0x00F00000, which read near-static unless a period is programmed - so on a homebrew ROM this hook is usually an improvement, not just parity." },
		{ 0x034EB8, 0x53200, "sel_disp_init",                     Kind::Host,    false, "Raises a host render/event flag when the character-select display initialises." },
		{ 0x0366F0, 0x53220, "char_add2_pass_p1+0x810",           Kind::Content, true,  "P1 hidden characters: fakes the ROM's dormant bit-3 flag while Start is held on Honey / Metal Sonic / Eggman." },
		{ 0x03674C, 0x532B0, "char_add2_pass_p1+0x86c",           Kind::Content, true,  "P1 character-slot lookup: supplies a substitute table, so the ROM's own table at 0xDACAC is never read." },
		{ 0x03771C, 0x53360, "char_add2_pass_p2+0x810",           Kind::Content, true,  "P2 twin of the hidden-character flag hook." },
		{ 0x037778, 0x533F0, "char_add2_pass_p2+0x86c",           Kind::Content, true,  "P2 twin of the character-slot lookup hook." },
		{ 0x036974, 0x534A0, "char_add2_pass_p1+0xa94",           Kind::Content, false, "Forces the hidden-character branch taken for P1, then runs the original." },
		{ 0x0379A0, 0x53560, "char_add2_pass_p2+0xa94",           Kind::Content, false, "P2 twin." },
		{ 0x02EFEC, 0x53620, "calc_rob_angle_int+0xb4",           Kind::Content, true,  "Overrides the 16-entry joint-angle table at ROM 0xC49C4 with a host copy (only for that address range)." },
		{ 0x0215B8, 0x536A0, "snc_eye_thd_set+0x148",             Kind::Content, false, "Zeroes the eye-tracking target for Honey (character 0x0F / 0x29), then runs the original." },
		{ 0x0215F8, 0x536A0, "snc_eye_thd_set+0x188",             Kind::Content, false, "Second eye-tracking site." },
		{ 0x058054, 0x536E0, "md_deathegg_sekkin_init+0xb8",      Kind::Content, false, "Remaps sprite ids 0x0F/0x10 in the Death Egg approach cutscene." },
		{ 0x030608, 0x537B0, "get_frame_dat+0x140",               Kind::Content, false, "Honey head-tilt fix: zeroes the blend weight for elements 15-17 of the animation blend loop." },
		{ 0x02EDF4, 0x537E0, "scr_bg_int+0x28",                   Kind::Content, true,  "Remaps the stage / background id in g0 through a 16-entry host table." },
		{ 0x010240, 0x53860, "ENDSUB_WAIT_1+0x68",                Kind::Content, true,  "Remaps the id in r3 through the same 16-entry host table." },
		{ 0x07CDE8, 0x53860, "MES_CONTINUE_INT+0xa0",             Kind::Content, true,  "Same id remap, on the continue screen." },
		{ 0x01089C, 0x538E0, "NAME_INT+0x19c",                    Kind::Content, false, "Forces g0 = 0x7080 (a duration constant), then runs the original." },
		{ 0x0541F0, 0x538E0, "adv_movie_snc_init+0x14",           Kind::Content, false, "Same duration override, attract movie init." },
		{ 0x055E04, 0x538E0, "adv_movie_tornado+0x28",            Kind::Content, false, "Same duration override, tornado sequence." },
		{ 0x056FE8, 0x538E0, "am_upper7_black+0x7b0",             Kind::Content, false, "Same duration override, attract fade." },
		{ 0x00E830, 0x53900, "VIC_INT+0x144",                     Kind::Host,    false, "Reports the match result (round number and both characters) to the Lost Judgment host for minigame progress." },
		{ 0x00DB8C, 0x52D70, "jdi_set_sound",                     Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x02AAA0, 0x52D70, "coli_attack_chk+0x428",             Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x02AB84, 0x52D70, "coli_attack_chk+0x50c",             Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x01315C, 0x52D70, "exec_command+0x11c",                Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x02EF74, 0x52D70, "calc_rob_angle_int+0x3c",           Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00ADA8, 0x539F0, "pl1_skp+0x45c",                     Kind::Host,    false, "Raises a host event flag on entry to character select, then runs the original." },
		{ 0x00C254, 0x52D70, "BUNRI_DSP+0x38",                    Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00C340, 0x52D70, "BUNRI_DSP+0x124",                   Kind::Inert,   false, "Trap runs the original unchanged (stripped probe point)." },
		{ 0x080200, 0x53A10, "em_scroll4+0x8",                    Kind::Content, false, "Forces r3 = 1 in the ending credit scroll, then runs the original." },
		{ 0x01BFC8, 0x53A30, "pm_game_2P_ss_check_pass+0x8",      Kind::Host,    false, "Lets the 2P join check pass while fewer than 8 credits are banked." },
		{ 0x00A9E8, 0x53A60, "pl1_skp+0x9c",                      Kind::Content, false, "Keeps P1's stored character in sync when the opponent picked a hidden variant." },
		{ 0x00AB2C, 0x53AE0, "pl1_skp+0x1e0",                     Kind::Content, false, "P2 side of the same fix-up." },
		{ 0x04F41C, 0x53B60, "name_init+0x88",                    Kind::Content, true,  "Name-entry ranking lookup for hidden characters; reads emulated ROM 0xDC0E4 through the host memory map." },
		{ 0x04F424, 0x03A70, "name_init+0x90",                    Kind::Removed, true,  "Instruction deleted - paired with the preceding hook." },
		{ 0x07D6E4, 0x53C30, "rm_wipe_in+0x44",                   Kind::Content, true,  "Honey VS portrait: forces sprite id 0x96 for characters 0x0F / 0x29 (her ROM entries point at Eggman's)." },
		{ 0x07D6FC, 0x53CA0, "rm_wipe_in+0x5c",                   Kind::Content, true,  "Honey VS portrait: forces sprite id 0x98." },
		{ 0x07D750, 0x53D10, "rm_char_move+0x38",                 Kind::Content, true,  "Honey P1 character card: forces id 0x19A." },
		{ 0x07D824, 0x53D10, "rm_char_disp_int+0x68",             Kind::Content, true,  "Honey P1 character card (second site)." },
		{ 0x07D944, 0x53D10, "rm_char_move2+0x4c",                Kind::Content, true,  "Honey P1 character card (third site)." },
		{ 0x07D784, 0x53D80, "rm_char_move+0x6c",                 Kind::Content, true,  "Honey P2 character card: forces id 0x19C." },
		{ 0x07D874, 0x53D80, "rm_char_disp_int+0xb8",             Kind::Content, true,  "Honey P2 character card (second site)." },
		{ 0x07D978, 0x53D80, "rm_char_move2+0x80",                Kind::Content, true,  "Honey P2 character card (third site)." },
	};

	// ---- Fighting Vipers -----------------------------------------------------------------
	//
	// All 95 hooks, in the DLL's own table order (installer FUN_180049C50, table DLL+0x1E5840).
	// Sites are symbolised with FV's own 732-entry ROM symbol table (DLL+0x172390).
	//
	// FV shares StF's engine, so the first eight records line up almost item for item - board
	// init, composite enable, frame yield, the interrupt handshake and the vsync trio - and
	// then the two tables diverge completely. The differences that matter to netplay are called
	// out in the notes: the wall-clock texture budget (39-42), and the fact that FV draws from
	// TWO host RNG streams (43 and 30) where StF has one.
	constexpr Info FV_HOOKS[] =
	{
		{ 0x012398, 0x51240, "init_fix+0x74", Kind::Core,     true,   "Forces g0 = 0 in the board hardware-init check." },
		{ 0x007954, 0x512A0, "main+0x914", Kind::Core,     true,   "REQUIRED FOR ANY PICTURE. Sets the module's composite-enable flag (DLL+0x6BB772 - StF's 0x6B9172 plus the same +0x2600 delta as the boot-state global) and clears the 0x2000-byte host tile/sprite buffer at [DLL+0xAFA2D8]." },
		{ 0x007958, 0x51320, "main_loop", Kind::Core,     true,   "Per-frame yield: clears r3, raises ctx+0x1B0 and calls the host yield (FUN_18004E990) every main-loop iteration." },
		{ 0x002238, 0x513B0, "interrupt_wait", Kind::Core,     true,   "Interrupt handshake: raises the pending-interrupt bit (ctx+0x188 |= 1) when ctx+0x18C bit 0 is armed." },
		{ 0x002240, 0x51420, "interrupt_wait+0x8", Kind::Core,     true,   "Second half of the interrupt handshake: host yield + ctx+0x1B0, then skips the original (returns 8)." },
		{ 0x0118D4, 0x51450, "interrupt_wait_b+0x88", Kind::Core,     true,   "Vsync wait: supplies the host frame counter (DLL+0xAFA2E0) in g0 and raises the pending-interrupt bit." },
		{ 0x0118DC, 0x514E0, "interrupt_wait_b+0x90", Kind::Core,     true,   "Vsync wait: supplies the host frame counter in r3." },
		{ 0x0118E4, 0x51550, "interrupt_wait_b+0x98", Kind::Core,     true,   "Vsync wait loop: returns -8 to re-execute until the host frame counter advances (r3 != g0)." },
		{ 0x001A88, 0x03A80, "ucb_adr_init+0x4", Kind::Removed,  true,   "Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place." },
		{ 0x04AA74, 0x515C0, "send_tex_default+0x17C", Kind::Core,     true,   "Texture DMA completion: copies r4 into r3 and skips the original." },
		{ 0x04AABC, 0x515C0, "send_tex_default+0x1C4", Kind::Core,     true,   "Texture DMA completion (site 2)." },
		{ 0x04AB04, 0x515C0, "send_tex_default+0x20C", Kind::Core,     true,   "Texture DMA completion (site 3)." },
		{ 0x00708C, 0x515E0, "main+0x4C", Kind::Host,     true,   "Injects the whole 0x60-byte backup-RAM / DIP block from the module config (DLL+0x1EA590: +4, +5, +9, +0xA) into the board's backup RAM at [DLL+0x6CC188]+0x91+0x3320..0x3378, mirroring it into DLL+0xB96600. THE GAME ASSIGNMENTS / DIP INJECTOR - StF's equivalent is its hook 8 (set_window_data+0x564)." },
		{ 0x004128, 0x03A80, "chg_pol_color_send+0xC8", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x063DF8, 0x51760, "kill_osage_sub+0x30", Kind::Content,  true,   "Returns 0x28, skipping ten instructions of the osage (cloth/hair) term - FV's twin of StF's calc_kaze hook." },
		{ 0x007944, 0x51770, "main+0x904", Kind::Core,     true,   "Forces r3 = 1 to pass the board self-test / checksum path. A modified ROM will not boot without these." },
		{ 0x008234, 0x51770, "WARNING_INT+0x14", Kind::Core,     true,   "Self-test / checksum bypass: forces r3 = 1 (site 2)." },
		{ 0x0089F0, 0x51810, "ADV_FBI_PIC_INT+0x88", Kind::Content,  true,   "Forces r3 = 0 in the FBI-warning attract picture." },
		{ 0x041F04, 0x517E0, "sound_request_special", Kind::Host,     true,   "WHOLE-FUNCTION HLE: routes the special (voice / announcer) sound request in g0 to the host mixer, then performs the i960 ret itself (FUN_1800248D0) and returns 0." },
		{ 0x00B784, 0x51880, "GAME_INT+0x8", Kind::Inert,    false,  "Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build." },
		{ 0x0024D0, 0x51880, "player_entry+0x14", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x008864, 0x51890, "ADV_DSP+0x98", Kind::Host,     false,  "In the module's VS/host context (config+0xA), writes 0x30 into emulated RAM 0x500704, then runs the original." },
		{ 0x0025A8, 0x518B0, "pushed_st1_data_bd_ex+0x54", Kind::Host,     false,  "In the module's VS/host context (config+0xA), zeroes ctx+0x88 and skips the original; otherwise runs it unchanged." },
		{ 0x0025D8, 0x518B0, "random_check1", Kind::Host,     false,  "Host-context register guard (site 2)." },
		{ 0x002714, 0x518B0, "pushed_st2_data_bd_ex+0x54", Kind::Host,     false,  "Host-context register guard (site 3)." },
		{ 0x002750, 0x518B0, "random_check2", Kind::Host,     false,  "Host-context register guard (site 4)." },
		{ 0x00AA98, 0x51920, "SEL_INT", Kind::Host,     false,  "Raises host event flag 0x2000 and flushes the queued host sound commands on entry to character select, then runs the original." },
		{ 0x00AF2C, 0x51880, "SEL_DSP+0x10", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00B098, 0x51880, "sel_dsp_next+0x20", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00B124, 0x519E0, "sel_wait_chk+0x40", Kind::Host,     false,  "Raises host event flag 0x4000 (select wait), then runs the original." },
		{ 0x00B648, 0x51A00, "set_vs_cnt_and_stage_num_sel+0x3C", Kind::Content,  false,  "VS mode only: picks the stage with the HOST MT RNG (generator [DLL+0x68E188+0x08], value % 9 through a 9-byte table) instead of the ROM's own sequence, then runs the original. NETPLAY-CRITICAL: second RNG stream, must be seeded." },
		{ 0x00B4C4, 0x51A70, "_draw_lp_d+0x78", Kind::Content,  false,  "Rewrites the draw-loop word at [RAM 0x500814]: clears bits 1-2 and ORs 0x1FC000, then runs the original." },
		{ 0x00E150, 0x51880, "JUDGE_DSP_INT+0x5D0", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00EC88, 0x51880, "VIC_INT", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00F014, 0x51880, "VIC_INT+0x38C", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00F150, 0x51880, "VIC_INT+0x4C8", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00EAC0, 0x51B10, "vs_game_continue_check_ex+0x18", Kind::Host,     true,   "VS mode only: 2P continue/credit bypass - redirects the i960 IP to 0xF7BC and ORs bit 0/2 into the credit words at RAM 0x500248 / 0x50024C." },
		{ 0x00F814, 0x51880, "vs_conti+0x58", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x0349EC, 0x51880, "MES_ROUND_INT+0x4", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x04C3D8, 0x51C20, "send_beta_data+0x138", Kind::Core,     true,   "TEXTURE-UPLOAD BUDGET, AND THE ONE HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us >= budget), where the budget is 12 ms normally and 8 ms while master state (DLL+0xAFA30B) is 6 or 7. Times FUN_180064D90() against DLL+0x690130. A fast machine and a slow one do different amounts of work in the same emulated frame - the exact divergence StF's SetTextureBudgetDeterministic pins." },
		{ 0x04C550, 0x51C20, "send_lod_data+0xE0", Kind::Core,     true,   "Texture-upload budget check (site 2)." },
		{ 0x04C7E4, 0x51C20, "send_lod_data_q_sub_norm+0x54", Kind::Core,     true,   "Texture-upload budget check (site 3)." },
		{ 0x04C920, 0x51C20, "send_lod_data_q_sub_anim+0x54", Kind::Core,     true,   "Texture-upload budget check (site 4)." },
		{ 0x006DC8, 0x51CE0, "rand", Kind::Host,     true,   "WHOLE-FUNCTION HLE of the ROM's rand: draws a 16-bit value from the host Mersenne Twister (generator FUN_180008D00, state object [DLL+0x68E188+0x20]), writes it into g0 and performs the i960 ret itself. NETPLAY-CRITICAL: primary RNG stream." },
		{ 0x038B60, 0x51EA0, "select_init_wait+0x4C", Kind::Content,  true,   "Zeroes ctx+0x94 in the select-init wait." },
		{ 0x038B70, 0x51EA0, "select_init_wait+0x5C", Kind::Content,  true,   "Zeroes ctx+0x94 in the select-init wait (site 2)." },
		{ 0x038E54, 0x03A80, "old_set_skip+0x1C", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x038E58, 0x03A80, "old_set_skip+0x20", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x038E5C, 0x03A80, "old_set_skip+0x24", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x038E70, 0x03A80, "old_set_skip+0x38", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x039E04, 0x03A80, "old_set_skip_P2+0x1C", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x039E08, 0x03A80, "old_set_skip_P2+0x20", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x039E0C, 0x03A80, "old_set_skip_P2+0x24", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x039E20, 0x03A80, "old_set_skip_P2+0x38", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x038E74, 0x03A80, "old_set_skip+0x3C", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x039E24, 0x03A80, "old_set_skip_P2+0x3C", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x0531E4, 0x51D10, "adv_fade_in6+0x8", Kind::Host,     false,  "When r3 == 0x15, clears 0xC00 bytes of the host tile buffer at [DLL+0xAFA2D8]+0x9024 and raises its +0x98895 flag, then runs the original." },
		{ 0x07BAF8, 0x51D60, "fcc_loop+0xBC", Kind::Content,  false,  "Divide-by-zero guard: if either float register ctx+0x70 / +0x74 is zero, returns 0x30 (skips twelve instructions); otherwise runs the original." },
		{ 0x07B9B4, 0x51D90, "finish_coli_check+0xB8", Kind::Content,  false,  "Divide-by-zero guard: if either float register ctx+0x78 / +0x7C is zero, returns 0x38; otherwise runs the original." },
		{ 0x05046C, 0x51DC0, "name_init+0x68", Kind::Content,  false,  "Name-entry: when r3 == 8, redirects the i960 IP to ROM 0x504A4; otherwise runs the original." },
		{ 0x009CBC, 0x03A80, "ADV_REPLAY_WAIT1B+0x10", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x009CC8, 0x51880, "ADV_REPLAY_WAIT1B+0x1C", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x009CCC, 0x03A80, "ADV_REPLAY_WAIT1B+0x20", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x031BA8, 0x51F00, "kuc_arm+0x18", Kind::Host,     false,  "2P join check: lets the round start when either player's state byte is 9 and the 8-slot host credit array (DLL+0xC92BB8) has a free slot, by copying ctx+0x8C into ctx+0x90; then runs the original." },
		{ 0x00C4E0, 0x51FE0, "ROUND_INT", Kind::Host,     false,  "Banks the 2P-join flag (DLL+0x6C4058+2) once the credit word at RAM 0x5000A2 exceeds 9, then runs the original." },
		{ 0x00ED0C, 0x52020, "VIC_INT+0x84", Kind::Host,     false,  "Reports the match result to the Lost Judgment host for minigame progress: raises event flag 0x200 and writes round+1 and both players' character ids to DLL+0x1EB5A0 +0x1674/+0x1678/+0x167C, when master state (DLL+0xAFA30B) is 9. StF's twin writes the same three fields to DLL+0x1EE4A0+0x1674." },
		{ 0x00E014, 0x51880, "JUDGE_DSP_INT+0x494", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x0114BC, 0x51880, "RANK_INT+0xC", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x00B4C0, 0x51880, "_draw_lp_d+0x74", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x026B64, 0x51880, "damage_unit+0x120", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x026B70, 0x51880, "damage_unit+0x12C", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x026C44, 0x51880, "damage_unit+0x200", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x026C9C, 0x51880, "damage_unit+0x258", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x078888, 0x51880, "cage_break_init", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x027160, 0x51880, "zibaku_ckeck+0x64", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x031D58, 0x51880, "kuc_motion+0x74", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x0182E0, 0x51880, "calc_unit_mat+0x1180", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x005D6C, 0x03A80, "set_obj_fifo+0x34", Kind::Removed,  true,   "Instruction deleted - the handler is the bare skip tail, so nothing runs in its place." },
		{ 0x052C40, 0x51880, "adv_special_command+0x1C", Kind::Inert,    false,  "Trap runs the original unchanged (stripped probe point)." },
		{ 0x01EFD0, 0x52110, "camera_control+0x130", Kind::Content,  true,   "Camera clamp: ctx+0x94 = min(ctx+0x90 + K, limit), using two rdata float constants." },
		{ 0x01B2EC, 0x521A0, "option_control+0xAC", Kind::Content,  false,  "When config+0x8 is set, forces ctx+0x90 = 2." },
		{ 0x01CB44, 0x521A0, "copy_option_data+0x18", Kind::Content,  false,  "When config+0x8 is set, forces ctx+0x90 = 2 (site 2)." },
		{ 0x012678, 0x521A0, "efc_rob_set_set+0x1E0", Kind::Content,  false,  "When config+0x8 is set, forces ctx+0x90 = 2 (site 3)." },
		{ 0x06D128, 0x521C0, "kanban_dsp:+0x64", Kind::Content,  false,  "When config+0x8 is set, forces r3 = 2." },
		{ 0x06D338, 0x521E0, "trailer_dsp+0x3C", Kind::Content,  false,  "When config+0x8 is set, forces ctx+0x68 = 2." },
		{ 0x06D748, 0x521E0, "trailer_dsp_b0+0x64", Kind::Content,  false,  "When config+0x8 is set, forces ctx+0x68 = 2 (site 2)." },
		{ 0x02F3CC, 0x52200, "adv_subobj_set+0xF00", Kind::Content,  false,  "When config+0x8 is set, forces ctx+0x6C = 2." },
		{ 0x02FF3C, 0x52200, "adv_subobj_set+0x1A70", Kind::Content,  false,  "When config+0x8 is set, forces ctx+0x6C = 2 (site 2)." },
		{ 0x0300C4, 0x52200, "adv_subobj_set+0x1BF8", Kind::Content,  false,  "When config+0x8 is set, forces ctx+0x6C = 2 (site 3)." },
		{ 0x0337D4, 0x52220, "set_rank_total_win", Kind::Content,  false,  "Ranking arithmetic, stash half: saves ctx+0x70 into host scratch DLL+0xC92BC8." },
		{ 0x033814, 0x52260, "time_points_zero+0x4", Kind::Content,  false,  "Ranking arithmetic, stash half: saves ctx+0x78 into host scratch DLL+0xC92BC4." },
		{ 0x033824, 0x522A0, "time_points_zero+0x14", Kind::Content,  false,  "Ranking arithmetic, stash half: saves ctx+0x70 into host scratch DLL+0xC92BC0." },
		{ 0x020504, 0x52240, "cc_ranking+0x184", Kind::Content,  false,  "Ranking arithmetic, restore half: g0 = host scratch DLL+0xC92BC8." },
		{ 0x02054C, 0x52280, "cc_ranking+0x1CC", Kind::Content,  false,  "Ranking arithmetic, restore half: g0 = host scratch DLL+0xC92BC4 * 2." },
		{ 0x02059C, 0x522C0, "cc_ranking+0x21C", Kind::Content,  false,  "Ranking arithmetic, restore half: g0 = host scratch DLL+0xC92BC0." },	};

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

#include "VonHooks.inc"

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

	static_assert(std::size(VON_HOOKS) <= m2ftg::HleHooks::MAX_COUNT, "raise MAX_COUNT");
	static_assert(std::size(STF_HOOKS) <= m2ftg::HleHooks::MAX_COUNT, "raise MAX_COUNT");
	static_assert(std::size(FV_HOOKS) <= m2ftg::HleHooks::MAX_COUNT, "raise MAX_COUNT");
	static_assert(std::size(VF2_HOOKS) <= m2ftg::HleHooks::MAX_COUNT, "raise MAX_COUNT");

	const GameHooks* CurrentHooks()
	{
		switch (gGeneral.GetGameId())
		{
		case YAMPGeneral::GameId::StF: return &GAME_STF;
		case YAMPGeneral::GameId::FV:  return &GAME_FV;
		case YAMPGeneral::GameId::VF2: return &GAME_VF2;
		case YAMPGeneral::GameId::VON_K2: return &GAME_VON;
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
