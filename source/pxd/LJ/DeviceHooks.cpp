#include "DeviceHooks.h"

#include <cstdio>

#include "../../wil/com.h"

#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"

// D3D12 debug-layer message sink: every validation message goes through DebugLogFile
// (DebugLog.h — debugger + d3d12_debug.log, flushed per line so the message emitted right
// before a driver crash is on disk). Registered via ID3D12InfoQueue1::RegisterMessageCallback
// when the debug layer is enabled. Debug builds only; compiles away in Release.
void CALLBACK D3D12DebugMessageCallback(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
	D3D12_MESSAGE_ID id, LPCSTR description, void* /*context*/)
{
	DebugLogFile("[sev=%d cat=%d id=%d] %s\n", severity, category, id, description ? description : "(null)");
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
static void* g_modulePsos[512] = {};
static int   g_modulePsoCount = 0;
bool IsModulePso(void* pso)
{
	if (!pso) return false;
	for (int i = 0; i < g_modulePsoCount; ++i) if (g_modulePsos[i] == pso) return true;
	return false;
}

// ---- DRED (Device Removed Extended Data) -----------------------------------------------------------
// The composite (reading StF's output texture through 11on12) removes the GPU device with an async
// fault that surfaces as a STATUS_BREAKPOINT deep in d3d12.dll at a varying location. DRED records GPU
// auto-breadcrumbs (which op the GPU last completed) + the page-fault VA and whether it hit a RECENTLY
// FREED allocation (which would confirm the immediate-release flag is freeing a still-in-use resource).
static ID3D12Device* g_dredDevice = nullptr;

static void DumpDRED(ID3D12Device* dev)
{
	if (!dev) return;
	wil::com_ptr<ID3D12DeviceRemovedExtendedData> dred;
	if (FAILED(dev->QueryInterface(IID_PPV_ARGS(dred.put()))) || !dred)
	{
		DebugLogFile("[DRED] no ID3D12DeviceRemovedExtendedData interface\n");
		return;
	}
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
	{
		int nodeN = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE* node = bc.pHeadAutoBreadcrumbNode;
			node && nodeN < 8; node = node->pNext, ++nodeN)
		{
			const UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			DebugLogFile("[DRED] cmdlist='%ls' queue='%ls' count=%u lastCompleted=%u\n",
				node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"?",
				node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"?",
				node->BreadcrumbCount, last);
			// Print the ops straddling the last completed one — that's where the GPU died.
			const UINT lo = last > 3 ? last - 3 : 0;
			const UINT hi = (last + 4 < node->BreadcrumbCount) ? last + 4 : node->BreadcrumbCount;
			for (UINT i = lo; i < hi; ++i)
			{
				DebugLogFile("[DRED]   op[%u]=%d%s\n", i,
					static_cast<int>(node->pCommandHistory[i]), i == last ? "  <== last GPU-completed" : "");
			}
		}
	}
	D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)))
	{
		DebugLogFile("[DRED] PageFaultVA=0x%llX\n", static_cast<unsigned long long>(pf.PageFaultVA));
		for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
		{
			DebugLogFile("[DRED]   EXISTING alloc '%ls' type=%d\n",
				a->ObjectNameW ? a->ObjectNameW : L"?", a->AllocationType);
		}
		for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
		{
			DebugLogFile("[DRED]   RECENTLY-FREED alloc '%ls' type=%d  <== USE-AFTER-FREE?\n",
				a->ObjectNameW ? a->ObjectNameW : L"?", a->AllocationType);
		}
	}
	DebugLogFile("[DRED] ---- end ----\n");
}

// Callable from HostCdevice's frame-submit path: dump DRED (last GPU op + page-fault resource) on a
// detected device-removal so the diagnosis survives even when nothing is attached to read
// OutputDebugString (DebugLogFile routes it to d3d12_debug.log).
void DumpDredNow() { DumpDRED(g_dredDevice); }

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
			DebugLogFile("[DRED] VEH exception code=0x%08lX at %p\n",
				static_cast<unsigned long>(code), ep->ExceptionRecord->ExceptionAddress);
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
				DebugLogFile(
					"[crash] AV #%d %s addr=0x%llX rip=0x%llX (%s +0x%llX) rsp=0x%llX\n",
					s_av, info[0] ? "WRITE" : "READ",
					static_cast<unsigned long long>(info[1]),
					static_cast<unsigned long long>(rip), mod,
					static_cast<unsigned long long>(rip - modBase),
					static_cast<unsigned long long>(ep->ContextRecord->Rsp));
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
						DebugLogFile("[crash]   stack[%d] -> %s+0x%llX\n", i, nm,
							static_cast<unsigned long long>(v - vb));
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
					DebugLog("[DRED] exception 0x%08lX at %p; deviceRemovedReason=0x%08lX\n",
						static_cast<unsigned long>(code), ep->ExceptionRecord->ExceptionAddress,
						static_cast<unsigned long>(drr));
					DumpDRED(g_dredDevice);
				}
			}
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// Return the hardware register SV_Position occupies in a shader's input (ISG1/ISGN) or output
// (OSG1/OSGN) signature, or -1 if absent/unparsable. D3D12 requires the VS output and PS input to
// agree on it; D3D11 did not, which is why pxd shader pairs can be legal there and rejected here
// (debug layer STATE_CREATION ERROR #660).
static int SignaturePositionRegister(const void* bc, size_t len, bool wantInput)
{
	const uint8_t* m = static_cast<const uint8_t*>(bc);
	if (!m || len < 0x24 || m[0] != 'D' || m[1] != 'X' || m[2] != 'B' || m[3] != 'C') return -1;
	const uint32_t nchunks = *reinterpret_cast<const uint32_t*>(m + 0x1C);
	const uint32_t* offs = reinterpret_cast<const uint32_t*>(m + 0x20);
	for (uint32_t i = 0; i < nchunks; ++i)
	{
		if (offs[i] + 8 > len) continue;
		const char* fc = reinterpret_cast<const char*>(m + offs[i]);
		const bool isIn = (fc[0] == 'I' && fc[1] == 'S' && fc[2] == 'G');   // ISGN / ISG1
		const bool isOut = (fc[0] == 'O' && fc[1] == 'S' && fc[2] == 'G');  // OSGN / OSG1
		if (wantInput ? !isIn : !isOut) continue;
		const uint8_t* p = m + offs[i] + 8;
		const uint32_t count = *reinterpret_cast<const uint32_t*>(p);
		constexpr size_t kElem = 32; // stream,nameOff,semIdx,sysVal,compType,reg,mask,rwMask,pad
		for (uint32_t e = 0; e < count; ++e)
		{
			const uint8_t* el = p + 8 + e * kElem;
			if (el + kElem > m + len) break;
			const uint32_t sysVal = *reinterpret_cast<const uint32_t*>(el + 12);
			if (sysVal == 1) // D3D_NAME_POSITION
				return static_cast<int>(*reinterpret_cast<const uint32_t*>(el + 20));
		}
	}
	return -1;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePipelineState(
	ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPSO)
{
	// Diagnostic stream dump — Debug builds only (YAMP_DEBUG_LOGGING, see DebugLog.h). In Release
	// psoLog is statically null, so every `if (psoLog...)` block below is dead code the optimizer
	// strips (no file, no strings).
#if YAMP_DEBUG_LOGGING
	static FILE* psoLog = nullptr;
	if (psoLog == nullptr)
	{
		fopen_s(&psoLog, "pso_stream.log", "w");
	}
#else
	FILE* const psoLog = nullptr;
#endif
	// Dump the stream BEFORE forwarding, so it is on disk even if the call faults.
	if (psoLog != nullptr && pDesc != nullptr)
	{
		// Caller RVA too: the module chooses its shader variant before this hook ever runs, so the
		// return address is the shortest route to the PSO builder that made the choice. Relative to
		// the loaded module because every pxd module is ASLR'd.
		const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
		HMODULE owner = nullptr;
		uintptr_t rva = 0;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
			| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(ra), &owner) && owner != nullptr)
		{
			rva = ra - reinterpret_cast<uintptr_t>(owner);
		}
		fprintf(psoLog, "=== CreatePipelineState SizeInBytes=%zu pStream=%p callerRVA=0x%llX ===\n",
			pDesc->SizeInBytes, pDesc->pPipelineStateSubobjectStream,
			(unsigned long long)rva);
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
			//
			// m2ftg ONLY. This was an StF black-screen fix (GBV id=679: a PSO with NumRenderTargets==0
			// discards every pixel the shader writes), where numRT==0 always meant "the template was
			// never filled in". The LJ VF5FS module legitimately builds depth-only pipelines —
			// numRT==0 with a D32_FLOAT DSV and a PS that writes only SV_Depth — and forcing a colour
			// target onto those is simply wrong.
			const bool kForceRenderTargetFormats =
				gGeneral.GetGameId() != YAMPGeneral::GameId::VF5FS_LJ &&
				gGeneral.GetGameId() != YAMPGeneral::GameId::VF5FS;
			if (kForceRenderTargetFormats && *numRT == 0)
			{
				*numRT = 1;
				if (rtFormats[0] == DXGI_FORMAT_UNKNOWN) rtFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
				if (psoLog != nullptr) fprintf(psoLog, "  [rtfmt] FORCED numRT=1 fmt=%d\n", rtFormats[0]);
			}
		}


		// --- VS/PS signature linkage check (VF5FS) -------------------------------------------------
		// The LJ VF5FS module's first pipeline pairs a full character-model VS (11 varyings, so DXIL
		// puts SV_Position in register 11) with a depth-only PS (its ONLY input is SV_Position, which
		// DXIL therefore assigns register 0). D3D11 ignored surplus VS outputs; D3D12 refuses to link
		// mismatched SV_Position registers (#660) -> E_INVALIDARG -> the module binds a null PSO and
		// SetPipelineState(null) faults inside D3D12Core.
		//
		// A pipeline with no render targets and a PS that only writes SV_Depth is a depth pass, and a
		// depth pass is legal with NO pixel shader at all. So when the two cannot link, drop the PS
		// and let the pipeline validate as depth-only. This is LOAD-BEARING: it fires exactly once
		// per run (VS out reg=11 vs PS in reg=0, numRT=0) and without it that pipeline fails to
		// create, the module binds a null PSO and SetPipelineState(null) faults. The dropped PS
		// samples t0, so a depth copy/reproject is presumably lost; revisit if artefacts appear.
		{
			const void* vsBc = *reinterpret_cast<void* const*>(s + 0x018);
			const size_t vsLen = *reinterpret_cast<const size_t*>(s + 0x020);
			void*& psBc = *reinterpret_cast<void**>(s + 0x030);
			size_t& psLen = *reinterpret_cast<size_t*>(s + 0x038);
			const uint32_t numRT = *reinterpret_cast<const uint32_t*>(s + 0x27C + 8 * sizeof(DXGI_FORMAT));
			if (vsBc && psBc)
			{
				const int vsPos = SignaturePositionRegister(vsBc, vsLen, /*wantInput*/false);
				const int psPos = SignaturePositionRegister(psBc, psLen, /*wantInput*/true);
				if (vsPos >= 0 && psPos >= 0 && vsPos != psPos)
				{
					static int s_logged = 0;
					if (s_logged < 4)
					{
						DebugLog("[pso-link] SV_Position VS out reg=%d vs PS in reg=%d, numRT=%u -> %s\n",
							vsPos, psPos, numRT, numRT == 0 ? "dropping PS (depth-only)" : "left alone");
						s_logged++;
					}
					if (numRT == 0)
					{
						psBc = nullptr;
						psLen = 0;
					}
				}
			}
		}

		// The ROOT_SIGNATURE subobject payload (stream+0x08) is null in YAMP (the host normally
		// creates the shared root signature; LJ's is non-null). D3D12 rejects a graphics PSO with no
		// root signature, so build the PIX-captured one below and stamp it in.
		uint64_t& rsSlot = *reinterpret_cast<uint64_t*>(s + 0x08);
		if (rsSlot == 0)
		{
			static ID3D12RootSignature* s_rootSig = nullptr;
			static bool s_rsTried = false;
			if (!s_rsTried)
			{
				s_rsTried = true;
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
					fprintf(psoLog, "  [rootsig] 10-param graphics sig shr=0x%08lX rhr=0x%08lX obj=%p %s\n",
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
	if (SUCCEEDED(hr) && ppPSO && *ppPSO && g_modulePsoCount < 512)
		g_modulePsos[g_modulePsoCount++] = *ppPSO; // mark as an StF PSO for injection gating
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
	// Heap-creation trace — Debug builds only (see psoLog note in HookedCreatePipelineState).
#if YAMP_DEBUG_LOGGING
	static FILE* f = nullptr;
	if (f == nullptr) fopen_s(&f, "heaps.log", "w");
#else
	FILE* const f = nullptr;
#endif
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

// One installer for the whole layer, called by the RenderWindow constructor once the real
// device exists: the DRED breakpoint handler needs the device to dump against, and the
// CreatePipelineState hook is installed on the real (layer-off) device unconditionally so the
// failing PSO's transient subobject stream is always captured. Cheap: two vtable slot swaps.
void InstallDeviceHooks(ID3D12Device* device)
{
	g_dredDevice = device;
	AddVectoredExceptionHandler(1, &DredVeh);
	InstallCreatePipelineStateHook(device);
}
