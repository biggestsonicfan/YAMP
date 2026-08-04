#include "gs.h"

#include "../../RenderWindow.h"
#include "../../DebugLog.h"

// Lock the handle-table offsets to the DLL's true layout. Verified against a live
// Lost Judgment gs context dump (LostJudgment.exe+0x3B722C0) and the DLL constructor
// (pxd::gs::context_t::context_t). See scratchpad/live-gscontext-spec.md.
static_assert(offsetof(pxd::gs::context_t, handle_mesh) == 0x107A98, "handle_mesh offset drift");
static_assert(offsetof(pxd::gs::context_t, handle_tex)  == 0x107AB8, "handle_tex offset drift");
static_assert(offsetof(pxd::gs::context_t, handle_fx)   == 0x107BB8, "handle_fx offset drift");
// The shared immediate-mode primitive buffers. Motor Raid's 2D quad pusher reads p_ib_quad
// straight off the context (FUN_18009C829: `mov rcx,[gs_context+0x7670]`), so a layout drift
// here is a null-deref inside the DLL rather than a YAMP-side error.
static_assert(offsetof(pxd::gs::context_t, p_vb_sphere) == 0x7640, "p_vb_sphere offset drift");
static_assert(offsetof(pxd::gs::context_t, p_ib_quad) == 0x7670, "p_ib_quad offset drift");
static_assert(offsetof(pxd::gs::context_t, p_ib_fan)  == 0x7678, "p_ib_fan offset drift");
static_assert(offsetof(pxd::gs::context_t, p_ib_rect) == 0x7680, "p_ib_rect offset drift");
static_assert(offsetof(pxd::gs::context_t, p_device_context) == 0x7738, "p_device_context offset drift");

// The same fields in the Like a Dragon Gaiden build (module 2023-11-27, context 0x3898C0).
// Every one of them is exactly +0xBA8 from its LJ position - that uniform shift IS the layout
// difference, and it is the reason this is one templated struct rather than two. Derived by
// aligning the two module constructors instruction-by-instruction; see gs.h.
static_assert(offsetof(pxd::gs::context_gaiden_t, handle_mesh) == 0x108640, "gaiden handle_mesh drift");
static_assert(offsetof(pxd::gs::context_gaiden_t, handle_tex)  == 0x108660, "gaiden handle_tex drift");
static_assert(offsetof(pxd::gs::context_gaiden_t, handle_fx)   == 0x108760, "gaiden handle_fx drift");
static_assert(offsetof(pxd::gs::context_gaiden_t, p_vb_sphere) == 0x81E8, "gaiden p_vb_sphere drift");
static_assert(offsetof(pxd::gs::context_gaiden_t, p_ib_quad) == 0x8218, "gaiden p_ib_quad drift");
static_assert(offsetof(pxd::gs::context_gaiden_t, p_ib_fan)  == 0x8220, "gaiden p_ib_fan drift");
static_assert(offsetof(pxd::gs::context_gaiden_t, p_ib_rect) == 0x8228, "gaiden p_ib_rect drift");
static_assert(offsetof(pxd::gs::context_gaiden_t, p_device_context) == 0x82E0, "gaiden p_device_context drift");

// Everything ABOVE gap18 must agree in both builds - that is what lets the host read the header
// fields (and frame_counter) straight off `sm_context` without knowing which build is loaded.
static_assert(offsetof(pxd::gs::context_t, frame_counter)
	== offsetof(pxd::gs::context_gaiden_t, frame_counter), "header must be build-independent");
static_assert(offsetof(pxd::gs::context_t, unknown_table_ptr3)
	== offsetof(pxd::gs::context_gaiden_t, unknown_table_ptr3), "header must be build-independent");

namespace pxd
{
		namespace gs {

			cgs_vb* (*vb_create)(uint64_t fvf, unsigned int vertices, vb_usage_t usage, unsigned int flags, const void* p_initial_data, const char* sz_name);
			cgs_ib* (*ib_create)(unsigned int indices, ib_usage_t usage, unsigned int flags, const void* p_initial_data, const char* sz_name);
			context_t* sm_context;
			// False = the LJ / Y:LAD layout (context 0x388A00), which is every host but Gaiden.
			bool sm_is_gaiden_layout = false;
			// Defaults to the LJ layout so any host that never sets it behaves exactly as before.
			DeviceContextOffsets sm_dc = DC_OFFSETS_LJ;

			// The three below-gap18 fields the host itself reads. Everything else that has to know
			// the layout lives in PatchGs, which is templated over the context type.
			cgs_device_context* device_context()
			{
				return with_tail([](auto* ctx) { return ctx->p_device_context; });
			}

			t_instance_tbl<cgs_tex>& handle_tex()
			{
				return with_tail([](auto* ctx) -> t_instance_tbl<cgs_tex>& { return ctx->handle_tex; });
			}

			sbgl::cdevice& sbgl_device()
			{
				return with_tail([](auto* ctx) -> sbgl::cdevice& { return ctx->sbgl_device; });
			}

			void inject_pointer(context_t* sm_context) {
				uintptr_t value = (uintptr_t)sm_context + 0x78;
				memcpy((uint8_t*)sm_context + 0xb0, &value, sizeof(void*));
			}

			// The primitive buffers all live BELOW gap18, so the body runs against whichever
			// layout is loaded - hence the generic lambda rather than a plain `context_t*`.
			void primitive_initialize()
			{
				with_tail([](auto* context)
				{
				for (size_t i = 0; i < 3; i++)
				{
					context->p_vb_sphere[i] = nullptr;
					context->p_vb_capsule[i] = nullptr;
				}

				{
					constexpr size_t NUM_PRIMITIVES = 192;
					uint16_t verts[NUM_PRIMITIVES][6];
					uint16_t primNum = 0;
					for (auto& vert : verts)
					{
						const uint16_t startVertNum = 4 * primNum++;
						size_t idx = 0;
						vert[idx++] = startVertNum;
						vert[idx++] = startVertNum + 1;
						vert[idx++] = startVertNum + 2;
						vert[idx++] = startVertNum;
						vert[idx++] = startVertNum + 2;
						vert[idx++] = startVertNum + 3;
					}
					context->p_ib_quad = ib_create(NUM_PRIMITIVES * 6, IB_USAGE_IMMUTABLE, 0, verts, nullptr);
				}
				{
					constexpr size_t NUM_PRIMITIVES = 256;
					uint16_t verts[NUM_PRIMITIVES][3];
					uint16_t primNum = 0;
					for (auto& vert : verts)
					{
						const uint16_t startVertexNum = primNum++;
						size_t idx = 0;
						vert[idx++] = startVertexNum + 2;
						vert[idx++] = 0;
						vert[idx++] = startVertexNum + 1;
					}
					// BUG: Original code creates a IB 2x bigger, but this seems like a bug
					context->p_ib_fan = ib_create(NUM_PRIMITIVES * 3, IB_USAGE_IMMUTABLE, 0, verts, nullptr);
				}
				{
					// The rect index buffer (+0x7680). Unlike p_ib_quad, its consumer draws with
					// TRIANGLESTRIP topology and a fixed 4 indices per call (LJ VF5FS
					// FUN_18023D740: topology arg 5 then DrawIndexed(4, 1, StartIndex=0)), so the
					// indices are simply the identity run per rect: rect i uses {4i, 4i+1, 4i+2,
					// 4i+3}, which is what a strip needs when the module supplies the four corner
					// vertices itself. Sized to match p_ib_quad's rect capacity so a nonzero
					// StartIndex would still land in range.
					//
					// Created for every LJ-era host, not just VF5FS: the m2ftg modules never read
					// the field, and A/B runs of Sonic the Fighters with this block on and off gave
					// identical frame counts (2026-07-29), so it costs them nothing.
					constexpr size_t NUM_PRIMITIVES = 192;
					uint16_t verts[NUM_PRIMITIVES][4];
					uint16_t primNum = 0;
					for (auto& vert : verts)
					{
						const uint16_t startVertNum = 4 * primNum++;
						for (size_t idx = 0; idx < 4; idx++)
						{
							vert[idx] = static_cast<uint16_t>(startVertNum + idx);
						}
					}
					context->p_ib_rect = ib_create(NUM_PRIMITIVES * 4, IB_USAGE_IMMUTABLE, 0, verts, nullptr);
				}
				DebugLogFile("[primitives] p_ib_quad=%p p_ib_fan=%p p_ib_rect=%p\n",
					static_cast<void*>(context->p_ib_quad), static_cast<void*>(context->p_ib_fan),
					static_cast<void*>(context->p_ib_rect));
				});
			}

		}

		void cgs_device_context::initialize(sbgl::ccontext* p_context)
		{
			// Native DX12 has no D3D11 immediate context, so p_context is null here. The old
			// `if (p_context != nullptr)` guard therefore skipped the ENTIRE body under DX12,
			// leaving the pools (mp_cb_pool/up_pool/shader_uniform) and device state
			// uninitialized (mp_sbgl_context/+0x28 read as 0 in a live dump). Run it regardless;
			// reset_state_all is safe with an unbound command context because its +0xC8 block is
			// gated on gs_context+0x7690, which YAMP leaves 0.
			mp_sbgl_context = p_context;

			// The three pools sit below gap18, so they move with the build's layout.
			gs::with_tail([this](auto* ctx)
			{
				mp_cb_pool = ctx->stack_cb_pool.pop();
				mp_up_pool = ctx->stack_up_pool.pop();
				mp_shader_uniform = ctx->stack_shader_uniform.pop();
			});
			reset_state_all();

			// TODO: Yakuza 6 returns an error code here??
		}

		namespace sbgl {

			// The pxd generation this file models is DX12-native: RenderWindow always creates a
			// D3D12 device (it drives the swapchain and hands the modules an 11-on-12 D3D11 device
			// purely for YAMP's own composite/overlay), so there is no DX11 branch to take here.
			// The engine reads m_pD3DDeviceContext and m_DXGIAdapterDesc out of this object, so both
			// are still filled — the context deliberately null, since DX12 has no immediate context.
			void cdevice::initialize(const RenderWindow& renderWindow)
			{
				m_pD3DDeviceContext = nullptr;

				m_DXGIAdapterDesc = {};
				wil::com_ptr<IDXGIFactory6> factory;
				if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
				{
					wil::com_ptr<IDXGIAdapter1> adapter;
					if (SUCCEEDED(factory->EnumAdapters1(0, adapter.addressof())))
						adapter->GetDesc1(&m_DXGIAdapterDesc);
				}

				m_swap_chain.initialize(renderWindow);
			}

			// DX12-native generation (see cdevice::initialize): the modules render through their own
			// D3D12 device and never read this object's DX11 RTV/DSV fields, so the only thing the
			// engine needs from here is the swapchain pointer — the present path reaches it as
			// sm_context->sbgl_device.m_swap_chain.m_pDXGISwapChain. RenderWindow owns the swapchain
			// for the process lifetime, so this is a borrowed raw pointer.
			//
			// This used to build an RTV heap + per-backbuffer RTVs and a DSV heap + a full-window
			// committed depth buffer here. Every one of those went into a local wil::com_ptr and was
			// released again on return (the original note admitted as much: "we don't store RTV/DSV
			// heaps or resources"), so it allocated and freed a depth target every boot to no effect.
			HRESULT cswap_chain_common::initialize(const RenderWindow& window)
			{
				m_pDXGISwapChain = window.GetSwapChain();
				return m_pDXGISwapChain != nullptr ? S_OK : E_POINTER;
			}

			void ccontext_native::desc_st::reset(ID3D11RenderTargetView* pDepthStencilView, float depth_clear_value, unsigned int width, unsigned int height)
			{
				for (size_t i = 0; i < 8; i++)
				{
					m_ppRenderTargetView[i] = nullptr;
					//m_p_fast_clear_color[i] = {}; // YLAD only
				}
				m_num_slots = 0;
				m_width = width;
				m_height = height;
				m_depth_clear_value = depth_clear_value;
			}

		}

		void cgs_up_pool::initialize(unsigned int vb_size, unsigned int ib_size, bool is_push_support)
		{
			m_vb_size = (vb_size + 63) & ~63;
			m_ib_size = (ib_size + 63) & ~63;

			if (m_vb_size != 0)
			{
				// TODO: Fix this filthy, disgusting, dirty hack
				gs::inject_pointer(gs::sm_context);
				mp_vb = gs::vb_create(0xE00003u, m_vb_size / 16, gs::VB_USAGE_DYNAMIC, 0xF0000000, nullptr, nullptr);
			}
			if (m_ib_size != 0)
			{
				mp_ib = gs::ib_create(m_ib_size / 2, gs::IB_USAGE_DYNAMIC, 0xF0000000, nullptr, nullptr);
			}
			// A null mp_vb here is fatal later and far from obvious: the engine binds it as a vertex
			// stream and dereferences it (a packed pointer) without a null check.
			DebugLogFile("[up-pool] initialize vb_size=%u ib_size=%u push=%d -> mp_vb=%p mp_ib=%p\n",
				m_vb_size, m_ib_size, is_push_support ? 1 : 0,
				static_cast<void*>(mp_vb), static_cast<void*>(mp_ib));
			if (is_push_support)
			{
				char* buf = static_cast<char*>(::operator new(2 * m_vb_size, std::align_val_t(16)));
				mp_push_polygon = buf;
				mp_push_line = buf + m_vb_size;
			}
		}

		void cgs_shader_uniform::initialize()
		{
			// (was m_clip_far twice — m_clip_near was left holding whatever the uninitialized
			// `new cgs_shader_uniform` allocation contained, 0xCD fill under the debug CRT.)
			m_clip_near = 0.0f;
			m_clip_far = 1.0f;
			m_data.m_data_material_modify.saturation = 1.0f;
			m_data.m_data_material_modify.palette0 = -1;
			m_data.m_data_material_modify.palette1 = -1;
			m_data.m_data_material_modify._reserve0 = 1.0f;
		}
	}

