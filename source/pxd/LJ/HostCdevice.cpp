#include "HostCdevice.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "D3D12MemAlloc.h"
#include "../../wil/com.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <unordered_map>
#include <vector>

#include "../../DebugLog.h"
#include "../../YAMPGeneral.h" // gGeneral.GetGameTag() — this path is shared by StF and FV

// Defined in RenderWindow.cpp. The DLL binds descriptor heaps + never sets a root signature on a
// DIFFERENT command list than the one it draws on, so the draw list has neither. The SetPipelineState
// hook (which fires on the draw list) re-binds the DLL's own heaps + the PSO's root sig there.
ID3D12RootSignature*  GetCapturedRootSignature();
ID3D12DescriptorHeap* GetDllRingCbvSrvHeap();
ID3D12DescriptorHeap* GetDllRingSamplerHeap();
// True only for PSOs the DLL created (tracked in RenderWindow.cpp). The command-list vtable is shared
// with d3d11on12's blit list; gate the StF heap/root-sig injection on this so we don't corrupt the blit.
bool IsModulePso(void* pso);
// Defined in RenderWindow.cpp: dump DRED (last GPU op + page-fault resource) on device-removal, and
// append a line to d3d12_debug.log. Used by the frame-submit path to record a GPU hang once.
void DumpDredNow();

namespace pxd
{
		// ---- Game DLL address range (for return-address caller checks) ------
		// StF's DLL always loads at its fixed preferred base 0x180000000 (relocs work but
		// DYNAMIC_BASE is off), which the RA checks used to hardcode. FV's DLL is built WITH
		// DYNAMIC_BASE and relocates, so the range must come from the loaded module. Seeded
		// with the historical fixed range so nothing breaks if SetGameDllRange is never called.
		static uintptr_t g_gameDllBase = 0x180000000ull;
		static uintptr_t g_gameDllEnd  = 0x181000000ull;
		static bool IsGameDllAddr(uintptr_t addr)
		{
			return addr >= g_gameDllBase && addr < g_gameDllEnd;
		}
		void SetGameDllRange(void* dllBase)
		{
			const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(dllBase);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
				static_cast<const uint8_t*>(dllBase) + dos->e_lfanew);
			g_gameDllBase = reinterpret_cast<uintptr_t>(dllBase);
			g_gameDllEnd = g_gameDllBase + nt->OptionalHeader.SizeOfImage;
			DebugLogFile("[dll-range] game module 0x%llX..0x%llX\n",
				static_cast<unsigned long long>(g_gameDllBase),
				static_cast<unsigned long long>(g_gameDllEnd));
		}

		namespace
		{
			// ---- Sizing -----------------------------------------------------
			// The real object is >= 0x1A00 bytes; pad generously so every field
			// the DLL touches (up to +0x18dc) is in range.
			constexpr size_t kCdeviceSize = 0x2000;

			// Intermediate-buffer freelist: 64 nodes of 0x20 bytes (from LJ dump).
			constexpr uint32_t kNodeCount = 64;
			constexpr size_t   kNodeSize = 0x20;

			struct ibuf_node // 0x20 bytes
			{
				uint64_t next;   // +0x00 link (lo48 followed by the DLL pop)
				uint64_t f08;    // +0x08 fresh = 0xFFFFFFFFFFFFFFFF
				uint64_t gpuva;  // +0x10 fresh = 0 (bound to a real GPU VA on use)
				uint64_t size;   // +0x18 fresh = 0
			};
			static_assert(sizeof(ibuf_node) == kNodeSize, "node must be 0x20");

			// ---- Static storage --------------------------------------------
			alignas(16) uint8_t  s_cdevice[kCdeviceSize] = {};
			alignas(16) ibuf_node s_nodes[kNodeCount] = {};

			// cdevice+0x30 : upload-buffer POOL. The DLL manages a counter at +0x00,
			// an inline buffer array, and a lock-free free-stack at +0x2008 that it
			// self-populates via FUN_1800a0af0 -> our resource factory. Start zeroed.
			alignas(16) uint8_t s_uploadPool[0x4000] = {};
			// cdevice+0x58 : device/ring state object. Only [+0x67d0] (current upload
			// buffer id) is read on the boot path; zero is a valid initial value.
			alignas(16) uint8_t s_devState[0x8000] = {};

			ID3D12Device* s_device = nullptr;

			inline uint64_t& CdevU64(size_t off) { return *reinterpret_cast<uint64_t*>(s_cdevice + off); }
			inline uint32_t& CdevU32(size_t off) { return *reinterpret_cast<uint32_t*>(s_cdevice + off); }

			inline uint64_t PackHead(void* node, uint16_t aba)
			{
				return (static_cast<uint64_t>(aba) << 48) | (reinterpret_cast<uint64_t>(node) & 0x0000FFFFFFFFFFFFull);
			}

			// ---- Allocator object (cdevice+0x68) ----------------------------
			// DLL calls (**allocObj)(allocObj, size, align) / vtbl[1](allocObj, ptr).
			void* AllocFn(void* /*self*/, size_t size, size_t align)
			{
				// The pxd engine relies on its allocator returning ZEROED memory (matching the
				// host's csl_allocator). Under YAMP's debug CRT, un-zeroed blocks come back filled
				// with 0xCD, so uninitialized pointer/count fields (e.g. a render-batch count) read
				// as garbage and the game AVs dereferencing 0xCDCDCDCD... Zero every block.
				void* p = _aligned_malloc(size, align < 16 ? 16 : align);
				if (p) std::memset(p, 0, size);
				return p;
			}
			void FreeFn(void* /*self*/, void* p)
			{
				_aligned_free(p);
			}
			void* s_allocVtbl[4] = { reinterpret_cast<void*>(&AllocFn), reinterpret_cast<void*>(&FreeFn), nullptr, nullptr };
			struct AllocObj { void* vtbl; } s_allocObj = { s_allocVtbl };

			// ---- Resource factory (embedded at cdevice+0x17b0) --------------
			// [cdevice+0x17b0] = pointer to this vtable; self = &cdevice[0x17b0].
			// vf+8 creates a committed resource; *out must be an ID3D12Resource
			// (the DLL immediately calls out->SetName, ID3D12Object vf+0x30).
			uint64_t CreateResourceFn(void* /*self*/, void* desc, uint32_t fmt, void* p4,
				uint32_t tag, int /*zero*/, const wchar_t* name, void** out)
			{
				// Log the raw desc so we can learn its real layout from live runs.
				if (desc)
				{
					const uint64_t* d = reinterpret_cast<const uint64_t*>(desc);
					// TEMPORARY (pre3 bring-up): DebugLogFile, not DebugLog. This line was
					// OutputDebugString-only, so in a normal run it went nowhere and the factory
					// looked like it was never called. pre3's texture upload asks this factory for
					// an INTERMEDIATE TEXTURE (FUN_18007dda0 names the result
					// "pbgl::intermidate_texture%lld"), and we hand back a generic 8 MiB ROW_MAJOR
					// BUFFER regardless of the desc - which a CopyTextureRegion cannot read as a
					// texture subresource. Dump the real descs so the desc layout can be parsed.
					DebugLog(
						"[cdevice] CreateResource fmt=0x%X tag=0x%X desc=[%016llX %016llX %016llX %016llX %016llX %016llX]\n",
						fmt, tag,
						(unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2],
						(unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5]);
				}

				if (out) *out = nullptr;
				if (!s_device || !out) return 0x80004005; // E_FAIL

				// TEMP: create a generic upload buffer. Real size/type parsing of the
				// pxd desc comes next once we capture live descs under the debugger.
				const UINT64 kTempSize = 0x800000; // 8 MiB (matches pbgl::upload path)

				D3D12_HEAP_PROPERTIES hp = {};
				hp.Type = D3D12_HEAP_TYPE_UPLOAD;
				hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

				D3D12_RESOURCE_DESC rd = {};
				rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				rd.Alignment = 0;
				rd.Width = kTempSize;
				rd.Height = 1;
				rd.DepthOrArraySize = 1;
				rd.MipLevels = 1;
				rd.Format = DXGI_FORMAT_UNKNOWN;
				rd.SampleDesc.Count = 1;
				rd.SampleDesc.Quality = 0;
				rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				rd.Flags = D3D12_RESOURCE_FLAG_NONE;

				ID3D12Resource* res = nullptr;
				HRESULT hr = s_device->CreateCommittedResource(
					&hp, D3D12_HEAP_FLAG_NONE, &rd,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
					IID_PPV_ARGS(&res));
				if (FAILED(hr))
				{
					DebugLog("[%s cdevice] CreateCommittedResource failed hr=0x%08X\n", gGeneral.GetGameTag(), hr);
					return static_cast<uint64_t>(hr);
				}

				// Zero the buffer. CreateCommittedResource returns UNINITIALIZED memory, and the
				// upload data StF builds in these buffers has NaN in the 4th float of every
				// 32-byte vertex record (verified via the [copy] source dumps) while xyz are
				// valid and animating - i.e. the producer writes only the lanes it owns and
				// inherits garbage in the rest. The real pxd host serves these from zeroed
				// engine memory; NaN positions kill every triangle, so zero-fill on creation.
				{
					void* zp = nullptr; D3D12_RANGE zr{ 0, 0 };
					if (SUCCEEDED(res->Map(0, &zr, &zp)) && zp != nullptr)
					{
						std::memset(zp, 0, static_cast<size_t>(rd.Width));
						res->Unmap(0, nullptr);
					}
				}

				*out = res;
				(void)fmt; (void)p4; (void)tag; (void)name;
				return 0;
			}

			// Enough slots to survive an unexpected slot read; slot 1 (+8) is the create.
			void* s_factoryVtbl[16] = {};

			void SeedFreelist()
			{
				for (uint32_t i = 0; i < kNodeCount; ++i)
				{
					s_nodes[i].next = (i + 1 < kNodeCount)
						? reinterpret_cast<uint64_t>(&s_nodes[i + 1])
						: 0ull;
					s_nodes[i].f08 = 0xFFFFFFFFFFFFFFFFull;
					s_nodes[i].gpuva = 0;
					s_nodes[i].size = 0;
				}
				CdevU64(0x38) = PackHead(&s_nodes[0], 0);
			}

			// ---- AVL-map node pool (cdevice+0x16e0) --------------------------
			// The command contexts' resource-state map (cmdctx+0x80) allocates 0x20-byte nodes on
			// demand via FUN_180070220(*(cdevice+0x16e0), 0x20) -> lock-free pop (FUN_1800702c0
			// pushes them back when the map resets, so the pool recycles). The cdevice ctor ZEROES
			// cdevice+0x16e0; the host device-start builds this pool, so YAMP must too — else the
			// first resource-barrier record (FUN_180095560) pops from a null pool and derefs the
			// null node. Pool obj = { +0x00 uint node_size(>=0x20), +0x08 packed lock-free head };
			// nodes are 0x20 bytes with +0x00 = next (same packing as the +0x38 freelist).
			constexpr uint32_t kMapNodeCount = 8192; // shared by all cmdctxs; per-frame peak recycles
			struct map_node_pool { uint32_t node_size; uint32_t pad; uint64_t head; };
			static_assert(sizeof(map_node_pool) == 0x10, "pool obj: size@+0x00, head@+0x08");
			alignas(16) map_node_pool s_mapPool = {};
			alignas(16) ibuf_node     s_mapNodes[kMapNodeCount] = {};

			void SeedMapNodePool()
			{
				for (uint32_t i = 0; i < kMapNodeCount; ++i)
				{
					s_mapNodes[i].next  = (i + 1 < kMapNodeCount) ? reinterpret_cast<uint64_t>(&s_mapNodes[i + 1]) : 0ull;
					s_mapNodes[i].f08   = 0;
					s_mapNodes[i].gpuva = 0;
					s_mapNodes[i].size  = 0;
				}
				s_mapPool.node_size = 0x20;
				s_mapPool.head      = PackHead(&s_mapNodes[0], 0);
				CdevU64(0x16e0)     = reinterpret_cast<uint64_t>(&s_mapPool);
			}

			// ---- Deferred-destruction record pool (cdevice+0x90) -------------
			// When a texture/resource is destroyed (gs path 0x9ce70/0x9d725 -> FUN_18008e0a0) the
			// engine records a (48-bit value) into a node popped from a free-list at cdevice+0x90
			// (head@+0x90, tail@+0x98) and moves it to the in-use list at +0xA0/+0xA8. The nodes are
			// the 0x40-entry x 0x10-byte block the ctor reserved at cdevice+0xC8 (base ptr @+0xB0,
			// count @+0xB8), but the ctor leaves the free-list EMPTY — the host device-start links
			// it. YAMP must too, or the first resource destroy pops a null head and derefs it (AV in
			// FUN_18008dff0). The list is OFFSET-linked: node+0x00 = prev_off (prev-node), node+0x04 =
			// next_off (next-node-4); 0 terminates. (Derived from FUN_18008dff0/FUN_18008e0a0.)
			void SeedDeferredNodePool()
			{
				uint8_t* base  = reinterpret_cast<uint8_t*>(CdevU64(0xB0)); // = s_cdevice+0xC8
				uint32_t count = CdevU32(0xB8);                            // 64
				DebugLog("[%s cdevice] deferred pool ctor: base=%p count=%u\n",
					gGeneral.GetGameTag(), reinterpret_cast<void*>(base), count);
				if (!base || count == 0 || count > 256)
				{
					// The DLL ctor leaves +0xB0/+0xB8 empty (the host device-start normally fills them).
					// Match live LJ where pool+0xB0 = pool+0xC8 (inline node block) and pool+0xB8 = 0x40.
					base  = s_cdevice + 0xC8;
					count = 0x40;
					CdevU64(0xB0) = reinterpret_cast<uint64_t>(base);
					CdevU32(0xB8) = count;
					DebugLog("[%s cdevice] deferred pool: self-provisioned nodes @ cdevice+0xC8 x0x40\n", gGeneral.GetGameTag());
				}
				constexpr int32_t STRIDE = 0x10;
				for (uint32_t i = 0; i < count; ++i)
				{
					int32_t* node = reinterpret_cast<int32_t*>(base + i * STRIDE);
					node[0] = (i > 0)         ? -STRIDE      : 0;   // prev_off (prev - node)
					node[1] = (i + 1 < count) ? (STRIDE - 4) : 0;   // next_off (next - node - 4)
					*reinterpret_cast<uint64_t*>(base + i * STRIDE + 8) = 0;
				}
				CdevU64(0x90) = reinterpret_cast<uint64_t>(base);                          // free head = node[0]
				CdevU64(0x98) = reinterpret_cast<uint64_t>(base + (count - 1) * STRIDE);   // free tail = node[last]
			}

			// ---- Command-context pool (cdevice+0x28) ------------------------
			// The pxd DX12 upload path (static/IMMUTABLE buffers & textures) acquires a
			// "command context" from the cdevice+0x28 lock-free stack to record a GPU copy
			// (staging -> dest). The device-start (host, Denuvo-virtualized in LJ) pre-seeds
			// this pool; YAMP skips it, so the acquire (FUN_1800960f0) spins forever on empty.
			// We replicate one context exactly from live-LJ ground truth (scratchpad/cmdctx-spec.md):
			//   cmdctx (~0x270B) = { +0x00/+0x268 pxd vtable (never called by the upload path -> stub),
			//     +0x08/+0x10 ID3D12GraphicsCommandList, +0x20 -> alloc wrapper{+0x08 allocator},
			//     +0x28 -> self+0x38, +0x30 = 8, +0x80 = 0 (empty AVL resource-state map; its nodes are
			//     allocated on demand from the cdevice+0x16e0 pool), +0x88 -> self+0x98 (inline 16-entry
			//     barrier-record array), +0x90 = 0x10 capacity, +0x94 = 0 count, +0x258 = -1 }.
			// Release routes DIRECT-type contexts back to +0x28, so the pool self-sustains once seeded.
			constexpr uint32_t kCmdCtxCount = 8;
			constexpr size_t   kCmdCtxSize  = 0x280;

			struct cmdctx_pool_node { uint64_t next; uint64_t cmdctx; uint64_t f10; uint64_t f18; };
			static_assert(sizeof(cmdctx_pool_node) == 0x20, "pool node must be 0x20");

			struct alloc_wrapper { void* f00; void* alloc; void* f10; void* alloc2; };
			static_assert(sizeof(alloc_wrapper) == 0x20, "alloc wrapper must be 0x20");

			alignas(16) uint8_t s_cmdctx[kCmdCtxCount][kCmdCtxSize] = {};
			alloc_wrapper       s_cmdAllocWrap[kCmdCtxCount] = {};
			cmdctx_pool_node    s_cmdNodes[kCmdCtxCount] = {};

			// Dedicated storage for the cgs_device_context+0xC8 render command context (see
			// BuildRenderCommandContext). Same 0x280 layout as the +0x28 pool contexts, but its
			// command list is left OPEN (recording) because nothing acquires/reset()s it.
			alignas(16) uint8_t s_renderCmdCtx[kCmdCtxSize] = {};
			alloc_wrapper       s_renderCmdAllocWrap = {};

			// cmdctx+0x00/+0x268 pxd vtable: the pxd engine never calls it on the paths YAMP drives
			// (the frame submit is host-side — see SubmitModuleFrameList), but every slot is filled
			// with a return-0 no-op so an unexpected virtual call cannot fault on a null entry.
			uint64_t STDMETHODCALLTYPE CmdCtxStub(void*, void*, void*, void*) { return 0; }
			void* s_cmdCtxStubVtbl[24] = {};
			void FillCmdCtxStubVtbl()
			{
				for (auto& slot : s_cmdCtxStubVtbl) slot = reinterpret_cast<void*>(&CmdCtxStub);
			}

			// Build one pxd command context (0x280 layout, ground truth in scratchpad/cmdctx-spec.md)
			// into cc/w: creates a DIRECT command list + two allocators and fills the header fields the
			// engine reads (+0x10 cmdlist, +0x28 self+0x38 slot array, +0x80 empty AVL state map,
			// +0x88 self+0x98 barrier records, +0x94 count, +0x258 = -1). leaveOpen keeps the list in
			// the recording state (device-context path) vs Close()d for the acquire+reset() pool path.
			// Forward decls so the draw hooks (below) can flag the list they draw into as StF's. Flagging
			// by draw (during func()) is far more reliable than by PSO: StF's scene lists use PSOs from a
			// creation path IsModulePso() doesn't track, so PSO-flagging missed them and they never executed.
			void MarkModuleRenderList(ID3D12GraphicsCommandList*);
			bool ModuleRenderActive();

			// Draw hooks (slots 12/13): flag the list the module records into so SubmitModuleFrameList
			// knows which lists to close + execute. The counters drive the periodic [draw] health line
			// (a healthy StF run is ~0.7 draws/frame; zero means the module never reached render code).
			typedef void (STDMETHODCALLTYPE* DrawInstanced_t)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
			typedef void (STDMETHODCALLTYPE* DrawIndexed_t)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
			typedef void (STDMETHODCALLTYPE* Dispatch_t)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
			static DrawInstanced_t g_origDrawInstanced = nullptr;
			static DrawIndexed_t   g_origDrawIndexed   = nullptr;
			static Dispatch_t      g_origDispatch      = nullptr;
			static uint64_t g_drawInstCount = 0, g_drawIdxCount = 0, g_drawVertsTotal = 0;
			// Compute. Counted for the same reason the draws are: a module whose work is dispatched
			// rather than drawn would otherwise show up as a completely idle frame. pre3 (Model 3)
			// is the case that needs it — a PIX capture of Like a Dragon Gaiden running Fighting
			// Vipers 2 has 3 Dispatch calls per frame alongside the module's 189 draws, and with
			// only the draw hooks in place there was no way to tell whether ours were running.
			static uint64_t g_dispatchCount = 0;
			// Totals for the two render-entry probes below. Those log only their first few calls, so
			// without these the tally line is the only place the real counts appear — reading a
			// capped log as a count is a mistake worth not repeating.
			extern uint64_t g_moduleBufCopyCount;
			extern uint64_t g_moduleTexCopyCount;
			extern uint64_t g_moduleTexCopyOutsideFrame;
			static void LogDrawTally()
			{
				// The format string used to take six specifiers while eight arguments were passed,
				// so the two texture-copy totals were silently dropped. They are the health signal
				// for pre3's Model 3 texture uploads, so they are printed now.
				if ((g_drawInstCount + g_drawIdxCount) % 100 == 1)
					DebugLogFile("[draw] instanced=%llu indexed=%llu vertsTotal=%llu dispatch=%llu"
						" bufcopy=%llu texcopy=%llu texcopyLoader=%llu\n",
						(unsigned long long)g_drawInstCount, (unsigned long long)g_drawIdxCount,
						(unsigned long long)g_drawVertsTotal, (unsigned long long)g_dispatchCount,
						(unsigned long long)g_moduleBufCopyCount,
						(unsigned long long)g_moduleTexCopyCount,
						(unsigned long long)g_moduleTexCopyOutsideFrame);
			}

			static void STDMETHODCALLTYPE HookedDispatch(ID3D12GraphicsCommandList* self,
				UINT x, UINT y, UINT z)
			{
				g_dispatchCount++;
				// Flagged like a draw: a list that only ever dispatches is still the module's, and
				// SubmitModuleFrameList has to close and execute it or the work never reaches the GPU.
				if (ModuleRenderActive()) MarkModuleRenderList(self);
				// Logged individually rather than via the shared tally — these are rare enough (a
				// handful per frame) that the first few are worth seeing in full, and a module that
				// dispatches zero times never reaches LogDrawTally's 500-call cadence at all.
				if (g_dispatchCount <= 8)
					DebugLogFile("[dispatch] #%llu %ux%ux%u%s\n", (unsigned long long)g_dispatchCount,
						x, y, z, ModuleRenderActive() ? " (module)" : "");
				g_origDispatch(self, x, y, z);
			}

			static void STDMETHODCALLTYPE HookedDrawInstanced(ID3D12GraphicsCommandList* self,
				UINT vpi, UINT inst, UINT svl, UINT sil)
			{
				g_drawInstCount++; g_drawVertsTotal += static_cast<uint64_t>(vpi) * (inst ? inst : 1);
				if (ModuleRenderActive()) MarkModuleRenderList(self); // this list records the module's scene
				LogDrawTally();
				g_origDrawInstanced(self, vpi, inst, svl, sil);
			}

			static void STDMETHODCALLTYPE HookedDrawIndexed(ID3D12GraphicsCommandList* self,
				UINT ipi, UINT inst, UINT sil, INT bvl, UINT sivl)
			{
				g_drawIdxCount++; g_drawVertsTotal += static_cast<uint64_t>(ipi) * (inst ? inst : 1);
				if (ModuleRenderActive()) MarkModuleRenderList(self); // this list records the module's scene
				LogDrawTally();
				g_origDrawIndexed(self, ipi, inst, sil, bvl, sivl);
			}

			typedef void (STDMETHODCALLTYPE* SetPSO_t)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
			static SetPSO_t g_origSetPSO = nullptr;
			static void STDMETHODCALLTYPE HookedSetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso)
			{
				g_origSetPSO(self, pso);
				// The DLL binds the shader-visible heaps + never sets a root signature on a DIFFERENT
				// command list than the draw list. So on THIS (draw) list, right after the PSO and
				// before it binds the root descriptor table, re-bind the DLL's own heaps and the PSO's
				// root sig. Fixes id=708 ("no root signature") + "no CBV_SRV_UAV heap set".
				// ONLY for StF PSOs: this vtable is shared with d3d11on12's blit list, and injecting
				// StF's state there makes d3d11on12's own root-table calls run against the wrong root
				// signature -> driver AV. d3d11on12's blit PSO is not tracked, so it is skipped.
				if (!IsModulePso(pso)) return;
				MarkModuleRenderList(self); // this list records StF's scene -> execute it when StF closes it
				ID3D12DescriptorHeap* heaps[2] = { GetDllRingCbvSrvHeap(), GetDllRingSamplerHeap() };
				if (heaps[0] && heaps[1])
					self->SetDescriptorHeaps(2, heaps);
				if (ID3D12RootSignature* rs = GetCapturedRootSignature())
					self->SetGraphicsRootSignature(rs);
			}
			// ResourceBarrier hook (slot 26): corrects the module's transition StateBefore values (see
			// below) and catalogs the render targets it draws into, which the host composite picks its
			// display source from (GetModuleRenderTarget / GetModuleRenderTargetState).
			typedef void (STDMETHODCALLTYPE* ResourceBarrier_t)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
			static ResourceBarrier_t g_origResourceBarrier = nullptr;
			ID3D12Resource* g_rtSeen[64] = {};
			int g_rtSeenCount = 0;
			D3D12_RESOURCE_STATES g_rtLastState[64] = {}; // last StateAfter StF transitioned each RT to

			// --- Barrier StateBefore correction ------------------------------------------------------
			// StF's render targets ping-pong across frames: each frame a barrier transitions them FROM
			// NON_PIXEL|PIXEL_SHADER_RESOURCE (0xC0, where the PREVIOUS frame left them) TO RENDER_TARGET,
			// draws, then back to 0xC0 (PIX export CommandLists_000: nearly every RT barrier's StateBefore
			// is 0xC0). But YAMP creates every resource in COMMON, so on the FIRST frame that 0xC0 before-
			// state does not match reality -> the D3D12 runtime rejects the barrier (d3d12_debug.log id=527
			// "Before state RENDER_TARGET ... does not match ... COMMON"), the RT never enters RENDER_TARGET,
			// the draws are invalid, and the desync is permanent. Fix: track each resource's true current
			// state (seeded COMMON at first sight) and REWRITE every StF transition barrier's StateBefore to
			// that tracked value, then advance the tracked state to StateAfter. This bootstraps the ping-pong
			// (frame 1: 0xC0->RT becomes COMMON->RT, valid) and stays self-consistent every frame after.
			// Gated to g_inModuleRender so it only touches StF's own barriers (recorded inside func()), never
			// d3d11on12's blit barriers (recorded later, in BlitDX12Texture) which are already correct.
			static std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> g_resState;
			// Guards g_resState: written by the resource-creation hooks (may run on an asset-loader thread)
			// and read/written by the ResourceBarrier hook (render thread). Cheap relative to the per-frame
			// GPU flush.
			static std::mutex g_resStateMutex;
			static bool g_inModuleRender = false;
			void SetModuleRenderActive(bool active) { g_inModuleRender = active; }
			bool ModuleRenderActive() { return g_inModuleRender; }

			// The swapchain backbuffers, registered from RenderWindow::CreateWrappedBackbuffers. Their
			// transitions are driven by d3d11on12 + TransitionBackbufferToRenderTarget, so the barrier
			// corrector below must leave them strictly alone.
			static ID3D12Resource* g_watchRes[8] = {};
			static int g_watchCount = 0;
			void RegisterWatchResource(ID3D12Resource* r)
			{
				if (!r || g_watchCount >= 8) return;
				for (int i = 0; i < g_watchCount; ++i) if (g_watchRes[i] == r) return;
				g_watchRes[g_watchCount++] = r;
				DebugLogFile("[watch] registered backbuffer #%d = %p\n", g_watchCount - 1, static_cast<void*>(r));
			}
			static bool IsWatched(ID3D12Resource* r)
			{
				for (int i = 0; i < g_watchCount; ++i) if (g_watchRes[i] == r) return true;
				return false;
			}

			static void STDMETHODCALLTYPE HookedResourceBarrier(
				ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* bars)
			{
				std::vector<D3D12_RESOURCE_BARRIER> patched;
				const D3D12_RESOURCE_BARRIER* pass = bars;
				// Stage an editable copy for ANY call with barriers — the correction now applies both inside
				// func() (g_inModuleRender) and to StF-created resources barriered outside it. Barriers we don't
				// touch are copied verbatim, so passing `patched` is identical to `bars` for them.
				if (bars)
					patched.assign(bars, bars + n);

				if (bars)
				{
					for (UINT i = 0; i < n; ++i)
					{
						if (bars[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
						ID3D12Resource* r = bars[i].Transition.pResource;
						if (!r) continue;

						// Correct StateBefore to our tracked current state, then advance the tracking. Only
						// simple (non-split) whole-resource transitions are rewritten; leave split barriers
						// (BEGIN/END flags) untouched so we never desync a half-transition. Apply during
						// func() (StF's own render) OR to any resource StF created (now known via the
						// creation-seed hook) — the id=527 texture desyncs happen on StF's draw lists that
						// aren't always bracketed by g_inModuleRender. NEVER touch the watched swapchain
						// backbuffers: those are driven by 11on12 + TransitionBackbufferToRenderTarget.
						std::unique_lock<std::mutex> resLk(g_resStateMutex);
						const bool knownModuleRes = (g_resState.find(r) != g_resState.end());
						if (bars[i].Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE && !IsWatched(r)
							&& (g_inModuleRender || knownModuleRes))
						{
							auto it = g_resState.find(r);
							const D3D12_RESOURCE_STATES cur = (it != g_resState.end())
								? it->second : D3D12_RESOURCE_STATE_COMMON; // fallback if never seen created
							if (cur != bars[i].Transition.StateAfter)     // skip no-op self-transitions
								patched[i].Transition.StateBefore = cur;
							g_resState[r] = bars[i].Transition.StateAfter;
						}

						// Catalog every DISTINCT resource StF transitions INTO RENDER_TARGET (its RTs), and
						// track the LAST state it leaves each in — 11on12 must be told that exact state to
						// read the RT correctly (separate state tracking, else it reads compressed garbage).
						if (bars[i].Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET)
						{
							bool seen = false;
							for (int k = 0; k < g_rtSeenCount; ++k) if (g_rtSeen[k] == r) { seen = true; break; }
							if (!seen && g_rtSeenCount < 64)
							{
								g_rtSeen[g_rtSeenCount] = r;
								g_rtLastState[g_rtSeenCount] = bars[i].Transition.StateAfter;
								g_rtSeenCount++;
								const D3D12_RESOURCE_DESC d = r->GetDesc();
								DebugLog("[rt] #%d res=%p W=%llu H=%u fmt=%d flags=0x%X\n",
									g_rtSeenCount - 1, static_cast<void*>(r),
									static_cast<unsigned long long>(d.Width), d.Height, d.Format,
									static_cast<unsigned int>(d.Flags));
							}
						}
						// Keep the last-known state for any RT we track (whatever StF moves it to).
						for (int k = 0; k < g_rtSeenCount; ++k)
							if (g_rtSeen[k] == r) { g_rtLastState[k] = bars[i].Transition.StateAfter; break; }
					}
				}

				if (bars) pass = patched.data();
				g_origResourceBarrier(self, n, pass);
			}

			// YAMP's own D3D12 queue — the one the frame batch is submitted on.
			static ID3D12CommandQueue* g_yampQueue = nullptr;

			// Close(slot 9) / Reset(slot 10) hooks. The module never closes or resets its own lists
			// (the host's job), but the ORIGINAL entry points are needed: the frame submit must close
			// and reopen those lists without re-entering the hooks.
			typedef HRESULT(STDMETHODCALLTYPE* Close_t)(ID3D12GraphicsCommandList*);
			typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
			static Close_t g_origClose = nullptr;
			static Reset_t g_origReset = nullptr;

			// THE FIX: StF records its scene into SEVERAL command lists and never submits them — LJ's HOST
			// does, via ONE ExecuteCommandLists(N, lists) + ONE FlushCommandQueue (PIX export
			// RenderFrame_000 submits 10 lists in a single call). Executing them one-at-a-time with a flush
			// between BREAKS D3D12's persistent per-resource state tracking: StF's later lists barrier a
			// resource FROM RENDER_TARGET, but if the earlier list that transitioned it INTO RENDER_TARGET
			// (and did the scene draws) has not run first, the runtime still has it in COMMON
			// (d3d12_debug.log id=527 "Before state RENDER_TARGET ... does not match ... COMMON") and the
			// draws are dropped -> black. So collect EVERY StF list StF closes this frame, in close order,
			// and submit them together in one call, flushing once.
			static ID3D12GraphicsCommandList* g_moduleFlagged[64] = {}; // lists flagged StF since their last Reset
			static int g_moduleFlaggedCount = 0;
			// Per-list allocator we own, so we can Reset (reopen) StF's lists each frame for it to re-record.
			static std::unordered_map<ID3D12GraphicsCommandList*, ID3D12CommandAllocator*> g_listAlloc;
			static ID3D12Fence* g_execFence = nullptr; static UINT64 g_execFv = 0; static HANDLE g_execEv = nullptr;
			void MarkModuleRenderList(ID3D12GraphicsCommandList* l) // "this list is StF's" (from the PSO hook)
			{
				if (!l) return;
				for (int i = 0; i < g_moduleFlaggedCount; ++i) if (g_moduleFlagged[i] == l) return;
				if (g_moduleFlaggedCount < 64) g_moduleFlagged[g_moduleFlaggedCount++] = l;
				static int m = 0; if (m < 8) { DebugLogFile("[mark] %s list %p type=%d (flagged=%d)\n", gGeneral.GetGameTag(), static_cast<void*>(l), l->GetType(), g_moduleFlaggedCount); } m++;
			}
			static void UnflagModuleList(ID3D12GraphicsCommandList* l) // on Reset the recording is gone; re-earn it
			{
				for (int i = 0; i < g_moduleFlaggedCount; ++i) if (g_moduleFlagged[i] == l) { g_moduleFlagged[i] = g_moduleFlagged[--g_moduleFlaggedCount]; return; }
			}

			// Frame-end submit (called from GameLoop after func()): ONE ExecuteCommandLists + ONE flush.
			// One-shot self-disable: if a submit removes the device (bad descriptor/resource -> GPU hang ->
			// TDR), detect it, dump DRED, and stop executing so we can never re-freeze the desktop frame
			// after frame — the failure is captured once, not repeated.
			static bool g_execDisabled = false;
			bool ModuleExecDisabled() { return g_execDisabled; }

				// PATH B (multi-list): StF records its frame across SEVERAL command lists and NEVER
				// Close/Execute/Reset-s any of them (host's job; dynamically confirmed closes=resets=0). Dynamic
				// capture proved StF draws into 2+ lists per scene frame (flaggedLists=2); submitting only the
				// LAST one left the earlier list's draws unexecuted -> black screen. So Close EVERY
				// list StF drew into this frame (flagged in draw order by the draw/PSO hooks), ExecuteCommandLists
				// them ALL in ONE call (D3D12 carries resource state across them, like LJ's RenderFrame submitting
				// ~10 lists at once), flush once, then Reset each (its own allocator) so StF re-records next frame.
				ID3D12GraphicsCommandList* ShadowCopyCloseForSubmit(); // defined with the copy hooks below
				void ShadowCopyReopen();
				void SubmitModuleFrameList()
				{
					if (g_execDisabled || !g_yampQueue || !s_device) return;

					ID3D12CommandList* lists[64]; ID3D12GraphicsCommandList* gl[64]; int nn = 0;
					// The shadow copy list goes FIRST: StF's one-time buffer uploads (quad IB etc.),
					// replayed from its loader threads' recordings, must land before the draws that
					// consume them. It is YAMP-owned so closing it here races nothing.
					int shadowIdx = -1;
					if (ID3D12GraphicsCommandList* sc = ShadowCopyCloseForSubmit())
					{
						shadowIdx = nn; gl[nn] = sc; lists[nn] = sc; ++nn;
					}
					for (int i = 0; i < g_moduleFlaggedCount && nn < 64; ++i)
					{
						ID3D12GraphicsCommandList* l = g_moduleFlagged[i];
						if (!l || l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) continue;
						const HRESULT hrc = g_origClose ? g_origClose(l) : l->Close(); // StF leaves them OPEN
						if (FAILED(hrc))
						{
							static int e = 0; if (e++ < 8) { DebugLogFile("[pathb] Close failed 0x%08X list=%p (skip)\n", static_cast<unsigned>(hrc), static_cast<void*>(l)); }
							continue;
						}
						gl[nn] = l; lists[nn] = l; ++nn;
					}
					if (nn == 0) { g_moduleFlaggedCount = 0; return; }

					g_yampQueue->ExecuteCommandLists(static_cast<UINT>(nn), lists);

					if (!g_execFence) { s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_execFence)); g_execEv = CreateEventA(nullptr, FALSE, FALSE, nullptr); }
					bool timedOut = false;
					if (g_execFence) { g_yampQueue->Signal(g_execFence, ++g_execFv); if (g_execFence->GetCompletedValue() < g_execFv) { g_execFence->SetEventOnCompletion(g_execFv, g_execEv); timedOut = (WaitForSingleObject(g_execEv, 4000) == WAIT_TIMEOUT); } }
					static int f = 0; if (f < 8) { DebugLogFile("[pathb] submitted %d %s list(s)%s\n", nn, gGeneral.GetGameTag(), timedOut ? " (FLUSH TIMEOUT - GPU HANG)" : ""); } f++;

					const HRESULT rr = s_device->GetDeviceRemovedReason();
					if (timedOut || FAILED(rr))
					{
						DebugLogFile("[pathb] device removed/hang (0x%08X) disabling + DRED\n", static_cast<unsigned>(rr)); DumpDredNow(); g_execDisabled = true; return;
					}

					// Reopen each list for next frame (StF never resets them). Each needs its own allocator, so use
					// the per-list g_listAlloc map (one allocator cannot back two open lists). The shadow copy
					// list has its own allocator + reopen path.
					if (g_origReset)
					{
						for (int i = 0; i < nn; ++i)
						{
							if (i == shadowIdx) continue;
							ID3D12CommandAllocator*& a = g_listAlloc[gl[i]];
							if (!a) s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
							if (a) { a->Reset(); g_origReset(gl[i], a, nullptr); }
						}
					}
					if (shadowIdx >= 0) ShadowCopyReopen();
					g_moduleFlaggedCount = 0; // consumed this frame's lists; the draw hooks re-flag next frame
				}

			// ---- Per-frame upload-pool recycle (the host frame-advance) --------------------------------
			// StF's upload allocator (FUN_1800a0cf0) tags each 8MB buffer with the frame stamp at
			// s_devState+0x67d8 and, on every pop from the pool's "available" node-stack
			// (s_uploadPool+0x2008), pushes the buffer's node onto the "in-use" node-stack
			// (s_uploadPool+0x2010). The HOST owns the other half of the cycle: each frame it (1) advances
			// the stamp so the allocator starts a fresh linear buffer, and (2) returns completed frames'
			// in-use nodes to the available stack. LJ's engine does exactly this
			// (lostjudgment.exe+0x1849A88: ++[devobj+0x67d8] then a fence-gated recycle FUN_141849F60).
			// YAMP never did either, so the available stack drained, FUN_1800a0bb0 refilled a NEW 8MB
			// buffer every ~8MB uploaded, the pool's inline buffer array overran its +0x2008 head at ~512
			// buffers, and StF crashed in FUN_18009be60 (~frame 570). YAMP flushes the GPU every frame
			// (SubmitModuleFrameList), so at each frame boundary ALL in-use buffers are GPU-complete and can
			// be recycled unconditionally — no fence tracking needed.
			void AdvanceFrameStamp()
			{
				// (1) advance the upload-frame stamp: next frame's first alloc sees tag != stamp and pops a
				// fresh buffer (offset reset to 0) instead of growing one buffer for the whole session.
				uint64_t& stamp = *reinterpret_cast<uint64_t*>(s_devState + 0x67d8);
				++stamp;

				// (2) recycle: move every node from the in-use stack (+0x2010) back to the available stack
				// (+0x2008). We run single-threaded at the frame boundary (StF is idle), so plain moves are
				// safe — but preserve the packed 48-bit-ptr / 16-bit-ABA head layout the allocator's
				// lock-free pop (FUN_1800a0cf0) and push (FUN_1800702d0) expect.
				uint64_t* inUse = reinterpret_cast<uint64_t*>(s_uploadPool + 0x2010);
				uint64_t* avail = reinterpret_cast<uint64_t*>(s_uploadPool + 0x2008);
				uint32_t moved = 0;
				for (;;)
				{
					const uint64_t ih = *inUse;
					uint64_t* node = reinterpret_cast<uint64_t*>(static_cast<int64_t>(ih << 16) >> 16); // lo48
					if (!node) break;
					*inUse = (ih & 0xFFFF000000000000ull) + 0x1000000000000ull | (*node & 0xFFFFFFFFFFFFull); // pop
					const uint64_t ah = *avail;
					*node  = static_cast<uint64_t>(static_cast<int64_t>(ah << 16) >> 16);                    // node->next = avail head ptr
					*avail = (ah & 0xFFFF000000000000ull) + 0x1000000000000ull |
						(reinterpret_cast<uint64_t>(node) & 0xFFFFFFFFFFFFull);                              // push
					if (++moved > 100000) break; // paranoia: never spin on a corrupt list
				}

				// Diagnostic: uploadRefills is the pool's monotonic refill counter (FUN_1800a0bb0 bumps
				// *pool). With the recycle working it PLATEAUS (pool self-limits to one frame's peak); if it
				// keeps climbing, buffers aren't returning and the crash will still come.
				static int fr = 0;
				if (fr < 40 || fr % 60 == 0)
				{
					DebugLogFile("[stamp] frame=%d stamp=%llu recycled=%u uploadRefills=%u\n",
						fr, static_cast<unsigned long long>(stamp), moved,
						*reinterpret_cast<uint32_t*>(s_uploadPool));
				}
				fr++;
			}

			// Reset clears a list's recording, so it must re-earn its module flag via a new module draw
			// before the frame submit will close + execute it.
			static HRESULT STDMETHODCALLTYPE HookedReset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
			{
				UnflagModuleList(self);
				return g_origReset(self, a, p);
			}

			// ---- Resource-creation hooks: seed the barrier-state tracker with TRUE InitialState --------
			// id=527 root cause: StF creates textures shader-readable (0xC0) but its engine's barrier
			// state-tracker initializes new resources to COMMON, so the FIRST transition barrier declares
			// before=COMMON while the runtime knows the resource is in 0xC0 -> desync -> the sampled texture
			// reads back as black -> black screen. HookedResourceBarrier can only fix this if it knows the
			// resource's real starting state; on first sight it otherwise assumes COMMON (agreeing with
			// StF's wrong value and never correcting it). So record every texture's real InitialState into
			// g_resState at creation. Covers StF's own CreateCommittedResource AND D3D12MA's placed-resource
			// path (D3D12MA calls CreatePlacedResource on this same device). Buffers are skipped: the runtime
			// forces them to COMMON regardless of InitialState (id=1328), so tracking their requested state
			// would itself create a false mismatch.
			typedef HRESULT(STDMETHODCALLTYPE* CreateCommitted_t)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
			typedef HRESULT(STDMETHODCALLTYPE* CreatePlaced_t)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
			static CreateCommitted_t g_origCreateCommitted = nullptr;
			static CreatePlaced_t    g_origCreatePlaced    = nullptr;

			static void SeedCreatedResourceState(const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st, REFIID riid, void** ppv)
			{
				if (!ppv || !*ppv || !desc) return;
				if (riid != __uuidof(ID3D12Resource)) return;
				// The runtime forces buffers to COMMON regardless of InitialState (id=1328); tracking
				// their requested state would itself create a false mismatch.
				if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return;
				std::lock_guard<std::mutex> lk(g_resStateMutex);
				g_resState[static_cast<ID3D12Resource*>(*ppv)] = st; // TRUE initial state for the barrier corrector
			}
			static HRESULT STDMETHODCALLTYPE HookedCreateCommitted(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
				const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv)
			{
				const HRESULT hr = g_origCreateCommitted(self, hp, hf, desc, st, cv, riid, ppv);
				if (SUCCEEDED(hr)) SeedCreatedResourceState(desc, st, riid, ppv);
				return hr;
			}
			static HRESULT STDMETHODCALLTYPE HookedCreatePlaced(ID3D12Device* self, ID3D12Heap* heap, UINT64 off,
				const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv)
			{
				const HRESULT hr = g_origCreatePlaced(self, heap, off, desc, st, cv, riid, ppv);
				if (SUCCEEDED(hr)) SeedCreatedResourceState(desc, st, riid, ppv);
				return hr;
			}
			static void HookDeviceResourceCreation(ID3D12Device* dev)
			{
				if (g_origCreateCommitted || !dev) return; // once
				void** vtbl = *reinterpret_cast<void***>(dev);
				DWORD op = 0;
				if (VirtualProtect(&vtbl[27], sizeof(void*), PAGE_READWRITE, &op)) // CreateCommittedResource
				{
					g_origCreateCommitted = reinterpret_cast<CreateCommitted_t>(vtbl[27]);
					vtbl[27] = reinterpret_cast<void*>(&HookedCreateCommitted);
					VirtualProtect(&vtbl[27], sizeof(void*), op, &op);
				}
				if (VirtualProtect(&vtbl[29], sizeof(void*), PAGE_READWRITE, &op)) // CreatePlacedResource
				{
					g_origCreatePlaced = reinterpret_cast<CreatePlaced_t>(vtbl[29]);
					vtbl[29] = reinterpret_cast<void*>(&HookedCreatePlaced);
					VirtualProtect(&vtbl[29], sizeof(void*), op, &op);
				}
			}

			typedef void (STDMETHODCALLTYPE* CopyBufferRegion_t)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
			static CopyBufferRegion_t g_origCopyBufferRegion = nullptr;
			// *** IB-FILL FIX v2 — SHADOW COPY LIST (2026-07-26; replaces the v1 "flag copy lists"
			// approach). StF records its ONE-TIME buffer uploads (e.g. the emulator's 24576-index
			// tile-grid quad IB) on loader lists that never draw and are recorded from WORKER THREADS
			// at arbitrary times. v1 flagged those lists so SubmitModuleFrameList would close/execute/
			// reset them each frame — but closing a list another thread is mid-recording corrupts or
			// drops its commands: intermittent per-frame corruption ("HUD flashes wildly", garbled
			// stage transitions). v2: NEVER touch StF's loader lists. Instead REPLAY every StF buffer
			// copy onto a YAMP-owned shadow list submitted with the frame batch. Buffer dsts need no
			// barriers (implicit COMMON<->COPY_DEST promotion/decay; the frame flush guarantees decay),
			// and re-executing a same-data copy is idempotent, so shadowing draw-list copies too is
			// harmless. The copy SOURCES are StF's persistent CPU-mapped intermediate buffers, alive
			// across frames.
			static std::mutex g_shadowCopyMutex; // copies arrive from multiple StF threads
			static ID3D12CommandAllocator* s_shadowCopyAlloc = nullptr;
			static ID3D12GraphicsCommandList* s_shadowCopyList = nullptr;
			static unsigned s_shadowCopyPending = 0;
			static void ShadowRecordBufferCopy(ID3D12Resource* dst, UINT64 dstOff, ID3D12Resource* src, UINT64 srcOff, UINT64 bytes)
			{
				std::lock_guard<std::mutex> lk(g_shadowCopyMutex);
				if (!s_shadowCopyList)
				{
					if (!s_device) return;
					s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_shadowCopyAlloc));
					if (s_shadowCopyAlloc)
						s_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_shadowCopyAlloc, nullptr, IID_PPV_ARGS(&s_shadowCopyList));
					if (!s_shadowCopyList) return;
				}
				// Record via the ORIGINAL vtable entry (the shared-vtable hook would re-enter us).
				g_origCopyBufferRegion(s_shadowCopyList, dst, dstOff, src, srcOff, bytes);
				++s_shadowCopyPending;
			}
			// Called by SubmitModuleFrameList: close the shadow list and hand it over for the frame batch
			// (executes FIRST so one-time uploads land before the draws that consume them). Returns
			// null if nothing was recorded. Caller must ShadowCopyReopen() after the flush.
			ID3D12GraphicsCommandList* ShadowCopyCloseForSubmit()
			{
				std::lock_guard<std::mutex> lk(g_shadowCopyMutex);
				if (!s_shadowCopyList || s_shadowCopyPending == 0) return nullptr;
				if (FAILED(g_origClose(s_shadowCopyList))) return nullptr;
				return s_shadowCopyList;
			}
			void ShadowCopyReopen()
			{
				std::lock_guard<std::mutex> lk(g_shadowCopyMutex);
				if (!s_shadowCopyList || !s_shadowCopyAlloc) return;
				s_shadowCopyAlloc->Reset();
				g_origReset(s_shadowCopyList, s_shadowCopyAlloc, nullptr);
				s_shadowCopyPending = 0;
			}
			// Buffer copies the MODULE issued. Counted (and the first few logged) because the copy
			// is the one part of a module's frame that happens before any draw: pre3's render pass
			// opens by uploading its 0x120000 scroll buffer, so whether that copy appears at all
			// separates "the render path never started" from "it started and found nothing to draw".
			uint64_t g_moduleBufCopyCount = 0;

			static void STDMETHODCALLTYPE HookedCopyBufferRegion(ID3D12GraphicsCommandList* self, ID3D12Resource* dst, UINT64 dstOff, ID3D12Resource* src, UINT64 srcOff, UINT64 bytes)
			{
				// GATE ON THE CALLER, not ModuleRenderActive(): the module records copies from
				// loader/worker threads OUTSIDE the func() bracket. A return address inside the
				// loaded game DLL's range is definitively the module, never d3d11on12/YAMP.
				const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
				if (IsGameDllAddr(ra))
				{
					if (++g_moduleBufCopyCount <= 8)
						DebugLogFile("[bufcopy] #%llu %llu bytes dst=%p+%llu src=%p+%llu%s\n",
							(unsigned long long)g_moduleBufCopyCount, (unsigned long long)bytes,
							static_cast<void*>(dst), (unsigned long long)dstOff,
							static_cast<void*>(src), (unsigned long long)srcOff,
							ModuleRenderActive() ? " (in frame)" : " (loader)");
					ShadowRecordBufferCopy(dst, dstOff, src, srcOff, bytes);
				}
				g_origCopyBufferRegion(self, dst, dstOff, src, srcOff, bytes);
			}
			// CopyTextureRegion (slot 16) — DIAGNOSTIC for now, but on the same fault line as the
			// CopyBufferRegion hook above. That one exists because the module records uploads from
			// loader/worker threads OUTSIDE the frame bracket, onto lists the host never executes,
			// so they are replayed onto a shadow list. Texture uploads take this path instead, and
			// nothing replays them. For pre3 (Model 3) that would leave the polygons untextured —
			// which is what "renders, but very dark" looks like. Counting first: if these fire
			// outside the frame, they are being lost the same way buffer copies were.
			typedef void (STDMETHODCALLTYPE* CopyTextureRegion_t)(ID3D12GraphicsCommandList*,
				const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
				const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
			static CopyTextureRegion_t g_origCopyTextureRegion = nullptr;
			uint64_t g_moduleTexCopyCount = 0;
			uint64_t g_moduleTexCopyOutsideFrame = 0;
			static void STDMETHODCALLTYPE HookedCopyTextureRegion(ID3D12GraphicsCommandList* self,
				const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
				const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box)
			{
				if (IsGameDllAddr(reinterpret_cast<uintptr_t>(_ReturnAddress())))
				{
					const bool inFrame = ModuleRenderActive();
					if (!inFrame) g_moduleTexCopyOutsideFrame++;
					++g_moduleTexCopyCount;
					// LOAD-BEARING, not a diagnostic. pre3 records its Model 3 texture uploads onto a
					// command list that carries NO draws (measured: of 91 distinct copy destinations,
					// the 2D tilemap is on the draw list and all 90 game textures are on one draw-free
					// list). SubmitModuleFrameList only submits lists the DRAW hooks flagged, so that
					// list was never closed or executed and every Model 3 texture stayed empty - the
					// symptom being perfect black silhouettes with correct pose, depth and animation.
					// Flagging here puts it in the frame batch, which runs uploads before the draws
					// that consume them. Measured no-op for the other games: MR is byte-identical and
					// StF stays inside its run-to-run spread (3 samples per arm, -frames 2000).
					MarkModuleRenderList(self);
				}
				g_origCopyTextureRegion(self, dst, x, y, z, src, box);
			}

			// ResolveSubresource hook (slot 19). The MSAA->non-MS resolve DST = the Model 2 3D scene
			// layer (1024x768), which the host composites (see RenderWindow::BlitDX12Texture). StF
			// leaves it SHADER-READABLE (0xC0) at frame end (fight-capture final-state ground truth).
			typedef void (STDMETHODCALLTYPE* ResolveSubresource_t)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT);
			static ResolveSubresource_t g_origResolveSubresource = nullptr;
			static ID3D12Resource* g_lastModuleResolveDst = nullptr;
			static void STDMETHODCALLTYPE HookedResolveSubresource(ID3D12GraphicsCommandList* self,
				ID3D12Resource* dst, UINT dstSub, ID3D12Resource* src, UINT srcSub, DXGI_FORMAT fmt)
			{
				// No MarkModuleRenderList here: the resolve rides StF's DRAW list (already flagged by the
				// draw hooks); flagging based on the resolve alone risks touching a foreign list.
				if (IsGameDllAddr(reinterpret_cast<uintptr_t>(_ReturnAddress())))
					g_lastModuleResolveDst = dst; // fallback display source (see BlitDX12Texture)
				g_origResolveSubresource(self, dst, dstSub, src, srcSub, fmt);
			}


			// Patch the ID3D12GraphicsCommandList vtable. NOTE: that vtable is SHARED by every list in
			// the process, so each hook below also fires on d3d11on12's internal blit lists — only slots
			// the host genuinely needs are patched, and each hook is written to be inert for foreign
			// lists (the PSO injection gates on IsModulePso, the barrier rewrite on module-created
			// resources). Do not add diagnostic-only hooks here.
			//      10 Reset            clear the list's module flag (its recording is gone).
			//   12/13 Draw*            flag the list the module records into (the submit set).
			//      14 Dispatch         same job as the Draw hooks, for compute. A module whose frame
			//                          is dispatched rather than drawn would otherwise record into a
			//                          list nothing ever flags, so the submit set would miss it and
			//                          the work would never reach the GPU. Not diagnostic-only: the
			//                          counter it also keeps is a by-product, not the reason.
			//      15 CopyBufferRegion replay the module's buffer uploads onto the shadow copy list.
			//      19 ResolveSubresource  capture the 3D-layer resolve dst the host composites.
			//      25 SetPipelineState inject the module's descriptor heaps + root signature.
			//      26 ResourceBarrier  correct StateBefore + catalog the module's render targets.
			// Slot 9 (Close) is only READ, never replaced: the frame submit calls the original directly
			// (the module never closes its own lists, so there is nothing to intercept).
			static void HookCmdListVtable(ID3D12GraphicsCommandList* list)
			{
				if (g_origSetPSO || !list) return; // once
				void** vtbl = *reinterpret_cast<void***>(list);
				DWORD op = 0;
				g_origClose = reinterpret_cast<Close_t>(vtbl[9]);

				struct Patch { int slot; void** orig; void* hook; };
				const Patch patches[] = {
					{ 10, reinterpret_cast<void**>(&g_origReset),              reinterpret_cast<void*>(&HookedReset) },
					{ 12, reinterpret_cast<void**>(&g_origDrawInstanced),      reinterpret_cast<void*>(&HookedDrawInstanced) },
					{ 13, reinterpret_cast<void**>(&g_origDrawIndexed),        reinterpret_cast<void*>(&HookedDrawIndexed) },
					{ 14, reinterpret_cast<void**>(&g_origDispatch),           reinterpret_cast<void*>(&HookedDispatch) },
					{ 15, reinterpret_cast<void**>(&g_origCopyBufferRegion),   reinterpret_cast<void*>(&HookedCopyBufferRegion) },
					// LOAD-BEARING: flags pre3's draw-free texture-upload list for submission.
					{ 16, reinterpret_cast<void**>(&g_origCopyTextureRegion),  reinterpret_cast<void*>(&HookedCopyTextureRegion) },
					{ 19, reinterpret_cast<void**>(&g_origResolveSubresource), reinterpret_cast<void*>(&HookedResolveSubresource) },
					{ 26, reinterpret_cast<void**>(&g_origResourceBarrier),    reinterpret_cast<void*>(&HookedResourceBarrier) },
					// SetPipelineState last: g_origSetPSO doubles as the "already hooked" sentinel above.
					{ 25, reinterpret_cast<void**>(&g_origSetPSO),             reinterpret_cast<void*>(&HookedSetPipelineState) },
				};
				for (const Patch& p : patches)
				{
					if (!VirtualProtect(&vtbl[p.slot], sizeof(void*), PAGE_READWRITE, &op)) continue;
					*p.orig = vtbl[p.slot];
					vtbl[p.slot] = p.hook;
					VirtualProtect(&vtbl[p.slot], sizeof(void*), op, &op);
				}
			}

			bool BuildCmdCtx(uint8_t* cc, alloc_wrapper& w, bool leaveOpen)
			{
				if (!s_device) return false;
				FillCmdCtxStubVtbl();

				ID3D12CommandAllocator*    alloc  = nullptr;
				ID3D12CommandAllocator*    alloc2 = nullptr;
				ID3D12GraphicsCommandList* list   = nullptr;
				if (FAILED(s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
				s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc2));
				if (FAILED(s_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))) return false;
				HookCmdListVtable(list); // diagnostic: log descriptor-heap binds
				if (!leaveOpen) list->Close(); // so the engine's reset() -> list->Reset(alloc, null) succeeds

				w.f00 = nullptr; w.alloc = alloc; w.f10 = nullptr; w.alloc2 = alloc2;

				auto P64 = [&](size_t off, uint64_t v) { *reinterpret_cast<uint64_t*>(cc + off) = v; };
				auto P32 = [&](size_t off, uint32_t v) { *reinterpret_cast<uint32_t*>(cc + off) = v; };
				P64(0x00, reinterpret_cast<uint64_t>(s_cmdCtxStubVtbl));   // pxd vtable (stub)
				P64(0x08, reinterpret_cast<uint64_t>(list));              // ID3D12GraphicsCommandList
				P64(0x10, reinterpret_cast<uint64_t>(list));              // (dup ref)
				P64(0x20, reinterpret_cast<uint64_t>(&w));                // -> alloc wrapper
				P64(0x28, reinterpret_cast<uint64_t>(cc + 0x38));         // self ref (embedded list head)
				P64(0x30, 8);
				P64(0x80, 0);                                            // AVL resource-state map: empty root
				P64(0x88, reinterpret_cast<uint64_t>(cc + 0x98));         // inline barrier-record array base
				P32(0x90, 0x10);                                         // record capacity (16)
				P32(0x94, 0);                                            // record count
				P64(0x258, 0xFFFFFFFFFFFFFFFFull);
				P64(0x268, reinterpret_cast<uint64_t>(s_cmdCtxStubVtbl)); // 2nd vtable (stub)
				return true;
			}

			void SeedCommandContextPool()
			{
				if (!s_device) return;

				uint32_t built = 0;
				for (uint32_t i = 0; i < kCmdCtxCount; ++i)
				{
					if (!BuildCmdCtx(s_cmdctx[i], s_cmdAllocWrap[i], /*leaveOpen*/ false)) break;
					s_cmdNodes[i].cmdctx = reinterpret_cast<uint64_t>(s_cmdctx[i]);
					s_cmdNodes[i].f10    = 0;
					s_cmdNodes[i].f18    = 0;
					built = i + 1;
				}

				// Link the built nodes into the cdevice+0x28 lock-free stack (packed head + ABA;
				// PackHead(x,0) == x for user-space pointers, matching the +0x38 freelist seed).
				for (uint32_t i = 0; i < built; ++i)
					s_cmdNodes[i].next = (i + 1 < built) ? reinterpret_cast<uint64_t>(&s_cmdNodes[i + 1]) : 0ull;
				CdevU64(0x28) = built ? PackHead(&s_cmdNodes[0], 0) : 0ull;

				DebugLog("[%s cdevice] seeded %u command contexts at +0x28\n", gGeneral.GetGameTag(), built);
			}
		} // namespace

		void* BuildRenderCommandContext()
		{
			// The cgs_device_context's +0xC8 "command recording" context is host-provided (the LJ
			// engine binds it during device-start; the StF DLL only reads it). FUN_180097520 records
			// draws into its +0x10 command list. Build one with the SAME layout as the +0x28 pool
			// contexts, but leave the list OPEN (recording) since nothing acquires/reset()s it here.
			if (!BuildCmdCtx(s_renderCmdCtx, s_renderCmdAllocWrap, /*leaveOpen*/ true))
			{
				DebugLog("[%s cdevice] BuildRenderCommandContext failed\n", gGeneral.GetGameTag());
				return nullptr;
			}
			return s_renderCmdCtx;
		}

		void* BuildHostCdevice(ID3D12Device* device, ID3D12CommandQueue* queue, void* (*cdeviceCtor)(void*))
		{
			s_device = device;

			// Hook resource creation FIRST (before StF/D3D12MA allocate anything) so the barrier-state
			// tracker learns every texture's true InitialState — fixes the id=527 desync that leaves StF's
			// sampled textures reading black. See HookDeviceResourceCreation.
			HookDeviceResourceCreation(device);

			g_yampQueue = queue;

			memset(s_cdevice, 0, sizeof(s_cdevice));

			// Run the DLL's OWN cdevice constructor (imported as CDEVICE_CTOR). It
			// initializes every field exactly as the pxd engine expects, INCLUDING the
			// real cd3d12_mem_allocator resource factory vtable embedded at +0x17b0 — so
			// resource creation is performed by the DLL's own (correct) allocator instead
			// of our stub. It only writes fields + a vtable pointer + memset, so it is
			// safe to call standalone.
			cdeviceCtor(s_cdevice);

			// Fields the ctor leaves for the host device-start to fill:
			CdevU64(0x08) = reinterpret_cast<uint64_t>(device);       // ID3D12Device
			// +0x18 : the device object cache (FUN_1800a1480), used for BOTH sampler-state (from the
			// m2ftg sampler-create path FUN_18009e620) AND graphics PSOs (from the PSO build path
			// FUN_18009dcd0 -> FUN_1800a17f0 -> FUN_1800a1480). Layout: +0x000..+0x400 = 64 sampler
			// buckets ({rwspinlock, hashmap-root}, 0x10 each); +0x400.. = the PSO cache (FUN_1800a2b90
			// reads *(base+0x400) as the cache root, +0x838 a spinlock, +0x840 a hashmap). YAMP used to
			// allocate only the 0x400 sampler part -> the PSO path read *(base+0x400) OOB -> garbage
			// pointer -> crash. Allocate a full zeroed object so every cache root is empty (miss ->
			// build fresh) and every lock is unlocked.
			static uint8_t s_samplerCache[0x1000] = {};
			CdevU64(0x18) = reinterpret_cast<uint64_t>(&s_samplerCache[0]);
			CdevU64(0x68) = reinterpret_cast<uint64_t>(&s_allocObj);  // alloc/free allocator
			CdevU64(0x30) = reinterpret_cast<uint64_t>(s_uploadPool); // upload pool (self-populates)
			CdevU64(0x58) = reinterpret_cast<uint64_t>(s_devState);   // ring state
			CdevU32(0x40) = 0x20; CdevU32(0x44) = 0x20;
			CdevU32(0x48) = 0x20; CdevU32(0x4C) = 0x08;               // node-size/align metadata

			// +0x8c : deferred-destruction mode flag (read as PTR_DAT_1801943a8[0x8c] in FUN_18009cd30).
			// When 0 the DLL DEFERS a resource-destroy into the cdevice+0x90 node pool and expects the
			// host device-start to drain that in-use queue on GPU-fence completion (returning nodes to
			// the free-list). YAMP has no such drain, so the 64-node free-list exhausts within a few
			// frames and FUN_18008dff0 pops a null head -> AV. Force immediate release (non-zero) so no
			// node is ever enqueued. Safe here: the render path flushes/waits per frame; a real fence-
			// based queue drain can replace this later. (LJ ground truth: this byte is 0.)
			*reinterpret_cast<uint8_t*>(s_cdevice + 0x8c) = 1;

			// +0x17b8 : the pxd cd3d12_mem_allocator (embedded at +0x17b0) expects a
			// D3D12MA::Allocator here (its m_Pimpl at +0x10 is what the DLL's own D3D12MA
			// methods operate on). The host normally creates it; the StF DLL only ships
			// D3D12MA's *runtime* methods (CreateAllocator was stripped), so we build a
			// layout-matched one with vendored D3D12MA v2.0.1 (the version the DLL embeds).
			{
				wil::com_ptr<IDXGIFactory4> factory;
				wil::com_ptr<IDXGIAdapter> adapter;
				if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
				{
					const LUID luid = device->GetAdapterLuid();
					factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put()));
				}

				D3D12MA::ALLOCATOR_DESC ad = {};
				// Multithreaded (real locks), matching how Lost Judgment creates the
				// allocator. Our vendored D3D12MA's RWMutex is std::mutex-based so the
				// AllocatorPimpl layout matches the DLL's byte-for-byte (see D3D12MemAlloc.cpp).
				ad.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
				ad.pDevice = device;
				ad.PreferredBlockSize = 0;
				ad.pAllocationCallbacks = nullptr;
				ad.pAdapter = adapter.get();

				D3D12MA::Allocator* allocator = nullptr;
				const HRESULT hr = D3D12MA::CreateAllocator(&ad, &allocator);
				if (SUCCEEDED(hr) && allocator)
				{
					CdevU64(0x17b8) = reinterpret_cast<uint64_t>(allocator);
				}
				else
				{
					DebugLog("[%s cdevice] D3D12MA::CreateAllocator failed hr=0x%08X\n", gGeneral.GetGameTag(), hr);
				}
			}

			// +0x38 : seed the 64-node intermediate-buffer freelist (ctor leaves it empty).
			SeedFreelist();

			// +0x28 : seed the command-context pool (GPU-copy contexts for static/IMMUTABLE uploads).
			SeedCommandContextPool();

			// +0x16e0 : seed the AVL-map node pool (0x20 nodes for the cmdctx resource-state maps).
			SeedMapNodePool();

			// +0x90 : seed the deferred-destruction record pool (0x40 x 0x10 nodes @ +0xC8).
			SeedDeferredNodePool();

			return s_cdevice;
		}

		void* GetHostCdevice()
		{
			return s_device ? s_cdevice : nullptr;
		}
}

// Global accessor (matches the GetModuleOutputResource pattern) so RenderWindow's composite can readback
// each RT StF renders to and pick the one holding the frame. g_rtSeen lives in pxd's anon namespace.
ID3D12Resource* GetModuleRenderTarget(int index)
{
	if (index < 0 || index >= pxd::g_rtSeenCount) return nullptr;
	return pxd::g_rtSeen[index];
}
int GetModuleRenderTargetCount()
{
	return pxd::g_rtSeenCount;
}

// Last D3D12 state StF left render target #index in (for 11on12 to wrap it with the correct InState).
int GetModuleRenderTargetState(int index)
{
	if (index < 0 || index >= pxd::g_rtSeenCount) return 0;
	return static_cast<int>(pxd::g_rtLastState[index]);
}

// The Model 2 3D-scene layer: dst of StF's per-frame MSAA ResolveSubresource (captured in
// HookedResolveSubresource). StF leaves it SHADER-READABLE (0xC0) at frame end; the host must
// composite it UNDER the 496x384 2D output (LJ host does the same — fight-capture ground truth).
ID3D12Resource* GetModuleResolveDst()
{
	return pxd::g_lastModuleResolveDst;
}

// Called by GameLoop right after the DLL records a frame (func()): close + ExecuteCommandLists + flush
// + reopen every list the module drew into — the per-frame submit it records for but never issues
// (the host's job). See the PIX RenderFrame export: LJ submits ~10 lists in ONE call.
void SubmitModuleFrameListNow() { pxd::SubmitModuleFrameList(); }
// The other half of the host's per-frame job: advance the upload-frame stamp and recycle StF's upload
// buffers (in-use -> available). Called after the flush so all recycled buffers are GPU-complete. Fixes
// the upload-pool exhaustion crash (FUN_18009be60, ~frame 570).
void AdvanceFrameStampNow() { pxd::AdvanceFrameStamp(); }
// True once a submit hung/removed the device (so GameLoop can stop before StF's next-frame upload-alloc
// crashes on the dead device — lets DRED's dump survive as the last thing in the log).
bool ModuleExecDisabledNow() { return pxd::ModuleExecDisabled(); }

// GameLoop brackets the DLL's per-frame render (func()) with this so the ResourceBarrier hook only
// corrects StF's own barriers, not d3d11on12's blit barriers recorded outside func().
void SetModuleRenderActiveNow(bool active) { pxd::SetModuleRenderActive(active); }

// RenderWindow registers the swapchain backbuffers so the ResourceBarrier hook dumps their transitions.
void RegisterWatchResourceNow(ID3D12Resource* r) { pxd::RegisterWatchResource(r); }


