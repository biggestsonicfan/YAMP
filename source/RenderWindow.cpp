#include "RenderWindow.h"

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

#include "wil/win32_helpers.h"
#include "wil/resource.h"

#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "YAMPGeneral.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static constexpr DXGI_FORMAT OUTPUT_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM;

// D3D12 debug-layer message sink: appends every validation message to d3d12_debug.log in the CWD,
// flushing per line so the message emitted right before a driver crash is on disk. Registered via
// ID3D12InfoQueue1::RegisterMessageCallback when the debug layer is enabled.
// Shared d3d12_debug.log handle so both the validation callback AND the DRED dump land in one file
// (DRED matters most when the debug layer is off / the process is dying — route it to disk, not just
// the debugger). Opened "w" by whoever writes first; subsequent writers append to the same handle.
static FILE* g_d3d12LogFile = nullptr;
static void D3D12LogWrite(const char* s)
{
	if (!g_d3d12LogFile) fopen_s(&g_d3d12LogFile, "d3d12_debug.log", "w");
	if (g_d3d12LogFile) { fputs(s, g_d3d12LogFile); fflush(g_d3d12LogFile); }
	OutputDebugStringA(s);
}

static void CALLBACK D3D12DebugMessageCallback(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
	D3D12_MESSAGE_ID id, LPCSTR description, void* /*context*/)
{
	char line[1024];
	_snprintf_s(line, _TRUNCATE, "[sev=%d cat=%d id=%d] %s\n", severity, category, id, description ? description : "(null)");
	D3D12LogWrite(line);
}

// ---- CreatePipelineState capture hook -------------------------------------------------------------
// The StF DLL builds its PSOs with the stream-based ID3D12Device2::CreatePipelineState, and one of
// them fails to parse ("Duplicate ROOT_SIGNATURE / Stream parsing failed") -> null PSO -> the frame-44
// SetPipelineState(null) crash. The stream desc is transient (gone by the time a debugger can inspect
// post-hoc), so we capture it from OUR side: YAMP owns the ID3D12Device handed to the DLL, so we hook
// its vtable slot 47 (CreatePipelineState) to dump SizeInBytes + the raw subobject stream to a file
// before forwarding. This confirms whether SizeInBytes overruns the used length (phantom type-0
// ROOT_SIGNATURE from trailing zeros). Deterministic — no debugger timing/ASLR issues.
typedef HRESULT(STDMETHODCALLTYPE* CreatePipelineState_t)(
	ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
static CreatePipelineState_t g_origCreatePipelineState = nullptr;
// The DLL sets the PSO but never SetGraphicsRootSignature on the command list (that's the host's
// job in LJ). We create the root sig in the PSO fixup below and re-bind it after each SetPipelineState.
static ID3D12RootSignature* g_capturedRootSig = nullptr;
ID3D12RootSignature* GetCapturedRootSignature() { return g_capturedRootSig; }

// PSOs created through the DLL's CreatePipelineState (slot 47). The command-list vtable is SHARED by
// every ID3D12GraphicsCommandList, so YAMP's SetPipelineState hook also fires on d3d11on12's blit
// list. Injecting StF's heaps/root-sig there corrupts the blit (driver AV). Gate the injection on
// "is this an StF PSO" so only the DLL's own draws get the StF state.
static void* g_stfPsos[512] = {};
static int   g_stfPsoCount = 0;
bool IsStfPso(void* pso)
{
	if (!pso) return false;
	for (int i = 0; i < g_stfPsoCount; ++i) if (g_stfPsos[i] == pso) return true;
	return false;
}

// ---- DRED (Device Removed Extended Data) -----------------------------------------------------------
// The composite (reading StF's output texture through 11on12) removes the GPU device with an async
// fault that surfaces as a STATUS_BREAKPOINT deep in d3d12.dll at a varying location. DRED records GPU
// auto-breadcrumbs (which op the GPU last completed) + the page-fault VA and whether it hit a RECENTLY
// FREED allocation (which would confirm the immediate-release flag is freeing a still-in-use resource).
static ID3D12Device* g_dredDevice = nullptr;

// Published each frame from BlitDX12Texture; the ResourceBarrier hook watches for this exact resource
// to learn whether StF ever transitions display_tex+0x98 to RENDER_TARGET (i.e. renders into it).
static ID3D12Resource* g_stfOutputRes = nullptr;
void SetStfOutputResource(ID3D12Resource* r) { g_stfOutputRes = r; }
ID3D12Resource* GetStfOutputResource() { return g_stfOutputRes; }

static void DumpDRED(ID3D12Device* dev)
{
	if (!dev) return;
	wil::com_ptr<ID3D12DeviceRemovedExtendedData> dred;
	if (FAILED(dev->QueryInterface(IID_PPV_ARGS(dred.put()))) || !dred)
	{
		D3D12LogWrite("[DRED] no ID3D12DeviceRemovedExtendedData interface\n");
		return;
	}
	char b[320];
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
	{
		int nodeN = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE* node = bc.pHeadAutoBreadcrumbNode;
			node && nodeN < 8; node = node->pNext, ++nodeN)
		{
			const UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			_snprintf_s(b, _TRUNCATE, "[DRED] cmdlist='%ls' queue='%ls' count=%u lastCompleted=%u\n",
				node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"?",
				node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"?",
				node->BreadcrumbCount, last);
			D3D12LogWrite(b);
			// Print the ops straddling the last completed one — that's where the GPU died.
			const UINT lo = last > 3 ? last - 3 : 0;
			const UINT hi = (last + 4 < node->BreadcrumbCount) ? last + 4 : node->BreadcrumbCount;
			for (UINT i = lo; i < hi; ++i)
			{
				_snprintf_s(b, _TRUNCATE, "[DRED]   op[%u]=%d%s\n", i,
					static_cast<int>(node->pCommandHistory[i]), i == last ? "  <== last GPU-completed" : "");
				D3D12LogWrite(b);
			}
		}
	}
	D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)))
	{
		_snprintf_s(b, _TRUNCATE, "[DRED] PageFaultVA=0x%llX\n", static_cast<unsigned long long>(pf.PageFaultVA));
		D3D12LogWrite(b);
		for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
		{
			_snprintf_s(b, _TRUNCATE, "[DRED]   EXISTING alloc '%ls' type=%d\n",
				a->ObjectNameW ? a->ObjectNameW : L"?", a->AllocationType);
			D3D12LogWrite(b);
		}
		for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
		{
			_snprintf_s(b, _TRUNCATE, "[DRED]   RECENTLY-FREED alloc '%ls' type=%d  <== USE-AFTER-FREE?\n",
				a->ObjectNameW ? a->ObjectNameW : L"?", a->AllocationType);
			D3D12LogWrite(b);
		}
	}
	D3D12LogWrite("[DRED] ---- end ----\n");
}

// Callable from HostCdevice's frame-submit path: dump DRED (last GPU op + page-fault resource) on a
// detected device-removal, and append an arbitrary line to d3d12_debug.log so the diagnosis survives
// even when nothing is attached to read OutputDebugString.
void DumpDredNow() { DumpDRED(g_dredDevice); }
void D3D12LogLine(const char* s) { if (s) D3D12LogWrite(s); }

// Catch the d3d12 STATUS_BREAKPOINT wherever it fires; if the device is actually removed, dump DRED
// once, then let the exception propagate (process still terminates, but we have the fault report).
static LONG WINAPI DredVeh(EXCEPTION_POINTERS* ep)
{
	if (ep && ep->ExceptionRecord)
	{
		static int s_seen = 0;
		const DWORD code = ep->ExceptionRecord->ExceptionCode;
		// Log the first handful of "hard" exceptions (skip the common C++/first-chance 0xE06D7363).
		if (s_seen < 16 && code != 0xE06D7363 && code != 0x406D1388)
		{
			++s_seen;
			char b[128];
			_snprintf_s(b, _TRUNCATE, "[DRED] VEH exception code=0x%08lX at %p\n",
				static_cast<unsigned long>(code), ep->ExceptionRecord->ExceptionAddress);
			D3D12LogLine(b);
		}
		// The crash is a CPU access violation the D3D12 log can't otherwise see. Dump WHERE: the faulting
		// instruction (RIP), the bad pointer + read/write, and any return addresses on the stack that land
		// in the StF DLL (fixed base 0x180000000) — i.e. whether StF's own code is faulting (our list
		// close/reset corrupted its state) or ours is. First 6 AVs only (SafeAcquireResource's guarded QI
		// probe raises expected first-chance AVs; the fatal one shows a distinct RIP / StF call chain).
		if (code == 0xC0000005 && ep->ContextRecord)
		{
			static int s_av = 0;
			if (s_av < 6)
			{
				++s_av;
				const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
				const ULONG_PTR rip = ep->ContextRecord->Rip;
				// Resolve the module the faulting instruction lives in (name + offset).
				char mod[MAX_PATH] = "?"; ULONG_PTR modBase = 0;
				HMODULE hm = nullptr;
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(rip), &hm) && hm)
				{
					modBase = reinterpret_cast<ULONG_PTR>(hm);
					char full[MAX_PATH]; if (GetModuleFileNameA(hm, full, MAX_PATH))
					{ const char* s = strrchr(full, '\\'); lstrcpynA(mod, s ? s + 1 : full, MAX_PATH); }
				}
				char b[288];
				_snprintf_s(b, _TRUNCATE,
					"[crash] AV #%d %s addr=0x%llX rip=0x%llX (%s +0x%llX) rsp=0x%llX\n",
					s_av, info[0] ? "WRITE" : "READ",
					static_cast<unsigned long long>(info[1]),
					static_cast<unsigned long long>(rip), mod,
					static_cast<unsigned long long>(rip - modBase),
					static_cast<unsigned long long>(ep->ContextRecord->Rsp));
				D3D12LogLine(b);
				// Scan the stack for return addresses in ANY loaded module -> a mini call chain.
				const ULONG_PTR* sp = reinterpret_cast<const ULONG_PTR*>(ep->ContextRecord->Rsp);
				int found = 0;
				for (int i = 0; i < 160 && found < 10; ++i)
				{
					ULONG_PTR v = 0;
					__try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
					if (v < 0x10000ull) continue;
					HMODULE hmv = nullptr;
					if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCSTR>(v), &hmv) && hmv)
					{
						char full[MAX_PATH] = "?", nm[64] = "?";
						if (GetModuleFileNameA(hmv, full, MAX_PATH)) { const char* s = strrchr(full, '\\'); lstrcpynA(nm, s ? s + 1 : full, 64); }
						const ULONG_PTR vb = reinterpret_cast<ULONG_PTR>(hmv);
						char sb[96];
						_snprintf_s(sb, _TRUNCATE, "[crash]   stack[%d] -> %s+0x%llX\n", i, nm,
							static_cast<unsigned long long>(v - vb));
						D3D12LogLine(sb);
						++found;
					}
				}
			}
		}
	}
	if (ep && ep->ExceptionRecord && g_dredDevice)
	{
		const DWORD code = ep->ExceptionRecord->ExceptionCode;
		if (code == 0x80000003 || code == 0xC0000005)
		{
			static bool s_dumped = false;
			if (!s_dumped)
			{
				const HRESULT drr = g_dredDevice->GetDeviceRemovedReason();
				if (drr != S_OK)
				{
					s_dumped = true;
					char b[112];
					_snprintf_s(b, _TRUNCATE, "[DRED] exception 0x%08lX at %p; deviceRemovedReason=0x%08lX\n",
						static_cast<unsigned long>(code), ep->ExceptionRecord->ExceptionAddress,
						static_cast<unsigned long>(drr));
					OutputDebugStringA(b);
					DumpDRED(g_dredDevice);
				}
			}
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// Reflect a shader's resource bindings (the ground truth for the root sig). D3D_SHADER_INPUT_TYPE:
// 0=CBV, 1=TBUFFER, 2=SRV(texture), 3=SAMPLER, 4=UAV. Logs each resource's register + space so we can
// build a root signature that matches exactly (replacing the fabricated best-guess one).
typedef HRESULT(WINAPI* D3DReflect_t)(LPCVOID, SIZE_T, REFIID, void**);
static void ReflectShaderBindings(const void* bc, size_t len, const char* stage)
{
	if (!bc || !len) { char b[64]; _snprintf_s(b, _TRUNCATE, "[reflect] %s: no bytecode\n", stage); OutputDebugStringA(b); return; }
	{
		const uint8_t* m = static_cast<const uint8_t*>(bc);
		char b[96];
		// DXBC magic "DXBC"; then scan the chunk FourCCs so we know if it's DXIL (DXIL/STAT) vs DXBC (RDEF).
		_snprintf_s(b, _TRUNCATE, "[reflect] %s magic=%c%c%c%c chunks:", stage, m[0], m[1], m[2], m[3]);
		OutputDebugStringA(b);
		if (m[0] == 'D' && m[1] == 'X' && m[2] == 'B' && m[3] == 'C' && len > 0x20)
		{
			const uint32_t nchunks = *reinterpret_cast<const uint32_t*>(m + 0x1C);
			const uint32_t* offs = reinterpret_cast<const uint32_t*>(m + 0x20);
			for (uint32_t i = 0; i < nchunks && i < 16; ++i)
			{
				if (offs[i] + 4 > len) break;
				const char* fc = reinterpret_cast<const char*>(m + offs[i]);
				_snprintf_s(b, _TRUNCATE, "  %c%c%c%c", fc[0], fc[1], fc[2], fc[3]);
				OutputDebugStringA(b);
			}
		}
		OutputDebugStringA("\n");
	}
	// DXIL: parse the PSV0 (Pipeline State Validation) chunk for the resource bindings.
	const uint8_t* m = static_cast<const uint8_t*>(bc);
	if (!(m[0] == 'D' && m[1] == 'X' && m[2] == 'B' && m[3] == 'C') || len < 0x20) return;
	const uint32_t nchunks = *reinterpret_cast<const uint32_t*>(m + 0x1C);
	const uint32_t* offs = reinterpret_cast<const uint32_t*>(m + 0x20);
	const uint8_t* psv = nullptr;
	for (uint32_t i = 0; i < nchunks && i < 32; ++i)
	{
		const char* fc = reinterpret_cast<const char*>(m + offs[i]);
		if (fc[0] == 'P' && fc[1] == 'S' && fc[2] == 'V' && fc[3] == '0') { psv = m + offs[i] + 8; break; }
	}
	if (!psv) { char b[48]; _snprintf_s(b, _TRUNCATE, "[reflect] %s: no PSV0\n", stage); OutputDebugStringA(b); return; }
	const uint8_t* p = psv;
	const uint32_t riSize = *reinterpret_cast<const uint32_t*>(p); p += 4 + riSize;   // skip PSVRuntimeInfo
	const uint32_t resCount = *reinterpret_cast<const uint32_t*>(p); p += 4;
	char b[128];
	_snprintf_s(b, _TRUNCATE, "[reflect] %s: PSV0 resources=%u\n", stage, resCount);
	OutputDebugStringA(b);
	if (resCount)
	{
		const uint32_t biSize = *reinterpret_cast<const uint32_t*>(p); p += 4;
		for (uint32_t i = 0; i < resCount; ++i)
		{
			const uint32_t* ri = reinterpret_cast<const uint32_t*>(p + i * biSize);
			const uint32_t resType = ri[0], space = ri[1], lo = ri[2], hi = ri[3];
			const char* tn = resType == 1 ? "SAMPLER" : resType == 2 ? "CBV"
				: (resType >= 3 && resType <= 5) ? "SRV" : (resType >= 6) ? "UAV" : "?";
			_snprintf_s(b, _TRUNCATE, "[reflect]   %s %s(rt%u) space=%u reg=%u..%u\n",
				stage, tn, resType, space, lo, hi);
			OutputDebugStringA(b);
		}
	}
}

static HRESULT STDMETHODCALLTYPE HookedCreatePipelineState(
	ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPSO)
{
	static FILE* psoLog = nullptr;
	if (psoLog == nullptr)
	{
		fopen_s(&psoLog, "pso_stream.log", "w");
	}
	// Dump the stream BEFORE forwarding, so it is on disk even if the call faults.
	if (psoLog != nullptr && pDesc != nullptr)
	{
		fprintf(psoLog, "=== CreatePipelineState SizeInBytes=%zu pStream=%p ===\n",
			pDesc->SizeInBytes, pDesc->pPipelineStateSubobjectStream);
		const uint8_t* p = static_cast<const uint8_t*>(pDesc->pPipelineStateSubobjectStream);
		if (p != nullptr && pDesc->SizeInBytes <= 0x4000)
		{
			for (size_t i = 0; i < pDesc->SizeInBytes; i += 16)
			{
				fprintf(psoLog, "%04zX: ", i);
				for (size_t j = 0; j < 16 && (i + j) < pDesc->SizeInBytes; ++j)
					fprintf(psoLog, "%02X ", p[i + j]);
				fprintf(psoLog, "\n");
			}
		}
		fflush(psoLog);
	}

	// --- Subobject type-tag fixup for the m2ftg 720-byte PSO stream -----------------------------
	// The pxd engine's per-draw resolver (FUN_18009dcd0) fills only the PAYLOADS of the PSO stream
	// template at cgs_device_context+0x518; the D3D12 subobject TYPE tags are laid down once by the
	// host's device_context construction, which YAMP (as host) does not perform (`new cgs_device_context{}`
	// zero-inits). Result: every subobject type reads 0, so D3D12 sees two type-0 ROOT_SIGNATUREs ->
	// E_INVALIDARG -> null PSO -> SetPipelineState(null) crash. The correct 17-tag skeleton was captured
	// by reading a live Lost Judgment device_context+0x518 (byte-for-byte the payloads already match
	// YAMP's; only the type tags were missing). Stamp them here, right before CreatePipelineState.
	if (pDesc != nullptr && pDesc->SizeInBytes == 720 && pDesc->pPipelineStateSubobjectStream != nullptr)
	{
		static const struct { uint32_t off; uint32_t type; } kTags[] = {
			{0x000, 0},  // ROOT_SIGNATURE
			{0x010, 1},  // VS
			{0x028, 2},  // PS
			{0x040, 3},  // DS
			{0x058, 4},  // HS
			{0x070, 5},  // GS
			{0x088, 8},  // BLEND
			{0x1D8, 9},  // SAMPLE_MASK
			{0x1E0, 10}, // RASTERIZER
			{0x210, 21}, // DEPTH_STENCIL1
			{0x250, 12}, // INPUT_LAYOUT
			{0x268, 13}, // IB_STRIP_CUT_VALUE
			{0x270, 14}, // PRIMITIVE_TOPOLOGY
			{0x278, 15}, // RENDER_TARGET_FORMATS
			{0x2A0, 16}, // DEPTH_STENCIL_FORMAT
			{0x2A8, 17}, // SAMPLE_DESC
			{0x2B8, 19}, // CACHED_PIPELINE_STATE
		};
		uint8_t* s = static_cast<uint8_t*>(const_cast<void*>(pDesc->pPipelineStateSubobjectStream));
		for (const auto& t : kTags)
			*reinterpret_cast<uint32_t*>(s + t.off) = t.type;

		// *** THE BLACK-SCREEN FIX (PIX three-way diff, 2026-07-25) ***
		// Like the type tags, the SAMPLE_MASK payload is host-provided state the template never fills:
		// every StF PSO in the YAMP capture had SampleMask=0 (ALL rasterized samples masked out -> every
		// draw's output discarded at the OM; clears unaffected — exactly the observed black-with-red-clear),
		// while every PSO in the working LJ and VF2 captures had 0xFFFFFFFF. Payload sits right after the
		// 4-byte tag at 0x1D8. Stamp it unconditionally, as with the tags.
		*reinterpret_cast<uint32_t*>(s + 0x1DC) = 0xFFFFFFFFu;
		// LJ also sets IB_STRIP_CUT_VALUE to 0xFFFF (YAMP template: DISABLED). Inert for the triangle
		// lists StF draws, but match the known-good stream. Payload follows the tag at 0x268.
		*reinterpret_cast<uint32_t*>(s + 0x26C) = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
		if (psoLog != nullptr)
			fprintf(psoLog, "  [samplemask] post-stamp 0x1DC=0x%08X strip=0x%X\n",
				*reinterpret_cast<uint32_t*>(s + 0x1DC), *reinterpret_cast<uint32_t*>(s + 0x26C));

		// *** BLACK-SCREEN FIX CANDIDATE (GPU-Based Validation, 2026-07-25) ***
		// GBV reported id=679: "The Pixel Shader expects a Render Target View bound to slot 0, but
		// the generic program indicates that none will be bound. This is OK, as writes of an unbound
		// Render Target are discarded." A PSO with NumRenderTargets==0 discards EVERY pixel the shader
		// writes, no matter what OMSetRenderTargets binds — exactly the observed symptom (valid
		// geometry, valid state, black RTs). The RT_FORMATS subobject payload is a
		// D3D12_RT_FORMAT_ARRAY at 0x280: DXGI_FORMAT RTFormats[8] then UINT NumRenderTargets@0x2A0-4.
		// Payload offset: the RT_FORMATS tag is at 0x278 and the next tag (DSV_FORMAT) at 0x2A0 —
		// a 40-byte span = 4-byte type + sizeof(D3D12_RT_FORMAT_ARRAY)(36). So the payload starts at
		// 0x27C (NOT 0x280; reading there returns the *next* subobject's type tag, which looked like
		// a bogus "numRT=16").
		{
			DXGI_FORMAT* rtFormats = reinterpret_cast<DXGI_FORMAT*>(s + 0x27C);
			uint32_t* numRT = reinterpret_cast<uint32_t*>(s + 0x27C + 8 * sizeof(DXGI_FORMAT));
			static int s_rtLog = 0;
			if (psoLog != nullptr && s_rtLog < 8)
			{
				fprintf(psoLog, "  [rtfmt] numRT=%u fmts=[%d %d %d %d]\n",
					*numRT, rtFormats[0], rtFormats[1], rtFormats[2], rtFormats[3]);
				s_rtLog++;
			}
			// StF's RTs are all B8G8R8A8_UNORM (87) per the [rt-rb] readbacks.
			constexpr bool kForceRenderTargetFormats = true;
			if (kForceRenderTargetFormats && *numRT == 0)
			{
				*numRT = 1;
				if (rtFormats[0] == DXGI_FORMAT_UNKNOWN) rtFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
				if (psoLog != nullptr) fprintf(psoLog, "  [rtfmt] FORCED numRT=1 fmt=%d\n", rtFormats[0]);
			}
		}

		// ONE-TIME: reflect the VS/PS shader resource bindings to derive the REAL root-sig layout.
		// Subobjects are {8-byte type} + payload; VS type@0x010 -> D3D12_SHADER_BYTECODE{ptr@0x018,len@0x020},
		// PS type@0x028 -> {ptr@0x030,len@0x038}.
		{
			static bool s_reflected = false;
			if (!s_reflected)
			{
				s_reflected = true;
				const void* vsBc = *reinterpret_cast<void**>(s + 0x018);
				const size_t vsLen = *reinterpret_cast<size_t*>(s + 0x020);
				const void* psBc = *reinterpret_cast<void**>(s + 0x030);
				const size_t psLen = *reinterpret_cast<size_t*>(s + 0x038);
				char b[128];
				_snprintf_s(b, _TRUNCATE, "[reflect] VS bc=%p len=%zu | PS bc=%p len=%zu\n", vsBc, vsLen, psBc, psLen);
				OutputDebugStringA(b);
				ReflectShaderBindings(vsBc, vsLen, "VS");
				ReflectShaderBindings(psBc, psLen, "PS");
			}
		}

		// The ROOT_SIGNATURE subobject payload (stream+0x08) is null in YAMP (the host normally
		// creates the shared root signature; LJ's is non-null). D3D12 rejects a graphics PSO with no
		// root signature, so try to build one from the VS/PS embedded RTS0 blob and stamp it in.
		uint64_t& rsSlot = *reinterpret_cast<uint64_t*>(s + 0x08);
		if (rsSlot == 0)
		{
			static ID3D12RootSignature* s_rootSig = nullptr;
			static bool s_rsTried = false;
			if (!s_rsTried)
			{
				s_rsTried = true;
				// The pxd m2ftg root signature, captured byte-for-byte from a live Lost Judgment: LJ's
				// host builds it at boot via D3D12SerializeVersionedRootSignature (sub_141AF9DC0, from
				// shader reflection) and hands the ID3D12RootSignature to the DLL through the template.
				// The shaders don't embed an RTS0 chunk, so we recreate the sig from the serialized blob.
				// DXBC/RTS0, version 1.1, 2 params, 0 static samplers (204 bytes).
				static const uint8_t kRootSigBlob[] = {
					0x44, 0x58, 0x42, 0x43, 0x35, 0x30, 0x48, 0xBE, 0x96, 0xA3, 0xB7, 0x2A,
					0xB5, 0xF8, 0xB1, 0xD2, 0xE0, 0xFC, 0x49, 0x29, 0x01, 0x00, 0x00, 0x00,
					0xCC, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
					0x52, 0x54, 0x53, 0x30, 0xA0, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
					0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0xA0, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Flags: 0x3E->0x01 (ALLOW_INPUT_ASSEMBLER, no denies) TEST
					0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
					0x38, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
					0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
					0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
					0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x88, 0x00, 0x00, 0x00,
					0x03, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
				};
				// EXACT graphics root sig (LJ ApiObjectId 1302), captured from a PIX C++ export of LJ+StF. 10 params
				// = per-stage table PAIRS { {CBV b0[14] + SRV t0[24]} , {SAMPLER s0[16]} } for the 5 graphics stages
				// in order PIXEL, VERTEX, GEOMETRY, HULL, DOMAIN. CBV/SRV ranges are DESCRIPTORS_VOLATILE; the sampler
				// range is FLAG_NONE. (My earlier 4-param guess was missing GS/HS/DS and wrongly added a UAV range.)
				auto CsRange = [](D3D12_DESCRIPTOR_RANGE_TYPE t, UINT n) {
					D3D12_DESCRIPTOR_RANGE1 r{};
					r.RangeType = t; r.NumDescriptors = n; r.BaseShaderRegister = 0; r.RegisterSpace = 0;
					r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
					r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
					return r;
				};
				static D3D12_DESCRIPTOR_RANGE1 rCbvSrv[5][2];
				static D3D12_DESCRIPTOR_RANGE1 rSamp[5][1];
				for (int st = 0; st < 5; ++st) {
					rCbvSrv[st][0] = CsRange(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 14);
					rCbvSrv[st][1] = CsRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 24);
					rSamp[st][0] = CsRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16);
					// PIX's 1302 declared this sampler range STATIC (FLAG_NONE), which requires every sampler
					// descriptor in the bound range to be initialized before SetGraphicsRootDescriptorTable
					// (d3d12_debug.log id=646, 16k hits — StF binds sampler-heap slots our ring never seeds).
					// LJ satisfies STATIC by fully populating its sampler heap at device-start; YAMP does not,
					// so mark the range DESCRIPTORS_VOLATILE (as CBV/SRV already are) to defer to execute-time
					// instead of failing the set. Strictly more permissive; safe.
					rSamp[st][0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
				}
				// PIX-captured order (LJ ApiObjectId 1302): PIXEL, VERTEX, GEOMETRY, HULL, DOMAIN.
				// (Black-screen tests 2026-07-25: VISIBILITY_ALL -> GPU hang ~frame 40 [unseeded
				// sampler slots became visible everywhere]; VERTEX/PIXEL swap -> unchanged black.
				// So the visibility order matches the capture and is NOT the black-screen cause.)
				static const D3D12_SHADER_VISIBILITY kVis[5] = {
					D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_VERTEX,
					D3D12_SHADER_VISIBILITY_GEOMETRY, D3D12_SHADER_VISIBILITY_HULL, D3D12_SHADER_VISIBILITY_DOMAIN };
				static D3D12_ROOT_PARAMETER1 params[10] = {};
				for (int st = 0; st < 5; ++st) {
					params[2*st].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
					params[2*st].DescriptorTable.NumDescriptorRanges = 2;
					params[2*st].DescriptorTable.pDescriptorRanges = rCbvSrv[st];
					params[2*st].ShaderVisibility = kVis[st];
					params[2*st+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
					params[2*st+1].DescriptorTable.NumDescriptorRanges = 1;
					params[2*st+1].DescriptorTable.pDescriptorRanges = rSamp[st];
					params[2*st+1].ShaderVisibility = kVis[st];
				}
				D3D12_VERSIONED_ROOT_SIGNATURE_DESC vdesc = {};
				vdesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
				vdesc.Desc_1_1.NumParameters = 10;
				vdesc.Desc_1_1.pParameters = params;
				vdesc.Desc_1_1.NumStaticSamplers = 0;
				vdesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
				ID3DBlob* sblob = nullptr; ID3DBlob* serr = nullptr;
				HRESULT shr = D3D12SerializeVersionedRootSignature(&vdesc, &sblob, &serr);
				HRESULT rhr = E_FAIL;
				if (SUCCEEDED(shr) && sblob != nullptr)
					rhr = self->CreateRootSignature(0, sblob->GetBufferPointer(),
						sblob->GetBufferSize(), IID_PPV_ARGS(&s_rootSig));
				if (psoLog != nullptr)
					fprintf(psoLog, "  [rootsig] 4-param graphics sig shr=0x%08lX rhr=0x%08lX obj=%p %s\n",
						static_cast<unsigned long>(shr), static_cast<unsigned long>(rhr),
						static_cast<void*>(s_rootSig),
						serr ? reinterpret_cast<const char*>(serr->GetBufferPointer()) : "");
				if (sblob) sblob->Release();
				if (serr) serr->Release();
			}
			if (s_rootSig != nullptr) { rsSlot = reinterpret_cast<uint64_t>(s_rootSig); g_capturedRootSig = s_rootSig; }
		}

		if (psoLog != nullptr)
		{
			fprintf(psoLog, "  [fixup] stamped %zu type tags; rootsig slot=0x%llX\n",
				sizeof(kTags) / sizeof(kTags[0]), (unsigned long long)rsSlot);
			fflush(psoLog);
		}
	}

	HRESULT hr = g_origCreatePipelineState(self, pDesc, riid, ppPSO);
	if (SUCCEEDED(hr) && ppPSO && *ppPSO && g_stfPsoCount < 512)
		g_stfPsos[g_stfPsoCount++] = *ppPSO; // mark as an StF PSO for injection gating
	if (psoLog != nullptr)
	{
		fprintf(psoLog, "  -> hr=0x%08lX  ppPSO=%p\n",
			static_cast<unsigned long>(hr), ppPSO ? *ppPSO : nullptr);
		// On failure, retry the SAME stream on a throwaway DEBUG device to capture WHY D3D12 rejected
		// it. The main device runs without the debug layer (enabling it shifts timing / trips the
		// 11-on-12 path before we reach here), so we lazily create a private debug device just for
		// validation. Shader bytecode in the stream is device-agnostic, so the verdict is identical.
		if (FAILED(hr))
		{
			static ID3D12Device2* s_dbgDev = nullptr;
			static ID3D12InfoQueue* s_dbgIq = nullptr;
			static bool s_tried = false;
			if (!s_tried)
			{
				s_tried = true;
				wil::com_ptr<ID3D12Debug> dc;
				if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dc.put()))))
					dc->EnableDebugLayer();
				ID3D12Device2* d = nullptr;
				if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d))))
				{
					s_dbgDev = d;
					d->QueryInterface(IID_PPV_ARGS(&s_dbgIq));
				}
			}
			if (s_dbgDev != nullptr && s_dbgIq != nullptr)
			{
				s_dbgIq->ClearStoredMessages();
				ID3D12PipelineState* tmp = nullptr;
				HRESULT hr2 = s_dbgDev->CreatePipelineState(pDesc, IID_PPV_ARGS(&tmp));
				const UINT64 n = s_dbgIq->GetNumStoredMessages();
				fprintf(psoLog, "  [dbg-retry] hr2=0x%08lX  %llu messages:\n",
					static_cast<unsigned long>(hr2), (unsigned long long)n);
				for (UINT64 i = 0; i < n; ++i)
				{
					SIZE_T len = 0;
					s_dbgIq->GetMessage(i, nullptr, &len);
					if (len == 0) continue;
					std::vector<uint8_t> buf(len);
					auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
					if (SUCCEEDED(s_dbgIq->GetMessage(i, msg, &len)) && msg->pDescription)
						fprintf(psoLog, "    [id=%d] %s\n", msg->ID, msg->pDescription);
				}
				if (tmp) tmp->Release();
			}
			else
			{
				fprintf(psoLog, "  [dbg-retry] could not create debug device\n");
			}
		}
		fprintf(psoLog, "\n");
		fflush(psoLog);
	}
	return hr;
}

// DIAGNOSTIC: log every CreateDescriptorHeap so we can see who creates the shader-visible
// CBV/SRV/UAV (num~1M) and SAMPLER (num 2048) heaps the DLL binds via SetDescriptorHeaps —
// they are NOT the gs+0x7550 rings YAMP wired, so the root-table handle mismatches.
typedef HRESULT(STDMETHODCALLTYPE* CreateDescriptorHeap_t)(
	ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
static CreateDescriptorHeap_t g_origCreateDescriptorHeap = nullptr;
// The DLL creates its own large shader-visible heaps (CBV/SRV/UAV ~1M + SAMPLER 2048) and binds
// them; the gs+0x7550 descriptor rings must reference THESE (not YAMP-created heaps) so the DLL's
// root-table GPU handles resolve into the bound heap. Capture them here for PatchGs to wire.
static ID3D12DescriptorHeap* g_dllRingCbvSrvHeap  = nullptr;
static ID3D12DescriptorHeap* g_dllRingSamplerHeap = nullptr;
ID3D12DescriptorHeap* GetDllRingCbvSrvHeap()  { return g_dllRingCbvSrvHeap; }
ID3D12DescriptorHeap* GetDllRingSamplerHeap() { return g_dllRingSamplerHeap; }

static HRESULT STDMETHODCALLTYPE HookedCreateDescriptorHeap(
	ID3D12Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID riid, void** ppv)
{
	HRESULT hr = g_origCreateDescriptorHeap(self, d, riid, ppv);
	if (SUCCEEDED(hr) && d && ppv && *ppv && (d->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
	{
		auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(*ppv);
		if (d->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && d->NumDescriptors >= 500000 && !g_dllRingCbvSrvHeap)
			g_dllRingCbvSrvHeap = heap;
		else if (d->Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER && !g_dllRingSamplerHeap)
			g_dllRingSamplerHeap = heap;
	}
	static FILE* f = nullptr;
	if (f == nullptr) fopen_s(&f, "heaps.log", "w");
	if (f != nullptr && d != nullptr)
	{
		void* h = ppv ? *ppv : nullptr;
		unsigned long long gpu = 0, cpu = 0;
		if (SUCCEEDED(hr) && h && (d->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
		{
			auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(h);
			gpu = heap->GetGPUDescriptorHandleForHeapStart().ptr;
			cpu = heap->GetCPUDescriptorHandleForHeapStart().ptr;
		}
		fprintf(f, "CreateDescriptorHeap type=%d num=%u sv=%d -> heap=%p gpu=0x%llX cpu=0x%llX hr=0x%lX\n",
			d->Type, d->NumDescriptors, (d->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? 1 : 0,
			h, gpu, cpu, static_cast<unsigned long>(hr));
		fflush(f);
	}
	return hr;
}

static void InstallCreatePipelineStateHook(ID3D12Device* device)
{
	if (device == nullptr || g_origCreatePipelineState != nullptr) return;
	void** vtbl = *reinterpret_cast<void***>(device);
	void** slot = &vtbl[47]; // ID3D12Device2::CreatePipelineState (offset 47*8 = 0x178)
	g_origCreatePipelineState = reinterpret_cast<CreatePipelineState_t>(*slot);
	DWORD oldProtect = 0;
	if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
	{
		*slot = reinterpret_cast<void*>(&HookedCreatePipelineState);
		VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
	}
	// Also hook CreateDescriptorHeap (ID3D12Device vtable slot 14 = offset 0x70).
	void** slot14 = &vtbl[14];
	g_origCreateDescriptorHeap = reinterpret_cast<CreateDescriptorHeap_t>(*slot14);
	if (VirtualProtect(slot14, sizeof(void*), PAGE_READWRITE, &oldProtect))
	{
		*slot14 = reinterpret_cast<void*>(&HookedCreateDescriptorHeap);
		VirtualProtect(slot14, sizeof(void*), oldProtect, &oldProtect);
	}
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// The single RenderWindow instance, for WndProc (which has no user data pointer) to reach.
static RenderWindow* g_windowInstance = nullptr;

static LRESULT WINAPI WindowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	LRESULT imguiResult = ImGui_ImplWin32_WndProcHandler(hWnd, Msg, wParam, lParam);
	if (imguiResult != 0) return imguiResult;

	switch (Msg)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_SIZE:
		// Runs on the window thread; the render thread picks it up in BeginFrame.
		if (wParam != SIZE_MINIMIZED && g_windowInstance != nullptr)
		{
			g_windowInstance->RequestResize(LOWORD(lParam), HIWORD(lParam));
		}
		break;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		gGeneral.SetKeyPressed(wParam, true);
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		gGeneral.SetKeyPressed(wParam, false);
		break;

	default:
		break;
	}
	return DefWindowProc(hWnd, Msg, wParam, lParam);
}

RenderWindow::RenderWindow(HINSTANCE instance, HINSTANCE dllInstance, int cmdShow)
{
	g_windowInstance = this;

	wil::unique_event startupEvent(wil::EventOptions::None);
	m_windowThread = std::thread([this, &startupEvent, instance, dllInstance, cmdShow] {

		WNDCLASSEX wndClass { sizeof(wndClass) };
		wndClass.hInstance = instance;
		wndClass.lpfnWndProc = WindowProc;
		wndClass.lpszClassName = L"YAKUZA_VF5FS";
		wndClass.hIcon = LoadIcon(dllInstance, MAKEINTRESOURCE(101));
		wndClass.hCursor = LoadCursor(nullptr, IDC_ARROW);

		const ATOM windowClass = RegisterClassEx(&wndClass);
		THROW_LAST_ERROR_IF(windowClass == 0);

		const YAMPSettings* settings = gGeneral.GetSettings();

		DWORD style;
		if (settings->m_fullscreen) // Borderless for now
		{
			style = WS_POPUP;

			RECT desktop;
			GetWindowRect(GetDesktopWindow(), &desktop);
			m_width = desktop.right - desktop.left;
			m_height = desktop.bottom - desktop.top;
		}
		else
		{
			style = WS_OVERLAPPEDWINDOW;
			m_width = settings->m_resX;
			m_height = settings->m_resY;
		}

        RECT clientArea { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
		AdjustWindowRect(&clientArea, style, FALSE);

		wil::unique_hwnd window(CreateWindowExW(0, L"YAKUZA_VF5FS", L"Virtua Fighter 5: Final Showdown", style, CW_USEDEFAULT, CW_USEDEFAULT,
			clientArea.right - clientArea.left, clientArea.bottom - clientArea.top, nullptr, nullptr, instance, nullptr));
		THROW_LAST_ERROR_IF_NULL(window);

		ImGui_ImplWin32_Init(window.get());

		// --- D3D12 debug layer. MUST be enabled BEFORE device creation. It converts raw driver
		// faults (e.g. the null-store crash inside nvwgf2umx.dll from the m2ftg 2D ring draw) into
		// readable validation messages naming the exact bad parameter. GPU-based validation
		// additionally catches malformed vertex-buffer views / missing bindings at draw time.
		// Gated on the existing m_useD3DDebugLayer setting (already used for the 11-on-12 device).
		if (gGeneral.GetSettings()->m_useD3DDebugLayer)
		{
			wil::com_ptr<ID3D12Debug> debugController;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
			{
				debugController->EnableDebugLayer();
				// GPU-based validation catches draw-time resource/VBV errors but is very slow
				// (can time out the automated driver). Left off for the first pass; flip to TRUE
				// if the basic layer doesn't flag the bad call.
				wil::com_ptr<ID3D12Debug1> debugController1;
				if (SUCCEEDED(debugController->QueryInterface(IID_PPV_ARGS(debugController1.put()))))
				{
					// GBV is the one instrument never yet run to the scene: it names the exact
					// draw-time descriptor/resource fault the basic layer cannot see. It was tried
					// once and the layer died at ~frame 11 on the id=527 barrier desync, which is
					// FIXED now, so it should survive into StF's scene. Slow but decisive.
					debugController1->SetEnableGPUBasedValidation(FALSE); // GBV already gave us id=679; too slow for normal runs
				}
			}
		}

		// --- DRED: enable GPU auto-breadcrumbs + page-fault reporting BEFORE device creation, so a
		// device-removal reports the exact faulting GPU op + resource (independent of the debug layer).
		{
			wil::com_ptr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dredSettings.put()))) && dredSettings)
			{
				dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				OutputDebugStringA("[DRED] enabled auto-breadcrumbs + page-fault reporting\n");
			}
		}

		// --- D3D12 device
		THROW_IF_FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3d12Device)));

		// Register the DRED breakpoint handler now that the device exists.
		g_dredDevice = m_d3d12Device.get();
		AddVectoredExceptionHandler(1, &DredVeh);

		// Hook CreatePipelineState on the real device unconditionally (independent of the debug layer)
		// so the failing PSO's transient subobject stream is dumped to pso_stream.log. Runs layer-off,
		// avoiding the SDKLayers wrapper. Cheap: one vtable slot swap; the hook just forwards.
		InstallCreatePipelineStateHook(m_d3d12Device.get());

		// --- Capture validation messages to a file. Since the x64dbg MCP can't read the debugger
		// log, we register an ID3D12InfoQueue1 callback that writes each message to disk immediately
		// (fflush per line) so even the message that precedes the driver crash is captured. Written
		// to "d3d12_debug.log" in the CWD (the exe dir). Also break on CORRUPTION (never benign).
		if (gGeneral.GetSettings()->m_useD3DDebugLayer)
		{
			wil::com_ptr<ID3D12InfoQueue1> infoQueue1;
			if (SUCCEEDED(m_d3d12Device->QueryInterface(IID_PPV_ARGS(infoQueue1.put()))))
			{
				DWORD cookie = 0;
				infoQueue1->RegisterMessageCallback(&D3D12DebugMessageCallback,
					D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
			}
			// NOTE: intentionally NOT calling SetBreakOnSeverity — a DebugBreak on the recurring
			// CORRUPTION messages halts the debuggee before it reaches the real driver fault. The
			// file callback above captures every message up to (and including) the crashing frame.
		}

		// --- Command queue
        {
		D3D12_COMMAND_QUEUE_DESC qdesc{};
		qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		THROW_IF_FAILED(m_d3d12Device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&m_cmdQueue)));

		// Dedicated list for the per-frame COMMON->RENDER_TARGET backbuffer barrier 11on12 omits.
		THROW_IF_FAILED(m_d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_bbBarrierAlloc)));
		THROW_IF_FAILED(m_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_bbBarrierAlloc.get(), nullptr, IID_PPV_ARGS(&m_bbBarrierList)));
		THROW_IF_FAILED(m_bbBarrierList->Close());
        }

		// --- 11-on-12 device + context
        {
            // Keep the D3D11-on-12 debug flag OFF even when the D3D12 debug layer is on: the D3D11
            // validation halts on a benign PSSetShaderResources(null) in the ImGui/11-on-12 path
            // before the StF DX12 PSO is ever built, masking the D3D12 error we actually want.
            const UINT flags = 0;

		wil::com_ptr<ID3D11Device> device11;
		wil::com_ptr<ID3D11DeviceContext> context11;
		IUnknown* queues[] = { m_cmdQueue.get() };

		THROW_IF_FAILED(D3D11On12CreateDevice(
			m_d3d12Device.get(),
			flags,
			nullptr, 0,
			queues, _countof(queues),
			0,
			&device11, &context11, nullptr));

		m_device = std::move(device11);
		m_deviceContext = std::move(context11);
		m_d3d11on12 = m_device.query<ID3D11On12Device>();
        }

		// --- Create swap chain for the D3D12 queue
		{
		wil::com_ptr<IDXGIFactory6> factory;
		THROW_IF_FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));;

		DXGI_SWAP_CHAIN_DESC1 scd{};
		scd.Width = m_width;
		scd.Height = m_height;
		scd.Format = OUTPUT_FORMAT;
		scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scd.BufferCount = kBufferCount;
		scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		scd.SampleDesc.Count = 1;

		wil::com_ptr<IDXGISwapChain1> sc1;
		THROW_IF_FAILED(factory->CreateSwapChainForHwnd(
			m_cmdQueue.get(), window.get(), &scd, nullptr, nullptr, &sc1));

		m_swapChain = std::move(sc1);
        }

        // --- ImGui init (DX11 backend)
		ImGui_ImplDX11_Init(m_device.get(), m_deviceContext.get());

		// --- Wrap backbuffers
		CreateWrappedBackbuffers();
		ShowWindow(window.get(), cmdShow);
		UpdateWindow(window.get());
		m_window = std::unique_ptr<std::remove_pointer_t<HWND>, hwnd_deleter>(window.release());

		CreateRenderResources();
		EnumerateDisplayModes();
		CalculateViewport();

		m_ui.GetDefaultsFromSettings();
		startupEvent.SetEvent();

		BOOL ret;
		MSG msg;
        while ((ret = GetMessage(&msg, NULL, 0, 0)) != 0)
		{
			if (ret == -1)
			{
				// handle the error and possibly exit
			}
			else
			{
				TranslateMessage(&msg); 
				DispatchMessage(&msg); 
			}
		}

		m_shuttingDownWindow.store(true, std::memory_order_relaxed);

		// TODO: Pass the exit code back to WinMain via RenderWindow
		
	});
	startupEvent.wait();
}

RenderWindow::~RenderWindow()
{
	PostMessage(m_window.get(), WM_CLOSE, 0, 0);
	m_windowThread.join();
}

// Lost Judgment's OWN CRT filter, ported verbatim. Ground truth: LJ applies the CRT look in a
// single host-side draw (PSO 9775 char-select / 9155 fight captures; PS hash
// c2860f630d5602cc47d194b29340d27f, DXIL SM6.4) sampling ONLY the resolved 1024x768 StF frame —
// no scanline texture, no cbuffer; the mask is computed analytically and is anchored to SOURCE
// texels: one scanline per Model 2 line (uv.y*384, quartic ramp, max 12.96% darkening), one
// grille stripe per source column (uv.x*512, ^8 ramp, max 1.68%), a Bayer 4x4 dither jittering
// the scanline phase (grid = 2048x1536), all 4x supersampled along the pixel derivative. LJ's
// quad also CROPS 16px off each side of the 1024-wide frame (UV x = 16/1024..1008/1024 — the
// active 992px Model 2 image); we do the same crop in the shader since our fullscreen triangle
// spans UV 0..1. Reverse-engineered 2026-07-26 from the PIX C++ exports (C:\temp\stf-lj-pix).
static const char LJ_CRT_PS_HLSL[] = R"(
Texture2D backBufferTexture : register(t0);
SamplerState backBufferSampler : register(s0);

float3 CrtTap(float2 uv)
{
	float3 c = backBufferTexture.Sample(backBufferSampler, uv).rgb;

	// Bayer 4x4 phase dither, keyed on a 2048x1536 grid (4x the 512x384 logical grid)
	static const float kBayer[16] = {
		 0.0,  8.0,  2.0, 10.0,
		12.0,  4.0, 14.0,  6.0,
		 3.0, 11.0,  1.0,  9.0,
		15.0,  7.0, 13.0,  5.0,
	};
	int xi = (int)(frac(round(uv.x * 2048.0) * 0.25) * 4.0);
	int yi = (int)(frac(round(uv.y * 1536.0) * 0.25) * 4.0);
	float dither = kBayer[yi * 4 + xi] / 255.0;

	// Horizontal scanlines: one per source line, quartic ramp clamped at phase 0.6
	float py = frac(dither + uv.y * 384.0);
	float m4 = min(py, 0.6);
	m4 *= m4; m4 *= m4;
	float scan = 1.0 - m4;

	// Vertical aperture grille: one stripe per source column, ^8 ramp (much subtler)
	float px = frac(uv.x * 512.0);
	float m8 = min(px, 0.6);
	m8 *= m8; m8 *= m8; m8 *= m8;
	float grille = saturate(1.0 - m8);

	return c * (scan * grille);
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
	// LJ's quad carries TEXCOORD x = 16/1024 .. 1008/1024; ours is 0..1, so remap first.
	float2 uvc = float2(0.015625 + uv.x * 0.96875, uv.y);
	float2 d = float2(ddx_coarse(uvc.x), ddy_coarse(uvc.y));
	float3 s = CrtTap(uvc) + CrtTap(uvc + 0.25 * d) + CrtTap(uvc + 0.5 * d) + CrtTap(uvc + 0.75 * d);
	return float4(s * 0.25, 1.0);
}
)";

bool RenderWindow::EnsureCrtResources()
{
	if (m_crtCompileFailed) return false;

	HRESULT hr = S_OK;
	wil::com_ptr<ID3DBlob> errors;
	if (!m_psCrt)
	{
		wil::com_ptr<ID3DBlob> code;
		hr = D3DCompile(LJ_CRT_PS_HLSL, sizeof(LJ_CRT_PS_HLSL) - 1, "lj_crt_ps", nullptr, nullptr,
			"main", "ps_5_0", 0, 0, code.put(), errors.put());
		if (SUCCEEDED(hr))
		{
			hr = m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, m_psCrt.put());
		}
	}

	if (SUCCEEDED(hr))
	{
		// LJ sizes its CRT target to the WINDOW (menu captures ran 1776x956, the unpaused capture
		// happened to be a 1920x1080 window), recomputing the game quad per frame. Do the same,
		// but clamp the target up to >= 1080p tall: below that the 384 scanlines alias into
		// broken segments (LJ never runs that small, so this divergence has no LJ counterpart).
		const float upscale = m_height < 1080 ? 1080.0f / m_height : 1.0f;
		const UINT targetW = static_cast<UINT>(m_width * upscale + 0.5f);
		const UINT targetH = static_cast<UINT>(m_height * upscale + 0.5f);
		if (!m_crtTex || m_crtTexW != targetW || m_crtTexH != targetH)
		{
			m_crtSRV.reset();
			m_crtRTV.reset();
			m_crtTex.reset();

			D3D11_TEXTURE2D_DESC td {};
			td.Width = targetW;
			td.Height = targetH;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = OUTPUT_FORMAT;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			hr = m_device->CreateTexture2D(&td, nullptr, m_crtTex.put());
			if (SUCCEEDED(hr))
			{
				hr = m_device->CreateRenderTargetView(m_crtTex.get(), nullptr, m_crtRTV.put());
			}
			if (SUCCEEDED(hr))
			{
				hr = m_device->CreateShaderResourceView(m_crtTex.get(), nullptr, m_crtSRV.put());
			}
			if (SUCCEEDED(hr))
			{
				m_crtTexW = targetW;
				m_crtTexH = targetH;
			}
		}
	}

	if (FAILED(hr))
	{
		m_crtCompileFailed = true;
		m_psCrt.reset();
		m_crtTex.reset();
		m_crtRTV.reset();
		m_crtSRV.reset();
		char b[512];
		_snprintf_s(b, _TRUNCATE, "[crt] shader setup failed hr=0x%08lX: %s\n", static_cast<unsigned long>(hr),
			errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no compiler output)");
		OutputDebugStringA(b);
		return false;
	}
	return true;
}

void RenderWindow::BlitGameFrame(ID3D11ShaderResourceView* src, bool alphaBlend)
{
	const FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	// Live setting: route the frame through LJ's CRT pass first (never on the alpha-blended
	// overlay layer — LJ applies the CRT once, to the completed composite).
	const bool useCrt = !alphaBlend && gGeneral.GetSettings()->m_stfCrtFilter && EnsureCrtResources();
	D3D11_VIEWPORT finalViewport = m_viewport;
	bool clearBackbuffer = m_requiresClear && !alphaBlend;
	if (useCrt)
	{
		// PASS 1 — replicate LJ: evaluate the CRT shader into the window-shaped intermediate
		// target, game quad = the aspect-corrected game rect (boxing baked into the frame as
		// black, from the clear). The target may be upscaled from the window (see
		// EnsureCrtResources), so scale the viewport with it.
		m_deviceContext->ClearRenderTargetView(m_crtRTV.get(), clearColor);
		m_deviceContext->OMSetRenderTargets(1, m_crtRTV.addressof(), nullptr);
		m_deviceContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		m_deviceContext->VSSetShader(m_vs.get(), nullptr, 0);
		const UINT crtOffsets[1] = { 0 };
		m_deviceContext->IASetInputLayout(m_inputLayout.get());
		m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		m_deviceContext->IASetVertexBuffers(0, 1, m_vb.addressof(), &m_vbStride, crtOffsets);
		m_deviceContext->PSSetShader(m_psCrt.get(), nullptr, 0);
		m_deviceContext->PSSetShaderResources(0, 1, &src);
		m_deviceContext->PSSetSamplers(0, 1, m_blitSampler.addressof());

		const float upscale = static_cast<float>(m_crtTexH) / m_height;
		D3D11_VIEWPORT crtViewport = m_viewport;
		crtViewport.TopLeftX *= upscale;
		crtViewport.TopLeftY *= upscale;
		crtViewport.Width *= upscale;
		crtViewport.Height *= upscale;
		m_deviceContext->RSSetViewports(1, &crtViewport);
		m_deviceContext->RSSetState(nullptr);
		m_deviceContext->Draw(3, 0);

		ID3D11ShaderResourceView* nullSrv = nullptr;
		m_deviceContext->PSSetShaderResources(0, 1, &nullSrv);

		// PASS 2 shows the finished frame 1:1 over the whole window (boxing already baked in).
		src = m_crtSRV.get();
		finalViewport.TopLeftX = 0.0f;
		finalViewport.TopLeftY = 0.0f;
		finalViewport.Width = static_cast<float>(m_width);
		finalViewport.Height = static_cast<float>(m_height);
		finalViewport.MinDepth = 0.0f;
		finalViewport.MaxDepth = 1.0f;
		clearBackbuffer = false;
	}

	if (clearBackbuffer)
	{
		m_deviceContext->ClearRenderTargetView(m_backBufferRTV.get(), clearColor);
	}

	m_deviceContext->OMSetRenderTargets(1, m_backBufferRTV.addressof(), nullptr);
	if (alphaBlend)
	{
		// StF layer composite (mirrors the LJ host): the 496x384 2D output carries a proper alpha
		// mask (HUD pixels a=255, empty a=0) and must be blended OVER the 1024x768 3D layer.
		static wil::com_ptr<ID3D11BlendState> s_alphaBlend;
		if (!s_alphaBlend)
		{
			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			m_device->CreateBlendState(&bd, s_alphaBlend.put());
		}
		m_deviceContext->OMSetBlendState(s_alphaBlend.get(), nullptr, 0xFFFFFFFF);
	}
	else
		m_deviceContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	m_deviceContext->VSSetShader(m_vs.get(), nullptr, 0);

	const UINT Offsets[1] = { 0 };
	m_deviceContext->IASetInputLayout(m_inputLayout.get());
	m_deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_deviceContext->IASetVertexBuffers(0, 1, m_vb.addressof(), &m_vbStride, Offsets);

	m_deviceContext->PSSetShader(m_ps.get(), nullptr, 0);
	m_deviceContext->PSSetShaderResources(0, 1, &src);
	// Bind an explicit linear-clamp sampler: the blit used to rely on whatever sampler the ImGui
	// backend left bound at s0 (same linear-clamp state, but by accident rather than by contract).
	// LJ's CRT pass uses the same filtering (MIN_MAG_MIP_LINEAR, clamp).
	m_deviceContext->PSSetSamplers(0, 1, m_blitSampler.addressof());

	m_deviceContext->RSSetViewports(1, &finalViewport);
	m_deviceContext->RSSetState(nullptr);

	m_deviceContext->Draw(3, 0);

	if (useCrt)
	{
		// Unbind the CRT frame SRV so next frame's pass-1 RTV bind doesn't hazard against it.
		ID3D11ShaderResourceView* nullSrv = nullptr;
		m_deviceContext->PSSetShaderResources(0, 1, &nullSrv);
	}
}

// The +0x98 pointer StF hands us can DANGLE (immediate resource release frees the object while the
// slot still points at it). QueryInterface both validates it IS a live ID3D12Resource AND AddRefs it,
// keeping it alive for the duration of the composite. Guarded so a truly-freed object just skips the
// frame instead of crashing. Returns a ref'd resource in *out (caller Releases) or false.
static bool SafeAcquireResource(ID3D12Resource* p, ID3D12Resource** out)
{
	*out = nullptr;
	if (!p) return false;
	__try
	{
		ID3D12Resource* r = nullptr;
		if (p->QueryInterface(__uuidof(ID3D12Resource), reinterpret_cast<void**>(&r)) == S_OK && r)
		{
			*out = r;
			return true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}
	return false;
}

// Composite a DX12-native texture (StF's output render target) onto the swapchain. StF renders in
// native D3D12, so its output has no D3D11 SRV; wrap the ID3D12Resource through 11on12 into a D3D11
// shader-resource, then reuse the D3D11 fullscreen-triangle blit. The wrap + SRV are cached by
// resource pointer (StF reuses the same RT across frames).
ID3D12Resource* GetStfRenderTarget(int index);
int GetStfRenderTargetCount();
int GetStfRenderTargetState(int index);
ID3D12Resource* GetStfResolveDst();

void RenderWindow::BlitDX12Texture(ID3D12Resource* texRaw)
{
	if (!m_d3d11on12) return;

	// (The old [rt-rb] every-600-frames readback diagnostic lived here. Removed 2026-07-26: the
	// black-screen investigation it served is long solved, and it crashed after window resizes —
	// it called GetDesc() on raw tracked-RT pointers, which include freed pre-resize backbuffers.)

	// *** TWO-LAYER HOST COMPOSITE (fight-capture diff, 2026-07-26) ***
	// The Model 2 3D scene (characters/arena) renders into the MSAA RT and is RESOLVED into the
	// 1024x768 RT — which StF's OWN 4-quad composite never consumes. In Lost Judgment the HOST
	// engine samples that resolved layer and composites it into the scene itself (fight capture:
	// LJ engine PSO 9155 samples the resolve dst; StF's 496x384 output is used as an alpha-masked
	// OVERLAY). Mirror that here: blit the 1024x768 3D layer first (opaque), then alpha-blend the
	// 496x384 2D output (HUD/tile layers; alpha mask verified: HUD a=255, empty a=0) over it.
	// PRIMARY SOURCE = texRaw (execute_info.output_texid -> cgs_tex -> sbgl_resource+0x98, resolved
	// by GameLoop): the module's own "completed frame" pointer — it does the double-buffer flip, so
	// it never points at the in-progress buffer. (Blitting "last resolve dst" instead flashed
	// mid-frame/intermediate passes during transitions.) The old "display_tex+0x98 is never rendered
	// to" note predates the render fixes and no longer holds. Fallbacks: the latest MSAA resolve dst,
	// then the 496x384 output (pre-first-resolve boot frames only).
	constexpr D3D12_RESOURCE_STATES kShaderRead =
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	ID3D12Resource* rt3d = texRaw ? texRaw : GetStfResolveDst();
	D3D12_RESOURCE_STATES st3d = kShaderRead;
	ID3D12Resource* rt2d = nullptr; D3D12_RESOURCE_STATES st2d = D3D12_RESOURCE_STATE_COMMON;
	const int nrt = GetStfRenderTargetCount();
	for (int i = 0; i < nrt; ++i)
	{
		ID3D12Resource* raw = GetStfRenderTarget(i);
		if (!raw) continue;
		// The tracked-RT list holds raw pointers and includes old swapchain backbuffers, which
		// DIE on window resize — validate + ref before touching (freed entries just get skipped).
		ID3D12Resource* r = nullptr;
		if (!SafeAcquireResource(raw, &r)) continue;
		const D3D12_RESOURCE_DESC d = r->GetDesc();
		r->Release();
		const D3D12_RESOURCE_STATES rs = static_cast<D3D12_RESOURCE_STATES>(GetStfRenderTargetState(i));
		// Only RTs StF leaves SHADER-READABLE (0xC0) can get an SRV.
		const bool readable = (rs & kShaderRead) != 0;
		if (readable && d.Width == 496 && d.Height == 384) { rt2d = raw; st2d = rs; }
	}
	{ static int s_srcLog = 0; if (s_srcLog < 12) { ++s_srcLog; char b[192];
		_snprintf_s(b, _TRUNCATE, "[blit] src: texRaw=%p resolveDst=%p rt2d=%p (texRaw==resolveDst:%d)\n",
			static_cast<void*>(texRaw), static_cast<void*>(GetStfResolveDst()),
			static_cast<void*>(rt2d), texRaw == GetStfResolveDst() ? 1 : 0); D3D12LogLine(b); } }
	if (!rt3d && !rt2d) return; // nothing shader-readable yet — skip (don't composite the black display_tex)

	// Per-layer 11on12 wrap + SRV cache (two live layers, headroom for RT churn).
	struct LayerCache
	{
		ID3D12Resource* tex = nullptr;
		wil::com_ptr<ID3D11Resource> wrapped;
		wil::com_ptr<ID3D11ShaderResourceView> srv;
	};
	static LayerCache s_layerCache[4];

	auto blitLayer = [&](ID3D12Resource* srcRt, D3D12_RESOURCE_STATES srcState, bool alphaBlend)
	{
		ID3D12Resource* tex = nullptr;
		if (!SafeAcquireResource(srcRt, &tex)) { D3D12LogLine("[blit] skip: src not a live resource\n"); return; }
		struct RelGuard { ID3D12Resource* r; ~RelGuard() { if (r) r->Release(); } } relGuard{ tex };

		LayerCache* c = nullptr;
		for (auto& e : s_layerCache) if (e.tex == tex) { c = &e; break; }
		if (!c)
		{
			for (auto& e : s_layerCache) if (!e.tex) { c = &e; break; }
			if (!c) { c = &s_layerCache[0]; c->srv.reset(); c->wrapped.reset(); }
			c->tex = tex;
			D3D11_RESOURCE_FLAGS flags{};
			flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			HRESULT hrw = m_d3d11on12->CreateWrappedResource(
				tex, &flags, srcState, srcState, IID_PPV_ARGS(c->wrapped.put()));
			char b[160];
			_snprintf_s(b, _TRUNCATE, "[blit] CreateWrappedResource hr=0x%08lX wrapped=%p tex=%p\n",
				static_cast<unsigned long>(hrw), static_cast<void*>(c->wrapped.get()), static_cast<void*>(tex));
			D3D12LogLine(b);
		}
		if (!c->wrapped) { c->tex = nullptr; return; }

		ID3D11Resource* wr[] = { c->wrapped.get() };
		m_d3d11on12->AcquireWrappedResources(wr, 1);
		if (!c->srv)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			sd.Format = tex->GetDesc().Format;
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MostDetailedMip = 0;
			sd.Texture2D.MipLevels = 1;
			m_device->CreateShaderResourceView(c->wrapped.get(), &sd, c->srv.put());
		}
		if (c->srv) BlitGameFrame(c->srv.get(), alphaBlend);
		m_d3d11on12->ReleaseWrappedResources(wr, 1);
	};

	// LJ ground truth (both captures, fight AND char select): the host samples ONLY the resolved
	// 1024x768 layer — it already contains the COMPLETE frame (3D scene + HUD sprites); the 496x384
	// output is never displayed (0 SRVs reference it in either LJ capture). Blitting both doubles
	// the HUD. Show the resolve layer alone; fall back to the 496x384 only before the first resolve.
	if (rt3d)      blitLayer(rt3d, st3d, false);
	else if (rt2d) blitLayer(rt2d, st2d, false);

}

void RenderWindow::NewImGuiFrame()
{
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
}

void RenderWindow::RenderImGui()
{
	m_ui.Draw();

	ImGui::Render();

	// CANARY (overlay invisible in the StF path, fine in VF2): confirm the overlay actually has
	// geometry to draw and a live render target. If vtx>0 and the RTV is non-null, ImGui really is
	// drawing and the loss is downstream (11on12 flush / present); if vtx==0 or DisplaySize is 0,
	// the ImGui frame itself is empty and the bug is in NewImGuiFrame/UI state, not in D3D.
	{
		static int s_n = 0;
		if (s_n < 6)
		{
			const ImDrawData* dd = ImGui::GetDrawData();
			const ImGuiIO& io = ImGui::GetIO();
			char b[224];
			_snprintf_s(b, _TRUNCATE,
				"[imgui-canary] valid=%d cmdLists=%d vtx=%d idx=%d display=%.0fx%.0f rtv=%p ctx=%p\n",
				dd ? dd->Valid : -1, dd ? dd->CmdListsCount : -1,
				dd ? dd->TotalVtxCount : -1, dd ? dd->TotalIdxCount : -1,
				io.DisplaySize.x, io.DisplaySize.y,
				static_cast<void*>(m_backBufferRTV.get()), static_cast<void*>(m_deviceContext.get()));
			OutputDebugStringA(b);
			s_n++;
		}
	}

	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

wil::com_ptr<IDXGISwapChain> RenderWindow::CreateSwapChainForWindow(ID3D11Device* device, HWND window)
{
	wil::com_ptr<IDXGISwapChain> swapChain;

	float refreshRate = gGeneral.GetSettings()->m_refreshRate;

	DXGI_SWAP_CHAIN_DESC swapChainDesc {};
	swapChainDesc.BufferDesc.RefreshRate.Numerator = static_cast<UINT>(refreshRate * 10000);
	swapChainDesc.BufferDesc.RefreshRate.Denominator = 10000;
	swapChainDesc.BufferDesc.Format = OUTPUT_FORMAT;
	swapChainDesc.SampleDesc.Count = 1;                             
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.OutputWindow = window;
	swapChainDesc.Windowed = TRUE;

	wil::com_ptr<IDXGIDevice> dxgiDevice;
	HRESULT hr = device->QueryInterface(IID_PPV_ARGS(dxgiDevice.addressof()));
	THROW_IF_FAILED(hr);

	wil::com_ptr<IDXGIAdapter> adapter;
	hr = dxgiDevice->GetAdapter(adapter.addressof());
	THROW_IF_FAILED(hr);

	wil::com_ptr<IDXGIFactory> factory;
	hr = adapter->GetParent(IID_PPV_ARGS(factory.addressof()));
	THROW_IF_FAILED(hr);


	// Try flip models, if it fails (because of an old Windows version), try a normal blit model
	for (auto swapEffect : {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_SWAP_EFFECT_DISCARD})
	{
		swapChainDesc.SwapEffect = swapEffect;
		hr = factory->CreateSwapChain(device, &swapChainDesc, swapChain.put());
		if (SUCCEEDED(hr))
		{
			break;
		}
	}
	THROW_IF_FAILED(hr);

	return swapChain;
}

void RenderWindow::CreateRenderResources()
{
	ID3D11Device* device = m_device.get();

	// Create shaders
	static const uint8_t vs_main[] =
	{
		 68,  88,  66,  67,   0, 158, 
		 39, 114, 142,  75, 234,  29, 
		 89, 242, 219,   6,  14, 240, 
		 60,  96,   1,   0,   0,   0, 
		 56,   2,   0,   0,   5,   0, 
		  0,   0,  52,   0,   0,   0, 
		128,   0,   0,   0, 212,   0, 
		  0,   0,  44,   1,   0,   0, 
		188,   1,   0,   0,  82,  68, 
		 69,  70,  68,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		 28,   0,   0,   0,   0,   4, 
		254, 255,   0, 129,   0,   0, 
		 28,   0,   0,   0,  77, 105, 
		 99, 114, 111, 115, 111, 102, 
		116,  32,  40,  82,  41,  32, 
		 72,  76,  83,  76,  32,  83, 
		104,  97, 100, 101, 114,  32, 
		 67, 111, 109, 112, 105, 108, 
		101, 114,  32,  49,  48,  46, 
		 49,   0,  73,  83,  71,  78, 
		 76,   0,   0,   0,   2,   0, 
		  0,   0,   8,   0,   0,   0, 
		 56,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  3,   0,   0,   0,   0,   0, 
		  0,   0,   3,   3,   0,   0, 
		 65,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  3,   0,   0,   0,   1,   0, 
		  0,   0,   3,   3,   0,   0, 
		 80,  79,  83,  73,  84,  73, 
		 79,  78,   0,  84,  69,  88, 
		 67,  79,  79,  82,  68,   0, 
		171, 171,  79,  83,  71,  78, 
		 80,   0,   0,   0,   2,   0, 
		  0,   0,   8,   0,   0,   0, 
		 56,   0,   0,   0,   0,   0, 
		  0,   0,   1,   0,   0,   0, 
		  3,   0,   0,   0,   0,   0, 
		  0,   0,  15,   0,   0,   0, 
		 68,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  3,   0,   0,   0,   1,   0, 
		  0,   0,   3,  12,   0,   0, 
		 83,  86,  95,  80,  79,  83, 
		 73,  84,  73,  79,  78,   0, 
		 84,  69,  88,  67,  79,  79, 
		 82,  68,   0, 171, 171, 171, 
		 83,  72,  68,  82, 136,   0, 
		  0,   0,  64,   0,   1,   0, 
		 34,   0,   0,   0,  95,   0, 
		  0,   3,  50,  16,  16,   0, 
		  0,   0,   0,   0,  95,   0, 
		  0,   3,  50,  16,  16,   0, 
		  1,   0,   0,   0, 103,   0, 
		  0,   4, 242,  32,  16,   0, 
		  0,   0,   0,   0,   1,   0, 
		  0,   0, 101,   0,   0,   3, 
		 50,  32,  16,   0,   1,   0, 
		  0,   0,  54,   0,   0,   5, 
		 50,  32,  16,   0,   0,   0, 
		  0,   0,  70,  16,  16,   0, 
		  0,   0,   0,   0,  54,   0, 
		  0,   8, 194,  32,  16,   0, 
		  0,   0,   0,   0,   2,  64, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		128,  63,   0,   0, 128,  63, 
		 54,   0,   0,   5,  50,  32, 
		 16,   0,   1,   0,   0,   0, 
		 70,  16,  16,   0,   1,   0, 
		  0,   0,  62,   0,   0,   1, 
		 83,  84,  65,  84, 116,   0, 
		  0,   0,   4,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   4,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  1,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  3,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0
	};
	HRESULT hr = device->CreateVertexShader(vs_main, sizeof(vs_main), nullptr, m_vs.addressof());
	THROW_IF_FAILED(hr);

	const D3D11_INPUT_ELEMENT_DESC inputElements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	hr = device->CreateInputLayout(inputElements, std::size(inputElements), vs_main, sizeof(vs_main), m_inputLayout.addressof());
	THROW_IF_FAILED(hr);

	static const uint8_t ps_main[] =
	{
		 68,  88,  66,  67, 106, 118, 
		 18, 107, 193, 156, 216,  42, 
		  8, 105, 148, 229, 179, 147, 
		188, 143,   1,   0,   0,   0, 
		 88,   2,   0,   0,   5,   0, 
		  0,   0,  52,   0,   0,   0, 
		228,   0,   0,   0,  60,   1, 
		  0,   0, 112,   1,   0,   0, 
		220,   1,   0,   0,  82,  68, 
		 69,  70, 168,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   2,   0,   0,   0, 
		 28,   0,   0,   0,   0,   4, 
		255, 255,   0, 129,   0,   0, 
		128,   0,   0,   0,  92,   0, 
		  0,   0,   3,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   1,   0, 
		  0,   0,   1,   0,   0,   0, 
		110,   0,   0,   0,   2,   0, 
		  0,   0,   5,   0,   0,   0, 
		  4,   0,   0,   0, 255, 255, 
		255, 255,   0,   0,   0,   0, 
		  1,   0,   0,   0,  13,   0, 
		  0,   0,  98,  97,  99, 107, 
		 66, 117, 102, 102, 101, 114, 
		 83,  97, 109, 112, 108, 101, 
		114,   0,  98,  97,  99, 107, 
		 66, 117, 102, 102, 101, 114, 
		 84, 101, 120, 116, 117, 114, 
		101,   0,  77, 105,  99, 114, 
		111, 115, 111, 102, 116,  32, 
		 40,  82,  41,  32,  72,  76, 
		 83,  76,  32,  83, 104,  97, 
		100, 101, 114,  32,  67, 111, 
		109, 112, 105, 108, 101, 114, 
		 32,  49,  48,  46,  49,   0, 
		 73,  83,  71,  78,  80,   0, 
		  0,   0,   2,   0,   0,   0, 
		  8,   0,   0,   0,  56,   0, 
		  0,   0,   0,   0,   0,   0, 
		  1,   0,   0,   0,   3,   0, 
		  0,   0,   0,   0,   0,   0, 
		 15,   0,   0,   0,  68,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   3,   0, 
		  0,   0,   1,   0,   0,   0, 
		  3,   3,   0,   0,  83,  86, 
		 95,  80,  79,  83,  73,  84, 
		 73,  79,  78,   0,  84,  69, 
		 88,  67,  79,  79,  82,  68, 
		  0, 171, 171, 171,  79,  83, 
		 71,  78,  44,   0,   0,   0, 
		  1,   0,   0,   0,   8,   0, 
		  0,   0,  32,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   3,   0,   0,   0, 
		  0,   0,   0,   0,  15,   0, 
		  0,   0,  83,  86,  95,  84, 
		 65,  82,  71,  69,  84,   0, 
		171, 171,  83,  72,  68,  82, 
		100,   0,   0,   0,  64,   0, 
		  0,   0,  25,   0,   0,   0, 
		 90,   0,   0,   3,   0,  96, 
		 16,   0,   0,   0,   0,   0, 
		 88,  24,   0,   4,   0, 112, 
		 16,   0,   0,   0,   0,   0, 
		 85,  85,   0,   0,  98,  16, 
		  0,   3,  50,  16,  16,   0, 
		  1,   0,   0,   0, 101,   0, 
		  0,   3, 242,  32,  16,   0, 
		  0,   0,   0,   0,  69,   0, 
		  0,   9, 242,  32,  16,   0, 
		  0,   0,   0,   0,  70,  16, 
		 16,   0,   1,   0,   0,   0, 
		 70, 126,  16,   0,   0,   0, 
		  0,   0,   0,  96,  16,   0, 
		  0,   0,   0,   0,  62,   0, 
		  0,   1,  83,  84,  65,  84, 
		116,   0,   0,   0,   2,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   2,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   1,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  1,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0, 
		  0,   0,   0,   0,   0,   0
	};
	hr = device->CreatePixelShader(ps_main, sizeof(ps_main), nullptr, m_ps.addressof());
	THROW_IF_FAILED(hr);

	// Create a vertex buffer
	{
		using namespace DirectX;

		static const struct
		{
			XMFLOAT2 Position;
			XMFLOAT2 Texcoord0;
		} vertexBuffer[] = {
			{ XMFLOAT2(-1.0f, -3.0f), XMFLOAT2(0.0f, 2.0f) },
			{ XMFLOAT2(-1.0f, 1.0f), XMFLOAT2(0.0f, 0.0f) },
			{ XMFLOAT2(3.0f, 1.0f), XMFLOAT2(2.0f, 0.0f) },
		};
		D3D11_BUFFER_DESC desc;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.ByteWidth = sizeof(vertexBuffer);
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;

		const D3D11_SUBRESOURCE_DATA initData { vertexBuffer };
		hr = device->CreateBuffer(&desc, &initData, m_vb.addressof());
		THROW_IF_FAILED(hr);

		m_vbStride = sizeof(vertexBuffer[0]);
	}

	// Linear-clamp sampler for the game blit (see the PSSetSamplers note in BlitGameFrame)
	{
		D3D11_SAMPLER_DESC desc {};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		hr = device->CreateSamplerState(&desc, m_blitSampler.addressof());
		THROW_IF_FAILED(hr);
	}
}

void RenderWindow::EnumerateDisplayModes()
{
	wil::com_ptr<IDXGIDevice> dxgiDevice;
	HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
	wil::com_ptr<IDXGIAdapter> adapter;
	hr = dxgiDevice->GetAdapter(adapter.addressof());
	THROW_IF_FAILED(hr);

	wil::com_ptr<IDXGIOutput> output;
	hr = adapter->EnumOutputs(0, output.addressof());
	THROW_IF_FAILED(hr);

	UINT numModes = 0;
	hr = output->GetDisplayModeList(OUTPUT_FORMAT, 0, &numModes, nullptr);
	THROW_IF_FAILED(hr);

	auto displayModes = std::make_unique<DXGI_MODE_DESC[]>(numModes);
	output->GetDisplayModeList(OUTPUT_FORMAT, 0, &numModes, displayModes.get());

	std::vector<std::tuple<uint32_t, uint32_t, float>> displayModesVector;
	for (auto* mode = displayModes.get(); mode < displayModes.get() + numModes; ++mode)
	{
		if (mode->ScanlineOrdering != DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE || mode->Scaling != DXGI_MODE_SCALING_UNSPECIFIED) continue;

		// We don't want resolutions smaller than 800x600, as the settings window is 600x600
		if (mode->Width < 800 || mode->Height < 600) continue;

		displayModesVector.push_back({mode->Width, mode->Height, static_cast<float>(mode->RefreshRate.Numerator) / mode->RefreshRate.Denominator});
	}

	// Refresh rates seem to be unsorted, fix it
	std::sort(displayModesVector.begin(), displayModesVector.end());
	for (const auto& mode : displayModesVector)
	{
		m_ui.AddResolution(std::get<0>(mode), std::get<1>(mode), std::get<2>(mode));
	}
}

void RenderWindow::CalculateViewport()
{
	if (m_fillViewport)
	{
		// "Fill Window": stretch the game over the whole backbuffer, no boxing.
		m_viewport.TopLeftX = 0;
		m_viewport.TopLeftY = 0;
		m_viewport.Width = static_cast<float>(m_width);
		m_viewport.Height = static_cast<float>(m_height);
		m_viewport.MinDepth = 0.0f;
		m_viewport.MaxDepth = 1.0f;
		m_requiresClear = false;
		return;
	}

	const float gameAR = m_gameAspect; // per-game (16:9 default; StF sets 4:3 via SetGameAspectRatio)
	const float windowAR = static_cast<float>(m_width) / m_height;
	if (gameAR >= windowAR)
	{
		// Letterbox - or nothing, if window is precisely 16:9
		const UINT targetHeight = static_cast<UINT>(m_width / gameAR);

		m_viewport.TopLeftX = 0;
		m_viewport.TopLeftY = (m_height - targetHeight) / 2;
		m_viewport.Width = m_width;
		m_viewport.Height = targetHeight;
	}
	else
	{
		// Pillarbox
		const UINT targetWidth = static_cast<UINT>(m_height * gameAR);

		m_viewport.TopLeftX = (m_width - targetWidth) / 2;
		m_viewport.TopLeftY = 0;
		m_viewport.Width = targetWidth;
		m_viewport.Height = m_height;
	}

	m_viewport.MinDepth = 0.0f;
	m_viewport.MaxDepth = 1.0f;

	m_requiresClear = m_viewport.TopLeftX != 0 || m_viewport.TopLeftY != 0;
}

// Defined in HostCdevice.cpp: register a resource for per-transition barrier logging (diagnostic).
void RegisterWatchResourceNow(ID3D12Resource* r);

void RenderWindow::CreateWrappedBackbuffers()
{
	// clean previous
	for (UINT i = 0; i < kBufferCount; ++i) {
		m_wrappedBackbuffers[i].reset();
		m_backbuffers[i].reset();
	}

	// fetch 12 backbuffers and wrap for 11
	for (UINT i = 0; i < kBufferCount; ++i) {
		THROW_IF_FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backbuffers[i])));

		D3D11_RESOURCE_FLAGS flags{};
		flags.BindFlags = D3D11_BIND_RENDER_TARGET;

		THROW_IF_FAILED(m_d3d11on12->CreateWrappedResource(
			m_backbuffers[i].get(),
			&flags,
			D3D12_RESOURCE_STATE_RENDER_TARGET,   // before DX11 uses it
			D3D12_RESOURCE_STATE_PRESENT,         // after DX11 work
			IID_PPV_ARGS(&m_wrappedBackbuffers[i])));

		// Watch these backbuffers in the ResourceBarrier hook so we can see the exact Acquire/Release
		// transition sequence (they persistently show COMMON at draw -> id=527/538 -> black present).
		RegisterWatchResourceNow(m_backbuffers[i].get());
	}

	// Make an RTV for the current buffer (0 for boot; we�ll update each frame)
	wil::com_ptr<ID3D11Resource> res = m_wrappedBackbuffers[0];
	THROW_IF_FAILED(m_device->CreateRenderTargetView(res.get(), nullptr, &m_backBufferRTV));
}

void RenderWindow::ResizeOn12(UINT w, UINT h)
{
	// ResizeBuffers fails on ANY outstanding backbuffer reference: drop the RTV + the 11on12
	// wrappers + our ID3D12Resource refs, flush whatever the D3D11 context still holds deferred,
	// and wait for the D3D12 queue to finish with the old buffers before resizing.
	m_backBufferRTV.reset();
	for (UINT i = 0; i < kBufferCount; ++i) {
		m_wrappedBackbuffers[i].reset();
		m_backbuffers[i].reset();
	}
	m_deviceContext->ClearState();
	m_deviceContext->Flush();

	if (!m_resizeFence)
	{
		THROW_IF_FAILED(m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_resizeFence.put())));
	}
	const uint64_t fenceValue = ++m_resizeFenceValue;
	THROW_IF_FAILED(m_cmdQueue->Signal(m_resizeFence.get(), fenceValue));
	if (m_resizeFence->GetCompletedValue() < fenceValue)
	{
		wil::unique_event gpuIdle(wil::EventOptions::None);
		THROW_IF_FAILED(m_resizeFence->SetEventOnCompletion(fenceValue, gpuIdle.get()));
		gpuIdle.wait();
	}

	// The closed m_bbBarrierList still holds a reference to the backbuffer it last transitioned;
	// reset it (safe now — the GPU is idle) so ResizeBuffers sees no outstanding refs.
	THROW_IF_FAILED(m_bbBarrierAlloc->Reset());
	THROW_IF_FAILED(m_bbBarrierList->Reset(m_bbBarrierAlloc.get(), nullptr));
	THROW_IF_FAILED(m_bbBarrierList->Close());

	const HRESULT hr = m_swapChain->ResizeBuffers(kBufferCount, w, h, OUTPUT_FORMAT, 0);
	{
		char b[128];
		_snprintf_s(b, _TRUNCATE, "[resize] ResizeBuffers %ux%u hr=0x%08lX\n", w, h, static_cast<unsigned long>(hr));
		D3D12LogWrite(b);
	}
	THROW_IF_FAILED(hr);
	CreateWrappedBackbuffers();
}

// Apply a window resize posted by WndProc: resize the swapchain to the new client size, rewrap
// the backbuffers for 11on12, and recompute the aspect-corrected game viewport. Runs on the
// render thread (top of BeginFrame), between frames, so no wrapped resource is acquired.
void RenderWindow::ApplyPendingResize()
{
	const uint64_t packed = m_pendingResize.exchange(0, std::memory_order_acquire);
	if (packed == 0) return;

	const UINT w = static_cast<UINT>(packed >> 32);
	const UINT h = static_cast<UINT>(packed & 0xFFFFFFFF);
	if (w == 0 || h == 0 || (w == m_width && h == m_height)) return;

	m_width = w;
	m_height = h;
	ResizeOn12(w, h);
	CalculateViewport();
}

// Issue the COMMON->RENDER_TARGET barrier d3d11on12 omits on Acquire, on m_cmdQueue. The queue serializes
// this ahead of 11on12's later flush (the composite/ImGui draws), so the backbuffer is RENDER_TARGET when
// they run. StF's Present leaves it back in PRESENT(==COMMON) for the next frame, so before is always 0x0.
void RenderWindow::TransitionBackbufferToRenderTarget(UINT idx)
{
	THROW_IF_FAILED(m_bbBarrierAlloc->Reset());
	THROW_IF_FAILED(m_bbBarrierList->Reset(m_bbBarrierAlloc.get(), nullptr));
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource   = m_backbuffers[idx].get();
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
	m_bbBarrierList->ResourceBarrier(1, &b);
	THROW_IF_FAILED(m_bbBarrierList->Close());
	ID3D12CommandList* lists[] = { m_bbBarrierList.get() };
	m_cmdQueue->ExecuteCommandLists(1, lists);
}

void RenderWindow::BeginFrame()
{
	// Between frames and before any wrapped-resource Acquire: safe point to follow a window resize.
	ApplyPendingResize();

	wil::com_ptr<IDXGISwapChain3> sc3;

	if (auto sc3 = m_swapChain.try_query<IDXGISwapChain3>()) {
		const UINT idx = sc3->GetCurrentBackBufferIndex();

		// CANARY (overlay never reaches the screen in the StF path, but does in VF2): the 11on12
		// wrapped backbuffers are created ONCE at startup. If the swapchain is resized/recreated
		// afterwards (StF's boot resizes the window), those wrappers reference orphaned buffers —
		// ImGui then draws into dead resources while Present shows the new, never-rendered ones.
		// Compare the LIVE swapchain buffer against the one we wrapped for a few frames.
		{
			static int s_chk = 0;
			if (s_chk < 6)
			{
				DXGI_SWAP_CHAIN_DESC sd{};
				sc3->GetDesc(&sd);
				ID3D12Resource* live = nullptr;
				const HRESULT gb = sc3->GetBuffer(idx, IID_PPV_ARGS(&live));
				char b[256];
				_snprintf_s(b, _TRUNCATE,
					"[bb-canary] idx=%u desc=%ux%u count=%u live=%p tracked=%p SAME=%d wrapped=%p gb=0x%08lX\n",
					idx, sd.BufferDesc.Width, sd.BufferDesc.Height, sd.BufferCount,
					static_cast<void*>(live), static_cast<void*>(m_backbuffers[idx].get()),
					(live == m_backbuffers[idx].get()) ? 1 : 0,
					static_cast<void*>(m_wrappedBackbuffers[idx].get()),
					static_cast<unsigned long>(gb));
				OutputDebugStringA(b);
				if (live) live->Release();
				s_chk++;
			}
		}

		// Put the backbuffer into RENDER_TARGET BEFORE handing it to 11on12 (which won't do it itself).
		TransitionBackbufferToRenderTarget(idx);
		ID3D11Resource* const res[] = { m_wrappedBackbuffers[idx].get() };
		m_d3d11on12->AcquireWrappedResources(res, 1);
		THROW_IF_FAILED(m_device->CreateRenderTargetView(res[0], nullptr, m_backBufferRTV.put()));
	}
}

void RenderWindow::EndFrame()
{
	// Prefer non-throwing try_query and early return.
	if (auto sc3 = m_swapChain.try_query<IDXGISwapChain3>()) {
		const UINT idx = sc3->GetCurrentBackBufferIndex();

		ID3D11Resource* const res[] = { m_wrappedBackbuffers[idx].get() };

		// Pair with AcquireWrappedResources from BeginFrame
		m_d3d11on12->ReleaseWrappedResources(res, 1);

		// Often not strictly required each frame, but OK if you need it.
		m_deviceContext->Flush();
	}
}