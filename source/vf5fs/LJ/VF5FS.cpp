#include "VF5FS.h"
#include "../../ModuleLoad.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <filesystem>
#include <memory>
#include <string>

#include "ImportSymbols.h"

#include "../../criware/Cri.h"
// The pad-binding policy csl_pad::set_state reads (source/input/Pad.cpp). VF5FS-LJ shares the
// m2ftg titles' P/K/G control scheme, so it shares their bindings rather than duplicating them.
#include "../../input/Input.h"
#include "../../pxd/LJ/sl.h"
#include "../../pxd/LJ/gs.h"
#include "../../pxd/LJ/PatchGs.h"
#include "../../pxd/LJ/HostCdevice.h"
#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../Utils/ScopedUnprotect.hpp"
#include "../../wil/resource.h"

// Host-side per-frame duties implemented in ../../pxd/HostCdevice.cpp.
// SubmitModuleFrameListNow: close + ExecuteCommandLists the command lists the module recorded into and
// left OPEN (module_main only RECORDS; submitting is the host's job — in Lost Judgment the engine's
// render-system loop does it). Despite the name it is not StF-specific: it works off the lists
// flagged as belonging to the game module.
// AdvanceFrameStampNow: advance the upload-frame stamp (cdevice+0x58 -> +0x67D8) and recycle the
// upload-buffer pool's in-use nodes back to available.
void SubmitModuleFrameListNow();
void AdvanceFrameStampNow();
bool ModuleExecDisabledNow();

namespace vf5fs
{
	namespace LJ
	{
		// The shared Lost-Judgment-era pxd platform layer (source/pxd): sl, gs,
		// cgs_device_context, the DX12 host cdevice, PatchSl/PatchGs.
		using namespace pxd;

		static const wchar_t* DLL_NAME = L"vf5fs-pxd-w64-d3d12_retail.dll";
		static const wchar_t* DLL_SUBDIR = L"vf5fs";

		// ct context: the module's initialize only checks size (0x30 here, same as LJ's m2ftg
		// modules), and nothing in YAMP reads it, so an opaque block of the right size is enough.
		struct ct_context_t
		{
			uint32_t tag_id;
			uint32_t version;
			uint32_t size_of_struct = sizeof(ct_context_t);
			std::byte unknown[0x30 - 0x0C]{};
		};
		static_assert(sizeof(ct_context_t) == 0x30);

		// TODO: by value once the bring-up is stable; on the heap for now so page-heap catches
		// out-of-bounds writes (same reasoning as the other hosts).
		static ct_context_t* g_ct_context = new ct_context_t;

		// The per-frame block, size-gated by module_main (`cmp rcx, 0x690` at 0x1801EDCCF).
		//
		// The 0x20-byte header is the shared pxd one (see ../vf5fs.h). The tail fields are the ones
		// module_main and its callees actually touch, read out of the DLL:
		//   +0x663  byte, THE MASTER VOLUME, on a 0..20 scale (0 = silence). module_main compares it
		//           against the module's current setting and, on change, writes it into BOTH volume
		//           fields of the engine settings blob (FUN_1801976F0 -> +0x4FD4 and +0x4FD8) and
		//           calls FUN_1801970D0, which converts it with `level / 20.0f` (the constant at
		//           0x180292CAC) through a log into a dB attenuation. That attenuation lands in the
		//           mixer channels at DAT_180654CD0 (3 x 0x60), and the per-frame mixer FUN_18016F250
		//           turns it back into a linear amplitude for criAtomExPlayer_SetVolume — but only
		//           when it is above the -960 (-96 dB) silence floor, else it passes a hard 0.0f.
		//           Leaving this byte at 0 therefore mutes EVERY cue while the rest of the audio
		//           path works perfectly: the LJ-era equivalent of the Y6 sound_volume mute bug.
		//   +0x674  byte, returned by the accessor at 0x1801F2C80
		//   +0x67C  bool, written when the event path raises status bit 0x400 (FUN_18009D7A0)
		//
		// INPUT lives at +0x20: pad[2], 0x190 stride — the same LJ-era block m2ftg uses, NOT the Y6
		// VF5FS layout (which puts assign[2][8] at +0x20 and 0x170 pads at +0x30). Established by
		// walking the xrefs to the live execute_info global DAT_180B75978 (module_main stores the
		// pointer there on entry, clears it on return; every consumer in the DLL reaches the struct
		// through it). The one reader in this range is FUN_1801F22C0:
		//     for (i = 0; i < 2; i++)  p = execute_info + 0x20 + i*0x190   // 400 decimal
		//         m_now   = *(uint*)(p + 0x00)   -> remapped to the module's own button bits
		//         m_x1/m_y1/m_x2/m_y2 = floats at p+0x10/0x14/0x18/0x1C, scaled to +-32767
		//                               (y axes are sign-flipped, so up = -1.0 as in m2ftg)
		//         m_buttons[4], m_buttons[5] at p+0xA4/0xA5 -> analog trigger pressure (<< 7)
		// It then derives stick-direction bits by thresholding the scaled axes at +-0x4651, so the
		// module handles digital-vs-analog itself and the host only has to fill the pad honestly.
		//
		// An earlier live capture of LJ's own execute_info read as "all zero except +0x100" and I
		// wrongly concluded the pads were elsewhere: nothing was being pressed, and +0x100 is
		// 0x20 + 0xE0 = pad[0].m_port (-1, i.e. unassigned).
		//
		// +0x680 and +0x688 are int[2] (per player) BUTTON ASSIGNMENTS, and +0x674 a byte read
		// alongside them. The accessors at 0x1801F2C90/0x1801F2CB0 index them by player and pass the
		// value through FUN_1801F25F0 — the SAME sl-index -> module-bit remap the pad reader uses
		// (0->4, 1->2, 2->8, 3->1, ...). Consumers fold the result into fixed masks, e.g.
		// FUN_180189E50 does `assigned_bit | 0x20F`. Leaving all three at 0 is BENIGN rather than
		// merely untested: index 0 remaps to bit 0x4, which those masks already contain. Revisit only
		// if a menu/confirm button is unresponsive while the in-game pad works.
		struct alignas(16) execute_info_t
		{
			size_t size_of_struct;
			cgs_device_context* p_device_context;
			int status;
			int result;
			unsigned int output_texid;
			float sound_volume;
			lj_pad_t pad[2];
			std::byte unmapped[0x660 - 0x340];
			std::byte work_pre[0x663 - 0x660];
			uint8_t sound_volume_level;
			std::byte gap0[0x674 - 0x664];
			uint8_t unknown_674;
			std::byte gap1[0x67C - 0x675];
			bool event_flag_67C;
			std::byte tail[0x690 - 0x67D];
		};
		static_assert(sizeof(execute_info_t) == 0x690);
		static_assert(offsetof(execute_info_t, sound_volume) == 0x1C);
		static_assert(offsetof(execute_info_t, pad) == 0x20);
		static_assert(offsetof(execute_info_t, pad[1]) == 0x1B0);
		static_assert(offsetof(execute_info_t, sound_volume_level) == 0x663);
		static_assert(offsetof(execute_info_t, unknown_674) == 0x674);
		static_assert(offsetof(execute_info_t, event_flag_67C) == 0x67C);

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
			Import(sl::kernel_calloc_internal, ImportSymbol::SL_KERNEL_CALLOC);
			Import(cgs_device_context::reset_state_all_internal, ImportSymbol::DEVICE_CONTEXT_RESET_STATE_ALL);
			Import(gs::vb_create, ImportSymbol::VB_CREATE);
			Import(gs::ib_create, ImportSymbol::IB_CREATE);
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

			// As on the m2ftg LJ path (and unlike the Y6 DX11 host): "D3DDEVICE" is
			// cdevice_common::g_pD3DDevice, which the DX12 renderer treats as the pxd host cdevice
			// object, NOT as an ID3D12Device. Storing the raw device leaves its intermediate-buffer
			// freelist empty and the module spins forever.
			void** ppDevice12;
			Import(ppDevice12, ImportSymbol::D3DDEVICE);

			using cdevice_ctor_fn = void* (*)(void*);
			auto cdeviceCtor = reinterpret_cast<cdevice_ctor_fn>(symbols.GetSymbol(ImportSymbol::CDEVICE_CTOR));
			*ppDevice12 = BuildHostCdevice(window.GetD3D12Device(), window.GetD3D12Queue(), cdeviceCtor);
		}

		static bool ResolveSymbolsAndInstallPatches(void* dll, const RenderWindow& window) try
		{
			// This DLL is DYNAMIC_BASE (ASLR), like LJ's FV module — it does NOT load at
			// 0x180000000, so the hooks' return-address checks need its real range.
			SetGameDllRange(dll);

			const Imports symbolMap = BuildSymbolMap(dll);

			// Refuse to start rather than crash if a pattern stops resolving (e.g. after a game
			// update): the sl file layer would otherwise call through null function pointers on the
			// module's first archive open.
			if (const char* missing = RequiredButMissing(symbolMap); *missing != '\0')
			{
				const std::wstring str(L"YAMP cannot run " + std::wstring(DLL_NAME) + L".\n\n"
					L"These symbols did not resolve: " + UTF8ToWchar(missing) +
					L"\n\nThe DLL is probably a different version than the byte patterns in "
					L"source/vf5fs/LJ/ImportSymbols.cpp were built for.");
				MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
				DebugLogFile("[vf5fs::LJ] missing symbols: %s\n", missing);
				return false;
			}
			DebugLogFile("[vf5fs::LJ] all required symbols resolved (dll base %p)\n", dll);

			const ScopedUnprotect::Section text(static_cast<HMODULE>(dll), ".text");
			const ScopedUnprotect::Section rdata(static_cast<HMODULE>(dll), ".rdata");

			ImportFunctions(symbolMap);
			DebugLogFile("[vf5fs::LJ] imports bound: sl ctx %p, gs ctx %p\n",
				static_cast<void*>(sl::sm_context), static_cast<void*>(gs::sm_context));

			PrefillVariables(symbolMap, window);
			DebugLogFile("[vf5fs::LJ] host cdevice built: %p\n", GetHostCdevice());

			if (sl::sm_context && sl::sm_context->handles.p_handle_buffer == nullptr)
			{
				constexpr uint32_t kHandleCapacity = 0x100000;
				const int ic = sl::initialize();
				if (ic != 0)
				{
					DebugLogFile("[vf5fs::LJ] sl::initialize failed: 0x%X\n", ic);
					return false;
				}
				const int rc = sl::handle_initialize(kHandleCapacity);
				if (rc != 0)
				{
					DebugLogFile("[vf5fs::LJ] sl::handle_initialize failed: 0x%X\n", rc);
					return false;
				}
			}
			DebugLogFile("[vf5fs::LJ] sl initialized\n");

			PatchSl(sl::sm_context);
			DebugLogFile("[vf5fs::LJ] PatchSl done\n");
			PatchGs(gs::sm_context, window);
			DebugLogFile("[vf5fs::LJ] PatchGs done\n");

			return true;
		}
		catch (...)
		{
			const std::wstring str(L"Failed to resolve imports and/or patch " + std::wstring(DLL_NAME) +
				L"!\n\nIt's either not a valid Virtua Fighter 5: Final Showdown DLL from Lost Judgment, "
				L"or the game has been updated and YAMP is not forward compatible with that new version.");
			MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
			return false;
		}

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

		void Run(RenderWindow& window)
		{
			const auto module_start = reinterpret_cast<module_func_t>(GetProcAddress(gameDll.get(), "module_start"));
			THROW_LAST_ERROR_IF_NULL(module_start);
			const auto module_stop = reinterpret_cast<module_func_t>(GetProcAddress(gameDll.get(), "module_stop"));
			THROW_LAST_ERROR_IF_NULL(module_stop);
			module_func_t module_main;

			if (!ResolveSymbolsAndInstallPatches(gameDll.get(), window))
			{
				DebugLog("[vf5fs::LJ] ResolveSymbolsAndInstallPatches FAILED\n");
				return;
			}
			DebugLog("[vf5fs::LJ] patches installed OK\n");

			Cri criware;

			// Sizes are the ones this DLL's own initialize_module functions demand — the same values
			// LJ's m2ftg modules use, which is what makes the shared source/pxd layer applicable:
			//   sl 0x18020AF40: sl_module 0x10, sl context 0xF000
			//   gs 0x18022AEB0: gs_module 0x58, gs context 0x388A00
			//   ct 0x180225620: ct_module 0x10, ct context 0x30
			struct sl_module_t
			{
				size_t size = sizeof(sl_module_t);
				sl::context_t* context;
			} sl_module;
			static_assert(sizeof(sl_module_t) == 0x10);
			sl_module.context = sl::sm_context;

			struct gs_module_t
			{
				size_t size = sizeof(gs_module_t);
				gs::context_t* context;
				uint8_t pad[72];
			} gs_module;
			static_assert(sizeof(gs_module_t) == 0x58);
			gs_module.context = gs::sm_context;

			const struct ct_module_t
			{
				size_t size = sizeof(ct_module_t);
				ct_context_t* context = g_ct_context;
			} ct_module;
			static_assert(sizeof(ct_module_t) == 0x10);

			using params_t = module_params_t<lj_game_config_t, sl_module_t, gs_module_t, ct_module_t, icri>;
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

			params.config.core.is_dural_unlocked = false;
			params.config.core.is_triangle_start = false;
			params.config.core.game_mode = settings->m_arcadeMode ? 1 : 0;
			params.config.core.lang = static_cast<int8_t>(settings->m_language);
			params.config.core.diff = 1;
			params.config.core.energy = 200;
			params.config.core.round = 2;
			params.config.core.time = 60;

			// 60 FPS limiter, unless the cap is disabled in Debug (same shape as the other hosts).
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

			DebugLogFile("[vf5fs::LJ] module_start(size=%zu, root='%s')\n", sizeof(params), params.root_path);
			const int startResult = module_start(sizeof(params), &params);
			DebugLogFile("[vf5fs::LJ] module_start -> 0x%X, module_main=%p\n",
				startResult, reinterpret_cast<void*>(module_main));
			if (startResult == 0)
			{
				LARGE_INTEGER lastTime;
				QueryPerformanceCounter(&lastTime);
				// "-frames N" ends the run HERE rather than by killing the process, so smoke tests take
				// the real teardown path (see YAMPGeneral::GetFrameLimit).
				const uint32_t frameLimit = gGeneral.GetFrameLimit();
				uint32_t framesRun = 0;
				while (!window.IsShuttingDown())
				{
					if (!GameLoop(module_main, window)) break;
					if (frameLimit != 0 && ++framesRun >= frameLimit)
					{
						DebugLogFile("[vf5fs::LJ] frame limit %u reached, shutting down\n", frameLimit);
						break;
					}

					LARGE_INTEGER currentTime;
					do
					{
						QueryPerformanceCounter(&currentTime);
					} while (((currentTime.QuadPart - lastTime.QuadPart) * 1000) < frameTimeTicks);
					lastTime = currentTime;
				}

				// Tell the module to shut down. Completes the start/main/stop protocol instead of
				// relying on process exit, and gives the module a chance to release what it allocated.
				const int stopResult = module_stop(0, nullptr);
				DebugLogFile("[vf5fs::LJ] module_stop -> 0x%X\n", stopResult);
			}
		}

		bool GameLoop(module_func_t func, RenderWindow& window)
		{
			// Persistent across frames: the module keeps state in the un-mapped region, exactly like
			// the m2ftg protocol's work block, so this must NOT be a fresh local each frame.
			static execute_info_t execute_info{};
			// The pads csl_pad::set_state fills; persistent so m_prev/m_push/m_pull stay meaningful.
			static csl_pad s_pads[2];
			execute_info.size_of_struct = sizeof(execute_info);
			execute_info.p_device_context = gs::sm_context->p_device_context;
			execute_info.status = 0;
			execute_info.result = 0x80004005; // the host presets E_FAIL; module_main overwrites it
			execute_info.output_texid = 0;
			execute_info.sound_volume = 1.0f;
			// Master volume, 0..20 (see the struct notes). Nothing in this DLL reads the float
			// sound_volume at +0x1C that the m2ftg protocol uses — this byte is what actually drives
			// the mixer, and leaving it 0 mutes every cue. Passed on the module's own scale so its
			// mixer performs the attenuation rather than YAMP scaling samples after the fact.
			execute_info.sound_volume_level =
				static_cast<uint8_t>(min(gGeneral.GetSettings()->m_volumePercent, 100u) * 20u / 100u);

			// Input: refresh the shared XInput snapshot, evaluate each player's bindings via csl_pad
			// (Input, set up on the YAMP Controls page), then copy the shared 0xE0-byte prefix into
			// the module's LJ-era pad blocks — the same three steps the m2ftg host performs, because
			// this is the same engine generation and the same pad layout. VF5FS is a P/K/G fighter
			// like the m2ftg titles, so the existing binding policy in Pad.cpp maps over directly.
			Input::PollPads();
			s_pads[0].set_state(0);
			s_pads[1].set_state(1);
			for (int i = 0; i < 2; i++)
			{
				memcpy(&execute_info.pad[i], &s_pads[i], kLjPadCopyBytes);
				execute_info.pad[i].m_port = static_cast<unsigned int>(i);
				execute_info.pad[i].m_user_id = i;
				execute_info.pad[i].m_is_connected = true;
			}

			window.BeginFrame();
			window.NewImGuiFrame();

			// Per-frame transient: the descriptor-copy ring cursors must be reset before the module
			// records, or they grow past the heap and CopyDescriptors AVs (id=646).
			ResetCbvSrvRingCursors(gs::sm_context);

			const int funcResult = func(sizeof(execute_info), &execute_info);

			// Bring-up trace: log the first frames and then only on change, so the log shows how far
			// the module gets without drowning in per-frame lines.
			{
				static int s_frame = 0;
				static int s_lastState = -1;
				const int state = execute_info.status | static_cast<int>(execute_info.output_texid << 8);
				// Also tick every 60 frames so the log shows progress once the state goes quiet —
				// otherwise "5 lines logged" is indistinguishable from "died after 5 frames".
				if (s_frame < 5 || state != s_lastState || (s_frame % 60) == 0)
				{
					DebugLogFile("[vf5fs::LJ] frame=%d func=0x%X status=0x%X result=0x%X texid=%u\n",
						s_frame, funcResult, execute_info.status, execute_info.result,
						execute_info.output_texid);
					s_lastState = state;
				}
				s_frame++;
			}

			if (funcResult != 0) return false;

			// THE SUBMIT. module_main only RECORDS into command lists and leaves them open; in Lost
			// Judgment the engine's render-system loop closes and executes them. Without this the
			// module's whole frame — every draw and resolve our hooks log — is recorded and then
			// thrown away, so its display target stays byte-zero and the window is black no matter
			// what the composite does (proved by the readback probe above: 0/3686400 non-zero).
			SubmitModuleFrameListNow();
			if (ModuleExecDisabledNow())
			{
				DebugLogFile("[vf5fs::LJ] submit disabled the device (see [DRED]) -> stopping\n");
				return false;
			}

			// Host duty, same as the m2ftg path: advance the upload-frame stamp and recycle the
			// upload-buffer pool. Without it the pool exhausts within a few frames and the engine's
			// intermediate-buffer allocator hands back a node whose resource pointer (node+0x10) is
			// still null, which FUN_180239300 then AddRefs -> call through a null vtable slot.
			// VF5FS reads the stamp at *(cdevice+0x58)+0x67D8, exactly what AdvanceFrameStamp bumps.
			AdvanceFrameStampNow();

			// TODO(submit): on the m2ftg LJ path module_main only RECORDS — the host must also drive
			// the engine's per-frame submit and upload-buffer recycle (SubmitModuleFrameListNow /
			// AdvanceFrameStampNow, reached through the STF_FRAME_SUBMIT + STF_RENDER_EXECINFO
			// symbols). Neither of m2ftg's patterns for those matches this DLL (0 hits in the
			// 2026-07-29 scan), so VF5FS's equivalent submit entry point still has to be found; the
			// frame will very likely stay black until it is.

			cgs_tex* display_tex = gs::sm_context->handle_tex.get(execute_info.output_texid);
			if (display_tex == nullptr)
			{
				static bool s_logged = false;
				if (!s_logged) { s_logged = true; DebugLogFile("[vf5fs::LJ] no display tex for texid=%u\n", execute_info.output_texid); }
				return false;
			}
			if (display_tex->m_type != 2)
			{
				static bool s_logged = false;
				if (!s_logged) { s_logged = true; DebugLogFile("[vf5fs::LJ] display tex type=%d (want 2)\n", display_tex->m_type); }
				return false;
			}

			// DX12-native output: the DX11-typed fields are null on this path, and VF5FS keeps the
			// output ID3D12Resource at sbgl_resource+**0xB0** — NOT the +0x98 the m2ftg LJ modules
			// use. Found during bring-up by walking the object for a field whose vtable lives in a
			// D3D12 module: +0x98 is not a D3D12 object at all here, while +0xB0 is TEXTURE2D
			// 1280x720 B8G8R8A8_UNORM ALLOW_RENDER_TARGET — the module's own display target
			// (FUN_180202C80 creates it at 0x500 x 0x2D0). Blitting +0x98 is what made the window
			// come up blank.
			auto* res = display_tex->mp_sbgl_resource;
			ID3D12Resource* rt = res
				? *reinterpret_cast<ID3D12Resource**>(reinterpret_cast<uint8_t*>(res) + 0xB0)
				: nullptr;

			window.BlitDX12Texture(rt);

			window.RenderImGui();
			window.EndFrame();

			auto& swapChain = gs::sm_context->sbgl_device.m_swap_chain;

			// Canary for the StF-style blank screen: BlitDX12Texture + RenderImGui composite into the
			// RenderWindow's swap chain, but we Present the one hanging off the gs context. If those
			// are different objects we draw into one and present the other — the window stays blank
			// AND YAMP's own ImGui overlay never appears, which is exactly the symptom.
			{
				static bool s_logged = false;
				if (!s_logged)
				{
					s_logged = true;
					DebugLogFile("[vf5fs::LJ swapchain] gs=%p window=%p SAME=%d\n",
						static_cast<void*>(swapChain.m_pDXGISwapChain),
						static_cast<void*>(window.GetSwapChain()),
						swapChain.m_pDXGISwapChain == window.GetSwapChain() ? 1 : 0);
				}
			}

			HRESULT hr = swapChain.m_pDXGISwapChain->Present(1, 0);
			if (FAILED(hr)) return false;

			gs::sm_context->frame_counter++;
			return true;
		}
	}
}
