#include "HleHooks.h"

#include "../../YAMPGeneral.h"
#include "../../DebugLog.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cstdio>

namespace pre3
{
	namespace HleHooks
	{
		// ---- The module's own globals ----------------------------------------------------
		//
		// RVAs rather than byte patterns, on the same justification the pxd system-shader loader
		// in Pre3Host.cpp uses: GameVerify has already pinned this exact build by SHA-256 before
		// any of this runs, and these are DATA - there is no code to pattern-match. Every one was
		// read out of the functions named beside it.
		namespace rva
		{
			// Written by CPpc603e::reset (DLL 0x180030840) with the CPU object, so it is null
			// until the board has been built.
			inline constexpr uintptr_t CPU_PTR = 0x62AA90;
			// The 19 per-game hook tables (DLL 0x1800307C5 indexes this).
			inline constexpr uintptr_t HOOK_TABLES = 0x1084A0;
			// The interpreter's register file, as every HLE handler addresses it (the module's
			// own DAT_18052AA88). Layout, read off those handlers: +0x08 current PC, +0x0C next
			// PC, +0x10 + 4n GPRn (so r3 = +0x1C), +0x98 + 8n FPRn, +0x1A0 the link register.
			inline constexpr uintptr_t REG_PTR = 0x52AA88;
			// The CM3Mem object (the module's DAT_18052AA80).
			inline constexpr uintptr_t MEM_PTR = 0x52AA80;
			// THE guest RAM base - the module's DAT_18062AA98, which is what its own HLE handlers
			// use for direct guest memory: the SRC2 settings injector writes bytes at
			// `(guestAddr ^ 3) + DAT_18062AA98` and dwords at `DAT_18062AA98 + 0xC4550`.
			//
			// NOT CM3Mem+0x18, which is a different base and reads as zeroes at addresses that
			// provably hold code. That mistake produced a whole run of confident, wrong readings
			// before a peek at a known-fixed address (0x0189EC, which must be 0x4806302D) caught
			// it - which is why the validation is now written down here rather than assumed.
			inline constexpr uintptr_t RAM_PTR = 0x62AA98;
		}

		// Offsets inside the CPpc603e object, all from CPpc603e::init (DLL 0x1800306B0) and the
		// pre-decoder (DLL 0x180015E50).
		namespace cpu
		{
			inline constexpr size_t REGION_SIZE = 0x2F0;  // bytes of guest code the trace covers
			inline constexpr size_t TRACE_ARRAY = 0x2F8;  // {u32 word, u32 handlerIndex}[]
			inline constexpr size_t HOOK_TABLE = 0x300;   // the table init selected
		}

		// One decoded instruction. The interpreter reads these 8 bytes and calls
		// dispatch[handler_index]; `word` is the instruction the pre-decoder saw, kept so a
		// handler can still get at it - and so a hook can be undone.
		struct TraceEntry
		{
			uint32_t word;
			uint32_t handler_index;
		};

		// One record of a per-game hook table.
		struct Record
		{
			uint32_t guest_address;
			uint32_t pad;
			void* handler;
		};

		// Primary opcode 1 - unassigned on PowerPC, which is why the module can use its 2048
		// dispatch slots for hooks. Hook i lives at index HOOK_INDEX_BASE + i.
		inline constexpr uint32_t HOOK_INDEX_BASE = 0x800;

		// What the pre-decoder computes for an ordinary instruction: primary opcode into bits
		// 11..16, extended opcode in bits 0..10.
		static uint32_t NaturalIndex(uint32_t word)
		{
			return ((word >> 15) & 0x1F800u) | (word & 0x7FFu);
		}

#include "Hooks/Fv2Hooks.inc"

#include "Hooks/Src2Hooks.inc"

		// The SRC2 hooks the board cannot run without, from the one-at-a-time sweep
		// (400 frames each against a 676-draw baseline, the failures re-confirmed at 2000):
		//
		//   1  deletes `bl 0x07BA18`  - never reaches frame 1. Diagnosed: re-enabling this call
		//                               alone re-enters boot-phase frame-sync code (guest
		//                               0x001E78) after its device-address table at 0x0BB918 has
		//                               been reused, so it kicks a bogus address and spins on a
		//                               status bit that can never clear.
		//   2  deletes `bla 0x55D800` - freezes on its 8th draw, calling into empty memory.
		//   3  deletes the third call into the same overlay - and unlike 1 and 2 the board BOOTS
		//      with it disabled, which is how it briefly landed in the default disable mask and
		//      broke the TEST switch (2026-08-08, bisected): attract and races run untouched, but
		//      TEST-menu entry reaches the restored `bla 0x55D800`, lands in the overlay hooks 1
		//      and 2 never let load, and the guest crash-reboots into the MODEL3 SYSTEM PROGRAM
		//      screen. Continuing from there hangs in the 0x001E78 status spin above - the same
		//      reused-table failure as hook 1, arrived at through the menu instead of the boot.
		//      "Boot-critical" here means the CRASH-REBOOTED boot it causes, not the first one.
		//   5  arcade settings         - never reaches frame 1. SRC2 will not start without its
		//                               settings written into game RAM.
		//
		// FV2 has NO entry here on purpose: the sweep has not been run on it, and guessing from
		// SRC2's indices would be the same class of error as handing FV2's default mask to SRC2.
		static constexpr size_t SRC2_BOOT_CRITICAL[] = { 1, 2, 3, 5 };

		static const Info* CurrentTable(size_t& count)
		{
			switch (gGeneral.GetGameId())
			{
			case YAMPGeneral::GameId::FV2:
				count = sizeof(FV2_HOOKS) / sizeof(FV2_HOOKS[0]);
				return FV2_HOOKS;
			case YAMPGeneral::GameId::SRC2:
				count = sizeof(SRC2_HOOKS) / sizeof(SRC2_HOOKS[0]);
				return SRC2_HOOKS;
			default:
				break;
			}
			count = 0;
			return nullptr;
		}

		static bool s_live = false;
		static size_t s_liveRecords = 0;
		// Set once, when the live table has been checked against the descriptions above.
		static bool s_verified = false;
		static bool s_mismatch = false;

		static const uint8_t* ModuleBase()
		{
			return reinterpret_cast<const uint8_t*>(GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
		}

		size_t Count()
		{
			size_t count = 0;
			CurrentTable(count);
			return count;
		}

		bool Supported() { return Count() != 0; }
		bool Live() { return s_live; }
		size_t LiveRecordCount() { return s_liveRecords; }

		const Info& Get(size_t index)
		{
			size_t count = 0;
			const Info* table = CurrentTable(count);
			if (table == nullptr)
			{
				static constexpr Info EMPTY{ 0, 0, Kind::Core, "" };
				return EMPTY;
			}
			return table[index < count ? index : 0];
		}

		const char* KindName(Kind kind)
		{
			switch (kind)
			{
			case Kind::Core:    return "Core";
			case Kind::Host:    return "Host";
			case Kind::Speed:   return "Speed";
			case Kind::Patch:   return "Patch";
			case Kind::Removed: return "Removed";
			}
			return "?";
		}

		const char* KindDescription(Kind kind)
		{
			switch (kind)
			{
			case Kind::Core:
				return "Emulator plumbing: the handler spots the board spinning and gives the rest of\n"
					"the CPU's timeslice back. Disabling one is safe but costs frame rate - the board\n"
					"spins out the whole slice instead.";
			case Kind::Host:
				return "Host integration: arcade settings from YAMP's own config written into the game's\n"
					"RAM, a table clear, and the routines that reach the host through the board object.\n"
					"Disabling one boots, but loses whatever it provided.";
			case Kind::Speed:
				return "A whole ROM routine reimplemented in native x64 - the handler returns to the link\n"
					"register, so the ROM's own body never runs. Disabling one hands the work back to\n"
					"the interpreted PowerPC code: slower, and otherwise identical. This is the way to\n"
					"test whether one of them is wrong.";
			case Kind::Patch:
				return "The handler alters the instruction word before executing it, or pokes a register\n"
					"and then lets the original run. Small deliberate changes to the ROM's behaviour.";
			case Kind::Removed:
				return "The ROM's code is deleted here - either the one instruction, or the whole routine\n"
					"when the handler returns to the link register straight away.";
			}
			return "";
		}

		bool BootCritical(size_t index)
		{
			if (gGeneral.GetGameId() != YAMPGeneral::GameId::SRC2) return false;
			for (const size_t critical : SRC2_BOOT_CRITICAL)
			{
				if (critical == index) return true;
			}
			return false;
		}

		bool MaskTest(const uint64_t mask[2], size_t index)
		{
			// MAX_COUNT, not 128: drift from the m2ftg copy this API came from (where MAX_COUNT
			// happens to be 128). The hardcoded bound let MaskSet write bits 64-127 that no
			// Count()-driven reader ever looks at.
			return index < MAX_COUNT && (mask[index >> 6] & (1ull << (index & 63))) != 0;
		}

		void MaskSet(uint64_t mask[2], size_t index, bool disabled)
		{
			if (index >= MAX_COUNT) return;
			const uint64_t bit = 1ull << (index & 63);
			if (disabled) mask[index >> 6] |= bit;
			else          mask[index >> 6] &= ~bit;
		}

		void MaskForKinds(uint64_t mask[2], unsigned kinds)
		{
			mask[0] = mask[1] = 0;
			const size_t count = Count();
			for (size_t i = 0; i < count; i++)
			{
				if ((kinds & KindBit(Get(i).kind)) != 0)
				{
					MaskSet(mask, i, true);
				}
			}
		}

		bool MaskStripKinds(uint64_t mask[2], unsigned kinds)
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

		// ---- guest-code dump -------------------------------------------------------------
		//
		// The decoded trace is the ONLY practical way to read this board's PowerPC. The hook
		// addresses are RAM addresses the board has copied its program into, and eprom.bin in
		// image/src2.par is the raw EPROM set - a different interleave with no simple mapping to
		// them, so disassembling the archive does not answer "what instruction sits at 0x018B28".
		// The trace does: the pre-decoder writes {originalWord, handlerIndex} for EVERY word of
		// the region, and the original word is kept precisely so a hook can be undone. Reading it
		// back costs nothing and needs no debugger.
		//
		// Env-gated rather than a command-line flag, matching YAMP_PRE3_DIAG:
		//   YAMP_PRE3_DUMP=18B28,7480:40   - comma-separated <hexAddr>[:<wordCount>], default 64.
		//
		// SELF-TIMING, and it has to be. On the first frame the table verifies, every word in the
		// trace is still ZERO: the board has not yet copied its program into RAM, so the
		// pre-decoder has not run over this region. A dump there prints nothing but zeroes and
		// handler indices that Update itself has just written. So it retries each frame and only
		// fires once the FIRST requested address has a non-zero word, i.e. once that region has
		// actually been decoded.
		//
		// (The same emptiness is why Update reconciles every frame rather than once - see the
		// note at its loop. Writing a natural index derived from a zero word is harmless only
		// because the pre-decoder overwrites the whole entry when it finally runs.)
		static void DumpGuestCode(const TraceEntry* trace, uint32_t entries)
		{
			static bool done = false;
			if (done) return;

			char spec[256];
			size_t specLen = 0;
			if (getenv_s(&specLen, spec, sizeof(spec), "YAMP_PRE3_DUMP") != 0 || specLen == 0)
			{
				done = true;
				return;
			}

			{
				const unsigned long first = strtoul(spec, nullptr, 16);
				const uint32_t index = static_cast<uint32_t>(first) / 4;
				if (index >= entries || trace[index].word == 0) return;   // not decoded yet
			}
			done = true;

			for (const char* p = spec; *p != '\0'; )
			{
				char* end = nullptr;
				const unsigned long addr = strtoul(p, &end, 16);
				if (end == p) break;
				unsigned long words = 64;
				if (*end == ':')
				{
					words = strtoul(end + 1, &end, 10);
				}
				DebugLogFile("[%s dump] guest 0x%06lX, %lu words\n",
					gGeneral.GetGameTag(), addr, words);
				for (unsigned long i = 0; i < words; i++)
				{
					const uint32_t at = static_cast<uint32_t>(addr) + i * 4;
					const uint32_t index = at / 4;
					if (index >= entries) break;
					DebugLogFile("[%s dump] %06X  %08X  idx=%04X\n", gGeneral.GetGameTag(),
						at, trace[index].word, trace[index].handler_index);
				}
				p = (*end == ',') ? end + 1 : end;
				while (*p == ',') p++;
			}
		}

		// ---- whole-region image dump -----------------------------------------------------
		//
		// Reconstructs the board's ENTIRE code region as a flat big-endian image, straight out of
		// the decoded trace, so it can be loaded into a real disassembler:
		//
		//   YAMP_PRE3_DUMPBIN=src2_guest.bin
		//
		// Import into Ghidra as a RAW BINARY, language PowerPC:BE:32:default, base address 0 -
		// the trace is indexed by guestAddress/4, so byte i of this file IS guest address i, and
		// no rebasing is needed. Auto-analysis then gives functions, xrefs and a decompiler over
		// the guest, which is a different class of tool from disassembling 200-word windows.
		//
		// Two honest caveats. The trace is a snapshot of DECODE time, not live RAM: regions the
		// board writes later (the security overlay's output buffer at 0x55D800 is the one that
		// matters here) read as zero. And a hooked address holds the ORIGINAL word - which is the
		// point, since that is the instruction the module replaced.
		static void DumpGuestImage(const TraceEntry* trace, uint32_t entries)
		{
			static bool done = false;
			if (done) return;

			char path[MAX_PATH];
			size_t pathLen = 0;
			if (getenv_s(&pathLen, path, sizeof(path), "YAMP_PRE3_DUMPBIN") != 0 || pathLen == 0)
			{
				done = true;
				return;
			}
			// Same self-timing rule as DumpGuestCode, for the same reason - on the first verified
			// frame the whole trace is still zero. Anchor on a hook site, which is by definition
			// a real instruction in this game's code.
			size_t count = 0;
			const Info* table = CurrentTable(count);
			if (table == nullptr || count == 0) { done = true; return; }
			const uint32_t anchor = table[0].guestAddress / 4;
			if (anchor >= entries || trace[anchor].word == 0) return;
			done = true;

			FILE* f = nullptr;
			if (fopen_s(&f, path, "wb") != 0 || f == nullptr)
			{
				DebugLogFile("[%s dump] could not open %s\n", gGeneral.GetGameTag(), path);
				return;
			}
			for (uint32_t i = 0; i < entries; i++)
			{
				const uint32_t w = trace[i].word;
				const unsigned char be[4] = {
					static_cast<unsigned char>(w >> 24), static_cast<unsigned char>(w >> 16),
					static_cast<unsigned char>(w >> 8),  static_cast<unsigned char>(w)
				};
				fwrite(be, 1, 4, f);
			}
			fclose(f);
			DebugLogFile("[%s dump] wrote %s - %u bytes of guest code, base 0, PowerPC BE 32\n",
				gGeneral.GetGameTag(), path, entries * 4);
		}

		// ---- guest PC probe --------------------------------------------------------------
		//
		// "The board does not advance" and "the board is spinning at address X" look identical
		// from the host: the frame loop runs at full speed either way, because the emulator has
		// its own thread and never stops handing back frames. The only way to tell them apart is
		// to look at the guest's own program counter.
		//
		//   YAMP_PRE3_PC=1     - log PC and LR every frame
		//   YAMP_PRE3_PC=30    - ...every 30th frame
		//
		// A guest stuck in a tight loop prints the same one or two addresses forever, and that
		// address is directly disassemblable with YAMP_PRE3_DUMP.
		static void SampleGuestPc(const uint8_t* base)
		{
			char spec[32];
			size_t specLen = 0;
			if (getenv_s(&specLen, spec, sizeof(spec), "YAMP_PRE3_PC") != 0 || specLen == 0)
			{
				return;
			}
			const unsigned long every = (std::max)(1UL, strtoul(spec, nullptr, 10));

			const auto* regs = *reinterpret_cast<const uint8_t* const*>(base + rva::REG_PTR);
			if (regs == nullptr) return;

			static unsigned long frame = 0;
			if (frame++ % every != 0) return;

			// LIVE guest RAM, on the same cadence:
			//   YAMP_PRE3_PEEK=BB910:8   - <hexGuestAddr>[:<wordCount>]
			//
			// Distinct from YAMP_PRE3_DUMP and needed alongside it: the trace holds what the
			// PRE-DECODER saw, so it answers "what instruction is at X" but not "what does the
			// board's data at X say NOW". Anything the board writes after decode - tables it
			// fills in, buffers it populates - is invisible there and visible here.
			//
			// Guest word at A is the host dword at RAM_PTR + A: the emulator keeps RAM
			// word-swizzled, which is why BYTE access in the module's handlers goes through
			// `addr ^ 3` while dword access does not.
			{
				char peek[128];
				size_t peekLen = 0;
				if (getenv_s(&peekLen, peek, sizeof(peek), "YAMP_PRE3_PEEK") == 0 && peekLen != 0)
				{
					const auto* ram = *reinterpret_cast<const uint8_t* const*>(base + rva::RAM_PTR);
					if (ram != nullptr)
					{
						char* end = nullptr;
						const unsigned long at = strtoul(peek, &end, 16);
						const unsigned long words = (*end == ':') ? strtoul(end + 1, nullptr, 10) : 4;
						for (unsigned long i = 0; i < words; i++)
						{
							DebugLogFile("[%s peek] %06lX = %08X\n", gGeneral.GetGameTag(),
								at + i * 4,
								*reinterpret_cast<const uint32_t*>(ram + at + i * 4));
						}
					}
				}
			}

			// r3..r6 rather than r3 alone: the interesting spins on this board poll a device
			// address held in a register, and the address is the whole question.
			DebugLogFile("[%s pc] frame=%lu pc=%06X next=%06X lr=%06X r3=%08X r4=%08X r5=%08X r6=%08X\n",
				gGeneral.GetGameTag(), frame - 1,
				*reinterpret_cast<const uint32_t*>(regs + 0x08),
				*reinterpret_cast<const uint32_t*>(regs + 0x0C),
				*reinterpret_cast<const uint32_t*>(regs + 0x1A0),
				*reinterpret_cast<const uint32_t*>(regs + 0x1C),
				*reinterpret_cast<const uint32_t*>(regs + 0x20),
				*reinterpret_cast<const uint32_t*>(regs + 0x24),
				*reinterpret_cast<const uint32_t*>(regs + 0x28));
		}

		bool MaskStripBootCritical(uint64_t mask[2])
		{
			// Deliberate escape hatch for diagnosis: YAMP_PRE3_ALLOW_CRITICAL=1 lets a
			// boot-critical hook actually stay disabled, ini and all.
			//
			// It exists because the strip is otherwise INVISIBLE and silently invalidates any
			// experiment on these three hooks - a settings.ini asking for hook 1 off simply comes
			// back with it on, and a run that looks like a clean bisect is really the default
			// mask. That cost a whole round of measurements here before it was spotted. Nothing
			// in the UI sets this; it is for the person holding a debugger.
			static const bool allow = []
			{
				char v[8];
				size_t n = 0;
				return getenv_s(&n, v, sizeof(v), "YAMP_PRE3_ALLOW_CRITICAL") == 0 && n != 0
					&& v[0] != '0';
			}();
			if (allow) return false;

			bool changed = false;
			const size_t count = Count();
			for (size_t i = 0; i < count; i++)
			{
				if (BootCritical(i) && MaskTest(mask, i))
				{
					MaskSet(mask, i, false);
					changed = true;
				}
			}
			return changed;
		}

		const uint64_t* DefaultDisableMask()
		{
			// PER-GAME, and it has to be: the indices below are per-title. Handing FV2's mask to
			// SRC2 would silently patch the wrong instructions in a different game.
			//
			// THIS IS ALSO THE NETPLAY ORACLE. A session forces every peer onto this value and
			// ignores settings.ini entirely (see YAMPUserInterface's Update call), because the
			// mask changes what the board DOES - Patch hooks rewrite instructions and Removed
			// hooks delete them - so two machines running different masks are running different
			// games. Anything added here therefore has to be right for a match, not just pleasant
			// for a single player.
			static constexpr uint64_t NONE[2] = { 0, 0 };

			if (gGeneral.GetGameId() == YAMPGeneral::GameId::SRC2)
			{
				// SEGA RACING CLASSIC 2, established 2026-08-08 by bisecting the live table - which
				// only became testable once the game was drivable at all (its ADC ring had never
				// been filled; see pre3::SetDrivingAxes). Three groups, all user-confirmed on
				// screen:
				//
				//   6       THE START-UP WARNING SCREEN. The hook zeroes the COUNTRY byte at the
				//           boot chain's `cmpwi r3, 1` (guest 0x1922C), and COUNTRY == JAPAN is
				//           what makes the ROM draw the screen. MEASURED by cold-boot bisection:
				//           this bit alone brings the screen back; the 16-21 group without it
				//           changes nothing visible. Same argument as the rest of this mask -
				//           YAMP is the cabinet, so the board's own power-up sequence plays.
				//   13, 14  the two `rom+0x374` / `+0x378` multiplies. Hook 13 zeroes the board's
				//           MOTION-INTEGRATION SCALAR (guest 0x1051E4), which is what froze every
				//           CPU car a few seconds into a race; 14 does the same to a bonus-time
				//           accumulator. Redundant with the 1.0f the host now writes into those
				//           fields (pre3_execute_info_t::src2_scalars) - the two are behaviourally
				//           identical, and both are kept because the scalar is the right VALUE
				//           whatever the mask says, and the mask is the thing a match agrees on.
				//   16-21   pieces of the composite boot-presentation handler (the 0x91598 screen:
				//           message mutes at 0x91660 / 0x6A4B4, the 3D walker in FUN_000917C0).
				//           The original claim that disabling this group "brought back the warning
				//           screen and the title logo" is RETRACTED - that measurement ran under a
				//           wider everything-off mask and the effect belonged to hook 6. Disabling
				//           16-21 alone shows no visible boot difference; they stay in the mask as
				//           original-ROM behaviour, not as a measured screen switch.
				//
				// NOT hooks 1, 2, 3 or 5 - SRC2_BOOT_CRITICAL, the hooks the board cannot run
				// without. Hook 3 was in this mask for one day on the reasoning "hooks 1 and 2
				// excise the overlay's loader, so the third call should go with them" - which is
				// backwards: BECAUSE the loader is excised, the call into the overlay must stay
				// excised too. Disabled, the board still boots and races (which is why the sweep
				// missed it), but the TEST switch crash-reboots into the MODEL3 SYSTEM PROGRAM
				// instead of opening the service menu. Bisected and measured on screen; the full
				// story is in SRC2_BOOT_CRITICAL's comment and
				// docs/src2-service-menu-regression.md.
				static constexpr uint64_t SRC2_DEFAULT[2] = {
					(1ull << 6) | (1ull << 13) | (1ull << 14)
					| (1ull << 16) | (1ull << 17) | (1ull << 18)
					| (1ull << 19) | (1ull << 20) | (1ull << 21),
					0
				};
				static_assert(SRC2_DEFAULT[0] == 0x3F6040);
				return SRC2_DEFAULT;
			}

			if (gGeneral.GetGameId() != YAMPGeneral::GameId::FV2)
			{
				return NONE;
			}

			// THE START-UP WARNING SCREEN PLAYS BY DEFAULT, which means hooks 7 and 10 ship
			// disabled. Between them they skip the screen every Sega arcade board shows on
			// power-up. Like a Dragon Gaiden wants it gone - there the emulator is a minigame
			// inside a menu, and nobody wants a hardware warning in front of it. YAMP is the
			// cabinet, so the board's own power-up sequence is the authentic behaviour.
			//
			// A CORRECTION WORTH KEEPING, because the measurement that produced the wrong answer
			// looked sound. This was first derived from the board's own game-mode byte over a
			// 900-frame headless run: mode 01 (start-up) holds for one sample with every hook on,
			// three with hook 6 off and four with 9+10 off, which reads exactly like a screen
			// being shown for longer - so 6, 9 and 10 were named as the trio. They are not; the
			// pair is 7 and 10, established by WATCHING THE SCREEN. Two lessons. The mode byte
			// does not distinguish "the screen is up" from "the board is between states", so it
			// cannot answer a question about what is displayed. And a 900-frame window is shorter
			// than the thing being measured - the screen has a real dwell time, so "it has not
			// advanced yet" and "it will never advance" look identical inside it.
			//
			// NB an existing settings.ini carries an explicit mask, and an explicit value beats a
			// default - so a profile written before this keeps the screen skipped until "Restore
			// defaults" is pressed.
			static constexpr uint64_t DEFAULT[2] = { (1ull << 7) | (1ull << 10), 0 };
			return DEFAULT;
		}

		// Checks the module's live table against the descriptions once. If the addresses and
		// handlers do not line up exactly, the descriptions belong to some other build and acting
		// on them would disable the wrong instructions - so Update stops instead.
		static bool Verify(const uint8_t* base, const Record* records, size_t liveCount)
		{
			size_t count = 0;
			const Info* table = CurrentTable(count);
			if (table == nullptr) return false;
			if (liveCount != count) return false;
			for (size_t i = 0; i < count; i++)
			{
				if (records[i].guest_address != table[i].guestAddress) return false;
				if (records[i].handler != base + table[i].handlerRva) return false;
			}
			return true;
		}

		void Update(const uint64_t mask[2])
		{
			s_live = false;

			size_t count = 0;
			const Info* table = CurrentTable(count);
			if (table == nullptr) return;

			const uint8_t* const base = ModuleBase();
			if (base == nullptr) return;

			auto* const cpuObject = *reinterpret_cast<uint8_t* const*>(base + rva::CPU_PTR);
			if (cpuObject == nullptr) return;   // board not built yet

			const auto* records = *reinterpret_cast<const Record* const*>(cpuObject + cpu::HOOK_TABLE);
			auto* const trace = *reinterpret_cast<TraceEntry* const*>(cpuObject + cpu::TRACE_ARRAY);
			const uint32_t regionSize = *reinterpret_cast<const uint32_t*>(cpuObject + cpu::REGION_SIZE);
			if (records == nullptr || trace == nullptr || regionSize == 0) return;

			// The table is null-handler terminated, exactly as CPpc603e::init walks it.
			size_t liveCount = 0;
			while (liveCount < MAX_COUNT && records[liveCount].handler != nullptr)
			{
				liveCount++;
			}
			s_liveRecords = liveCount;

			if (!s_verified)
			{
				s_verified = true;
				s_mismatch = !Verify(base, records, liveCount);
				if (s_mismatch)
				{
					DebugLogFile("[%s hle] live table (%zu records) does not match the %zu described "
						"hooks - leaving them alone\n", gGeneral.GetGameTag(), liveCount, count);
				}
				else
				{
					DebugLogFile("[%s hle] %zu hooks verified against the module's own table\n",
						gGeneral.GetGameTag(), count);
				}
			}
			if (s_mismatch) return;

			// Reconciled EVERY frame rather than once, and the reason is measurable: the board
			// re-runs the pre-decoder after it has copied its code into RAM, which puts every
			// hook back. A one-shot apply is silently undone a few frames into the boot.
			const uint32_t entries = regionSize / 4;
			for (size_t i = 0; i < count; i++)
			{
				const uint32_t word_index = table[i].guestAddress / 4;
				if (word_index >= entries) continue;

				TraceEntry& entry = trace[word_index];
				const uint32_t wanted = MaskTest(mask, i)
					? NaturalIndex(entry.word)
					: static_cast<uint32_t>(HOOK_INDEX_BASE + i);
				if (entry.handler_index != wanted)
				{
					entry.handler_index = wanted;
				}
			}

			DumpGuestCode(trace, entries);
			DumpGuestImage(trace, entries);
			SampleGuestPc(base);
			s_live = true;
		}
	}
}
