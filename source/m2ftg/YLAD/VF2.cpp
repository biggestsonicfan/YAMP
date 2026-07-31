#include "VF2.h"

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
#include "../../pxd/Imports.h"
#include "../ImportSymbols.h"
#include "../HostUI.h" // ApplyAspectSetting / DrawPauseMenu (shared m2ftg host helpers)
#include "../../input/Input.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../RenderWindow.h"

#include "../../Utils/Patterns.h"
#include "../../Utils/MemoryMgr.h"
#include "../../Utils/ScopedUnprotect.hpp"
#include "../../DebugLog.h"

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

		template<typename T = void>
		static auto get_module_pattern(void* module, std::string_view pattern_string, ptrdiff_t offset = 0)
		{
			return hook::txn::pattern(module, std::move(pattern_string)).get_first<T>(offset);
		}

		static void* immediate(void* addr)
		{
			void* val;
			Memory::ReadOffsetValue(addr, val);
			return val;
		}

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
		static void* __fastcall HostAlloc(void* /*self*/, size_t size, size_t align)
		{
			void* p = _aligned_malloc(size, align != 0 ? align : 16);
			if (p != nullptr) memset(p, 0, size);
			return p;
		}
		static void __fastcall HostFree(void* /*self*/, void* p)
		{
			_aligned_free(p);
		}
		static void __fastcall HostAllocNoop(void*) {}
		static void* s_allocatorVtbl[4] = {
			reinterpret_cast<void*>(&HostAlloc),
			reinterpret_cast<void*>(&HostFree),
			reinterpret_cast<void*>(&HostAllocNoop),
			reinterpret_cast<void*>(&HostAllocNoop),
		};
		static void* s_allocator[2] = { s_allocatorVtbl, nullptr };

		// gs-context allocator (ctx+0xB0): vtbl slot 1 = alloc, slot 2 = free
		static void* s_gsAllocatorVtbl[4] = {
			reinterpret_cast<void*>(&HostAllocNoop),
			reinterpret_cast<void*>(&HostAlloc),
			reinterpret_cast<void*>(&HostFree),
			reinterpret_cast<void*>(&HostAllocNoop),
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
			const uint32_t packedDims = (1280 - 1) | ((720 - 1) << 14);
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

			// sbgl attaches a shadow-state block to the ID3D11DeviceContext via SetPrivateData
			// with a pxd GUID (DLL .rdata +0x104a18; bytes embedded below). The render path
			// (FUN_18008f4b0) does GetPrivateData(guid, 8, &blockPtr) and writes bound-target/
			// viewport state into the block — normally the host device-start attaches it.
			{
				static const GUID kPxdCtxGuid =
					{ 0xa84b07f7, 0x3bd0, 0x4c4e, { 0x89, 0x90, 0xe9, 0xd5, 0xaf, 0x6e, 0xfb, 0xa2 } };
				static uint8_t s_ctxShadowState[0x1000]{};
				void* blockPtr = s_ctxShadowState;
				window.GetD3D11DeviceContext()->SetPrivateData(kPxdCtxGuid, sizeof(blockPtr), &blockPtr);
			}

			// Handle (instance) tables: 10 x 0x20-byte t_instance_tbl at ctx+0x101718 (the ctor
			// zeroes exactly 0x101718..0x101858 = 10 tables), LJ order mesh/tex/vs/ps/gs/hs/ds/
			// cs/gts/fx. Layout verified from the DLL: lookup FUN_18008d3c0 reads array@+0x00 and
			// capacity@+0x14 (vs @+0x101758, ps @+0x101778); the bitmap allocator FUN_1800858d0
			// reads bitmap@+0x08, cursor@+0x18, word-count@+0x1C. Same struct as LJ t_instance_tbl.
			{
				struct vf2_instance_tbl
				{
					void** tbl;
					uint64_t* free_tbl;
					uint32_t status;
					uint32_t max;
					uint32_t free_top;
					uint32_t free_words;
				};
				static_assert(sizeof(vf2_instance_tbl) == 0x20);

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
				for (int i = 0; i < 10; i++)
				{
					auto* t = reinterpret_cast<vf2_instance_tbl*>(g_gsContext + 0x101718 + i * 0x20);
					const uint32_t cap = kCaps[i];
					const uint32_t words = (cap + 63) / 64;
					t->tbl = new void* [cap]();
					t->free_tbl = new uint64_t[words];
					for (uint32_t w = 0; w < words; w++) t->free_tbl[w] = ~0ull;
					t->status = 0;
					t->max = cap;
					t->free_top = 0;
					t->free_words = words;
				}
			}

			// Per-texture-id format table at ctx+0x1690: texture create (FUN_180086300) writes
			// `*(u32*)(*(ctx+0x1690) + (id-1)*4) = fmtbits` after allocating from the tex table
			// (cap 32768). Host-provided (same class as StF's InitTexIdTables). Zeroed, 4B/id.
			*reinterpret_cast<void**>(g_gsContext + 0x1690) = new uint32_t[32768]();
		}

		// ================================== boot ==================================

		HMODULE LoadDLL()
		{
			DWORD dwSize = GetCurrentDirectoryW(0, nullptr);
			auto buf = std::make_unique<wchar_t[]>(dwSize);
			GetCurrentDirectoryW(dwSize, buf.get());
			gamePath.assign(buf.get());
			gamePath /= L"vf2";

			// Locate the DLL as a FILE first: the integrity check has to run before LoadLibrary,
			// or a module YAMP is about to reject has already run its DllMain.
			const std::filesystem::path dllFile = gamePath / DLL_NAME;
			if (!std::filesystem::is_regular_file(dllFile))
			{
				const std::wstring str(L"Could not load " + std::wstring(DLL_NAME) +
					L"!\n\nPlace the DLL (with its rom/ and w64/ folders) in the \"vf2\" subdirectory next to YAMP.exe.");
				MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
				return nullptr;
			}

			gGeneral.SetDLLName(WcharToUTF8(DLL_NAME));

			// Hard gate: known-good module checksum + an installed copy of Yakuza: Like a Dragon.
			if (!Verify::CheckBeforeLoad(gGeneral.GetGameId(), dllFile))
			{
				return nullptr;
			}

			gameDll.reset(LoadLibraryW(dllFile.c_str()));
			if (gameDll == nullptr)
			{
				const std::wstring str(L"Could not load " + std::wstring(DLL_NAME) +
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
			const Imports symbolMap = BuildVF2SymbolMap(dll);

			const ScopedUnprotect::Section text(static_cast<HMODULE>(dll), ".text");
			const ScopedUnprotect::Section rdata(static_cast<HMODULE>(dll), ".rdata");

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
			static bool s_startScreen = false;
			static bool s_coinPending = false;
			static bool s_startToggle = false;
			static int s_frame = 0;
			static int s_lastLogged = -1;

			// YLAD-era DX11 cgs_device_context: layout matches LJ's known offsets (+0x18
			// mp_sbgl_context, +0x20 mp_sbgl_deferred_context, +0x28 mp_cb_pool, +0x30 mp_up_pool —
			// confirmed by FUN_18008394D passing *(devctx+0x18) as the D3D11 context). Zeroed buffer,
			// fields mapped as the bring-up progresses.
			static uint8_t* s_deviceContext = []() {
				uint8_t* p = new uint8_t[0x40000]();
				return p;
			}();
			// In the DX11 pxd world the sbgl ccontext IS the immediate ID3D11DeviceContext
			// (Y6 does the same reinterpret_cast in its PatchGs).
			*reinterpret_cast<void**>(s_deviceContext + 0x18) = window.GetD3D11DeviceContext();
			// Host-provided render-state block (reset_state_all FUN_180082290 fills defaults at
			// blk+0x670 etc.; the LJ equivalent lived at devctx+0x12C98, here it's devctx+0x38).
			// Zeroed => optional callback ptr inside stays null and is skipped.
			static uint8_t* s_renderStateBlock = new uint8_t[0x8000]();
			*reinterpret_cast<void**>(s_deviceContext + 0x38) = s_renderStateBlock;
			// mp_cb_pool (devctx+0x28): size-bucketed lazily-filling buffer table
			// (FUN_180090660 indexes [bucket*0x20 + sizeclass]*8 and creates buffers on miss via
			// FUN_18008ef90; +0x08 = live count). A zeroed table self-populates. +0x30 = mp_up_pool,
			// same treatment.
			static uint8_t* s_cbPool = new uint8_t[0x40000]();
			static uint8_t* s_upPool = new uint8_t[0x40000]();
			*reinterpret_cast<void**>(s_deviceContext + 0x28) = s_cbPool;
			*reinterpret_cast<void**>(s_deviceContext + 0x30) = s_upPool;

			// Live-apply the aspect-ratio setting (same poll as m2ftg::GameLoop; the ImGui
			// Apply button only writes the settings struct).
			{
				static uint32_t s_lastAspect = UINT32_MAX;
				if (gGeneral.GetSettings()->m_m2Aspect != s_lastAspect)
				{
					s_lastAspect = gGeneral.GetSettings()->m_m2Aspect;
					ApplyAspectSetting(window, s_lastAspect);
				}
			}

			execute_info.size_of_struct = sizeof(execute_info);
			execute_info.p_device_context = reinterpret_cast<cgs_device_context*>(s_deviceContext);
			execute_info.status = 0;
			execute_info.output_texid = 0;
			execute_info.result = 0x80004005;
			// Master volume: the module's own field, so its mixer does the attenuation (see
			// YAMPSettings::m_volumePercent). 100% = 1.0f, exactly what this host passed before.
			execute_info.sound_volume =
				static_cast<float>(gGeneral.GetSettings()->m_volumePercent) / 100.0f;

			// Escape toggles the pause menu; while it is open, drive the MODULE'S OWN pause
			// path (status bit0 — same m2ftg protocol as StF/FV, see StF.cpp's RE notes).
			static bool s_pauseMenuOpen = false;
			{
				static bool s_escWasDown = false;
				const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
				if (escDown && !s_escWasDown)
				{
					s_pauseMenuOpen = !s_pauseMenuOpen;
				}
				s_escWasDown = escDown;
			}
			if (s_pauseMenuOpen)
			{
				execute_info.status |= 1;
			}

			// Input: refresh the shared XInput snapshot, then evaluate each player's bindings
			// via csl_pad (Input, set up on the YAMP Controls page — shared with StF/FV).
			Input::PollPads();
			s_pads[0].set_state(0);
			s_pads[1].set_state(1);
			for (int i = 0; i < 2; i++)
			{
				memcpy(&execute_info.pad[i], &s_pads[i], 0xE0);
				execute_info.pad[i].m_port = static_cast<unsigned int>(i);
				execute_info.pad[i].m_user_id = i;
				execute_info.pad[i].m_is_connected = true;
			}

			// Button assignments: the FIXED module-facing table (slot order A, B, Y, X, LT, LB,
			// RT, RB). Remapping happens host-side in csl_pad::set_state, which routes each
			// player's bound inputs onto the button bit carrying the wanted combo — so this must
			// be Input::MODULE_ASSIGN, the table those routes assume (the old raw template
			// here shifted every combo button by one slot).
			for (int p = 0; p < 2; p++)
			{
				for (int i = 0; i < 8; i++)
				{
					execute_info.assign[p][i] = Input::MODULE_ASSIGN[i];
				}
			}

			// Dedicated coin binding: a press becomes the coin status bit (host->module bit5),
			// as in StF/FV. Meaningless in freeplay, swallowed while the pause menu is open.
			{
				static bool s_coinWasDown[2] = {};
				for (int p = 0; p < 2; p++)
				{
					const bool down = Input::ActionDown(p, Input::Action_Coin);
					if (down && !s_coinWasDown[p] && !g_isFreeplay && !s_pauseMenuOpen)
					{
						execute_info.status |= 0x20;
					}
					s_coinWasDown[p] = down;
				}
			}

			// Arcade coin/start dance (see StF.cpp / YLAD method_pre_render for the reference
			// logic). ONLY meaningful with the freeplay dip switch OFF — with is_freeplay=1 the
			// module takes START directly and there is no coin to insert; the dance would just
			// swallow the player's first press.
			if (!g_isFreeplay && !s_pauseMenuOpen)
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
			if (s_pauseMenuOpen)
			{
				if (!DrawPauseMenu(window, s_pauseMenuOpen))
				{
					return false; // Quit picked
				}
			}

			const int funcResult = module_main(sizeof(execute_info), &execute_info);

			s_startScreen = (execute_info.status & 0x40) != 0;

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
			// copies the config once, so changes need a restart). is_vf20/is_disable_pepsi are
			// the module's own VF2-only switches.
			const auto* settings = gGeneral.GetSettings();
			params.config.kind = 0;       // vf2 -> "%s/rom/vf2_rom.par"
			params.config.difficulty = settings->m_m2Difficulty <= 3 ? static_cast<uint8_t>(settings->m_m2Difficulty) : 1;
			params.config.country = settings->m_m2Country <= 2 ? static_cast<uint8_t>(settings->m_m2Country) : 0;
			params.config.is_freeplay = settings->m_m2Freeplay ? 1 : 0;
			params.config.is_vs_mode = settings->m_m2VersusMode ? 1 : 0;
			params.config.is_vf20 = settings->m_vf2Version20 ? 1 : 0;
			params.config.is_disable_pepsi = settings->m_vf2DisablePepsi ? 1 : 0;
			g_isFreeplay = params.config.is_freeplay != 0;

			// VF2 is a native 4:3 Model 2 arcade game like StF/FV; apply the shared
			// presentation setting (re-applied live from GameLoop when it changes).
			ApplyAspectSetting(window, settings->m_m2Aspect);

			DebugLog("[vf2] calling module_start (root=%s)\n", utf8Path.c_str());
			const int startResult = module_start(sizeof(params), &params);
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
