#include "DebugWindows.h"

#include "../ModuleBuild.h"
#include "../../YAMPGeneral.h"
#include "../../DebugLog.h"
#include "../ELF/ElfRom.h"
#include "../../net/NetPlugin.h"

#include "../../imgui/imgui.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <cstdio>
#include <cstring>

#include "DwGame.h"

// The retail StF DLL still carries the descriptor data of M2's "dw" debug-window framework
// (the same system visible in M2's Daytona USA HD port: DEBUG MENU / MON_I960 / PERFORMANCE
// windows). The window manager itself was stripped from the DLL, but the full menu tree —
// window headers, item labels, item types, bound variables and the action handlers — survives
// and is verified present exactly once (signature scan over the whole image). YAMP walks that
// tree at runtime and renders it with ImGui; every action goes through the DLL's own code and
// every toggle/slider writes the DLL's own variable. Nothing here reimplements DLL logic.
//
// Descriptor layout (verified against the DLL's boot-time menu init at RVA +0x4B070):
//   window: { const char* title; u64 itemCount; const item* items; u64 type (0xA root / 9 sub) }
//   item:   { const char* label; void* action; u64 type; void* var; u64 aux }
// Item types seen in the tree: 0xA = action (call fn), 9 = submenu (action = window*),
// 7 = byte toggle (var), 1 = numeric (var, aux = packed {u32 min, u32 max}, optional fn).

namespace
{
	using namespace m2ftg::dwdbg;


	constexpr int MAX_FRAMES = 64;
	constexpr int MAX_HISTORY = 12;

	struct StackSample
	{
		uint32_t topIp;
		int frames;
		uint32_t hits;
	};

	static StackSample s_history[MAX_HISTORY];
	static int s_historyCount = 0;
	static int s_totalSamples = 0;

	// Keeps the distinct stacks seen, most-recently-first, counting repeats.
	static void RecordStackSample(const uint32_t* chain, int frameCount)
	{
		if (frameCount <= 0)
		{
			return;
		}
		s_totalSamples++;

		for (int i = 0; i < s_historyCount; i++)
		{
			if (s_history[i].topIp == chain[0])
			{
				s_history[i].hits++;
				return;
			}
		}

		if (s_historyCount < MAX_HISTORY)
		{
			s_historyCount++;
		}
		for (int i = s_historyCount - 1; i > 0; i--)
		{
			s_history[i] = s_history[i - 1];
		}
		s_history[0] = { chain[0], frameCount, 1 };
	}

	// The live 960STAT call stack. This is the output the DLL's own CALL STACK handler computes
	// and then throws away: same context fields, same frame-alignment mask, same [FP+8]=return
	// IP / [FP+0]=previous frame chain, same memory accessors, same symbol table.
	// Aggregates the sampled IP buckets by symbol and lists the hottest. Rebuilt a few times a
	// second rather than every frame - it walks 16384 buckets and the numbers do not move fast
	// enough to be worth more.
	static void DrawProfilePane(uint8_t* base, bool booted)
	{
		namespace Profile = m2ftg::I960Profile;

		ImGui::Separator();
		ImGui::TextUnformatted("PROFILE (sampled inside the frame)");

		if (!booted)
		{
			ImGui::TextDisabled("i960 not running.");
			return;
		}

		struct Entry { std::string name; uint32_t address; uint64_t samples; };
		static std::vector<Entry> s_entries;
		static uint64_t s_total = 0;
		static double s_lastRebuild = 0.0;

		const double now = ImGui::GetTime();
		if (now - s_lastRebuild >= 0.4)
		{
			s_lastRebuild = now;
			s_entries.clear();
			s_total = Profile::TotalSamples();

			std::unordered_map<std::string, size_t> seen;
			for (size_t i = 0; i < Profile::BUCKET_COUNT; i++)
			{
				const uint32_t count = Profile::Bucket(i);
				if (count == 0)
				{
					continue;
				}
				const uint32_t address = static_cast<uint32_t>(i) << Profile::BUCKET_SHIFT;
				char symbol[128];
				SymbolizeI960(base, address, symbol, sizeof(symbol));
				// Fold "name+0x40" back to "name" so a function's buckets add up together.
				std::string key(symbol);
				if (const size_t plus = key.rfind("+0x"); plus != std::string::npos)
				{
					key.erase(plus);
				}

				const auto it = seen.find(key);
				if (it == seen.end())
				{
					seen.emplace(key, s_entries.size());
					s_entries.push_back({ std::move(key), address, count });
				}
				else
				{
					s_entries[it->second].samples += count;
				}
			}

			std::sort(s_entries.begin(), s_entries.end(),
				[](const Entry& a, const Entry& b) { return a.samples > b.samples; });
		}

		if (s_total == 0)
		{
			ImGui::TextDisabled("No samples yet.");
			return;
		}

		ImGui::Text("%llu samples", static_cast<unsigned long long>(s_total));
		ImGui::SameLine();
		if (ImGui::SmallButton("Reset profile"))
		{
			Profile::Reset();
			s_entries.clear();
			s_total = 0;
		}

		const size_t shown = s_entries.size() < 14 ? s_entries.size() : 14;
		for (size_t i = 0; i < shown; i++)
		{
			const Entry& entry = s_entries[i];
			ImGui::Text("%5.1f%%  0x%06X  %s",
				100.0 * static_cast<double>(entry.samples) / static_cast<double>(s_total),
				entry.address, entry.name.c_str());
		}
	}

	// Rebuilds the absolute i960 IP from the bank-relative one, the way the DLL's own call
	// handler does when it stores a return address into r2.
	static bool CurrentI960Ip(uint8_t* ctx, uint32_t& out)
	{
		const void* liveBank = *reinterpret_cast<void* const*>(ctx + CTX_CODE_BASE);
		if (liveBank == nullptr)
		{
			return false;
		}
		const uint32_t ip = *reinterpret_cast<const uint32_t*>(ctx + CTX_IP);
		if (liveBank == *reinterpret_cast<void* const*>(ctx + CTX_BANK_ROM2))
		{
			out = ip + 0x200000;
		}
		else if (liveBank == *reinterpret_cast<void* const*>(ctx + CTX_BANK_RAM))
		{
			out = ip + 0x500000;
		}
		else
		{
			// Includes the CTX_BANK_ROM0 case, which needs no adjustment.
			out = ip;
		}
		return true;
	}

	// The 960STAT DISASM item points at the generic stub (+0x4C780, `xor eax,eax; ret`) in the
	// retail DLL, so there is no handler left to call. What it used to do survives in the PS3
	// build of the same emulator, whose DISASM handler symbolises the running IP and then drops
	// the string; this is that line, kept.
	static void DrawDisasmPane(uint8_t* base, bool booted)
	{
		ImGui::Separator();
		ImGui::TextUnformatted("DISASM (live IP)");

		uint8_t* ctx = booted ? *reinterpret_cast<uint8_t**>(base + DW_STF.rvaCpuCtxPtr) : nullptr;
		uint32_t ip = 0;
		if (ctx == nullptr || !CurrentI960Ip(ctx, ip))
		{
			ImGui::TextDisabled("i960 not running.");
			return;
		}

		char symbol[128];
		SymbolizeI960(base, ip, symbol, sizeof(symbol));
		ImGui::Text("IP 0x%08X  %s", ip, symbol);
		ImGui::Text("FP 0x%08X   SP 0x%08X   PFP 0x%08X",
			*reinterpret_cast<const uint32_t*>(ctx + CTX_REG_G15_FP) & I960_FRAME_ALIGN_MASK,
			*reinterpret_cast<const uint32_t*>(ctx + CTX_REG_R0_PFP + 4),
			*reinterpret_cast<const uint32_t*>(ctx + CTX_REG_R0_PFP) & I960_FRAME_ALIGN_MASK);
	}

	static void DrawCallStackPane(uint8_t* base, bool booted)
	{
		ImGui::Separator();
		ImGui::TextUnformatted("CALL STACK (live)");

		if (!booted)
		{
			ImGui::TextDisabled("i960 not running.");
			return;
		}

		uint8_t* ctx = *reinterpret_cast<uint8_t**>(base + DW_STF.rvaCpuCtxPtr);
		if (ctx == nullptr)
		{
			ImGui::TextDisabled("No CPU context.");
			return;
		}

		const int depth = *reinterpret_cast<const int*>(ctx + CTX_CALL_DEPTH);
		const uint32_t livePtr = *reinterpret_cast<const uint32_t*>(ctx + CTX_REG_G15_FP);
		uint32_t framePtr = *reinterpret_cast<const uint32_t*>(ctx + CTX_REG_R0_PFP);
		ImGui::Text("FP 0x%08X    (break-state depth %d)",
			livePtr & I960_FRAME_ALIGN_MASK, depth);

		uint32_t chain[MAX_FRAMES];
		int frameCount = 0;

		// The DLL's own walker starts at r0, so its first entry is already the return address
		// into the caller's caller: it never names the running procedure or its immediate
		// caller. Both are live in registers rather than in the frame save area, so seed the
		// chain with them before walking memory.
		uint32_t liveIp = 0;
		if (CurrentI960Ip(ctx, liveIp) && liveIp != 0 && liveIp < m2ftg::ElfRom::PROGRAM_ROM_SIZE)
		{
			chain[frameCount++] = liveIp;
		}
		const uint32_t returnIntoCaller = *reinterpret_cast<const uint32_t*>(ctx + CTX_REG_R2_RIP);
		if (returnIntoCaller != 0 && returnIntoCaller < m2ftg::ElfRom::PROGRAM_ROM_SIZE &&
			(returnIntoCaller & 3) == 0)
		{
			chain[frameCount++] = returnIntoCaller;
		}

		if (framePtr == 0 && frameCount == 0)
		{
			ImGui::TextDisabled("No frame pointer.");
			return;
		}

		// Walk until the chain terminates on a null previous-frame pointer. The DLL bounds the
		// loop with its frame count instead, but that counter is part of the break state.
		for (int i = frameCount; i < MAX_FRAMES && framePtr != 0; i++)
		{
			framePtr &= I960_FRAME_ALIGN_MASK;

			uint32_t returnIp = 0;
			uint32_t previousFramePtr = 0;
			if (!ReadEmulated32(base, framePtr + 8, returnIp) ||
				!ReadEmulated32(base, framePtr, previousFramePtr))
			{
				break;
			}

			// A return IP is a code address: inside the program ROM and instruction-aligned.
			// Anything else means this is not a live call frame - which is the normal state
			// once the module yields per frame, because the board is parked in the yield
			// handler rather than mid-call. Without this the walk happily symbolises string
			// data and stack garbage (0x30302020 is ASCII "  00") into plausible-looking
			// frames, which is worse than showing nothing.
			if (returnIp == 0 || returnIp >= m2ftg::ElfRom::PROGRAM_ROM_SIZE || (returnIp & 3) != 0)
			{
				break;
			}

			chain[frameCount++] = returnIp;

			if (previousFramePtr == 0 || (previousFramePtr & I960_FRAME_ALIGN_MASK) == framePtr)
			{
				break;
			}
			framePtr = previousFramePtr;
		}

		if (frameCount == 0)
		{
			ImGui::TextDisabled("No live call frame (board parked between frames).");
			ImGui::TextDisabled("Use the profile below - it samples from inside the frame.");
			return;
		}

		for (int i = 0; i < frameCount; i++)
		{
			char symbol[128];
			SymbolizeI960(base, chain[i], symbol, sizeof(symbol));
			ImGui::Text("#%02d  0x%08X  %s", i, chain[i], symbol);
		}

		RecordStackSample(chain, frameCount);

		// The emulated CPU runs in bursts on the module's own thread, so a sample taken from
		// the render thread nearly always lands on the same frame-yield point — the pane looks
		// frozen even though the machine is busy. The history keeps the distinct stacks caught
		// mid-burst, which is where the interesting code shows up.
		ImGui::Separator();
		ImGui::Text("Distinct stacks seen (%d samples)", s_totalSamples);
		for (int i = 0; i < s_historyCount; i++)
		{
			const StackSample& sample = s_history[i];
			char symbol[128];
			SymbolizeI960(base, sample.topIp, symbol, sizeof(symbol));
			ImGui::Text("%6u x  0x%08X  %s%s", sample.hits, sample.topIp, symbol,
				sample.frames > 1 ? "  ..." : "");
		}
		if (ImGui::Button("Clear history"))
		{
			s_historyCount = 0;
			s_totalSamples = 0;
		}
	}

	static const char* NoteFor(const DwMenuItem& item, uint8_t* base)
	{
		const uintptr_t actionRva = item.action ? reinterpret_cast<uint8_t*>(item.action) - base : 0;
		if (auto it = RVA_NOTES.find(actionRva); item.action && it != RVA_NOTES.end())
		{
			return it->second;
		}
		const uintptr_t varRva = item.var ? reinterpret_cast<uint8_t*>(item.var) - base : 0;
		if (auto it = RVA_NOTES.find(varRva); item.var && it != RVA_NOTES.end())
		{
			return it->second;
		}
		return nullptr;
	}

	static void DrawItems(const DwMenuWindow* window, uint8_t* base, bool booted);

	// Submenu windows toggled open from their parent, keyed by window descriptor.
	static std::unordered_map<const DwMenuWindow*, bool> s_openWindows;

	// The vendored ImGui predates BeginDisabled/EndDisabled; mirror ButtonToggleable's dimming.
	static void PushDim(bool dim)
	{
		if (dim)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
		}
	}

	static void PopDim(bool dim)
	{
		if (dim)
		{
			ImGui::PopStyleVar();
		}
	}

	static void DrawItem(const DwMenuItem& item, uint8_t* base, bool booted)
	{
		ImGui::PushID(&item);

		switch (item.type)
		{
		case ITEM_SUBMENU:
		{
			const auto* sub = static_cast<const DwMenuWindow*>(item.action);
			bool& open = s_openWindows[sub];
			if (ImGui::Button(item.label, { 160, 0 }))
			{
				open = !open;
			}
			break;
		}

		case ITEM_ACTION:
		{
			PushDim(!booted);
			if (ImGui::Button(item.label, { 160, 0 }) && booted && item.action)
			{
				InvokeDwAction(item.action);
			}
			PopDim(!booted);
			break;
		}

		case ITEM_TOGGLE8:
		{
			auto* flag = static_cast<uint8_t*>(item.var);
			bool value = flag && *flag != 0;
			if (ImGui::Checkbox(item.label, &value) && flag)
			{
				*flag = value ? 1 : 0;
			}
			break;
		}

		case ITEM_NUMERIC:
		{
			if (item.action)
			{
				// Numeric entry with a DLL-side mutator (e.g. DEKU): go through the DLL's own
				// handler and just display the byte it maintains.
				auto* value = static_cast<uint8_t*>(item.var);
				PushDim(!booted);
				if (ImGui::Button(item.label, { 160, 0 }) && booted)
				{
					InvokeDwAction(item.action);
				}
				PopDim(!booted);
				if (value)
				{
					ImGui::SameLine();
					ImGui::Text("= %u", *value);
				}
			}
			else if (item.var)
			{
				// Plain bound variable with a packed {min,max} range (e.g. STAGE, dword: the
				// DLL reads it with a 32-bit load).
				const int minVal = static_cast<int>(item.aux & 0xFFFFFFFF);
				const int maxVal = static_cast<int>(item.aux >> 32);
				auto* value = static_cast<int*>(item.var);
				int edit = *value;
				ImGui::SetNextItemWidth(160);
				if (maxVal > minVal)
				{
					if (ImGui::SliderInt(item.label, &edit, minVal, maxVal))
					{
						*value = edit;
					}
				}
				else if (ImGui::InputInt(item.label, &edit))
				{
					*value = edit;
				}
			}
			break;
		}

		default:
			ImGui::TextDisabled("%s (type 0x%llX)", item.label, item.type);
			break;
		}

		if (const char* note = NoteFor(item, base); note && ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", note);
		}

		ImGui::PopID();
	}

	static void DrawItems(const DwMenuWindow* window, uint8_t* base, bool booted)
	{
		for (uint64_t i = 0; i < window->itemCount; i++)
		{
			DrawItem(window->items[i], base, booted);
		}
	}
}

// Backing store for the i960 sampling profiler declared in DebugWindows.h. Written from the
// emulator thread (RamExecFetch::FetchExec) and read from the UI thread; both are plain counters
// where a torn or slightly stale read costs nothing, so this needs no synchronisation - and the
// fetch path is far too hot to afford any.
uint32_t m2ftg::I960Profile::detail::g_buckets[m2ftg::I960Profile::BUCKET_COUNT] = {};
uint32_t m2ftg::I960Profile::detail::g_counter = 0;
uint64_t m2ftg::I960Profile::detail::g_samples = 0;

void m2ftg::I960Profile::Reset()
{
	for (size_t i = 0; i < BUCKET_COUNT; i++)
	{
		detail::g_buckets[i] = 0;
	}
	detail::g_samples = 0;
}

uint64_t m2ftg::I960Profile::TotalSamples()
{
	return detail::g_samples;
}

uint32_t m2ftg::I960Profile::Bucket(size_t index)
{
	return index < BUCKET_COUNT ? detail::g_buckets[index] : 0;
}


void m2ftg::DrawDebugWindows()
{
	if (gGeneral.GetGameId() != YAMPGeneral::GameId::StF)
	{
		return;
	}
	const YAMPSettings* settings = gGeneral.GetSettings();
	if (settings == nullptr || !settings->m_stfShowDebugFeatures)
	{
		return;
	}
	uint8_t* base = ModuleBase();
	if (base == nullptr)
	{
		return;
	}

	const auto* root = reinterpret_cast<const DwMenuWindow*>(base + RootWindowRva());
	const uint32_t bootState = *reinterpret_cast<const uint32_t*>(base + DW_STF.rvaBootState);
	const bool booted = bootState == 2;

	ImGui::SetNextWindowPos({ 10.0f, 40.0f }, ImGuiCond_FirstUseEver);
	if (ImGui::Begin(root->title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (!booted)
		{
			ImGui::TextColored({ 1.0f, 1.0f, 0.0f, 1.0f }, "Board not booted yet (state %u)", bootState);
		}
		DrawItems(root, base, booted);

		ImGui::Separator();
		ImGui::TextDisabled("run-state: %u (no reader in retail DLL)",
			*reinterpret_cast<const uint32_t*>(base + RunStateRva()));
	}
	ImGui::End();

	for (auto& [sub, open] : s_openWindows)
	{
		if (!open)
		{
			continue;
		}
		if (ImGui::Begin(sub->title, &open, ImGuiWindowFlags_AlwaysAutoResize))
		{
			DrawItems(sub, base, booted);
			if (strcmp(sub->title, "960STAT") == 0)
			{
				DrawDisasmPane(base, booted);
				DrawCallStackPane(base, booted);
				DrawProfilePane(base, booted);
			}
		}
		ImGui::End();
	}
}


void m2ftg::UpdateGameDebugFlag()
{
	if (gGeneral.GetGameId() != YAMPGeneral::GameId::StF)
	{
		return;
	}
	const YAMPSettings* settings = gGeneral.GetSettings();
	// Forced OFF during netplay, for the same reason the HLE mask is ignored there: this writes
	// EMULATED RAM (the dword at 0x508000), so one machine having it set and the other not means
	// the two are simulating different memory. Both peers run with it clear, which is a state they
	// agree on without having to exchange anything.
	const bool enable = settings != nullptr && settings->m_stfGameDebugFlag
	                 && !net::SessionInProgress();

	uint8_t* base = ModuleBase();
	if (base == nullptr)
	{
		return;
	}

	// The flag's off-state bits are captured from RAM the first booted frame, before any
	// write of ours; "set" then means those bits differ from that baseline. This keeps the
	// XOR toggle idempotent without hardcoding what the ROM initializes the dword to.
	static bool baselineKnown = false;
	static uint32_t baselineBits = 0;
	static bool applied = false;
	if (*reinterpret_cast<const uint32_t*>(base + DW_STF.rvaBootState) != 2)
	{
		baselineKnown = false;
		applied = false;
		return;
	}

	uint32_t value = 0;
	if (!ReadEmulated32(base, DEBUG_FLAG_ADDRESS, value))
	{
		return;
	}
	if (!baselineKnown)
	{
		baselineKnown = true;
		baselineBits = value & DEBUG_FLAG_XOR;
	}

	// Enforce every frame rather than toggling once: the ROM's own code initializes this
	// RAM after boot and could rewrite it later, which would silently undo a one-shot XOR.
	const bool flagSet = (value & DEBUG_FLAG_XOR) != baselineBits;
	if (enable && !flagSet)
	{
		applied = WriteEmulated32(base, DEBUG_FLAG_ADDRESS, value ^ DEBUG_FLAG_XOR);
	}
	else if (!enable && flagSet && applied)
	{
		// Only undo a state this code set itself; leave the game's own writes alone.
		if (WriteEmulated32(base, DEBUG_FLAG_ADDRESS, value ^ DEBUG_FLAG_XOR))
		{
			applied = false;
		}
	}
}

