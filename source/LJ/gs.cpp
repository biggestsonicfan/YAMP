#include "gs.h"

#include "../RenderWindow.h"

// Lock the handle-table offsets to the DLL's true layout. Verified against a live
// Lost Judgment gs context dump (LostJudgment.exe+0x3B722C0) and the DLL constructor
// (pxd::gs::context_t::context_t). YAMP loads the 0x388A00-size DLL variant, so these
// are the authoritative offsets. See scratchpad/live-gscontext-spec.md.
static_assert(offsetof(LJ::StF::gs::context_t, handle_mesh) == 0x107A98, "handle_mesh offset drift");
static_assert(offsetof(LJ::StF::gs::context_t, handle_tex)  == 0x107AB8, "handle_tex offset drift");
static_assert(offsetof(LJ::StF::gs::context_t, handle_fx)   == 0x107BB8, "handle_fx offset drift");

namespace LJ
{
	namespace StF
	{
		namespace gs {

			cgs_vb* (*vb_create)(uint64_t fvf, unsigned int vertices, vb_usage_t usage, unsigned int flags, const void* p_initial_data, const char* sz_name);
			cgs_ib* (*ib_create)(unsigned int indices, ib_usage_t usage, unsigned int flags, const void* p_initial_data, const char* sz_name);
			context_t* sm_context;

			void inject_pointer(context_t* sm_context) {
				uintptr_t value = (uintptr_t)sm_context + 0x78;
				memcpy((uint8_t*)sm_context + 0xb0, &value, sizeof(void*));
			}

			void primitive_initialize()
			{
				context_t* context = sm_context;

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

			mp_cb_pool = gs::sm_context->stack_cb_pool.pop();
			mp_up_pool = gs::sm_context->stack_up_pool.pop();
			mp_shader_uniform = gs::sm_context->stack_shader_uniform.pop();
			reset_state_all();

			// TODO: Yakuza 6 returns an error code here??
		}

		namespace sbgl {

			void cdevice::initialize(const RenderWindow& renderWindow)
			{
				// --- DirectX 12 conditional path (native DX12)
				if (auto* d3d12 = renderWindow.GetD3D12Device())
				{
					OutputDebugStringA("[sbgl::cdevice] Initializing using native DirectX 12 device.\n");

					// Create a dummy DX11 context replacement if necessary
					m_pD3DDeviceContext = nullptr; // DX12 has no immediate context

					// Local factory; no RenderWindow dependency
					m_DXGIAdapterDesc = {};
					wil::com_ptr<IDXGIFactory6> factory;
					if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
					{
						wil::com_ptr<IDXGIAdapter1> adapter;
						if (SUCCEEDED(factory->EnumAdapters1(0, adapter.addressof())))
							adapter->GetDesc1(&m_DXGIAdapterDesc);
					}


					// Initialize swap chain via DX12 path
					m_swap_chain.initialize(renderWindow);

					// Initialize DX12-specific pipeline state or queues if you need them
					// (store queue for later submissions, etc.)
					// Example:
					// ID3D12CommandQueue* queue = renderWindow.GetD3D12CommandQueue();

					// Since no D3D11 device/context exist here, skip private data setup
					return; // skip the DX11 initialize logic entirely
				}
				else {
					m_swap_chain.initialize(renderWindow);
					m_pD3DDeviceContext = renderWindow.GetD3D11DeviceContext();

					std::fill(std::begin(m_p_border_color), std::end(m_p_border_color), _mm_set1_ps(std::numeric_limits<float>::quiet_NaN()));
					m_p_border_color[0] = _mm_setzero_ps();
					m_p_border_color[1] = _mm_set_ps(0.0f, 0.0f, 0.0f, 1.0f);
					m_p_border_color[2] = _mm_set1_ps(1.0f);

					// TODO: YLAD presents an empty frame here... Yakuza 6 doesn't seem to
					// The game sets private data, then renders a frame, then resets private data
					// For now, just do it directly
					m_context_desc.reset(nullptr, 0.0f, 0, 0);
					void* dataPtr = &m_context_desc;
					m_pD3DDeviceContext->SetPrivateData(ccontext_native::GUID_ContextPrivateData, sizeof(dataPtr), &dataPtr);
				}
			}

			HRESULT cswap_chain_common::initialize(const RenderWindow& window)
			{
				// --- DirectX 12 conditional path (native DX12)
			// --- DirectX 12 conditional path (native DX12; skip DX11 below if this runs)
				if (ID3D12Device* d3d12 = window.GetD3D12Device())
				{
					OutputDebugStringA("[sbgl::cswap_chain_common] DX12 path: building RTV/DSV.\n");

					HRESULT hr = S_OK;

					// Validate swapchain and upcast to IDXGISwapChain3
					wil::com_ptr<IDXGISwapChain> scBase(window.GetSwapChain());
					if (!scBase)
						return E_POINTER;

					// Store the swapchain so the present path can reach it via
					// sm_context->sbgl_device.m_swap_chain.m_pDXGISwapChain->Present(1,0). The DX11
					// branch below sets this; the DX12 branch only used the swapchain locally for
					// RTV/DSV setup, leaving m_pDXGISwapChain null -> Present on a null ptr AVs.
					// RenderWindow owns the swapchain for the process lifetime (borrowed raw ptr).
					m_pDXGISwapChain = window.GetSwapChain();

					wil::com_ptr<IDXGISwapChain3> sc3 = scBase.try_query<IDXGISwapChain3>();
					if (!sc3)
						return E_NOINTERFACE;

					// Buffer count (RenderWindow uses 2)
					const UINT kBufferCount = 2;

					// ------------------------------
					// RTV HEAP (DX12 requires a heap)
					// ------------------------------
					D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
					rtvHeapDesc.NumDescriptors = kBufferCount;
					rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
					rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

					wil::com_ptr<ID3D12DescriptorHeap> rtvHeap;
					hr = d3d12->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
					if (FAILED(hr)) return hr;

					const UINT rtvInc = d3d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
					D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();

					// Create RTVs for all backbuffers
					for (UINT i = 0; i < kBufferCount; ++i)
					{
						wil::com_ptr<ID3D12Resource> backBuffer;
						hr = sc3->GetBuffer(i, IID_PPV_ARGS(&backBuffer));
						if (FAILED(hr)) return hr;

						D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
						rtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // matches RenderWindow output format
						rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

						d3d12->CreateRenderTargetView(backBuffer.get(), &rtvDesc, rtvHandle);
						rtvHandle.ptr += rtvInc; // advance handle
					}

					// --------------------------------
					// DSV (depth buffer + DSV heap)
					// --------------------------------
					D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
					dsvHeapDesc.NumDescriptors = 1;
					dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
					dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

					wil::com_ptr<ID3D12DescriptorHeap> dsvHeap;
					hr = d3d12->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap));
					if (FAILED(hr)) return hr;

					D3D12_RESOURCE_DESC depthDesc{};
					depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
					depthDesc.Alignment = 0;
					depthDesc.Width = window.GetWidth();
					depthDesc.Height = window.GetHeight();
					depthDesc.DepthOrArraySize = 1;
					depthDesc.MipLevels = 1;
					depthDesc.Format = DXGI_FORMAT_D32_FLOAT; // depth only
					depthDesc.SampleDesc.Count = 1;
					depthDesc.SampleDesc.Quality = 0;
					depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
					depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

					D3D12_CLEAR_VALUE clearValue{};
					clearValue.Format = DXGI_FORMAT_D32_FLOAT;
					clearValue.DepthStencil.Depth = 1.0f;
					clearValue.DepthStencil.Stencil = 0;

					D3D12_HEAP_PROPERTIES heapProps{};
					heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
					heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
					heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
					heapProps.CreationNodeMask = 1;
					heapProps.VisibleNodeMask = 1;

					wil::com_ptr<ID3D12Resource> depthBuffer;
					hr = d3d12->CreateCommittedResource(
						&heapProps,
						D3D12_HEAP_FLAG_NONE,
						&depthDesc,
						D3D12_RESOURCE_STATE_DEPTH_WRITE,
						&clearValue,
						IID_PPV_ARGS(&depthBuffer)
					);
					if (FAILED(hr)) return hr;

					D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
					dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
					dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
					dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

					d3d12->CreateDepthStencilView(depthBuffer.get(), &dsvDesc, dsvHeap->GetCPUDescriptorHandleForHeapStart());

					// NOTE: We don't store RTV/DSV heaps or resources in cswap_chain_common because
					// you asked for minimal edits. If you need them later, add fields to the class.
					return S_OK; // all good; skip DX11 path
				}
				else {
					// TODO: Validate
					constexpr sbgl_format_t depth_format = SBGL_FORMAT_D32_F_S8X24_U;

					ID3D11Device* device = window.GetD3D11Device();

					m_pDXGISwapChain = window.GetSwapChain();

					wil::com_ptr<ID3D11Texture2D> swapChainBuffer;
					HRESULT hr = m_pDXGISwapChain->GetBuffer(0, IID_PPV_ARGS(swapChainBuffer.addressof()));
					if (SUCCEEDED(hr))
					{
						hr = device->CreateRenderTargetView(swapChainBuffer.get(), nullptr, &m_pD3DRenderTargetView);
						if (SUCCEEDED(hr))
						{
							D3D11_TEXTURE2D_DESC dsDesc;
							dsDesc.Width = window.GetWidth();
							dsDesc.Height = window.GetHeight();
							dsDesc.MipLevels = 1;
							dsDesc.ArraySize = 1;
							dsDesc.Format = static_cast<DXGI_FORMAT>((depth_format >> 7) & 0x7F);
							dsDesc.SampleDesc.Count = 1;
							dsDesc.SampleDesc.Quality = 0;
							dsDesc.Usage = D3D11_USAGE_DEFAULT;
							dsDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
							dsDesc.CPUAccessFlags = 0;
							dsDesc.MiscFlags = 0;
							hr = device->CreateTexture2D(&dsDesc, nullptr, &m_pD3DDepthStencilBuffer);
							if (SUCCEEDED(hr))
							{
								D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
								dsvDesc.Format = static_cast<DXGI_FORMAT>(depth_format & 0x7F);
								dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
								dsvDesc.Flags = 0;
								dsvDesc.Texture2D.MipSlice = 0;
								hr = device->CreateDepthStencilView(m_pD3DDepthStencilBuffer, &dsvDesc, &m_pD3DDepthStencilView);
							}
						}
					}
					return hr;
				}
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
				m_height = 0;
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
			if (is_push_support)
			{
				char* buf = static_cast<char*>(::operator new(2 * m_vb_size, std::align_val_t(16)));
				mp_push_polygon = buf;
				mp_push_line = buf + m_vb_size;
			}
		}

		void cgs_shader_uniform::initialize()
		{
			m_clip_far = 0.0f;
			m_clip_far = 1.0f;
			m_data.m_data_material_modify.saturation = 1.0f;
			m_data.m_data_material_modify.palette0 = -1;
			m_data.m_data_material_modify.palette1 = -1;
			m_data.m_data_material_modify._reserve0 = 1.0f;
		}
	}
}