#include "HleHooks.h"

#include "../../YAMPGeneral.h"
#include "../../YAMPSettings.h"
#include "../../DebugLog.h"
#include "../ELF/ElfRom.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
	using m2ftg::HleHooks::Info;
	using Kind = m2ftg::HleHooks::Kind;

	// RVAs inside stf-pxd-w64-d3d12_retail.dll (fixed preferred base 0x180000000).
	//
	// RVA_HLE_TABLE is the installer's input: 76 records of {uint32 romOffset, pad, uint64
	// handler}. It lives in .data, not .rdata, so it is writable without VirtualProtect - but
	// this code never needs to touch it, because it works on the ROM image instead.
	//
	// The installer (FUN_18004B070, board bring-up stage 2) does, for every record whose
	// romOffset is below 0x200000:
	//     savedWords[i] = *(uint64*)(romBase + romOffset);          // original instruction(s)
	//     *(uint32*)(romBase + romOffset) = 0x4000000 | (i * 4);    // trap word
	// The trap word's low bits are the record index, so the emulator's opcode dispatch can
	// find the handler and the saved original again. Restoring the ROM word from savedWords
	// therefore un-does a hook completely and reversibly, at any time.
	constexpr uintptr_t RVA_HLE_TABLE = 0x1E8870;
	constexpr uintptr_t RVA_SAVED_WORDS = 0x68E540;  // uint64[] - originals saved by the installer
	constexpr uintptr_t RVA_ROM_BASE = 0x9F7CD0;     // 1 MB i960 program ROM (slot 0)
	constexpr uintptr_t RVA_BOOT_STATE = 0x6B9300;   // ROM/CPU boot phase; 2 = board booted
	constexpr uint32_t ROM_SIZE = 0x100000;
	constexpr uint32_t TRAP_OPCODE = 0x4000000;

	struct HleTableEntry
	{
		uint32_t romOffset;
		uint32_t padding;
		uint64_t handler;
	};
	static_assert(sizeof(HleTableEntry) == 0x10, "HLE record is 16 bytes in the DLL");

	uint8_t* ModuleBase()
	{
		return reinterpret_cast<uint8_t*>(GetModuleHandleW(L"stf-pxd-w64-d3d12_retail.dll"));
	}

	// The two shared tails every handler ends in, and the whole reason the table splits so
	// cleanly into "the original still runs" and "the original is gone":
	//   0x39D0  executes the saved original instruction (recomputes the opcode-table index
	//           exactly like the CPU's fetch path and tail-jumps its handler)
	//   0x3A70  returns only the original's length (4 or 8) - the instruction is skipped
	// A handler that ends by returning a length inline is doing the same thing as 0x3A70.
	constexpr uintptr_t RVA_TAIL_EXEC_ORIGINAL = 0x39D0;
	constexpr uintptr_t RVA_TAIL_SKIP_ORIGINAL = 0x3A70;

	// All 76 hooks, in the DLL's own table order. Sites are symbolised with the module's
	// 800-entry ROM symbol table (DLL+0x1742D0), which is AM2's own naming.
	constexpr Info HOOKS[m2ftg::HleHooks::COUNT] =
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
		{ 0x00B0F8, 0x52CA0, "GAME_INT+0x4",                      Kind::Host,    false, "Writes a host-supplied byte into emulated RAM 0x500090 each game interrupt, then runs the original." },
		{ 0x0096AC, 0x52CD0, "ADV_REPLAY_WAIT1A+0x128",           Kind::Host,    true,  "Supplies attract-mode replay data into emulated RAM 0x500028 from a host table." },
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
}

const m2ftg::HleHooks::Info& m2ftg::HleHooks::Get(size_t index)
{
	return HOOKS[index < COUNT ? index : 0];
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
	}
	return "";
}

bool m2ftg::HleHooks::MaskTest(const uint64_t mask[2], size_t index)
{
	return index < COUNT && (mask[index >> 6] & (1ull << (index & 63))) != 0;
}

void m2ftg::HleHooks::MaskSet(uint64_t mask[2], size_t index, bool disabled)
{
	if (index >= COUNT)
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
	for (size_t i = 0; i < COUNT; i++)
	{
		if ((kinds & KindBit(HOOKS[i].kind)) != 0)
		{
			MaskSet(mask, i, true);
		}
	}
}

bool m2ftg::HleHooks::MaskStripKinds(uint64_t mask[2], unsigned kinds)
{
	bool changed = false;
	for (size_t i = 0; i < COUNT; i++)
	{
		if ((kinds & KindBit(HOOKS[i].kind)) != 0 && MaskTest(mask, i))
		{
			MaskSet(mask, i, false);
			changed = true;
		}
	}
	return changed;
}

void m2ftg::HleHooks::Update()
{
	if (gGeneral.GetGameId() != YAMPGeneral::GameId::StF)
	{
		return;
	}

	uint8_t* base = ModuleBase();
	if (base == nullptr)
	{
		return;
	}

	// Nothing to restore until the installer has run; it does so in board bring-up stage 2,
	// which is also when the boot state reaches 2.
	if (*reinterpret_cast<const uint32_t*>(base + RVA_BOOT_STATE) != 2)
	{
		return;
	}

	const YAMPSettings* settings = gGeneral.GetSettings();
	if (settings == nullptr)
	{
		return;
	}
	const uint64_t* wanted = settings->m_stfHleDisableMask;

	const auto* table = reinterpret_cast<const HleTableEntry*>(base + RVA_HLE_TABLE);
	const auto* savedWords = reinterpret_cast<const uint64_t*>(base + RVA_SAVED_WORDS);
	auto* rom = reinterpret_cast<uint8_t*>(base + RVA_ROM_BASE);

	// Enforced every frame rather than applied once. It is only 76 aligned dword compares,
	// and it means the setting can be toggled live, survives a board reset re-installing the
	// traps, and needs no bookkeeping about what was applied when.
	for (size_t i = 0; i < COUNT; i++)
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
		if (originalWord >= TRAP_OPCODE && originalWord < TRAP_OPCODE + COUNT * 4)
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

uint32_t m2ftg::HleHooks::detail::g_hitCounts[COUNT] = {};

// Set by ApplyRetarget when game.elf declared any convention symbol; the settings UI uses it to
// decide whether the homebrew health checks apply (they would false-alarm on stock StF).
static bool g_usedConvention = false;

uint32_t m2ftg::HleHooks::HitCount(size_t index)
{
	return index < COUNT ? detail::g_hitCounts[index] : 0;
}

void m2ftg::HleHooks::ResetHitCounts()
{
	for (size_t i = 0; i < COUNT; i++)
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

size_t m2ftg::HleHooks::ApplyRetarget(const std::string iniRetarget[COUNT])
{
	if (gGeneral.GetGameId() != YAMPGeneral::GameId::StF)
	{
		return 0;
	}

	uint8_t* base = ModuleBase();
	if (base == nullptr)
	{
		DebugLogFile("[HleHooks] retarget skipped: module not loaded\n");
		return 0;
	}

	// Fold the ROM's own declarations in underneath the ini, so an explicit line still wins.
	std::string retarget[COUNT];
	for (size_t i = 0; i < COUNT; i++)
	{
		retarget[i] = iniRetarget[i];
	}

	g_usedConvention = false;
	size_t declared = 0;
	for (size_t c = 0; c < CONVENTION_COUNT; c++)
	{
		const ConventionSite& site = CONVENTION[c];
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
		// pointed at Sonic the Fighters' own offsets, which in a different program ROM are
		// arbitrary instructions.
		size_t suppressed = 0;
		for (size_t i = 0; i < COUNT; i++)
		{
			if (retarget[i].empty()) { retarget[i] = "off"; suppressed++; }
		}
		DebugLogFile("[HleHooks] ROM declared %zu hook site(s); %zu unnamed hook(s) suppressed\n",
			declared, suppressed);
	}

	auto* table = reinterpret_cast<HleTableEntry*>(base + RVA_HLE_TABLE);
	size_t changed = 0;
	for (size_t i = 0; i < COUNT; i++)
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

		DebugLogFile("[HleHooks] hook %zu (%s): ROM 0x%06X -> %s\n", i, HOOKS[i].site, table[i].romOffset,
			want == RETARGET_SUPPRESS ? "not installed" : "retargeted");
		table[i].romOffset = want;
		changed++;
	}

	if (changed != 0)
	{
		DebugLogFile("[HleHooks] retargeted %zu of %zu hook records before module_start\n", changed, COUNT);
	}
	return changed;
}

bool m2ftg::HleHooks::GetInstalledOffsets(uint32_t out[COUNT])
{
	const uint8_t* base = ModuleBase();
	if (base == nullptr)
	{
		return false;
	}

	const auto* table = reinterpret_cast<const HleTableEntry*>(base + RVA_HLE_TABLE);
	for (size_t i = 0; i < COUNT; i++)
	{
		out[i] = table[i].romOffset;
	}
	return true;
}
