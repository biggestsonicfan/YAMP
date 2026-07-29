#include "Patch.h"

#include "../file_access.h"
#include "../pxd_types.h"
#include "../async_request.h"

#include "../../wil/common.h"
#include "../../DebugLog.h"
#include "../../Utils/MemoryMgr.h"
#include "../../Utils/Trampoline.h"

#include "sys_util.h"
#include "cs_game.h"
#include "HleHooks.h"
#include "DebugWindows.h"

#include "../ImportSymbols.h"
#include "../Imports.h"
#include "HostCdevice.h"
#include "LJHost.h" // GameDesc / CurrentGame()

#include <d3d12.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include "../../wil/com.h"
#include "../../RenderWindow.h"

namespace m2ftg
{
		// ---- CBV/SRV/UAV descriptor heap (gs context_t+0x7928) --------------
		// The pxd DX12 path allocates SRV/UAV/CBV descriptors from a bitmap allocator
		// embedded at gs::sm_context+0x7928 (alloc FUN_18008e770, free FUN_18008e2c0).
		// The host's sbgl::cdevice::initialize normally creates the ID3D12DescriptorHeap
		// and fills this struct; YAMP stubs that init, so the allocator is all-zero and
		// FUN_18008e770 returns a NULL descriptor handle -> CreateShaderResourceView/UAV
		// writes to null -> NVIDIA driver AV. We create the heap and fill the struct here.
		// Field offsets (relative to context_t+0x7928, all confirmed from the DLL):
		//   +0x00 ID3D12DescriptorHeap*  +0x04 increment  +0x18 CPU base  +0x20 GPU base
		//   +0x28 cursor  +0x2c capacity  +0x30 count  +0x34 bitmap-word-count  +0x38 bitmap (1=free)
		// The DX12 gs-init registers built-in textures by writing a 32-bit format/descriptor
		// value into per-id tables pointed to by context+0x79F8/+0x7A00/+0x7A08/+0x7A10 (verified:
		// the live LJ host context holds 4 heap pointers there; a fresh YAMP context leaves them
		// null, so the DLL's `mov [*(context+0x7A10) + id*4], ebx` AVs). The host device-start
		// allocates these tables; YAMP replicates that here (id space up to 0x10000, matching the
		// handle-table capacities). Zero-init is fine — the DLL populates them during init.
		static void InitTexIdTables(gs::context_t* context)
		{
			if (!context) return;
			constexpr size_t kEntries = 0x10000;          // max id space
			constexpr size_t kTableBytes = kEntries * 8;  // 8B/entry: covers uint32 and ptr uses
			static const size_t kOffsets[4] = { 0x79F8, 0x7A00, 0x7A08, 0x7A10 };
			for (size_t off : kOffsets)
			{
				uint8_t* buf = static_cast<uint8_t*>(::operator new(kTableBytes));
				std::memset(buf, 0, kTableBytes);
				*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(context) + off) = buf;
			}
		}

		// Set up one pxd descriptor-heap "block" (the DLL's bitmap allocator, FUN_18008e770).
		// Block field layout (relative to blockOffset, confirmed from FUN_18008e770 which computes
		// handle = *(+0x04)*index + *(+0x18)): +0x00 ID3D12DescriptorHeap*, +0x04 increment,
		// +0x18 CPU base, +0x20 GPU base, +0x28 cursor, +0x2C capacity, +0x30 count,
		// +0x34 bitmap-word-count, +0x38 bitmap ptr (1=free). The blocks live in an array (order
		// confirmed against a live LJ gs context): +0x7870 = SAMPLER, +0x78B0 = RTV, +0x78F0 = DSV,
		// +0x7930 = CBV/SRV/UAV. All four are NON-shader-visible CPU-staging heaps (see below).
		static bool SetupDescriptorBlock(gs::context_t* context, size_t blockOffset,
			D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisible, UINT capacity,
			ID3D12Device* device, wil::com_ptr<ID3D12DescriptorHeap>& heapOut,
			std::vector<uint64_t>& bitmapOut, const char* label)
		{
			const UINT words = (capacity + 63) / 64;

			D3D12_DESCRIPTOR_HEAP_DESC hd = {};
			hd.Type = type;
			hd.NumDescriptors = capacity;
			hd.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
			                         : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(heapOut.put()))))
			{
				DebugLog("[%s gs] CreateDescriptorHeap(%s) failed\n", gGeneral.GetGameTag(), label);
				return false;
			}

			const UINT   inc     = device->GetDescriptorHandleIncrementSize(type);
			const UINT64 cpuBase = heapOut->GetCPUDescriptorHandleForHeapStart().ptr;
			const UINT64 gpuBase = shaderVisible ? heapOut->GetGPUDescriptorHandleForHeapStart().ptr : 0;
			bitmapOut.assign(words, 0xFFFFFFFFFFFFFFFFull); // every descriptor free

			uint8_t* a = reinterpret_cast<uint8_t*>(context) + blockOffset;
			*reinterpret_cast<uint64_t*>(a + 0x00) = reinterpret_cast<uint64_t>(heapOut.get());
			*reinterpret_cast<uint32_t*>(a + 0x04) = inc;
			*reinterpret_cast<uint64_t*>(a + 0x18) = cpuBase;
			*reinterpret_cast<uint64_t*>(a + 0x20) = gpuBase;
			*reinterpret_cast<uint32_t*>(a + 0x28) = 0;        // cursor
			*reinterpret_cast<uint32_t*>(a + 0x2c) = capacity; // capacity
			*reinterpret_cast<uint32_t*>(a + 0x30) = 0;        // count
			*reinterpret_cast<uint32_t*>(a + 0x34) = words;    // bitmap words
			*reinterpret_cast<uint64_t*>(a + 0x38) = reinterpret_cast<uint64_t>(bitmapOut.data());

			DebugLog("[%s gs] descblock %s @ context+0x%zX: inc=%u cap=%u cpuBase=0x%llX gpuBase=0x%llX\n",
				gGeneral.GetGameTag(), label, blockOffset, inc, capacity,
				(unsigned long long)cpuBase, (unsigned long long)gpuBase);
			return true;
		}

		static void InitSrvUavDescriptorHeap(gs::context_t* context, const RenderWindow& window)
		{
			ID3D12Device* device = window.GetD3D12Device();
			if (!device || !context) return;

			// The four pxd descriptor blocks are CPU-STAGING heaps: the DLL creates views into them
			// (CreateSampler/SRV/RTV/DSV at the block's CPU handle) and, at draw time, COPIES the used
			// descriptors into its own shader-visible ring which it binds via SetDescriptorHeaps. So
			// these blocks MUST be NON-shader-visible: verified two ways — (1) the DLL descriptor-alloc
			// functions (FUN_18009e620 sampler, FUN_1800a20c0 SRV, FUN_180094700 RTV) only ever read
			// each block's increment(+0x04) and CPU base(+0x18), never the GPU base(+0x20); (2) a live
			// Lost Judgment gs context has GPUbase=0 on all four (they are CPU-write-only staging).
			// Making them SHADER_VISIBLE (the old fix #8/#9) is exactly what makes the DLL's draw-time
			// CopyDescriptors read an illegal CPU-write-only source -> D3D12 validation id=654. Caps and
			// shader-visibility below mirror the LJ ground truth. (The +0x11878 CBV heap in PatchGs is
			// the exception: CreateConstantBufferView writes it DIRECTLY and its GPU handle is bound,
			// so that one stays shader-visible.)

			// CBV/SRV/UAV staging @ +0x7930 (FUN_1800a20c0 -> CreateShaderResourceView at CPU handle).
			static wil::com_ptr<ID3D12DescriptorHeap> s_srvHeap;
			static std::vector<uint64_t> s_srvBitmap;
			SetupDescriptorBlock(context, 0x7930, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, false,
				262144, device, s_srvHeap, s_srvBitmap, "CBV/SRV/UAV");

			// RTV staging @ +0x78B0 (FUN_180094700 -> CreateRenderTargetView).
			static wil::com_ptr<ID3D12DescriptorHeap> s_rtvHeap;
			static std::vector<uint64_t> s_rtvBitmap;
			SetupDescriptorBlock(context, 0x78B0, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, false,
				1024, device, s_rtvHeap, s_rtvBitmap, "RTV");

			// DSV staging @ +0x78F0 — LJ wires this block (incr 0x08, cap 1024); YAMP never had it.
			// The array order is SAMPLER(+0x7870), RTV(+0x78B0), DSV(+0x78F0), CBV/SRV/UAV(+0x7930).
			static wil::com_ptr<ID3D12DescriptorHeap> s_dsvHeap;
			static std::vector<uint64_t> s_dsvBitmap;
			SetupDescriptorBlock(context, 0x78F0, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, false,
				1024, device, s_dsvHeap, s_dsvBitmap, "DSV");

			// SAMPLER staging @ +0x7870 — block[0] of the array. FUN_18009e620 bitmap-allocates a slot
			// and calls CreateSampler (params to FUN_1800acd90 are D3D12_SAMPLER_DESC fields) at the
			// CPU handle only. Non-shader-visible staging matches LJ (cap 256).
			static wil::com_ptr<ID3D12DescriptorHeap> s_samplerHeap;
			static std::vector<uint64_t> s_samplerBitmap;
			SetupDescriptorBlock(context, 0x7870, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, false,
				256, device, s_samplerHeap, s_samplerBitmap, "SAMPLER");

			// gs+0x7550..0x75E8: the SHADER-VISIBLE descriptor-copy rings (the CopyDescriptors dest the
			// non-shader-visible staging blocks feed into). CRITICAL: the ring descriptors are INLINE in
			// the gs context (NOT a separate array) and gs+0x7550 is a POINTER into that inline region.
			// Captured from a live LJ (gs+0x7550 = 0x143b79848 = gs+0x7588): three 0x30-byte ring
			// descriptors at gs+0x7558/0x7588/0x75B8, each a shader-visible CBV/SRV/UAV heap, and
			// gs+0x7550 points at ring[1] (gs+0x7588). Ring layout: +0x04 stride(=incr 32), +0x08 cap,
			// +0x10 ID3D12DescriptorHeap*, +0x18 heap CPU-handle base (the CopyDescriptors DEST; the
			// heap's own CPU descriptor memory), +0x20 heap GPU-handle base (bound via SetGraphicsRoot
			// DescriptorTable), +0x28 mask=0xFFFFFFFF, +0x2C atomic cursor. The DLL's flush reads the
			// inline descriptors directly for the bind, so pointing gs+0x7550 at an external array (as
			// before) left them unset -> SetGraphicsRootDescriptorTable got a garbage GPU handle ->
			// nvwgf2umx AV. Writing them inline fixes the heap/handle mismatch. LJ caps at 999936; a
			// lighter cap is fine (live peak cursor ~377).
			{
				constexpr UINT   kCopyRingCap   = 999936; // match LJ exactly (descriptor indices reach ~297k)
				constexpr UINT   kCopyRingCount = 3;       // LJ: rings @ gs+0x7558/0x7588/0x75B8
				constexpr size_t kRingBase      = 0x7558;  // first inline ring
				constexpr size_t kRingStride    = 0x30;
				constexpr size_t kCurrentRing   = 1;       // gs+0x7550 points at ring[1] (gs+0x7588)

				// Wire the rings to the DLL'S OWN shader-visible CBV/SRV/UAV heap (the one it creates
				// early — before PatchGs — and binds via SetDescriptorHeaps). Confirmed via the device
				// CreateDescriptorHeap hook + the command-list SetDescriptorHeaps hook: the DLL binds a
				// num~1,000,000 shader-visible CBV/SRV/UAV heap, but the SetGraphicsRootDescriptorTable
				// GPU handle is taken from gs+0x7550. So the ring must reference the SAME heap the DLL
				// binds, else the handle "does not refer to a location in a descriptor heap" -> nvwgf2umx
				// AV. YAMP creating its own heap is exactly that mismatch. (YAMP has seen only one such
				// heap so far — it crashed early; the DLL may create up to 3 for triple-buffering. Point
				// all 3 ring slots at the one we have; extend if a 2nd/3rd appears.)
				const UINT stride = device->GetDescriptorHandleIncrementSize(
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				uint8_t* gc = reinterpret_cast<uint8_t*>(context);
				ID3D12DescriptorHeap* dllHeap = GetDllRingCbvSrvHeap();
				if (dllHeap != nullptr)
				{
					const D3D12_DESCRIPTOR_HEAP_DESC hd = dllHeap->GetDesc();
					const uint64_t cpuBase = dllHeap->GetCPUDescriptorHandleForHeapStart().ptr;
					const uint64_t gpuBase = dllHeap->GetGPUDescriptorHandleForHeapStart().ptr;
					for (UINT i = 0; i < kCopyRingCount; ++i)
					{
						uint8_t* r = gc + kRingBase + i * kRingStride;
						*reinterpret_cast<uint32_t*>(r + 0x00) = 0;                 // flags
						*reinterpret_cast<uint32_t*>(r + 0x04) = stride;            // stride (= incr, 32)
						*reinterpret_cast<uint32_t*>(r + 0x08) = hd.NumDescriptors - 64; // cap (LJ: heap-64 = 999936)
						*reinterpret_cast<uint32_t*>(r + 0x0C) = 0;
						*reinterpret_cast<uint64_t*>(r + 0x10) = reinterpret_cast<uint64_t>(dllHeap);
						*reinterpret_cast<uint64_t*>(r + 0x18) = cpuBase;
						*reinterpret_cast<uint64_t*>(r + 0x20) = gpuBase;
						*reinterpret_cast<uint32_t*>(r + 0x28) = 0xFFFFFFFF;        // mask
						// Cursor must start >= the descriptor-table size: FUN_18009dbd0 writes a
						// param_4-descriptor table ENDING at the cursor and returns the GPU handle
						// gpuBase + (cursor - param_4)*stride. With cursor=0 that handle lands BEFORE
						// the heap (base - param_4*32) -> "not in a descriptor heap" -> nvwgf2umx AV.
						// LJ's cursor was already 377 (accumulated); seed a safe offset so the first
						// table's start slot is non-negative (well above any table size).
						*reinterpret_cast<uint32_t*>(r + 0x2C) = 0x400;             // cursor (seeded)
					}
					DebugLog("[%s gs] gs+0x7550 rings -> DLL heap %p (num=%u) gpu=0x%llX\n",
						gGeneral.GetGameTag(), (void*)dllHeap, hd.NumDescriptors, (unsigned long long)gpuBase);
				}
				else
				{
					DebugLog("[%s gs] WARNING: DLL ring CBV/SRV heap not captured yet!\n", gGeneral.GetGameTag());
				}
				(void)kCopyRingCap;
				// gs+0x7550 = pointer to the "current" inline ring (LJ points at ring[1] @ gs+0x7588).
				*reinterpret_cast<void**>(gc + 0x7550) = gc + kRingBase + kCurrentRing * kRingStride;
			}
		}

		// FUN_18009dbd0 is a per-FRAME transient descriptor-copy allocator: the cursor (ring+0x2C) is
		// atomically bumped for every table it copies and LJ RESETS it each frame (so it never grows
		// past the heap). YAMP seeds it once, so without a per-frame reset the cursor climbs
		// monotonically until CopyDescriptorsSimple writes one past the heap end (id=646 dest = heap
		// end + 0x20 -> nvwgf2umx driver AV). Reset all 3 rings to the seed at the start of each frame.
		void ResetCbvSrvRingCursors(gs::context_t* context)
		{
			if (context == nullptr) return;
			uint8_t* gc = reinterpret_cast<uint8_t*>(context);
			for (int i = 0; i < 3; ++i)
				*reinterpret_cast<uint32_t*>(gc + 0x7558 + i * 0x30 + 0x2C) = 0x400;
		}


		// (PatchSl moved to ../sl.cpp — it is host-generic sl-context setup shared with the
		// YLAD VF2 host, not an LJ patch.)

		// Stub resource wrapper for the transient upload ring. The ring descriptor at gs+0x188200
		// points here via +0x18; FUN_18009c280 copies the pointer into each lazily-created ring
		// entry and calls vtable[1] (AddRef). FUN_18005eb00 (m2ftg vertex-bind) calls vtable[0xB]
		// (+0x58) = GetGPUVirtualAddress to fetch the ring's GPU VA base. Object layout matches the
		// pxd resource convention: {+0x00 vtable, +0x08 gpu_va}.
		static uint64_t __fastcall RingRes_GetGpuVa(void* self)
		{
			return *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(self) + 8);
		}
		static uint64_t __fastcall RingRes_Noop(void* /*self*/) { return 0; }

		void PatchGs(gs::context_t* context, const RenderWindow& window)
		{
			// gs::initialize_module (0x180092fe0) validates the context header:
			// size_of_struct (+8) must equal the DLL's real value, then it imports the
			// sbgl shared symbols from context+0x20 (export_context.sbgl_context).
			// YAMP loads the 0x388A00-size DLL variant (verified: the constant 00 8A 38 00
			// appears in the DLL; 0x3889C0 does not). The embedded template already carries
			// this value; we set it defensively to match the DLL, NOT the older 0x3889C0 build.
			context->size_of_struct = 0x388A00;

			// Initialize cgs_device_context
			cgs_device_context* device_context = new cgs_device_context{};

			// Wire the two host-provided cgs_device_context sub-objects the LJ engine binds during
			// device-start but the StF DLL only ever READS (never writes). Both must be set BEFORE
			// device_context->initialize() below, which calls reset_state_all() and dereferences the
			// render-state block at +0x12c98. (Fields are inside the object's gap, so set by raw offset.)
			{
				uint8_t* dc = reinterpret_cast<uint8_t*>(device_context);

				// +0x12c98 : per-device-context render-state block. reset_state_all() fills graphics
				// defaults into it (max touched ~+0x373c; the optional callback at block+0x318 stays
				// null in a zeroed block -> skipped). 0x5000 gives headroom over the 0x4340 boundary.
				static uint8_t s_renderStateBlock[0x5000] = {};
				*reinterpret_cast<void**>(dc + 0x12c98) = s_renderStateBlock;

				// +0xC8 : the "command recording" context. FUN_180097520 records draws into its +0x10
				// ID3D12GraphicsCommandList; same 0x280 layout as the cdevice+0x28 pool contexts.
				*reinterpret_cast<void**>(dc + 0xC8) = BuildRenderCommandContext();

				// +0x12c88 : the per-device-context cache table for the transient vertex-upload ring
				// (FUN_18009f560: table+0x08 count, table+0x10 + idx*8 entries; idx is small). Host
				// never writes it (DLL only reads); a zeroed table is filled lazily by the engine.
				static uint8_t s_upCacheTable[0x810] = {}; // 256 entries + header
				*reinterpret_cast<void**>(dc + 0x12c88) = s_upCacheTable;

				// +0x11878 : host-provided CBV_SRV_UAV descriptor-heap wrapper. FUN_18005eb00 (m2ftg +
				// reset_state_all) CreateConstantBufferView's into its CPU handle (w+0x04 incr, w+0x18 CPU
				// base). This is a STAGING heap: the CBV is written here (non-shader-visible) then COPIED
				// into the gs+0x7550 shader-visible ring at draw time (CopyDescriptors), same as the
				// SRV/sampler staging blocks. Must be NON-shader-visible -> otherwise the draw-time copy
				// reads a CPU-write-only source (validation id=654). GPU base (+0x20) left 0.
				{
					static ID3D12DescriptorHeap* s_cbvHeap = nullptr;
					if (s_cbvHeap == nullptr)
					{
						D3D12_DESCRIPTOR_HEAP_DESC hd{};
						hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
						hd.NumDescriptors = 4096; // indices seen up to ~0x100; headroom
						hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // staging (non-shader-visible)
						window.GetD3D12Device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_cbvHeap));
					}
					if (s_cbvHeap != nullptr)
					{
						uint8_t* w = dc + 0x11878;
						const UINT incr = window.GetD3D12Device()->GetDescriptorHandleIncrementSize(
							D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
						*reinterpret_cast<void**>(w + 0x00)     = s_cbvHeap; // keep the heap object referenced
						*reinterpret_cast<uint32_t*>(w + 0x04)  = incr;
						*reinterpret_cast<uint64_t*>(w + 0x18)  = s_cbvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
						*reinterpret_cast<uint64_t*>(w + 0x20)  = 0; // non-shader-visible: no GPU handle
					}
				}

				// +0x11ba0 : the SAMPLER binding cache (FUN_18009f1b0, the sampler descriptor-ring
				// allocator — the sampler-side parallel of the gs+0x7550 CBV/SRV ring). Struct: +0x00
				// current-ring ptr, +0x10.. inline ring descriptors, +0xD0 sorted lookup table, +0xDC
				// entry count. FUN_18009f1b0 dereferences *(+0x00) as the ring and writes its +0x28
				// cursor (null -> AV), and skips the binary search when +0xDC==0. Wire a ring that
				// references the DLL's OWN shader-visible 2048 SAMPLER heap (captured by the device
				// CreateDescriptorHeap hook), with an empty cache. Ring layout mirrors the live LJ ring:
				// +0x04 stride, +0x08 cap, +0x10 heap, +0x18 CPU base, +0x20 GPU base, +0x28 cursor,
				// +0x30 sorted-table ptr, +0x38 table capacity.
				if (ID3D12DescriptorHeap* samplerHeap = GetDllRingSamplerHeap())
				{
					static uint8_t s_samplerRing[0x40] = {};
					static uint8_t s_samplerSortTable[0x2000] = {};
					const UINT sinc = window.GetD3D12Device()->GetDescriptorHandleIncrementSize(
						D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
					const D3D12_DESCRIPTOR_HEAP_DESC shd = samplerHeap->GetDesc();
					const uint64_t sCpu = samplerHeap->GetCPUDescriptorHandleForHeapStart().ptr;
					const uint64_t sGpu = samplerHeap->GetGPUDescriptorHandleForHeapStart().ptr;
					uint8_t* rr = s_samplerRing;
					*reinterpret_cast<uint32_t*>(rr + 0x00) = 1;                  // flags
					*reinterpret_cast<uint32_t*>(rr + 0x04) = sinc;               // stride (32)
					*reinterpret_cast<uint32_t*>(rr + 0x08) = shd.NumDescriptors; // cap (2048)
					*reinterpret_cast<uint64_t*>(rr + 0x10) = reinterpret_cast<uint64_t>(samplerHeap);
					*reinterpret_cast<uint64_t*>(rr + 0x18) = sCpu;
					*reinterpret_cast<uint64_t*>(rr + 0x20) = sGpu;
					// Sampler ring grows DOWN from the top: FUN_18009f1b0 places the table at slot
					// (cap - cursor). With cursor=0 the table start = cap (OOB at the heap end). Seed a
					// small positive cursor (LJ's was ~25) so the table sits inside the heap.
					*reinterpret_cast<uint32_t*>(rr + 0x28) = 0x40;               // cursor (seeded)
					*reinterpret_cast<void**>(rr + 0x30)    = s_samplerSortTable; // ring's sorted table
					*reinterpret_cast<uint32_t*>(rr + 0x38) = 0x200;             // table capacity

					uint8_t* sc = dc + 0x11ba0;
					*reinterpret_cast<void**>(sc + 0x00)    = rr;                 // current-ring ptr
					*reinterpret_cast<void**>(sc + 0xD0)    = s_samplerSortTable; // struct's sorted table
					*reinterpret_cast<uint32_t*>(sc + 0xD8) = 0x200;            // table capacity
					*reinterpret_cast<uint32_t*>(sc + 0xDC) = 0;                 // entry count (skip search)
					DebugLog("[%s gs] sampler ring @ device_context+0x11ba0 wired\n", gGeneral.GetGameTag());
				}
			}

			// Transient 2D vertex-upload ring (m2ftg): host-provided, StF DLL only READS it
			// (FUN_18009f560/F5D0). gs+0x188208 = 32MB CPU-writable ring base (sub-alloc =
			// (cursor<<4)+base; cursor in 16-byte units wraps at 0x200000 = 32MB), gs+0x188210 =
			// cursor, gs+0x188200 = ptr to a descriptor whose +0x18 vtable'd sub-object we leave null
			// (FUN_18009c280 returns E_FAIL gracefully then, avoiding a fabricated vtable). Written via
			// context (= DAT_1802000c0 = 0x180200100 at runtime), the base FUN_18009f560 reads.
			{
				uint8_t* gc = reinterpret_cast<uint8_t*>(context);

				// 32MB persistently-mapped upload buffer so the CPU-written vertices are GPU-readable.
				static ID3D12Resource* s_upRing = nullptr;
				static void* s_upRingCpu = nullptr;
				if (s_upRing == nullptr)
				{
					D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
					D3D12_RESOURCE_DESC rd{};
					rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
					rd.Width = 0x2000000; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
					rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
					rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
					if (SUCCEEDED(window.GetD3D12Device()->CreateCommittedResource(
						&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
						nullptr, IID_PPV_ARGS(&s_upRing))))
					{
						D3D12_RANGE nr{ 0, 0 };
						s_upRing->Map(0, &nr, &s_upRingCpu);
					}
				}
				// Resource wrapper {vtable, gpu_va} + its vtable (slot 1 = AddRef, slot 0xB = GetGpuVa).
				static void* s_ringResVtbl[16];
				static uint64_t s_ringRes[2];    // [0]=vtable, [1]=gpu_va
				// Descriptor: +0x18 must hold a POINTER to the wrapper (FUN_18009c280 dereferences it);
				// +0x20..+0x50 are 7 metadata qwords copied into the entry (left zeroed for now).
				static uint64_t s_upDesc[12] = {}; // 0x60 bytes
				for (auto& slot : s_ringResVtbl) slot = reinterpret_cast<void*>(&RingRes_Noop);
				s_ringResVtbl[0xB] = reinterpret_cast<void*>(&RingRes_GetGpuVa);
				s_ringRes[0] = reinterpret_cast<uint64_t>(s_ringResVtbl);
				s_ringRes[1] = (s_upRing != nullptr) ? s_upRing->GetGPUVirtualAddress() : 0;
				s_upDesc[3] = reinterpret_cast<uint64_t>(s_ringRes); // +0x18 = &wrapper

				*reinterpret_cast<void**>(gc + 0x188200)     = s_upDesc;
				*reinterpret_cast<void**>(gc + 0x188208)     = s_upRingCpu;
				*reinterpret_cast<uint32_t*>(gc + 0x188210)  = 0;
			}

			context->sbgl_device.initialize(window);

			static constexpr size_t NUM_CONTEXTES = 16; // TODO: Uneducated guess, real value comes from the init struct
			for (size_t i = 0; i < NUM_CONTEXTES; i++)
			{
				cgs_cb_pool* cbPool = new cgs_cb_pool;
				context->stack_cb_pool.push(cbPool);

				// TODO: Figure out proper sizes ASAP, now hardcoded for both which is terrible and probably wrong
				constexpr unsigned int UP_VB_SIZE = 32768, UP_IB_SIZE = 8192;
				//constexpr unsigned int UP_VB_SIZE = 4096, UP_IB_SIZE = 4096;
				cgs_up_pool* upPool = new cgs_up_pool;
				upPool->initialize(UP_VB_SIZE, UP_IB_SIZE, true);
				context->stack_up_pool.push(upPool);

				cgs_shader_uniform* uniform = new cgs_shader_uniform;
				uniform->initialize();

				context->stack_shader_uniform.push(uniform);
			}

			// +0x12c90 : a SECOND cgs_up_pool, the one the engine's immediate-mode quad pusher
			// uses. Host-provided like +0x12c88/+0x12c98 — nothing in any of the module DLLs ever
			// writes it, they only dereference it. Motor Raid is the game that actually exercises
			// this path: FUN_18009C720 (dc, verts, stride) tail-calls the up-pool vertex lock with
			// *(dc+0x12c90) as `this`, then FUN_18009C7A0 binds *(pool+0x38) (mp_vb) and draws —
			// so a null here is an AV the moment MR's attract demo draws its first 2D quad. StF's
			// and FV's DLLs never reference the field, so wiring it is free for them.
			//
			// It must be its OWN pool, not device_context->mp_up_pool: both are per-frame rings
			// that reset on a frame-counter change and append otherwise, so sharing one would let
			// the 2D pusher and the regular dynamic-geometry path stomp each other's cursors.
			// Sized well above mp_up_pool's: the pusher appends one 4-vertex quad (stride 0x18,
			// 0x80 bytes after the 64-byte round-up) per call and only wraps with a MAP_DISCARD,
			// which would invalidate quads already recorded into this frame's command list.
			{
				constexpr unsigned int PUSH_VB_SIZE = 1 << 20; // ~8k quads/frame before a discard
				cgs_up_pool* pushPool = new cgs_up_pool;
				pushPool->initialize(PUSH_VB_SIZE, 0, true);
				*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(device_context) + 0x12c90) = pushPool;
			}

			device_context->initialize(reinterpret_cast<sbgl::ccontext*>(context->sbgl_device.m_pD3DDeviceContext));
			context->p_device_context = device_context;

			// Handle-table capacities taken from a live Lost Judgment gs context (the host's
			// real device-start). Order/offsets verified: mesh@+0x107A98, tex@+0x107AB8, ...
			constexpr unsigned int MESH_MAX = 4096;   // 0x1000
			constexpr unsigned int TEX_MAX  = 32768;  // 0x8000
			constexpr unsigned int VS_MAX   = 8192;   // 0x2000
			constexpr unsigned int PS_MAX   = 8192;
			constexpr unsigned int GS_MAX   = 8192;
			constexpr unsigned int HS_MAX   = 8192;
			constexpr unsigned int DS_MAX   = 8192;
			constexpr unsigned int CS_MAX   = 1024;   // 0x400
			constexpr unsigned int GTS_MAX  = 8192;
			constexpr unsigned int FX_MAX   = 8192;
			context->handle_mesh.initialize(nullptr, MESH_MAX);
			context->handle_tex.initialize(nullptr, TEX_MAX);
			context->handle_vs.initialize(nullptr, VS_MAX);
			context->handle_ps.initialize(nullptr, PS_MAX);
			context->handle_gs.initialize(nullptr, GS_MAX);
			context->handle_ds.initialize(nullptr, DS_MAX);
			context->handle_hs.initialize(nullptr, HS_MAX);
			context->handle_cs.initialize(nullptr, CS_MAX);
			context->handle_gts.initialize(nullptr, GTS_MAX);
			context->handle_fx.initialize(nullptr, FX_MAX);

			// Fill the export context.
			// gs::initialize_module (DLL 0x1800930A0) copies p_value[0] straight into the
			// cdevice_common::g_pD3DDevice global, and the DX12 renderer then reads
			// g_pD3DDevice+8 as the ID3D12Device (e.g. CheckFeatureSupport in the format-caps
			// setup). So p_value[0] must be OUR host cdevice (whose +8 = the D3D12 device, set
			// by BuildHostCdevice), NOT the D3D11 device — otherwise g_pD3DDevice+8 is null and
			// module_start AVs. (The DX11/VF5FS path put the D3D11 device here; DX12 differs.)
			auto& export_context = context->export_context;
			export_context.size_of_struct = sizeof(export_context);
			if (void* hostCdevice = GetHostCdevice())
				export_context.sbgl_context.p_value[0] = hostCdevice;
			else
				export_context.sbgl_context.p_value[0] = window.GetD3D11Device(); // DX11 fallback
			export_context.sbgl_context.p_value[1] = static_cast<sbgl::cdevice_native*>(&context->sbgl_device);
			export_context.sbgl_context.p_value[2] = &context->sbgl_device.m_swap_chain;

			// Create + wire the CBV/SRV/UAV descriptor heap the DX12 path allocates from
			// (context+0x7928) BEFORE any resource/texture creation happens.
			InitSrvUavDescriptorHeap(context, window);

			// Allocate the per-tex-id tables (context+0x79F8..+0x7A10) the gs-init writes into.
			InitTexIdTables(context);

			gs::primitive_initialize();
		}

		// Swallow the debug-CRT "invalid parameter" assert so a bad/custom format specifier can't
		// pop the "Debug Assertion Failed" dialog / abort the process.
		static void SilentInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
			unsigned int, uintptr_t) {}

		static void prj_trap(const char* format, ...)
		{
			// The pxd engine's trap/log uses custom format directives (e.g. "%~", "%(") that the
			// standard CRT formatter rejects — in a Debug build vsprintf_s would raise the
			// "Incorrect format specifier" assertion and kill the process. The game calls this
			// during the normal GameLoop, so it must NOT crash or halt. Format leniently (swallow
			// invalid-parameter asserts, fall back to the raw string) and never __debugbreak.
			char buf[512] = {};
			va_list vl;
			va_start(vl, format);
			const _invalid_parameter_handler prev =
				_set_thread_local_invalid_parameter_handler(&SilentInvalidParameter);
			const int n = format ? _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, format, vl) : -1;
			_set_thread_local_invalid_parameter_handler(prev);
			va_end(vl);

			if (n < 0)
			{
				// Formatting failed (custom directive / arg mismatch) — emit the raw format text.
				// The raw text may contain % directives, so pass it as an argument, never as the format.
				DebugLog("[%s prj_trap] ", gGeneral.GetGameTag());
				if (format) DebugLog("%s", format);
				DebugLog("\n");
			}
			else
			{
				DebugLog("%s", buf);
			}
		}

		void ReinstateLogging(void* dll, const Imports& symbols)
		{
			Trampoline* t = Trampoline::MakeTrampoline(dll);
			for (const auto& [key, func] : symbols.GetSymbolRange(ImportSymbol::PRJ_TRAP))
			{
				Memory::InjectHook(func, t->Jump(&prj_trap), Memory::HookType::Jump);
			}
		}

		void InjectTraps(const Imports& symbols)
		{
#ifdef _DEBUG
			for (const auto& [key, ptr] : symbols.GetSymbolRange(ImportSymbol::TRAP_ALLOC_INSTANCE_TBL))
			{
				Memory::Patch<uint8_t>(ptr, 0xCC);
			}
#endif
		}

		// ---- RAM-executable i960 instruction fetch --------------------------
		// The ROM's dw debug menu (enabled via the RAM 0x508000 debug flag) dispatches menu
		// items by copying an i960 instruction pair into a trampoline at RAM 0x59F270 and
		// `callx`-ing into it — legal on hardware, but the retail DLL's CPU core fetches
		// instructions linearly from the program-ROM host buffer only (ctx->codeBase + IP),
		// so a RAM IP reads past the 1MB ROM image and crashes the host. This reimplements
		// the DLL's fetch/decode dispatcher byte-for-byte in behaviour, with one change:
		// the host base is chosen per region, exactly like the DLL's own data accessors.
		namespace RamExecFetch
		{
			// DLL globals by fixed RVA (same layout StfDebugWindows relies on): the i960
			// context pointer, the 0x1000-entry opcode dispatch table the original indexes,
			// and the work-RAM host base (the emulated address is added directly to it).
			// Per-game — the StF and FV DLLs place them differently; values live in the
			// GameDesc table (LJHost.h) and are latched at install time.
			static uintptr_t RVA_CPU_CTX_PTR = 0;
			static uintptr_t RVA_OPCODE_TABLE = 0;
			static uintptr_t RVA_RAM_BASE_PTR = 0;
			// Work-RAM window the trampoline lives in (memory-map region 5).
			constexpr uint32_t RAM_START = 0x500000;
			constexpr uint32_t RAM_SIZE = 0x100000;

			static uint8_t* dllBase = nullptr;

			static void FetchExec()
			{
				uint8_t* const ctx = *reinterpret_cast<uint8_t* const*>(dllBase + RVA_CPU_CTX_PTR);
				const uint32_t ip = *reinterpret_cast<const uint32_t*>(ctx + 8);
				const uint8_t* instr;
				if (ip - RAM_START < RAM_SIZE)
				{
					instr = *reinterpret_cast<uint8_t* const*>(dllBase + RVA_RAM_BASE_PTR) + ip;
				}
				else
				{
					instr = *reinterpret_cast<uint8_t* const*>(ctx) + ip;
				}

				// Dispatch-table index, exactly as the original computes it: REG formats
				// (opcode 0x5x-0x7x) pack the sub-opcode from bits 7-10; the 0x8x-0xCx MEM
				// group packs bits 10-13, with bit 12 selecting the wider mask.
				const uint32_t word = *reinterpret_cast<const uint32_t*>(instr);
				// Every executed instruction passes through here, so this is where an HLE trap
				// can be counted and the IP sampled without patching anything in the module -
				// and it is the only vantage point that sees inside a frame.
				HleHooks::NoteFetchedWord(word);
				I960Profile::NoteFetch(ip);
				const uint32_t opcode = instr[3];
				uint32_t index = opcode << 4;
				const uint32_t group = opcode >> 4;
				if (group >= 5 && group <= 7)
				{
					index |= (word >> 7) & 0xF;
				}
				else if (group >= 8 && group <= 0xC)
				{
					index |= (((word >> 12) & 1) != 0 ? 0xFu : 0x8u) & (word >> 10);
				}

				const auto handler = *reinterpret_cast<int32_t(* const*)(const void*)>(
					dllBase + RVA_OPCODE_TABLE + static_cast<size_t>(index) * 8);
				*reinterpret_cast<int32_t*>(ctx + 8) += handler(instr);
			}
		}

		void InstallRamExecFetch(void* dll, const Imports& symbols)
		{
			const GameDesc& game = CurrentGame();
			// Motor Raid inlines fetch+decode into its execution loop, so it has neither the
			// single-step dispatcher to hook nor the i960 RVAs this reimplementation needs.
			// It also has no ROM debug menu to reach, so simply leave its CPU core alone.
			void* const fetchExec = symbols.TryGetSymbol(ImportSymbol::I960_FETCH_EXEC);
			if (fetchExec == nullptr || game.rva_cpu_ctx_ptr == 0)
			{
				return;
			}

			RamExecFetch::RVA_CPU_CTX_PTR = game.rva_cpu_ctx_ptr;
			RamExecFetch::RVA_OPCODE_TABLE = game.rva_opcode_table;
			RamExecFetch::RVA_RAM_BASE_PTR = game.rva_ram_base_ptr;
			RamExecFetch::dllBase = static_cast<uint8_t*>(dll);
			Trampoline* t = Trampoline::MakeTrampoline(dll);
			Memory::InjectHook(fetchExec, t->Jump(&RamExecFetch::FetchExec), Memory::HookType::Jump);
		}

		static void assign_helper_enable_shared_from_this(...)
		{
		}

		static float get_frame_speed_pause_stub()
		{
			return 1.0f;
		}

		static void* (*orgVF5AppCtor)(void* obj, int argc, char** argv);
		static void* VF5AppCtor_arguments(void* obj, int /*argc*/, char** /*argv*/)
		{
			return orgVF5AppCtor(obj, __argc, __argv);
		}
	}

