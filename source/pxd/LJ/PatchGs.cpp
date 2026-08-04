// The pxd DX12 gs bring-up: everything the host must put into a gs::context_t before a
// Lost-Judgment-era module will render, reverse-engineered from a live LJ dump (descriptor blocks at
// gs+0x7870/0x78B0/0x78F0/0x7930, the shader-visible descriptor-copy rings at gs+0x7558/0x7588/
// 0x75B8, the tex-id tables, the host cdevice behind cdevice_common::g_pD3DDevice).
//
// This is engine wiring, not per-game patching: it was extracted from m2ftg/LJ/Patch.cpp (which
// keeps the m2ftg-specific logging/traps/i960 patches) so that every LJ-era host — the m2ftg
// modules and VF5FS — shares one implementation.

#include "PatchGs.h"

#include "HostCdevice.h"
#include "pxd_types.h"

#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"
#include "../../RenderWindow.h"
#include "../../Utils/MemoryMgr.h"
#include "../../wil/com.h"

#include <d3d12.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace pxd
{
		template <size_t Gap18>
		static void InitTexIdTables(gs::context_tmpl<Gap18>* context)
		{
			if (!context) return;
			constexpr size_t kEntries = 0x10000;          // max id space
			constexpr size_t kTableBytes = kEntries * 8;  // 8B/entry: covers uint32 and ptr uses
			static const size_t kOffsets[4] = {
				gs::raw_off<Gap18>(0x79F8), gs::raw_off<Gap18>(0x7A00),
				gs::raw_off<Gap18>(0x7A08), gs::raw_off<Gap18>(0x7A10) };
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
		template <size_t Gap18>
		static bool SetupDescriptorBlock(gs::context_tmpl<Gap18>* context, size_t blockOffset,
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

		template <size_t Gap18>
		static void InitSrvUavDescriptorHeap(gs::context_tmpl<Gap18>* context, const RenderWindow& window)
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
			SetupDescriptorBlock(context, gs::raw_off<Gap18>(0x7930), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, false,
				262144, device, s_srvHeap, s_srvBitmap, "CBV/SRV/UAV");

			// RTV staging @ +0x78B0 (FUN_180094700 -> CreateRenderTargetView).
			static wil::com_ptr<ID3D12DescriptorHeap> s_rtvHeap;
			static std::vector<uint64_t> s_rtvBitmap;
			SetupDescriptorBlock(context, gs::raw_off<Gap18>(0x78B0), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, false,
				1024, device, s_rtvHeap, s_rtvBitmap, "RTV");

			// DSV staging @ +0x78F0 — LJ wires this block (incr 0x08, cap 1024); YAMP never had it.
			// The array order is SAMPLER(+0x7870), RTV(+0x78B0), DSV(+0x78F0), CBV/SRV/UAV(+0x7930).
			static wil::com_ptr<ID3D12DescriptorHeap> s_dsvHeap;
			static std::vector<uint64_t> s_dsvBitmap;
			SetupDescriptorBlock(context, gs::raw_off<Gap18>(0x78F0), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, false,
				1024, device, s_dsvHeap, s_dsvBitmap, "DSV");

			// SAMPLER staging @ +0x7870 — block[0] of the array. FUN_18009e620 bitmap-allocates a slot
			// and calls CreateSampler (params to FUN_1800acd90 are D3D12_SAMPLER_DESC fields) at the
			// CPU handle only. Non-shader-visible staging matches LJ (cap 256).
			static wil::com_ptr<ID3D12DescriptorHeap> s_samplerHeap;
			static std::vector<uint64_t> s_samplerBitmap;
			SetupDescriptorBlock(context, gs::raw_off<Gap18>(0x7870), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, false,
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
				const size_t     kRingBase      = gs::raw_off<Gap18>(0x7558);  // first inline ring
				// NB the ring base and the gs+0x7550 pointer to it sit either side of one of the
				// Gaiden insertions, so they shift by DIFFERENT amounts (0x28 and 0x20). Both go
				// through raw_off rather than one being derived from the other.
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
				void* const currentRing = gc + kRingBase + kCurrentRing * kRingStride;
				*reinterpret_cast<void**>(gc + gs::raw_off<Gap18>(0x7550)) = currentRing;

				// Builds that cache the current ring in the device context need it seeded with the
				// same pointer: the read is unconditional, ahead of the branch that would refresh
				// it, so leaving it null faults in the ring allocator on the first descriptor copy.
				// The ring-count word beside the pointer is deliberately left 0 (<= 1), which is
				// what keeps this build on Lost Judgment's behaviour of one fixed current ring
				// rather than rotating through rings the host has not prepared.
				if (gs::sm_dc.current_copy_ring != 0 && context->p_device_context != nullptr)
				{
					*reinterpret_cast<void**>(
						reinterpret_cast<uint8_t*>(context->p_device_context)
						+ gs::sm_dc.current_copy_ring) = currentRing;
				}
			}
		}

		// FUN_18009dbd0 is a per-FRAME transient descriptor-copy allocator: the cursor (ring+0x2C) is
		// atomically bumped for every table it copies and LJ RESETS it each frame (so it never grows
		// past the heap). YAMP seeds it once, so without a per-frame reset the cursor climbs
		// monotonically until CopyDescriptorsSimple writes one past the heap end (id=646 dest = heap
		// end + 0x20 -> nvwgf2umx driver AV). Reset all 3 rings to the seed at the start of each frame.
		template <size_t Gap18>
		void ResetCbvSrvRingCursors(gs::context_tmpl<Gap18>* context)
		{
			if (context == nullptr) return;
			uint8_t* gc = reinterpret_cast<uint8_t*>(context);
			const size_t ringBase = gs::raw_off<Gap18>(0x7558);
			for (int i = 0; i < 3; ++i)
				*reinterpret_cast<uint32_t*>(gc + ringBase + i * 0x30 + 0x2C) = 0x400;
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

		// --- Per-frame display descriptors: context+0x1B0.. and the array at context+0x2B0 ----------
		//
		// Replicated from a LIVE Lost Judgment capture in the VF5FS minigame (2026-07-29, x64dbg;
		// LJ's own gs context was at 0x143B722C0). LJ has:
		//   context+0x134            a frame index the engine ROTATES 0,1,2 (verified across frames)
		//   context+0x2B0 + i*0x10   3 entries { +0x00 48-bit packed pointer, +0x08 descriptor handle }
		//                            whose pointers are context+0x1B0 + i*0x40 and whose handles are
		//                            0x20 apart (one CBV_SRV_UAV descriptor, increment 32)
		//   context+0x1B0 + i*0x40   per-frame descriptor: +0x00 an ID3D12Resource* (confirmed: its
		//                            vtable is in d3d12sdklayers and the object carries the literal
		//                            string "ID3D12Re..."), then the metadata words below
		// YAMP leaves ALL of this zero, so the module's 2D fullscreen-rect flush reads entry[0], gets a
		// null packed pointer and faults reading [null+0x14] (FUN_18023D740 -> FUN_180233030 ->
		// FUN_180236490 at DLL+0x2364E2). No code in the module DLL ever writes these fields — only
		// dereferences them — the same signature as the gs+0x7550 rings and the host cdevice.
		//
		// The resource shape came from calling ID3D12Resource::GetDesc on one of LJ's three resources
		// in the debugger (vtable slot 10, context saved/restored around the call):
		//   TEXTURE2D 1776x956, 1 mip, B8G8R8A8_UNORM, Alignment 64KB, Flags ALLOW_RENDER_TARGET.
		// That also decodes two of the metadata words, so they are DERIVED from the dimensions here
		// rather than hardcoded:
		//   +0x0C = Width (0x6F0 = 1776)          +0x10 = (MipLevels << 16) | Height (0x000103BC)
		// +0x08 (0x01840000) and +0x14 (0x00C40101, whose bits 18..21 the DLL reads) are still copied
		// verbatim — their fields are not reversed yet, though +0x14 must encode the format/sample
		// state since the resource is B8G8R8A8 with 1 sample.
		// The captured 1776x956 was NOT a VF5FS constant: it was the size of LJ's window during the
		// capture, so these resources track the HOST's output resolution. They are therefore derived
		// from the RenderWindow below, and the two metadata words follow automatically. A stale
		// hardcoded size would put the module's 2D flush on a target that does not match the
		// swap chain at any resolution other than the one that happened to be captured.
		template <size_t Gap18>
		static void InitPerFrameDisplayDescriptors(gs::context_tmpl<Gap18>* context, const RenderWindow& window)
		{
			ID3D12Device* device = window.GetD3D12Device();
			if (device == nullptr)
			{
				DebugLogFile("[%s gs] per-frame display: no D3D12 device, skipped\n", gGeneral.GetGameTag());
				return;
			}

			constexpr size_t kFrames = 3;              // LJ rotates context+0x134 over 0..2
			constexpr size_t kDescStride = 0x40;       // spacing of the per-frame structs
			constexpr size_t kDescBase = 0x1B0;
			constexpr size_t kArrayBase = 0x2B0;

			// One descriptor per frame, contiguous so the handles come out 0x20 apart as in LJ.
			//
			// These MUST be RENDER TARGET views, not SRVs: the module hands entry+0x08 straight to
			// OMSetRenderTargets. With an SRV handle from a CBV_SRV_UAV heap the call reached
			// D3D12Core's OMSetRenderTargets helper and faulted inside nvwgf2umx (AV reading -1) —
			// captured from the crash stack. The resource is created ALLOW_RENDER_TARGET to match.
			static ID3D12DescriptorHeap* s_heap = nullptr;
			if (s_heap == nullptr)
			{
				D3D12_DESCRIPTOR_HEAP_DESC hd{};
				hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				hd.NumDescriptors = kFrames;
				hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // RTV heaps are never shader-visible
				if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_heap))))
				{
					DebugLogFile("[%s gs] per-frame display: CreateDescriptorHeap failed\n", gGeneral.GetGameTag());
					return;
				}
			}
			const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			const D3D12_CPU_DESCRIPTOR_HANDLE heapStart = s_heap->GetCPUDescriptorHandleForHeapStart();

		// Follow the host's output size (see the note above); LJ's capture was simply its window.
		const UINT kWidth = window.GetWidth() != 0 ? window.GetWidth() : 1776;
		const UINT kHeight = window.GetHeight() != 0 ? window.GetHeight() : 956;
		constexpr UINT16 kMips = 1;

			auto* base = reinterpret_cast<uint8_t*>(context);
			for (size_t i = 0; i < kFrames; ++i)
			{
				D3D12_HEAP_PROPERTIES hp{};
				hp.Type = D3D12_HEAP_TYPE_DEFAULT;
				D3D12_RESOURCE_DESC rd{};
				rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				rd.Alignment = 65536;                   // 0x10000, as LJ's
				rd.Width = kWidth;
				rd.Height = kHeight;
				rd.DepthOrArraySize = 1;
				rd.MipLevels = kMips;
				rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				rd.SampleDesc.Count = 1;
				rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

				ID3D12Resource* res = nullptr;
				if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res))))
				{
					DebugLogFile("[%s gs] per-frame display[%zu]: CreateCommittedResource failed\n",
						gGeneral.GetGameTag(), i);
					return;
				}

				D3D12_CPU_DESCRIPTOR_HANDLE h{ heapStart.ptr + i * incr };
				D3D12_RENDER_TARGET_VIEW_DESC rv{};
				rv.Format = rd.Format;
				rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				device->CreateRenderTargetView(res, &rv, h);

				// The per-frame descriptor struct. Words other than the resource pointer are the
				// captured LJ values (identical across its three frames).
				uint8_t* d = base + kDescBase + i * kDescStride;
				*reinterpret_cast<ID3D12Resource**>(d + 0x00) = res;
				*reinterpret_cast<uint32_t*>(d + 0x08) = 0x01840000;               // not yet decoded
				*reinterpret_cast<uint32_t*>(d + 0x0C) = kWidth;                   // LJ: 0x6F0 = 1776
				*reinterpret_cast<uint32_t*>(d + 0x10) = (kMips << 16) | kHeight;   // LJ: 0x000103BC
				*reinterpret_cast<uint32_t*>(d + 0x14) = 0x00C40101; // the word FUN_180236490 reads
				*reinterpret_cast<uint32_t*>(d + 0x18) = 0x2D1026D7; // magic, also at LJ's gs+0x1A8

				// The array entry: a 48-bit packed pointer to that struct (LJ's tag bits were 0), plus
				// the descriptor handle.
				uint8_t* e = base + kArrayBase + i * 0x10;
				*reinterpret_cast<uint64_t*>(e + 0x00) = reinterpret_cast<uintptr_t>(d) & 0x0000FFFFFFFFFFFFull;
				*reinterpret_cast<uint64_t*>(e + 0x08) = h.ptr;
			}

			// LJ also has this magic sitting just before the first struct; harmless to match.
			*reinterpret_cast<uint32_t*>(base + 0x1A8) = 0x2D1026D7;

			DebugLogFile("[%s gs] per-frame display descriptors wired: %zu frames at +0x%zX, array at +0x%zX\n",
				gGeneral.GetGameTag(), kFrames, kDescBase, kArrayBase);
		}

		template <size_t Gap18>
		void PatchGs(gs::context_tmpl<Gap18>* context, const RenderWindow& window)
		{
			// gs::initialize_module (0x180092fe0) validates the context header:
			// size_of_struct (+8) must equal the DLL's real value, then it imports the
			// sbgl shared symbols from context+0x20 (export_context.sbgl_context).
			// The value is per module build - 0x388A00 for the Lost Judgment / Y:LAD DLLs,
			// 0x3898C0 for the Like a Dragon Gaiden one - and lives in gs::context_traits so
			// the two numbers sit next to the layouts they belong to. The embedded template
			// already carries the right value; we set it defensively to match the DLL.
			context->size_of_struct = gs::context_traits<Gap18>::size_of_struct;

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
				*reinterpret_cast<void**>(dc + gs::sm_dc.render_state_block) = s_renderStateBlock;

				// +0xC8 : the "command recording" context. FUN_180097520 records draws into its +0x10
				// ID3D12GraphicsCommandList; same 0x280 layout as the cdevice+0x28 pool contexts.
				*reinterpret_cast<void**>(dc + gs::sm_dc.render_command_ctx) = BuildRenderCommandContext();

				// +0x12c88 : the per-device-context cache table for the transient vertex-upload ring
				// (FUN_18009f560: table+0x08 count, table+0x10 + idx*8 entries; idx is small). Host
				// never writes it (DLL only reads); a zeroed table is filled lazily by the engine.
				static uint8_t s_upCacheTable[0x810] = {}; // 256 entries + header
				*reinterpret_cast<void**>(dc + gs::sm_dc.up_cache_table) = s_upCacheTable;

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
						uint8_t* w = dc + gs::sm_dc.cbv_srv_ring;
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

					uint8_t* sc = dc + gs::sm_dc.sampler_ring;
					*reinterpret_cast<void**>(sc + 0x00)    = rr;                 // current-ring ptr
					*reinterpret_cast<void**>(sc + 0xD0)    = s_samplerSortTable; // struct's sorted table
					*reinterpret_cast<uint32_t*>(sc + 0xD8) = 0x200;            // table capacity
					*reinterpret_cast<uint32_t*>(sc + 0xDC) = 0;                 // entry count (skip search)
					DebugLog("[%s gs] sampler ring @ device_context+0x%zX wired\n",
						gGeneral.GetGameTag(), gs::sm_dc.sampler_ring);
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

				// Persistently-mapped upload buffer so the CPU-written vertices are GPU-readable.
				// Its SIZE is dictated by the module, not chosen here - see context_traits.
				static ID3D12Resource* s_upRing = nullptr;
				static void* s_upRingCpu = nullptr;
				if (s_upRing == nullptr)
				{
					D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
					D3D12_RESOURCE_DESC rd{};
					rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
					rd.Width = gs::context_traits<Gap18>::up_ring_bytes;
					rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
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

				// Fourth band: these sit past handle_fx, so they move by 0xBC0 in the Gaiden
				// layout rather than the 0xBA8 every named member moves by. Left unmapped, the
				// module read a garbage descriptor pointer and faulted copying it.
				*reinterpret_cast<void**>(gc + gs::raw_off<Gap18>(0x188200))     = s_upDesc;
				*reinterpret_cast<void**>(gc + gs::raw_off<Gap18>(0x188208))     = s_upRingCpu;
				*reinterpret_cast<uint32_t*>(gc + gs::raw_off<Gap18>(0x188210))  = 0;
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
				*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(device_context) + gs::sm_dc.push_up_pool) = pushPool;
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

			// The per-frame display descriptors (context+0x1B0.. and the array at context+0x2B0).
			// Host-provided like everything else here, but only the VF5FS module reads them, and the
			// metadata words are copied from a VF5FS capture — so keep the m2ftg games bit-identical
			// to before rather than writing fields they never touch.
			if (gGeneral.GetGameId() == YAMPGeneral::GameId::VF5FS_LJ)
			{
				InitPerFrameDisplayDescriptors(context, window);
			}
		}

		// The two shipped context layouts (see gs.h). Instantiated here rather than in the header
		// so this file's static helpers and its D3D12 includes stay out of everything that only
		// needs the declarations.
		template void PatchGs<gs::GAP18_LJ>(gs::context_t*, const RenderWindow&);
		template void PatchGs<gs::GAP18_GAIDEN>(gs::context_gaiden_t*, const RenderWindow&);
		template void ResetCbvSrvRingCursors<gs::GAP18_LJ>(gs::context_t*);
		template void ResetCbvSrvRingCursors<gs::GAP18_GAIDEN>(gs::context_gaiden_t*);
	}
