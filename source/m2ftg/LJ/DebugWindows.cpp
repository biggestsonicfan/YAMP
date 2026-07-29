#include "DebugWindows.h"

#include "../../YAMPGeneral.h"
#include "../ElfRom.h"

#include "../../imgui/imgui.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cstring>

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
	struct DwMenuItem
	{
		const char* label;
		void* action;
		uint64_t type;
		void* var;
		uint64_t aux;
	};
	static_assert(sizeof(DwMenuItem) == 0x28, "dw item record is 0x28 bytes in the DLL");

	struct DwMenuWindow
	{
		const char* title;
		uint64_t itemCount;
		const DwMenuItem* items;
		uint64_t type;
	};
	static_assert(sizeof(DwMenuWindow) == 0x20, "dw window header is 0x20 bytes in the DLL");

	constexpr uint64_t ITEM_ACTION = 0xA;
	constexpr uint64_t ITEM_SUBMENU = 9;
	constexpr uint64_t ITEM_TOGGLE8 = 7;
	constexpr uint64_t ITEM_NUMERIC = 1;

	// RVAs inside stf-pxd-w64-d3d12_retail.dll (fixed preferred base 0x180000000).
	constexpr uintptr_t RVA_ROOT_WINDOW = 0x1E8850;   // "DEBUG MENU" window header
	constexpr uintptr_t RVA_BOOT_STATE = 0x6B9300;    // ROM/CPU boot phase; 2 = board booted
	constexpr uintptr_t RVA_RUN_STATE = 0x6C19E0;     // written by STEP/GO/RESET handlers
	constexpr uintptr_t RVA_STUB_RET0 = 0x4C780;      // `xor eax,eax; ret` — stripped handlers

	// i960 machine state, used by the 960STAT call-stack pane. All of this is the DLL's own
	// data and code; the only piece missing from the retail DLL is the text output, which the
	// stripped CALL STACK handler (+0x4C6C0) discarded — it still performs the walk correctly.
	constexpr uintptr_t RVA_CPU_CTX_PTR = 0x58A960;   // -> i960 context (set by the CPU init)
	// The DLL's walker reads its frame pointer from +0x58 and a frame count from +0x48, but
	// retail never populates either (verified by polling the live context: both stay 0 while
	// the game runs) — they belong to the stripped break/debugger state. The frame pointer the
	// running CPU actually maintains is +0x5C, and walking it with the DLL's own algorithm
	// produces a correct, cleanly terminating chain. Prefer it, fall back to the DLL's field.
	constexpr uintptr_t CTX_CALL_DEPTH = 0x48;        // int32: frame count (break-state only)
	constexpr uintptr_t CTX_FRAME_PTR = 0x58;         // uint32: frame pointer (break-state only)
	constexpr uintptr_t CTX_LIVE_FRAME_PTR = 0x5C;    // uint32: frame pointer of the running CPU
	// Emulated memory map: 64 records x 0x70, each holding {read,write} pairs by access size.
	// Slot +0x20 is the 32-bit read: void read(void* out, uint32_t addr); slot +0x28 is its
	// paired 32-bit write: void write(uint32_t addr, const void* src). Unmapped regions
	// point at a bare `ret` stub, so an out-of-range address is inert rather than fatal.
	constexpr uintptr_t RVA_MEMMAP_TBL = 0x172660;
	constexpr size_t MEMMAP_RECORD_SIZE = 0x70;
	constexpr size_t MEMMAP_READ32 = 0x20;
	constexpr size_t MEMMAP_WRITE32 = 0x28;
	constexpr size_t MEMMAP_RECORD_COUNT = 64;
	// The game's own debug flag lives in emulated RAM: XORing this dword with the mask
	// flips it. Surfaced as the "Set the game's debug flag" debug setting.
	constexpr uint32_t DEBUG_FLAG_ADDRESS = 0x508000;
	constexpr uint32_t DEBUG_FLAG_XOR = 0x24;
	// ROM symbol table: 800 records of {uint64_t addr; const char* name}, sorted ascending by
	// addr, starting at 0x1742D0. Earlier passes misread it as {name, addr} records starting
	// 8 bytes later at 0x1742D8, which pairs every name with the NEXT function's address and
	// then needs bogus "corrections". Ground truth for the record phase: the first record is
	// {0xB0, "start_ip"}, and 0xB0 is exactly the start IP stored in the ROM's i960 initial
	// memory image (boot word 3 of rom_code1.bin), while the misread pairing gives start_ip
	// 0x5A8, which matches nothing.
	constexpr uintptr_t RVA_SYMBOL_TBL = 0x1742D0;
	constexpr size_t SYMBOL_COUNT = 800;
	// The DLL's own walk masks the frame pointer to the i960's 64-byte frame alignment.
	constexpr uint32_t I960_FRAME_ALIGN_MASK = 0xFFFFFFC0;

	struct I960Symbol
	{
		uint64_t address;
		const char* name;
	};
	static_assert(sizeof(I960Symbol) == 16, "symbol record is 16 bytes in the DLL");

	// Reverse-engineering notes for known tree entries, keyed by the RVA of the item's action
	// (or bound variable when it has no action). Display-only.
	static const std::unordered_map<uintptr_t, const char*> RVA_NOTES =
	{
		{ 0x4C780,  "Stub in the retail DLL: the original window implementation was stripped, the handler just returns 0." },
		{ 0x4C820,  "Writes debugger run-state 0 (single step). The retail DLL no longer reads the run-state." },
		{ 0x4C830,  "Writes debugger run-state 2 (run). The retail DLL no longer reads the run-state." },
		{ 0x4C840,  "Writes run-state 0, then re-runs the DLL's own i960 CPU/board init. This is a REAL reset." },
		{ 0x4C6C0,  "Walks the emulated i960 call chain via the DLL's memory-map accessors. Its text output was stripped, so it returns 0." },
		{ 0x4C790,  "DLL handler: toggles the DEKU byte flag." },
		{ 0x4C7A0,  "Pokes the emulated RAM: sets 1P health word to 1." },
		{ 0x4C7C0,  "Pokes the emulated RAM: sets 1P health word to 150 (max)." },
		{ 0x4C7E0,  "Pokes the emulated RAM: sets 2P health word to 1." },
		{ 0x4C800,  "Pokes the emulated RAM: sets 2P health word to 150 (max)." },
		{ 0x58A0CE, "Byte flag read by the DLL's live per-frame perf-stat collectors." },
		{ 0x1E50D4, "Stage-select variable, read by live game logic (including module_main)." },
	};

	static uint8_t* ModuleBase()
	{
		// The DLL links with a fixed preferred base and no dynamic relocation, but resolve the
		// real base anyway instead of assuming 0x180000000.
		return reinterpret_cast<uint8_t*>(GetModuleHandleW(L"stf-pxd-w64-d3d12_retail.dll"));
	}

	// Calls a handler inside the game DLL. The handlers are tiny and take no arguments (they
	// operate on DLL globals), but some dereference emulated-machine pointers that only exist
	// once the board has booted — keep a structured-exception net around the call regardless.
	static unsigned long long InvokeDwAction(void* fn) noexcept
	{
		__try
		{
			return reinterpret_cast<unsigned long long(*)()>(fn)();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return ~0ull;
		}
	}

	// Reads 32 bits of emulated i960 memory through the DLL's own memory-map dispatch, using
	// the same region-index formula as the DLL's call-stack walker.
	static bool ReadEmulated32(uint8_t* base, uint32_t address, uint32_t& out) noexcept
	{
		const uint32_t index = address < 0x3000000 ? (address >> 20) : ((address >> 28) + 0x30);
		if (index >= MEMMAP_RECORD_COUNT)
		{
			return false;
		}
		void* reader = *reinterpret_cast<void**>(
			base + RVA_MEMMAP_TBL + index * MEMMAP_RECORD_SIZE + MEMMAP_READ32);
		if (reader == nullptr)
		{
			return false;
		}
		out = 0;
		__try
		{
			reinterpret_cast<void(*)(void*, uint32_t)>(reader)(&out, address);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
		return true;
	}

	// Writes 32 bits of emulated i960 memory through the same memory-map dispatch, using the
	// read accessor's pair slot in the region record.
	static bool WriteEmulated32(uint8_t* base, uint32_t address, uint32_t value) noexcept
	{
		const uint32_t index = address < 0x3000000 ? (address >> 20) : ((address >> 28) + 0x30);
		if (index >= MEMMAP_RECORD_COUNT)
		{
			return false;
		}
		void* writer = *reinterpret_cast<void**>(
			base + RVA_MEMMAP_TBL + index * MEMMAP_RECORD_SIZE + MEMMAP_WRITE32);
		if (writer == nullptr)
		{
			return false;
		}
		__try
		{
			reinterpret_cast<void(*)(uint32_t, const void*)>(writer)(address, &value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
		return true;
	}

	// Resolves an emulated address to the DLL's own ROM symbol table: the greatest symbol at or
	// below the address, plus a byte offset.
	static void SymbolizeI960(uint8_t* base, uint32_t address, char* buffer, size_t bufferSize)
	{
		// A homebrew program ROM makes the DLL's built-in table actively misleading - it would
		// label the running code with whatever Sonic the Fighters function happens to sit at
		// that address. When game.elf supplied the ROM, its own symbols are the truth.
		if (m2ftg::ElfRom::IsLoaded())
		{
			const char* name = nullptr;
			uint32_t offset = 0;
			if (!m2ftg::ElfRom::SymbolizeAddress(address, name, offset))
			{
				_snprintf_s(buffer, bufferSize, _TRUNCATE, "(no symbol)");
			}
			else if (offset != 0)
			{
				_snprintf_s(buffer, bufferSize, _TRUNCATE, "%s+0x%X", name, offset);
			}
			else
			{
				_snprintf_s(buffer, bufferSize, _TRUNCATE, "%s", name);
			}
			return;
		}

		const auto* symbols = reinterpret_cast<const I960Symbol*>(base + RVA_SYMBOL_TBL);
		size_t low = 0, high = SYMBOL_COUNT;
		while (low < high)
		{
			const size_t mid = low + (high - low) / 2;
			if (symbols[mid].address <= address)
			{
				low = mid + 1;
			}
			else
			{
				high = mid;
			}
		}
		if (low == 0)
		{
			_snprintf_s(buffer, bufferSize, _TRUNCATE, "(no symbol)");
			return;
		}
		const I960Symbol& sym = symbols[low - 1];
		const uint64_t offset = address - sym.address;
		if (offset != 0)
		{
			_snprintf_s(buffer, bufferSize, _TRUNCATE, "%s+0x%llX", sym.name, offset);
		}
		else
		{
			_snprintf_s(buffer, bufferSize, _TRUNCATE, "%s", sym.name);
		}
	}

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

	static void DrawCallStackPane(uint8_t* base, bool booted)
	{
		ImGui::Separator();
		ImGui::TextUnformatted("CALL STACK (live)");

		if (!booted)
		{
			ImGui::TextDisabled("i960 not running.");
			return;
		}

		uint8_t* ctx = *reinterpret_cast<uint8_t**>(base + RVA_CPU_CTX_PTR);
		if (ctx == nullptr)
		{
			ImGui::TextDisabled("No CPU context.");
			return;
		}

		const int depth = *reinterpret_cast<const int*>(ctx + CTX_CALL_DEPTH);
		uint32_t framePtr = *reinterpret_cast<const uint32_t*>(ctx + CTX_LIVE_FRAME_PTR);
		if (framePtr == 0)
		{
			framePtr = *reinterpret_cast<const uint32_t*>(ctx + CTX_FRAME_PTR);
		}
		ImGui::Text("FP 0x%08X    (break-state depth %d)", framePtr, depth);

		if (framePtr == 0)
		{
			ImGui::TextDisabled("No frame pointer.");
			return;
		}

		// Walk until the chain terminates on a null previous-frame pointer. The DLL bounds the
		// loop with its frame count instead, but that counter is part of the break state.
		uint32_t chain[MAX_FRAMES];
		int frameCount = 0;
		for (int i = 0; i < MAX_FRAMES; i++)
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

	const auto* root = reinterpret_cast<const DwMenuWindow*>(base + RVA_ROOT_WINDOW);
	const uint32_t bootState = *reinterpret_cast<const uint32_t*>(base + RVA_BOOT_STATE);
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
			*reinterpret_cast<const uint32_t*>(base + RVA_RUN_STATE));
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
	const bool enable = settings != nullptr && settings->m_stfGameDebugFlag;

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
	if (*reinterpret_cast<const uint32_t*>(base + RVA_BOOT_STATE) != 2)
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
