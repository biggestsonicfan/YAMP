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
bool IsStfPso(void* pso);
// display_tex+0x98 (StF's output RT), published each frame by BlitDX12Texture. The ResourceBarrier hook
// watches for it to learn whether StF ever renders into it (transitions it to RENDER_TARGET).
ID3D12Resource* GetStfOutputResource();
// Defined in RenderWindow.cpp: dump DRED (last GPU op + page-fault resource) on device-removal, and
// append a line to d3d12_debug.log. Used by the frame-submit path to record a GPU hang once.
void DumpDredNow();
// render-target bind can be identified rather than guessed at from its dimensions.

namespace m2ftg
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

			ID3D12Device*       s_device = nullptr;
			ID3D12CommandQueue* s_queue = nullptr;

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
					DebugLog(
						"[StF cdevice] CreateResource fmt=0x%X tag=0x%X desc=[%016llX %016llX %016llX %016llX %016llX %016llX]\n",
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

			// cmdctx+0x00/+0x268 pxd vtable: the upload path never calls it, but stub it with
			// return-0 no-ops so an unexpected virtual call cannot fault on a null vtable pointer.
			uint64_t CmdCtxStub(void*) { return 0; }
			void* s_cmdCtxStubVtbl[24] = {};

			// DIAGNOSTIC: PIX proved StF's scene draws are recorded but the render command list is NEVER
			// executed (LJ executes it; YAMP stubs the cmdctx vtable). Log per-slot call counts so we can
			// find which vtable slot is the frame-end SUBMIT (the one called ~once per frame) — that's the
			// method that must Close() + ExecuteCommandLists() the recorded list.
			uint64_t g_cmdctxSlotCount[24] = {};
			uint64_t g_cmdctxTotal = 0;
			template<int N>
			uint64_t STDMETHODCALLTYPE CmdCtxSlotLog(void*, void*, void*, void*)
			{
				g_cmdctxSlotCount[N]++; g_cmdctxTotal++;
				if (g_cmdctxSlotCount[N] <= 2 || g_cmdctxTotal % 400 == 0)
				{
					char b[300]; int o = _snprintf_s(b, sizeof(b), _TRUNCATE, "[cmdctx] slot %d (total=%llu):", N, (unsigned long long)g_cmdctxTotal);
					for (int i = 0; i < 24; ++i)
						if (g_cmdctxSlotCount[i]) o += _snprintf_s(b + o, sizeof(b) - o, _TRUNCATE, " s%d=%llu", i, (unsigned long long)g_cmdctxSlotCount[i]);
					DebugLog("%s\n", b);
				}
				return 0;
			}
			template<int... Ns>
			void FillCmdCtxStubVtbl(std::integer_sequence<int, Ns...>)
			{
				((s_cmdCtxStubVtbl[Ns] = reinterpret_cast<void*>(&CmdCtxSlotLog<Ns>)), ...);
			}

			// Build one pxd command context (0x280 layout, ground truth in scratchpad/cmdctx-spec.md)
			// into cc/w: creates a DIRECT command list + two allocators and fills the header fields the
			// engine reads (+0x10 cmdlist, +0x28 self+0x38 slot array, +0x80 empty AVL state map,
			// +0x88 self+0x98 barrier records, +0x94 count, +0x258 = -1). leaveOpen keeps the list in
			// the recording state (device-context path) vs Close()d for the acquire+reset() pool path.
			// ---- DIAGNOSTIC: log SetDescriptorHeaps / SetGraphicsRootDescriptorTable -------------------
			// The DLL binds a shader-visible descriptor heap and a root table into the render command
			// list; the debug layer reports the bound GPU handle "does not refer to a location in a
			// descriptor heap". Hook the command list's vtable (slot 28 = SetDescriptorHeaps, slot 32 =
			// SetGraphicsRootDescriptorTable) to log the bound heap(s) + each handle, so we can see which
			// shader-visible heap the DLL actually uses vs the gs+0x7550 rings YAMP wired.
			typedef void (STDMETHODCALLTYPE* SetDescHeaps_t)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
			typedef void (STDMETHODCALLTYPE* SetGfxRootTable_t)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
			static SetDescHeaps_t    g_origSetDescHeaps    = nullptr;
			static SetGfxRootTable_t g_origSetGfxRootTable = nullptr;
			static FILE*             g_cmdLog              = nullptr;
			// cmdlist.log DISABLED: it logged every SetDescriptorHeaps/SetGraphicsRootDescriptorTable/PSO
			// call (per-draw), ballooning past 1 GB in a long run. The descriptor-heap-binding question it
			// answered is settled; leave g_cmdLog null so every `if (g_cmdLog)` write is skipped (the PSO
			// hook's StF heap/root-sig INJECTION is outside that guard and still runs). Restore the fopen_s
			// if a future descriptor bug needs it.
			static void CmdLogOpen() { /* disabled — was ballooning cmdlist.log to gigabytes */ }

			// Forward decls so the draw hooks (below) can flag the list they draw into as StF's. Flagging
			// by draw (during func()) is far more reliable than by PSO: StF's scene lists use PSOs from a
			// creation path IsStfPso() doesn't track, so PSO-flagging missed them and they never executed.
			void MarkStfRenderList(ID3D12GraphicsCommandList*);
			bool StfRenderActive();

			// CAPTURE: the last command list StF drew into (during func()) + lifecycle counters. Logged each
			// frame so we get the EXACT list pointer (to bp in x64dbg) and confirm StF never Close/Reset/
			// Execute-s it (the missing submit). g_lastStfCmdList is deliberately a plain global so its
			// address is easy to find (it's logged); read/watch it live via the x64dbg bridge.
			ID3D12GraphicsCommandList* g_lastStfCmdList = nullptr;
			uint64_t g_stfListCloses = 0, g_stfListResets = 0, g_stfListExecs = 0;

			// TEMP [seq]: capture the EXACT per-frame call order on StF's draw list (root sig / heaps /
			// root tables / PSO / draw) to see whether YAMP's PSO-hook SetGraphicsRootSignature injection
			// wipes root descriptor tables StF set earlier (SetGraphicsRootSignature resets all root
			// params). Shared cap shows ~the first frame's sequence, then stops. Remove once resolved.
			static int g_seqLog = 0;
#ifdef _DEBUG
			static void SeqLog(const char* s) { if (StfRenderActive() && g_seqLog < 140) { g_seqLog++; DebugLogFile("%s", s); } }
#else
#define SeqLog(s) ((void)0)
#endif

			// The CB buffer + offsets StF bound CBVs at — re-read at SUBMIT time (all writes done) to tell a
			// genuinely zero/NaN matrix from a read-before-write artifact of the CreateCBV-time [cbv] dump.
			static ID3D12Resource* g_cbDumpRes = nullptr;
			static uint64_t g_cbDumpOff[8] = {}; static int g_cbDumpCount = 0;

			// DIAGNOSTIC: count draw calls. StF's output RT is confirmed all-black over 300+ frames; if
			// the DLL issues zero draws it isn't reaching render code (assets/game-state stuck); if it
			// draws a lot, the geometry just isn't landing on the output (state / wrong RT). Slots 12/13.
			typedef void (STDMETHODCALLTYPE* DrawInstanced_t)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
			typedef void (STDMETHODCALLTYPE* DrawIndexed_t)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
			static DrawInstanced_t g_origDrawInstanced = nullptr;
			static DrawIndexed_t   g_origDrawIndexed   = nullptr;
			static uint64_t g_drawInstCount = 0, g_drawIdxCount = 0, g_drawVertsTotal = 0;
			// TEMP [vp-tally]: bucket draws by the last-set viewport, to see whether the (dead)
			// 3D scene pass (vp 1024x768 -> MSAA RT -> resolve) still records draws at all.
			static UINT g_lastVpW = 0, g_lastVpH = 0;
			static uint64_t g_vpDraws768 = 0, g_vpDraws1024 = 0, g_vpDraws384 = 0, g_vpDrawsOther = 0;
			static void TallyDrawViewport()
			{
				if (g_lastVpH == 768) ++g_vpDraws768;
				else if (g_lastVpH == 1024) ++g_vpDraws1024;
				else if (g_lastVpH == 384) ++g_vpDraws384;
				else ++g_vpDrawsOther;
			}

			static void STDMETHODCALLTYPE HookedDrawInstanced(ID3D12GraphicsCommandList* self,
				UINT vpi, UINT inst, UINT svl, UINT sil)
			{
				g_drawInstCount++; g_drawVertsTotal += static_cast<uint64_t>(vpi) * (inst ? inst : 1);
				TallyDrawViewport();
				if (StfRenderActive()) { g_lastStfCmdList = self; MarkStfRenderList(self); } // capture StF's draw list
				if ((g_drawInstCount + g_drawIdxCount) % 500 == 1)
				{ DebugLogFile("[draw] instanced=%llu indexed=%llu vertsTotal=%llu\n",
					(unsigned long long)g_drawInstCount,(unsigned long long)g_drawIdxCount,(unsigned long long)g_drawVertsTotal); }
				g_origDrawInstanced(self, vpi, inst, svl, sil);
			}
			// Closed-list tracking (defined with the Close/Reset hooks further down): GBV id=547 says
			// commands recorded onto a closed list are silently dropped.
			static bool IsListClosed(ID3D12GraphicsCommandList* l);
			extern uint64_t g_drawsOnClosedList;
			extern uint64_t g_drawsOnOpenList;

			static void STDMETHODCALLTYPE HookedDrawIndexed(ID3D12GraphicsCommandList* self,
				UINT ipi, UINT inst, UINT sil, INT bvl, UINT sivl)
			{
				g_drawIdxCount++; g_drawVertsTotal += static_cast<uint64_t>(ipi) * (inst ? inst : 1);
				TallyDrawViewport();
				// GBV id=547 test: is StF drawing into a list we have already Closed? Such draws are
				// silently dropped by the runtime, which would explain "counted but never rendered".
				if (IsListClosed(self))
				{
					g_drawsOnClosedList++;
					static int s_cl = 0;
					if (s_cl < 8)
					{
						DebugLogFile("[closed-draw] DrawIndexed on CLOSED list %p (dropped by runtime)\n",
							static_cast<void*>(self));
						s_cl++;
					}
				}
				else g_drawsOnOpenList++;
				if (StfRenderActive() && g_seqLog < 140) { char b[112]; _snprintf_s(b, _TRUNCATE, "[seq] DrawIndexed idxPerInst=%u inst=%u list=%p\n", ipi, inst, static_cast<void*>(self)); SeqLog(b); }
				if (StfRenderActive()) { g_lastStfCmdList = self; MarkStfRenderList(self); } // capture StF's draw list
				if ((g_drawInstCount + g_drawIdxCount) % 500 == 1)
				{ DebugLogFile("[draw] instanced=%llu indexed=%llu vertsTotal=%llu\n",
					(unsigned long long)g_drawInstCount,(unsigned long long)g_drawIdxCount,(unsigned long long)g_drawVertsTotal); }
				g_origDrawIndexed(self, ipi, inst, sil, bvl, sivl);
			}

			static void STDMETHODCALLTYPE HookedSetDescriptorHeaps(
				ID3D12GraphicsCommandList* self, UINT n, ID3D12DescriptorHeap* const* heaps)
			{
				if (StfRenderActive() && g_seqLog < 140)
				{
					unsigned long long g0 = 0; D3D12_DESCRIPTOR_HEAP_DESC d0{};
					if (n && heaps && heaps[0]) { d0 = heaps[0]->GetDesc(); if (d0.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) g0 = heaps[0]->GetGPUDescriptorHandleForHeapStart().ptr; }
					char b[160]; _snprintf_s(b, _TRUNCATE, "[seq] SetHeaps n=%u heap0=%p type=%d gpuBase=0x%llX list=%p\n", n, n && heaps ? static_cast<void*>(heaps[0]) : nullptr, d0.Type, g0, static_cast<void*>(self));
					SeqLog(b);
				}
				CmdLogOpen();
				if (g_cmdLog)
				{
					for (UINT i = 0; i < n && heaps; ++i)
					{
						ID3D12DescriptorHeap* h = heaps[i];
						D3D12_DESCRIPTOR_HEAP_DESC d = h ? h->GetDesc() : D3D12_DESCRIPTOR_HEAP_DESC{};
						unsigned long long gpu = 0, cpu = 0;
						if (h && (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
						{ gpu = h->GetGPUDescriptorHandleForHeapStart().ptr; cpu = h->GetCPUDescriptorHandleForHeapStart().ptr; }
						fprintf(g_cmdLog, "list=%p SetDescriptorHeaps[%u/%u] heap=%p type=%d num=%u sv=%d gpuBase=0x%llX cpuBase=0x%llX\n",
							(void*)self, i, n, (void*)h, d.Type, d.NumDescriptors,
							(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? 1 : 0, gpu, cpu);
					}
					fflush(g_cmdLog);
				}
				g_origSetDescHeaps(self, n, heaps);
			}
			static void STDMETHODCALLTYPE HookedSetGraphicsRootDescriptorTable(
				ID3D12GraphicsCommandList* self, UINT idx, D3D12_GPU_DESCRIPTOR_HANDLE h)
			{
				if (StfRenderActive() && g_seqLog < 140) { char b[128]; _snprintf_s(b, _TRUNCATE, "[seq] SetRootTable root=%u gpu=0x%llX list=%p\n", idx, static_cast<unsigned long long>(h.ptr), static_cast<void*>(self)); SeqLog(b); }
				CmdLogOpen();
				if (g_cmdLog)
				{
					void* ret = _ReturnAddress();
					uint64_t dllBase = reinterpret_cast<uint64_t>(GetModuleHandleW(L"stf-pxd-w64-d3d12_retail.dll"));
					fprintf(g_cmdLog, "  list=%p SetGraphicsRootDescriptorTable root=%u gpu=0x%llX  <- ret=0x%llX (dll+0x%llX)\n",
						(void*)self, idx, (unsigned long long)h.ptr, (unsigned long long)ret,
						dllBase ? (unsigned long long)((uint64_t)ret - dllBase) : 0ull);
					fflush(g_cmdLog);
				}
				g_origSetGfxRootTable(self, idx, h);
			}
			typedef void (STDMETHODCALLTYPE* SetGfxRootSig_t)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
			typedef void (STDMETHODCALLTYPE* SetPSO_t)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
			static SetGfxRootSig_t g_origSetGfxRootSig = nullptr;
			static SetPSO_t        g_origSetPSO        = nullptr;
			void MarkStfRenderList(ID3D12GraphicsCommandList*); // defined below (Close-hook section)
			static void STDMETHODCALLTYPE HookedSetGraphicsRootSignature(ID3D12GraphicsCommandList* self, ID3D12RootSignature* rs)
			{
				if (StfRenderActive() && g_seqLog < 140) { char b[96]; _snprintf_s(b, _TRUNCATE, "[seq] SetRootSig rs=%p list=%p\n", static_cast<void*>(rs), static_cast<void*>(self)); SeqLog(b); }
				CmdLogOpen();
				if (g_cmdLog) { fprintf(g_cmdLog, "SetGraphicsRootSignature rs=%p\n", (void*)rs); fflush(g_cmdLog); }
				g_origSetGfxRootSig(self, rs);
			}
			static void STDMETHODCALLTYPE HookedSetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso)
			{
				if (StfRenderActive() && g_seqLog < 140) { char b[112]; _snprintf_s(b, _TRUNCATE, "[seq] SetPSO pso=%p isStf=%d list=%p\n", static_cast<void*>(pso), IsStfPso(pso) ? 1 : 0, static_cast<void*>(self)); SeqLog(b); }
				CmdLogOpen();
				if (g_cmdLog) { fprintf(g_cmdLog, "SetPipelineState pso=%p\n", (void*)pso); fflush(g_cmdLog); }
				g_origSetPSO(self, pso);
				// The DLL binds the shader-visible heaps + never sets a root signature on a DIFFERENT
				// command list than the draw list. So on THIS (draw) list, right after the PSO and
				// before it binds the root descriptor table, re-bind the DLL's own heaps and the PSO's
				// root sig. Fixes id=708 ("no root signature") + "no CBV_SRV_UAV heap set".
				// ONLY for StF PSOs: this vtable is shared with d3d11on12's blit list, and injecting
				// StF's state there makes d3d11on12's own root-table calls run against the wrong root
				// signature -> driver AV. d3d11on12's blit PSO is not tracked, so it is skipped.
				if (!IsStfPso(pso)) return;
				MarkStfRenderList(self); // this list records StF's scene -> execute it when StF closes it
				ID3D12DescriptorHeap* heaps[2] = { GetDllRingCbvSrvHeap(), GetDllRingSamplerHeap() };
				if (heaps[0] && heaps[1])
					self->SetDescriptorHeaps(2, heaps);
				if (ID3D12RootSignature* rs = GetCapturedRootSignature())
					self->SetGraphicsRootSignature(rs);
				if (StfRenderActive() && g_seqLog < 140) { char b[128]; _snprintf_s(b, _TRUNCATE, "[seq]   -> YAMP INJECT heaps(%d)+rootsig(%p) [resets root params!] list=%p\n", (heaps[0] && heaps[1]) ? 2 : 0, static_cast<void*>(GetCapturedRootSignature()), static_cast<void*>(self)); SeqLog(b); }
			}
			// ResourceBarrier hook (slot 26): watch whether StF ever transitions its output RT
			// (display_tex+0x98) — if it does, StF renders into it (state issue); if never, StF renders
			// elsewhere and display_tex is a copy dest that's not being written.
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
			// Gated to g_inStfRender so it only touches StF's own barriers (recorded inside func()), never
			// d3d11on12's blit barriers (recorded later, in BlitDX12Texture) which are already correct.
			static std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> g_resState;
			// Guards g_resState: written by the resource-creation hooks (may run on an asset-loader thread)
			// and read/written by the ResourceBarrier hook (render thread). Cheap relative to the per-frame
			// GPU flush.
			static std::mutex g_resStateMutex;
			static bool g_inStfRender = false;
			void SetStfRenderActive(bool active) { g_inStfRender = active; }
			bool StfRenderActive() { return g_inStfRender; }

			// Diagnostic watch-list: specific resources (the swapchain backbuffers) whose EVERY transition
			// we dump, so we can see the exact 11on12 Acquire/Release sequence that leaves them COMMON at
			// draw (id=527/538). Registered from RenderWindow::CreateWrappedBackbuffers.
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
				// func() (g_inStfRender) and to StF-created resources barriered outside it. Barriers we don't
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

						// TEMP DESYNC DETECTOR (id=527 diagnosis): keep a GLOBAL mirror of every resource's
						// state (updated on EVERY barrier we see, across ALL lists, in record order) and log
						// only the barriers whose declared StateBefore disagrees with the mirror — i.e. the
						// actual desyncs the runtime rejects as id=527. This isolates the offending list
						// (e.g. F0430AE0) + shows the state the barrier SHOULD have declared, validating the
						// global-tracking fix. Capped. Remove once id=527 is fixed.
						{
							static std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> mirror;
							auto it = mirror.find(r);
							const D3D12_RESOURCE_STATES known = (it != mirror.end())
								? it->second : bars[i].Transition.StateBefore; // first sight: trust the barrier
							if (known != bars[i].Transition.StateBefore)
							{
								static int t = 0;
								if (t < 400)
								{
									const D3D12_RESOURCE_DESC de = r->GetDesc();
									DebugLogFile(
										"[bar-desync2] res=%p %llux%u before=0x%X MIRROR=0x%X after=0x%X inStf=%d list=%p\n",
										static_cast<void*>(r), static_cast<unsigned long long>(de.Width), de.Height,
										static_cast<unsigned>(bars[i].Transition.StateBefore), static_cast<unsigned>(known),
										static_cast<unsigned>(bars[i].Transition.StateAfter),
										g_inStfRender ? 1 : 0, static_cast<void*>(self));
								}
								t++;
							}
							mirror[r] = bars[i].Transition.StateAfter;
						}

						// Dump every transition on a watched backbuffer (who/before/after).
						if (IsWatched(r))
						{
							static int w = 0;
							if (w < 48)
							{
								DebugLogFile("[watch-bar] res=%p %s before=0x%X after=0x%X\n",
									static_cast<void*>(r), g_inStfRender ? "StF" : "11on12/blit",
									static_cast<unsigned>(bars[i].Transition.StateBefore),
									static_cast<unsigned>(bars[i].Transition.StateAfter));
							}
							w++;
						}

						// Correct StateBefore to our tracked current state, then advance the tracking. Only
						// simple (non-split) whole-resource transitions are rewritten; leave split barriers
						// (BEGIN/END flags) untouched so we never desync a half-transition. Apply during
						// func() (StF's own render) OR to any resource StF created (now known via the
						// creation-seed hook) — the id=527 texture desyncs happen on StF's draw lists that
						// aren't always bracketed by g_inStfRender. NEVER touch the watched swapchain
						// backbuffers: those are driven by 11on12 + TransitionBackbufferToRenderTarget.
						// CANARY: g_resState is seeded by our device-level creation hook for EVERY texture
						// created on the device — including ones d3d11on12 makes for itself (ImGui font
						// atlas, staging textures). For those, `knownStfRes` is true and we rewrite
						// 11on12's OWN barriers using state tracking that never saw its internal
						// transitions, which would corrupt its rendering — matching the observed
						// "host overlay never reaches the screen" symptom (VF2 has no such hooks).
						// Set false to pass every barrier through untouched.
						constexpr bool kEnableBarrierRewrite = true; // canary test done: disabling it did NOT restore the overlay
						std::unique_lock<std::mutex> resLk(g_resStateMutex);
						const bool knownStfRes = (g_resState.find(r) != g_resState.end());
						if (kEnableBarrierRewrite
							&& bars[i].Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE && !IsWatched(r)
							&& (g_inStfRender || knownStfRes))
						{
							auto it = g_resState.find(r);
							D3D12_RESOURCE_STATES cur = (it != g_resState.end())
								? it->second : D3D12_RESOURCE_STATE_COMMON; // fallback if never seen created
							// Diagnostic: StF's own StateBefore disagreeing with our tracked state means this
							// resource desynced (e.g. 11on12 transitioned it outside func()). These are exactly
							// the resources that still trip id=527/538. Name them (+ desc) to the log file.
							if (cur != bars[i].Transition.StateBefore)
							{
								static int d = 0;
								if (d < 64)
								{
									const D3D12_RESOURCE_DESC de = r->GetDesc();
									DebugLogFile(
										"[barrier-desync] res=%p W=%llu H=%u fmt=%d  StF says before=0x%X, tracked=0x%X, after=0x%X\n",
										static_cast<void*>(r), static_cast<unsigned long long>(de.Width), de.Height, de.Format,
										static_cast<unsigned>(bars[i].Transition.StateBefore), static_cast<unsigned>(cur),
										static_cast<unsigned>(bars[i].Transition.StateAfter));
								}
								d++;
							}
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

			// ClearRenderTargetView hook (slot 48): log the color StF clears its RTs to. If a RT is
			// cleared to non-black but reads back black, even clears aren't landing (state/sync). If it
			// reads the clear color, only the draws fail (pipeline/root-sig). Decisive either way.
			typedef void (STDMETHODCALLTYPE* ClearRTV_t)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*);
			static ClearRTV_t g_origClearRTV = nullptr;
			// DIAGNOSTIC TEST (black-RT): force StF's render-target clears to bright RED. Decisive:
			// if red (or non-black readback) shows, the RT is being written AND displayed -> StF's draws
			// produce no color (shader/texture/constant issue). If still pure black, the RT isn't written
			// or isn't reaching the screen (execution/blit/wrong-target). Only override StF's own clears
			// (g_inStfRender); leave 11on12/ImGui clears alone. Set kForceRedClear=false to disable.
			static constexpr bool kForceRedClear = false;
			static void STDMETHODCALLTYPE HookedClearRTV(ID3D12GraphicsCommandList* self,
				D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c[4], UINT nr, const D3D12_RECT* rects)
			{
				static int s_n = 0;
				if (s_n < 24 && c) { ++s_n;
					DebugLogFile("[clear] RTV=0x%llX color=(%.3f, %.3f, %.3f, %.3f) inStf=%d rects=%u list=%p\n",
						static_cast<unsigned long long>(h.ptr), c[0], c[1], c[2], c[3], StfRenderActive() ? 1 : 0, nr, static_cast<void*>(self));
				}
				if (kForceRedClear && StfRenderActive())
				{
					const FLOAT red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
					g_origClearRTV(self, h, red, nr, rects);
					return;
				}
				g_origClearRTV(self, h, c, nr, rects);
			}

			// ExecuteCommandLists hook (queue slot 10): does StF submit on YAMP's queue (=> 11on12 reads
			// are ordered) or its own (=> the read races and sees black even though StF rendered)?
			typedef void (STDMETHODCALLTYPE* ExecCmdLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
			static ExecCmdLists_t g_origExec = nullptr;
			static ID3D12CommandQueue* g_yampQueue = nullptr;
			static void STDMETHODCALLTYPE HookedExec(ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists)
			{
				// Does StF actually submit? Log the CALLER: return address inside the game DLL
				// means its own sbgl backend is doing ExecuteCommandLists; else it's 11on12/our blit.
				static int s_n = 0;
				if (s_n < 24) { ++s_n;
					const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
					const bool inStf = IsGameDllAddr(ra);
					DebugLogFile("[exec] n=%u SAME=%d caller=%s (%s+0x%llX)\n",
						n, self == g_yampQueue ? 1 : 0, inStf ? "StF" : "other",
						inStf ? "StF" : "?", inStf ? static_cast<unsigned long long>(ra - g_gameDllBase) : static_cast<unsigned long long>(ra));
				}
				if (lists) for (UINT i = 0; i < n; ++i) if (lists[i] == g_lastStfCmdList) g_stfListExecs++;
				g_origExec(self, n, lists);
			}

			// Close(slot 9) / Reset(slot 10) hooks: does StF close its render command list (=> executable)
			// or leave it open (=> YAMP must close it)? And what allocator does it Reset with?
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
			static ID3D12GraphicsCommandList* g_stfFlagged[64] = {}; // lists flagged StF since their last Reset
			static int g_stfFlaggedCount = 0;
			static ID3D12GraphicsCommandList* g_frameLists[64] = {}; // StF lists closed this frame, in order
			// Per-list allocator we own, so we can Reset (reopen) StF's lists each frame for it to re-record.
			static std::unordered_map<ID3D12GraphicsCommandList*, ID3D12CommandAllocator*> g_listAlloc;
			static int g_frameListCount = 0;
			static ID3D12Fence* g_execFence = nullptr; static UINT64 g_execFv = 0; static HANDLE g_execEv = nullptr;
			void MarkStfRenderList(ID3D12GraphicsCommandList* l) // "this list is StF's" (from the PSO hook)
			{
				if (!l) return;
				for (int i = 0; i < g_stfFlaggedCount; ++i) if (g_stfFlagged[i] == l) return;
				if (g_stfFlaggedCount < 64) g_stfFlagged[g_stfFlaggedCount++] = l;
				static int m = 0; if (m < 8) { DebugLogFile("[mark] %s list %p type=%d (flagged=%d)\n", gGeneral.GetGameTag(), static_cast<void*>(l), l->GetType(), g_stfFlaggedCount); } m++;
			}
			static bool IsStfFlagged(ID3D12GraphicsCommandList* l)
			{
				for (int i = 0; i < g_stfFlaggedCount; ++i) if (g_stfFlagged[i] == l) return true;
				return false;
			}
			static void UnflagStfList(ID3D12GraphicsCommandList* l) // on Reset the recording is gone; re-earn it
			{
				for (int i = 0; i < g_stfFlaggedCount; ++i) if (g_stfFlagged[i] == l) { g_stfFlagged[i] = g_stfFlagged[--g_stfFlaggedCount]; return; }
			}

			// Frame-end submit (called from GameLoop after func()): ONE ExecuteCommandLists + ONE flush.
			// One-shot self-disable: if a submit removes the device (bad descriptor/resource -> GPU hang ->
			// TDR), detect it, dump DRED, and stop executing so we can never re-freeze the desktop frame
			// after frame — the failure is captured once, not repeated.
			static bool g_execDisabled = false;
			bool StfExecDisabled() { return g_execDisabled; }

				// PATH B (multi-list): StF records its frame across SEVERAL command lists and NEVER
				// Close/Execute/Reset-s any of them (host's job; dynamically confirmed closes=resets=0). Dynamic
				// capture proved StF draws into 2+ lists per scene frame (flaggedLists=2); submitting only the
				// LAST (g_lastStfCmdList) left the earlier list's draws unexecuted -> black screen. So Close EVERY
				// list StF drew into this frame (flagged in draw order by the draw/PSO hooks), ExecuteCommandLists
				// them ALL in ONE call (D3D12 carries resource state across them, like LJ's RenderFrame submitting
				// ~10 lists at once), flush once, then Reset each (its own allocator) so StF re-records next frame.
				ID3D12GraphicsCommandList* ShadowCopyCloseForSubmit(); // defined with the copy hooks below
				void ShadowCopyReopen();
				void SubmitStfFrameList()
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
					for (int i = 0; i < g_stfFlaggedCount && nn < 64; ++i)
					{
						ID3D12GraphicsCommandList* l = g_stfFlagged[i];
						if (!l || l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) continue;
						const HRESULT hrc = g_origClose ? g_origClose(l) : l->Close(); // StF leaves them OPEN
						if (FAILED(hrc))
						{
							static int e = 0; if (e++ < 8) { DebugLogFile("[pathb] Close failed 0x%08X list=%p (skip)\n", static_cast<unsigned>(hrc), static_cast<void*>(l)); }
							continue;
						}
						gl[nn] = l; lists[nn] = l; ++nn;
					}
					if (nn == 0) { g_stfFlaggedCount = 0; return; }

					// VERIFY constant buffers at SUBMIT time (all StF writes for the frame are done, right
					// before the GPU reads them) — distinguishes a genuinely zero/NaN matrix from a
					// read-before-write artifact of the CreateCBV-time [cbv] dump.
					{
						static int cbf = 0;
						if (cbf < 20 && g_cbDumpRes && g_cbDumpCount) // log the CPU addr a while so there's time to set the x64dbg bp
						{
							void* p = nullptr; D3D12_RANGE rr{ 0, 0 };
							if (SUCCEEDED(g_cbDumpRes->Map(0, &rr, &p)) && p)
							{
								for (int i = 0; i < g_cbDumpCount; ++i)
								{
									float f[16]; std::memcpy(f, static_cast<uint8_t*>(p) + g_cbDumpOff[i], sizeof(f));
									DebugLogFile("[cbv-submit] off=%llu [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n",
										g_cbDumpOff[i], f[0],f[1],f[2],f[3], f[4],f[5],f[6],f[7], f[8],f[9],f[10],f[11], f[12],f[13],f[14],f[15]);
								}
								// x64dbg NaN-TRACE: log the CPU write-addresses of each dumped CB slot via
								// OutputDebugString (visible in x64dbg's log). The mapped pointer is stable
								// across frames (StF's CB ring is persistently mapped), so set a HARDWARE WRITE
								// breakpoint on a NaN/zero slot's address, continue, and either catch StF's
								// matrix-compute writing it (stack -> the fn + its bad input) or see it NEVER
								// fires (=> the CB is left uninitialized; StF creates a CBV for a CB it doesn't
								// fill). Compare vs the valid off=0 (2D ortho) slot's writer.
								char ab[320]; int ao = _snprintf_s(ab, _TRUNCATE, "STF-CB-CPU base=%p", p);
								for (int i = 0; i < g_cbDumpCount && ao > 0; ++i)
									ao += _snprintf_s(ab + ao, sizeof(ab) - ao, _TRUNCATE, " | off%llu=%p", g_cbDumpOff[i], static_cast<void*>(static_cast<uint8_t*>(p) + g_cbDumpOff[i]));
								_snprintf_s(ab + ao, sizeof(ab) - ao, _TRUNCATE, "  <-- hw write-bp a NaN/zero slot in x64dbg\n");
								DebugLog("%s", ab);
								g_cbDumpRes->Unmap(0, nullptr);
							}
							cbf++;
						}
					}
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
					g_stfFlaggedCount = 0; // consumed this frame's lists; the draw hooks re-flag next frame
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
			// (SubmitStfFrameList), so at each frame boundary ALL in-use buffers are GPU-complete and can
			// be recycled unconditionally — no fence tracking needed.
			// The stamp advance + recycle (the upload-exhaustion fix) — VERIFIED not to affect StF's CB data
			// (constant buffers were identical zero/NaN with this on vs off; StF's CB ring is separate from
			// the upload pool). Kept ON as the correct host behavior that prevents the ~frame-570 crash.
			static constexpr bool kAdvanceStamp = true;
			void AdvanceFrameStamp()
			{
				if (!kAdvanceStamp) return;
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

			// Upload-copy counters (defined with the CopyBufferRegion/CopyResource hooks below).
			extern unsigned int g_copyBufCount;
			extern unsigned int g_copyResCount;
			extern unsigned int g_resolveCount;

			// DIAGNOSTIC: measure StF's per-frame recording without submitting (no GPU work -> no TDR freeze),
			// to test whether draw volume is constant (normal multi-pass) or GROWING (StF's lists accumulate
			// because our per-frame Reset is failing / frame never completes). Set false to actually render.
			static constexpr bool kExecuteStfLists = false;
			void ExecuteStfRenderLists()
			{
				{
					static uint64_t s_li = 0, s_ln = 0; static int s_fr = 0;
					const uint64_t di = g_drawIdxCount - s_li, dn = g_drawInstCount - s_ln;
					s_li = g_drawIdxCount; s_ln = g_drawInstCount;
					// Log boot in detail, then every 20th frame up to 2000 — to see if the in-scene draw rate
					// is FLAT (normal) or CLIMBING (StF re-recording / accumulating).
					if ((s_fr < 40 || s_fr % 20 == 0) && s_fr < 2000) { DebugLogFile(
						"[frame %d] draws this frame: indexed=%llu instanced=%llu, flaggedLists=%d\n",
						s_fr, (unsigned long long)di, (unsigned long long)dn, g_stfFlaggedCount); }
					// The exact StF draw list + whether StF EVER closes/resets/executes it (the missing submit).
					if (s_fr < 40 || s_fr % 20 == 0) { DebugLogFile(
						"[stf-list] list=%p closes=%llu resets=%llu execs=%llu  (&g_lastStfCmdList=%p)\n",
						static_cast<void*>(g_lastStfCmdList), (unsigned long long)g_stfListCloses,
						(unsigned long long)g_stfListResets, (unsigned long long)g_stfListExecs,
						static_cast<void*>(&g_lastStfCmdList)); }
					// Upload-copy tally: are StF's DEFAULT-heap vertex/index buffers ever filled?
					if (s_fr < 40 || s_fr % 20 == 0) { DebugLogFile(
						"[copy-tally] copyBufferRegion=%u copyResource=%u resolves=%u\n",
						g_copyBufCount, g_copyResCount, g_resolveCount); }
					// [vp-tally] per-frame deltas: does the 3D pass (vp height 768) record draws?
					{
						static uint64_t p768 = 0, p1024 = 0, p384 = 0, pOther = 0;
						const uint64_t d768 = g_vpDraws768 - p768, d1024 = g_vpDraws1024 - p1024,
							d384 = g_vpDraws384 - p384, dOther = g_vpDrawsOther - pOther;
						p768 = g_vpDraws768; p1024 = g_vpDraws1024; p384 = g_vpDraws384; pOther = g_vpDrawsOther;
						if (s_fr < 40 || s_fr % 20 == 0) { DebugLogFile(
							"[vp-tally] draws vp768=%llu vp1024=%llu vp384=%llu other=%llu\n",
							(unsigned long long)d768, (unsigned long long)d1024,
							(unsigned long long)d384, (unsigned long long)dOther); }
					}
					// GBV id=547 tally: draws landing on a CLOSED list are silently dropped by the runtime.
					if (s_fr < 40 || s_fr % 20 == 0) { DebugLogFile(
						"[closed-tally] drawsOnClosed=%llu drawsOnOpen=%llu\n",
						(unsigned long long)g_drawsOnClosedList, (unsigned long long)g_drawsOnOpenList); }
					s_fr++;
				}
				if (!kExecuteStfLists) { g_stfFlaggedCount = 0; g_frameListCount = 0; return; } // measure only
				if (g_execDisabled) { g_stfFlaggedCount = 0; g_frameListCount = 0; return; }
				if (!g_yampQueue || !s_device || g_stfFlaggedCount == 0) { g_stfFlaggedCount = 0; g_frameListCount = 0; return; }
				// Execute EVERY list StF drew into this frame (flagged by the draw hooks during func()), in
				// flag order (= draw-encounter order ~= record order). StF hands lists to the host to submit
				// and may leave them OPEN, so close any still recording (bypassing our Close hook) to make
				// them submittable. ONE ExecuteCommandLists + ONE flush = LJ's RenderFrame_000.
				// Snapshot the flagged lists (the reset loop below calls g_origReset, which must not perturb
				// the array; and we need them again after the flush).
				ID3D12CommandList* lists[64]; ID3D12GraphicsCommandList* gl[64]; int n = 0;
				for (int i = 0; i < g_stfFlaggedCount && n < 64; ++i)
				{
					ID3D12GraphicsCommandList* l = g_stfFlagged[i];
					if (!l || l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) continue;
					if (g_origClose) g_origClose(l); // StF never closes its lists; the host must before submit
					gl[n] = l; lists[n] = l; ++n;
				}
				if (n)
				{
					g_yampQueue->ExecuteCommandLists(static_cast<UINT>(n), lists);
					if (!g_execFence) { s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_execFence)); g_execEv = CreateEventA(nullptr, FALSE, FALSE, nullptr); }
					// Wait 4s (> the ~2s GPU TDR window) so a hang on StF's draws surfaces as a clean
					// device-removal we can name via DRED — instead of the 1s timeout letting the next frame
					// resubmit/reset a still-in-flight list (id=553/544/547 cascade -> crash).
					bool timedOut = false;
					if (g_execFence) { g_yampQueue->Signal(g_execFence, ++g_execFv); if (g_execFence->GetCompletedValue() < g_execFv) { g_execFence->SetEventOnCompletion(g_execFv, g_execEv); timedOut = (WaitForSingleObject(g_execEv, 4000) == WAIT_TIMEOUT); } }
					static int f = 0; if (f < 8) { DebugLogFile("[exec-fix] frame executed %d %s list(s) in ONE submit%s\n", n, gGeneral.GetGameTag(), timedOut ? " (FLUSH TIMED OUT - GPU HANG)" : ""); } f++;
					if (timedOut) { DebugLogFile("[exec-fix] GPU did not complete in 4s -> hang on %s draws; disabling + dumping DRED\n", gGeneral.GetGameTag()); DumpDredNow(); g_execDisabled = true; }
					const HRESULT rr = s_device->GetDeviceRemovedReason();
					if (FAILED(rr))
					{
						DebugLogFile("[exec-fix] DEVICE REMOVED 0x%08X after submit — disabling %s execution\n", static_cast<unsigned>(rr), gGeneral.GetGameTag());
						DumpDredNow();
						g_execDisabled = true;
					}

					// REOPEN each list for next frame. StF never Resets/Closes its lists — in LJ the host does
					// (PopulateCommandList opens with Reset, closes before Execute). We just flushed, so the GPU
					// is done and the per-list allocator is safe to Reset. Without this, StF's next-frame
					// recording hits a CLOSED list (id=547 flood) and re-submitting the same in-flight list
					// fails the fence check (id=553). Reset via g_origReset to bypass our Reset hook.
					if (!g_execDisabled && g_origReset)
					{
						for (int i = 0; i < n; ++i)
						{
							ID3D12CommandAllocator*& a = g_listAlloc[gl[i]];
							if (!a) s_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
							if (a) { a->Reset(); g_origReset(gl[i], a, nullptr); }
						}
					}
				}
				g_stfFlaggedCount = 0; // clear this frame's flags; next frame re-flags via the draw hooks
				g_frameListCount = 0;
			}

			// GBV id=547 ("This API cannot be called on a closed command list") — commands recorded onto
			// a CLOSED list are silently DROPPED, which would explain draws that we count but never see.
			// Track open/closed per list and count the draws that land while closed. Our submit does
			// Close -> Execute -> flush -> Reset while StF has ~22 worker threads, so any recording in
			// that window evaporates.
			static std::unordered_map<ID3D12GraphicsCommandList*, bool> g_listClosed;
			static std::mutex g_listClosedMutex;
			uint64_t g_drawsOnClosedList = 0, g_drawsOnOpenList = 0;
			static bool IsListClosed(ID3D12GraphicsCommandList* l)
			{
				std::lock_guard<std::mutex> lk(g_listClosedMutex);
				auto it = g_listClosed.find(l);
				return it != g_listClosed.end() && it->second;
			}
			static void MarkListClosed(ID3D12GraphicsCommandList* l, bool closed)
			{
				std::lock_guard<std::mutex> lk(g_listClosedMutex);
				g_listClosed[l] = closed;
			}
			static HRESULT STDMETHODCALLTYPE HookedClose(ID3D12GraphicsCommandList* self)
			{
				if (self == g_lastStfCmdList) g_stfListCloses++; // does StF close its own draw list? (expect: no)
				const HRESULT hr = g_origClose(self);
				if (SUCCEEDED(hr)) MarkListClosed(self, true);
				// Diagnostic only: note when StF closes a list it drew into. Execution is driven by the
				// frame-end flagged-list sweep (ExecuteStfRenderLists), not by close, so nothing is queued here.
				if (SUCCEEDED(hr) && IsStfFlagged(self))
				{
					static int c = 0; if (c < 16) { DebugLogFile("[close] game-flagged list %p closed by %s\n", static_cast<void*>(self), gGeneral.GetGameTag()); } c++;
				}
				return hr;
			}
			static HRESULT STDMETHODCALLTYPE HookedReset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
			{
				if (self == g_lastStfCmdList) g_stfListResets++; // does StF reset its own draw list? (expect: no)
				MarkListClosed(self, false); // Reset reopens it for recording
				UnflagStfList(self); // recording cleared; the list must re-earn its StF flag via a new StF PSO
				static int c = 0; if (c < 8 || c % 600 == 0) { DebugLog("[cmdlist] Reset self=%p alloc=%p #%d\n", static_cast<void*>(self), static_cast<void*>(a), c); } c++;
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

			// TEST: force CullMode=NONE on every graphics PSO. If StF's scene appears, back-face culling
			// (winding/projection-handedness mismatch) was discarding all triangles -> solid clear. Also
			// logs the original rasterizer/depth state so we see what StF used. kForceCullNone toggles it.
			static constexpr bool kForceCullNone = false;
			static constexpr bool kForceDepthOff = false;
			typedef HRESULT(STDMETHODCALLTYPE* CreateGfxPSO_t)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
			static CreateGfxPSO_t g_origCreateGfxPSO = nullptr;
			static HRESULT STDMETHODCALLTYPE HookedCreateGfxPSO(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv)
			{
				if (desc)
				{
					static int s = 0;
					if (s < 24) { s++; DebugLogFile(
						"[pso] cull=%d frontCCW=%d fill=%d depthEnable=%d depthFunc=%d depthWrite=%d numRT=%u rt0fmt=%d dsvfmt=%d\n",
						desc->RasterizerState.CullMode, desc->RasterizerState.FrontCounterClockwise, desc->RasterizerState.FillMode,
						desc->DepthStencilState.DepthEnable, desc->DepthStencilState.DepthFunc, desc->DepthStencilState.DepthWriteMask,
						desc->NumRenderTargets, desc->RTVFormats[0], desc->DSVFormat); }
				}
				// TEST: StF's PSOs enable depth test (LESS) but set DSVFormat=UNKNOWN and bind no DSV, so the
				// depth test reads 0 and rejects every fragment -> black. Force DepthEnable=FALSE to confirm
				// that's the black-screen cause (proper fix = provide a real depth buffer, like the host).
				if ((kForceCullNone || kForceDepthOff) && desc)
				{
					D3D12_GRAPHICS_PIPELINE_STATE_DESC copy = *desc;
					if (kForceCullNone) copy.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
					if (kForceDepthOff) { copy.DepthStencilState.DepthEnable = FALSE; copy.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; }
					return g_origCreateGfxPSO(self, &copy, riid, ppv);
				}
				return g_origCreateGfxPSO(self, desc, riid, ppv);
			}
			// TEMP: track BUFFER resources' GPU-VA ranges so a CBV's BufferLocation can be mapped back to
			// its resource + offset, to dump the constant-buffer contents (the transform matrix) StF binds.
			struct BufRange { uint64_t va0, va1; ID3D12Resource* res; };
			static BufRange g_bufRanges[4096];
			static int      g_bufRangeCount = 0;
			static void SeedCreatedResourceState(const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st, REFIID riid, void** ppv)
			{
				if (!ppv || !*ppv || !desc) return;
				if (riid != __uuidof(ID3D12Resource)) return;
				if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
				{
					ID3D12Resource* r = static_cast<ID3D12Resource*>(*ppv);
					const uint64_t va = r->GetGPUVirtualAddress();
					if (va)
					{
						std::lock_guard<std::mutex> lk(g_resStateMutex);
						if (g_bufRangeCount < 4096) g_bufRanges[g_bufRangeCount++] = { va, va + desc->Width, r };
					}
					return; // runtime forces buffers to COMMON — don't seed barrier state for them
				}
				std::lock_guard<std::mutex> lk(g_resStateMutex);
				g_resState[static_cast<ID3D12Resource*>(*ppv)] = st; // TRUE initial state for the barrier corrector
			}
			// CreateConstantBufferView (device slot 17): dump the first 16 floats (a 4x4 matrix) of each CB
			// StF binds during render — if all zero, the host normally fills this and YAMP doesn't.
			typedef void (STDMETHODCALLTYPE* CreateCBV_t)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
			static CreateCBV_t g_origCreateCBV = nullptr;
			static int g_cbvLog = 0;
			static void STDMETHODCALLTYPE HookedCreateCBV(ID3D12Device* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)
			{
				if (StfRenderActive() && d && g_cbvLog < 12)
				{
					ID3D12Resource* res = nullptr; uint64_t off = 0;
					{
						std::lock_guard<std::mutex> lk(g_resStateMutex);
						for (int i = 0; i < g_bufRangeCount; ++i)
							if (d->BufferLocation >= g_bufRanges[i].va0 && d->BufferLocation < g_bufRanges[i].va1)
							{ res = g_bufRanges[i].res; off = d->BufferLocation - g_bufRanges[i].va0; break; }
					}
					float f[16] = {}; bool got = false;
					if (res) { void* p = nullptr; D3D12_RANGE rr{ 0, 0 };
						if (SUCCEEDED(res->Map(0, &rr, &p)) && p) { std::memcpy(f, static_cast<uint8_t*>(p) + off, sizeof(f)); res->Unmap(0, nullptr); got = true; } }
					if (res) { g_cbDumpRes = res; if (g_cbDumpCount < 8) g_cbDumpOff[g_cbDumpCount++] = off; } // for the submit-time re-read
					DebugLogFile("[cbv] va=0x%llX size=%u res=%p off=%llu got=%d\n     [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n",
						static_cast<unsigned long long>(d->BufferLocation), d->SizeInBytes, static_cast<void*>(res), off, got ? 1 : 0,
						f[0],f[1],f[2],f[3], f[4],f[5],f[6],f[7], f[8],f[9],f[10],f[11], f[12],f[13],f[14],f[15]);
					g_cbvLog++;
				}
				g_origCreateCBV(self, d, h);
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
				if (VirtualProtect(&vtbl[10], sizeof(void*), PAGE_READWRITE, &op)) // CreateGraphicsPipelineState
				{
					g_origCreateGfxPSO = reinterpret_cast<CreateGfxPSO_t>(vtbl[10]);
					vtbl[10] = reinterpret_cast<void*>(&HookedCreateGfxPSO);
					VirtualProtect(&vtbl[10], sizeof(void*), op, &op);
				}
				if (VirtualProtect(&vtbl[17], sizeof(void*), PAGE_READWRITE, &op)) // CreateConstantBufferView
				{
					g_origCreateCBV = reinterpret_cast<CreateCBV_t>(vtbl[17]);
					vtbl[17] = reinterpret_cast<void*>(&HookedCreateCBV);
					VirtualProtect(&vtbl[17], sizeof(void*), op, &op);
				}
			}

			// DIAGNOSTIC (black-RT, draw-time): StF's draws execute on a submitted list but produce no
			// pixels. The two classic causes are a degenerate viewport/scissor (nothing rasterized) or an
			// unbound/wrong render target (draws go nowhere). Log the viewport StF sets and the RTV it binds
			// during its render (g_inStfRender), capped.
			typedef void (STDMETHODCALLTYPE* RSSetViewports_t)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
			typedef void (STDMETHODCALLTYPE* OMSetRenderTargets_t)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
			// Scissor (slot 22) — never inspected before. An empty/degenerate scissor discards every
			// pixel with NO validation error and no symptom other than a black RT: exactly what we see.
			typedef void (STDMETHODCALLTYPE* RSSetScissorRects_t)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
			static RSSetScissorRects_t  g_origRSSetScissorRects  = nullptr;
			static RSSetViewports_t     g_origRSSetViewports     = nullptr;
			static OMSetRenderTargets_t g_origOMSetRenderTargets = nullptr;
			static void STDMETHODCALLTYPE HookedRSSetScissorRects(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RECT* rects)
			{
				if (StfRenderActive() && rects && n)
				{
					static int s = 0;
					if (s < 20)
					{
						DebugLogFile("[scissor] n=%u [0] l=%ld t=%ld r=%ld b=%ld (w=%ld h=%ld) list=%p\n",
							n, rects[0].left, rects[0].top, rects[0].right, rects[0].bottom,
							rects[0].right - rects[0].left, rects[0].bottom - rects[0].top,
							static_cast<void*>(self));
						s++;
					}
					// TEST: force a wide-open scissor. If pixels appear, StF's scissor was the killer.
					constexpr bool kForceFullScissor = false; // tested 2026-07-25: StF's scissors are already
					// correct (1024x768 / 1024x1024 / 496x384, matching each RT); forcing wide-open changed nothing
					if (kForceFullScissor)
					{
						D3D12_RECT wide{ 0, 0, 16384, 16384 };
						g_origRSSetScissorRects(self, 1, &wide);
						return;
					}
				}
				g_origRSSetScissorRects(self, n, rects);
			}
			// IA-stage diagnostics: topology (slot 20), index buffer (43), vertex buffers (44). A garbage
			// VB/IB GPU VA or an odd topology makes draws execute but rasterize nothing.
			typedef void (STDMETHODCALLTYPE* IASetTopology_t)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
			typedef void (STDMETHODCALLTYPE* IASetIndexBuffer_t)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
			typedef void (STDMETHODCALLTYPE* IASetVertexBuffers_t)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
			static IASetTopology_t      g_origIASetTopology      = nullptr;
			static IASetIndexBuffer_t   g_origIASetIndexBuffer   = nullptr;
			static IASetVertexBuffers_t g_origIASetVertexBuffers = nullptr;
			static void STDMETHODCALLTYPE HookedIASetTopology(ID3D12GraphicsCommandList* self, D3D12_PRIMITIVE_TOPOLOGY t)
			{
				if (StfRenderActive() && g_seqLog < 140) { char b[80]; _snprintf_s(b, _TRUNCATE, "[seq] IATopology=%d list=%p\n", static_cast<int>(t), static_cast<void*>(self)); SeqLog(b); }
				g_origIASetTopology(self, t);
			}
			static void STDMETHODCALLTYPE HookedIASetIndexBuffer(ID3D12GraphicsCommandList* self, const D3D12_INDEX_BUFFER_VIEW* v)
			{
				if (StfRenderActive() && g_seqLog < 140) { char b[128]; _snprintf_s(b, _TRUNCATE, "[seq] IASetIB va=0x%llX size=%u fmt=%d list=%p\n", v ? static_cast<unsigned long long>(v->BufferLocation) : 0, v ? v->SizeInBytes : 0, v ? v->Format : 0, static_cast<void*>(self)); SeqLog(b); }
				g_origIASetIndexBuffer(self, v);
			}
			static void STDMETHODCALLTYPE HookedIASetVertexBuffers(ID3D12GraphicsCommandList* self, UINT start, UINT num, const D3D12_VERTEX_BUFFER_VIEW* v)
			{
				if (StfRenderActive() && g_seqLog < 140) { char b[144]; _snprintf_s(b, _TRUNCATE, "[seq] IASetVB slot=%u num=%u va=0x%llX size=%u stride=%u list=%p\n", start, num, (v && num) ? static_cast<unsigned long long>(v[0].BufferLocation) : 0, (v && num) ? v[0].SizeInBytes : 0, (v && num) ? v[0].StrideInBytes : 0, static_cast<void*>(self)); SeqLog(b); }
				// TEST (black-screen): dump the actual CONTENT of the bound VB. If the buffer maps
				// (upload heap) and the bytes are all zero, the geometry is degenerate and no matrix
				// can save it — the missing piece would be the VB upload/copy, not transforms.
				{
					static int s_vbDump = 0;
					if (StfRenderActive() && s_vbDump < 10 && v != nullptr && num != 0 && v[0].BufferLocation != 0)
					{
						ID3D12Resource* res = nullptr; uint64_t off = 0;
						{
							std::lock_guard<std::mutex> lk(g_resStateMutex);
							for (int i = 0; i < g_bufRangeCount; ++i)
								if (v[0].BufferLocation >= g_bufRanges[i].va0 && v[0].BufferLocation < g_bufRanges[i].va1)
								{ res = g_bufRanges[i].res; off = v[0].BufferLocation - g_bufRanges[i].va0; break; }
						}
						float f[8] = {}; int got = 0; unsigned int nz = 0;
						if (res != nullptr)
						{
							void* p = nullptr; D3D12_RANGE rr{ 0, 0 };
							if (SUCCEEDED(res->Map(0, &rr, &p)) && p != nullptr)
							{
								const uint8_t* base = static_cast<uint8_t*>(p) + off;
								std::memcpy(f, base, sizeof(f));
								for (unsigned int k = 0; k < 256 && off + k < v[0].SizeInBytes; ++k) if (base[k] != 0) ++nz;
								res->Unmap(0, nullptr); got = 1;
							}
							else got = -1; // not CPU-mappable (default heap)
						}
						DebugLogFile("[vbdump] va=0x%llX res=%p off=%llu map=%d nz256=%u [%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f]\n",
							static_cast<unsigned long long>(v[0].BufferLocation), static_cast<void*>(res), off, got, nz,
							f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
						s_vbDump++;
					}
				}
				g_origIASetVertexBuffers(self, start, num, v);
			}
			// TEST (black-screen): StF's 3D vertex buffers live in DEFAULT heaps (Map fails), so their
			// contents can only arrive through a GPU copy from an upload/intermediate buffer. Count
			// and log those copies during StF's render: if none target the bound VB, the geometry is
			// never uploaded (degenerate -> black) and the missing host piece is the intermediate-
			// buffer flush, not the transform CBs.
			typedef void (STDMETHODCALLTYPE* CopyBufferRegion_t)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
			typedef void (STDMETHODCALLTYPE* CopyResource_t)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
			static CopyBufferRegion_t g_origCopyBufferRegion = nullptr;
			static CopyResource_t     g_origCopyResource     = nullptr;
			static int g_copyLog = 0;
			unsigned int g_copyBufCount = 0, g_copyResCount = 0;
			// *** IB-FILL FIX v2 — SHADOW COPY LIST (2026-07-26; replaces the v1 "flag copy lists"
			// approach). StF records its ONE-TIME buffer uploads (e.g. the emulator's 24576-index
			// tile-grid quad IB) on loader lists that never draw and are recorded from WORKER THREADS
			// at arbitrary times. v1 flagged those lists so SubmitStfFrameList would close/execute/
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
			// Called by SubmitStfFrameList: close the shadow list and hand it over for the frame batch
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
			static void STDMETHODCALLTYPE HookedCopyBufferRegion(ID3D12GraphicsCommandList* self, ID3D12Resource* dst, UINT64 dstOff, ID3D12Resource* src, UINT64 srcOff, UINT64 bytes)
			{
				// GATE ON THE CALLER, not StfRenderActive(): the module records copies from
				// loader/worker threads OUTSIDE the func() bracket. A return address inside the
				// loaded game DLL's range is definitively the module, never d3d11on12/YAMP.
				const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
				const bool fromStf = IsGameDllAddr(ra);
				if (fromStf)
					ShadowRecordBufferCopy(dst, dstOff, src, srcOff, bytes);
				if (fromStf || StfRenderActive())
				{
					++g_copyBufCount;
					if (g_copyLog < 16)
					{
						// The SOURCE is StF's intermediate/upload buffer (CPU-mappable): dump its
						// content at the copy offset. All-zero here => StF never wrote the vertex
						// data (broken upload path upstream), which no transform could rescue.
						unsigned int nz = 0; int mapped = 0; float f[12] = {};
						unsigned int nanCount = 0; int firstNan = -1, nanStride = -1;
						if (src != nullptr)
						{
							void* p = nullptr; D3D12_RANGE rr{ 0, 0 };
							if (SUCCEEDED(src->Map(0, &rr, &p)) && p != nullptr)
							{
								const uint8_t* s = static_cast<uint8_t*>(p) + srcOff;
								const uint64_t n = bytes < 4096 ? bytes : 4096;
								for (uint64_t k = 0; k < n; ++k) if (s[k] != 0) ++nz;
								std::memcpy(f, s, sizeof(f));
								// NaN periodicity over the first 1024 floats: which lane repeats?
								const float* fs = reinterpret_cast<const float*>(s);
								const uint64_t fn = (bytes / 4) < 1024 ? (bytes / 4) : 1024;
								int prevNan = -1;
								for (uint64_t k = 0; k < fn; ++k)
								{
									if (std::isnan(fs[k]))
									{
										++nanCount;
										if (firstNan < 0) firstNan = static_cast<int>(k);
										else if (nanStride < 0 && prevNan >= 0) nanStride = static_cast<int>(k) - prevNan;
										prevNan = static_cast<int>(k);
									}
								}
								src->Unmap(0, nullptr); mapped = 1;
							}
							else mapped = -1;
						}
						// RAW HEX too: '-nan' is exactly how 0xFFFFFFFF prints when read as a float, i.e.
						// an opaque-white PACKED VERTEX COLOR would masquerade as a broken position .w.
						// Check the bits before trusting the float reading of lane 3.
						{
							const uint32_t* uu = reinterpret_cast<const uint32_t*>(f);
							DebugLogFile("[copy-hex] %08X %08X %08X %08X | %08X %08X %08X %08X\n",
								uu[0], uu[1], uu[2], uu[3], uu[4], uu[5], uu[6], uu[7]);
						}
						DebugLogFile("[copy] dstVA=0x%llX bytes=%llu srcMap=%d nz4k=%u nan/1024=%u first=%d stride=%d [%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f]\n",
							dst ? static_cast<unsigned long long>(dst->GetGPUVirtualAddress()) : 0ull, bytes,
							mapped, nz, nanCount, firstNan, nanStride,
							f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
						g_copyLog++;
					}
				}
				g_origCopyBufferRegion(self, dst, dstOff, src, srcOff, bytes);
			}
			// [resolve-tally] TEMP: does the MSAA->non-MS ResolveSubresource (the 3D layer's path to
			// the 1024x768 RT) ever get recorded live? Slot 19.
			typedef void (STDMETHODCALLTYPE* ResolveSubresource_t)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT);
			static ResolveSubresource_t g_origResolveSubresource = nullptr;
			unsigned int g_resolveCount = 0;
			// The MSAA->non-MS resolve DST = the Model 2 3D scene layer (1024x768). The host must
			// composite it under the 496x384 2D output (see RenderWindow::BlitDX12Texture). StF
			// leaves it SHADER-READABLE (0xC0) at frame end (fight-capture final-state ground truth).
			static ID3D12Resource* g_lastStfResolveDst = nullptr;
			static void STDMETHODCALLTYPE HookedResolveSubresource(ID3D12GraphicsCommandList* self,
				ID3D12Resource* dst, UINT dstSub, ID3D12Resource* src, UINT srcSub, DXGI_FORMAT fmt)
			{
				++g_resolveCount;
				// No MarkStfRenderList here: the resolve rides StF's DRAW list (already flagged by the
				// draw hooks); flagging based on the resolve alone risks touching a foreign list.
				const uintptr_t raRS = reinterpret_cast<uintptr_t>(_ReturnAddress());
				if (IsGameDllAddr(raRS))
					g_lastStfResolveDst = dst; // fallback display source (see BlitDX12Texture)
				static int s_rs = 0;
				if (s_rs < 12)
				{
					D3D12_RESOURCE_DESC sd{}, dd{};
					if (src) sd = src->GetDesc();
					if (dst) dd = dst->GetDesc();
					DebugLogFile("[resolve] src=%p %llux%u s=%u -> dst=%p %llux%u fmt=%d ra=0x%llX list=%p\n",
						static_cast<void*>(src), static_cast<unsigned long long>(sd.Width), sd.Height, sd.SampleDesc.Count,
						static_cast<void*>(dst), static_cast<unsigned long long>(dd.Width), dd.Height, fmt,
						static_cast<unsigned long long>(raRS), static_cast<void*>(self));
					s_rs++;
				}
				g_origResolveSubresource(self, dst, dstSub, src, srcSub, fmt);
			}
			static void STDMETHODCALLTYPE HookedCopyResource(ID3D12GraphicsCommandList* self, ID3D12Resource* dst, ID3D12Resource* src)
			{
				// CopyResource is not shadowed (never observed from StF: g_copyResCount==0 all runs);
				// log if it ever fires so we know to extend the shadow list to it.
				const uintptr_t raCR = reinterpret_cast<uintptr_t>(_ReturnAddress());
				const bool fromStfCR = IsGameDllAddr(raCR);
				if (fromStfCR || StfRenderActive())
				{
					++g_copyResCount;
					if (g_copyLog < 16)
					{
						DebugLogFile("[copy] Resource dst=%p src=%p dstVA=0x%llX list=%p\n",
							static_cast<void*>(dst), static_cast<void*>(src),
							dst ? static_cast<unsigned long long>(dst->GetGPUVirtualAddress()) : 0ull, static_cast<void*>(self));
						g_copyLog++;
					}
				}
				g_origCopyResource(self, dst, src);
			}
			static void STDMETHODCALLTYPE HookedRSSetViewports(ID3D12GraphicsCommandList* self, UINT n, const D3D12_VIEWPORT* vp)
			{
				if (vp && n) { g_lastVpW = static_cast<UINT>(vp[0].Width); g_lastVpH = static_cast<UINT>(vp[0].Height); } // [vp-tally]
				if (StfRenderActive() && vp && n)
				{
					static int s = 0;
					if (s < 24) { DebugLogFile(
						"[vp] n=%u [0] x=%.1f y=%.1f w=%.1f h=%.1f z=[%.2f,%.2f] list=%p\n",
						n, vp[0].TopLeftX, vp[0].TopLeftY, vp[0].Width, vp[0].Height, vp[0].MinDepth, vp[0].MaxDepth, static_cast<void*>(self));
						s++; }
				}
				g_origRSSetViewports(self, n, vp);
			}
			static void STDMETHODCALLTYPE HookedOMSetRenderTargets(ID3D12GraphicsCommandList* self, UINT n,
				const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
			{
				if (StfRenderActive())
				{
					static int s = 0;
					if (s < 24) { DebugLogFile(
						"[omrt] numRT=%u rtv[0]=0x%llX single=%d dsv=%s list=%p\n",
						n, static_cast<unsigned long long>(rtv && n ? rtv[0].ptr : 0), single ? 1 : 0, dsv ? "yes" : "no", static_cast<void*>(self));
						s++; }
				}
				g_origOMSetRenderTargets(self, n, rtv, single, dsv);
			}

			static void HookCmdListVtable(ID3D12GraphicsCommandList* list)
			{
				// CANARY: the D3D12 command-list vtable is SHARED by every list in the process, so
				// these StF-only hooks also intercept d3d11on12's internal lists. VF2 installs none
				// of them and its identical ImGui/11on12 overlay works; in StF the overlay never
				// reaches the screen. Set false to install NO hooks and see if the overlay returns.
				// (Disables the SetPipelineState injection, barrier correction and all diagnostics.)
				constexpr bool kInstallCmdListHooks = true; // canary: disabling ALL hooks crashes StF (the
				// SetPipelineState injection is required), so this test cannot isolate the overlay bug
				if (!kInstallCmdListHooks) return;
				if (g_origSetDescHeaps || !list) return; // once
				void** vtbl = *reinterpret_cast<void***>(list);
				DWORD op = 0;
				if (VirtualProtect(&vtbl[25], sizeof(void*) * 8, PAGE_READWRITE, &op))
				{
					g_origSetPSO          = reinterpret_cast<SetPSO_t>(vtbl[25]);
					g_origSetDescHeaps    = reinterpret_cast<SetDescHeaps_t>(vtbl[28]);
					g_origSetGfxRootSig   = reinterpret_cast<SetGfxRootSig_t>(vtbl[30]);
					g_origSetGfxRootTable = reinterpret_cast<SetGfxRootTable_t>(vtbl[32]);
					g_origResourceBarrier = reinterpret_cast<ResourceBarrier_t>(vtbl[26]);
					vtbl[25] = reinterpret_cast<void*>(&HookedSetPipelineState);
					vtbl[26] = reinterpret_cast<void*>(&HookedResourceBarrier);
					vtbl[28] = reinterpret_cast<void*>(&HookedSetDescriptorHeaps);
					vtbl[30] = reinterpret_cast<void*>(&HookedSetGraphicsRootSignature);
					vtbl[32] = reinterpret_cast<void*>(&HookedSetGraphicsRootDescriptorTable);
					VirtualProtect(&vtbl[25], sizeof(void*) * 8, op, &op);
				}
				// Draw hooks (slots 12 = DrawInstanced, 13 = DrawIndexedInstanced).
				if (VirtualProtect(&vtbl[12], sizeof(void*) * 2, PAGE_READWRITE, &op))
				{
					g_origDrawInstanced = reinterpret_cast<DrawInstanced_t>(vtbl[12]);
					g_origDrawIndexed   = reinterpret_cast<DrawIndexed_t>(vtbl[13]);
					vtbl[12] = reinterpret_cast<void*>(&HookedDrawInstanced);
					vtbl[13] = reinterpret_cast<void*>(&HookedDrawIndexed);
					VirtualProtect(&vtbl[12], sizeof(void*) * 2, op, &op);
				}
				// ClearRenderTargetView hook (slot 48).
				if (VirtualProtect(&vtbl[48], sizeof(void*), PAGE_READWRITE, &op))
				{
					g_origClearRTV = reinterpret_cast<ClearRTV_t>(vtbl[48]);
					vtbl[48] = reinterpret_cast<void*>(&HookedClearRTV);
					VirtualProtect(&vtbl[48], sizeof(void*), op, &op);
				}
				// Close(slot 9) + Reset(slot 10) hooks.
				if (VirtualProtect(&vtbl[9], sizeof(void*) * 2, PAGE_READWRITE, &op))
				{
					g_origClose = reinterpret_cast<Close_t>(vtbl[9]);
					g_origReset = reinterpret_cast<Reset_t>(vtbl[10]);
					vtbl[9]  = reinterpret_cast<void*>(&HookedClose);
					vtbl[10] = reinterpret_cast<void*>(&HookedReset);
					VirtualProtect(&vtbl[9], sizeof(void*) * 2, op, &op);
				}
				// RSSetViewports(slot 21) + OMSetRenderTargets(slot 46) diagnostics.
				if (VirtualProtect(&vtbl[22], sizeof(void*), PAGE_READWRITE, &op))
				{
					g_origRSSetScissorRects = reinterpret_cast<RSSetScissorRects_t>(vtbl[22]);
					vtbl[22] = reinterpret_cast<void*>(&HookedRSSetScissorRects);
					VirtualProtect(&vtbl[22], sizeof(void*), op, &op);
				}
				if (VirtualProtect(&vtbl[21], sizeof(void*), PAGE_READWRITE, &op))
				{
					g_origRSSetViewports = reinterpret_cast<RSSetViewports_t>(vtbl[21]);
					vtbl[21] = reinterpret_cast<void*>(&HookedRSSetViewports);
					VirtualProtect(&vtbl[21], sizeof(void*), op, &op);
				}
				if (VirtualProtect(&vtbl[46], sizeof(void*), PAGE_READWRITE, &op))
				{
					g_origOMSetRenderTargets = reinterpret_cast<OMSetRenderTargets_t>(vtbl[46]);
					vtbl[46] = reinterpret_cast<void*>(&HookedOMSetRenderTargets);
					VirtualProtect(&vtbl[46], sizeof(void*), op, &op);
				}
				// IA diagnostics: IASetPrimitiveTopology(20), IASetIndexBuffer(43), IASetVertexBuffers(44).
				if (VirtualProtect(&vtbl[20], sizeof(void*), PAGE_READWRITE, &op))
				{
					g_origIASetTopology = reinterpret_cast<IASetTopology_t>(vtbl[20]);
					vtbl[20] = reinterpret_cast<void*>(&HookedIASetTopology);
					VirtualProtect(&vtbl[20], sizeof(void*), op, &op);
				}
				if (VirtualProtect(&vtbl[43], sizeof(void*) * 2, PAGE_READWRITE, &op))
				{
					g_origIASetIndexBuffer   = reinterpret_cast<IASetIndexBuffer_t>(vtbl[43]);
					g_origIASetVertexBuffers = reinterpret_cast<IASetVertexBuffers_t>(vtbl[44]);
					vtbl[43] = reinterpret_cast<void*>(&HookedIASetIndexBuffer);
					vtbl[44] = reinterpret_cast<void*>(&HookedIASetVertexBuffers);
					VirtualProtect(&vtbl[43], sizeof(void*) * 2, op, &op);
				}
				// Upload-copy diagnostics: CopyBufferRegion(15), CopyResource(17).
				if (VirtualProtect(&vtbl[15], sizeof(void*), PAGE_READWRITE, &op))
				{
					g_origCopyBufferRegion = reinterpret_cast<CopyBufferRegion_t>(vtbl[15]);
					vtbl[15] = reinterpret_cast<void*>(&HookedCopyBufferRegion);
					VirtualProtect(&vtbl[15], sizeof(void*), op, &op);
				}
				// ResolveSubresource diagnostics + flagging (slot 19).
				if (VirtualProtect(&vtbl[19], sizeof(void*), PAGE_READWRITE, &op))
				{
					g_origResolveSubresource = reinterpret_cast<ResolveSubresource_t>(vtbl[19]);
					vtbl[19] = reinterpret_cast<void*>(&HookedResolveSubresource);
					VirtualProtect(&vtbl[19], sizeof(void*), op, &op);
				}
				if (VirtualProtect(&vtbl[17], sizeof(void*), PAGE_READWRITE, &op))
				{
					g_origCopyResource = reinterpret_cast<CopyResource_t>(vtbl[17]);
					vtbl[17] = reinterpret_cast<void*>(&HookedCopyResource);
					VirtualProtect(&vtbl[17], sizeof(void*), op, &op);
				}
			}

			bool BuildCmdCtx(uint8_t* cc, alloc_wrapper& w, bool leaveOpen)
			{
				if (!s_device) return false;
				FillCmdCtxStubVtbl(std::make_integer_sequence<int, 24>{});

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
			s_queue = queue;

			// Hook resource creation FIRST (before StF/D3D12MA allocate anything) so the barrier-state
			// tracker learns every texture's true InitialState — fixes the id=527 desync that leaves StF's
			// sampled textures reading black. See HookDeviceResourceCreation.
			HookDeviceResourceCreation(device);

			// Hook ExecuteCommandLists on the queue vtable (shared by all queues) to see which queue StF
			// submits its rendering on vs YAMP's.
			g_yampQueue = queue;
			if (queue)
			{
				void** qvtbl = *reinterpret_cast<void***>(queue);
				DWORD qop = 0;
				if (VirtualProtect(&qvtbl[10], sizeof(void*), PAGE_READWRITE, &qop))
				{
					if (!g_origExec) g_origExec = reinterpret_cast<ExecCmdLists_t>(qvtbl[10]);
					qvtbl[10] = reinterpret_cast<void*>(&HookedExec);
					VirtualProtect(&qvtbl[10], sizeof(void*), qop, &qop);
				}
			}

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

// Global accessor (matches the GetStfOutputResource pattern) so RenderWindow's composite can readback
// each RT StF renders to and pick the one holding the frame. g_rtSeen lives in m2ftg's anon namespace.
ID3D12Resource* GetStfRenderTarget(int index)
{
	if (index < 0 || index >= m2ftg::g_rtSeenCount) return nullptr;
	return m2ftg::g_rtSeen[index];
}
int GetStfRenderTargetCount()
{
	return m2ftg::g_rtSeenCount;
}

// Last D3D12 state StF left render target #index in (for 11on12 to wrap it with the correct InState).
int GetStfRenderTargetState(int index)
{
	if (index < 0 || index >= m2ftg::g_rtSeenCount) return 0;
	return static_cast<int>(m2ftg::g_rtLastState[index]);
}

// The Model 2 3D-scene layer: dst of StF's per-frame MSAA ResolveSubresource (captured in
// HookedResolveSubresource). StF leaves it SHADER-READABLE (0xC0) at frame end; the host must
// composite it UNDER the 496x384 2D output (LJ host does the same — fight-capture ground truth).
ID3D12Resource* GetStfResolveDst()
{
	return m2ftg::g_lastStfResolveDst;
}

// Called by GameLoop right after the DLL records a frame (func()): close + ExecuteCommandLists StF's
// recorded render lists on YAMP's queue (the host's job — StF never submits them itself). See PIX.
void ExecuteStfRenderListsNow() { m2ftg::ExecuteStfRenderLists(); }

// PATH B: close + ExecuteCommandLists + flush + reopen StF's own draw list (g_lastStfCmdList) each frame
// after func() — the per-frame submit StF records for but never issues (the host's job).
void SubmitStfFrameListNow() { m2ftg::SubmitStfFrameList(); }
// The other half of the host's per-frame job: advance the upload-frame stamp and recycle StF's upload
// buffers (in-use -> available). Called after the flush so all recycled buffers are GPU-complete. Fixes
// the upload-pool exhaustion crash (FUN_18009be60, ~frame 570).
void AdvanceFrameStampNow() { m2ftg::AdvanceFrameStamp(); }
// True once a submit hung/removed the device (so GameLoop can stop before StF's next-frame upload-alloc
// crashes on the dead device — lets DRED's dump survive as the last thing in the log).
bool StfExecDisabledNow() { return m2ftg::StfExecDisabled(); }

// GameLoop brackets the DLL's per-frame render (func()) with this so the ResourceBarrier hook only
// corrects StF's own barriers, not d3d11on12's blit barriers recorded outside func().
void SetStfRenderActiveNow(bool active) { m2ftg::SetStfRenderActive(active); }

// RenderWindow registers the swapchain backbuffers so the ResourceBarrier hook dumps their transitions.
void RegisterWatchResourceNow(ID3D12Resource* r) { m2ftg::RegisterWatchResource(r); }


