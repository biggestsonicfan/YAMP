#include "DebugWindows.h"

#include "ModuleBuild.h"
#include "../YAMPGeneral.h"
#include "../DebugLog.h"
#include "ELF/ElfRom.h"
#include "../net/NetPlugin.h"

#include "../imgui/imgui.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
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
	//
	// The four below drive the DEBUG MENU panes, which are StF-only: their window tree, item
	// labels and RVA_NOTES are all StF's. The addresses the NETPLAY DETERMINISM helpers need -
	// boot state, CPU context, memory map, RESET, the RNG holder and the texture-budget
	// handler - are per-game and live in the DwGame table further down, because Fighting
	// Vipers needs every one of them and has different values for all of them.
	// Per module build, like everything else here. The Gaiden root window sits 0x20 ahead of that
	// build's HLE table exactly as Lost Judgment's does — the module's own installer names it
	// (PTR_s_DEBUG_MENU_1801f31a0), which is as direct a confirmation as this gets. The run-state
	// word is display-only ("no reader in retail DLL").
	constexpr uintptr_t RVA_ROOT_WINDOW_LJ = 0x1E8850;   // "DEBUG MENU" window header
	constexpr uintptr_t RVA_ROOT_WINDOW_GAIDEN = 0x1F31A0;
	constexpr uintptr_t RVA_RUN_STATE_LJ = 0x6C19E0;     // written by STEP/GO/RESET handlers
	constexpr uintptr_t RVA_RUN_STATE_GAIDEN = 0x6CD3A0;
	inline uintptr_t RootWindowRva()
	{
		return m2ftg::IsGaidenBuild() ? RVA_ROOT_WINDOW_GAIDEN : RVA_ROOT_WINDOW_LJ;
	}
	inline uintptr_t RunStateRva()
	{
		return m2ftg::IsGaidenBuild() ? RVA_RUN_STATE_GAIDEN : RVA_RUN_STATE_LJ;
	}
	constexpr uintptr_t RVA_STUB_RET0 = 0x4C780;      // `xor eax,eax; ret` — stripped handlers

	// i960 machine state, used by the 960STAT call-stack and DISASM panes. All of this is the
	// DLL's own data and code; the only piece missing from the retail DLL is the text output,
	// which the stripped CALL STACK handler (+0x4C6C0) discarded — it still performs the walk
	// correctly.
	// (StF's value; the live one comes from DwGame::rvaCpuCtxPtr - FV's is 0x58CF60.)
	// The emulated register file is embedded in the context at +0x58 as 64 consecutive u32
	// slots, indexed straight off the instruction's register field: the interpreter's operand
	// fetch is *(u32*)(ctx + 0x58 + regIndex * 4) with a 5-bit index (+0x180027120) or a 6-bit
	// one composed from the global bit (+0x1800260F0). Slots 0-15 are the i960 locals r0-r15,
	// 16-31 the globals g0-g15, so by the i960 ABI:
	//   r0  = pfp, previous frame pointer, return-type bits in the low 6   -> ctx+0x58
	//   r1  = sp,  stack pointer                                          -> ctx+0x5C
	//   r2  = rip, return address of the running procedure                -> ctx+0x60
	//   g15 = fp,  frame pointer of the running procedure                 -> ctx+0xD4
	// Confirmed against the DLL's own call/ret pair (+0x1800258D0 / +0x180025CB0), which does
	// newFP = (sp + 0x3F) & ~0x3F; saves r0-r15 to [oldFP]; sets g15 = newFP, sp = newFP + 0x40,
	// r0 = (oldFP & ~0x3F) | returnType — and masks +0xD4 with 0xFFFFFFC0 throughout.
	//
	// An earlier pass took +0x58 for "the break-state frame pointer" and +0x5C for the live one.
	// +0x5C is the stack pointer, which only aliases the frame base while a frame is empty; the
	// frame pointer is +0xD4.
	constexpr uintptr_t CTX_CALL_DEPTH = 0x48;        // int32: frame count (break-state only)
	constexpr uintptr_t CTX_REG_R0_PFP = 0x58;        // uint32: r0, caller's frame pointer
	constexpr uintptr_t CTX_REG_R2_RIP = 0x60;        // uint32: r2, return address into the caller
	constexpr uintptr_t CTX_REG_G15_FP = 0xD4;        // uint32: g15, running frame pointer
	// Instruction pointer. The interpreter fetches from *(u64*)(ctx+0) + *(u32*)(ctx+8), so +8
	// is an offset within whichever code bank +0 currently selects (+0x1800255F0). The bank
	// bases sit at +0x10 (ROM 0x000000), +0x20 (ROM 0x200000) and +0x18 (RAM 0x500000); the
	// DLL's own call handler reconstructs the absolute address by comparing +0 against them.
	constexpr uintptr_t CTX_CODE_BASE = 0x00;         // void*: host base of the live code bank
	constexpr uintptr_t CTX_IP = 0x08;                // uint32: IP relative to that bank
	constexpr uintptr_t CTX_BANK_ROM0 = 0x10;         // void*: bank for guest 0x000000+
	constexpr uintptr_t CTX_BANK_RAM = 0x18;          // void*: bank for guest 0x500000+
	constexpr uintptr_t CTX_BANK_ROM2 = 0x20;         // void*: bank for guest 0x200000+
	// The four interval timers, and the interrupt state they feed. Read out of the CPU loop's
	// own decompilation (VF2 +0x1800210C0), which is four textually identical blocks over
	// ctx+0x190/0x194, 0x198/0x19C, 0x1A0/0x1A4 and 0x1A8/0x1AC:
	//
	//     if (enable[n] && (count[n] -= 12) < 1) {
	//         if (mask & bit) { pending |= bit; RaiseInterrupt(2); }
	//         enable[n] = 0;
	//     }
	//
	// run once per batch of TWELVE executed instructions. So `count` is an instruction budget,
	// not a clock: the ROM asks for a period in 25 MHz ticks and the period write (VF2
	// +0x18004C170, guest 0xF00000 + n*4) converts it with count = ticks * (576000/25000000)
	// before storing it here. The frame-yield flag is checked INSIDE the batch and returns
	// immediately, so the instructions of the final partial batch of a frame are never charged.
	constexpr uintptr_t CTX_TIMER_BASE = 0x190;       // per channel: +0 enable byte, +4 int32 count
	constexpr uintptr_t CTX_TIMER_STRIDE = 8;
	constexpr size_t CTX_TIMER_COUNT = 4;
	constexpr uintptr_t CTX_IRQ_PENDING = 0x188;      // uint32: raised interrupt bits
	constexpr uintptr_t CTX_IRQ_MASK = 0x18C;         // uint32: which raised bits interrupt
	constexpr uintptr_t CTX_FRAME_YIELD = 0x1B0;      // uint8: set by the ROM's frame yield
	// Emulated memory map: 64 records x 0x70, each holding {read,write} pairs by access size.
	// Slot +0x20 is the 32-bit read: void read(void* out, uint32_t addr); slot +0x28 is its
	// paired 32-bit write: void write(uint32_t addr, const void* src). Unmapped regions
	// point at a bare `ret` stub, so an out-of-range address is inert rather than fatal.
	// (StF's table is at 0x172660, FV's at 0x170750 - see DwGame::rvaMemmapTbl. Both were read
	// out of the DLL's own dispatch, which indexes an array of 8-byte pointers by `index * 0xE`
	// and so pins the record size at 0x70 as well as the base.)
	constexpr size_t MEMMAP_RECORD_SIZE = 0x70;
	constexpr size_t MEMMAP_READ32 = 0x20;
	constexpr size_t MEMMAP_WRITE32 = 0x28;
	constexpr size_t MEMMAP_RECORD_COUNT = 64;
	// The game's own debug flag lives in emulated RAM: XORing this dword with the mask
	// flips it. Surfaced as the "Set the game's debug flag" debug setting.
	constexpr uint32_t DEBUG_FLAG_ADDRESS = 0x508000;
	constexpr uint32_t DEBUG_FLAG_XOR = 0x24;
	// GAME ASSIGNMENTS -> DAMAGE. The service menu's page edits a block of eighteen operator
	// settings that the ROM keeps in work RAM at 0x59C320 and the module mirrors into backup SRAM
	// +0x3320 (its injector, HLE hook 8 / set_window_data+0x564, copies the block verbatim and
	// substitutes only difficulty, country, free play and VS mode - DAMAGE is not one of the
	// values it supplies, which is why this is a live RAM write rather than another config field).
	// game_assignments_flag is byte +0x33 of that block, i.e. RAM 0x59C353, and bit 0x80 is REAL
	// damage: the whole byte reads 0x00 with everything at its default and 0x80 with REAL picked.
	//
	// Read-modify-written through the 32-bit accessors on the ALIGNED dword below, with only bit
	// 0x80 of its top byte touched - the rest of that byte is other items' flags and the three
	// bytes under it are TST_*/TIME/COUNTRY, none of which are ours to move. The dword's top byte
	// is 0x59C353 because emulated RAM is a flat little-endian host buffer: the DLL's own 32-bit
	// reader (0x18004F150) copies four bytes from ramBase+address in ascending order, no swap.
	// Field offsets inside m2ftg_config_t (source/m2ftg/m2ftg.h). Only the two that a netplay
	// round has to force are here; the block itself is 0x100C bytes.
	constexpr uintptr_t CONFIG_IS_VF20 = 0x07;      // VF2 only: version 2.0 when set, else 2.1
	constexpr uintptr_t CONFIG_IS_VS_MODE = 0x0A;   // all three games: credited 2P versus boot
	constexpr uint32_t GAME_ASSIGN_FLAG_DWORD = 0x59C350;   // holds 0x59C350..0x59C353
	constexpr uint32_t DAMAGE_REAL_BIT = 0x80u << 24;       // byte +0x33, bit 0x80
	// ROM symbol table: 800 records of {uint64_t addr; const char* name}, sorted ascending by
	// addr, starting at 0x1742D0. Earlier passes misread it as {name, addr} records starting
	// 8 bytes later at 0x1742D8, which pairs every name with the NEXT function's address and
	// then needs bogus "corrections". Ground truth for the record phase: the first record is
	// {0xB0, "start_ip"}, and 0xB0 is exactly the start IP stored in the ROM's i960 initial
	// memory image (boot word 3 of rom_code1.bin), while the misread pairing gives start_ip
	// 0x5A8, which matches nothing.
	// Per module BUILD, like every other RVA here: Like a Dragon Gaiden's Sonic the Fighters DLL
	// puts the same table at 0x17C230. Both were located by their content (first record
	// {0xB0, "start_ip"}, second 0x5A8), not by assuming a uniform section shift.
	constexpr uintptr_t RVA_SYMBOL_TBL_LJ = 0x1742D0;
	constexpr uintptr_t RVA_SYMBOL_TBL_GAIDEN = 0x17C230;
	inline uintptr_t SymbolTableRva()
	{
		return m2ftg::IsGaidenBuild() ? RVA_SYMBOL_TBL_GAIDEN : RVA_SYMBOL_TBL_LJ;
	}
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
		{ 0x4C6C0,  "Walks the emulated i960 call chain via the DLL's memory-map accessors, starting at r0. Its text output was stripped, so it returns 0. The pane below runs the same walk, plus the running procedure and its caller (live in g15/r2, not yet in a frame save area)." },
		{ 0x4C790,  "DLL handler: toggles the DEKU byte flag." },
		{ 0x4C7A0,  "Pokes the emulated RAM: sets 1P health word to 1." },
		{ 0x4C7C0,  "Pokes the emulated RAM: sets 1P health word to 150 (max)." },
		{ 0x4C7E0,  "Pokes the emulated RAM: sets 2P health word to 1." },
		{ 0x4C800,  "Pokes the emulated RAM: sets 2P health word to 150 (max)." },
		{ 0x58A0CE, "Byte flag read by the DLL's live per-frame perf-stat collectors." },
		{ 0x1E50D4, "Stage-select variable, read by live game logic (including module_main)." },
	};

	// ---- Per-game addresses ------------------------------------------------------------------
	//
	// Everything the netplay determinism layer touches, for each game that has it. The DEBUG
	// MENU panes above stay StF-only (their descriptor tree and notes are StF's), but the
	// helpers below - board booted, reset, RNG seeding, texture budget, emulated-RAM access -
	// are what a netplay round is built on and all of them now work for Fighting Vipers too.
	//
	// FV values were read out of fv-pxd-w64-d3d12_retail.dll; see docs/fv-hle-hooks.md.
	struct DwGame
	{
		const wchar_t* dllName;        // resolved with GetModuleHandleW; every module is ASLR'd
		uintptr_t rvaBootState;        // ROM/CPU boot phase; 2 = board booted
		uintptr_t rvaCpuCtxPtr;        // -> i960 context (set by the CPU init)
		uintptr_t rvaMemmapTbl;        // 64 x 0x70 emulated memory-map records
		uintptr_t rvaResetHandler;     // DEBUG MENU "RESET": run-state 0 + real board init
		uintptr_t rvaRngHolder;        // -> struct holding the host Mersenne Twister(s)
		// Offsets within that struct of each twister the ROM actually draws from. StF has one
		// (behind `rand`); FV and VF2 have TWO - `rand` at +0x20 and the stage picker at +0x08 -
		// and seeding only the first leaves the stage choice free to disagree between peers.
		const uintptr_t* rngStreams;
		size_t rngStreamCount;
		uintptr_t rvaTexBudgetHandler; // the wall-clock "has the upload budget expired?" handler
		uintptr_t rvaHleTable;         // installer input, so the handler above can be repointed
		size_t hleCount;
		// The ROM's own per-frame counter, an address in EMULATED RAM rather than an RVA. It is
		// the round-start anchor and the desync canary, so netplay is impossible without it -
		// 0 means "not measured yet", which is how a game is kept out of netplay entirely.
		//
		// StF and FV share 0x500020, which is not the coincidence it looks like: StF was built
		// on FV's engine and inherited its low-RAM global block. The rest of the layout did not
		// come across, so both were MEASURED advancing it by exactly 1 per module_main call
		// rather than assumed. VF2 is the common ancestor and very likely matches, but it has
		// not been measured, so it stays 0.
		uint32_t romFrameCounter;
		// See NetplaySupported(): measured-and-correct is not the same as ready to play.
		bool netplayReady;
		// Module-config bytes that change the SIMULATION and so must match between peers. The
		// config block is the one module_start memcpy'd in, and the DLL re-reads it live - VF2's
		// backup-RAM injector (hook 8) reads is_vf20 every time the ROM inits the board, which is
		// what lets a round adopt the room's value without relaunching the game.
		// 0 = this game has no such byte.
		// Base of the module's copy of m2ftg_config_t - the 0x100C bytes module_start memcpy'd
		// from params+0x38. Fields are written at fixed offsets from here: is_vf20 at +0x07,
		// is_vs_mode at +0x0A. Storing the BASE rather than each field's own RVA is what makes
		// the write checkable: +0x00 is `kind`, a small enum identifying the game, so a base that
		// is wrong for this module is caught before anything is written rather than quietly
		// corrupting whatever .data actually lives there. 0 = not measured, which makes every
		// config setter a no-op.
		uintptr_t rvaConfigBase;
		// What +0x00 must read for rvaConfigBase to be believed: {0=vf2, 1=fv, 2=stf}.
		uint32_t configKind;
		// ---- Desync canary -----------------------------------------------------------------
		// What each peer submits per frame for the two to compare.
		//
		// StF and FV submit romFrameCounter, which in those games advances exactly once per
		// module_main. VF2's does not - and the reason recorded here was WRONG until later on
		// 2026-08-02. It read the counter's behaviour ("each peer missed two VBLANKs over 40
		// frames, at DIFFERENT frames") as the counter LYING about a simulation that was fine:
		// "bookkeeping ABOUT the frame, not part of the simulation".
		//
		// The opposite was true. The counter was accurate, and those calls really did advance the
		// board by nothing - ~5% of VF2's module_main calls execute no instructions at all, which
		// is what put the two peers a frame apart. Treating the honest signal as noise is what
		// kept the real fault hidden; NetSession::EndFrame now acts on it directly.
		//
		// VF2 still hashes a slice of work RAM, because that is strictly the stronger canary -
		// actual game state rather than one counter, and it is what caught the numbering fault
		// within two frames. What changed is the justification, not the choice. Zero length keeps
		// the counter, so StF and FV are untouched and their two-machine verification stands.
		uintptr_t rvaRamBasePtr;    // -> host base of emulated RAM; guest G sits at +G
		uint32_t stateHashBase;     // guest address
		uint32_t stateHashLen;      // 0 = submit romFrameCounter instead
	};

	// The host RNG streams, identical in all three games because it is one engine.
	//
	// Each module builds its generator holder the same way (StF FUN_180064820, VF2 FUN_18005EF00):
	// allocate a block, write a count of FIVE, vector-construct five objects of 0x18 bytes, then
	// seed them from three reads of the performance counter. The Mersenne Twister state pointer
	// sits at +0x08 within each object, so the slots are 0x08 + N*0x18.
	//
	// All three lists were incomplete until 2026-08-02, because they were derived by inspecting
	// the HLE handlers - which only reveals the streams the ROM itself draws from. Everything
	// else kept its WALL-CLOCK seed and therefore differed between peers from the moment the
	// module loaded: a per-machine input sitting inside the simulation's reach, which is exactly
	// what this layer exists to eliminate.
	//
	// StF hid it because its second ROM-facing stream is the VS stage picker (hook 22), gated on
	// is_vs_mode - off by default, so the unseeded generator was never drawn from. Latent, not
	// absent. Seeding a stream nothing reads costs nothing; SeedHostRng requires all of them to
	// succeed, so a wrong slot fails loudly rather than silently half-seeding a round.
	constexpr uintptr_t RNG_STREAMS[] = { 0x08, 0x20, 0x38, 0x50, 0x68 };

	constexpr DwGame DW_STF = {
		L"stf-pxd-w64-d3d12_retail.dll",
		0x6B9300, 0x58A960, 0x172660, 0x4C840,
		0x68BB88, RNG_STREAMS, std::size(RNG_STREAMS),
		0x52FD0, 0x1E8870, 76, 0x500020, true,
		0x1ED490, 2,   // config base (module_start's memcpy target); kind 2 = stf
		0x8F7CC8, 0, 0,   // canary: frame_counter (verified on two machines)
	};

	// The same game out of Like a Dragon Gaiden (module 2023-11-27). Every RVA here moved, and
	// NETPLAY IS WHY THIS ROW EXISTS: ResetBoard() gates on rvaBootState and then calls
	// rvaResetHandler, so with Lost Judgment's addresses the Gaiden build read a boot state that
	// was never 2, the reset silently did nothing, and "Start match" appeared to do nothing at all
	// — the exact failure the note in NetSession::Step warns about.
	//
	// Each value was derived from the module itself and then checked, not shifted by an assumed
	// delta (the sections do not move uniformly: .data +0xB9C0 high up but +0xA950 around the HLE
	// table, .text varies, .rdata +0x7EC0 here):
	//   rvaResetHandler   item 4 of the DEBUG MENU tree, whose root is 0x20 ahead of the HLE
	//                     table in both builds; the LJ item at that index holds 0x4C840.
	//   rvaMemmapTbl      `lea r14,[tbl]` in the memory dispatch, byte-identical code in both;
	//                     22 rip-relative references in each build.
	//   rvaRngHolder      9 references in each build.
	//   rvaTexBudgetHandler / rvaHleTable   read straight out of the HLE installer table.
	//   rvaConfigBase     the destination of module_start's 0x100C config memcpy.
	//   rvaBootState / rvaCpuCtxPtr / rvaRamBasePtr   cross-checked against the HLE installer and
	//                     the i960 fetch dispatcher (see m2ftg/ModuleBuild.h).
	// romFrameCounter is an EMULATED-RAM address, so it is the same in both — which is the whole
	// reason cross-build netplay can work at all.
	constexpr DwGame DW_STF_GAIDEN = {
		L"stf-pxd-w64-d3d12_retail.dll",
		0x6C4CC0, 0x596320, 0x17A520, 0x4C7F0,
		0x697548, RNG_STREAMS, std::size(RNG_STREAMS),
		0x52F90, 0x1F31C0, 76, 0x500020, true,
		0x1F7DE0, 2,
		0x903688, 0, 0,
	};
	constexpr DwGame DW_FV = {
		L"fv-pxd-w64-d3d12_retail.dll",
		0x6BB900, 0x58CF60, 0x170750, 0x4B3E0,
		0x68E188, RNG_STREAMS, std::size(RNG_STREAMS),
		0x51C20, 0x1E5840, 95, 0x500020, true,
		0x1EA590, 1,   // config base (hook 12 injects +4,+5,+9,+0xA from here); kind 1 = fv
		0x8FA2C8, 0, 0,   // canary: frame_counter (verified on two machines)
	};
	// Virtua Fighter 2 as shipped in Yakuza: Like a Dragon. Hosted by YLAD/VF2.cpp rather than
	// the LJ host, which is why none of this may reach for an LJ GameDesc. See
	// docs/vf2-hle-hooks.md; the Kiwami 2 build of VF2 is a different module and is not here.
	constexpr DwGame DW_VF2 = {
		L"vf2-pxd-w64-retail.dll",
		0x641890, 0x51F9B8, 0x15C080, 0x490A0,
		0x623788, RNG_STREAMS, std::size(RNG_STREAMS),
		0x4F740, 0x185640, 67,
		// MEASURED 2026-08-02, same address as StF and FV: +1 per module_main call, sampled in
		// two bursts 340 frames apart. Predicted by the lineage (VF2 is the common ancestor) but
		// confirmed rather than assumed, because the rest of VF2's low-RAM layout is its own.
		0x500020,
		// Netplay ON since 2026-08-02, verified on two machines with no reported desyncs.
		//
		// What had held it back was read as a one-emulated-frame divergence. It was not a
		// divergence at all: the two simulations were bit-identical and only their frame
		// NUMBERING disagreed, because ~5% of VF2's module_main calls advance the emulated board
		// by nothing and netplay was counting every call as a frame. NetSession::EndFrame now
		// declines to spend a netplay frame on a call that advanced nothing. StF and FV stall the
		// same way but only during boot, before a round can start, which is why only VF2 showed
		// it. Full write-up in docs/vf2-hle-hooks.md.
		true,
		0x6263F0, 0,   // config base; kind 0 = vf2. is_vf20 at +0x07, is_vs_mode at +0x0A
		// Canary: hash work RAM, not the counter. The range deliberately SKIPS the first 0x100
		// bytes, which is where the jittery interrupt bookkeeping lives (frame_counter 0x500020,
		// CTRL_TIMER 0x500024). If this ever produces false positives, narrowing the range is
		// the knob - the mechanism is right even when the bounds want tuning.
		0x880030, 0x500100, 0x10000,
	};

	// Virtual On (Cyber Troopers: Operation Moon Gate), hosted on Yakuza Kiwami 2.
	//
	// Every value here was MEASURED against omg-pxd-w64-gog_retail.dll rather than carried over
	// from another game; the full derivation is in docs/von-netplay-recon.md. Two of them differ
	// in kind from the other three titles, because Virtual On is a LINKED-CABINET game and the
	// module emulates BOTH Model 2 boards:
	//
	//   * The bank switch (RVA 0x69D30) re-points work RAM, the comm block, backup/IO and the
	//     video region per board. rvaRamBasePtr below is the i960 0x500000 bias it writes, so it
	//     always names whichever board is currently selected - board 0 after the frame step.
	//   * The RNG holder is NOT re-pointed, so both boards draw from one set of five generators
	//     and a single seeding covers the pair.
	constexpr DwGame DW_OMG = {
		L"omg-pxd-w64-gog_retail.dll",
		// bootState: the phase machine at 0x18006A100 walks 0/1/2 and reaches 2 once the ROM is
		// loaded and both boards are initialised. Identified, not guessed - its phase-2 arm
		// installs the debug-menu root header 0x476248, the same one the RESET item hangs off.
		0x7ADCA8,
		// cpuCtxPtr: the per-board i960 context. The bank switch writes this global as
		// 0x7C21D0 + board*0x1B0, and 0x1B0 is exactly CTX_FRAME_YIELD - i.e. the stride IS the
		// context size, so the array and the struct agree.
		//
		// NOT optional, despite an earlier note here saying it only drove the StF debug panes.
		// NetSession's stall test has three signals - canary moved, a timer channel counted down,
		// a channel was re-armed - and the last two come from ReadI960Timers, which fails flat
		// when this is 0. Virtual On holds on ~65% of module_main calls (VF2: 5%), so it leans on
		// that test far harder than any other game, and running it on the canary alone is what a
		// slower peer would expose as "one emulated frame behind".
		//
		// VERIFIED LIVE: three channels enabled and counting down by exactly 6120 instructions on
		// a frame that advances, and by 0 on a hold - so delta.charged is now a clean discriminator
		// and EndFrame no longer spends a netplay frame on a call that ran nothing.
		//
		// CAVEAT: only the TIMER and IRQ offsets are right for this generation. The trace's `ip=`
		// (CTX_IP, +0x08) reads 0x346E7010, not a ROM address, and `yield=` (CTX_FRAME_YIELD,
		// +0x1B0) reads 204 because 0x1B0 is the array STRIDE here, so it lands on the next
		// board's first byte. Both are trace-only - nothing in the stall test or the canary reads
		// them - but do not trust those two columns when diffing two peers' logs.
		0x691148,
		// memmapTbl: REQUIRED, despite an earlier note in this file's history claiming otherwise.
		// ReadEmulated32 dispatches through it, and NetSession reads the ROM frame counter that way
		// to detect the reset and reach the round anchor - so a zero here silently gives romFrame=0
		// forever and no round can ever start. (The desync canary still worked, because
		// StateCheckValue hashes the host buffer directly and bypasses this dispatch, which is what
		// made the gap look harmless at first.)
		//
		// Derived, then cross-checked twice: the comm-board device's record sits at 0x44EFE0 and
		// i960 0x01A10000 >> 20 = 0x1A, giving 0x44EFE0 - 0x1A*0x70 = 0x44E480. The other device
		// record found independently at 0x44E800 lands on exactly (0x44E800-0x44E480)/0x70 = index
		// 8, i.e. i960 0x800000-0x8FFFFF, which is where CoproDataPort (0x884000) lives.
		0x44E480,
		0x6BA90,   // RESET: the one real debug-menu action; STEP/GO are the usual stubs
		0x690810, RNG_STREAMS, std::size(RNG_STREAMS),
		// texBudgetHandler: ABSENT. The wall-clock wrapper 0x1800856B0 has 49 call sites and not
		// one of them is an HLE handler, so there is no per-machine timing input inside the
		// simulation to repoint. See the caveat in the doc: that rules out a DIRECT call only.
		0,
		0x476510, 121,   // the largest HLE table of the four (StF 76, FV 95, VF2 67)
		// romFrameCounter: `GlobCntr`, measured by sweeping every dword in the work-RAM window and
		// classifying each module_main call. It steps by one or holds, and holds are the NORM here
		// (311 advances in 900 calls - an uncapped host loop against a ~59 Hz board), which is
		// exactly what EndFrame already handles. Reads identically on BOTH boards.
		0x5024E8,
		// PROVISIONAL. Every field above is measured and verified live - the anchor read tracks
		// GlobCntr exactly and the canary produces a changing work-RAM hash - but NO VON ROUND HAS
		// BEEN PLAYED YET. Enabled so it can be tested; expect to turn it back off if the first
		// loopback round misbehaves. Two known risks, both written down rather than discovered
		// later:
		//   * GlobCntr makes four non-unit jumps over 900 calls (~+11 total), almost certainly at
		//     boot/state transitions. The anchor's whole job is to be reached at the SAME value on
		//     both peers, so a jump straddling the anchor would start the round misaligned.
		//   * ResetBoard clears the two-board gate, so round start drops to one cabinet unless the
		//     host holds it - K2Host reasserts it every frame, which is why that is required.
		// Also unverified: whether RESET re-inits BOTH boards or only the selected one.
		true,
		0x7A7FB0, 3,   // config base (module_start's memcpy target, verified live reading kind=3)
		// Canary: hash work RAM rather than the counter, for VF2's reason plus one of VON's own -
		// a hash covers game state, and the counter has those four jumps. Same skip of the first
		// 0x100 bytes.
		//
		// RANGE NARROWED TO CHUNK 0 (0x500100..0x510000) after the first loopback smoke test,
		// which desynced at frame 1. The plugin's work-RAM map localised it precisely: of the
		// sixteen 0x10000 chunks, only 0x510000, 0x520000 and 0x590000 differed between the
		// peers - chunk 0, which holds GlobCntr, MainMode, cRecn/cSend and the whole low-globals
		// block the ROM initialises, matched BIT FOR BIT. VF2's 0x10000 length spills 0x100 bytes
		// past chunk 0, and that overhang is the only reason the canary tripped.
		//
		// Those three chunks are stale rather than divergent: they are empty in a fresh run and
		// only accumulate content as attract/demo plays, so two machines that idled for different
		// lengths before the round start carry different leftovers. ResetBoard restarts the ROM
		// (GlobCntr restarts) without zeroing them. Narrowing is the knob VF2's own comment names
		// for this case.
		//
		// HONEST STATUS: this assumes the ROM never READS those bytes before writing them. If a
		// round now runs but the two screens drift apart, that assumption is wrong and the fix is
		// a deeper reset, not a narrower canary.
		//
		// Checked and NOT the cause (measured, single instance): the board index is 0 at every
		// sample, so the canary always reads board 0 - it is not comparing one peer's master
		// against the other's slave. Both boards' chunk hashes are identical within a machine
		// anyway, since they boot the same ROM deterministically.
		0xC37000, 0x500100, 0xFF00,
	};

	static const DwGame* CurrentDw()
	{
		switch (gGeneral.GetGameId())
		{
		// Sonic the Fighters ships in two titles and every RVA in the descriptor differs between
		// the two module builds, so this picks by BUILD as well as by game.
		case YAMPGeneral::GameId::StF: return m2ftg::IsGaidenBuild() ? &DW_STF_GAIDEN : &DW_STF;
		case YAMPGeneral::GameId::FV:  return &DW_FV;
		case YAMPGeneral::GameId::VF2: return &DW_VF2;
		case YAMPGeneral::GameId::VON_K2: return &DW_OMG;
		default:                       return nullptr;
		}
	}

	static uint8_t* ModuleBase()
	{
		// Every pxd module ships with ASLR set, so resolve the real base by name rather than
		// assuming the preferred 0x180000000 - the mistake that cost FV its first bring-up.
		// The name comes from the game's own descriptor, not an LJ GameDesc lookup: this file
		// serves the YLAD host too now, and that host has no GameDesc table to ask.
		const DwGame* game = CurrentDw();
		return game != nullptr
			? reinterpret_cast<uint8_t*>(GetModuleHandleW(game->dllName))
			: nullptr;
	}

	// Board booted, for a game that has a descriptor. Returns false (rather than reading a
	// wild address) for any game that does not.
	static bool BoardBooted(const DwGame* game, const uint8_t* base) noexcept
	{
		return game != nullptr && base != nullptr
			&& *reinterpret_cast<const uint32_t*>(base + game->rvaBootState) == 2;
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
		const DwGame* game = CurrentDw();
		if (game == nullptr)
		{
			return false;
		}
		void* reader = *reinterpret_cast<void**>(
			base + game->rvaMemmapTbl + index * MEMMAP_RECORD_SIZE + MEMMAP_READ32);
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
		const DwGame* game = CurrentDw();
		if (game == nullptr)
		{
			return false;
		}
		void* writer = *reinterpret_cast<void**>(
			base + game->rvaMemmapTbl + index * MEMMAP_RECORD_SIZE + MEMMAP_WRITE32);
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

		const auto* symbols = reinterpret_cast<const I960Symbol*>(base + SymbolTableRva());
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

namespace
{
	// --- Deterministic texture-upload budget (see the header) --------------------------------
	// (The table, its length and the handler to repoint all come from DwGame - StF's budget
	// handler is 0x52FD0 across 4 records, FV's is 0x51C20 across 4 of its own.)
	struct HleEntry
	{
		uint32_t romOffset;
		uint32_t padding;
		uint64_t handler;
	};
	static_assert(sizeof(HleEntry) == 0x10, "HLE record is 16 bytes in the DLL");

	using HleHandlerFn = unsigned long long (*)(unsigned int);

	HleHandlerFn g_originalTexBudget = nullptr;
	uint8_t* g_texBudgetBase = nullptr;
	// Captured when the wrapper is installed, so the hot path needs no game lookup.
	uintptr_t g_texBudgetCtxRva = 0;

	// Runs the DLL's own handler first so its return value (the trapped instruction's length, 4 or
	// 8) stays exactly right, then replaces the wall-clock answer it wrote into g0.
	unsigned long long DeterministicTexBudget(unsigned int index)
	{
		const unsigned long long length = g_originalTexBudget(index);

		if (g_texBudgetBase != nullptr)
		{
			uint8_t* ctx = *reinterpret_cast<uint8_t**>(g_texBudgetBase + g_texBudgetCtxRva);
			if (ctx != nullptr)
			{
				// g0 = 0: "the budget has not expired", so the unpack loop runs to completion
				// instead of stopping wherever this machine happened to be after the deadline
				// (9 ms in StF; 12 ms, or 8 in some master states, in FV).
				*reinterpret_cast<uint32_t*>(ctx + 0x98) = 0;
			}
		}
		return length;
	}
}

bool m2ftg::SetTextureBudgetDeterministic(bool enable)
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (!BoardBooted(game, base))
	{
		return false;
	}

	g_texBudgetBase = base;
	g_texBudgetCtxRva = game->rvaCpuCtxPtr;
	const uint64_t original = reinterpret_cast<uint64_t>(base + game->rvaTexBudgetHandler);
	const uint64_t replacement = reinterpret_cast<uint64_t>(&DeterministicTexBudget);

	// The table lives in .data and the installer writes it at boot, so it is already writable -
	// no VirtualProtect dance needed.
	auto* table = reinterpret_cast<HleEntry*>(base + game->rvaHleTable);
	size_t patched = 0;
	for (size_t i = 0; i < game->hleCount; ++i)
	{
		if (enable && table[i].handler == original)
		{
			if (g_originalTexBudget == nullptr)
			{
				g_originalTexBudget = reinterpret_cast<HleHandlerFn>(original);
			}
			table[i].handler = replacement;
			++patched;
		}
		else if (!enable && table[i].handler == replacement)
		{
			table[i].handler = original;
			++patched;
		}
	}
}

namespace
{
	// Layout from the generator (StF DLL+0x8D40 / FV DLL+0x8D00, the same code): the holder
	// global points at a struct whose slots hold Mersenne Twister state objects.
	constexpr uintptr_t MT_OFF_STATE = 0x008;    // u32 [624]
	constexpr uintptr_t MT_OFF_INDEX = 0x9C8;    // circular index, 0..623
	constexpr uint32_t MT_N = 624;

	// Seeds one twister in place. Returns false without writing anything if the object fails
	// its sanity check, so a wrong address costs nothing rather than scribbling 2.5 KB.
	static bool SeedOneTwister(uint8_t* holder, uintptr_t slot, uint32_t seed) noexcept
	{
		__try
		{
			uint8_t* mt = *reinterpret_cast<uint8_t**>(holder + slot);
			if (mt == nullptr)
			{
				return false;
			}

			// A live twister always has its index inside the state array.
			uint32_t& index = *reinterpret_cast<uint32_t*>(mt + MT_OFF_INDEX);
			if (index >= MT_N)
			{
				return false;
			}

			// Standard init_genrand:
			//     mt[0] = seed; mt[i] = 1812433253 * (mt[i-1] ^ (mt[i-1]>>30)) + i.
			uint32_t* state = reinterpret_cast<uint32_t*>(mt + MT_OFF_STATE);
			state[0] = seed;
			for (uint32_t i = 1; i < MT_N; ++i)
			{
				const uint32_t prev = state[i - 1];
				state[i] = 1812433253u * (prev ^ (prev >> 30)) + i;
			}
			// This generator twists in place off a circular index rather than regenerating in
			// blocks, so a freshly seeded state starts at element 0.
			index = 0;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}

bool m2ftg::SeedHostRng(uint32_t seed)
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (!BoardBooted(game, base))
	{
		return false;
	}

	uint8_t* holder = nullptr;
	__try
	{
		holder = *reinterpret_cast<uint8_t**>(base + game->rvaRngHolder);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	if (holder == nullptr)
	{
		return false;
	}

	// EVERY stream the ROM draws from has to be seeded, and each one gets a DIFFERENT seed.
	//
	// FV has two: `rand` (hook 43) and the VS stage picker (hook 30), which is its own
	// generator object rather than a second draw from the first. Seeding only `rand` would
	// leave both peers agreeing on the fight and disagreeing on the stage - a desync that
	// happens before the first frame of the round and looks nothing like an RNG problem.
	//
	// Deriving each stream's seed from the shared one keeps a single value on the wire while
	// making sure two streams seeded from the same match do not run in lockstep with each
	// other, which would be a needless correlation between unrelated draws.
	// EVERY stream must seed, not merely one of them.
	//
	// This returned "any succeeded" until 2026-08-02, which is a silent desync generator: a game
	// whose second twister failed its sanity check still reported success, the round started, and
	// the two peers then agreed on the fight and disagreed on the stage - the exact failure the
	// per-stream list exists to prevent. A determinism precondition has to be all-or-nothing, and
	// refusing the round is always better than playing a diverging one.
	bool allSeeded = game->rngStreamCount != 0;
	for (size_t i = 0; i < game->rngStreamCount; ++i)
	{
		// Weyl-style decorrelation; any fixed odd stride would do.
		const uint32_t streamSeed = seed + static_cast<uint32_t>(i) * 0x9E3779B9u;
		const bool ok = SeedOneTwister(holder, game->rngStreams[i], streamSeed);
		if (!ok)
		{
			allSeeded = false;
		}
		// Logged per stream: "seeding failed" on its own does not say WHICH generator, and the
		// second one is invisible in-game until a stage is picked.
		DebugLogFile("[rng] stream %zu (holder+0x%02X) seed=0x%08X -> %s\n",
			i, static_cast<unsigned>(game->rngStreams[i]), streamSeed, ok ? "ok" : "FAILED");
	}
	return allSeeded;
}

bool m2ftg::ReadEmulatedRam32(uint32_t address, uint32_t& out)
{
	uint8_t* base = ModuleBase();
	if (base == nullptr)
	{
		return false;
	}
	return ReadEmulated32(base, address, out);
}

bool m2ftg::IsBoardBooted()
{
	return BoardBooted(CurrentDw(), ModuleBase());
}

bool m2ftg::ResetBoard()
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	// Calling the reset before the board has finished booting would re-init a CPU whose ROM is
	// still loading, so gate on the same boot dword the menu actions use.
	if (!BoardBooted(game, base))
	{
		return false;
	}

	// The DEBUG MENU's "RESET" item: writes run-state 0 and calls the DLL's real i960/board
	// init. Unlike STEP/GO/REGS it is NOT a stub in either retail build.
	InvokeDwAction(base + game->rvaResetHandler);
	return true;
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

void m2ftg::UpdateDamageAssignment()
{
	if (gGeneral.GetGameId() != YAMPGeneral::GameId::StF)
	{
		return;
	}
	const YAMPSettings* settings = gGeneral.GetSettings();
	// NOT forced off during netplay, unlike the debug flag above - this one is meant to be on in
	// a match. What makes it safe there is that the value comes from the ROOM rather than from
	// this machine's settings, so both peers write the same bit; net::EffectiveRealDamage is the
	// single place that choice is made.
	const bool real = net::EffectiveRealDamage(settings != nullptr && settings->m_m2RealDamage);

	uint8_t* base = ModuleBase();
	if (base == nullptr)
	{
		return;
	}
	// Before the board is up there is no block to write, and the ROM's own init_game_assignments
	// runs during boot - a write landing before it would simply be overwritten.
	if (*reinterpret_cast<const uint32_t*>(base + DW_STF.rvaBootState) != 2)
	{
		return;
	}

	uint32_t value = 0;
	if (!ReadEmulated32(base, GAME_ASSIGN_FLAG_DWORD, value))
	{
		return;
	}

	// Enforced every frame, but only WRITTEN when it differs. Re-asserting is what makes this
	// survive the ROM reloading its assignments (a board reset, or leaving the service menu),
	// which is exactly what happens at the start of every netplay round.
	const uint32_t wanted = real ? (value | DAMAGE_REAL_BIT) : (value & ~DAMAGE_REAL_BIT);
	if (wanted != value)
	{
		WriteEmulated32(base, GAME_ASSIGN_FLAG_DWORD, wanted);
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

// Writes one byte of the module's config block, but only after proving the block is where we
// think it is.
//
// The config setters poke the module's own .data directly - there is no API for it - so a wrong
// base does not fail, it corrupts. The block's first dword is `kind`, a small enum naming the
// game ({0=vf2, 1=fv, 2=stf}), which makes the base cheap to verify at the moment of use: if it
// does not read back as this game, nothing is written and the caller sees false. That turns a
// mis-transcribed RVA into a setting that visibly does not apply instead of a random byte flipped
// somewhere in the module.
static bool WriteConfigByte(uintptr_t fieldOffset, uint8_t value) noexcept
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (game == nullptr || base == nullptr || game->rvaConfigBase == 0)
	{
		return false;
	}
	__try
	{
		uint8_t* config = base + game->rvaConfigBase;
		if (*reinterpret_cast<const uint32_t*>(config) != game->configKind)
		{
			return false;
		}
		*reinterpret_cast<volatile uint8_t*>(config + fieldOffset) = value;
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

bool m2ftg::SetVf2Version20(bool version20)
{
	// The config block is plain .data the module memcpy'd in at module_start; no VirtualProtect.
	return WriteConfigByte(CONFIG_IS_VF20, version20 ? 1u : 0u);
}

bool m2ftg::SetVsMode(bool vsMode)
{
	// Same mechanism and the same window as SetVf2Version20: re-read by the backup-RAM injector
	// every time the ROM initialises the board, so a write here takes effect at the NEXT board
	// init. That is exactly what the round-start ResetBoard() is - so this must be called BEFORE
	// that reset, never after.
	return WriteConfigByte(CONFIG_IS_VS_MODE, vsMode ? 1u : 0u);
}

bool m2ftg::StateCheckIsHash()
{
	const DwGame* game = CurrentDw();
	// Same test StateCheckValue branches on, deliberately read from the same field rather than
	// listed per game: a game that gains a hash range gains the right comparison with it.
	return game != nullptr && game->stateHashLen != 0;
}

uint32_t m2ftg::StateCheckValue()
{
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (game == nullptr || base == nullptr)
	{
		return 0;
	}

	// No hash range configured: the ROM's own frame counter, as StF and FV have always used.
	if (game->stateHashLen == 0)
	{
		uint32_t v = 0;
		ReadEmulatedRam32(game->romFrameCounter, v);
		return v;
	}

	// Hash the host buffer directly rather than through the memory-map dispatch, which costs a
	// function pointer per dword - this runs once per emulated frame.
	__try
	{
		const uint8_t* ram = *reinterpret_cast<uint8_t* const*>(base + game->rvaRamBasePtr);
		if (ram == nullptr)
		{
			return 0;
		}
		const uint8_t* p = ram + game->stateHashBase;
		// FNV-1a, stepped a DWORD at a time rather than a byte: it only has to be identical on
		// both peers and sensitive to a changed byte, and the byte-wise form is a serial
		// xor-multiply chain four times longer for no extra sensitivity. Both peers are x86-64,
		// so the word load agrees on both without an endian conversion.
		//
		// Only the 32-bit RESULT goes on the wire (submit_state_check takes a uint32), so this
		// costs no bandwidth over the single counter it replaces - it is purely local work, and
		// it runs once per EMULATED frame, not once per host frame.
		uint32_t h = 2166136261u;
		const uint32_t words = game->stateHashLen / 4;
		const auto* w = reinterpret_cast<const uint32_t*>(p);
		for (uint32_t i = 0; i < words; ++i)
		{
			h = (h ^ w[i]) * 16777619u;
		}
		return h;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return 0;
	}
}




bool m2ftg::ReadI960Timers(I960Timers& out)
{
	out = {};
	const DwGame* game = CurrentDw();
	uint8_t* base = ModuleBase();
	if (!BoardBooted(game, base))
	{
		return false;
	}

	// The context pointer is written by the CPU init and survives a board reset, but this runs
	// from the module thread while the emulator is between frames - keep the same structured
	// net every other reader here uses rather than trusting the boot phase alone.
	__try
	{
		const uint8_t* ctx = *reinterpret_cast<uint8_t* const*>(base + game->rvaCpuCtxPtr);
		if (ctx == nullptr)
		{
			return false;
		}
		for (size_t i = 0; i < CTX_TIMER_COUNT; ++i)
		{
			const uint8_t* channel = ctx + CTX_TIMER_BASE + i * CTX_TIMER_STRIDE;
			out.enabled[i] = *channel;
			out.count[i] = *reinterpret_cast<const int32_t*>(channel + 4);
		}
		out.pending = *reinterpret_cast<const uint32_t*>(ctx + CTX_IRQ_PENDING);
		out.mask = *reinterpret_cast<const uint32_t*>(ctx + CTX_IRQ_MASK);
		out.ip = *reinterpret_cast<const uint32_t*>(ctx + CTX_IP);
		out.yield = *(ctx + CTX_FRAME_YIELD);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		out = {};
		return false;
	}
}

bool m2ftg::NetplaySupported()
{
	const DwGame* game = CurrentDw();
	if (game == nullptr || game->romFrameCounter == 0)
	{
		return false;
	}
	// The diagnostic override deliberately does NOT relax the frame-counter test above: without
	// a measured counter there is no anchor and no canary, so a round could not be set up at
	// all. What it relaxes is the per-game "this one is known to diverge" verdict, which is the
	// only thing standing between VF2 and an observable round.
	if (game->netplayReady)
	{
		return true;
	}
	const YAMPSettings* settings = gGeneral.GetSettings();
	return settings != nullptr && settings->m_netForceUnsupported;
}

uint32_t m2ftg::RomFrameCounterAddress()
{
	const DwGame* game = CurrentDw();
	return game != nullptr ? game->romFrameCounter : 0;
}
