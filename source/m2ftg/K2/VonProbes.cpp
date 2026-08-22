#include "K2Host.h"
#include "../../ModuleLoad.h"

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
#include "../Determinism.h"           // RomFrameCounterAddress / ReadEmulatedRam32
#include "../VirtualClock.h"          // the module's timebase, driven by frames not real time
#include "../Debug/HleHooks.h"              // the HLE trap table, disabled by repointing its handlers
#include "../CommBoard/CommBoard.h"   // the Model 2 comm board's DPRAM model, shared with MrLink
#include "../../cabinet/Cabinet.h"    // the shared cabinet front panel (pause/coin/assign/switches)
#include "../SystemSwitches.h"        // cabinet TEST / SERVICE on the emulated I/O board
#include "../../net/NetPlugin.h"      // net::Logf — the diagnostic sink both peers share
#include "../ModuleArgs.h"
#include "../DisplayModes.h"
#include "../../input/Input.h"
#include "../HostUI.h"
#include "../../imgui/imgui.h"   // the -von-modes overlay
#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../Utils/MemoryMgr.h"       // VP::Patch — protection-aware writes into module .text
#include "../../Utils/ScopedUnprotect.hpp"
#include "../../Utils/Trampoline.h"
#include "../../wil/resource.h"

#include "VonBoard.h"

namespace m2ftg
{
	namespace K2
	{
		using namespace pxd;

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

			void Sample(uint8_t* dllBase, int board)
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

			void SnapGlobals(uint8_t* dllBase)
			{
				const size_t n = (GLOB_HI - GLOB_LO) / 4;
				s_globFirst.resize(n);
				memcpy(s_globFirst.data(), dllBase + GLOB_LO, n * 4);
			}

			void ReportGlobals(uint8_t* dllBase)
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
			void Report(int board)
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


		void LogTwoBoardState(int frame, uint32_t texid)
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

			// Two hex chars per port byte plus the terminator - 0x11 ports is 35 bytes, and the
			// [32] this used to be overran the frame on the first -von-2probe run after the port
			// window was widened (caught by RTC as an int3 at frame 0, presenting as a crash).
			char io0[OmgRva::IO_PORTS_LEN * 2 + 1], io1[OmgRva::IO_PORTS_LEN * 2 + 1];
			for (size_t i = 0; i < OmgRva::IO_PORTS_LEN; ++i)
			{
				snprintf(io0 + i * 2, 3, "%02X", io(0)[i]);
				snprintf(io1 + i * 2, 3, "%02X", io(1)[i]);
			}
			// The control-mapping scheme the module actually latched, and the first two pairs it
			// copied out of the table. Valid data is {mask, code} pairs with small codes; an index
			// past the end of the five-entry table copies POINTERS (0x18045xxxx) instead, which is
			// what decoded every pad as nothing. See the assign[0][4] note in the frame loop.
			DebugLogFile("[2board] scheme=%u map=%08X,%08X,%08X,%08X\n",
				*reinterpret_cast<uint32_t*>(at(0x6917F8)),
				*reinterpret_cast<uint32_t*>(at(0x6916B0)), *reinterpret_cast<uint32_t*>(at(0x6916B4)),
				*reinterpret_cast<uint32_t*>(at(0x6916B8)), *reinterpret_cast<uint32_t*>(at(0x6916BC)));
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
			// The comm board as the ROM's link check sees it: the two registers, then the firmware
			// status bytes in BOTH banks (bit0 of the flag says which one the i960 is looking at).
			{
				// One buffer per argument, deliberately. A rotating static would be evaluated four
				// times before DebugLogFile ran and print the same two values twice.
				char st[4][OmgRva::COMM_STATUS_LEN * 2 + 1];
				const size_t bases[4] = {
					OmgRva::COMM_P1, OmgRva::COMM_P1 + OmgRva::COMM_BANK,
					OmgRva::COMM_P2, OmgRva::COMM_P2 + OmgRva::COMM_BANK,
				};
				for (size_t b = 0; b < 4; ++b)
				{
					for (size_t i = 0; i < OmgRva::COMM_STATUS_LEN; ++i)
					{
						snprintf(st[b] + i * 2, 3, "%02X", *at(bases[b] + i));
					}
				}
				DebugLogFile("[comm] B0 reset=%02X flag=%02X bank0=%s bank1=%s | "
					"B1 reset=%02X flag=%02X bank0=%s bank1=%s | brought-up=%u bootstate=%u\n",
					*at(OmgRva::COMM_P1_FLAG - 1), *at(OmgRva::COMM_P1_FLAG), st[0], st[1],
					*at(OmgRva::COMM_P2_FLAG - 1), *at(OmgRva::COMM_P2_FLAG), st[2], st[3],
					// The flag hook 0 sets at BlackOut+0x190, and the module's own boot state. Both
					// are downstream of Net_check RETURNING, so a board parked in the link check
					// never reaches either - which is the suspect for the blank screen.
					*at(0x6910DF), *reinterpret_cast<uint32_t*>(at(0x7ADCA8)));
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
	}
}

namespace m2ftg
{
	namespace K2
	{
		// See the WantModeOverlay note in VonBoard.h for why this is an overlay and not a page.
		void DrawModeOverlay(bool switchesSuppressed)
		{
			auto read = [](uint32_t address) -> long long
				{
					uint32_t value = 0;
					return ReadEmulatedRam32(address, value) ? static_cast<long long>(value) : -1;
				};

			ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowBgAlpha(0.75f);
			if (ImGui::Begin("Virtual On board state", nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing
				| ImGuiWindowFlags_NoNav))
			{
				// -1 means the read failed, which before the board has booted is the normal
				// answer rather than an error - printed as "--" so it is not mistaken for a mode.
				auto mode = [](long long v, char* out, size_t bytes)
					{
						if (v < 0) snprintf(out, bytes, "--");
						else snprintf(out, bytes, "%lld", v);
					};
				char main[16], sub[16], versus[16];
				mode(read(OmgRva::SYM_MAINMODE), main, sizeof(main));
				mode(read(OmgRva::SYM_SUBMODE), sub, sizeof(sub));
				mode(read(OmgRva::SYM_VERSUSMODE), versus, sizeof(versus));
				ImGui::Text("MainMode %s   SubMode %s   VersusMode %s", main, sub, versus);

				// The switch lines as YAMP is driving them THIS frame. Either player's binding
				// closes the one cabinet switch, which is why both are tested.
				const bool test = Input::ActionDown(0, Input::Action_Test)
					|| Input::ActionDown(1, Input::Action_Test);
				const bool service = Input::ActionDown(0, Input::Action_Service)
					|| Input::ActionDown(1, Input::Action_Service);
				const ImVec4 lit(0.4f, 1.0f, 0.4f, 1.0f);
				const ImVec4 dim(0.5f, 0.5f, 0.5f, 1.0f);
				ImGui::Text("Switches:"); ImGui::SameLine();
				ImGui::TextColored(test ? lit : dim, "TEST"); ImGui::SameLine();
				ImGui::TextColored(service ? lit : dim, "SERVICE");
				if (switchesSuppressed)
				{
					ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
						"(suppressed - close the pause menu)");
				}
			}
			ImGui::End();
		}
	}
}
