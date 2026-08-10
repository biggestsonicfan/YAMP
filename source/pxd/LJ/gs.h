#pragma once

#include <cstdint>
#include <cstddef>

#include <d3d11.h>
#include <xmmintrin.h>

#include "../pxd_shader.h"
#include "pxd_types.h"

class RenderWindow;

namespace pxd
{

#include "../gs_shared_sbgl.inc"
				unsigned int m_max_frame_latency;
				ID3D11Query* m_pD3DQuery;
				//__m128 m_fast_clear_color; // YLAD only
				__m128 m_p_border_color[256];
				DXGI_ADAPTER_DESC1 m_DXGIAdapterDesc;
			};
			static_assert(offsetof(cdevice_native, m_pD3DDeviceContext) == 0x90);
			static_assert(offsetof(cdevice_native, m_context_desc) == 0x98);
			static_assert(offsetof(cdevice_native, m_mwa_flags) == 0xF4);
			static_assert(offsetof(cdevice_native, m_max_frame_latency) == 0xF8);
			static_assert(offsetof(cdevice_native, m_pD3DQuery) == 0x100);
			static_assert(offsetof(cdevice_native, m_p_border_color) == 0x110);

			class cdevice : public cdevice_native
			{
			public:
				struct shared_symbol_st
				{
					void* p_value[9];
#include "../gs_shared_objects.inc"
			// The real pxd cgs_device_context is FAR larger than the old 7952-byte guess: the
			// DLL's flush/reset paths touch fields out to ~+0x14c94 (e.g. FUN_180089fa0 @+0x14bf8,
			// FUN_180088cd0's viewport/scissor batch arrays at +0x14b84 + count*0x10). YAMP `new`s
			// this object, so under-sizing it corrupts adjacent heap. Size to 0x16000 (headroom
			// over the observed max). Named fields above keep their offsets (gap trails them).
			std::byte gap[0x16000 - 0x40];

			static inline void(__thiscall* reset_state_all_internal)(cgs_device_context* obj);
		};
		static_assert(sizeof(cgs_device_context) == 0x16000);
		static_assert(offsetof(cgs_device_context, mp_sbgl_context) == 24);
		static_assert(offsetof(cgs_device_context, mp_shader_uniform) == 56);

		namespace gs {

			enum vb_usage_t
			{
				VB_USAGE_DEFAULT = 0x120,
				VB_USAGE_IMMUTABLE = 0x101,
				VB_USAGE_DYNAMIC = 0x4400,
				VB_USAGE_STAGING = 0x3B,
			};

			enum ib_usage_t
			{
				IB_USAGE_DEFAULT = 0x220,
				IB_USAGE_IMMUTABLE = 0x201,
				IB_USAGE_DYNAMIC = 0x4800,
				IB_USAGE_STAGING = 0x3B,
			};

			struct export_context_t
			{
				size_t size_of_struct;
				void* p_context;
				sbgl::cdevice::shared_symbol_st sbgl_context;
			};

			// ---- The two shipped layouts of this context -------------------------------------
			//
			// Two pxd builds host the m2ftg/VF5FS modules through this path and they do NOT agree
			// on the size of the context: Lost Judgment / Y:LAD (2021-2022 builds) use 0x388A00,
			// Like a Dragon Gaiden (2023-11-27) uses 0x3898C0. Diffing the two constructors
			// instruction-by-instruction (1005 of 1013 instructions align - it is the same
			// function recompiled) shows the 0xEC0 difference is FIVE separate insertions:
			//
			//     +0x20  before 0x7530  \
			//     +0x08  before 0x7558   >  all three inside gap18, below
			//     +0xB80 before 0x7618  /
			//     +0x18  before 0x107C00 \
			//     +0x98  before 0x3885F0  >  past the end of what this struct models
			//     +0x268 before 0x388648 /
			//
			// This struct stops at handle_fx (0x107BD8) - it always modelled a PREFIX of the real
			// context, which is why gap23 below is commented out - so the last three insertions
			// are invisible here. The first three all land in one padding array. That is the whole
			// difference: every field from p_vb_sphere onward sits +0xBA8 further along in the
			// Gaiden build, and everything above gap18 is identical in both.
			//
			// So the layout is templated on that one gap rather than duplicated, and the offsets
			// of BOTH instantiations are locked by static_asserts in gs.cpp. GAP18_LJ/GAP18_GAIDEN
			// are the only numbers that differ.
			inline constexpr size_t GAP18_LJ = 15224;            // context 0x388A00
			inline constexpr size_t GAP18_GAIDEN = 15224 + 0xBA8; // context 0x3898C0

			template <size_t Gap18>
			struct context_tmpl
			{
				uint32_t tag_id; // 0x7367424c
				uint32_t version; // 0x41601
				uint32_t size_of_struct; // Should be 3705280 when complete, 0x180588AC0
				uint32_t sbgl_initialize_flags;
				export_context_t export_context; // sbgl_context is 8 bytes larger than VF5FS
				unsigned int frame_counter;
				unsigned int max_frame_latency;
				// +0x70. Read by pre3's screen object to size its scroll-buffer ring (see
				// pre3/Gaiden/Pre3Host.cpp); no m2ftg or VF5FS module touches it, which is why
				// it stayed unwritten for so long. The offset is asserted below.
				unsigned int backbuffer_count;
				unsigned int unk_int1;
				csl_allocator* p_csl_allocator;
				csl_allocator* p_csl_allocator_with_pool;
				std::byte gap1[40];
				uint64_t* _p_isl_allocator; //Unpopulated
				uint64_t* unpop_pointer2;
				uint64_t* unpop_pointer3;
				uint64_t one;
				uint64_t* unpop_pointer4;
				std::byte FFs[8];
				std::byte gap2[120];
				uint64_t four;
				std::byte gap3[136];
				float one1;
				float zero1;
				float zero2;
				float zero3;
				float ones[4];
				float NaNs[1008];
				std::byte gap4[128];
				std::byte FFs1[8];
				std::byte gap5[8];
				std::byte FFs2[8];
				std::byte gap6[368];
				std::byte garbage[8];
				std::byte gap7[144];
				important_thing1* csystem_command_list;
				std::byte gap8[32];
				uint64_t* p_after_eight;
				uint64_t eight;
				std::byte gap9[80];
				uint64_t* p_after_sixteen;
				uint64_t sixteen;
				std::byte gap10[1048];
				uint64_t* p_after_sixtyfour;
				uint64_t sixtyfour;
				std::byte gap11[608];
				uint64_t* p_after_sixtyfour2;
				uint64_t sixtyfour2;
				std::byte gap12[560];
				uint64_t* p_after_sixtyfour3;
				uint64_t sixtyfour3;
				// Skip a bit, brother
				std::byte gap13[1096];
				uint64_t* unknown_table_ptr2_0; //0x180118C88 in newest dll
				std::byte gap14[608];
				uint64_t* unknown_table_ptr2_1; //0x180118C88 in newest dll
				std::byte gap15[1368];
				uint64_t* unknown_table_ptr2_2; //0x180118C88 in newest dll
				std::byte gap16[608];
				uint64_t* unknown_table_ptr2_3; //0x180118C88 in newest dll
				// Skip a bit more, brother
				std::byte gap17[3688];
				uint64_t* unknown_table_ptr3; //0x1801192D0 in newest dll
				// THE build-variant gap - see GAP18_LJ / GAP18_GAIDEN above. Everything below this
				// line is at +0xBA8 in the Gaiden build and unchanged in the LJ/Y:LAD one.
				std::byte gap18[Gap18];
				// The shared immediate-mode primitive buffers (primitive_initialize fills them).
				// These sit BEFORE p_device_context, not after sbgl_device where gs.h used to put
				// them: Motor Raid's 2D quad pusher reads p_ib_quad straight off the context
				// (FUN_18009C829 `mov rcx,[gs_context+0x7670]`, the module-stored pointer from
				// gs::initialize_module, no +8 skew), then binds *(it+0x18) as the index buffer.
				// At the old offset (+0x89E0) the DLL read a zero and null-dereferenced on MR's
				// first 2D quad; StF and FV never touch the field, which is why it went unnoticed.
				cgs_vb* p_vb_sphere[3];   // +0x7640
				cgs_vb* p_vb_capsule[3];  // +0x7658
				cgs_ib* p_ib_quad;        // +0x7670
				cgs_ib* p_ib_fan;         // +0x7678
				// +0x7680: the rect (triangle-STRIP) index buffer. Confirmed used by LJ's VF5FS
				// module: its 2D rect pusher FUN_18023D740 reads `[gs_context+0x7680]`, binds
				// *(it+0x18) as the index buffer with format *(it+0xc), and draws 4 indices at
				// TRIANGLESTRIP topology — the strip twin of the p_ib_quad pusher FUN_18023D590,
				// which reads +0x7670 and draws TRIANGLELIST. Left unset it null-dereferences on the
				// module's first rect (AV reading [null+0x10] at DLL+0x23D7B1).
				cgs_ib* p_ib_rect;        // +0x7680
				std::byte gap18b[96];
				uint64_t unk_variable1; //0x48674BC7h
				std::byte gap19[64];
				uint64_t unk_variable2; //0x48674BC7h
				cgs_device_context* p_device_context;
				sbgl::cdevice sbgl_device;
				std::byte gap20[32];
				std::byte gap21[160];
				t_lockfree_stack<cgs_cb_pool> stack_cb_pool;
				t_lockfree_stack<cgs_up_pool> stack_up_pool;
				t_lockfree_stack<cgs_shader_uniform> stack_shader_uniform;
				// The real DX12 pxd context embeds two 0x10000-entry descriptor-handle arrays
				// (0x80000 bytes each; built by the DLL ctor at ~+0x7A10 and +0x87A10) plus
				// heap-allocator state between the pools and the handle tables. gs.h originally
				// omitted them, collapsing the tail by ~1MB and mis-placing every handle table,
				// which made the DLL's t_instance_tbl allocator (FUN_18008e6d0) int3-assert.
				// This padding restores the true offsets so handle_mesh lands at +0x107A98.
				// Verified against a live LJ gs context dump + the DLL constructor; the offsets
				// are locked by static_asserts in gs.cpp. See scratchpad/live-gscontext-spec.md.
				// This struct's alignment is 16 (several members below carry SIMD types), and the
				// Gaiden gap18 delta of 0xBA8 is 8 mod 16 — so the compiler re-aligns the first
				// 16-aligned member after gap18 and silently inserts 8 bytes of padding that the
				// REAL context does not have. Measured: handle_mesh comes out at 0x108648 where
				// the module puts it at 0x108640. Shrinking this gap by the same misalignment
				// cancels the padding exactly, for any future build's delta as well as this one
				// (the term is 0 whenever a delta is a clean multiple of 16, so the LJ layout is
				// bit-for-bit what it always was). The static_asserts in gs.cpp check both.
				std::byte gap_descriptor_handle_arrays[0xFF030 - ((Gap18 - GAP18_LJ) % 16)];
				t_instance_tbl<cgs_mesh> handle_mesh; // +0x107A98
				t_instance_tbl<cgs_tex> handle_tex;
				t_instance_tbl<cgs_vs> handle_vs;
				t_instance_tbl<cgs_ps> handle_ps;
				t_instance_tbl<cgs_gs> handle_gs;
				t_instance_tbl<cgs_hs> handle_hs;
				t_instance_tbl<cgs_ds> handle_ds;
				t_instance_tbl<cgs_cs> handle_cs;
				t_instance_tbl<cgs_gts> handle_gts;
				t_instance_tbl<cgs_fx> handle_fx;
				//std::byte gap23[231169];
			};

			// The LJ / Y:LAD layout keeps the name `context_t`, so every existing use site and
			// every host but the Gaiden one is untouched.
			using context_t = context_tmpl<GAP18_LJ>;
			using context_gaiden_t = context_tmpl<GAP18_GAIDEN>;

			// Translate a RAW gs-context offset — one written as a literal rather than reached
			// through a named member — from the LJ layout into the layout `Gap18` describes.
			//
			// The descriptor blocks, the copy rings and the tex-id tables are all addressed this
			// way (they live inside padding, so there is no member to name), and they do NOT all
			// move by the same amount: the three insertions this struct models fall at 0x7530,
			// 0x7558 and 0x7618, so a ring at 0x7550 shifts by 0x20 while the block at 0x7870
			// shifts by 0xBA8. Getting this wrong writes a valid-looking descriptor heap into the
			// wrong field and fails later, inside the GPU driver, with no hint of the real cause.
			//
			// Boundaries are the measured insertion points; see the table at the top of this file.
			// ALL FIVE bands are covered, not just the three the struct itself spans: the host
			// also writes raw offsets far past handle_fx (the transient upload ring at 0x188200
			// is in the fourth band), and those move by a different amount again.
			template <size_t Gap18>
			constexpr size_t raw_off(size_t ljOffset)
			{
				if (Gap18 == GAP18_LJ) return ljOffset;
				if (ljOffset < 0x7530)   return ljOffset;             // header: identical in both
				// Insertion 2 is a NEW FIELD rather than padding, and it sits between the
				// current-ring pointer and the ring array: Gaiden's copy-ring block reads
				// `cmp dword [gs+0x7578],1` to decide whether to rotate rings, with the rings
				// themselves at 0x7580 (= LJ 0x7558 + 0x28) and the pointer still at 0x7570
				// (= LJ 0x7550 + 0x20). So the boundary is 0x7558, NOT 0x7550 — putting it at
				// 0x7550 writes the ring pointer over that count and the module rotates into a
				// ring it never initialised.
				if (ljOffset < 0x7558)   return ljOffset + 0x20;      // after insertion 1
				if (ljOffset < 0x7618)   return ljOffset + 0x28;      // ...2
				if (ljOffset < 0x107C00) return ljOffset + 0xBA8;     // ...3 (every named member)
				if (ljOffset < 0x3885F0) return ljOffset + 0xBC0;     // ...4 (the upload ring)
				if (ljOffset < 0x388648) return ljOffset + 0xC58;     // ...5
				return ljOffset + 0xEC0;                              // tail; = the size delta
			}

			// The size the module's own initialize_module checks the context header against
			// (`cmp dword [rcx+8], <size>` in gs::initialize_module). It is NOT sizeof() of the
			// structs above - those deliberately model only the prefix up to handle_fx - so it has
			// to be stated per build. A mismatch makes the module quietly fall back to its own
			// embedded context and render nothing the host can see.
			// `up_ring_bytes` is the transient vertex-upload ring the host allocates and hands to
			// the module at gs+0x188208. Its size is NOT a host choice — the module's sub-allocator
			// masks the cursor and wraps it against a hard-coded limit, so the buffer has to be at
			// least that big or the ring walks straight off the end and `rep movsq` writes into
			// unmapped memory. The limit is in 16-byte units: Lost Judgment `and 0x1FFFFF` /
			// `cmp 0x200000` = 32 MB, Gaiden `and 0x7FFFFF` / `cmp 0x800000` = 128 MB.
			template <size_t Gap18> struct context_traits;
			template <> struct context_traits<GAP18_LJ>     {
				static constexpr uint32_t size_of_struct = 0x388A00;
				static constexpr uint64_t up_ring_bytes = 0x2000000;   // 32 MB
			};
			template <> struct context_traits<GAP18_GAIDEN> {
				static constexpr uint32_t size_of_struct = 0x3898C0;
				static constexpr uint64_t up_ring_bytes = 0x8000000;   // 128 MB
			};

			// pre3's screen object reads backbuffer_count by raw offset, so pin it.
			static_assert(offsetof(context_tmpl<GAP18_LJ>, backbuffer_count) == 0x70);
			static_assert(offsetof(context_tmpl<GAP18_GAIDEN>, backbuffer_count) == 0x70);

			void primitive_initialize();

			extern cgs_vb* (*vb_create)(uint64_t fvf, unsigned int vertices, vb_usage_t usage, unsigned int flags, const void* p_initial_data, const char* sz_name);
			extern cgs_ib* (*ib_create)(unsigned int indices, ib_usage_t usage, unsigned int flags, const void* p_initial_data, const char* sz_name);

			// The module's own static context instance, as imported by the host. It is typed to
			// the LJ layout because that is what every field ABOVE gap18 agrees on in both builds
			// - reading tag_id/version/size_of_struct/frame_counter through it is correct either
			// way. Fields BELOW gap18 must not be reached through it directly; use the accessors.
			extern context_t* sm_context;

			// ---- Build-variant field access ---------------------------------------------------
			//
			// Set once at module load, from the module build the verifier identified. Only the
			// fields below gap18 care, and only the host reads them, so a single flag plus these
			// inline accessors keeps ONE host code path instead of templating the whole host.
			extern bool sm_is_gaiden_layout;

			// Everything past gap18, reached with the running build's layout.
			template <typename Fn>
			inline decltype(auto) with_tail(Fn&& fn)
			{
				if (sm_is_gaiden_layout)
				{
					return fn(reinterpret_cast<context_gaiden_t*>(sm_context));
				}
				return fn(sm_context);
			}

			cgs_device_context* device_context();
			t_instance_tbl<cgs_tex>& handle_tex();
			sbgl::cdevice& sbgl_device();

			// ---- Host-provided cgs_device_context slots ---------------------------------------
			//
			// Six pointers the LJ host plants inside the device context during device-start and
			// the module then only ever READS. They live inside the object's gap rather than as
			// named members, so they are reached by raw offset — and unlike the gs context, this
			// struct was relaid out NON-uniformly by the Gaiden rebuild (measured deltas of
			// +0x18/+0x20/+0x28/+0x30/+0xF0 in different regions), so each one is stated per build
			// rather than derived from a single shift.
			//
			// Getting one wrong is a null-deref inside the module, not a YAMP-side error: the
			// render-state block is what reset_state_all writes through, so a stale offset there
			// faults at `vmovups [rcx+0x670],xmm1` with rcx = 0.
			struct DeviceContextOffsets
			{
				size_t render_command_ctx;   // the "command recording" context (+0x10 = cmd list)
				size_t cbv_srv_ring;         // CBV/SRV descriptor-copy ring state
				size_t sampler_ring;         // SAMPLER descriptor-ring binding cache
				size_t up_cache_table;       // transient vertex-upload ring cache table
				size_t push_up_pool;         // the immediate-mode quad pusher's own cgs_up_pool
				size_t render_state_block;   // reset_state_all's graphics-defaults block
				// Gaiden ONLY (0 = this build has no such field). The 2023 engine caches the
				// "current" descriptor-copy ring in the device context instead of re-reading
				// gs+0x7550 every time, and it reads that cache UNCONDITIONALLY — before the
				// branch that would refresh it — so the host has to seed it. Lost Judgment has no
				// counterpart: its copy-ring code loads gs+0x7550 directly at each use.
				size_t current_copy_ring;
			};

			inline constexpr DeviceContextOffsets DC_OFFSETS_LJ = {
				0xC8, 0x11878, 0x11ba0, 0x12c88, 0x12c90, 0x12c98, 0,
			};
			// Each of these was pinned against the Gaiden module itself rather than shifted by a
			// guess, because the three regions move by different amounts (+0x18 / +0x20 / +0x28):
			//   render_command_ctx  `mov r64,[reg+disp]; mov r64,[r64+0x10]` occurs at disp 0xC8
			//                       in the LJ DLL and nowhere in the Gaiden one, where it is 0xE0.
			//   cbv_srv_ring        the immediate 0x11878 appears at 13 sites in LJ; 0x11898
			//                       appears at exactly 13 in Gaiden.
			//   sampler_ring        0x11ba0 appears once in LJ; of the candidate shifts only
			//                       0x11bc0 appears in Gaiden at all.
			//   the three 0x12cxx   +0x28, which the first Gaiden boot proved the hard way: at the
			//                       old offset reset_state_all faulted on `vmovups [rcx+0x670]`
			//                       with rcx = 0, because the render-state block was never set.
			inline constexpr DeviceContextOffsets DC_OFFSETS_GAIDEN = {
				0xE0, 0x11898, 0x11bc0, 0x12cb0, 0x12cb8, 0x12cc0, 0x12ca0,
			};

			extern DeviceContextOffsets sm_dc;
		}
	}

