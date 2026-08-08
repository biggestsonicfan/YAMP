#include "Pre3Host.h"

// Defined in pxd/LJ/HostCdevice.cpp — the same four per-frame host duties every LJ-generation
// DX12 host has. See LJHost.cpp for what each of them fixes; nothing about them is m2ftg-specific.
void SetModuleRenderActiveNow(bool active);
void SubmitModuleFrameListNow();
bool ModuleExecDisabledNow();
void AdvanceFrameStampNow();
// Draw isolation, for chasing a rendering artifact down to the draw that makes it. See the note
// on g_drawLimit in pxd/LJ/HostCdevice.cpp.
void SetModuleDrawLimitNow(unsigned int limit);
unsigned int ModuleDrawsLastFrameNow();
unsigned int ModuleDrawLimitNow();

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <filesystem>
#include <string>
#include <algorithm>

#include "../../wil/resource.h"

#include "../../criware/Cri.h"

#include "../../pxd/LJ/sl.h"
#include "../../pxd/LJ/gs.h"
#include "../../pxd/LJ/PatchGs.h"
#include "../../pxd/LJ/HostCdevice.h"
#include "../../pxd/Imports.h"

#include "../pre3.h"
#include "../ImportSymbols.h"
#include "../SystemSwitches.h"
#include "../BoardVtables.h"
#include "../SecurityBoard.h"
#include "../CommBoard.h"
#include "../Determinism.h"
#include "../NetSession.h"

#include "../../input/Input.h"
// Presentation helpers only (aspect ratio + the Escape pause shell). They live in the m2ftg
// namespace because that is where they were first needed, but neither touches anything m2ftg —
// see m2ftg/HostUI.h.
#include "../../m2ftg/HostUI.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../DebugLog.h"
#include "../../Utils/ScopedUnprotect.hpp"

namespace pre3
{
	using namespace pxd;

	// ct::initialize_module (DLL 0x1800AC310) requires size_of_struct == 0x30, exactly as the
	// m2ftg modules do.
	struct ct_context_t
	{
		uint32_t tag_id;
		uint32_t version;
		uint32_t size_of_struct = 0x30;
		std::byte pad[36]{};
	};
	static_assert(sizeof(ct_context_t) == 0x30);

	static ct_context_t* g_ct_context = new ct_context_t;

	// ---- sl::kernel_calloc, rebuilt ---------------------------------------------------------
	//
	// pre3 has no sl::kernel_calloc (nothing in the module calls it, so it was linked out — see
	// Gaiden/ImportSymbols.cpp), but pxd's sl::initialize and sl::handle_initialize do. Rather
	// than hand those two a host heap — which would put the sl context's temp-work and handle
	// buffers outside the allocator the module itself uses — this reproduces the wrapper's body
	// over the module's own allocator, which IS present.
	//
	// The body is the StF build's verbatim: allocate, then zero when the caller asked for it.
	static void* (*s_kernel_alloc)(uint64_t bytes, uint32_t flags);

	static void* KernelCallocShim(uint64_t bytes, uint32_t flags)
	{
		void* p = s_kernel_alloc(bytes, flags);
		// Bit 3 is passed THROUGH to the allocator as well (it tests it too, for its own
		// large-alignment path); the zero-fill is in addition to that, not instead of it.
		if (p != nullptr && (flags & 8) != 0)
		{
			memset(p, 0, static_cast<size_t>(bytes));
		}
		return p;
	}

	// ---- gs::ib_create, rebuilt over the module's own vb_create ------------------------------
	//
	// pre3 has no ib_create (in Gaiden that lives in the host executable), but it DOES consume the
	// shared index buffers — the render pass faults reading `gs+0x8218` = LJ 0x7670 = p_ib_quad.
	// So the host has to supply them.
	//
	// Rather than hand-build the object and its D3D12 resource, this drives the module's own
	// vb_create, which survives in this build. That is sound rather than a trick, for two reasons
	// established by diffing the two constructors in the Gaiden Sonic the Fighters build (same pxd
	// generation, where both exist):
	//
	//   1. They share an IDENTICAL header. Both write alive=1 at +0x00, pack their own address
	//      into the low 48 bits of +0x10 (the state-cache id the device context compares at
	//      dc+0x13C20), and build the buffer sub-descriptor at +0x18 through the SAME call, with
	//      the same `usage == 0x210000` branch. They diverge only past +0xA8, and vb allocates
	//      0xD0 where ib allocates 0xC0 — so an ib-shaped object in a vb allocation cannot overrun.
	//   2. D3D12 buffers are typeless: there is no BIND_INDEX_BUFFER, so the resource itself is
	//      already valid to bind as an index buffer.
	//
	// The usage word is passed straight through to that shared sub-descriptor call, so handing
	// vb_create the INDEX usage (0x201, not the vertex 0x101) is what makes the resource state
	// index-buffer-correct. Only the tail then needs rewriting into ib form.
	//
	// fvf 0xE00003 has a 16-byte stride (the same one cgs_up_pool::initialize uses), and every
	// buffer primitive_initialize asks for is a multiple of 16 bytes (quad 0x900, fan/rect 0x600),
	// so vertices = bytes/16 makes +0x0C (= vertices * stride) exactly the byte size ib_create
	// would have written.
	inline constexpr uint64_t IB_SHIM_FVF = 0xE00003u;
	inline constexpr unsigned int IB_SHIM_STRIDE = 16;

	static cgs_ib* HostIbCreate(unsigned int indices, gs::ib_usage_t usage, unsigned int flags,
		const void* p_initial_data, const char* sz_name)
	{
		const unsigned int bytes = indices * 2u;
		if (gs::vb_create == nullptr || bytes == 0 || (bytes % IB_SHIM_STRIDE) != 0)
		{
			DebugLogFile("[%s ib] refusing %u indices (%u bytes): not a whole number of %u-byte "
				"vertices\n", gGeneral.GetGameTag(), indices, bytes, IB_SHIM_STRIDE);
			return nullptr;
		}

		auto* obj = reinterpret_cast<uint8_t*>(gs::vb_create(IB_SHIM_FVF, bytes / IB_SHIM_STRIDE,
			static_cast<gs::vb_usage_t>(usage), flags, p_initial_data, sz_name));
		if (obj == nullptr) return nullptr;

		// Rewrite vb's tail as ib's. Cleared first so the fields vb writes and ib leaves zero
		// (+0xB4 stride, +0xC0/+0xC4) do not survive as garbage the index path might read.
		memset(obj + 0xA8, 0, 0xC0 - 0xA8);
		*reinterpret_cast<uint32_t*>(obj + 0xA8) = indices;      // ib: index count
		*reinterpret_cast<uint32_t*>(obj + 0xAC) = 0xFFFFFFFFu;  // ib: written literally
		*reinterpret_cast<uint32_t*>(obj + 0xB0) = flags;        // ib: creation flags
		return reinterpret_cast<cgs_ib*>(obj);
	}



	// Loaded module base; defined below next to gameDll, forward-declared for the probes above it.
	static const uint8_t* ModuleBaseBytes();

	// ---- the pxd SYSTEM SHADER SET -----------------------------------------------------------
	//
	// pre3 carries pxd's built-in shader library — sys_2d, sys_vtx, sys_nvtx, sys_simple_skin,
	// the FSR passes, 52 entries — as a table of {SLLZ descriptor, name} pairs. NOTHING in the
	// module references that table, its entries, or its name strings: it is inert data, so the
	// HOST is what loads it. That is the same division of labour as gs::ib_create and
	// gs::primitive_initialize, both of which live in Like a Dragon Gaiden's executable.
	//
	// Consequence for YAMP: the module's composite pass wants `sys_2d` (VS 4299 / PS 3965 in a
	// native capture) and we never create it, so it falls back to a shader it does have and the
	// frame comes out very dark. See the notes in the project memory.
	//
	// Addressed by RVA rather than byte pattern. The two create wrappers are byte-identical for
	// 72 bytes (they differ only in wildcarded rip displacements) so a pattern cannot tell them
	// apart, and the table has no code reference to anchor one to at all. GameVerify has already
	// pinned this exact build by SHA-256 before any of this runs, which is the same justification
	// the m2ftg DwGame RVA tables use. Byte patterns for the three functions that ARE unique are
	// kept in the comments so a future build can be re-derived rather than re-discovered.
	namespace sysshader
	{
		// FUN_1800480C0 — reads an 'SLLZ' header and returns a pxd::csl_buffer whose decompressed
		// payload is at +0x20. Pattern: 40 53 48 83 EC 30 81 39 53 4C 4C 5A 48 8B D9 0F 85 ...
		inline constexpr uintptr_t RVA_SLLZ_LOAD = 0x480C0;
		// Crit-section wrappers around the per-stage create. Both are byte-identical for 72 bytes
		// (they differ only in the wildcarded rip displacement of the inner call), which is why
		// these are RVAs: FUN_18007EC90 -> FUN_18007E4E0 registers into handle_vs, and
		// FUN_18007F7C0 -> FUN_18007EF90 -> FUN_18007ED90 registers into handle_ps.
		inline constexpr uintptr_t RVA_VS_CREATE = 0x7EC90;
		inline constexpr uintptr_t RVA_PS_CREATE = 0x7F7C0;

		// The decompressed container's magic names the stage, so nothing has to be inferred:
		// 'GSVS' vertex, 'GSPS' pixel, 'GSCS' compute. The creates check it themselves too —
		// FUN_18007E4E0 rejects anything that is not 'GSVS' — so a mis-dispatch fails safely.
		// Little-endian: 'G'=0x47 'S'=0x53 then the stage letter then 'S'=0x53.
		inline constexpr uint32_t MAGIC_VS = 0x53565347; // "GSVS"  (as checked by FUN_18007E4E0)
		inline constexpr uint32_t MAGIC_PS = 0x53505347; // "GSPS"
		inline constexpr uint32_t MAGIC_CS = 0x53435347; // "GSCS"
		// FUN_1800486D0 — release the csl_buffer.
		// Pattern: 48 89 5C 24 08 57 48 83 EC 20 8B 51 68 48 8B D9 48 8B 05 ...
		inline constexpr uintptr_t RVA_BUFFER_RELEASE = 0x486D0;
		// The {descriptor, name} table and its length.
		inline constexpr uintptr_t RVA_TABLE = 0xF9010;
		inline constexpr size_t TABLE_ENTRIES = 52;

		struct Entry { const void* descriptor; const char* name; };

		using load_fn_t = void* (*)(const void* sllzDescriptor);
		using create_fn_t = uint32_t (*)(const void* gsvsData, uint32_t flags);
		using release_fn_t = void (*)(void* cslBuffer);
	}

	// Walk the table with the module's own loader and report what each entry decompresses to.
	// Diagnostic first on purpose: the table records no shader STAGE, and inferring it from
	// decompressed sizes proved unreliable (the GSVS overhead is not constant — 384 bytes on two
	// vertex shaders but 378 on a pixel one), so the stage has to be read out of the real
	// container rather than guessed before anything is created.
	static void LoadSystemShaders();


	static void ImportFunctions(const Imports& symbols)
	{
		auto Import = [&symbols](auto& var, auto symbol)
			{
				var = static_cast<std::decay_t<decltype(var)>>(symbols.GetSymbol(symbol));
			};

		Import(sl::sm_context, ImportSymbol::SL_CONTEXT_INSTANCE);
		Import(gs::sm_context, ImportSymbol::GS_CONTEXT_INSTANCE);
		Import(sl::file_create_internal, ImportSymbol::SL_FILE_CREATE);
		Import(sl::file_open_internal, ImportSymbol::SL_FILE_OPEN);
		Import(sl::file_read, ImportSymbol::SL_FILE_READ);
		Import(sl::file_close, ImportSymbol::SL_FILE_CLOSE);
		Import(sl::handle_create_internal, ImportSymbol::SL_HANDLE_CREATE);
		Import(sl::file_handle_destroy, ImportSymbol::SL_FILE_HANDLE_DESTROY);
		Import(sl::archive_lock_wlock, ImportSymbol::ARCHIVE_LOCK_WLOCK);
		Import(sl::archive_lock_wunlock, ImportSymbol::ARCHIVE_LOCK_WUNLOCK);
		Import(cgs_device_context::reset_state_all_internal, ImportSymbol::DEVICE_CONTEXT_RESET_STATE_ALL);
		Import(gs::vb_create, ImportSymbol::VB_CREATE);

		// sl::kernel_calloc does not exist in this module; point pxd's hook at the reconstruction
		// above, over the module's own allocator.
		s_kernel_alloc = reinterpret_cast<decltype(s_kernel_alloc)>(
			symbols.GetSymbol(ImportSymbol::SL_KERNEL_ALLOC));
		sl::kernel_calloc_internal = &KernelCallocShim;

		// ib_create is absent from this module by construction (see Gaiden/ImportSymbols.cpp), but
		// the module still CONSUMES the shared index buffers, so the host supplies its own over
		// the module's vb_create — see HostIbCreate. Prefer a real one if a future build has it.
		if (void* moduleIbCreate = symbols.TryGetSymbol(ImportSymbol::IB_CREATE))
		{
			gs::ib_create = reinterpret_cast<decltype(gs::ib_create)>(moduleIbCreate);
		}
		else
		{
			gs::ib_create = &HostIbCreate;
			DebugLogFile("[%s] ib_create absent; using the host shim over vb_create\n",
				gGeneral.GetGameTag());
		}
	}

	static void PrefillVariables(const Imports& symbols, const RenderWindow& window)
	{
		auto Import = [&symbols](auto& var, auto symbol)
			{
				var = static_cast<std::decay_t<decltype(var)>>(symbols.GetSymbol(symbol));
			};

		gs::context_t** ppContext;
		Import(ppContext, ImportSymbol::GS_CONTEXT_PTR);
		*ppContext = gs::sm_context;

		// As on every LJ-generation DX12 path: "D3DDEVICE" is cdevice_common::g_pD3DDevice, a
		// slot the renderer treats as the pxd host cdevice OBJECT, not as an ID3D12Device.
		void** ppDevice12;
		Import(ppDevice12, ImportSymbol::D3DDEVICE);

		using cdevice_ctor_fn = void* (*)(void*);
		auto cdeviceCtor = reinterpret_cast<cdevice_ctor_fn>(symbols.GetSymbol(ImportSymbol::CDEVICE_CTOR));
		*ppDevice12 = BuildHostCdevice(window.GetD3D12Device(), window.GetD3D12Queue(), cdeviceCtor);
	}

	static bool ResolveSymbolsAndInstallPatches(void* dll, const RenderWindow& window) try
	{
		// The module is ASLR'd (DYNAMIC_BASE), so the hooks' return-address checks need its real
		// load range rather than 0x180000000.
		SetGameDllRange(dll);

		// pre3 is the SAME pxd generation as Like a Dragon Gaiden's Sonic the Fighters module —
		// its own initialize_module size gates ask for sl 0xF140 and gs 0x3898C0, which is the
		// Gaiden pair. There is only one pre3 build, so this is unconditional rather than
		// timestamp-driven the way m2ftg's is; GameVerify has already refused anything else.
		gs::sm_is_gaiden_layout = true;
		gs::sm_dc = gs::DC_OFFSETS_GAIDEN;

		const Imports symbolMap = BuildSymbolMap(dll);
		if (const char* missing = RequiredButMissing(symbolMap); missing[0] != '\0')
		{
			DebugLogFile("[%s] unresolved symbols: %s\n", gGeneral.GetGameTag(), missing);
			return false;
		}

		const ScopedUnprotect::Section text(static_cast<HMODULE>(dll), ".text");
		const ScopedUnprotect::Section rdata(static_cast<HMODULE>(dll), ".rdata");

		ImportFunctions(symbolMap);
		PrefillVariables(symbolMap, window);

		if (sl::sm_context && sl::sm_context->handles.p_handle_buffer == nullptr)
		{
			constexpr uint32_t kHandleCapacity = 0x100000;

			const int ic = sl::initialize(0xF140);
			if (ic != 0) { return false; }

			const int rc = sl::handle_initialize(kHandleCapacity);
			if (rc != 0) { return false; }
		}

		PatchSl(sl::sm_context);
		gs::with_tail([&window](auto* ctx) { PatchGs(ctx, window); });

		// gs::context_t::backbuffer_count (context+0x70). PatchGs has never written it — no m2ftg
		// or VF5FS module reads it — but pre3's screen object DOES: its constructor
		// (DLL 0x180014760) sizes its ring of 0x48000-byte scroll buffers with
		//
		//     count = min(gs->backbuffer_count, 3)          // cmova — clamped ABOVE only
		//
		// and then fills `count` of them. Zero is therefore not a harmless default here: the fill
		// loop runs zero times, the ring stays null, and the first frame that presents memcpys
		// 0x120000 bytes into a null slot.
		//
		// Set here rather than in PatchGs on purpose. Writing it there would change the value
		// every OTHER host hands its module — all of which currently see zero and work — for no
		// benefit to them, and this is the one module that reads it.
		//
		// Two, to match the swap chain YAMP actually creates (RenderWindow::kBufferCount).
		gs::with_tail([](auto* ctx) { ctx->backbuffer_count = 2; });

		// Host redirects into the board's own vtables: the cabinet TEST / SERVICE switches, and
		// the real-time clock a netplay session pins. Both need the .rdata unprotect above and
		// must run before module_start constructs the board.
		//
		// The vtables are located ONCE and handed to both, because locating them keys on slot 0 -
		// which is exactly what InstallSystemSwitches replaces. Letting each installer find them
		// itself would make the second one silently find nothing.
		// The two board-side addresses everything else is expressed against, resolved by pattern
		// rather than by RVA. Handed over BEFORE module_start, because the first thing that reads
		// the machine object is the netplay install below it.
		//
		// Both are OPTIONAL in the import table and null-tolerant here: the RVA fallback is right
		// for the build GameVerify pins, so a pattern that stops matching must not stop the game
		// booting - it must only stop being silent about it, which SetBoardSymbols logs.
		SetBoardSymbols(symbolMap.TryGetSymbol(ImportSymbol::MACHINE_OBJECT),
			symbolMap.TryGetSymbol(ImportSymbol::CXCOMM_VTABLE));

		if (void* const readPort = symbolMap.TryGetSymbol(ImportSymbol::IO_READ_PORT))
		{
			void* vtables[MAX_BOARD_VTABLES];
			const size_t count = FindBoardVtables(dll, readPort, vtables, MAX_BOARD_VTABLES);
			InstallDeterministicClock(vtables, count);
			InstallSystemSwitches(vtables, count);
			// Third rider on the same list. Inert unless YAMP_PRE3_SECURITY asks for it, and
			// even then it only matters once the game's own protection hooks are disabled -
			// with them applied the guest never touches the security registers at all.
			InstallSecurityBoard(vtables, count);
		}
		else
		{
			DebugLog("[%s] No board I/O accessor - Test / Service and the clock pin are "
				"unavailable.\n", gGeneral.GetGameTag());
		}

		// The pxd system shader set. MUST precede module_start: the module refers to these by
		// handle id and ids are assigned in creation order, so they have to be created before the
		// module creates any of its own. See LoadSystemShaders.
		LoadSystemShaders();

		return true;
	}
	catch (...)
	{
		const std::wstring str(L"Failed to resolve imports and/or patch " + std::wstring(DLL_NAME) +
			L"!\n\nIt's either not a valid arcade module DLL from Like a Dragon Gaiden, "
			L"or the game has been updated and YAMP is not forward compatible with that new version.");
		MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
		return false;
	}

	static std::filesystem::path gamePath;
	static wil::unique_hmodule gameDll;

	static const uint8_t* ModuleBaseBytes()
	{
		return gameDll ? reinterpret_cast<const uint8_t*>(gameDll.get()) : nullptr;
	}

	// Create the whole pxd system shader set, in table order, using the module's own SLLZ loader
	// and per-stage create wrappers.
	//
	// ORDER AND TIMING MATTER. The module cannot look these up by name — nothing references the
	// name strings — so it can only refer to them by HANDLE ID, and ids are handed out purely in
	// creation order (the table allocator returns index + 1). In Gaiden the host creates these at
	// start-up, so they occupy the low ids; in YAMP nothing preceded the module and its own
	// shaders took ids 1..5 instead. Hence this runs before module_start, right after PatchGs has
	// stood the gs handle tables up.
	static void LoadSystemShaders()
	{
		const uint8_t* base = ModuleBaseBytes();
		if (base == nullptr) return;

		auto load = reinterpret_cast<sysshader::load_fn_t>(
			const_cast<uint8_t*>(base + sysshader::RVA_SLLZ_LOAD));
		auto release = reinterpret_cast<sysshader::release_fn_t>(
			const_cast<uint8_t*>(base + sysshader::RVA_BUFFER_RELEASE));
		auto vsCreate = reinterpret_cast<sysshader::create_fn_t>(
			const_cast<uint8_t*>(base + sysshader::RVA_VS_CREATE));
		auto psCreate = reinterpret_cast<sysshader::create_fn_t>(
			const_cast<uint8_t*>(base + sysshader::RVA_PS_CREATE));
		const auto* tbl = reinterpret_cast<const sysshader::Entry*>(base + sysshader::RVA_TABLE);

		unsigned vs = 0, ps = 0, skipped = 0, failed = 0;
		for (size_t i = 0; i < sysshader::TABLE_ENTRIES; i++)
		{
			const sysshader::Entry& e = tbl[i];
			if (e.descriptor == nullptr || e.name == nullptr) break;

			void* buf = load(e.descriptor);
			if (buf == nullptr) { failed++; continue; }

			const auto* data = *reinterpret_cast<const uint8_t* const*>(
				reinterpret_cast<const uint8_t*>(buf) + 0x20);
			if (data != nullptr)
			{
				const uint32_t magic = *reinterpret_cast<const uint32_t*>(data);
				uint32_t id = 0;
				if (magic == sysshader::MAGIC_VS) { id = vsCreate(data, 0); if (id) vs++; else failed++; }
				else if (magic == sysshader::MAGIC_PS) { id = psCreate(data, 0); if (id) ps++; else failed++; }
				else { skipped++; }   // GSCS compute (the FSR passes) — FV2 does not use them
			}
			release(buf);
		}
		DebugLogFile("[sysshader] loaded %u VS + %u PS (%u compute skipped, %u failed)\n",
			vs, ps, skipped, failed);
	}




	HMODULE pre3::LoadDLL()
	{
		{
			DWORD dwSize = GetCurrentDirectoryW(0, nullptr);
			auto buf = std::make_unique<wchar_t[]>(dwSize);
			GetCurrentDirectoryW(dwSize, buf.get());
			gamePath.assign(buf.get());
		}

		std::filesystem::path dllFile = gamePath / DLL_NAME;
		if (!std::filesystem::is_regular_file(dllFile))
		{
			gamePath.append(DLL_SUBDIR);
			dllFile = gamePath / DLL_NAME;
		}

		if (!std::filesystem::is_regular_file(dllFile))
		{
			const std::wstring str(L"Could not load " + std::wstring(DLL_NAME) +
				L"!\n\nMake sure that YAMP.exe is located next to the DLL file or its \"" +
				DLL_SUBDIR + L"\" subdirectory contains it.");
			MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
			return nullptr;
		}

		gGeneral.SetDLLName(WcharToUTF8(DLL_NAME));

		if (!Verify::CheckBeforeLoad(gGeneral.GetGameId(), dllFile))
		{
			return nullptr;
		}

		gameDll.reset(LoadLibraryW(dllFile.c_str()));
		if (!gameDll)
		{
			const std::wstring str(L"Could not load " + std::wstring(DLL_NAME) +
				L"!\n\nThe file exists but Windows refused to load it.");
			MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
		}

		return gameDll.get();
	}

	void pre3::PreInitialize()
	{
		gGeneral.SetDataPath();
		gGeneral.LoadSettings();
	}

	void pre3::Run(RenderWindow& window)
	{
		const auto module_start = reinterpret_cast<module_func_t>(GetProcAddress(gameDll.get(), "module_start"));
		THROW_LAST_ERROR_IF_NULL(module_start);
		const auto module_stop = reinterpret_cast<module_func_t>(GetProcAddress(gameDll.get(), "module_stop"));
		THROW_LAST_ERROR_IF_NULL(module_stop);
		ModuleEntries entries;

		if (!ResolveSymbolsAndInstallPatches(gameDll.get(), window))
		{
			DebugLog("[%s::Run] ResolveSymbolsAndInstallPatches FAILED\n", gGeneral.GetGameTag());
			return;
		}
		DebugLog("[%s::Run] patches installed OK\n", gGeneral.GetGameTag());

		Cri criware;
		// pre3 is a 2024-03-10 build — NEWER than the Gaiden Sonic the Fighters module that first
		// needed this, so it is built against at least that CRIWARE and its icri has the same
		// extra virtual at slot 9. UNVERIFIED for this specific module: the slot sweep that
		// established it for StF has not been re-run here. If the module faults inside Cri::free
		// (or any icri call lands one method off) while loading image/sound/m3e.acf, this line is
		// the first thing to re-measure — see Cri.h.
		criware.UseGaidenVtable();

		struct sl_module_t
		{
			size_t size = sizeof(decltype(*this));
			sl::context_t* context;
		} sl_module;
		sl_module.context = sl::sm_context;

		struct gs_module_t
		{
			size_t size = sizeof(decltype(*this)); // gs::initialize_module requires 0x58
			gs::context_t* context;
			uint8_t pad[72];
		} gs_module;
		static_assert(sizeof(gs_module_t) == 0x58);
		gs_module.context = gs::sm_context;

		const struct ct_module_t
		{
			size_t size = sizeof(decltype(*this));
			ct_context_t* context = g_ct_context;
		} ct_module;

		// The params block. Identical to m2ftg's through +0x38 — module_start reads sl/gs/ct/cri
		// from exactly those slots and stores the callback at +0x30 — and then diverges: the
		// config is 0x1028 rather than 0x100C, which pushes everything after it, and there are
		// THREE more fields past the config that m2ftg's block does not have.
		//
		// The last of them matters even though YAMP passes nothing in it: module_start reads
		// +0x1070 UNCONDITIONALLY (`mov rax,[rdx+0x1070]`, no null test) and stores it in a
		// global. A params block that stopped at the end of the config would have that read land
		// on whatever follows it in the caller's frame, and every use of the global — all of them
		// virtual calls through it — is guarded by `!= nullptr` and nothing else. So the field has
		// to exist and has to be null, not merely absent.
		struct module_params_t
		{
			size_t size = sizeof(decltype(*this));
			const sl_module_t* sl_module;
			const gs_module_t* gs_module;
			const ct_module_t* ct_module;
			const icri* cri_ptr;
			const char* root_path;
			module_func_t* module_main;
			pre3_config_t config{};
			// +0x1060 / +0x1068: the module's TWO RENDER STAGES, filled in only when the host
			// supplies a slot for them (0x180042690 and 0x1800427E0, both taking the same 0x1780
			// execute_info as module_main). They are not optional extras — see ModuleEntries in
			// Pre3Host.h. Leaving them null runs the board with the renderer switched off.
			module_func_t* module_render_begin;
			module_func_t* module_render_end;
			// +0x1070: the optional LINK-CABINET interface. The board settings initialiser
			// (0x180026840) forces VS mode on whenever it is present, and the input path calls
			// through it for peer state — it is how Spikeout and Sega Rally 2's linked seats talk.
			// Null until YAMP has netplay on this path.
			void* link_interface = nullptr;
		} params;
		static_assert(offsetof(module_params_t, config) == 0x38);
		static_assert(offsetof(module_params_t, module_render_begin) == 0x1060);
		static_assert(offsetof(module_params_t, module_render_end) == 0x1068);
		static_assert(offsetof(module_params_t, link_interface) == 0x1070);

		const std::string utf8Path = gamePath.u8string();

		params.sl_module = &sl_module;
		params.gs_module = &gs_module;
		params.ct_module = &ct_module;
		params.cri_ptr = &criware;
		params.module_main = &entries.update;
		params.module_render_begin = &entries.render_begin;
		params.module_render_end = &entries.render_end;
		params.root_path = utf8Path.c_str();

		const auto* settings = gGeneral.GetSettings();
		const GameDesc& game = CurrentGame();

		params.config.game = static_cast<uint8_t>(game.game);

		// DIFFICULTY AND REGION ARE PRE-FOLDED, because the module folds them AGAIN on the way
		// into the board and the two scales do not line up. Both mappings below were read out of
		// the module's settings injector (DLL 0x18002C410) and then CONFIRMED against the board's
		// own GAME ASSIGNMENTS screen, by reading the bytes it displays out of guest RAM.
		//
		// The board's settings live at guest 0x100180 (working copy) and 0x72629C (the NVRAM copy
		// the injector writes); same layout, so +0x21 is DIFFICULTY and +0x18 is COUNTRY in both.
		// The option lists come from the service menu's own tables at guest 0x0E5548.
		//
		// DIFFICULTY: the board has FOUR steps - EASY / NORMAL / HARD / HARDEST - exactly like
		// YAMP's combo. But SRC2's injector maps 0 and 1 BOTH to EASY, then 2 -> NORMAL,
		// 3 -> HARD, anything else -> HARDEST. Passing the UI index straight through therefore
		// lost a step and shifted the rest: "Normal" displayed as EASY, and HARDEST could not be
		// selected at all. Measured before the fix - config 1 -> board 0 (EASY), config 3 ->
		// board 2 (HARD) - and after it, config 4 -> board 3 (HARDEST).
		//
		// PER-GAME, and it has to be. FV2 has its own injector (DLL 0x180028180) whose fold is
		// the IDENTITY over 0..3 (0->0, 1->1, 2->2, 3->3, anything else -> 4), writing the byte
		// at guest 0x12054D. Applying SRC2's correction there would shift every FV2 difficulty up
		// by one - the exact bug being fixed, in the other direction.
		{
			const int index = settings->m_m2Difficulty >= 0 && settings->m_m2Difficulty <= 3
				? settings->m_m2Difficulty : 1;
			constexpr uint8_t SRC2_TO_BOARD[4] = { 0, 2, 3, 4 };
			params.config.difficulty = game.game == Game::Src2
				? SRC2_TO_BOARD[index]
				: static_cast<uint8_t>(index);
		}

		// COUNTRY: the board knows six - INTERNATIONAL, JAPAN, USA, EXPORT, AUSTRALIA, KOREA -
		// but the injector can only produce the first three, mapping config 0 -> JAPAN,
		// 1 -> USA, 2 -> INTERNATIONAL and everything else -> JAPAN. So only those three are
		// reachable through the config byte, and the UI offers exactly those three in board
		// order; ArcadeSettings writes the remaining ones directly.
		params.config.region = settings->m_m2Country <= 2 ? static_cast<uint8_t>(settings->m_m2Country) : 0;
		params.config.is_freeplay = settings->m_m2Freeplay ? 1 : 0;
		params.config.is_vs_mode = settings->m_m2VersusMode ? 1 : 0;

		// THE LINKED-CABINET RING, and it has to be settled HERE rather than when a session
		// starts: module_start is where the module latches both fields, and nothing reads them
		// again. So unlike Virtual On - which pulses the TEST switch to change role on a running
		// board - a role change here means restarting the module.
		//
		// Zero/zero unless CommBoard has been asked for a link, which is the value YAMP has always
		// passed and the state the module treats as a single cabinet.
		CommBoard::Configure();
		params.config.link_node_id = CommBoard::ConfigNodeId();
		params.config.link_peer_count = CommBoard::ConfigNodeCount();
		// The module derives the width from this at a fixed 496/384, so it is the whole
		// resolution story — there is no display-mode table here of the kind m2ftg's ModuleArgs
		// drives, and no default either: leaving it zero is what produced the 0x0 render target
		// the module traps on.
		//
		// Gaiden feeds it its own OUTPUT height (a PIX capture of a windowed session shows
		// 1162x900 targets, and 900 was that window's height), so do the same: the module's
		// buffers then scale with the window exactly as they do natively. Floored at the native
		// 384 so a tiny or not-yet-sized window cannot hand it a degenerate target again.
		// Setting 0 keeps that native behaviour; 1..6 pin the board to N x its native 384 instead,
		// which is what an arcade emulator is usually wanted to do. Floored either way.
		const uint32_t renderScale = settings->m_pre3RenderScale;
		params.config.render_height = renderScale > 0
			? NATIVE_HEIGHT * static_cast<int32_t>(renderScale)
			: std::max<int32_t>(NATIVE_HEIGHT, static_cast<int32_t>(window.GetHeight()));
		DebugLogFile("[%s] render target %dx%d (window height %u)\n", gGeneral.GetGameTag(),
			static_cast<int>(params.config.render_height * NATIVE_WIDTH / NATIVE_HEIGHT),
			params.config.render_height, window.GetHeight());

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

		m2ftg::ApplyAspectSetting(window, settings->m_m2Aspect);

		DebugLog("[%s::Run] calling module_start (game=%u '%s', archive '%s')...\n",
			gGeneral.GetGameTag(), params.config.game, game.display_name, game.image_archive);
		const auto msRet = module_start(sizeof(params), &params);
		DebugLog("[%s::Run] module_start returned %lld, update=%p render_begin=%p render_end=%p\n",
			gGeneral.GetGameTag(), (long long)msRet, static_cast<void*>(entries.update),
			static_cast<void*>(entries.render_begin), static_cast<void*>(entries.render_end));

		if (msRet == 0 && entries.update != nullptr)
		{
			LARGE_INTEGER lastTime;
			QueryPerformanceCounter(&lastTime);
			const uint32_t frameLimit = gGeneral.GetFrameLimit();
			uint32_t framesRun = 0;
			while (!window.IsShuttingDown())
			{
				if (!GameLoop(entries, window))
				{
					DebugLog("[%s::Run] GameLoop returned false\n", gGeneral.GetGameTag());
					break;
				}
				if (frameLimit != 0 && ++framesRun >= frameLimit)
				{
					DebugLogFile("[%s::Run] frame limit %u reached, shutting down\n",
						gGeneral.GetGameTag(), frameLimit);
					break;
				}

				LARGE_INTEGER currentTime;
				do
				{
					QueryPerformanceCounter(&currentTime);
				} while (((currentTime.QuadPart - lastTime.QuadPart) * 1000) < frameTimeTicks);
				lastTime = currentTime;
			}

			// The link probe's collected samples, written out once now rather than per frame -
			// see CommBoard::DumpTrace. No-op unless YAMP_PRE3_SYNCPROBE asked for them.
			CommBoard::DumpTrace();

			const int stopResult = module_stop(0, nullptr);
			DebugLogFile("[%s::Run] module_stop -> 0x%X\n", gGeneral.GetGameTag(), stopResult);
		}
	}

	bool pre3::GameLoop(const ModuleEntries& entries, RenderWindow& window)
	{
		static csl_pad s_pads[2];
		static bool s_startScreen = false;
		static bool s_coinPending = false;
		static bool s_startToggle = false;

		// (1) Read the session state ONCE, at the top, before Drive() can advance it - so pad
		// routing and the input suppression below all see the same answer for the whole frame.
		// See NetSession.h for the four-call-point contract.
		NetSession& net_ = NetplaySession();
		const NetSession::Status netStatus = net_.GetStatus();
		const bool netplayMatch = netStatus.inMatch;
		const int32_t netplayLocalPad = netStatus.localPad;
		const bool netplayLocked = netStatus.locked;

		const auto* settings = gGeneral.GetSettings();

		{
			static uint32_t s_lastAspect = UINT32_MAX;
			if (settings->m_m2Aspect != s_lastAspect)
			{
				s_lastAspect = settings->m_m2Aspect;
				m2ftg::ApplyAspectSetting(window, s_lastAspect);
			}
		}

		// PERSISTENT across frames, exactly as on the m2ftg path — the module keeps state in it,
		// including the save-data region it writes at +0x660.
		static pre3_execute_info_t execute_info{};
		execute_info.size_of_struct = sizeof(execute_info);
		execute_info.p_device_context = gs::device_context();
		execute_info.status = 0;
		execute_info.output_texid = 0;
		execute_info.result = 0x80004005;
		execute_info.sound_volume =
			static_cast<float>(gGeneral.GetSettings()->m_volumePercent) / 100.0f;
		// The two per-channel scalars the frame callback multiplies sound_volume by. The host is
		// the only thing that ever writes them and the module reads them raw, so a zero here is
		// silence on all six channels — unity is what "the master volume alone decides" means.
		execute_info.volume_channel5 = 1.0f;
		execute_info.volume_channels0_4 = 1.0f;

		static bool s_pauseMenuOpen = false;
		{
			// DISABLED DURING NETPLAY. Pausing is host->module input like any other: status bit0
			// stops the emulated board, and stopping it on one machine only desyncs the pair. It
			// also suppresses the per-frame submit and the upload-stamp advance below, so an "only
			// cosmetic" pause would still change how much work this side does. There is no
			// per-player pause in an arcade match.
			if (netplayLocked)
			{
				s_pauseMenuOpen = false;   // close it if a session started while it was open
			}

			static bool s_escWasDown = false;
			const bool escDown = gGeneral.GetPressedKeys()[VK_ESCAPE];
			if (escDown && !s_escWasDown && !netplayLocked)
			{
				s_pauseMenuOpen = !s_pauseMenuOpen;
			}
			s_escWasDown = escDown;
		}
		// The module's own pause: module_main freezes the board, pauses all six audio channels
		// and records nothing, so the submit below is skipped too and the last completed frame
		// keeps being presented. That is what Like a Dragon Gaiden does.
		//
		// A debug mode that froze the BOARD but kept the RENDERER going was tried here, to let the
		// draw sliders be swept with the action held still. It cannot work: the module builds its
		// draw list in the update stage, so skipping that stage leaves the render stages with
		// nothing to record - a PIX capture taken in that mode contained three draws and none of
		// the module's scene. Its own pause bit records nothing either, so there is no way through
		// this module's interface to re-draw a frozen frame. Sweep with the game running.
		if (s_pauseMenuOpen)
		{
			execute_info.status |= 1;
		}

		// Two pads filled of the four the struct carries. Fighting Vipers 2 is a two-player
		// cabinet; the other two slots stay zeroed (m_is_connected false), which is what an
		// unoccupied seat looks like.
		Input::PollPads();
		if (netplayMatch && netplayLocalPad >= 0 && netplayLocalPad < 2)
		{
			// ONLINE, YOU ALWAYS PLAY AS PLAYER 1 LOCALLY. The guest occupies pad slot 1 in the
			// match, but they are the only person at that keyboard/controller and have their own
			// Player 1 bindings set up - making them remap to the Player 2 controls just to play
			// online would be absurd. So this machine's P1 bindings drive whichever slot this
			// machine owns on the wire.
			//
			// The other slot is filled from P2 purely to keep it well-formed for this frame:
			// step() overwrites BOTH pads from the transmitted inputs before the update stage sees
			// them, so nothing local survives into the simulation.
			s_pads[netplayLocalPad].set_state(0);
			s_pads[1 - netplayLocalPad].set_state(1);
		}
		else
		{
			s_pads[0].set_state(0);
			s_pads[1].set_state(1);
		}
		for (int i = 0; i < 2; i++)
		{
			memcpy(&execute_info.pad[i], &s_pads[i], 0xE0);
			execute_info.pad[i].m_port = static_cast<unsigned int>(i);
			execute_info.pad[i].m_user_id = i;
			execute_info.pad[i].m_is_connected = true;
		}

		// What each of the module's eight button slots means, exactly as the m2ftg hosts hand it
		// over — same table, same slot order (see execute_info::assign in pre3.h). Rewritten every
		// frame because the board takes its copy every frame.
		for (int p = 0; p < 2; p++)
		{
			for (int i = 0; i < 8; i++)
			{
				execute_info.assign[p][i] = Input::MODULE_ASSIGN[i];
			}
		}

		// Dedicated coin binding: a press becomes the coin status bit (host->module bit5), which
		// the module turns into the SYSTEM port's coin line for one frame. Sent regardless of the
		// free-play setting, because on the cabinet the coin switch is wired to that line whatever
		// the dip switches say - the board's own input test shows it, and with free play on the
		// ROM simply does not credit it. Still swallowed while the pause menu is open, like every
		// other input.
		{
			static bool s_coinWasDown[2] = {};
			for (int p = 0; p < 2; p++)
			{
				const bool down = Input::ActionDown(p, Input::Action_Coin);
				// Not transmitted, so it cannot be honoured during netplay without desyncing: it
				// would raise the coin status bit on one machine only. A round starts from the
				// board's VS start state, which is already credited, so there is nothing it would
				// buy either.
				if (down && !s_coinWasDown[p] && !s_pauseMenuOpen && !netplayLocked)
				{
					execute_info.status |= 0x20;
				}
				s_coinWasDown[p] = down;
			}
		}

		// Cabinet service panel (see InstallSystemSwitches): TEST opens the board's own service
		// menu - the operator's input test, which is how the panel wiring gets checked - and
		// SERVICE is the credit / navigate button beside it. Held lines, like the panel; the ROM
		// decides what latches. Either player's binding closes the one switch, and both are
		// suppressed while paused so a menu keystroke cannot reach the board.
		{
			// Suppressed while paused, and for the whole of a netplay session: neither is in the
			// transmitted pad, so honouring them would drop one machine into the operator's menu
			// (or feed it a service credit) while the other carried on playing.
			bool test = false;
			bool service = false;
			if (!s_pauseMenuOpen && !netplayLocked)
			{
				for (int p = 0; p < 2; p++)
				{
					test = test || Input::ActionDown(p, Input::Action_Test);
					service = service || Input::ActionDown(p, Input::Action_Service);
				}
			}
			SetSystemSwitches(test, service);
		}

		// The arcade coin/start protocol, same shape as m2ftg's: while the module reports the
		// start screen (status bit6), a START press becomes a coin insert and START is then
		// injected on alternating frames until the module leaves that screen. Only meaningful
		// with the freeplay dip switch off.
		//
		// OFF FOR THE WHOLE OF A NETPLAY SESSION, and unlike the m2ftg path it is not re-run after
		// step() either. It both reads and writes pad[0] and raises the coin status bit from what
		// it finds, which on m2ftg had to be moved after the pads were synchronised or it desynced
		// them. Here it has nothing to do: a round starts from the board's own VS start savestate,
		// which is past the credit screen by construction, so the honest behaviour is for the
		// protocol simply not to run.
		if (!settings->m_m2Freeplay && !s_pauseMenuOpen && !netplayLocked)
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
			if (!m2ftg::DrawPauseMenu(window, s_pauseMenuOpen))
			{
				return false; // Quit picked
			}
		}

		gs::with_tail([](auto* ctx) { ResetCbvSrvRingCursors(ctx); });

		// ---- NETPLAY SESSION DRIVER ----------------------------------------------------
		// (2) Poll the plugin and run the round-start state machine, and (3) decide whether the
		// emulator may advance. Drive() runs HERE rather than at the top of the frame on purpose:
		// GetStatus() above is deliberately last frame's answer, so the frame on which a barrier
		// releases still routes its pads against a stable view. See NetSession.h.
		net_.Drive();

		// THE POWER-ON SNAPSHOT, and it has to be requested HERE - before the update stage, on the
		// first frame the machine reports itself running. At that point the board has reached its
		// terminal phase but has not stepped a single guest frame (phase becomes 0x10 at the end of
		// one update stage; stepping begins on the next), so what gets captured is the pristine
		// post-boot state. Two peers reach it identically by construction, which is the entire
		// reason a round can be started from it. Asking a frame later would capture a board that
		// had already run, and "how many frames had it run?" is a host-timing question.
		//
		// Unconditional rather than netplay-gated: the request is one byte and the capture is one
		// frame, and making it depend on whether a session happens to be up would make WHEN it was
		// taken depend on the player, which is exactly what must not vary. Self-limiting - see
		// SaveResetSnapshot.
		SaveResetSnapshot();
		// Step() also overwrites BOTH pads in execute_info with the transmitted inputs, which is
		// why it runs after the local pad fill above: whatever csl_pad produced is a local
		// prediction that the network's copy must replace, or the two machines simulate different
		// inputs and desync. On the savestate-restore frame it zeroes them instead.
		const bool advanceFrame = net_.Step(execute_info);

		// The host zeroes output_texid every frame and the module fills it in. On a stalled frame
		// nothing runs, so it would stay 0 - and 0 is not a valid handle (the pxd handle table is
		// 1-based and asserts on get(0)). Remember the last good id so a stall re-presents the
		// previous frame.
		static unsigned int s_lastOutputTexId = 0;

		// All three stages, in the module's own order. The update stage advances the emulated
		// board; the two render stages are what actually record anything — 0x18 uploads the
		// 0x120000 scroll buffer, 0x20 walks the texture tables and issues the geometry, 0x28
		// closes the frame and flips the buffer parity. Running only the first is why this
		// rendered nothing at all: the board simulated correctly and drew zero primitives.
		//
		// A netplay stall skips ALL THREE rather than just the update: the render stages read the
		// board state the update stage produced, so re-recording them against an unchanged board
		// would only redraw the frame already on screen at the cost of a full frame's GPU work.
		// Re-presenting the last texture is the same picture for nothing.
		int funcResult = 0;
		if (advanceFrame)
		{
			// Draw isolation: seed the hook layer from the ini ONCE. After that the value is owned
			// by the Debug page's slider, which writes it live - pushing the ini value every frame
			// would fight the slider and pin it back to 0.
			static bool s_drawLimitSeeded = false;
			if (!s_drawLimitSeeded)
			{
				s_drawLimitSeeded = true;
				if (settings->m_drawLimit != 0) SetModuleDrawLimitNow(settings->m_drawLimit);
			}

			// The linked cabinet's host half, and it runs BEFORE the update stage on purpose.
			// The emulated comm board fills its TX slot inside the module's frame, so the
			// freshest packet available to send is always the PREVIOUS frame's - taking it
			// here is what makes "one packet per board frame" true and keeps the read off a
			// buffer the module is writing underneath it. Virtual On learned that ordering the
			// expensive way; see docs/von-handoff.md.
			//
			// Attach re-points the three work pointers every frame rather than once: they live
			// in a persistent execute_info, and a "set it up once" rule is a trap for whoever
			// next adds a path that rebuilds the block. Both calls are no-ops without a link.
			CommBoard::Attach(execute_info.p_work_ptr);
			CommBoard::Update();

			// Marker BEFORE the update stage releases the emulator's worker, so the probe below can
			// wait for THIS frame rather than sampling whatever the worker happens to be doing. See
			// Determinism.h's WaitForEmulatedFrame: the busy flags alone cannot tell "the frame
			// finished" from "the frame has not started".
			const unsigned long long boardMarker = BoardFrameMarker();

			SetModuleRenderActiveNow(true);
			funcResult = entries.update(sizeof(execute_info), &execute_info);
			if (funcResult == 0 && entries.render_begin != nullptr)
			{
				funcResult = entries.render_begin(sizeof(execute_info), &execute_info);
			}
			if (funcResult == 0 && entries.render_end != nullptr)
			{
				funcResult = entries.render_end(sizeof(execute_info), &execute_info);
			}
			SetModuleRenderActiveNow(false);

			// The one place board state can be read coherently: the worker is parked and the frame
			// just simulated is the one being sampled. Everything else in CommBoard reads across the
			// running worker, which is fine for slow-moving fields and useless for an interrupt that
			// is raised and acknowledged inside a single emulated frame.
			if (WaitForEmulatedFrame(boardMarker))
			{
				CommBoard::SampleAtFrameBoundary();
			}

			if (execute_info.output_texid != 0)
			{
				s_lastOutputTexId = execute_info.output_texid;
			}

			// (4) The frame really ran, so submit the desync canary and advance the netplay frame
			// index. Only reachable here - a stalled frame must not advance it.
			net_.EndFrame(execute_info);
		}
		else
		{
			// Stalled: show the frame we already have.
			execute_info.output_texid = s_lastOutputTexId;
		}

		s_startScreen = (execute_info.status & 0x40) != 0;

		// The netplay groundwork's own health line - see LogBoardStateOnce. Deliberately outside
		// any netplay condition: its whole value is that an ordinary run reports whether a round
		// COULD have worked.
		LogBoardStateOnce();

		{
			static int s_frame = 0;
			static int s_lastLogged = -1;
			static int s_lastMode = -1;
			static unsigned int s_lastDraws = 0;
			// Health line for smoke runs: status or board-mode changes, plus a periodic
			// heartbeat. The mode pair is what makes an input verifiable without a screenshot —
			// it moves the moment the ROM changes screen, so a TEST press that reaches the board
			// shows up here even on a headless -frames run.
			const int mode = execute_info.game_mode << 8 | execute_info.game_sub_mode;
			// NO FRAME CAP on the interesting events. The cap used to be `s_frame < 4000`, which is
			// about 66 seconds - fine for a smoke run, useless for anything that happens during
			// PLAY. Chasing an object that only renders from the second round onwards produced no
			// lines at all, because the round it had to be compared against was minutes in.
			//
			// What is kept is the sparseness: a line only when something CHANGES (host status, the
			// board's mode pair, or the module's draw count moving by more than the couple of draws
			// an animation adds and removes), plus a slow heartbeat. draws= is the one that matters
			// for a missing object - it counts what the module ISSUED, before anything downstream
			// could drop it, so it separates "the board never drew it" from "we lost it".
			const unsigned int draws = ModuleDrawsLastFrameNow();
			const bool drawsMoved = draws > s_lastDraws + 2 || draws + 2 < s_lastDraws;
			if (execute_info.status != s_lastLogged || mode != s_lastMode || drawsMoved
				|| s_frame % 600 == 0)
			{
				// draws= is the upper bound for a draw-isolation sweep: the limit only means
				// anything between 1 and this. See YAMPSettings::m_drawLimit.
				DebugLogFile("[pre3] frame=%d status=0x%X result=0x%X texid=%u mode=%02X/%02X "
					"draws=%u%s\n",
					s_frame, execute_info.status, execute_info.result, execute_info.output_texid,
					execute_info.game_mode, execute_info.game_sub_mode, draws,
					ModuleDrawLimitNow() != 0 ? " (DRAW LIMIT ACTIVE)" : "");
				s_lastLogged = execute_info.status;
				s_lastMode = mode;
				s_lastDraws = draws;
			}
			s_frame++;
		}

		if (funcResult != 0) return false;

		if (!s_pauseMenuOpen) SubmitModuleFrameListNow();
		if (ModuleExecDisabledNow())
		{
			DebugLog("[%s::Run] submit hang/device-removed -> stopping loop (see [DRED])\n",
				gGeneral.GetGameTag());
			return false;
		}
		if (!s_pauseMenuOpen) AdvanceFrameStampNow();

		cgs_tex* display_tex = (execute_info.output_texid != 0)
			? gs::handle_tex().get(execute_info.output_texid)
			: nullptr;
		if (execute_info.output_texid != 0)
		{
			if (display_tex == nullptr) return false;
			if (display_tex->m_type != 2) return false;
		}

		if (display_tex != nullptr)
		{
			auto* res = display_tex->mp_sbgl_resource;
			// mp_sbgl_resource IS the ID3D12Resource here — do NOT dereference +0x98 the way the
			// m2ftg path does. Established by walking the object and asking the OS who owns each
			// candidate's vtable: this object's own +0x00 is a D3D12Core.dll vtable (the very same
			// one the cgs_ib sub-object carries), while the pointer at +0x98 has a d3d12.dll
			// vtable — an internal back-pointer into the runtime, not a resource. Passing that to
			// the blit is what made the window black; the D3D12 debug layer traps with an int 3
			// in NOutermost::CDeviceChild::LUCGetInternalIdentity the moment anything calls
			// through it, which is how it was caught.
			auto* rt = reinterpret_cast<ID3D12Resource*>(res);
			// One-shot: is what we present actually the module's render target? A black window with
			// a healthy draw count is either "nothing was drawn" or "the wrong thing is shown", and
			// this separates them. Logged once per distinct texid so a changing output is visible.
			{
				static unsigned int s_loggedTexId = 0xFFFFFFFF;
				if (execute_info.output_texid != s_loggedTexId)
				{
					s_loggedTexId = execute_info.output_texid;
					// Pointers only — do NOT call anything on `rt` here. An earlier version called
					// GetDesc() and the D3D12 debug layer trapped with an int 3 inside
					// NOutermost::CDeviceChild::LUCGetInternalIdentity, which is its response to
					// being handed something that is not one of its objects. That trap IS the
					// finding: whatever sits at mp_sbgl_resource+0x98 for this module is not an
					// ID3D12Resource, and it is what the blit has been receiving.
					DebugLogFile("[present] texid=%u tex=%p type=%u sbgl_res=%p +0x98=%p\n",
						execute_info.output_texid, static_cast<void*>(display_tex),
						display_tex->m_type, static_cast<void*>(res), static_cast<void*>(rt));
					// Find the real ID3D12Resource. A COM object's first qword is a vtable inside
					// the D3D12 runtime, so scan the sbgl_resource for a pointer whose target
					// begins with an address owned by a loaded module — that is the same test the
					// cgs_ib sub-object passed. Probing memory, never calling through it.
					if (res != nullptr)
					{
						const auto* q = reinterpret_cast<const uintptr_t*>(res);
						for (size_t off = 0; off < 0x180; off += 8)
						{
							const uintptr_t cand = q[off / 8];
							if (cand < 0x10000) continue;
							MEMORY_BASIC_INFORMATION mbi{};
							if (VirtualQuery(reinterpret_cast<void*>(cand), &mbi, sizeof(mbi)) == 0
								|| mbi.State != MEM_COMMIT) continue;
							const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(cand);
							HMODULE owner = nullptr;
							if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
								| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								reinterpret_cast<LPCWSTR>(vt), &owner) && owner != nullptr)
							{
								wchar_t name[MAX_PATH] = {};
								GetModuleFileNameW(owner, name, MAX_PATH);
								const wchar_t* leaf = wcsrchr(name, L'\\');
								DebugLogFile("[present]   +0x%02zX -> %p  vtable in %ls\n",
									off, reinterpret_cast<void*>(cand), leaf ? leaf + 1 : name);
							}
						}
					}
				}
			}
			window.BlitDX12Texture(rt);
			window.RenderImGui();
		}
		else
		{
			window.RenderImGui();
		}
		window.EndFrame();

		auto& swapChain = gs::sbgl_device().m_swap_chain;
		HRESULT hr = swapChain.m_pDXGISwapChain->Present(1, 0);
		if (FAILED(hr)) return false;

		gs::sm_context->frame_counter++;
		return true;
	}
}
