#include "VF2.h"
#include "../../pxd/PatternHelpers.h"
#include "../../ModuleLoad.h"

// Virtua Fighter 2 (YLAD "m2ftg" module, DX11) host.
//
// The module is `vf2-pxd-w64-retail.dll` from Yakuza: Like a Dragon (runtime/media/m2ftg).
// Unlike Sonic the Fighters (LJ, DX12), this is the DX11 build of the same Model 2 emulator
// family, hosted with the same m2ftg protocol (see m2ftg.h). The sl layer version matches
// Lost Judgment's exactly (tag 'LBsl', version 0x40601, size 0xF000), so the m2ftg sl
// reconstruction is reused wholesale. All facts below were verified against the DLL in Ghidra
// and YLAD's fully symbolized host (see memory: vf2-hosting-recon).
//
// Key DLL facts (base 0x180000000):
//   module_start @0x18005d500, module_stop @0x18005d780, module_main = 0x18005d8a0
//   sl init  FUN_180065e00: needs {size 0x10, ctx+8 == 0xF000};   embedded sl ctx @+0x187100
//   ct init  FUN_1800afad0: needs {size 0x10, ctx+8 == 0x30}
//   gs init  FUN_180089220: needs {size 0x58, ctx+8 == 0x3820C0}; embedded gs ctx @+0x196a40,
//            then import_shared_symbols(ctx+0x20) = FUN_180092440 consuming SIX slots
//            (named via YLAD sbgl::cdevice::export_shared_symbols @0x140245e70):
//              [0] g_pD3DDevice (ID3D11Device*)     [1] g_p_device_native (sbgl cdevice)
//              [2] g_p_swap_chain                   [3] g_FeatureSupport (dword, BY VALUE)
//              [4] g_p_num_swap_chains (int*)       [5] g_p_allocator (pxd allocator object)

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <filesystem>
#include <string>
#include <cstdio>

#include "../../wil/resource.h"
#include "../../criware/Cri.h"

#include "../../pxd/LJ/sl.h"
#include "../m2ftg.h"
#include "../ModuleArgs.h"
#include "../DisplayModes.h"
#include "../../pxd/Imports.h"
#include "../../pxd/GsBringup.h"
#include "../ImportSymbols.h"
#include "../HostUI.h" // PollAspectSetting / DrawPauseMenu (shared m2ftg host helpers)
#include "../../cabinet/Cabinet.h"
#include "../../input/Input.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../RenderWindow.h"

#include "../../Utils/Patterns.h"
#include "../../Utils/MemoryMgr.h"
#include "../../Utils/ScopedUnprotect.hpp"
#include "../../DebugLog.h"
#include "../NetSession.h" // the shared netplay session driver
#include "../SystemSwitches.h" // cabinet TEST / SERVICE on the emulated I/O board

namespace m2ftg
{
	// The pxd platform layer this host is built on (source/pxd): sl/gs/cgs_device_context,
	// the host cdevice, Imports/ImportSymbol. Shared with the LJ VF5FS host.
	using namespace pxd;
	namespace VF2
	{
		using namespace m2ftg; // sl layer, m2ftg structs, Imports machinery are shared

		static const wchar_t* DLL_NAME = L"vf2-pxd-w64-retail.dll";

		static wil::unique_hmodule gameDll;
		static std::filesystem::path gamePath;

		using module_func_t = int(__cdecl*)(uint64_t, void*);

		// ============================ symbol resolution ============================

		// Pattern resolution: the shared helpers in pxd/PatternHelpers.h, under this
		// namespace's names so the tables below read unchanged.
		using pxd::get_module_pattern;
		using pxd::immediate;
		using pxd::immediate8;
		using pxd::immediateImm8;

		// Pattern set verified against vf2-pxd-w64-retail.dll (each match confirmed unique and
		// resolved to the intended target; addresses in comments are the verified targets).
		static Imports BuildVF2SymbolMap(void* dll)
		{
			using S = ImportSymbol;
			Imports symbols{
				// static init FUN_180002040: LEA RCX,[embedded sl ctx]; CALL ctor; LEA RCX,[atexit lambda]
				{ S::SL_CONTEXT_INSTANCE, immediate(get_module_pattern(dll, "48 83 EC 28 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 0D F9 48 0B 00", 7)) }, // 180187100
				// gs self-context lazy-init: TEST RCX,RCX / JNZ / LEA RAX,[embedded gs ctx] / MOV [ctxptr],RAX
				{ S::GS_CONTEXT_INSTANCE, immediate(get_module_pattern(dll, "48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 8)) },  // 180196a40
				{ S::GS_CONTEXT_PTR,      immediate(get_module_pattern(dll, "48 85 C9 75 16 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ?", 15)) }, // 180196a10
				{ S::SL_KERNEL_CALLOC, get_module_pattern(dll, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B FA 48 8B F1 E8 ? ? ? ? 48 8B D8 48 85 C0 74 ? 40 F6 C7 08") }, // 180060d10
				{ S::SL_FILE_OPEN,  immediate(get_module_pattern(dll, "E8 ? ? ? ? 8B 08 85 C9", 1)) }, // 180068e30
				{ S::SL_FILE_READ,  get_module_pattern(dll, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 41 8B F0 48 8B EA 8B D9") }, // 180068f50
				{ S::SL_FILE_CLOSE, get_module_pattern(dll, "48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 8B D9 BD 05 40 00 80") }, // 180068d10
				{ S::SL_HANDLE_CREATE, immediate(get_module_pattern(dll, "E8 ? ? ? ? 8B 08 89 4E 20", 1)) }, // 1800674a0
				{ S::SL_FILE_HANDLE_DESTROY, get_module_pattern(dll, "48 85 C9 0F 84 ? ? ? ? 57") },
				{ S::ARCHIVE_LOCK_WLOCK,   immediate(get_module_pattern(dll, "E8 ? ? ? ? 8B 43 10 83 E8 01", 1)) }, // 180067740
				{ S::ARCHIVE_LOCK_WUNLOCK, get_module_pattern(dll, "8B 01 89 44 24 08 8B 44 24 08 FF C8 0F B7 D0") }, // 180067380
				// M2FTGAppModule per-frame render-system submit + live-execute_info global (same
				// protocol as StF: module_main `CMP RCX,0x1760 / JNZ / MOV [rip],RDX`).
				{ S::STF_FRAME_SUBMIT, get_module_pattern(dll, "48 83 EC 28 E8 ? ? ? ? 48 83 C4 28 E9 ? ? ? ?") }, // 180037e60
				{ S::STF_RENDER_EXECINFO, immediate(get_module_pattern(dll, "48 81 F9 60 17 00 00 0F 85 ? ? ? ? 48 89 15 ? ? ? ?", 16)) }, // 180627400
			};

			// Frame step (0x180051030); the payload is the CALL to the per-frame I/O refresh
			// (0x18004A060), which is what rebuilds the system-input port io[9] and is therefore
			// the only place a host-held TEST/SERVICE line can be applied and survive.
			//
			// Nearly the LJ pattern, and deliberately kept as its own: VF2's build compiles the
			// same source with a SHORT jump (74 ?) where StF and FV get a near one (0F 84 ? ? ? ?),
			// so the call lands at +0x21 rather than +0x25. That four-byte difference is the whole
			// reason the shared pattern missed here and the switches were assumed unavailable.
			{
				hook::pattern frameStep(dll,
					"40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 80 3D ? ? ? ? 00 48 89 43 60 74 ? FF 05 ? ? ? ? E8");
				if (frameStep.size() == 1)
				{
					symbols.Add(S::I960_IO_REFRESH_CALL, frameStep.get(0).get<void>(0x21));
				}
			}
			return symbols;
		}

		// ============================== host gs pieces =============================

		// The VF2 gs context is the DLL's OWN embedded template (0x3820C0 bytes, CRT-constructed);
		// we only touch its header. Shared symbols live at ctx+0x20 (six qwords, see above).
		static uint8_t* g_gsContext = nullptr;

		// Mirrors params.config.is_freeplay (set in Run); gates the coin-insert dance in GameLoop.
		static bool g_isFreeplay = true;

		// pxd allocator objects. Zero-fill like the host csl_allocator (the pxd engine expects
		// zeroed allocations). Two vtable conventions observed:
		//  - shared-symbol slot [5] (g_p_allocator): assumed {[0]=alloc(this,size,align), [1]=free}
		//  - gs context +0xB0: verified {[1]=alloc(this,size,align,tag), [2]=free?} — the shader
		//    create FUN_18008d270 calls vtbl+8 with (this, 0x40, 0x10, name)
		// The zero-filling allocator trio is shared (pxd/GsBringup.h); the two vtable ORDERS are
		// this module's own measured facts (and K2's differ - it frees at slot 3, not 2).
		static void* s_allocatorVtbl[4] = {
			reinterpret_cast<void*>(&pxd::GsBringup::ZeroAlloc),
			reinterpret_cast<void*>(&pxd::GsBringup::AlignedFree),
			reinterpret_cast<void*>(&pxd::GsBringup::AllocNoop),
			reinterpret_cast<void*>(&pxd::GsBringup::AllocNoop),
		};
		static void* s_allocator[2] = { s_allocatorVtbl, nullptr };

		// gs-context allocator (ctx+0xB0): vtbl slot 1 = alloc, slot 2 = free
		static void* s_gsAllocatorVtbl[4] = {
			reinterpret_cast<void*>(&pxd::GsBringup::AllocNoop),
			reinterpret_cast<void*>(&pxd::GsBringup::ZeroAlloc),
			reinterpret_cast<void*>(&pxd::GsBringup::AlignedFree),
			reinterpret_cast<void*>(&pxd::GsBringup::AllocNoop),
		};
		static void* s_gsAllocator[2] = { s_gsAllocatorVtbl, nullptr };

		// [1] g_p_device_native: the sbgl cdevice object. Exact YLAD-era layout unknown yet — start
		// with a large zeroed buffer and map fields crash-by-crash (StF bring-up method). Seed the
		// D3D11 device/context pointers at the Y6-era offsets as a first guess (+0x00/+0x08).
		static uint8_t s_deviceNative[0x2000]{};
		// [2] g_p_swap_chain: ditto — zeroed object, IDXGISwapChain* seeded at +0x00.
		static uint8_t s_swapChain[0x400]{};
		static int s_numSwapChains = 1;

		static void FillSharedSymbols(const RenderWindow& window)
		{
			*reinterpret_cast<void**>(s_deviceNative + 0x00) = window.GetD3D11Device();
			*reinterpret_cast<void**>(s_deviceNative + 0x08) = window.GetD3D11DeviceContext();

			// cswap_chain object layout (from FUN_18008f4b0, the bind-default-targets path):
			//   +0x00 IDXGISwapChain*, +0x08 color target obj, +0x10 depth target obj,
			//   +0x20 packed color dims ((w-1) | (h-1)<<14), +0x40 packed depth dims
			*reinterpret_cast<void**>(s_swapChain + 0x00) = window.GetSwapChain();
			const uint32_t packedDims = pxd::GsBringup::PackDims(1280, 720);
			*reinterpret_cast<uint32_t*>(s_swapChain + 0x20) = packedDims;
			*reinterpret_cast<uint32_t*>(s_swapChain + 0x40) = packedDims;

			void** p = reinterpret_cast<void**>(g_gsContext + 0x20);
			p[0] = window.GetD3D11Device();
			p[1] = s_deviceNative;
			p[2] = s_swapChain;
			p[3] = reinterpret_cast<void*>(uintptr_t(0)); // g_FeatureSupport dword, by value
			p[4] = &s_numSwapChains;
			p[5] = s_allocator;

			// gs-context allocator (ctx+0xB0): the shader/handle creates allocate through it
			// (FUN_18008d270: `(**(ctx+0xB0) + 8))(obj, 0x40, 0x10, name)`). Ctor leaves it null.
			*reinterpret_cast<void**>(g_gsContext + 0xB0) = s_gsAllocator;

			// The pxd shadow-state attach (DLL .rdata +0x104a18 holds the GUID; the render path
			// FUN_18008f4b0 fetches the block and writes bound-target/viewport state into it) -
			// shared with K2, same GUID bytes. See pxd/GsBringup.h.
			pxd::GsBringup::AttachCtxShadowState(window.GetD3D11DeviceContext());

			// Handle (instance) tables: 10 x 0x20-byte t_instance_tbl at ctx+0x101718 (the ctor
			// zeroes exactly 0x101718..0x101858 = 10 tables), LJ order mesh/tex/vs/ps/gs/hs/ds/
			// cs/gts/fx. Layout verified from the DLL: lookup FUN_18008d3c0 reads array@+0x00 and
			// capacity@+0x14 (vs @+0x101758, ps @+0x101778); the bitmap allocator FUN_1800858d0
			// reads bitmap@+0x08, cursor@+0x18, word-count@+0x1C. Same struct as LJ t_instance_tbl.
			{
				// LJ order mesh/tex/vs/ps/gs/hs/ds/cs/gts/fx; seeding is the shared bitmap fill
				// (pxd/GsBringup.h).
				static constexpr uint32_t kCaps[10] = {
					4096,   // mesh
					32768,  // tex
					8192,   // vs
					8192,   // ps
					8192,   // gs
					8192,   // hs
					8192,   // ds
					1024,   // cs
					8192,   // gts
					8192,   // fx
				};
				pxd::GsBringup::SeedInstanceTables(g_gsContext + 0x101718, kCaps, 10);
			}

			// Per-texture-id format table at ctx+0x1690: texture create (FUN_180086300) writes
			// `*(u32*)(*(ctx+0x1690) + (id-1)*4) = fmtbits` after allocating from the tex table
			// (cap 32768). Host-provided (same class as StF's InitTexIdTables). Zeroed, 4B/id.
			*reinterpret_cast<void**>(g_gsContext + 0x1690) = new uint32_t[32768]();
		}

		// ================================== boot ==================================

		HMODULE LoadDLL()
		{
			gameDll.reset(LoadModuleDll(DLL_NAME, L"vf2", ModuleSearch::SubdirOnly, gamePath,
				L"Place the DLL (with its rom/ and w64/ folders) in the \"vf2\" subdirectory next to YAMP.exe."));
			return gameDll.get();
		}

		void PreInitialize()
		{
			gGeneral.SetDataPath();
			gGeneral.LoadSettings();
		}

		static bool ResolveSymbolsAndPatch(void* dll, const RenderWindow& window) try
		{
			const Imports symbolMap = BuildVF2SymbolMap(dll);

			const ScopedUnprotect::Section text(static_cast<HMODULE>(dll), ".text");
			const ScopedUnprotect::Section rdata(static_cast<HMODULE>(dll), ".rdata");

			// Lets YAMP's command line reach the module's own option parser, which module_start
			// otherwise calls with an empty argv. Must be before module_start - the parse happens
			// inside it.
			ModuleArgs::Install(dll);

			// Cabinet Test / Service switches on the emulated I/O board's system port, which is
			// what makes the board's own service menu reachable. Same I/O core as the LJ modules
			// (guest 0x01C00002 -> io[9]); no-op if the frame-step pattern above did not match.
			InstallSystemSwitches(dll, symbolMap);

			auto Import = [&symbolMap](auto& var, auto symbol)
				{
					var = static_cast<std::decay_t<decltype(var)>>(symbolMap.GetSymbol(symbol));
				};

			Import(sl::sm_context, ImportSymbol::SL_CONTEXT_INSTANCE);
			Import(sl::file_open_internal, ImportSymbol::SL_FILE_OPEN);
			Import(sl::file_read, ImportSymbol::SL_FILE_READ);
			Import(sl::file_close, ImportSymbol::SL_FILE_CLOSE);
			Import(sl::handle_create_internal, ImportSymbol::SL_HANDLE_CREATE);
			Import(sl::file_handle_destroy, ImportSymbol::SL_FILE_HANDLE_DESTROY);
			Import(sl::archive_lock_wlock, ImportSymbol::ARCHIVE_LOCK_WLOCK);
			Import(sl::archive_lock_wunlock, ImportSymbol::ARCHIVE_LOCK_WUNLOCK);
			Import(sl::kernel_calloc_internal, ImportSymbol::SL_KERNEL_CALLOC);
			// NOTE: sl::file_create_internal not wired — no separate file_create found in the vf2
			// module (retail arcade module never creates files); left null deliberately.

			Import(g_gsContext, ImportSymbol::GS_CONTEXT_INSTANCE);

			// The DLL's own gs-context-pointer global (it lazily self-points; set it up front)
			uint8_t** ppGsCtx;
			Import(ppGsCtx, ImportSymbol::GS_CONTEXT_PTR);
			*ppGsCtx = g_gsContext;

			DebugLog("[vf2] slCtx=%p gsCtx=%p (gs size field=0x%X)\n",
				sl::sm_context, g_gsContext, *reinterpret_cast<uint32_t*>(g_gsContext + 8));

			// sl bring-up (identical generation to LJ StF; reuse its whole flow)
			if (sl::sm_context && sl::sm_context->handles.p_handle_buffer == nullptr)
			{
				constexpr uint32_t kHandleCapacity = 0x100000;
				if (sl::initialize() != 0) { DebugLog("[vf2] sl::initialize FAILED\n"); return false; }
				if (sl::handle_initialize(kHandleCapacity) != 0) { DebugLog("[vf2] sl::handle_initialize FAILED\n"); return false; }
			}
			PatchSl(sl::sm_context);

			FillSharedSymbols(window);
			return true;
		}
		catch (...)
		{
			DebugLog("[vf2] symbol resolution FAILED (pattern mismatch)\n");
			return false;
		}

		// ================================ game loop ================================

		// ct::initialize_module requires {size 0x10, ctx+8 == 0x30}
		struct vf2_ct_context_t
		{
			uint32_t tag_id;
			uint32_t version;
			uint32_t size_of_struct = 0x30;
			std::byte pad[36]{};
		};
		static vf2_ct_context_t s_ctContext;

		static bool GameLoop(module_func_t module_main, RenderWindow& window)
		{
			// Persistent across frames — the module keeps state in the work block (see m2ftg.h).
			static m2ftg_execute_info_t execute_info{};
			static csl_pad s_pads[2];
			static Cabinet::CoinStart s_coinStart;
			static Cabinet::PauseMenu s_pause;
			static Cabinet::CoinBinding s_coinButton;

			// (1) Read the session state ONCE, at the top, before Drive() can advance it - so pad
			// routing and the coin protocol see one stable answer for the whole frame. The order
			// of these four calls is part of the contract; see NetSession.h.
			NetSession& net_ = NetplaySession();
			const NetSession::Status netStatus = net_.GetStatus();
			const bool netplayMatch = netStatus.inMatch;
			const int32_t netplayLocalPad = netStatus.localPad;
			const bool netplayLocked = netStatus.locked;
			static int s_frame = 0;
			static int s_lastLogged = -1;

			// YLAD-era DX11 cgs_device_context: layout matches LJ's known offsets (+0x18
			// mp_sbgl_context, +0x20 mp_sbgl_deferred_context, +0x28 mp_cb_pool, +0x30 mp_up_pool —
			// confirmed by FUN_18008394D passing *(devctx+0x18) as the D3D11 context). Zeroed buffer,
			// fields mapped as the bring-up progresses.
			// The cgs_device_context stand-in - the shared shape (pxd/GsBringup.h): +0x18 the
			// immediate context (in the DX11 pxd world the sbgl ccontext IS the immediate
			// ID3D11DeviceContext; Y6 does the same reinterpret_cast in its PatchGs), +0x28/+0x30
			// the lazily-filling cb/up pools (FUN_180090660 / FUN_18008ef90), +0x38 the
			// render-state block reset_state_all FUN_180082290 fills defaults through.
			static uint8_t* s_deviceContext = nullptr;
			pxd::GsBringup::EnsureDeviceContextBlock(s_deviceContext, window.GetD3D11DeviceContext());

			PollAspectSetting(window);

			execute_info.size_of_struct = sizeof(execute_info);
			execute_info.p_device_context = reinterpret_cast<cgs_device_context*>(s_deviceContext);
			execute_info.status = 0;
			execute_info.output_texid = 0;
			execute_info.result = 0x80004005;
			execute_info.sound_volume = Cabinet::VolumeFraction();

			// Escape toggles the pause menu; while it is open, drive the MODULE'S OWN pause
			// path (status bit0 — same m2ftg protocol as StF/FV, see StF.cpp's RE notes).
			// Locked during netplay - rationale with Cabinet::PauseMenu.
			s_pause.Poll(netplayLocked);
			if (s_pause.open)
			{
				execute_info.status |= 1;
			}

			// Input: refresh the shared XInput snapshot, then evaluate each player's bindings
			// via csl_pad (Input, set up on the YAMP Controls page — shared with StF/FV).
			Input::PollPads();
			Cabinet::RoutePads(s_pads, netplayMatch, netplayLocalPad);
			for (int i = 0; i < 2; i++)
			{
				memcpy(&execute_info.pad[i], &s_pads[i], 0xE0);
				execute_info.pad[i].m_port = static_cast<unsigned int>(i);
				execute_info.pad[i].m_user_id = i;
				execute_info.pad[i].m_is_connected = true;
			}

			// Button assignments: the fixed module-facing table - see Cabinet::FillAssignTable
			// (the one-slot-shift bug it guards against was found in THIS host).
			Cabinet::FillAssignTable(execute_info);

			// Dedicated coin binding -> the coin status bit (host->module bit5), as in StF/FV.
			// Suppression rationale is with Cabinet::CoinBinding.
			if (s_coinButton.Pressed(g_isFreeplay || s_pause.open || netplayLocked))
			{
				execute_info.status |= 0x20;
			}

			// Cabinet TEST / SERVICE. Suppressed while paused, and for the whole of a netplay
			// session: neither is in the transmitted pad, so honouring them would drop one
			// machine into the operator's menu (or feed it a service credit) while the other
			// carried on playing.
			//
			// TODO(vf2): LEAVING the service menu corrupts the 3D geometry until a restart -
			// models render, but distorted. StF and FV exit the same menu cleanly, so this is a
			// gap in THIS host, not in the switch wiring or the emulated I/O board.
			//
			// What is established: exiting soft-resets the emulated board. The ROM's own
			// frame_counter (emulated RAM 0x500020) runs continuously to ~1215 and then restarts
			// from 0 on the exit press - measured, not inferred. So the i960 re-runs its init and
			// re-uploads its geometry believing it is on a fresh machine, while this host's
			// boot-time state is never rebuilt; the two then disagree about what is loaded, which
			// is why the damage is geometry-shaped rather than a crash or a blank screen.
			//
			// Ruled out, so nobody re-walks them: the I/O hook resolves the right board pointer
			// (0x180880020, the same global the port reader and DIP injector use) and only ever
			// ANDs io[9]; and the upload-budget timebase is NOT a stale load-time stamp - it is
			// TaskM2E's own field, re-stamped at the top of every frame step (0x180051045).
			//
			// Next measurement: whether the DLL's boot-state dword (0x641890) stays at 2 while
			// the ROM's counter restarts. If it does, the DLL's board init never re-ran, and the
			// fix is to notice the counter going backwards and rebuild whatever this host sets up
			// at boot. Reproduces headlessly in ~3 minutes by scripting two short TEST presses.
			Cabinet::PollSystemSwitches(s_pause.open || netplayLocked, SetSystemSwitches);

			// Arcade coin/start dance - Cabinet::CoinStart (see StF.cpp / YLAD method_pre_render
			// for the reference logic; the full write-up is with the class). During a netplay
			// round it runs AFTER Step() instead - see below.
			if (!g_isFreeplay && !s_pause.open && !netplayMatch)
			{
				s_coinStart.Run(execute_info.pad[0], execute_info.status);
			}

			// (2) Poll the plugin and run the round-start state machine, then (3) decide whether
			// the emulator may advance. Step() also overwrites BOTH pads with the transmitted
			// inputs, which is why it runs after the local pad fill above.
			net_.Drive();
			const bool advanceFrame = net_.Step(execute_info);

			// Now that Step() has written both pads from the transmitted inputs, run the arcade
			// coin/start protocol off that synchronised state - every input it consumes is
			// identical on both machines now, so the coin bit it raises is identical too.
			if (netplayMatch && !g_isFreeplay)
			{
				s_coinStart.Run(execute_info.pad[0], execute_info.status);
			}

			window.BeginFrame();
			window.NewImGuiFrame();
			if (s_pause.open)
			{
				if (!DrawPauseMenu(window, s_pause.open))
				{
					return false; // Quit picked
				}
			}

			// (4) module_main only runs when the session says the frame may advance; a stalled
			// lockstep frame skips it entirely, which IS the stall. output_texid is zeroed every
			// frame and only module_main fills it in, so a stall would leave it 0 - and 0 is not
			// a valid handle. Remember the last good id so a stall re-presents the previous frame.
			static unsigned int s_lastOutputTexId = 0;
			int funcResult = 0;
			if (advanceFrame)
			{
				funcResult = module_main(sizeof(execute_info), &execute_info);
				if (execute_info.output_texid != 0)
				{
					s_lastOutputTexId = execute_info.output_texid;
				}
				net_.EndFrame();
			}
			else
			{
				execute_info.output_texid = s_lastOutputTexId;
			}

			s_coinStart.NoteStatus(execute_info.status);

			const int interesting = (execute_info.status & ~0x20) | (execute_info.work_kind << 16);
			if (interesting != s_lastLogged && s_frame < 5000)
			{
				DebugLog("[vf2] frame=%d status=0x%X result=0x%X kind=%d texid=%u ret=%d\n",
					s_frame, execute_info.status, execute_info.result,
					execute_info.work_kind, execute_info.output_texid, funcResult);
				s_lastLogged = interesting;
			}
			s_frame++;

			// Display blit: resolve the game's output texture (OUR tex instance table at
			// gsCtx+0x101738; the object is literally named "output_tex") to its D3D11 SRV.
			// The SRV field offset inside the sbgl sub-objects isn't pinned yet, so probe COM
			// pointers with a guarded QueryInterface once and cache the result.
			if (execute_info.output_texid != 0)
			{
				static ID3D11ShaderResourceView* s_displaySrv = nullptr;
				static bool s_srvProbed = false;
				if (!s_srvProbed && s_frame >= 60)
				{
					s_srvProbed = true;
					void** tbl = *reinterpret_cast<void***>(g_gsContext + 0x101738);
					uint8_t* tex = tbl != nullptr ? static_cast<uint8_t*>(tbl[execute_info.output_texid - 1]) : nullptr;
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
							for (size_t subOff = 0; subOff <= 0x38 && s_displaySrv == nullptr; subOff += 8)
								s_displaySrv = tryQiSrv(*reinterpret_cast<void**>(sub + subOff));
							if (s_displaySrv != nullptr)
							{
								DebugLog("[vf2] display SRV found via tex+0x%zX (srv=%p)\n", texOff, s_displaySrv);
								break;
							}
						}
					}
					if (s_displaySrv == nullptr) DebugLog("[vf2] display SRV NOT found\n");
				}
				if (s_displaySrv != nullptr)
				{
					window.BlitGameFrame(s_displaySrv);
				}
			}

			window.RenderImGui();
			window.EndFrame();

			HRESULT hr = window.GetSwapChain()->Present(1, 0);
			if (FAILED(hr)) return false;

			return funcResult == 0;
		}

		void Run(RenderWindow& window)
		{
			const auto module_start = reinterpret_cast<module_func_t>(GetProcAddress(gameDll.get(), "module_start"));
			THROW_LAST_ERROR_IF_NULL(module_start);
			module_func_t module_main = nullptr;

			if (!ResolveSymbolsAndPatch(gameDll.get(), window))
			{
				DebugLog("[vf2] init failed\n");
				return;
			}
			DebugLog("[vf2] symbols + sl init OK\n");

			Cri criware;

			struct sl_module_t
			{
				size_t size = sizeof(decltype(*this));
				sl::context_t* context;
			} sl_module;
			sl_module.context = sl::sm_context;

			struct gs_module_t
			{
				size_t size = 0x58;
				void* context;
				uint8_t pad[72]{};
			} gs_module;
			static_assert(sizeof(gs_module_t) == 0x58);
			gs_module.context = g_gsContext;

			const struct ct_module_t
			{
				size_t size = sizeof(decltype(*this));
				vf2_ct_context_t* context = &s_ctContext;
			} ct_module;

			struct module_params_t
			{
				size_t size = sizeof(decltype(*this));
				const sl_module_t* sl_module;
				const gs_module_t* gs_module;
				const ct_module_t* ct_module;
				const icri* cri_ptr;
				const char* root_path;
				module_func_t* module_main;
				m2ftg_config_t config{};
			} params;
			static_assert(offsetof(module_params_t, config) == 0x38);
			static_assert(sizeof(module_params_t) == 0x1048);

			const std::string utf8Path = gamePath.u8string();

			params.sl_module = &sl_module;
			params.gs_module = &gs_module;
			params.ct_module = &ct_module;
			params.cri_ptr = &criware;
			params.module_main = &module_main;
			params.root_path = utf8Path.c_str();

			// Dip switches from the YAMP settings, same policy as m2ftg::Run (module_start
			// copies the config once, so changes need a restart). is_vf20 is
			// the module's own VF2-only switches.
			const auto* settings = gGeneral.GetSettings();
			params.config.kind = 0;       // vf2 -> "%s/rom/vf2_rom.par"
			params.config.difficulty = settings->m_m2Difficulty <= 3 ? static_cast<uint8_t>(settings->m_m2Difficulty) : 1;
			params.config.country = settings->m_m2Country <= 2 ? static_cast<uint8_t>(settings->m_m2Country) : 0;
			params.config.is_freeplay = settings->m_m2Freeplay ? 1 : 0;
			params.config.is_vs_mode = settings->m_m2VersusMode ? 1 : 0;
			params.config.is_vf20 = settings->m_vf2Version20 ? 1 : 0;
			g_isFreeplay = params.config.is_freeplay != 0;

			// VF2 is a native 4:3 Model 2 arcade game like StF/FV; apply the shared
			// presentation setting (re-applied live from GameLoop when it changes).
			ApplyAspectSetting(window, settings->m_m2Aspect);

			DebugLog("[vf2] calling module_start (root=%s)\n", utf8Path.c_str());
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
			DebugLog("[vf2] module_start returned %d, module_main=%p\n", startResult, module_main);

			if (startResult < 0 || module_main == nullptr) return;

			int64_t frameTimeTicks;
			{
				if (!gGeneral.GetSettings()->m_enableFpsCap)
				{
					frameTimeTicks = 0;
				}
				else
				{
					LARGE_INTEGER frequency;
					QueryPerformanceFrequency(&frequency);
					frameTimeTicks = (frequency.QuadPart * 50) / 3;
				}
			}

			LARGE_INTEGER lastTime;
			QueryPerformanceCounter(&lastTime);
			while (!window.IsShuttingDown())
			{
				if (!GameLoop(module_main, window)) break;

				LARGE_INTEGER currentTime;
				do
				{
					QueryPerformanceCounter(&currentTime);
				} while (((currentTime.QuadPart - lastTime.QuadPart) * 1000) < frameTimeTicks);
				lastTime = currentTime;
			}
		}
	}
}
