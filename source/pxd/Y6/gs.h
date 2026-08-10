#pragma once

#include <cstdint>
#include <cstddef>

#include <d3d11.h>
#include <xmmintrin.h>

#include "../pxd_shader.h"
#include "../LJ/pxd_types.h"

class RenderWindow;

namespace vf5fs
{
	namespace Y6
	{
		// The shared type layer (pxd/LJ/pxd_types.h - the Y6 copy was deleted in the
		// 2026-08-09 de-fork), under this generation's names.
		using pxd::spinlock_t;
		using pxd::rwspinlock_t;
		using pxd::recursive_rwspinlock_t;
		using pxd::t_instance_tbl;
		using pxd::t_fixed_deque;
		using pxd::t_lockfree_stack;
		using pxd::t_avl_tree_node;
		using pxd::t_status_ptr;
		using pxd::t_lockfree_ptr;
		using pxd::csl_hash;
		using pxd::sl::handle_t;
#include "../gs_shared_sbgl.inc"
				unsigned int m_max_frame_latency;
#ifdef _Y6_DAY1_DLL // Removed in a patch
				timestamp_st m_timestamp;
#endif
				ID3D11Query* m_pD3DQuery;
				//__m128 m_fast_clear_color; // YLAD only
				__m128 m_p_border_color[256];
				DXGI_ADAPTER_DESC1 m_DXGIAdapterDesc;
			};
			static_assert(offsetof(cdevice_native, m_pD3DDeviceContext) == 0x90);
			static_assert(offsetof(cdevice_native, m_context_desc) == 0x98);
			static_assert(offsetof(cdevice_native, m_mwa_flags) == 0xF4);
			static_assert(offsetof(cdevice_native, m_max_frame_latency) == 0xF8);
#ifdef _Y6_DAY1_DLL // TODO: Make this dynamic - this struct changed in a patch
			static_assert(offsetof(cdevice_native, m_pD3DQuery) == 0x118);
			static_assert(offsetof(cdevice_native, m_p_border_color) == 0x120);
#else
			static_assert(offsetof(cdevice_native, m_pD3DQuery) == 0x100);
			static_assert(offsetof(cdevice_native, m_p_border_color) == 0x110);
#endif

			class cdevice : public cdevice_native
			{
			public:
				struct shared_symbol_st
				{
					void* p_value[8];
#include "../gs_shared_objects.inc"
			std::byte gap[7882];

			static inline void(__thiscall* reset_state_all_internal)(cgs_device_context* obj);
		};
		static_assert(sizeof(cgs_device_context) == 7952);
		static_assert(offsetof(cgs_device_context, mp_sbgl_context) == 24);
		static_assert(offsetof(cgs_device_context, mp_shader_uniform) == 56);

		namespace gs {

			enum vb_usage_t
			{
				VB_USAGE_DEFAULT = 0x120,
				VB_USAGE_IMMUTABLE = 0x101,
				VB_USAGE_DYNAMIC = 0x10A,
				VB_USAGE_STAGING = 0x3B,
			};

			enum ib_usage_t
			{
				IB_USAGE_DEFAULT = 0x220,
				IB_USAGE_IMMUTABLE = 0x201,
				IB_USAGE_DYNAMIC = 0x20A,
				IB_USAGE_STAGING = 0x3B,
			};

			struct export_context_t
			{
				size_t size_of_struct;
				void* p_context;
				sbgl::cdevice::shared_symbol_st sbgl_context;
			};

			struct context_t
			{
				uint32_t tag_id; // 0x7367424C
				uint32_t version; // 0x40601
				uint32_t size_of_struct; // Should be 8128 when complete
				uint32_t sbgl_initialize_flags;
				export_context_t export_context;
				unsigned int frame_counter;
				std::byte gap[76];
				cgs_device_context* p_device_context;
				sbgl::cdevice sbgl_device;
				std::byte gap2[32];
				cgs_vb* p_vb_sphere[3];
				cgs_vb* p_vb_capsule[3];
				cgs_ib* p_ib_quad;
				cgs_ib* p_ib_fan;
				//cgs_ib *p_ib_rect; // YLAD only
				std::byte gap3[96];
				t_lockfree_stack<cgs_cb_pool> stack_cb_pool;
				t_lockfree_stack<cgs_up_pool> stack_up_pool;
				t_lockfree_stack<cgs_shader_uniform> stack_shader_uniform;
				std::byte gap4[808];
				t_instance_tbl<cgs_mesh> handle_mesh;
				t_instance_tbl<cgs_tex> handle_tex;
				t_instance_tbl<cgs_vs> handle_vs;
				t_instance_tbl<cgs_ps> handle_ps;
				t_instance_tbl<cgs_gs> handle_gs;
				t_instance_tbl<cgs_hs> handle_hs;
				t_instance_tbl<cgs_ds> handle_ds;
				t_instance_tbl<cgs_cs> handle_cs;
				t_instance_tbl<cgs_gts> handle_gts;
				t_instance_tbl<cgs_fx> handle_fx;
			};
			static_assert(offsetof(context_t, frame_counter) == 0x60);
			static_assert(offsetof(context_t, p_device_context) == 0xB0);
			static_assert(offsetof(context_t, sbgl_device) == 0xC0);
			static_assert(offsetof(context_t, sbgl_device.m_pD3DDeviceContext) == 0x150); // Redundant,but validates the assumption
			// that m_pD3DDeviceContext

// TODO: Make dynamic
#ifdef _Y6_DAY1_DLL
			static_assert(offsetof(context_t, p_ib_quad) == 0x1370);
			static_assert(offsetof(context_t, p_ib_fan) == 0x1378);
			static_assert(offsetof(context_t, stack_cb_pool) == 0x13E0);
			static_assert(offsetof(context_t, stack_up_pool) == 0x13E8);
			static_assert(offsetof(context_t, stack_shader_uniform) == 0x13F0);
			static_assert(offsetof(context_t, handle_tex) == 0x1740);
			static_assert(offsetof(context_t, handle_fx) == 0x1840);
#else
			static_assert(offsetof(context_t, p_ib_quad) == 0x1360);
			static_assert(offsetof(context_t, p_ib_fan) == 0x1368);
			static_assert(offsetof(context_t, stack_cb_pool) == 0x13D0);
			static_assert(offsetof(context_t, stack_up_pool) == 0x13D8);
			static_assert(offsetof(context_t, stack_shader_uniform) == 0x13E0);
			static_assert(offsetof(context_t, handle_tex) == 0x1730);
			static_assert(offsetof(context_t, handle_fx) == 0x1830);
#endif

			void primitive_initialize();

			extern cgs_vb* (*vb_create)(uint64_t fvf, unsigned int vertices, vb_usage_t usage, unsigned int flags, const void* p_initial_data, const char* sz_name);
			extern cgs_ib* (*ib_create)(unsigned int indices, ib_usage_t usage, unsigned int flags, const void* p_initial_data, const char* sz_name);

			extern context_t* sm_context;

		}
	}
}
