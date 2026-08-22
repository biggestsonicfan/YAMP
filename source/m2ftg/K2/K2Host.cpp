#include "K2Host.h"
#include "../../ModuleLoad.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ImportSymbols.h"

#include "../../criware/Cri.h"
#include "../../pxd/LJ/sl.h"            // pxd::sl — the head of the context, and the primitives
#include "../../pxd/K2/sl.h"            // pxd::K2 — THIS generation's context layout (0xF3C0)
#include "../../pxd/LJ/sl_internal.h"   // handle_internal_buffer_t (the 8-byte queue node)
#include "../m2ftg.h"                 // m2ftg_config_t (0x100C) — unchanged in this generation
#include "../Determinism.h"           // RomFrameCounterAddress / ReadEmulatedRam32
#include "../VirtualClock.h"          // the module's timebase, driven by frames not real time
#include "../Debug/HleHooks.h"              // the HLE trap table, disabled by repointing its handlers
#include "../CommBoard/CommBoard.h"   // the Model 2 comm board's DPRAM model, shared with MrLink
#include "../../pxd/GsBringup.h"      // the shared DX11 gs bring-up pieces
#include "../../cabinet/Cabinet.h"    // the shared cabinet front panel (pause/coin/assign/switches)
#include "../SystemSwitches.h"        // cabinet TEST / SERVICE on the emulated I/O board
#include "../../net/NetPlugin.h"      // net::Logf — the diagnostic sink both peers share
#include "../ModuleArgs.h"
#include "../DisplayModes.h"
#include "../../input/Input.h"
#include "../HostUI.h"
#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../Utils/MemoryMgr.h"       // VP::Patch — protection-aware writes into module .text
#include "../../Utils/ScopedUnprotect.hpp"
#include "../../Utils/Trampoline.h"
#include "../../wil/resource.h"

#include "VonBoard.h"
#include "VonTwinStick.h"          // the Saturn Twin Stick override, via source/input/BlissBox
#include "../../input/BlissBox.h"
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

		// The module's two-board gate, or null in a module that has no second board - see the note
		// on ImportSymbol::TWO_BOARD_GATE. Written once, after module_start.
		uint8_t* g_twoBoardGate = nullptr;

		// "-von-2board" on YAMP's command line. EXPERIMENT, off by default: nothing about normal
		// Virtual On play changes unless it is passed. Virtual On is a linked-cabinet game and the
		// module carries the whole second-board implementation with the switch never thrown; this
		// throws it, so the first question - does board 1 step at all? - can be answered by running
		// it rather than by reading more disassembly, which on this DLL has a poor track record.
		static uint8_t* g_renderBoardSelect = nullptr;

		// Choose which cabinet the render draws, by patching the two bytes that decide what the
		// frame step leaves selected: `XOR ECX,ECX` (board 0) <-> `MOV CL,1` (board 1).
		//
		// NOTE the VP::Patch. This writes into the module's .text, and the ScopedUnprotect that made
		// .text writable lives in ResolveSymbolsAndPatch and is long out of scope by the time the
		// game loop runs - so a bare store here faults. VP::Patch wraps the write in VirtualProtect
		// and restores the old protection.
		// SAY WHAT IT DID. This used to be entirely silent: null symbol, or bytes that did not match
		// the expected encoding, and it returned having done nothing at all - which made "the patch
		// is not being applied" indistinguishable from "the patch is applied and does not change
		// what is drawn", two completely different faults.
		static void SetRenderBoard(int board)
		{
			const bool wantOne = board != 0;
			static int s_reported = -1;
			const auto report = [&](const char* what) {
				if (s_reported != board)
				{
					s_reported = board;
					net::Logf("render board -> %d (%s)", board, what);
				}
			};

			if (g_renderBoardSelect == nullptr)
			{
				report("NO RENDER_BOARD_SELECT SYMBOL - the render always draws board 0");
				return;
			}
			const uint8_t* p = g_renderBoardSelect;
			// Verify before writing - never patch blind, and never write if it already reads right.
			if (wantOne && p[0] == 0x33 && p[1] == 0xC9)
			{
				Memory::VP::Patch(g_renderBoardSelect, { 0xB1, 0x01 });   // MOV CL,1  -> render board 1
				report("patched MOV CL,1");
			}
			else if (!wantOne && p[0] == 0xB1 && p[1] == 0x01)
			{
				Memory::VP::Patch(g_renderBoardSelect, { 0x33, 0xC9 });   // XOR ECX,ECX -> render board 0
				report("patched XOR ECX,ECX");
			}
			else if ((wantOne && p[0] == 0xB1 && p[1] == 0x01)
				|| (!wantOne && p[0] == 0x33 && p[1] == 0xC9))
			{
				report("already selected");
			}
			else
			{
				// Neither encoding: the symbol is not pointing at the instruction we think it is,
				// and every "patch" so far has silently done nothing.
				net::Logf("render board %d REFUSED: bytes at the select site are %02X %02X, "
					"expected 33 C9 or B1 01 - the symbol is wrong", board, p[0], p[1]);
			}
		}

		// The module's board-bank switch, or null in a module with only one board.
		using board_select_t = void(*)(int);
		board_select_t g_boardSelect = nullptr;

		uint8_t* g_dllBase = nullptr;

		// times per boot and must stay silent.
		// The zero-filling allocator trio is shared (pxd/GsBringup.h); the vtable ORDER is this
		// module's own measured fact - it frees at slot 3 where the YLAD generation frees at 2.
		static void* s_gsAllocatorVtbl[4] = {
			reinterpret_cast<void*>(&pxd::GsBringup::AllocNoop),    // slot 0
			reinterpret_cast<void*>(&pxd::GsBringup::ZeroAlloc),    // slot 1 (+0x08) — alloc(this, size, align)
			reinterpret_cast<void*>(&pxd::GsBringup::AllocNoop),    // slot 2
			reinterpret_cast<void*>(&pxd::GsBringup::AlignedFree),  // slot 3 (+0x18) — free(this, ptr)
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

			// The cgs_device_context stand-in - the shared shape (pxd/GsBringup.h): +0x18 the
			// immediate context (FUN_180093090 hands exactly this to the render-target setter),
			// +0x28/+0x30 the lazily-filling cb/up pools (a null table faults at DLL+0x9F23C on
			// frame 1's first constant-buffer upload), +0x38 the render-state block
			// (reset_state_all FUN_180091E40 writes through it unconditionally).
			pxd::GsBringup::EnsureDeviceContextBlock(s_deviceContext, window.GetD3D11DeviceContext());

			// The embedded display sub-object at gs+0xC0 — this generation's cswap_chain. The
			// render-target setter FUN_18009E1E0 reads it through DAT_1803914E8 (= shared symbol
			// slot[2]) when a caller asks for the DEFAULT targets (index -1): colour object at
			// +0x08, depth object at +0x10, and the packed dimensions at +0x20 / +0x40 as
			// `(w-1) | (h-1)<<14`. Same layout as the YLAD generation's cswap_chain.
			// The target objects themselves stay null (binding falls back gracefully); the dims
			// are seeded because they become the viewport width/height.
			uint8_t* sub = g_gsContext + 0xC0;
			*reinterpret_cast<void**>(sub + 0x00) = window.GetSwapChain();
			const uint32_t packedDims = pxd::GsBringup::PackDims(1280, 720);
			*reinterpret_cast<uint32_t*>(sub + 0x20) = packedDims;
			*reinterpret_cast<uint32_t*>(sub + 0x40) = packedDims;

			// The pxd shadow-state attach (an unattached block is an immediate AV WRITE at
			// DLL+0x9E34E when the render-target setter records the viewport) - shared, the GUID
			// is the same bytes in both DX11 generations. See pxd/GsBringup.h.
			pxd::GsBringup::AttachCtxShadowState(window.GetD3D11DeviceContext());

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
				constexpr uint32_t kCapacity = 16384;
				pxd::GsBringup::SeedInstanceTablesUniform(g_gsContext + 0x1808, kCapacity, 10);
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

		// Applied immediately before EVERY module_main call. A per-HOST-FRAME assert is not
		// enough, and each of the two times that was assumed it cost a two-machine run: one host
		// frame runs a BURST of module_main calls while the counter holds (all of boot), the
		// module's own bring-up zeroes the two-board gate mid-burst, and if the ROM's InitNetwork
		// runs later in the same burst it reads gate=0 and assigns NOLINK - the machine comes up
		// standalone. Asserting here means the gate is only ever down inside the one call that
		// zeroed it, and the read that matters is always calls later.
		static void AssertTwoBoardGate()
		{
			if (g_twoBoardGate != nullptr && WantTwoBoardMode())
			{
				*g_twoBoardGate = 1;
			}
		}

		int StepOneBoardFrame(execute_info_t& info)
		{
			// No virtual clock (symbol unresolved, or a module that never had one): behave exactly
			// as before, one call per host frame, real time still driving the board.
			if (!ModuleClock().Installed())
			{
				AssertTwoBoardGate();
				DriveCabinetRoleChanges();
				DriveCommFirmware();
				return g_moduleMain(sizeof(info), &info);
			}

			const uint32_t counterAddr = RomFrameCounterAddress();
			uint32_t before = 0;
			const bool haveCounter = counterAddr != 0 && ReadEmulatedRam32(counterAddr, before);

			// Without a readable counter there is nothing to stop on. One tick, one call - still an
			// improvement on real time, because the tick is the same on every machine.
			if (!haveCounter)
			{
				ModuleClock().Advance();
				AssertTwoBoardGate();
				DriveCabinetRoleChanges();
				DriveCommFirmware();
				return g_moduleMain(sizeof(info), &info);
			}

			// Bounded so a board that has stopped advancing (a crash, a halted CPU, the boot phases
			// before the ROM runs at all) cannot spin the host forever. SMALL, deliberately: the
			// frozen retries below exist to catch a service the module skipped, which lasts a call
			// or two - while a counter that simply is not moving yet (boot) should hand the host
			// frame back rather than burn hundreds of no-time calls per frame, which distorts the
			// module's per-call bring-up against its now-slower virtual time. A frame that gives
			// up here advanced nothing, which EndFrame's stall test already handles.
			constexpr int kMaxCalls = 16;

			// AND time itself is bounded, which is the two-machine fix (2026-08-04, desync at
			// frame 503): only the first three iterations tick the clock; the rest call with time
			// FROZEN. Without the cap, a call the module internally declines to service (VF2
			// measured ~5% of calls run nothing, and WHICH ones is machine-local) still got a tick,
			// so virtual time accrued across the skipped calls - and the call that finally serviced
			// ran EVERY accrued frame at once. Measured on two machines at netplay frame 503, off
			// a timer-rearm boundary: one peer's call ran rom 82->83 with 7,800 instructions, the
			// other's ran 82->84 with 15,648 - two board frames against one pad, unstoppable
			// mid-call, exactly the fault class the virtual clock exists to prevent.
			//
			// Three ticks is derived, not tuned: 3/120 s of accrued time guarantees AT LEAST one
			// frame is due for any board slower than 40 Hz, and LESS than two are due for any
			// board faster than 80 Hz - Model 2 runs ~57.5-60 Hz, comfortably inside both bounds.
			// A skipped service now just gets retried with no further time flowing, so the module
			// can never owe two frames, whatever it does internally.
			constexpr int kMaxTicks = 3;
			int result = 0;
			for (int i = 0; i < kMaxCalls; ++i)
			{
				if (i < kMaxTicks) ModuleClock().Advance();
				AssertTwoBoardGate();
				DriveCabinetRoleChanges();
				DriveCommFirmware();
				result = g_moduleMain(sizeof(info), &info);

				uint32_t after = 0;
				if (ReadEmulatedRam32(counterAddr, after) && after != before)
				{
					break;
				}
			}
			return result;
		}

		HMODULE LoadDLL()
		{
			const GameDesc& game = CurrentGame();
			gameDll.reset(LoadModuleDll(game.dll_name, game.subdir, ModuleSearch::CwdThenSubdir, gamePath));
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

			// Optional: only the linked-cabinet modules (omg, mr) have a second board at all.
			g_twoBoardGate = static_cast<uint8_t*>(symbolMap.TryGetSymbol(ImportSymbol::TWO_BOARD_GATE));
			g_boardSelect = reinterpret_cast<board_select_t>(
				symbolMap.TryGetSymbol(ImportSymbol::BOARD_SELECT));
			g_dllBase = static_cast<uint8_t*>(dll);

			// Cabinet TEST / SERVICE, so the board's own service menu - and with it the operator's
			// INPUT TEST - is reachable. Same I/O core and the same io[9] bits 2/3 as the Lost
			// Judgment modules; only the way the I/O pointer is found differs, because this
			// generation's refresh loads the board INDEX in its prologue rather than the board
			// pointer, so the global is named outright instead of decoded.
			InstallSystemSwitchesAt(dll, symbolMap.TryGetSymbol(ImportSymbol::I960_IO_REFRESH_CALL),
				symbolMap.TryGetSymbol(ImportSymbol::I960_IO_REFRESH_CALL_B1),
				reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(dll) + OmgRva::IO_STATE_PTR),
				/*multiplexedSystemPort=*/true);

			// Comm-board reset semantics, wrapped around the module's own link transfer - see
			// LinkTransferWithResetSemantics. Gated on the two-board gate as well as the pattern:
			// the same engine ships in the vf2 module, which has no comm board for the registers
			// to describe (measured: the pattern matches only omg across all six module DLLs, but
			// the gate makes that a guarantee rather than an observation). Installed here because
			// this pass holds the .text unprotect the call-site injection needs.
			if (void* xferCall = symbolMap.TryGetSymbol(ImportSymbol::LINK_TRANSFER_CALL);
				xferCall != nullptr && g_twoBoardGate != nullptr)
			{
				Memory::ReadCall(xferCall, g_origLinkTransfer);
				Trampoline* trampoline = Trampoline::MakeTrampoline(dll);
				Memory::InjectHook(xferCall, trampoline->Jump(&LinkTransferWithResetSemantics));
				DebugLogFile("[m2ftg::K2] comm-board reset semantics installed "
					"(call site %p, transfer %p)\n",
					xferCall, reinterpret_cast<void*>(g_origLinkTransfer));
			}

			// Take the module off the host's wall clock. Done here because this is the pass that
			// holds the .text unprotect, and done for local play too: the board advancing with real
			// time rather than with frames is what makes a slow host run two emulated frames in one
			// call, and that is a correctness problem before it is a netplay one.
			//
			// `-von-realclock` puts the module back on QueryPerformanceCounter and back to one
			// module_main call per host frame - i.e. exactly the behaviour before the virtual clock
			// existed. It is the A/B for "did the clock change break this?", which matters because
			// the clock moves the frame loop under everything else the host does. Netplay will
			// desync with it on; that is the point of it being a flag.
			if (wcsstr(GetCommandLineW(), L"-von-realclock") == nullptr)
			{
				ModuleClock().Install(symbolMap.TryGetSymbol(ImportSymbol::WALL_CLOCK));
			}
			else
			{
				DebugLogFile("[m2ftg::K2] -von-realclock: module left on the host wall clock\n");
			}

			// Which board the render draws. Applied per frame from the netplay role rather than
			// once at load from a command line - see SetRenderBoard.
			g_renderBoardSelect =
				static_cast<uint8_t*>(symbolMap.TryGetSymbol(ImportSymbol::RENDER_BOARD_SELECT));

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
			execute_info.sound_volume = Cabinet::VolumeFraction();

			// NETWORKING IS DELIBERATELY ABSENT FROM THIS HOST. A full VON netplay stack was
			// built here (lockstep rounds, a handshake-as-barrier round start, host-forced HLE
			// sets) and STRIPPED on 2026-08-04 after four two-machine runs in a row failed in
			// four different ways - the retrospective and everything learned is in
			// docs/von-netplay-recon.md. The next attempt starts from that document's
			// "fresh start" notes (one board per machine, the link over the wire), not from
			// code archaeology here.

			// Locked while a cabinet link is live - NEW with the Cabinet adoption: this host had
			// no pause lock at all, and pausing a linked Virtual On cabinet mid-ring stops the
			// board and trips the peer's link watchdog (the ROM's own error accounting) within
			// seconds, exactly the reason MrLink locks the LJ host's pause.
			static Cabinet::PauseMenu s_pause;
			s_pause.Poll(LinkIsLive());
			if (s_pause.open) execute_info.status |= 1;

			// Input: refresh the shared XInput snapshot, then evaluate each player's bindings via
			// csl_pad — the same Controls page every other m2ftg host uses. This generation embeds
			// the plain csl_pad in execute_info, so the whole struct goes across rather than LJ's
			// 0xE0 prefix.
			Input::PollPads();
			static pxd::csl_pad s_pads[2];
			Cabinet::RoutePads(s_pads, false, -1);   // no netplay on this host - no slot swap

			// THE SEGA SATURN TWIN STICK OVERRIDE. A real twin stick has one switch per cabinet
			// input, so when one is attached it replaces that player's binding-driven pad outright
			// rather than being mapped onto it - see VonTwinStick.cpp. Runs after RoutePads (which
			// it overwrites) and before the coin/start dance (which still ORs its own bits in, so
			// starting a credit works exactly as it does on a pad).
			bool twinStickActive = false;
			if (TwinStick::Enabled())
			{
				Input::BlissBox::Start();   // idempotent; the worker only exists while wanted
				for (int p = 0; p < 2; p++)
				{
					const int port = TwinStick::PortForPlayer(p);
					twinStickActive |= port >= 0 && TwinStick::SetPadState(s_pads[p], port);
				}
			}

			for (int p = 0; p < 2; p++)
			{
				execute_info.pad[p] = s_pads[p];
				execute_info.pad[p].m_port = static_cast<unsigned int>(p);
				execute_info.pad[p].m_user_id = p;
				execute_info.pad[p].m_is_connected = true;
			}

			// The fixed module-facing assignment table - see Cabinet::FillAssignTable.
			Cabinet::FillAssignTable(execute_info);

			// VIRTUAL ON DOES NOT USE assign[0][4] AS A BUTTON ASSIGNMENT. It reads that one byte -
			// execute_info + 0x15E4 - as an INDEX into its own control-mapping table, and that is
			// why no input of any kind worked: not a button, not Start, not the service switch.
			//
			// omg 0x180004BE0 is the per-frame input update. Its first act is
			// `scheme = *(u8*)(execute_info + 0x15E4)` followed by a 0xC0-byte copy out of the table
			// at 0x18012ABB0 into the globals the pad decoder reads; only then does it call the
			// execute_info pad reader (0x180081140). The table holds {host button mask -> Model 2
			// button code} pairs, twelve to an entry.
			//
			// **The table has five entries, 0 to 4.** Entries 0-4 all begin with the same valid
			// pairs (0x10->0x07, 0x20->0x09, 0x40->0x08, 0x80->0x0A); entry 5 onwards is an array of
			// POINTERS belonging to something else. Input::MODULE_ASSIGN[4] is 6 - the P+K+G slot
			// value, correct for the fighting games and two entries off the end here - so Virtual On
			// was decoding every pad through whatever those pointers happen to be.
			//
			// SCHEME 3 IS THE FULL TWIN-STICK MAPPING. Virtual On is a twin-lever cabinet, and the
			// five entries are five ways of driving those levers from a pad. Decoded from the table
			// itself (0x6E is the "unmapped" filler):
			//
			//   entry 0/1  face buttons and D-pad produce COMBO codes (0x70-0x77) - one button
			//              expands into a two-stick gesture. Confirmed against the board's own
			//              input test: Punch read as "right on the left stick + left on the right"
			//              (both inward = crouch), Guard as both outward (jump), Kick as a turbo.
			//   entry 2/4  directions only partly routed; most masks are the filler.
			//   entry 3    every direction DISCRETE - D-pad -> left stick (codes 3/6/4/5), face
			//              buttons -> right stick (codes B/E/C/D), shoulders -> the triggers and
			//              turbos (0x10/0x20/0x40/0x80 -> 7/9/8/A), which every entry shares.
			//
			// So 0 is a beginner scheme that can only ever reach the gestures someone chose to
			// pre-compose, and 3 is the cabinet's actual control set. Choosing 3 means a player can
			// produce every stick position, which is the difference between playing the game and
			// operating a shortcut menu.
			//
			// WHICH of the five is now the player's choice ([VirtualOn] ControlScheme, settings
			// page "Control type") - the beginner gestures are a legitimate way to play, and the
			// module re-latches this byte every frame, so the pick applies live. Clamped here as
			// well as at ini load, because an index past the table's five entries decodes every
			// pad through the pointer data that follows it - the original "no input at all" bug.
			if (gGeneral.GetGameId() == YAMPGeneral::GameId::VON_K2)
			{
				const auto* settings = gGeneral.GetSettings();
				uint32_t scheme =
					settings != nullptr && settings->m_vonControlScheme <= 4
						? settings->m_vonControlScheme : 3;
				// A live twin stick pins the scheme to the discrete entry, whatever the combo
				// says: the beginner entries route the stick's own directions into pre-composed
				// gestures, which is precisely the translation a real lever exists to avoid.
				// THE BYTE IS PER CABINET, NOT PER PLAYER, so a stick on either side puts both on
				// entry 3 - the module has one scheme selector and no way to split it.
				if (twinStickActive)
				{
					scheme = 3;
				}
				execute_info.assign[0][4] = static_cast<unsigned char>(scheme);
			}

			const bool advanceFrame = true;

			// Before the board is stepped, so the link the exchange asks about is this frame's.
			DriveNetSession();

			// Synthetic START, before the coin/start dance so it takes the same path a real press
			// does. Logged, because a run that crashes has to say what it did first.
			if (const int at = AutoStartFrame();
				at > 0 && s_frame >= at && s_frame < at + AUTOSTART_HOLD_FRAMES)
			{
				execute_info.pad[0].m_now |= 0x100;
				if (s_frame == at)
				{
					execute_info.pad[0].m_push |= 0x100;
					net::Logf("[von] AUTOSTART: pressing START on pad 0 at frame %d", s_frame);
				}
			}

			// Dedicated coin binding -> the coin status bit (host->module bit5). See
			// Cabinet::CoinBinding.
			static Cabinet::CoinBinding s_coinButton;
			if (s_coinButton.Pressed(s_isFreeplay || s_pause.open))
			{
				execute_info.status |= 0x20;
			}

			// The arcade coin/start dance - Cabinet::CoinStart (the write-up is with the class).
			static Cabinet::CoinStart s_coinStart;
			if (!s_isFreeplay && !s_pause.open)
			{
				s_coinStart.Run(execute_info.pad[0], execute_info.status);
			}

			window.BeginFrame();
			window.NewImGuiFrame();
			if (s_pause.open && !DrawPauseMenu(window, s_pause.open))
			{
				return false;   // Quit picked
			}

			// -von-modes: the ROM's mode machine and the cabinet switch lines, live. Drawn here
			// rather than on the settings page because the switches are suppressed while that page
			// is open - see WantModeOverlay in VonBoard.h.
			if (WantModeOverlay())
			{
				DrawModeOverlay(s_pause.open);
			}

			// The two-board gate is a LAUNCH-TIME choice (-von-2board), never a live toggle: an
			// idle second cabinet is a peer the ROM waits for, and flipping the gate on a running
			// board is the transition every crash in the netplay era traced back to. Per-frame
			// reassert because the module's own mode machine zeroes the gate during bring-up (and
			// the per-CALL assert in StepOneBoardFrame covers the bursts - see AssertTwoBoardGate).
			const bool gateOn = WantTwoBoardMode();
			if (g_twoBoardGate != nullptr && gateOn) *g_twoBoardGate = 1;

			// Which cabinet the render draws - a debug flag only (-von-render1).
			SetRenderBoard(WantRenderBoard1() ? 1 : 0);

			// Cabinet service panel - see Cabinet::PollSystemSwitches. A role change drives TEST
			// itself (SoftResetIntoRole), passed as forceTest so a user holding the real switch
			// is never fought and the reset is never suppressed by the pause menu.
			Cabinet::PollSystemSwitches(s_pause.open, SetSystemSwitches, DriveRoleResetSwitch());

			// Input-routing probe: hold a pattern on pad[1] and nothing on pad[0]. If player N's
			// input reaches board N, the two boards' I/O bytes have to diverge; if both boards read
			// pad[0], they stay identical no matter what pad[1] does.
			if (WantPadTest())
			{
				execute_info.pad[1].m_now = 0x000F;   // all four directions, unmistakable
				execute_info.pad[1].m_push = 0x000F;
				execute_info.pad[0].m_now = 0;
				execute_info.pad[0].m_push = 0;
			}

			static unsigned int s_lastOutputTexId = 0;
			int funcResult = 0;
			if (advanceFrame)
			{
				// KEEP THE BOARD AT 60 Hz WHILE A LINK IS LIVE, however slowly we present.
				//
				// One board frame per host frame makes game SPEED a function of the display, and
				// against a linked partner that is a competitive advantage rather than a cosmetic
				// difference - the cabinet running at 32 Hz has a pilot that moves and fires at
				// half the rate of the one at 60, and loses for a reason that is not the match.
				// See VirtualClock::BoardFramesDue; normally 1, capped at 4.
				const unsigned int due = ModuleClock().BoardFramesDue(LinkIsLive());
				for (unsigned int i = 0; i < due; ++i)
				{
					funcResult = StepOneBoardFrame(execute_info);
				}
				if (execute_info.output_texid != 0) s_lastOutputTexId = execute_info.output_texid;

				// Per-chunk work-RAM map behind the probe flag: sixteen 0x10000 chunks across the
				// i960 0x500000 window, per board - how a divergence between the two boards (or
				// between two runs) gets localised to a 64 KB chunk.
				if (g_dllBase != nullptr && WantWorkRamMap())
				{
					char line[16 * 9 + 1];
					for (int b = 0; b < 2; ++b)
					{
						int n = 0;
						for (int c = 0; c < 16; ++c)
						{
							n += snprintf(line + n, sizeof(line) - n, "%08X ",
								Hash32(I960At(g_dllBase, b, 0x500000 + c * 0x10000), 0x10000));
						}
						net::Logf("[vonmap] b%d %s", b, line);
					}
				}
			}
			else
			{
				execute_info.output_texid = s_lastOutputTexId;
			}

			const int interesting = execute_info.status | (funcResult << 16);
			if (interesting != s_lastLogged && s_frame < 5000)
			{
				DebugLogFile("[m2ftg::K2] frame=%d status=0x%X result=0x%X texid=%u ret=0x%X\n",
					s_frame, execute_info.status, execute_info.result,
					execute_info.output_texid, funcResult);
				s_lastLogged = interesting;
			}
			// Sample AFTER module_main, so each delta covers exactly one call.
			if (WantFindCounter() && g_dllBase != nullptr)
			{
				FindCtr::Sample(g_dllBase, 0);
				if (s_frame == 1)   FindCtr::SnapGlobals(g_dllBase);
				if (s_frame == 900) { FindCtr::Report(0); FindCtr::ReportGlobals(g_dllBase); }
			}
			if (WantTwoBoardProbe() && (s_frame % 200) == 0)
			{
				LogTwoBoardState(s_frame, execute_info.output_texid);
			}
			// Every host frame, unconditionally: it ages the link's peer timeout as well as
			// logging, and the log half gates itself on the setting.
			LogLinkState(s_frame);
			s_frame++;
			// status bit6 = the attract/insert-coin screen, the state the coin dance needs.
			s_coinStart.NoteStatus(execute_info.status);

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

			// MUST PRECEDE module_start, and this is the only call that can reach Virtual On's boot
			// path. The per-frame reconcile in the UI draw is a whole module_main behind the first
			// frame of emulation, so hooks 0-3 - the bring-up handshake, the vblank edge, the frame
			// yield and the cabinet-type branch - have all fired before it runs once, and turning
			// any of them off there tests nothing. Legal this early because the handler column is
			// static .data the installer never touches (see HleHooks::Update), so it is already the
			// live table before bring-up has read a byte of it.
			m2ftg::HleHooks::Update();

			// Same window, and for the same reason: the ROM reads the cabinet byte in InitNetwork,
			// which is hundreds of frames before anything in the UI can run.
			ApplyCabinetRole(gameDll.get(), settings->m_vonCabinetRole);

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

			// Throw the second-board switch, AFTER module_start: state 0x10 of the module's own
			// mode machine zeroes this byte (along with the board index and the link-enabled
			// gate), so setting it earlier would simply be undone during bring-up.
			//
			// The gate lives in .data, which is already writable - no ScopedUnprotect needed, and
			// the ones taken during symbol resolution are long out of scope by here.
			if (g_twoBoardGate != nullptr && WantTwoBoardMode())
			{
				*g_twoBoardGate = 1;
				DebugLogFile("[m2ftg::K2] two-board mode ENABLED (gate %p = 1)\n",
					static_cast<void*>(g_twoBoardGate));
			}
			else if (WantTwoBoardMode())
			{
				DebugLogFile("[m2ftg::K2] -von-2board asked for, but this module has no two-board gate\n");
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

					// PACE THE LOOP TO THE BOARD, not to a fixed 60. Every other host caps here with
					// the "Enable 60 FPS cap" setting; this one never read it, which is why Virtual
					// On has always run uncapped - and why two peers at different frame rates fed
					// the module's real-time pacing wildly different numbers of emulated frames per
					// call. That is the desync, upstream of anything netplay did.
					//
					// The cap setting is deliberately not consulted. With the module's clock now
					// driven by frames, the loop rate IS the game speed, so an uncapped loop does
					// not render more of the same game - it runs Virtual On three times too fast.
					// There is nothing here left to turn off. See VirtualClock::PaceToVirtualTime.
					ModuleClock().PaceToVirtualTime();
				}

				const int stopResult = module_stop(0, nullptr);
				DebugLogFile("[m2ftg::K2] module_stop -> 0x%X\n", stopResult);
			}
		}
	}
}
