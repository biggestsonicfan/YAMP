#include "K2Host.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ImportSymbols.h"

#include "../../criware/Cri.h"
#include "../../pxd/LJ/sl.h"            // pxd::sl — the head of the context, and the primitives
#include "../../pxd/K2/sl.h"            // pxd::K2 — THIS generation's context layout (0xF3C0)
#include "../../pxd/LJ/sl_internal.h"   // handle_internal_buffer_t (the 8-byte queue node)
#include "../m2ftg.h"                 // m2ftg_config_t (0x100C) — unchanged in this generation
#include "../NetSession.h"            // the shared netplay session driver
#include "../../net/NetPlugin.h"      // net::Logf — the diagnostic sink both peers share
#include "../ModuleArgs.h"
#include "../DisplayModes.h"
#include "../../input/Input.h"
#include "../HostUI.h"
#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../Utils/MemoryMgr.h"       // VP::Patch — protection-aware writes into module .text
#include "../../Utils/ScopedUnprotect.hpp"
#include "../../wil/resource.h"

namespace m2ftg
{
	namespace K2
	{
		// The sl half of the pxd platform layer. This generation's sl context is the LJ-era layout
		// with 0x3C0 bytes inserted before handle_free_queue (0xF3C0 vs 0xF000), so the head fields
		// come from pxd::sl and everything past sz_fs_root from pxd::K2 — see pxd/K2/sl.h. The gs
		// half does NOT carry over (0x202140 vs 0x388A00): it stays an opaque block, with only the
		// individual fields the module reads filled in below.
		using namespace pxd;

		const GameDesc& CurrentGame()
		{
			// Kiwami 2's two modules are the same engine build (their TimeDateStamps are one second
			// apart) and speak the same protocol — same 0x16E0 execute_info, same m2ftg_config_t,
			// same gs layout — so they differ only by GameDesc.
			return gGeneral.GetGameId() == YAMPGeneral::GameId::VON_K2 ? GAME_OMG : GAME_VF2;
		}

		// The per-frame block. Size gate verified BOTH statically (`CMP RCX, 0x16E0` at the top of
		// module_main) and live under x64dbg (entered with RCX = 0x16E0).
		//
		// The 0x20-byte header is the shared pxd one, and the live capture read back exactly the
		// values the host had just written before calling:
		//   +0x00 size = 0x16E0   +0x08 p_device_context   +0x10 status (bit0 pause, bit5 coin)
		//   +0x14 result = 0x80004005 preset   +0x18 output_texid   +0x1C sound_volume = 1.0f
		//
		// THE VOLUME IS THE +0x1C FLOAT here — the m2ftg/Y6 mechanism, not Lost Judgment's +0x663
		// byte. Getting that wrong mutes every cue while the rest of the audio path works.
		//
		// The INPUT half, read out of this module's own pad reader FUN_18005E410 rather than
		// carried over from Lost Judgment (whose execute_info is 0x1760 with a 0x190 pad stride):
		//   pad[player] at **+0x20 with a 0x170 stride** — and 0x170 is exactly sizeof(csl_pad),
		//   i.e. this generation embeds the plain engine pad, not LJ's extended variant. Every
		//   field the reader touches lines up: +0x00 m_now (`*(uint*)(exec + p*0x170 + 0x20)`),
		//   +0x10..+0x1C the four analog floats (read at 0x30/0x34/0x38/0x3C), and
		//   m_buttons[4]/[5] the analog triggers (read at 0xC4/0xC5).
		//
		//   assign[player][8] at **+0x15E0** — FUN_180004A80 indexes it as
		//   `exec + 0x15E0 + player*8` and looks each byte up in the module's own combo table.
		struct alignas(16) execute_info_t
		{
			size_t size_of_struct;
			void* p_device_context;
			int status;
			int result;
			unsigned int output_texid;
			float sound_volume;
			pxd::csl_pad pad[2];                       // +0x20, stride 0x170
			std::byte unmapped[0x15E0 - 0x300];
			uint8_t assign[2][8];                      // +0x15E0
			std::byte unmapped_tail[0x16E0 - 0x15F0];
		};
		static_assert(sizeof(execute_info_t) == 0x16E0);
		static_assert(offsetof(execute_info_t, sound_volume) == 0x1C);
		static_assert(offsetof(execute_info_t, pad) == 0x20);
		static_assert(offsetof(execute_info_t, pad[1]) == 0x190);
		static_assert(offsetof(execute_info_t, assign) == 0x15E0);

		// ============================== host gs pieces =============================
		//
		// The gs context is the DLL's own embedded 0x202140 template. Everything below was
		// CAPTURED from a live Yakuza Kiwami 2 (host gs context found by searching memory for
		// 'LBgs' + version + size 0x202140), and it differs from the YLAD generation in three
		// ways that would each have cost a debugging session to guess:
		//
		//   gs+0x20 slot[0] = ID3D11Device                     (vtable confirmed in d3d11.dll)
		//   gs+0x20 slot[1] = slot[2] = gs+0xC0                <- an EMBEDDED sub-object, and the
		//                                                         SAME pointer twice; YLAD used two
		//                                                         separate external blocks
		//   gs+0x20 slot[3] = 1                                 (a value, not a pointer)
		//   gs+0x20 slot[4] = pointer to a dword that read 0
		//   gs+0x20 slot[5] = 0                                 <- NO allocator; YLAD put one here
		//   gs+0xB0        = the cgs_device_context             <- the SAME pointer as
		//                                                         execute_info+0x08. In YLAD this
		//                                                         slot is the allocator vtable.
		//   gs+0xC0 sub-object: +0x00 IDXGISwapChain (dxgi.dll vtable),
		//                       +0x08/+0x10/+0x18 three d3d11.dll objects, then packed dwords.
		static uint8_t* g_gsContext = nullptr;
		static void* g_slContext = nullptr;
		static int s_zeroDword = 0;

		// The module's two-board gate, or null in a module that has no second board - see the note
		// on ImportSymbol::TWO_BOARD_GATE. Written once, after module_start.
		static uint8_t* g_twoBoardGate = nullptr;

		// "-von-2board" on YAMP's command line. EXPERIMENT, off by default: nothing about normal
		// Virtual On play changes unless it is passed. Virtual On is a linked-cabinet game and the
		// module carries the whole second-board implementation with the switch never thrown; this
		// throws it, so the first question - does board 1 step at all? - can be answered by running
		// it rather than by reading more disassembly, which on this DLL has a poor track record.
		static bool WantTwoBoardMode()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-2board") != nullptr;
			return wanted;
		}

		// "-von-2probe" logs the same state WITHOUT throwing the switch, so the two-board run has
		// a control to be compared against. Measuring only the experimental arm is how you talk
		// yourself into believing a number that was always going to be there.
		static bool WantTwoBoardProbe()
		{
			static const bool wanted = WantTwoBoardMode()
				|| wcsstr(GetCommandLineW(), L"-von-2probe") != nullptr;
			return wanted;
		}

		// ---- two-board diagnostics -----------------------------------------------------------
		//
		// Only reachable behind -von-2board, and only meaningful for `omg`. These are RVAs into
		// omg-pxd-w64-gog_retail.dll, taken from the reversing in docs/von-netplay-recon.md; they
		// are safe to hardcode ONLY because GameVerify pins that module by SHA-256 before it is
		// ever loaded, so a different build cannot reach this code. They exist to answer one
		// question - does board 1's ROM actually run? - and should go away once it is answered.
		//
		// THE TEST: the frame step runs the link transfer BEFORE the two-board gate, so both
		// boards' CommData fill in either way and prove nothing. CommSend is different: a board's
		// SEND buffer is written by that board's own ROM. So a non-zero P2.CommSend is the one
		// signal that means board 1 executed.
		namespace OmgRva
		{
			constexpr size_t COMM_P1        = 0x7C2730;   // comm block 0 base
			constexpr size_t COMM_P1_SEND   = 0x7C4730;   // P1 + 0x2000
			constexpr size_t COMM_P1_FLAG   = 0x7CA731;   // P1 + 0x8001, bit0 = bank select
			constexpr size_t COMM_P2_SEND   = 0x7CC738;   // P2 + 0x2000, P2 = P1 + 0x8008
			constexpr size_t COMM_P2_FLAG   = 0x7D2739;
			constexpr size_t BOARD_INDEX    = 0x6911B4;   // written by the bank switch
			constexpr size_t COMM_PAYLOAD   = 0x700;      // bytes the link moves per board per frame

			// Per-board work RAM. The bank switch at 0x180069D30 does NOT store region bases - it
			// stores BIAS pointers, `region - i960_base`, so the emulator can index them with a
			// raw i960 address. Its two arms give, for the i960 0x500000 window where all of
			// Virtual On's game globals live (`LEA RAX,[...]` then `SUB RAX,0x500000`):
			//     board 0:  0x181337020 - 0x500000   ->  i960 0x500000 sits at RVA 0x1337020
			//     board 1:  0x180E37010 - 0x500000   ->  i960 0x500000 sits at RVA 0x0E37010
			// (The other pair, 0x180C37010 / 0x180F37010 less 0x200000, is the 0x200000 window.)
			// The module's own copy of m2ftg_config_t, the 0x100C bytes module_start memcpy'd from
			// params+0x38 into &DAT_1807A7FB0. +0x00 is `kind`, which must read 3 for omg - that
			// is the check that makes a wrong base fail loudly instead of corrupting .data.
			constexpr size_t CONFIG_BASE    = 0x7A7FB0;

			constexpr size_t BOARD0_R5      = 0x1337020;
			constexpr size_t BOARD1_R5      = 0x0E37010;
			constexpr uint32_t I960_R5_BASE = 0x500000;

			// The per-board blocks the bank switch (0x180069D30) re-points, base + board*stride.
			//
			// BACKUP/IO is the one that answers input routing: `backup_write` writes through this,
			// backup RAM starts at +0x91 (0x4000 bytes) and the I/O ports sit just past it - which
			// is where `module_main` puts the coin byte, at +0x4098. If player N's pad reaches
			// board N, the two boards' I/O bytes must differ when only pad[1] is held.
			constexpr size_t BACKUP_IO_BASE = 0x180C2EEC0;
			constexpr size_t BACKUP_IO_STEP = 0x409C;
			constexpr size_t IO_PORTS       = 0x4091;   // just past the 0x4000 of backup RAM
			constexpr size_t IO_PORTS_LEN   = 0xB;

			// The ~2 MB per-board region - the presentation candidate (video/framebuffer sized).
			constexpr size_t VIDEO_BASE     = 0x1807DADC0;
			constexpr size_t VIDEO_STEP     = 0x20A080;
			constexpr size_t VIDEO_SAMPLE   = 0x1000;

			// Named ROM globals, straight out of the module's own symbol table (RVA 0x4507E0) and
			// identical in the PS3 build. THIS is the real test, and unlike CommSend it needs no
			// match to be running: if the gate makes the frame step call board 1's step functions,
			// board 1 boots its own ROM from frame 0 and these fill in. All still zero after a
			// thousand frames means board 1 never executed.
			constexpr uint32_t SYM_BOARDTYPE = 0x5023E0;
			constexpr uint32_t SYM_SYNCHFLAG = 0x5024E0;   // the ROM's link handshake (`synch`)
			constexpr uint32_t SYM_SYNCHTIME = 0x5024E4;
			constexpr uint32_t SYM_GLOBCNTR  = 0x5024E8;   // the ROM's own shared frame counter
			constexpr uint32_t SYM_MAINMODE  = 0x5039F4;
			constexpr uint32_t SYM_VERSUSMODE= 0x503A7C;
			constexpr uint32_t SYM_NET_FLAG  = 0x5770B0;
			constexpr uint32_t SYM_LINK_ID   = 0x5770B1;
		}

		// Native address of an i960 global in a given board's work RAM.
		static uint8_t* I960At(uint8_t* base, int board, uint32_t i960Addr)
		{
			const size_t bias = (board != 0) ? OmgRva::BOARD1_R5 : OmgRva::BOARD0_R5;
			return base + bias + (i960Addr - OmgRva::I960_R5_BASE);
		}

		// "-von-padtest": hold a distinctive button pattern on pad[1] ONLY, so the two boards can be
		// told apart by their inputs. Without it both pads are idle under -frames and the boards are
		// expected to look identical - which would prove nothing either way.
		static bool WantPadTest()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-padtest") != nullptr;
			return wanted;
		}

		// "-von-render1": force drawing board 1. OFFLINE DEBUG ONLY - online, which cabinet you see
		// follows your netplay role, never a switch. See SetRenderBoard.
		static bool WantRenderBoard1()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-render1") != nullptr;
			return wanted;
		}

		static uint8_t* g_renderBoardSelect = nullptr;

		// Choose which cabinet the render draws, by patching the two bytes that decide what the
		// frame step leaves selected: `XOR ECX,ECX` (board 0) <-> `MOV CL,1` (board 1).
		//
		// Driven per frame from the netplay role rather than from a command line, because a
		// launch-time choice is a footgun: both players could pick the same cabinet, or mismatch,
		// and the symptom would be a desync that looks like a netcode bug. Host owns pad 0 and the
		// MASTER board, guest owns pad 1 and the SLAVE - localPad already carries exactly that, so
		// nothing needs asking at launch and the two peers cannot disagree.
		// NOTE the VP::Patch. This writes into the module's .text, and the ScopedUnprotect that made
		// .text writable lives in ResolveSymbolsAndPatch and is long out of scope by the time the
		// game loop runs - so a bare store here faults. It faulted in the worst possible way, too:
		// the guards below mean nothing is written while the bytes already read correctly, so the
		// only frame that actually stored was the one where localPad first became 1 - i.e. YAMP
		// died precisely on "Start Match", and only for the guest. VP::Patch wraps the write in
		// VirtualProtect and restores the old protection.
		static void SetRenderBoard(int board)
		{
			if (g_renderBoardSelect == nullptr) return;
			const uint8_t* p = g_renderBoardSelect;
			const bool wantOne = board != 0;
			// Verify before writing - never patch blind, and never write if it already reads right.
			if (wantOne && p[0] == 0x33 && p[1] == 0xC9)
			{
				Memory::VP::Patch(g_renderBoardSelect, { 0xB1, 0x01 });   // MOV CL,1  -> render board 1
			}
			else if (!wantOne && p[0] == 0xB1 && p[1] == 0x01)
			{
				Memory::VP::Patch(g_renderBoardSelect, { 0x33, 0xC9 });   // XOR ECX,ECX -> render board 0
			}
		}

		// "-von-findctr": hunt for the ROM's per-frame counter.
		//
		// NetSession needs a value that advances exactly once per emulated frame: it is both the
		// round-start anchor and the desync canary, and getting it wrong is precisely what put
		// VF2's two peers one frame apart. Rather than guess from the symbol table (GlobCntr was
		// the obvious candidate and measured only ~0.37 ticks per module_main, so it is not one),
		// sweep the whole work-RAM window and let the data name it.
		//
		// For every dword in the i960 0x500000 window, classify each module_main call as
		// delta == +1, delta == 0, or anything else. A true frame counter is "+1 or 0, never
		// anything else": the zero frames are calls where the board genuinely did not advance,
		// which VF2 proved really happen (~5% there) and which EndFrame already handles.
		static bool WantFindCounter()
		{
			static const bool wanted = wcsstr(GetCommandLineW(), L"-von-findctr") != nullptr;
			return wanted;
		}

		// The module's board-bank switch, or null in a module with only one board.
		using board_select_t = void(*)(int);
		static board_select_t g_boardSelect = nullptr;

		// The module's per-board init — what boot calls for board 0 and board 1. RESET does not.
		static board_select_t g_perBoardInit = nullptr;

		static uint8_t* g_dllBase = nullptr;

		// Non-zero byte count, so "did anything ever land here" is one number.
		static uint32_t NonZeroCount(const uint8_t* p, size_t n)
		{
			uint32_t hits = 0;
			for (size_t i = 0; i < n; ++i) hits += (p[i] != 0);
			return hits;
		}

		// FNV-1a over a sample, so "are these two regions the same" is one comparison.
		static uint32_t Hash32(const uint8_t* p, size_t n)
		{
			uint32_t h = 2166136261u;
			for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
			return h;
		}

		// The counter sweep. One pass per module_main call, over the i960 0x500000 window.
		namespace FindCtr
		{
			// The named globals run to at least 0x577590 (`r_num`), so sweep the whole 512 KB
			// rather than the first 32 KB - the first pass found nothing but flags.
			constexpr uint32_t BASE  = 0x500000;
			constexpr uint32_t WORDS = 0x80000 / 4;

			struct Slot { uint32_t prev; uint32_t inc; uint32_t same; uint32_t other; };
			static std::vector<Slot> s_slots;
			static bool s_primed = false;

			static void Sample(uint8_t* dllBase, int board)
			{
				const uint8_t* ram = I960At(dllBase, board, BASE);
				if (s_slots.empty()) s_slots.resize(WORDS);
				for (size_t i = 0; i < WORDS; ++i)
				{
					uint32_t v;
					memcpy(&v, ram + i * 4, sizeof(v));
					if (s_primed)
					{
						const uint32_t d = v - s_slots[i].prev;
						if (d == 1)      ++s_slots[i].inc;
						else if (d == 0) ++s_slots[i].same;
						else             ++s_slots[i].other;
					}
					s_slots[i].prev = v;
				}
				s_primed = true;
			}

			// ---- boot-state hunt ---------------------------------------------------------
			//
			// `DwGame.rvaBootState` is a MODULE global (not emulated RAM) that reads 2 once the
			// board is up; BoardBooted() gates every debug-menu action and ResetBoard on it.
			// Rather than guess, snapshot the region where the module's other known globals live
			// - the gate 0x6910DE, board index 0x6911B4, RNG holder 0x690810 and run-state
			// 0x7D2B10 are all in here - and report what climbed to exactly 2 and stayed.
			constexpr size_t GLOB_LO = 0x680000;
			constexpr size_t GLOB_HI = 0x7E0000;
			static std::vector<uint32_t> s_globFirst;

			static void SnapGlobals(uint8_t* dllBase)
			{
				const size_t n = (GLOB_HI - GLOB_LO) / 4;
				s_globFirst.resize(n);
				memcpy(s_globFirst.data(), dllBase + GLOB_LO, n * 4);
			}

			static void ReportGlobals(uint8_t* dllBase)
			{
				size_t shown = 0;
				DebugLogFile("[findboot] dwords in %06zX..%06zX that are 2 now and were <2 at frame 1\n",
					GLOB_LO, GLOB_HI);
				for (size_t i = 0; i < s_globFirst.size(); ++i)
				{
					uint32_t now;
					memcpy(&now, dllBase + GLOB_LO + i * 4, sizeof(now));
					if (now != 2 || s_globFirst[i] >= 2) continue;
					DebugLogFile("[findboot]   RVA %06zX  %u -> 2\n",
						GLOB_LO + i * 4, s_globFirst[i]);
					if (++shown >= 40) { DebugLogFile("[findboot]   ...truncated\n"); break; }
				}
				DebugLogFile("[findboot] %zu candidate(s)\n", shown);
			}

			// Report the dwords that only ever held still or stepped by one, best first.
			static void Report(int board)
			{
				std::vector<size_t> hits;
				for (size_t i = 0; i < s_slots.size(); ++i)
				{
					if (s_slots[i].other == 0 && s_slots[i].inc > 0) hits.push_back(i);
				}
				std::sort(hits.begin(), hits.end(),
					[](size_t a, size_t b) { return s_slots[a].inc > s_slots[b].inc; });
				DebugLogFile("[findctr] board %d: %zu monotone dwords (+1 or 0, never else)\n",
					board, hits.size());
				for (size_t n = 0; n < hits.size() && n < 16; ++n)
				{
					const size_t i = hits[n];
					DebugLogFile("[findctr]   i960 0x%06X  +1 x%u  hold x%u  (now %u)\n",
						BASE + static_cast<uint32_t>(i * 4),
						s_slots[i].inc, s_slots[i].same, s_slots[i].prev);
				}

				// Near-misses matter as much as clean hits: a counter that mostly steps by one but
				// occasionally jumps is still the ROM's frame counter, it just is not safe as the
				// anchor. Listing them is what tells the two cases apart instead of reporting
				// "nothing found" and leaving it ambiguous.
				std::vector<size_t> nearMiss;
				for (size_t i = 0; i < s_slots.size(); ++i)
				{
					if (s_slots[i].other > 0 && s_slots[i].inc > 0) nearMiss.push_back(i);
				}
				std::sort(nearMiss.begin(), nearMiss.end(),
					[](size_t a, size_t b) { return s_slots[a].inc > s_slots[b].inc; });
				DebugLogFile("[findctr] board %d: top near-misses (step by one, but not always)\n", board);
				for (size_t n = 0; n < nearMiss.size() && n < 16; ++n)
				{
					const size_t i = nearMiss[n];
					DebugLogFile("[findctr]   i960 0x%06X  +1 x%u  hold x%u  other x%u  (now %u)\n",
						BASE + static_cast<uint32_t>(i * 4),
						s_slots[i].inc, s_slots[i].same, s_slots[i].other, s_slots[i].prev);
				}
			}
		}

		static void LogTwoBoardState(int frame, uint32_t texid)
		{
			if (g_dllBase == nullptr) return;
			const auto at = [](size_t rva) { return g_dllBase + rva; };
			// These two are absolute VAs in the static image, so rebase them like everything else.
			const auto io = [&](int b) {
				return g_dllBase + (OmgRva::BACKUP_IO_BASE - 0x180000000)
					+ b * OmgRva::BACKUP_IO_STEP + OmgRva::IO_PORTS;
			};
			const auto video = [&](int b) {
				return g_dllBase + (OmgRva::VIDEO_BASE - 0x180000000) + b * OmgRva::VIDEO_STEP;
			};

			char io0[32], io1[32];
			for (size_t i = 0; i < OmgRva::IO_PORTS_LEN; ++i)
			{
				snprintf(io0 + i * 2, 3, "%02X", io(0)[i]);
				snprintf(io1 + i * 2, 3, "%02X", io(1)[i]);
			}
			DebugLogFile("[2board] texid=%u io0=%s io1=%s vid0=%08X vid1=%08X\n",
				texid, io0, io1,
				Hash32(video(0), OmgRva::VIDEO_SAMPLE), Hash32(video(1), OmgRva::VIDEO_SAMPLE));
			const auto sym = [](int board, uint32_t a) { return I960At(g_dllBase, board, a); };
			const auto u32 = [&](int b, uint32_t a) { return *reinterpret_cast<uint32_t*>(sym(b, a)); };

			// Read the gate BACK every sample. The module's own mode machine has a state (0x10)
			// that zeroes it along with the board index, so a write made once after module_start
			// is not necessarily still there - and "we set it" is not the same claim as "it is
			// set".
			{
				const uint8_t* c = at(OmgRva::CONFIG_BASE);
				DebugLogFile("[cfg] kind=%u diff=%u country=%u acf=%u vf20=%u free=%u vs=%u sram=%u\n",
					*reinterpret_cast<const uint32_t*>(c), c[4], c[5], c[6], c[7], c[9], c[10], c[11]);
			}
			// WHICH BOARD IS THE CANARY ACTUALLY HASHING?
			//
			// DwGame.rvaRamBasePtr is 0xC37000, which the bank switch RE-POINTS per board, so it
			// names whichever board is selected at the moment StateCheckValue runs - not board 0 by
			// construction. If that selection differs between the two peers, the desync check
			// compares one machine's MASTER against the other's SLAVE and reports a divergence that
			// is not one.
			//
			// So log the live board index next to per-board hashes of the three chunks that
			// actually diverged (0x510000 / 0x520000 / 0x590000). Read across the two peers' logs:
			//   * peer A h0 == peer B h0  -> the boards agree; the canary is sampling the wrong one
			//   * h0 differs across peers -> a real divergence, and the board index is a red herring
			{
				const auto chunk = [&](int b, uint32_t a) {
					return Hash32(I960At(g_dllBase, b, a), 0x10000);
				};
				DebugLogFile("[board] idx=%d  B0 %08X/%08X/%08X  B1 %08X/%08X/%08X\n",
					*reinterpret_cast<int32_t*>(at(OmgRva::BOARD_INDEX)),
					chunk(0, 0x510000), chunk(0, 0x520000), chunk(0, 0x590000),
					chunk(1, 0x510000), chunk(1, 0x520000), chunk(1, 0x590000));
			}
			DebugLogFile("[2board] gate=%d ", g_twoBoardGate != nullptr ? *g_twoBoardGate : -1);
			DebugLogFile("[2board] frame=%d flags=%02X/%02X send=%u/%u | "
				"B0 type=%u glob=%u main=%u vs=%u synch=%u/%u net=%u id=%u | "
				"B1 type=%u glob=%u main=%u vs=%u synch=%u/%u net=%u id=%u\n",
				frame,
				*at(OmgRva::COMM_P1_FLAG), *at(OmgRva::COMM_P2_FLAG),
				NonZeroCount(at(OmgRva::COMM_P1_SEND), OmgRva::COMM_PAYLOAD),
				NonZeroCount(at(OmgRva::COMM_P2_SEND), OmgRva::COMM_PAYLOAD),
				u32(0, OmgRva::SYM_BOARDTYPE), u32(0, OmgRva::SYM_GLOBCNTR),
				u32(0, OmgRva::SYM_MAINMODE), u32(0, OmgRva::SYM_VERSUSMODE),
				u32(0, OmgRva::SYM_SYNCHFLAG), u32(0, OmgRva::SYM_SYNCHTIME),
				*sym(0, OmgRva::SYM_NET_FLAG), *sym(0, OmgRva::SYM_LINK_ID),
				u32(1, OmgRva::SYM_BOARDTYPE), u32(1, OmgRva::SYM_GLOBCNTR),
				u32(1, OmgRva::SYM_MAINMODE), u32(1, OmgRva::SYM_VERSUSMODE),
				u32(1, OmgRva::SYM_SYNCHFLAG), u32(1, OmgRva::SYM_SYNCHTIME),
				*sym(1, OmgRva::SYM_NET_FLAG), *sym(1, OmgRva::SYM_LINK_ID));
		}

		// The gs-context allocator, at **gs+0xA0** in this generation (YLAD puts its equivalent at
		// gs+0xB0 — here 0xB0 is the cgs_device_context instead, so do NOT carry that over).
		// Shape read straight out of the resource creator FUN_18009D120:
		//     alloc: (**(code **)(*obj + 0x08))(obj, size, align)     -> vtable slot 1
		//     free:  (**(code **)(*obj + 0x18))(obj, ptr)             -> vtable slot 3
		// The module allocates every shader/resource object through it, so it runs hundreds of
		// times per boot and must stay silent.
		static void* __fastcall GsAlloc(void* /*self*/, size_t size, size_t align)
		{
			void* p = _aligned_malloc(size, align != 0 ? align : 16);
			if (p != nullptr) memset(p, 0, size);
			return p;
		}
		static void __fastcall GsFree(void* /*self*/, void* p) { _aligned_free(p); }
		static void __fastcall GsAllocNoop(void*) {}

		static void* s_gsAllocatorVtbl[4] = {
			reinterpret_cast<void*>(&GsAllocNoop),   // slot 0
			reinterpret_cast<void*>(&GsAlloc),       // slot 1 (+0x08) — alloc(this, size, align)
			reinterpret_cast<void*>(&GsAllocNoop),   // slot 2
			reinterpret_cast<void*>(&GsFree),        // slot 3 (+0x18) — free(this, ptr)
		};
		static void* s_gsAllocator[2] = { s_gsAllocatorVtbl, nullptr };

		// The cgs_device_context YAMP hands the module (and what execute_info+0x08 points at).
		// A zeroed block plus four fields, each read out of the DLL and filled in FillSharedSymbols:
		// +0x18 the D3D11 context, +0x28/+0x30 the cb/up pools, +0x38 the render-state block.
		// The rest is scratch the module manages itself.
		static uint8_t* s_deviceContext = nullptr;

		// Mirrors config.is_freeplay, which decides whether the coin/start dance runs at all.
		static bool s_isFreeplay = true;

		// ----------------------- primitive_initialize (host-provided) -----------------------
		//
		// The shared immediate-mode index buffers. `pxd::gs::primitive_initialize` lives in the
		// HOST in every generation (YakuzaKiwami2.exe here), so the module's gs constructor only
		// zeroes these slots and nothing inside the DLL ever fills them.
		//
		// The 2D/immediate primitive pusher FUN_180059020 selects between exactly two of them:
		//   kind 0xF -> gs+0x1418, then DrawIndexed(verts * 3/2)   = a QUAD list  (4 verts -> 6)
		//   kind 0xE -> gs+0x1420, then DrawIndexed((verts-2) * 3) = a triangle FAN
		// both bound with IASetIndexBuffer(..., DXGI_FORMAT_R16_UINT) and TRIANGLELIST topology.
		// A null slot faults immediately at DLL+0x59082 reading the wrapper's +0x10.
		//
		// The wrapper shape is the same cgs buffer object the YLAD generation uses (self-pointer at
		// +0x10 is the entire cache "id" the device context compares at devctx+0xF98; +0x18 points
		// at the embedded sub-object, whose +0x10 is the real ID3D11Buffer).
		struct alignas(16) gs_buffer_t
		{
			uint64_t alive;              // +0x00
			uint32_t flags;              // +0x08  0x201 = index buffer
			uint32_t byte_size;          // +0x0C
			gs_buffer_t* self;           // +0x10  its OWN address — the state-cache id
			void* sub;                   // +0x18  = (uint8_t*)this + 0x30
			uint32_t element_count;      // +0x20
			uint32_t reserved;           // +0x24
			std::byte gap[0x30 - 0x28] {};
			uint64_t sub_zero0;          // +0x30
			uint64_t sub_zero8;          // +0x38
			ID3D11Buffer* resource;      // +0x40  (sub+0x10) — what IASetIndexBuffer receives
			uint32_t sub_flags;          // +0x48
			uint32_t sub_size;           // +0x4C
			std::byte tail[0x80 - 0x50] {};
		};
		static_assert(sizeof(gs_buffer_t) == 0x80);
		static_assert(offsetof(gs_buffer_t, self) == 0x10);
		static_assert(offsetof(gs_buffer_t, sub) == 0x18);
		static_assert(offsetof(gs_buffer_t, resource) == 0x40);

		static gs_buffer_t s_primitiveBuffers[2];

		static gs_buffer_t* CreateIndexBuffer(ID3D11Device* device, gs_buffer_t& out,
			const uint16_t* indices, uint32_t count, const char* name)
		{
			const uint32_t byteSize = count * sizeof(uint16_t);

			D3D11_BUFFER_DESC desc {};
			desc.ByteWidth = byteSize;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_INDEX_BUFFER;

			D3D11_SUBRESOURCE_DATA initial {};
			initial.pSysMem = indices;

			ID3D11Buffer* buffer = nullptr;
			const HRESULT hr = device->CreateBuffer(&desc, &initial, &buffer);
			if (FAILED(hr) || buffer == nullptr)
			{
				DebugLogFile("[m2ftg::K2] %s index buffer creation FAILED (hr=0x%08X)\n", name, hr);
				return nullptr;
			}

			out.alive = 1;
			out.flags = 0x201;
			out.byte_size = byteSize;
			out.self = &out;
			out.sub = reinterpret_cast<uint8_t*>(&out) + 0x30;
			out.element_count = count;
			out.reserved = 0xFFFFFFFF;
			out.resource = buffer;
			out.sub_flags = out.flags;
			out.sub_size = byteSize;

			DebugLogFile("[m2ftg::K2] %s: %u indices (%u bytes), wrapper=%p buffer=%p\n",
				name, count, byteSize, static_cast<void*>(&out), static_cast<void*>(buffer));
			return &out;
		}

		static void PrimitiveInitialize(const RenderWindow& window)
		{
			ID3D11Device* device = window.GetD3D11Device();
			if (device == nullptr || g_gsContext == nullptr) return;
			if (*reinterpret_cast<void**>(g_gsContext + 0x1418) != nullptr) return;   // re-entry

			// p_ib_quad — {4i, 4i+1, 4i+2, 4i, 4i+2, 4i+3}. The pusher batches at most 0xC0
			// vertices (48 quads) per draw, so 192 quads is well clear of what it can ask for.
			{
				constexpr uint32_t NUM_PRIMITIVES = 192;
				static uint16_t indices[NUM_PRIMITIVES * 6];
				for (uint32_t prim = 0; prim < NUM_PRIMITIVES; prim++)
				{
					const uint16_t base = static_cast<uint16_t>(4 * prim);
					uint16_t* v = &indices[prim * 6];
					v[0] = base; v[1] = base + 1; v[2] = base + 2;
					v[3] = base; v[4] = base + 2; v[5] = base + 3;
				}
				*reinterpret_cast<void**>(g_gsContext + 0x1418) =
					CreateIndexBuffer(device, s_primitiveBuffers[0], indices,
						NUM_PRIMITIVES * 3 * 2, "p_ib_quad");
			}

			// p_ib_fan — {i+2, 0, i+1}, hub at vertex 0, drawn as a triangle list.
			{
				constexpr uint32_t NUM_PRIMITIVES = 512;
				static uint16_t indices[NUM_PRIMITIVES * 3];
				for (uint32_t prim = 0; prim < NUM_PRIMITIVES; prim++)
				{
					uint16_t* v = &indices[prim * 3];
					v[0] = static_cast<uint16_t>(prim + 2);
					v[1] = 0;
					v[2] = static_cast<uint16_t>(prim + 1);
				}
				*reinterpret_cast<void**>(g_gsContext + 0x1420) =
					CreateIndexBuffer(device, s_primitiveBuffers[1], indices,
						NUM_PRIMITIVES * 3, "p_ib_fan");
			}
		}

		static void FillSharedSymbols(const RenderWindow& window)
		{
			if (g_gsContext == nullptr) return;

			if (s_deviceContext == nullptr) s_deviceContext = new uint8_t[0x40000]();
			// +0x18 mp_sbgl_context — the RAW ID3D11DeviceContext (immediate). FUN_180093090 hands
			// exactly this to the render-target setter.
			*reinterpret_cast<void**>(s_deviceContext + 0x18) = window.GetD3D11DeviceContext();
			// +0x28 mp_cb_pool / +0x30 mp_up_pool — size-bucketed buffer tables. FUN_18009F1D0
			// indexes one as `[bucket*0x20 + sizeclass]` (three size classes: <=0x20 inline,
			// <=0x120 at +0xC02, larger at +0x1802) and lazily creates the D3D11 buffer in an empty
			// slot, so the table itself only has to exist and start zeroed. Reached on the first
			// constant-buffer upload of frame 1: a null table faults at DLL+0x9F23C reading
			// [null + index*8].
			if (*reinterpret_cast<void**>(s_deviceContext + 0x28) == nullptr)
			{
				*reinterpret_cast<void**>(s_deviceContext + 0x28) = new uint8_t[0x40000]();
				*reinterpret_cast<void**>(s_deviceContext + 0x30) = new uint8_t[0x40000]();
			}
			// +0x38 — the render-state block. `reset_state_all` (FUN_180091E40) is called as
			// `FUN_180091E40(*(devctx + 0x38))` at the end of every device-context reset and writes
			// straight through it (0x670, 0x3670, 0x3700.., up to 0x4340), so a null here is an
			// instant AV WRITE at addr=0x670. Same field as the YLAD generation's devctx.
			if (*reinterpret_cast<void**>(s_deviceContext + 0x38) == nullptr)
			{
				*reinterpret_cast<void**>(s_deviceContext + 0x38) = new uint8_t[0x8000]();
			}

			// The embedded display sub-object at gs+0xC0 — this generation's cswap_chain. The
			// render-target setter FUN_18009E1E0 reads it through DAT_1803914E8 (= shared symbol
			// slot[2]) when a caller asks for the DEFAULT targets (index -1): colour object at
			// +0x08, depth object at +0x10, and the packed dimensions at +0x20 / +0x40 as
			// `(w-1) | (h-1)<<14`. Same layout as the YLAD generation's cswap_chain.
			// The target objects themselves stay null (binding falls back gracefully); the dims
			// are seeded because they become the viewport width/height.
			uint8_t* sub = g_gsContext + 0xC0;
			*reinterpret_cast<void**>(sub + 0x00) = window.GetSwapChain();
			const uint32_t packedDims = (1280 - 1) | ((720 - 1) << 14);
			*reinterpret_cast<uint32_t*>(sub + 0x20) = packedDims;
			*reinterpret_cast<uint32_t*>(sub + 0x40) = packedDims;

			// sbgl keeps its per-device-context shadow state in a block attached to the
			// ID3D11DeviceContext with a pxd GUID, and fetches it with
			// GetPrivateData(guid, 8, &blockPtr) — ID3D11DeviceChild vtable slot 4 (+0x20). The
			// render-target setter then writes the bound targets and the viewport straight into
			// that block, so an unattached (null) block is an immediate AV WRITE at DLL+0x9E34E.
			// In the real game the host's device-start attaches it; here YAMP does.
			{
				static const GUID kPxdCtxGuid =
					{ 0xa84b07f7, 0x3bd0, 0x4c4e, { 0x89, 0x90, 0xe9, 0xd5, 0xaf, 0x6e, 0xfb, 0xa2 } };
				static uint8_t s_ctxShadowState[0x1000] {};
				void* blockPtr = s_ctxShadowState;
				window.GetD3D11DeviceContext()->SetPrivateData(kPxdCtxGuid, sizeof(blockPtr), &blockPtr);
			}

			void** p = reinterpret_cast<void**>(g_gsContext + 0x20);
			p[0] = window.GetD3D11Device();
			p[1] = sub;
			p[2] = sub;                                        // yes, the same pointer twice
			p[3] = reinterpret_cast<void*>(uintptr_t(1));      // a value, not a pointer
			p[4] = &s_zeroDword;
			p[5] = nullptr;                                    // no allocator in this generation

			*reinterpret_cast<void**>(g_gsContext + 0xA0) = s_gsAllocator;
			*reinterpret_cast<void**>(g_gsContext + 0xB0) = s_deviceContext;

			// Handle (instance) tables — the same bitmap allocator the YLAD host builds, but at
			// **gs+0x1808** with a 0x20 stride, NOT YLAD's gs+0x101718.
			//
			// Structure read straight out of the allocator FUN_180093CC0: it takes `free_top`
			// (+0x18) as a WORD index into the `free_tbl` bitmap (+0x08), requires
			// `free_top < free_words` (+0x1C), pulls the highest set bit, stores the object into
			// `tbl` (+0x00) and returns a 1-BASED id. A zeroed table therefore has
			// `0 < 0 == false` and can NEVER allocate — which is exactly why GSVS resource
			// creation returned 0 and nulled the handle that crashed FUN_18005C050.
			//
			// Offsets in use, from the ADD RCX,imm before every call to that allocator:
			// 0x1808, 0x1828 (the shader/GSVS one), 0x1848, 0x1868, 0x1888, 0x18A8, 0x18E8, 0x1908.
			// A contiguous run of ten covers those plus the 0x18C8 gap (which the lookup/free
			// siblings use). Capacity is uniform and generous because the per-slot meaning is not
			// mapped for this generation — YLAD's order (mesh/tex/vs/ps/...) must NOT be assumed.
			{
				struct instance_tbl
				{
					void** tbl;
					uint64_t* free_tbl;
					uint32_t status;
					uint32_t max;
					uint32_t free_top;
					uint32_t free_words;
				};
				static_assert(sizeof(instance_tbl) == 0x20);

				constexpr uint32_t kCapacity = 16384;
				constexpr uint32_t kWords = (kCapacity + 63) / 64;
				for (int i = 0; i < 10; i++)
				{
					auto* t = reinterpret_cast<instance_tbl*>(g_gsContext + 0x1808 + i * 0x20);
					if (t->tbl != nullptr) continue;   // already built (re-entry safety)
					t->tbl = new void* [kCapacity]();
					t->free_tbl = new uint64_t[kWords];
					for (uint32_t w = 0; w < kWords; w++) t->free_tbl[w] = ~0ull;
					t->status = 0;
					t->max = kCapacity;
					t->free_top = 0;
					t->free_words = kWords;
				}
				DebugLogFile("[m2ftg::K2] instance tables: 10 x %u entries at gs+0x1808\n", kCapacity);
			}

			PrimitiveInitialize(window);

			DebugLogFile("[m2ftg::K2] gs=%p device=%p swapchain=%p devctx=%p\n",
				static_cast<void*>(g_gsContext), static_cast<void*>(window.GetD3D11Device()),
				static_cast<void*>(window.GetSwapChain()), static_cast<void*>(s_deviceContext));
		}

		// ================================== boot ==================================

		static std::filesystem::path gamePath;
		static wil::unique_hmodule gameDll;
		static module_func_t g_moduleMain = nullptr;
		// What module_start writes through params.module_main; cross-checked against the
		// pattern-scanned MODULE_MAIN so a bad pattern cannot go unnoticed.
		static module_func_t g_moduleMainFromParams = nullptr;

		HMODULE LoadDLL()
		{
			const GameDesc& game = CurrentGame();

			{
				DWORD dwSize = GetCurrentDirectoryW(0, nullptr);
				auto buf = std::make_unique<wchar_t[]>(dwSize);
				GetCurrentDirectoryW(dwSize, buf.get());
				gamePath.assign(buf.get());
			}

			// Locate the DLL as a FILE first, so the integrity check runs before LoadLibrary.
			// Kiwami 2 keeps both modules in <install>/m2ftg/ next to rom/ and w64/.
			std::filesystem::path dllFile = gamePath / game.dll_name;
			if (!std::filesystem::is_regular_file(dllFile))
			{
				gamePath.append(game.subdir);
				dllFile = gamePath / game.dll_name;
			}

			if (!std::filesystem::is_regular_file(dllFile))
			{
				const std::wstring str(L"Could not load " + std::wstring(game.dll_name) +
					L"!\n\nMake sure that YAMP.exe is located next to the DLL file, or that its \"" +
					game.subdir + L"\" subdirectory contains it.");
				MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
				return nullptr;
			}

			gGeneral.SetDLLName(WcharToUTF8(game.dll_name));

			if (!Verify::CheckBeforeLoad(gGeneral.GetGameId(), dllFile))
			{
				return nullptr;
			}

			gameDll.reset(LoadLibraryW(dllFile.c_str()));
			if (!gameDll)
			{
				const std::wstring str(L"Could not load " + std::wstring(game.dll_name) +
					L"!\n\nThe file exists but Windows refused to load it.");
				MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
			}
			return gameDll.get();
		}

		void PreInitialize()
		{
			gGeneral.SetDataPath();
			gGeneral.LoadSettings();
		}

		static bool ResolveSymbolsAndPatch(void* dll, const RenderWindow& window) try
		{
			const Imports symbolMap = BuildSymbolMap(dll);

			if (const char* missing = RequiredButMissing(symbolMap); missing[0] != '\0')
			{
				DebugLogFile("[m2ftg::K2] unresolved symbols: %s\n", missing);
				return false;
			}

			const ScopedUnprotect::Section text(static_cast<HMODULE>(dll), ".text");
			const ScopedUnprotect::Section rdata(static_cast<HMODULE>(dll), ".rdata");

			// Lets YAMP's command line reach the module's own option parser, which module_start
			// otherwise calls with an empty argv. Must be before module_start - the parse happens
			// inside it.
			ModuleArgs::Install(dll);

			g_moduleMain = reinterpret_cast<module_func_t>(
				symbolMap.GetSymbol(ImportSymbol::MODULE_MAIN));
			g_gsContext = static_cast<uint8_t*>(symbolMap.GetSymbol(ImportSymbol::GS_CONTEXT_INSTANCE));
			g_slContext = symbolMap.GetSymbol(ImportSymbol::SL_CONTEXT_INSTANCE);
			sl::handle_create_internal = reinterpret_cast<decltype(sl::handle_create_internal)>(
				symbolMap.GetSymbol(ImportSymbol::SL_HANDLE_CREATE));
			// Returns file handle objects to the pool. YAMP's async worker calls it on every
			// completed close; leaving it null faults the worker during teardown.
			sl::file_handle_destroy = reinterpret_cast<decltype(sl::file_handle_destroy)>(
				symbolMap.GetSymbol(ImportSymbol::SL_FILE_HANDLE_DESTROY));
			// This generation's plain rwspinlock stands in for the LJ/YLAD recursive one — see
			// ImportSymbols.h. csl_archive::create_instance calls these on every archive read, from
			// YAMP's own async worker thread.
			sl::archive_lock_wlock = reinterpret_cast<decltype(sl::archive_lock_wlock)>(
				symbolMap.GetSymbol(ImportSymbol::ARCHIVE_LOCK_RLOCK));
			sl::archive_lock_wunlock = reinterpret_cast<decltype(sl::archive_lock_wunlock)>(
				symbolMap.GetSymbol(ImportSymbol::ARCHIVE_LOCK_RUNLOCK));

			// Optional: only the linked-cabinet modules (omg, mr) have a second board at all.
			g_twoBoardGate = static_cast<uint8_t*>(symbolMap.TryGetSymbol(ImportSymbol::TWO_BOARD_GATE));
			g_boardSelect = reinterpret_cast<board_select_t>(
				symbolMap.TryGetSymbol(ImportSymbol::BOARD_SELECT));
			g_perBoardInit = reinterpret_cast<board_select_t>(
				symbolMap.TryGetSymbol(ImportSymbol::PER_BOARD_INIT));
			g_dllBase = static_cast<uint8_t*>(dll);

			// Which board the render draws. Applied per frame from the netplay role rather than
			// once at load from a command line - see SetRenderBoard.
			g_renderBoardSelect =
				static_cast<uint8_t*>(symbolMap.TryGetSymbol(ImportSymbol::RENDER_BOARD_SELECT));

			// The gs context pointer global self-initialises lazily; point it up front.
			auto** ppGsCtx = static_cast<uint8_t**>(symbolMap.GetSymbol(ImportSymbol::GS_CONTEXT_PTR));
			*ppGsCtx = g_gsContext;

			DebugLogFile("[m2ftg::K2] module_main=%p gsCtx=%p (size field 0x%X)\n",
				reinterpret_cast<void*>(g_moduleMain), static_cast<void*>(g_gsContext),
				g_gsContext != nullptr ? *reinterpret_cast<uint32_t*>(g_gsContext + 8) : 0);

			// ---- sl bring-up -----------------------------------------------------------------
			// The module's embedded sl context is CONSTRUCTED but never INITIALISED — verified on a
			// live Kiwami 2, where the host's own sl context has 18 populated qwords in
			// +0x10..+0x100 and this embedded one has exactly one. Handing the module a dead sl
			// context makes every file/handle lookup fail.
			//
			// pxd::sl's initialize() does the head of the context (all at the LJ offsets in this
			// generation), but the handle queue and the file-handle pool live 0x3C0 bytes further
			// down here — see pxd/K2/sl.h — so those come from pxd::K2 instead.
			pxd::K2::set_context(g_slContext);

			// pxd::sl allocates through this hook. Rather than hunt this DLL's own kernel_calloc
			// (its body has diverged too far from the LJ/YLAD builds for pattern transfer — a
			// similarity sweep tops out at noise), YAMP supplies its own zeroing allocator, which
			// mirrors the real host: its sl context's allocator slot at +0x158 points at an object
			// inside YakuzaKiwami2.exe, i.e. the host's allocator, not the module's.
			// CAVEAT: if the module ever frees these buffers through its own allocator, that
			// mismatch would surface at teardown — the first thing to suspect if module_stop faults.
			if (sl::kernel_calloc_internal == nullptr)
			{
				sl::kernel_calloc_internal = [](uint64_t bytes, uint32_t) -> void* {
					void* p = _aligned_malloc(static_cast<size_t>(bytes), 16);
					if (p != nullptr) memset(p, 0, static_cast<size_t>(bytes));
					return p;
				};
			}

			if (pxd::K2::context()->handles.p_handle_buffer == nullptr)
			{
				constexpr uint32_t kHandleCapacity = 0x100000;
				if (const int rc = sl::initialize(pxd::K2::kContextSize); rc != 0)
				{
					DebugLogFile("[m2ftg::K2] sl::initialize FAILED (0x%X)\n", rc);
					return false;
				}
				// NOT sl::handle_initialize: it builds the free queue at LJ's 0x6C0, and this
				// generation's handle_create_internal pops it at sl+0xA80.
				if (!pxd::K2::InitHandles(kHandleCapacity))
				{
					DebugLogFile("[m2ftg::K2] K2::InitHandles FAILED\n");
					return false;
				}
				// The FILE half of sl. Without it the module can only reach resources embedded in
				// the DLL; anything it loads from disk (notably m2ftg/w64/shader_pxd_w64.farc)
				// comes back as a null blob and faults far downstream. The live host's sl context
				// has all of these populated (+0x88 p_file_handle_tbl, +0x90 p_file_access,
				// +0x98 p_file_async_request, +0x118/+0x120 archive access).
				pxd::K2::PatchSl();

				DebugLogFile("[m2ftg::K2] sl initialised: ctx=%p size=0x%X handles=%p access=%p\n",
					static_cast<void*>(pxd::K2::context()),
					pxd::K2::context()->size_of_struct,
					static_cast<void*>(pxd::K2::context()->handles.p_handle_buffer),
					static_cast<void*>(pxd::K2::context()->p_file_access));
			}

			FillSharedSymbols(window);
			return true;
		}
		catch (...)
		{
			const std::wstring str(L"Failed to resolve imports and/or patch " +
				std::wstring(CurrentGame().dll_name) +
				L"!\n\nIt's either not a valid arcade module DLL from Yakuza Kiwami 2, or the game "
				L"has been updated and YAMP is not forward compatible with that new version.");
			MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
			return false;
		}

		static bool GameLoop(RenderWindow& window)
		{
			static execute_info_t execute_info {};
			static int s_frame = 0;
			static int s_lastLogged = -1;

			execute_info.size_of_struct = sizeof(execute_info);
			execute_info.p_device_context = s_deviceContext;
			execute_info.status = 0;
			execute_info.result = 0x80004005;   // the module writes S_OK over this on success
			execute_info.output_texid = 0;
			execute_info.sound_volume =
				static_cast<float>(gGeneral.GetSettings()->m_volumePercent) / 100.0f;

			// (1) Read the session state ONCE at the top, before Drive() can advance it, so pad
			// routing and the coin protocol see one stable answer for the whole frame. The order
			// of the four calls is part of the contract - see NetSession.h.
			NetSession& net_ = NetplaySession();
			const NetSession::Status netStatus = net_.GetStatus();
			const bool netplayMatch = netStatus.inMatch;
			const int32_t netplayLocalPad = netStatus.localPad;
			const bool netplayLocked = netStatus.locked;

			static bool s_pauseMenuOpen = false;
			{
				// Disabled during netplay, as in every other host: the module's pause path stops
				// the emulated board, and stopping it on one machine only desyncs the pair.
				if (netplayLocked) s_pauseMenuOpen = false;
				static bool s_escWasDown = false;
				const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
				if (escDown && !s_escWasDown && !netplayLocked) s_pauseMenuOpen = !s_pauseMenuOpen;
				s_escWasDown = escDown;
			}
			if (s_pauseMenuOpen) execute_info.status |= 1;

			// Input: refresh the shared XInput snapshot, then evaluate each player's bindings via
			// csl_pad — the same Controls page every other m2ftg host uses. This generation embeds
			// the plain csl_pad in execute_info, so the whole struct goes across rather than LJ's
			// 0xE0 prefix.
			Input::PollPads();
			static pxd::csl_pad s_pads[2];
			if (netplayMatch && netplayLocalPad >= 0)
			{
				// ONLINE YOU ALWAYS PLAY AS PLAYER 1 LOCALLY. The guest occupies pad slot 1 in the
				// match, but they are the only person at that keyboard and have their own Player 1
				// bindings. The other slot is filled from P2 only to stay well-formed: Step()
				// overwrites BOTH pads from the transmitted inputs before module_main sees them.
				//
				// For Virtual On this ALSO decides which cabinet you drive: pad[N] reaches board N
				// (measured - see docs/von-netplay-recon.md), so localPad is the cabinet.
				s_pads[netplayLocalPad].set_state(0);
				s_pads[1 - netplayLocalPad].set_state(1);
			}
			else
			{
				s_pads[0].set_state(0);
				s_pads[1].set_state(1);
			}
			for (int p = 0; p < 2; p++)
			{
				execute_info.pad[p] = s_pads[p];
				execute_info.pad[p].m_port = static_cast<unsigned int>(p);
				execute_info.pad[p].m_user_id = p;
				execute_info.pad[p].m_is_connected = true;
			}

			// The FIXED module-facing assignment table (slot order A, B, Y, X, LT, LB, RT, RB).
			// Remapping happens host-side in csl_pad::set_state, which routes each player's bound
			// inputs onto the bit carrying the wanted combo, so this must be Input::MODULE_ASSIGN —
			// the table those routes assume. Using the raw template instead shifts every combo
			// button by one slot (the bug that was found and fixed in the YLAD VF2 host).
			for (int p = 0; p < 2; p++)
			{
				for (int i = 0; i < 8; i++) execute_info.assign[p][i] = Input::MODULE_ASSIGN[i];
			}

			// (2) Poll the plugin and run the round-start state machine, then (3) decide whether the
			// emulator may advance. Step() also overwrites BOTH pads with the transmitted inputs,
			// which is why it runs after the local pad fill above.
			//
			// MARSHALLED, because this generation's execute_info is NOT the one the netplay plugin
			// knows. The plugin writes pads at Lost Judgment's offsets - 0x1760 struct, 0x190 pad
			// stride, pad[1] at 0x1B0 - and validates them through yampnet_layout at load. Kiwami 2
			// uses the plain engine pad instead: 0x16E0 struct, 0x170 stride, pad[1] at 0x190.
			//
			// So hand the plugin a shadow struct in ITS layout and copy the pads across. Loosening
			// the handshake instead would trade a load-time failure for a silent write at the wrong
			// offset, which is exactly what that handshake exists to prevent. The copy is safe
			// because lj_pad_t is csl_pad with a tail added - the first 0x170 bytes, which is
			// everything the plugin reads or writes (m_buttons +0xA0, m_port +0xE0), are identical.
			net_.Drive();
			static m2ftg_execute_info_t s_netShadow {};
			static_assert(sizeof(s_netShadow.pad[0]) >= sizeof(execute_info.pad[0]),
				"the LJ pad must be able to hold this generation's pad");
			for (int p = 0; p < 2; p++)
			{
				memcpy(&s_netShadow.pad[p], &execute_info.pad[p], sizeof(execute_info.pad[p]));
			}
			const bool advanceFrame = net_.Step(s_netShadow);
			for (int p = 0; p < 2; p++)
			{
				memcpy(&execute_info.pad[p], &s_netShadow.pad[p], sizeof(execute_info.pad[p]));
			}

			// Dedicated coin binding -> the coin status bit (host->module bit5). Meaningless in
			// freeplay, swallowed while the pause menu is open, and dead for the whole session
			// during netplay - it is a host->module input outside the transmitted pad, so one
			// machine raising it desyncs the pair.
			{
				static bool s_coinWasDown[2] = {};
				for (int p = 0; p < 2; p++)
				{
					const bool down = Input::ActionDown(p, Input::Action_Coin);
					if (down && !s_coinWasDown[p] && !s_isFreeplay && !s_pauseMenuOpen && !netplayLocked)
					{
						execute_info.status |= 0x20;
					}
					s_coinWasDown[p] = down;
				}
			}

			// The arcade coin/start dance. ONLY meaningful with the freeplay dip switch OFF: with
			// is_freeplay = 1 the module takes START directly and there is no coin to insert, so
			// running the dance would just swallow the player's first press.
			static bool s_startScreen = false;
			static bool s_coinPending = false;
			static bool s_startToggle = false;
			if (!s_isFreeplay && !s_pauseMenuOpen)
			{
				if (s_coinPending && s_startScreen)
				{
					if (!s_startToggle)
					{
						execute_info.pad[0].m_now |= 0x100;
						execute_info.pad[0].m_push |= 0x100;
					}
					s_startToggle = !s_startToggle;
				}
				else
				{
					s_coinPending = false;
					if (s_startScreen && (execute_info.pad[0].m_now & 0x100) != 0)
					{
						execute_info.status |= 0x20;
						execute_info.pad[0].m_now &= ~0x100u;
						execute_info.pad[0].m_push &= ~0x100u;
						s_coinPending = true;
						s_startToggle = false;
					}
				}
			}

			window.BeginFrame();
			window.NewImGuiFrame();
			if (s_pauseMenuOpen && !DrawPauseMenu(window, s_pauseMenuOpen))
			{
				return false;   // Quit picked
			}

			// Reassert the two-board gate every frame. Measured: a single write after module_start
			// is gone by frame 200 - the module's own mode machine passes through the state that
			// zeroes it (along with the board index and the link-enabled gate) during bring-up.
			// Holding it set is the crude version; once board 1 is proven to run, this should
			// become a write at the right point in that state machine rather than a per-frame
			// stomp.
			// ALWAYS two-board on a module that has a second board, from boot, whether or not a
			// session is up. Deliberately not conditional on netplay:
			//
			//   * Turning it on AT session start is itself a desync risk - board 1 would begin
			//     booting at a host-timing-dependent moment, differently on each peer. Always-on
			//     removes the transition rather than trying to synchronise it.
			//   * The launcher passes only the game's bootArg, so a flag could never reach a
			//     launcher-started match. Both peers would silently run single-board, which is
			//     what happened during the first two-machine tests.
			//   * MEASURED equivalent: over 2500 frames the two modes reach the same status
			//     progression (0x0 -> 0x40) and the same MainMode, with GlobCntr differing only by
			//     the link check's extra boot work. Virtual On does not stall waiting for the
			//     second cabinet.
			//
			// Still UNVERIFIED for actual gameplay - the ROM has WaitAnother/WaitChallenger, and
			// only boot and attract have been compared. `-von-1board` forces the old behaviour so
			// the two can still be A/B'd if a match misbehaves.
			const bool wantTwoBoard = wcsstr(GetCommandLineW(), L"-von-1board") == nullptr;

			// HOLD OFF WHILE THE BOARD IS BEING RESET. NetSession resets the board at round start,
			// and RESET (0x6BA90) clears this gate AND re-inits board 0 only - board 1's window
			// biases are left stale. Slamming the gate straight back on makes the frame step
			// immediately step board 1 through a dead memory map, which faulted at omg+0x44103
			// reading an unmapped host address for i960 0x500440.
			//
			// `locked && !inMatch` is exactly the prep window (reset -> settle -> anchor), so this
			// waits for the round to go live before re-enabling the second cabinet.
			//
			// THIS IS A GUARD, NOT THE FIX. The real answer is to re-init board 1 after a reset -
			// boot does it explicitly via the per-board init 0x18006BAF0(0)/(1), which RESET does
			// not call. Until that is wired up, board 1 simply stays out of the round.
			const bool boardResetting = netplayLocked && !netplayMatch;
			const bool gateOn = wantTwoBoard && !boardResetting;

			// RE-INIT BOARD 1 BEFORE LETTING IT RUN AGAIN. RESET only re-inits board 0, so board 1
			// keeps a stale i960 context across a round-start reset - its IP was measured pointing
			// at 0x500440, inside work RAM, and the fetch faulted the moment the gate came back on.
			// Boot itself calls this same routine for both boards, so this is the module's own
			// mechanism rather than anything invented here.
			//
			// Only on the OFF->ON edge, and only board 1: board 0 is mid-round by then and must not
			// be disturbed.
			{
				static bool s_gateWasOn = false;
				if (gateOn && !s_gateWasOn && g_perBoardInit != nullptr)
				{
					g_perBoardInit(1);
					DebugLogFile("[m2ftg::K2] board 1 re-initialised after reset\n");
				}
				s_gateWasOn = gateOn;
			}
			if (g_twoBoardGate != nullptr && gateOn) *g_twoBoardGate = 1;

			// And which cabinet you SEE follows your role: host drives the master, guest the slave.
			SetRenderBoard((netplayMatch && netplayLocalPad == 1) || (!netplayLocked && WantRenderBoard1())
				? 1 : 0);

			// Input-routing probe: hold a pattern on pad[1] and nothing on pad[0]. If player N's
			// input reaches board N, the two boards' I/O bytes have to diverge; if both boards read
			// pad[0], they stay identical no matter what pad[1] does.
			if (WantPadTest())
			{
				execute_info.pad[1].m_now = 0x000F;   // all four directions, unmistakable
				execute_info.pad[1].m_push = 0x000F;
				execute_info.pad[0].m_now = 0;
				execute_info.pad[0].m_push = 0;
			}

			// (4) module_main only runs when the session says the frame may advance; a stalled
			// lockstep frame skips it entirely, and that skip IS the stall. output_texid is zeroed
			// every frame and only module_main fills it in, so a stall would leave 0 - which is not
			// a valid handle. Remember the last good id so a stall re-presents the previous frame.
			static unsigned int s_lastOutputTexId = 0;
			int funcResult = 0;
			if (advanceFrame)
			{
				funcResult = g_moduleMain(sizeof(execute_info), &execute_info);
				if (execute_info.output_texid != 0) s_lastOutputTexId = execute_info.output_texid;

				// PIN BOARD 0 FOR THE CANARY. EndFrame reads emulated RAM through the per-board
				// bias, so without this it hashes whichever board the frame step happened to leave
				// selected - and the two boards genuinely differ in the chunk the canary covers
				// (link_ID alone is 1 on the master, 2 on the slave). Two peers selecting
				// differently then compare master against slave and desync on frame 1 with no real
				// divergence. -von-render1 guarantees exactly that, since it leaves board 1 up.
				//
				// Restoring afterwards is deliberate: the render already ran inside module_main, so
				// nothing this frame still depends on the selection, but leaving it as we found it
				// keeps this purely a measurement.
				// Only pin while a round is actually LIVE. During round PREP the board is being
				// reset and EndFrame submits nothing anyway (m_frame is UINT32_MAX and it returns
				// early), so pinning there buys nothing and costs a lot: the bank switch re-points
				// a dozen globals, and calling it while the module is mid-reset helped produce a
				// second-chance AV at omg+0x44103 reading an unmapped host address for i960
				// 0x500440 - i.e. a stale window bias - right after "board reset".
				//
				// The index is also clamped. The switch computes its bases with unchecked IMULs
				// off the board number, so a garbage value silently points every window somewhere
				// unmapped rather than failing.
				const int rawBoard = (g_dllBase != nullptr)
					? *reinterpret_cast<int32_t*>(g_dllBase + OmgRva::BOARD_INDEX) : 0;
				const bool pin = netplayMatch && g_boardSelect != nullptr && rawBoard == 1;
				if (pin) g_boardSelect(0);
				net_.EndFrame();
				if (pin) g_boardSelect(1);

				// Per-chunk work-RAM map, into yampnet.log so it lands beside the desync report on
				// BOTH machines and can be diffed over the share. This replaces the plugin's own
				// [rammap], which only appears in one of its desync modes and is not reachable from
				// YAMP's options.
				//
				// Logged straight after EndFrame, so each line follows that frame's `timers f=N`
				// trace and the netplay frame number is unambiguous. Sixteen 0x10000 chunks across
				// the i960 0x500000 window, PER BOARD - the previous map only showed board 0, and
				// with two cabinets a divergence can start on either.
				// Logged whenever a round is LIVE, not only behind a debug flag - the launcher
				// passes no flags, so a flag-gated diagnostic produces nothing on exactly the runs
				// that matter. That is why the guest's log had no [vonmap] lines at all.
				if (g_dllBase != nullptr && (netplayMatch || WantTwoBoardProbe()))
				{
					char line[16 * 9 + 1];
					for (int b = 0; b < 2; ++b)
					{
						int n = 0;
						for (int c = 0; c < 16; ++c)
						{
							n += snprintf(line + n, sizeof(line) - n, "%08X ",
								Hash32(I960At(g_dllBase, b, 0x500000 + c * 0x10000), 0x10000));
						}
						net::Logf("[vonmap] b%d %s", b, line);
					}
				}
			}
			else
			{
				execute_info.output_texid = s_lastOutputTexId;
			}

			const int interesting = execute_info.status | (funcResult << 16);
			if (interesting != s_lastLogged && s_frame < 5000)
			{
				DebugLogFile("[m2ftg::K2] frame=%d status=0x%X result=0x%X texid=%u ret=0x%X\n",
					s_frame, execute_info.status, execute_info.result,
					execute_info.output_texid, funcResult);
				s_lastLogged = interesting;
			}
			// Sample AFTER module_main, so each delta covers exactly one call.
			if (WantFindCounter() && g_dllBase != nullptr)
			{
				FindCtr::Sample(g_dllBase, 0);
				if (s_frame == 1)   FindCtr::SnapGlobals(g_dllBase);
				if (s_frame == 900) { FindCtr::Report(0); FindCtr::ReportGlobals(g_dllBase); }
			}
			if (WantTwoBoardProbe() && (s_frame % 200) == 0)
			{
				LogTwoBoardState(s_frame, execute_info.output_texid);
			}
			s_frame++;
			// status bit6 = the attract/insert-coin screen, the state the coin dance needs.
			s_startScreen = (execute_info.status & 0x40) != 0;

			// Display blit. execute_info.output_texid is a 1-BASED index into the FIRST instance
			// table (gs+0x1808, capacity at gs+0x181C) — the same table the render-target setter
			// FUN_180093090 indexes with `(texid - 1)` to resolve a colour target, which is what
			// identifies it as the texture table.
			//
			// From there the D3D11 view lives inside the sbgl sub-object at tex+0x20: the module's
			// own view getter FUN_180099EF0 reads an array of 0x40-byte per-subresource records at
			// sub+0x30 (dimensions at sub+0x14/+0x18). The exact field inside a record is not
			// pinned, so probe COM pointers once with a guarded QueryInterface and cache the hit —
			// the same approach the YLAD VF2 host uses.
			if (execute_info.output_texid != 0)
			{
				static ID3D11ShaderResourceView* s_displaySrv = nullptr;
				static bool s_srvProbed = false;
				if (!s_srvProbed && s_frame >= 60)
				{
					s_srvProbed = true;
					void** tbl = *reinterpret_cast<void***>(g_gsContext + 0x1808);
					uint8_t* tex = tbl != nullptr
						? static_cast<uint8_t*>(tbl[execute_info.output_texid - 1]) : nullptr;
					auto tryQiSrv = [](void* cand) -> ID3D11ShaderResourceView*
					{
						if (cand == nullptr || (reinterpret_cast<uintptr_t>(cand) & 0x7) != 0) return nullptr;
						ID3D11ShaderResourceView* srv = nullptr;
						__try
						{
							IUnknown* unk = static_cast<IUnknown*>(cand);
							if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&srv)))) return srv;
						}
						__except (EXCEPTION_EXECUTE_HANDLER) {}
						return nullptr;
					};
					if (tex != nullptr)
					{
						for (size_t texOff : { size_t(0x20), size_t(0x28) })
						{
							uint8_t* sub = *reinterpret_cast<uint8_t**>(tex + texOff);
							if (sub == nullptr) continue;
							for (size_t subOff = 0; subOff <= 0x40 && s_displaySrv == nullptr; subOff += 8)
								s_displaySrv = tryQiSrv(*reinterpret_cast<void**>(sub + subOff));
							// Then the per-subresource view records at sub+0x30, stride 0x40.
							uint8_t* records = *reinterpret_cast<uint8_t**>(sub + 0x30);
							for (size_t recOff = 0; records != nullptr && recOff < 0x40 &&
								s_displaySrv == nullptr; recOff += 8)
								s_displaySrv = tryQiSrv(*reinterpret_cast<void**>(records + recOff));
							if (s_displaySrv != nullptr)
							{
								DebugLogFile("[m2ftg::K2] display SRV found via tex+0x%zX (srv=%p)\n",
									texOff, static_cast<void*>(s_displaySrv));
								break;
							}
						}
					}
					if (s_displaySrv == nullptr)
					{
						DebugLogFile("[m2ftg::K2] display SRV NOT found (texid=%u tbl=%p tex=%p)\n",
							execute_info.output_texid, static_cast<void*>(tbl), static_cast<void*>(tex));
					}
				}
				if (s_displaySrv != nullptr)
				{
					window.BlitGameFrame(s_displaySrv);
				}
			}

			window.RenderImGui();
			window.EndFrame();
			if (FAILED(window.GetSwapChain()->Present(1, 0))) return false;
			return true;
		}

		void Run(RenderWindow& window)
		{
			const auto module_start = reinterpret_cast<module_func_t>(
				GetProcAddress(gameDll.get(), "module_start"));
			THROW_LAST_ERROR_IF_NULL(module_start);
			const auto module_stop = reinterpret_cast<module_func_t>(
				GetProcAddress(gameDll.get(), "module_stop"));
			THROW_LAST_ERROR_IF_NULL(module_stop);

			if (!ResolveSymbolsAndPatch(gameDll.get(), window))
			{
				DebugLogFile("[m2ftg::K2] ResolveSymbolsAndPatch FAILED\n");
				return;
			}

			Cri criware;

			// module_start's parameter block. It looks at first read as though module_start only
			// touches +0x20 and +0x38, but the rest is consumed by the helper it calls first
			// (FUN_18005E800), which does `*params.module_main = module_main` and hands the sl/ct/gs
			// blocks to their initialize_module functions. Sizes those enforce, read from each:
			//   sl (FUN_1800656D0): module 0x10, context 0xF3C0     — dereferenced unconditionally
			//   ct (FUN_1800A87A0): module 0x10, context 0x30       — dereferenced unconditionally
			//   gs (FUN_180097830): module **0x50**, context 0x202140, and NULL-SAFE (a null block
			//                       makes it fall back to its own embedded context)
			// Note gs's module block is 0x50 here, not the 0x58 of the LJ/YLAD generations.
			struct sl_module_t
			{
				size_t size = sizeof(sl_module_t);
				void* context;
			} sl_module;
			static_assert(sizeof(sl_module_t) == 0x10);
			sl_module.context = g_slContext;

			// ct: nothing in YAMP reads it and the module only size-checks it, so an opaque block
			// of the right size is enough (same as every other host).
			struct ct_context_t
			{
				uint32_t tag_id;
				uint32_t version;
				uint32_t size_of_struct = 0x30;
				std::byte unknown[0x30 - 0x0C] {};
			};
			static_assert(sizeof(ct_context_t) == 0x30);
			static ct_context_t s_ctContext;

			const struct ct_module_t
			{
				size_t size = sizeof(ct_module_t);
				ct_context_t* context = &s_ctContext;
			} ct_module;
			static_assert(sizeof(ct_module_t) == 0x10);

			// gs's module block. Passing NULL here is legal — initialize_module falls back to the
			// embedded context, which is the very one YAMP fills — but it SKIPS
			// FUN_1800A1010(ctx+0x20), and that function is not an initialiser: it READS the
			// shared-symbol block and copies the values into the DLL's own globals
			// (device -> DAT_1803914D8, the gs+0xC0 sub-object -> DAT_1803914E0/E8, slot[3] as a
			// DWORD -> DAT_1803914D4, slot[4] -> PTR_DAT_180171068). Skip it and those globals stay
			// null, which is what produced the null deref in FUN_1800A1880. So pass the block.
			struct gs_module_t
			{
				size_t size = 0x50;
				void* context;
				uint8_t pad[0x50 - 0x10] {};
			} gs_module;
			static_assert(sizeof(gs_module_t) == 0x50);
			gs_module.context = g_gsContext;

			struct params_t
			{
				size_t size = sizeof(params_t);
				const sl_module_t* sl_module = nullptr;   // +0x08
				const gs_module_t* gs_module = nullptr;   // +0x10
				const ct_module_t* ct_module = nullptr;   // +0x18
				const icri* cri_ptr = nullptr;            // +0x20
				const char* root_path = nullptr;          // +0x28
				module_func_t* module_main = nullptr;     // +0x30 — the module writes THROUGH this
				m2ftg_config_t config {};                 // +0x38, 0x100C bytes, copied wholesale
			} params;
			static_assert(offsetof(params_t, config) == 0x38);

			const std::string utf8Path = gamePath.u8string();
			params.sl_module = &sl_module;
			params.gs_module = &gs_module;
			params.ct_module = &ct_module;
			params.cri_ptr = &criware;
			params.root_path = utf8Path.c_str();
			// A real slot for the module to write into. Leaving this null is an immediate
			// null-deref inside FUN_18005E800. The pattern-scanned MODULE_MAIN should agree with
			// whatever lands here — Run() checks that below.
			params.module_main = &g_moduleMainFromParams;

			const auto* settings = gGeneral.GetSettings();
			params.config.kind = CurrentGame().kind;   // 0 = "vf2" in this DLL's own name table
			params.config.difficulty = settings->m_m2Difficulty <= 3
				? static_cast<uint8_t>(settings->m_m2Difficulty) : 1;
			params.config.country = settings->m_m2Country <= 2
				? static_cast<uint8_t>(settings->m_m2Country) : 0;
			params.config.is_freeplay = settings->m_m2Freeplay ? 1 : 0;
			s_isFreeplay = settings->m_m2Freeplay;
			params.config.is_vs_mode = settings->m_m2VersusMode ? 1 : 0;
			// The VF2 module's own two extra switches. Same m2ftg_config_t as every other
			// generation — the live capture decoded this DLL's copied config against it exactly —
			// so the YLAD VF2 settings apply here unchanged.
			params.config.is_vf20 = settings->m_vf2Version20 ? 1 : 0;

			ApplyAspectSetting(window, settings->m_m2Aspect);

			DebugLogFile("[m2ftg::K2] module_start(size=%zu, root='%s', kind=%u)\n",
				sizeof(params), params.root_path, params.config.kind);
			const int startResult = module_start(sizeof(params), &params);
			// The module renders into a fixed 1024x768 texture whatever its own resolution option
			// selected, so tell the compositor which sub-rect of it actually holds the frame. Read
			// after module_start: that is when the module's parse ran, and a switch on YAMP's command
			// line can differ from the setting.
			{
				uint32_t srcW = 0, srcH = 0;
				if (ModuleArgs::ResolvedRenderSize(srcW, srcH))
				{
					const_cast<RenderWindow&>(window).SetModuleSourceRect(srcW, srcH);
				}
			}
			DebugLogFile("[m2ftg::K2] module_start -> 0x%X, module_main(params)=%p\n",
				startResult, reinterpret_cast<void*>(g_moduleMainFromParams));
			if (g_moduleMainFromParams != nullptr && g_moduleMainFromParams != g_moduleMain)
			{
				// The module has just told us where module_main really is. Trust that over the
				// pattern and say so loudly — a silent disagreement means the pattern needs work.
				DebugLogFile("[m2ftg::K2] WARNING: pattern gave %p but the module returned %p\n",
					reinterpret_cast<void*>(g_moduleMain),
					reinterpret_cast<void*>(g_moduleMainFromParams));
				g_moduleMain = g_moduleMainFromParams;
			}

			// Throw the second-board switch, AFTER module_start: state 0x10 of the module's own
			// mode machine zeroes this byte (along with the board index and the link-enabled
			// gate), so setting it earlier would simply be undone during bring-up.
			//
			// The gate lives in .data, which is already writable - no ScopedUnprotect needed, and
			// the ones taken during symbol resolution are long out of scope by here.
			if (g_twoBoardGate != nullptr && WantTwoBoardMode())
			{
				*g_twoBoardGate = 1;
				DebugLogFile("[m2ftg::K2] two-board mode ENABLED (gate %p = 1)\n",
					static_cast<void*>(g_twoBoardGate));
			}
			else if (WantTwoBoardMode())
			{
				DebugLogFile("[m2ftg::K2] -von-2board asked for, but this module has no two-board gate\n");
			}

			if (startResult == 0 && g_moduleMain != nullptr)
			{
				const uint32_t frameLimit = gGeneral.GetFrameLimit();
				uint32_t framesRun = 0;
				while (!window.IsShuttingDown())
				{
					if (!GameLoop(window)) break;
					if (frameLimit != 0 && ++framesRun >= frameLimit)
					{
						DebugLogFile("[m2ftg::K2] frame limit %u reached, shutting down\n", frameLimit);
						break;
					}
				}

				const int stopResult = module_stop(0, nullptr);
				DebugLogFile("[m2ftg::K2] module_stop -> 0x%X\n", stopResult);
			}
		}
	}
}
