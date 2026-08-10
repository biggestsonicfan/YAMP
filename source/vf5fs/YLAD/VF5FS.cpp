#include "VF5FS.h"
#include "../../ModuleLoad.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <filesystem>
#include <memory>
#include <string>

#include "ImportSymbols.h"

#include "../../criware/Cri.h"
// VF5FS shares the m2ftg titles' P/K/G control scheme, so it shares their bindings and the
// pause/aspect UI rather than duplicating them (the LJ VF5FS host does the same).
#include "../../input/Input.h"
#include "../../m2ftg/HostUI.h"
#include "../../pxd/LJ/sl.h"
// For the cgs_device_context TYPE only (execute_info holds a pointer to one). This host does NOT
// use the pxd gs context itself — that one is the Lost Judgment generation (0x388A00); ours is
// the module's own embedded 0x3820C0 template, handled as an opaque block.
#include "../../pxd/LJ/gs.h"
#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../Utils/ScopedUnprotect.hpp"
#include "../../wil/resource.h"

namespace vf5fs
{
	namespace YLAD
	{
		// The sl layer is shared with every other pxd host (source/pxd). NOTE the gs side is NOT:
		// this generation's gs context is 0x3820C0 and comes embedded in the DLL, so there is no
		// pxd::gs bring-up here — see FillSharedSymbols below.
		using namespace pxd;

		static const wchar_t* DLL_NAME = L"vf5fs-pxd-w64-retail.dll";
		static const wchar_t* DLL_SUBDIR = L"vf5fs";

		// ct::initialize_module (FUN_1802297E0) only checks {module size 0x10, context+8 == 0x30},
		// and nothing in YAMP reads it, so an opaque block of the right size is enough.
		struct ct_context_t
		{
			uint32_t tag_id;
			uint32_t version;
			uint32_t size_of_struct = sizeof(ct_context_t);
			std::byte unknown[0x30 - 0x0C]{};
		};
		static_assert(sizeof(ct_context_t) == 0x30);

		static ct_context_t s_ctContext;

		// The per-frame block, size-gated by module_main (`if (param_1 == 0x680)` at FUN_1801F4D10).
		//
		// This is Lost Judgment's VF5FS execute_info MINUS its last 0x10 bytes: LJ's 0x690 layout
		// ends with the two int[2] button-assignment arrays at +0x680/+0x688, which this older build
		// does not have. Everything else lines up, and module_main's own code confirms the fields
		// this host writes:
		//   +0x10  status  — bit0 = pause (module skips its update; drives the host pause menu)
		//   +0x14  result  — the module writes S_OK over the host's E_FAIL preset on success
		//   +0x18  output_texid
		//   +0x663 byte, THE MASTER VOLUME on a 0..20 scale. module_main reads it every frame
		//          (`bVar1 = *(byte *)(param_2 + 0x663)`), compares against the module's current
		//          setting and pushes it through FUN_18019CB60 on change. This is the LJ-era
		//          mechanism, NOT the +0x1C sound_volume float the m2ftg/Y6 modules use — leaving
		//          it 0 mutes every cue while the rest of the audio path works (the bug that cost a
		//          session on the LJ host; see [[vf5fs-lj-hosting]]).
		//
		// UNVERIFIED FOR THIS DLL: the pad block at +0x20 with a 0x190 stride is inherited from the
		// LJ layout on the strength of the 0x680-vs-0x690 match. It has NOT been confirmed against
		// this module's own pad reader. Confirm before trusting input — a wrong stride here reads
		// plausible-looking garbage rather than failing loudly.
		struct alignas(16) execute_info_t
		{
			size_t size_of_struct;
			cgs_device_context* p_device_context;
			int status;
			int result;
			unsigned int output_texid;
			float sound_volume;          // the m2ftg/Y6 field; this generation ignores it
			lj_pad_t pad[2];
			std::byte unmapped[0x660 - 0x340];
			std::byte work_pre[0x663 - 0x660];
			uint8_t sound_volume_level;  // +0x663, 0..20
			std::byte tail[0x680 - 0x664];
		};
		static_assert(sizeof(execute_info_t) == 0x680);
		static_assert(offsetof(execute_info_t, pad) == 0x20);
		static_assert(offsetof(execute_info_t, sound_volume_level) == 0x663);

		// ============================== host gs pieces =============================
		//
		// The gs context is the DLL's OWN embedded template (0x3820C0), constructed by its CRT
		// init; the host only fills the fields the engine expects a host to provide. Every offset
		// below was established for the VF2 module (source/m2ftg/YLAD/VF2.cpp) and applies here
		// because the two DLLs are the same engine build — but they are INHERITED, not re-verified
		// against this DLL. If bring-up faults inside gs, re-check these first.
		//
		// TODO: once this host boots, factor this block and VF2's copy into one YLAD-era gs
		// bring-up. Kept duplicated for now so a first bring-up cannot destabilise working VF2.
		static uint8_t* g_gsContext = nullptr;

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

		// shared-symbol slot [5] (g_p_allocator): {[0]=alloc(this,size,align), [1]=free}
		static void* s_allocatorVtbl[4] = {
			reinterpret_cast<void*>(&HostAlloc),
			reinterpret_cast<void*>(&HostFree),
			reinterpret_cast<void*>(&HostAllocNoop),
			reinterpret_cast<void*>(&HostAllocNoop),
		};
		static void* s_allocator[2] = { s_allocatorVtbl, nullptr };

		// gs context +0xB0: vtbl slot 1 = alloc, slot 2 = free
		static void* s_gsAllocatorVtbl[4] = {
			reinterpret_cast<void*>(&HostAllocNoop),
			reinterpret_cast<void*>(&HostAlloc),
			reinterpret_cast<void*>(&HostFree),
			reinterpret_cast<void*>(&HostAllocNoop),
		};
		static void* s_gsAllocator[2] = { s_gsAllocatorVtbl, nullptr };

		static uint8_t s_deviceNative[0x2000]{};
		static uint8_t s_swapChain[0x400]{};
		static int s_numSwapChains = 1;

		static void FillSharedSymbols(const RenderWindow& window)
		{
			*reinterpret_cast<void**>(s_deviceNative + 0x00) = window.GetD3D11Device();
			*reinterpret_cast<void**>(s_deviceNative + 0x08) = window.GetD3D11DeviceContext();

			// cswap_chain: +0x00 IDXGISwapChain*, +0x20 packed color dims, +0x40 packed depth dims
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

			*reinterpret_cast<void**>(g_gsContext + 0xB0) = s_gsAllocator;

			// sbgl's shadow-state block, attached to the ID3D11DeviceContext under a pxd GUID —
			// normally the host's device-start does this.
			{
				static const GUID kPxdCtxGuid =
					{ 0xa84b07f7, 0x3bd0, 0x4c4e, { 0x89, 0x90, 0xe9, 0xd5, 0xaf, 0x6e, 0xfb, 0xa2 } };
				static uint8_t s_ctxShadowState[0x1000]{};
				void* blockPtr = s_ctxShadowState;
				window.GetD3D11DeviceContext()->SetPrivateData(kPxdCtxGuid, sizeof(blockPtr), &blockPtr);
			}

			// Handle (instance) tables: 10 x 0x20-byte t_instance_tbl at ctx+0x101718, order
			// mesh/tex/vs/ps/gs/hs/ds/cs/gts/fx.
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
					auto* t = reinterpret_cast<instance_tbl*>(g_gsContext + 0x101718 + i * 0x20);
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

			// Per-texture-id format table at ctx+0x1690 (4 bytes per id, tex table cap 32768).
			*reinterpret_cast<void**>(g_gsContext + 0x1690) = new uint32_t[32768]();
		}

		// ---------------------- primitive_initialize (host-provided) ----------------------
		//
		// gs+0x1408 .. gs+0x1450 is a block of ten pointers the gs constructor (FUN_180233760)
		// only ZEROES. Nothing in the module ever writes them: in the real game they are filled
		// by pxd::gs::primitive_initialize, which lives in the HOST (YakuzaLikeADragon.exe).
		// YAMP is the host, so filling them is ours to do — the same division of labour as
		// pxd::gs::primitive_initialize for the Lost Judgment generation (source/pxd/gs.cpp).
		//
		// Leaving them null is an immediate null-deref inside the module: FUN_1801FBE00 selects
		// gs+0x1438 for primitive kind 0xF and gs+0x1440 for kind 0xE, then reads [obj+0x10].
		//
		// EVERY constant below was captured from a live Yakuza: Like a Dragon under x64dbg
		// (host gs context found by searching for its ctor signature 'LBgs' + version 0x40601 +
		// size 0x3820C0), NOT inferred:
		//
		//   gs+0x1408/10/18  p_vb_sphere[0..2]   flags 0x101, 0x1B00 bytes, count 3
		//   gs+0x1420/28/30  p_vb_capsule[0..2]  flags 0x101, 0x2700 bytes, count 4
		//   gs+0x1438        p_ib_quad           flags 0x201, 0x900 bytes, 1152 indices (192 x 6)
		//   gs+0x1440        p_ib_fan            flags 0x201, 0xC00 bytes, 1536 indices (512 x 3)
		//   gs+0x1448        p_ib_rect           flags 0x201, 8 bytes,        4 indices (ONE rect)
		//   gs+0x1450        unidentified        flags 0x401, 0x20 bytes, count 0, NO resource,
		//                                        and its +0x18 is self+0x20 rather than self+0x30 —
		//                                        a different object shape. Left null: nothing on
		//                                        the faulting path touches it. Revisit if it faults.
		//
		// The sphere/capsule vertex buffers are debug-draw geometry. The LJ host explicitly nulls
		// them and renders correctly, so they stay null here too until something faults on them.
		struct alignas(16) gs_buffer_t
		{
			uint64_t alive;              // +0x00  always 1 in the capture
			uint32_t flags;              // +0x08  0x101 = VB, 0x201 = IB
			uint32_t byte_size;          // +0x0C
			gs_buffer_t* self;           // +0x10  the object's OWN address — this is the whole of
			                             //        the "id" the device-context state cache at
			                             //        devctx+0xF98 compares against, nothing more
			void* sub;                   // +0x18  = (uint8_t*)this + 0x30
			uint32_t element_count;      // +0x20  indices for an IB
			uint32_t reserved;           // +0x24  0xFFFFFFFF in every captured object
			std::byte gap[0x30 - 0x28]{};
			// the embedded sub-object at +0x30
			uint64_t sub_zero0;          // +0x30
			uint64_t sub_zero8;          // +0x38
			ID3D11Buffer* resource;      // +0x40  (sub+0x10) — vtable confirmed inside d3d11.dll
			uint32_t sub_flags;          // +0x48  (sub+0x18) mirrors +0x08
			uint32_t sub_size;           // +0x4C  mirrors +0x0C
			std::byte tail[0x80 - 0x50]{};
		};
		static_assert(sizeof(gs_buffer_t) == 0x80);
		static_assert(offsetof(gs_buffer_t, self) == 0x10);
		static_assert(offsetof(gs_buffer_t, sub) == 0x18);
		static_assert(offsetof(gs_buffer_t, element_count) == 0x20);
		static_assert(offsetof(gs_buffer_t, resource) == 0x40);
		static_assert(offsetof(gs_buffer_t, sub_flags) == 0x48);

		static gs_buffer_t s_primitiveBuffers[3];

		// Builds one immutable R16_UINT index buffer plus its wrapper, and returns the wrapper.
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
				DebugLogFile("[vf5fs::YLAD] %s index buffer creation FAILED (hr=0x%08X)\n", name, hr);
				return nullptr;
			}

			out.alive = 1;
			out.flags = 0x201;               // index buffer
			out.byte_size = byteSize;
			out.self = &out;                 // the id IS the address
			out.sub = reinterpret_cast<uint8_t*>(&out) + 0x30;
			out.element_count = count;
			out.reserved = 0xFFFFFFFF;
			out.resource = buffer;
			out.sub_flags = out.flags;
			out.sub_size = byteSize;

			DebugLogFile("[vf5fs::YLAD] %s: %u indices (%u bytes), wrapper=%p buffer=%p\n",
				name, count, byteSize, static_cast<void*>(&out), static_cast<void*>(buffer));
			return &out;
		}

		// ------------------------- the upload pool at devctx+0x30 -------------------------
		//
		// FUN_18023D330 is the up-pool suballocator; FUN_180244020 calls it and memcpys straight
		// into the result, so a NULL return is an immediate null-deref. Its first line is
		//     uVar5 = count * stride + 0x3f & 0xffffffc0;   // round up to 64
		//     if (*param_1 < uVar5) return 0;               // capacity check
		// so a zeroed pool block (capacity 0) fails every allocation. VF2 gets away with a zeroed
		// one only because its content never reaches this path.
		//
		// Layout read out of that function:
		//   +0x00 capacity in bytes   +0x08 cursor   +0x10 stride   +0x14 count
		//   +0x18 byte offset of the current allocation (passed to IASetVertexBuffers as pOffsets)
		//   +0x24 frame stamp, compared against *(uint*)(gs_context + 0x68)
		//   +0x38 a gs_buffer_t wrapping the pool's own D3D11 buffer
		// When the stamp matches and the request fits it Maps WRITE_NO_OVERWRITE (type 5) and
		// appends at the cursor; otherwise it resets the cursor, updates the stamp and Maps
		// DISCARD. The allocator maintains +0x08/+0x10/+0x14/+0x18/+0x24 itself — the host only
		// has to supply the capacity and the buffer.
		//
		// The consumer binds this buffer with IASetVertexBuffers (devctx+0x18 vtable +0x90) and
		// Unmaps it via +0x78, so it is a DYNAMIC vertex buffer with CPU write access.
		//
		// CAPACITY IS THE ONE VALUE NOT CAPTURED from the live host (finding its
		// cgs_device_context needs an anchor I do not have yet). It is a tuning parameter rather
		// than a protocol constant: too small returns NULL and crashes, too large only wastes
		// memory, and the pool is reset every frame. 4 MB is generous for per-frame 2D/immediate
		// geometry. If a frame ever needs more, the symptom is the same null-deref at DLL+0x2525A0.
		struct up_pool_t
		{
			uint32_t capacity;       // +0x00
			uint32_t unknown_04;
			uint32_t cursor;         // +0x08
			uint32_t unknown_0C;
			uint32_t stride;         // +0x10  written by the allocator
			uint32_t count;          // +0x14  written by the allocator
			uint32_t offset;         // +0x18  written by the allocator
			uint32_t unknown_1C;
			uint32_t unknown_20;
			uint32_t frame_stamp;    // +0x24
			std::byte gap[0x38 - 0x28]{};
			gs_buffer_t* buffer;     // +0x38
			std::byte tail[0x400 - 0x40]{};
		};
		static_assert(offsetof(up_pool_t, cursor) == 0x08);
		static_assert(offsetof(up_pool_t, offset) == 0x18);
		static_assert(offsetof(up_pool_t, frame_stamp) == 0x24);
		static_assert(offsetof(up_pool_t, buffer) == 0x38);

		static constexpr uint32_t kUploadPoolCapacity = 4 * 1024 * 1024;
		static up_pool_t s_upPoolState;
		static gs_buffer_t s_upPoolBuffer;

		static void InitUploadPool(const RenderWindow& window)
		{
			ID3D11Device* device = window.GetD3D11Device();
			if (device == nullptr) return;

			D3D11_BUFFER_DESC desc {};
			desc.ByteWidth = kUploadPoolCapacity;
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			ID3D11Buffer* buffer = nullptr;
			const HRESULT hr = device->CreateBuffer(&desc, nullptr, &buffer);
			if (FAILED(hr) || buffer == nullptr)
			{
				DebugLogFile("[vf5fs::YLAD] upload pool buffer FAILED (hr=0x%08X)\n", hr);
				return;
			}

			s_upPoolBuffer.alive = 1;
			s_upPoolBuffer.flags = 0x101;            // vertex buffer
			s_upPoolBuffer.byte_size = kUploadPoolCapacity;
			s_upPoolBuffer.self = &s_upPoolBuffer;
			s_upPoolBuffer.sub = reinterpret_cast<uint8_t*>(&s_upPoolBuffer) + 0x30;
			s_upPoolBuffer.element_count = 0;
			s_upPoolBuffer.reserved = 0xFFFFFFFF;
			s_upPoolBuffer.resource = buffer;
			s_upPoolBuffer.sub_flags = s_upPoolBuffer.flags;
			s_upPoolBuffer.sub_size = kUploadPoolCapacity;

			s_upPoolState.capacity = kUploadPoolCapacity;
			s_upPoolState.cursor = 0;
			// Anything that cannot equal gs+0x68 on the first frame, so the first allocation takes
			// the reset/DISCARD branch instead of appending into an unmapped buffer.
			s_upPoolState.frame_stamp = 0xFFFFFFFF;
			s_upPoolState.buffer = &s_upPoolBuffer;

			DebugLogFile("[vf5fs::YLAD] upload pool: %u bytes, pool=%p wrapper=%p buffer=%p\n",
				kUploadPoolCapacity, static_cast<void*>(&s_upPoolState),
				static_cast<void*>(&s_upPoolBuffer), static_cast<void*>(buffer));
		}

		static void PrimitiveInitialize(const RenderWindow& window)
		{
			ID3D11Device* device = window.GetD3D11Device();
			if (device == nullptr || g_gsContext == nullptr) return;

			// p_ib_quad — 192 quads, two triangles each: {4i, 4i+1, 4i+2, 4i, 4i+2, 4i+3}.
			// 192 x 6 = 1152 indices, which is exactly the count the live host was carrying (and
			// the same 192 primitives the LJ path in source/pxd/gs.cpp already uses).
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
				*reinterpret_cast<void**>(g_gsContext + 0x1438) =
					CreateIndexBuffer(device, s_primitiveBuffers[0], indices,
						NUM_PRIMITIVES * 6, "p_ib_quad");
			}

			// p_ib_fan — triangle fan: {i+2, 0, i+1}. The capture shows 1536 indices = 512 x 3.
			// NOTE that is the "2x bigger" size source/pxd/gs.cpp calls out as a suspected BUG in
			// the original for the LJ generation; the live YLAD host really does allocate it, so
			// here it is intentional. The LJ path is left alone — untested there.
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
				*reinterpret_cast<void**>(g_gsContext + 0x1440) =
					CreateIndexBuffer(device, s_primitiveBuffers[1], indices,
						NUM_PRIMITIVES * 3, "p_ib_fan");
			}

			// p_ib_rect — the live host carries exactly FOUR indices here (8 bytes): a single
			// triangle-strip rect, drawn as the identity run over the four corner vertices the
			// module supplies itself. (YAMP's LJ path builds a 192-rect buffer instead; whether
			// that generation differs is untested, so it is not being changed.)
			{
				static const uint16_t indices[4] = { 0, 1, 2, 3 };
				*reinterpret_cast<void**>(g_gsContext + 0x1448) =
					CreateIndexBuffer(device, s_primitiveBuffers[2], indices, 4, "p_ib_rect");
			}
		}

		// ================================== boot ==================================

		static std::filesystem::path gamePath;
		static wil::unique_hmodule gameDll;

		HMODULE LoadDLL()
		{
			gameDll.reset(LoadModuleDll(DLL_NAME, DLL_SUBDIR, ModuleSearch::CwdThenSubdir, gamePath));
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
				DebugLog("[vf5fs::YLAD] unresolved symbols: %s\n", missing);
				return false;
			}

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
			// As in the VF2 host: no separate sl::file_create in this module (a retail arcade
			// module never creates files), so file_create_internal stays null deliberately.

			Import(g_gsContext, ImportSymbol::GS_CONTEXT_INSTANCE);

			// Both context-pointer globals self-initialise lazily; set them up front.
			uint8_t** ppGsCtx;
			Import(ppGsCtx, ImportSymbol::GS_CONTEXT_PTR);
			*ppGsCtx = g_gsContext;

			void** ppSlCtx;
			Import(ppSlCtx, ImportSymbol::SL_CONTEXT_PTR);
			*ppSlCtx = sl::sm_context;

			DebugLog("[vf5fs::YLAD] slCtx=%p (size field 0x%X) gsCtx=%p (size field 0x%X)\n",
				sl::sm_context, sl::sm_context != nullptr
					? *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(sl::sm_context) + 8) : 0,
				g_gsContext, g_gsContext != nullptr ? *reinterpret_cast<uint32_t*>(g_gsContext + 8) : 0);

			// sl bring-up — same generation as the VF2 module, so the same flow.
			if (sl::sm_context && sl::sm_context->handles.p_handle_buffer == nullptr)
			{
				constexpr uint32_t kHandleCapacity = 0x100000;
				if (sl::initialize() != 0)
				{
					DebugLog("[vf5fs::YLAD] sl::initialize FAILED\n");
					return false;
				}
				if (sl::handle_initialize(kHandleCapacity) != 0)
				{
					DebugLog("[vf5fs::YLAD] sl::handle_initialize FAILED\n");
					return false;
				}
			}
			PatchSl(sl::sm_context);

			FillSharedSymbols(window);
			// Fill the host-provided primitive buffers the gs ctor only zeroes (see above) —
			// must happen before module_start, since the module draws with them immediately.
			PrimitiveInitialize(window);
			InitUploadPool(window);
			return true;
		}
		catch (...)
		{
			const std::wstring str(L"Failed to resolve imports and/or patch " + std::wstring(DLL_NAME) +
				L"!\n\nIt's either not a valid Virtua Fighter 5: Final Showdown DLL from Yakuza: Like a "
				L"Dragon, or the game has been updated and YAMP is not forward compatible with that "
				L"new version.");
			MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
			return false;
		}

		// ================================ game loop ================================

		bool GameLoop(module_func_t module_main, RenderWindow& window)
		{
			static execute_info_t execute_info{};
			static csl_pad s_pads[2];
			static int s_frame = 0;
			static int s_lastLogged = -1;

			// YLAD-era DX11 cgs_device_context, same shape the VF2 host maps:
			//   +0x18 sbgl context (IS the immediate ID3D11DeviceContext in the DX11 pxd world),
			//   +0x28 mp_cb_pool, +0x30 mp_up_pool (zeroed tables self-populate),
			//   +0x38 host render-state block.
			static uint8_t* s_deviceContext = new uint8_t[0x40000]();
			*reinterpret_cast<void**>(s_deviceContext + 0x18) = window.GetD3D11DeviceContext();
			static uint8_t* s_renderStateBlock = new uint8_t[0x8000]();
			*reinterpret_cast<void**>(s_deviceContext + 0x38) = s_renderStateBlock;
			// mp_cb_pool (+0x28) is a size-bucketed table that self-populates from zero (VF2's
			// mapping). mp_up_pool (+0x30) is NOT — it needs a real capacity and a mappable
			// buffer, both set up in InitUploadPool.
			static uint8_t* s_cbPool = new uint8_t[0x40000]();
			*reinterpret_cast<void**>(s_deviceContext + 0x28) = s_cbPool;
			*reinterpret_cast<void**>(s_deviceContext + 0x30) = &s_upPoolState;

			// NOTE deliberately no ApplyAspectSetting / CRT filter here. Those belong to the Model 2
			// boards (StF/FV/MR/VF2), which are 4:3 cabinet games; VF5FS is a modern widescreen 3D
			// title and presents at the window's own aspect, exactly as the LJ VF5FS host does.

			execute_info.size_of_struct = sizeof(execute_info);
			execute_info.p_device_context = reinterpret_cast<cgs_device_context*>(s_deviceContext);
			execute_info.status = 0;
			execute_info.output_texid = 0;
			execute_info.result = 0x80004005;
			// Master volume through the module's OWN mechanism: the 0..20 byte at +0x663.
			execute_info.sound_volume_level =
				static_cast<uint8_t>(gGeneral.GetSettings()->m_volumePercent * 20 / 100);

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
				execute_info.status |= static_cast<int>(EXECUTE_INFO_STATUS_PAUSE);
			}

			// Input: shared XInput snapshot -> per-player bindings via csl_pad, then the 0xE0
			// csl_pad prefix into each lj_pad_t (see the struct comment: stride UNVERIFIED here).
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

			window.BeginFrame();
			window.NewImGuiFrame();
			if (s_pauseMenuOpen)
			{
				if (!m2ftg::DrawPauseMenu(window, s_pauseMenuOpen))
				{
					return false; // Quit picked
				}
			}

			const int funcResult = module_main(sizeof(execute_info), &execute_info);

			const int interesting = execute_info.status | (funcResult << 16);
			if (interesting != s_lastLogged && s_frame < 5000)
			{
				DebugLogFile("[vf5fs::YLAD] frame=%d status=0x%X result=0x%X texid=%u ret=0x%X\n",
					s_frame, execute_info.status, execute_info.result,
					execute_info.output_texid, funcResult);
				s_lastLogged = interesting;
			}
			s_frame++;

			// Display blit: resolve the module's output texture through OUR tex instance table
			// (gsCtx+0x101738) to its D3D11 SRV, probing for the COM pointer once — the same
			// technique the VF2 host uses, since the SRV's offset inside the sbgl sub-object is
			// not pinned for this generation.
			if (execute_info.output_texid != 0)
			{
				static ID3D11ShaderResourceView* s_displaySrv = nullptr;
				static bool s_srvProbed = false;
				if (!s_srvProbed && s_frame >= 60)
				{
					s_srvProbed = true;
					void** tbl = *reinterpret_cast<void***>(g_gsContext + 0x101738);
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
							for (size_t subOff = 0; subOff <= 0x38 && s_displaySrv == nullptr; subOff += 8)
								s_displaySrv = tryQiSrv(*reinterpret_cast<void**>(sub + subOff));
							if (s_displaySrv != nullptr)
							{
								DebugLogFile("[vf5fs::YLAD] display SRV via tex+0x%zX (srv=%p)\n",
									texOff, s_displaySrv);
								break;
							}
						}
					}
					if (s_displaySrv == nullptr) DebugLogFile("[vf5fs::YLAD] display SRV NOT found\n");
				}
				if (s_displaySrv != nullptr)
				{
					window.BlitGameFrame(s_displaySrv);
				}
			}

			window.RenderImGui();
			window.EndFrame();

			if (FAILED(window.GetSwapChain()->Present(1, 0))) return false;
			return funcResult == 0;
		}

		void Run(RenderWindow& window)
		{
			const auto module_start = reinterpret_cast<module_func_t>(
				GetProcAddress(gameDll.get(), "module_start"));
			THROW_LAST_ERROR_IF_NULL(module_start);
			const auto module_stop = reinterpret_cast<module_func_t>(
				GetProcAddress(gameDll.get(), "module_stop"));
			THROW_LAST_ERROR_IF_NULL(module_stop);
			module_func_t module_main = nullptr;

			if (!ResolveSymbolsAndPatch(gameDll.get(), window))
			{
				DebugLogFile("[vf5fs::YLAD] ResolveSymbolsAndPatch FAILED\n");
				return;
			}
			DebugLogFile("[vf5fs::YLAD] symbols + sl init OK\n");

			Cri criware;

			// The three module blocks, sizes enforced by this DLL's own initialize_module
			// functions: sl 0x10/0xF000 (FUN_180212200), gs 0x58/0x3820C0 (FUN_180235900),
			// ct 0x10/0x30 (FUN_1802297E0).
			struct sl_module_t
			{
				size_t size = sizeof(sl_module_t);
				sl::context_t* context;
			} sl_module;
			static_assert(sizeof(sl_module_t) == 0x10);
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
				size_t size = sizeof(ct_module_t);
				ct_context_t* context = &s_ctContext;
			} ct_module;
			static_assert(sizeof(ct_module_t) == 0x10);

			// The config is read as a QWORD at params+0x38 by module_start: byte5 = game mode
			// (two derived flag globals, `!= 0` and `== 2`) and byte7's low/high nibbles are two
			// more flags — the same core game_config_t both other VF5FS generations take.
			using params_t = module_params_t<game_config_t, sl_module_t, gs_module_t, ct_module_t, icri>;
			params_t params;
			static_assert(offsetof(params_t, config) == 0x38);

			const std::string utf8Path = gamePath.u8string();

			params.sl_module = &sl_module;
			params.gs_module = &gs_module;
			params.ct_module = &ct_module;
			params.cri_ptr = &criware;
			params.module_main = &module_main;
			params.root_path = utf8Path.c_str();

			const auto* settings = gGeneral.GetSettings();
			params.config.is_dural_unlocked = false;
			params.config.is_triangle_start = false;
			params.config.game_mode = settings->m_arcadeMode ? 1 : 0;
			params.config.lang = static_cast<int8_t>(settings->m_language);
			params.config.diff = 1;
			params.config.energy = 200;
			params.config.round = 2;
			params.config.time = 60;

			int64_t frameTimeTicks;
			{
				if (!settings->m_enableFpsCap)
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

			DebugLogFile("[vf5fs::YLAD] module_start(size=%zu, root='%s')\n",
				sizeof(params), params.root_path);
			const int startResult = module_start(sizeof(params), &params);
			DebugLogFile("[vf5fs::YLAD] module_start -> 0x%X, module_main=%p\n",
				startResult, reinterpret_cast<void*>(module_main));

			if (startResult == 0 && module_main != nullptr)
			{
				LARGE_INTEGER lastTime;
				QueryPerformanceCounter(&lastTime);
				const uint32_t frameLimit = gGeneral.GetFrameLimit();
				uint32_t framesRun = 0;
				while (!window.IsShuttingDown())
				{
					if (!GameLoop(module_main, window)) break;
					if (frameLimit != 0 && ++framesRun >= frameLimit)
					{
						DebugLogFile("[vf5fs::YLAD] frame limit %u reached, shutting down\n", frameLimit);
						break;
					}

					LARGE_INTEGER currentTime;
					do
					{
						QueryPerformanceCounter(&currentTime);
					} while (((currentTime.QuadPart - lastTime.QuadPart) * 1000) < frameTimeTicks);
					lastTime = currentTime;
				}

				const int stopResult = module_stop(0, nullptr);
				DebugLogFile("[vf5fs::YLAD] module_stop -> 0x%X\n", stopResult);
			}
		}
	}
}
