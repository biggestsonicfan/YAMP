#include "K2Host.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <filesystem>
#include <memory>
#include <string>

#include "ImportSymbols.h"

#include "../../criware/Cri.h"
#include "../../pxd/LJ/sl.h"            // pxd::sl — the head of the context, and the primitives
#include "../../pxd/K2/sl.h"            // pxd::K2 — THIS generation's context layout (0xF3C0)
#include "../../pxd/LJ/sl_internal.h"   // handle_internal_buffer_t (the 8-byte queue node)
#include "../m2ftg.h"                 // m2ftg_config_t (0x100C) — unchanged in this generation
#include "../ModuleArgs.h"
#include "../DisplayModes.h"
#include "../../input/Input.h"
#include "../HostUI.h"
#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
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

			static bool s_pauseMenuOpen = false;
			{
				static bool s_escWasDown = false;
				const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
				if (escDown && !s_escWasDown) s_pauseMenuOpen = !s_pauseMenuOpen;
				s_escWasDown = escDown;
			}
			if (s_pauseMenuOpen) execute_info.status |= 1;

			// Input: refresh the shared XInput snapshot, then evaluate each player's bindings via
			// csl_pad — the same Controls page every other m2ftg host uses. This generation embeds
			// the plain csl_pad in execute_info, so the whole struct goes across rather than LJ's
			// 0xE0 prefix.
			Input::PollPads();
			static pxd::csl_pad s_pads[2];
			s_pads[0].set_state(0);
			s_pads[1].set_state(1);
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

			// Dedicated coin binding -> the coin status bit (host->module bit5). Meaningless in
			// freeplay, and swallowed while the pause menu is open.
			{
				static bool s_coinWasDown[2] = {};
				for (int p = 0; p < 2; p++)
				{
					const bool down = Input::ActionDown(p, Input::Action_Coin);
					if (down && !s_coinWasDown[p] && !s_isFreeplay && !s_pauseMenuOpen)
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

			const int funcResult = g_moduleMain(sizeof(execute_info), &execute_info);

			const int interesting = execute_info.status | (funcResult << 16);
			if (interesting != s_lastLogged && s_frame < 5000)
			{
				DebugLogFile("[m2ftg::K2] frame=%d status=0x%X result=0x%X texid=%u ret=0x%X\n",
					s_frame, execute_info.status, execute_info.result,
					execute_info.output_texid, funcResult);
				s_lastLogged = interesting;
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
