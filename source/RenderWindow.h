#pragma once

#include "YAMPUserInterface.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3d12.h>
#include <d3d11on12.h>

#include "wil/com.h"
#include "wil/resource.h"

#include <atomic>
#include <thread>
#include <vector>
#include <dxgi1_6.h>

// The StF DLL creates its own large shader-visible descriptor heaps (CBV/SRV/UAV ~1M + SAMPLER 2048)
// and binds them; PatchGs wires the gs+0x7550 descriptor rings to reference THESE (captured by the
// device CreateDescriptorHeap hook) so the DLL's root-table GPU handles resolve into the bound heap.
ID3D12DescriptorHeap* GetDllRingCbvSrvHeap();
ID3D12DescriptorHeap* GetDllRingSamplerHeap();
// The DLL never calls SetGraphicsRootSignature; the host must. This is the root sig the PSO was
// built with, re-bound after each SetPipelineState in the command-list hook.
ID3D12RootSignature* GetCapturedRootSignature();

// A simple manager for the window handle and the D3D11 device
class RenderWindow
{
public:
	RenderWindow(HINSTANCE instance, HINSTANCE dllInstance, int cmdShow);
	~RenderWindow();
	
	ID3D12Device* GetD3D12Device() const { return m_d3d12Device.get(); }
	ID3D12CommandQueue* GetD3D12Queue()  const { return m_cmdQueue.get(); }
	HWND                 GetHWND()        const { return m_window.get(); }

	IDXGISwapChain3* GetSwapChain3() const {
		IDXGISwapChain3* sc3 = nullptr;
		if (m_swapChain) {
			(void)m_swapChain->QueryInterface(IID_PPV_ARGS(&sc3));
		}
		return sc3;
	}

	ID3D11Device* GetD3D11Device() const { return m_device.get(); }
	ID3D11DeviceContext* GetD3D11DeviceContext() const { return m_deviceContext.get(); }
	IDXGISwapChain* GetSwapChain() const { return m_swapChain.get(); }

	void BlitGameFrame(ID3D11ShaderResourceView* src, bool alphaBlend = false);
	// Clear + bind the backbuffer without blitting a game frame — for frames that only draw
	// ImGui (the game launcher). Call between BeginFrame and RenderImGui.
	void ClearBackbuffer();
	// Composite a DX12-native texture (StF's output RT) via 11on12 wrap -> D3D11 SRV -> BlitGameFrame.
	void BlitDX12Texture(ID3D12Resource* tex);

	// The GAME's native aspect ratio (default 16:9 for VF5FS). The Model 2 arcade titles
	// (StF/FV/VF2: 496x384 internal upscaled to a 1024x768 display texture = 4:3) must be
	// pillarboxed in a widescreen window instead of stretched — their hosts apply the shared
	// aspect setting via m2ftg::ApplyAspectSetting. Recomputes the blit viewport immediately.
	void SetGameAspectRatio(float ar) { m_gameAspect = ar; CalculateViewport(); }
	// When set, the game blit covers the whole window regardless of aspect ratio ("Fill Window").
	void SetGameStretchToFill(bool fill) { m_fillViewport = fill; CalculateViewport(); }

	void NewImGuiFrame();
	void RenderImGui();

	YAMPUserInterface& GetUI() { return m_ui; }

	uint32_t GetWidth() const { return m_width; }
	uint32_t GetHeight() const { return m_height; }

	bool IsShuttingDown() const { return m_shuttingDownWindow.load(std::memory_order_relaxed); }

	// No-ops in DX11; active in DX11ON12
	void BeginFrame();
	void EndFrame();

	// Called from WndProc (window thread) on WM_SIZE; the render thread applies it at the top of
	// BeginFrame. Without this the swapchain stayed at its creation size forever: DXGI stretched
	// the stale backbuffer to the window (breaking the game's aspect ratio) and ImGui rendered at
	// backbuffer scale while the mouse reported window coordinates (hit-test misalignment).
	void RequestResize(uint32_t w, uint32_t h)
	{
		if (w != 0 && h != 0)
		{
			m_pendingResize.store((static_cast<uint64_t>(w) << 32) | h, std::memory_order_release);
		}
	}

private:
	struct hwnd_deleter {
		void operator()(HWND hwnd) const noexcept { if (hwnd) DestroyWindow(hwnd); }
	};

	wil::com_ptr<IDXGISwapChain> CreateSwapChainForWindow(ID3D11Device* device, HWND window);
	void CreateRenderResources();
	void EnumerateDisplayModes();
	void CalculateViewport();
	void ApplyPendingResize();

	std::atomic_bool m_shuttingDownWindow { false };
	std::thread m_windowThread;
	std::unique_ptr<std::remove_pointer_t<HWND>, hwnd_deleter> m_window;

	wil::com_ptr<ID3D11Device> m_device;
	wil::com_ptr<ID3D11DeviceContext> m_deviceContext;
	wil::com_ptr<IDXGISwapChain> m_swapChain;

	// Render resources required to render to backbuffer
	wil::com_ptr<ID3D11RenderTargetView> m_backBufferRTV;
	wil::com_ptr<ID3D11VertexShader> m_vs;
	wil::com_ptr<ID3D11PixelShader> m_ps;
	wil::com_ptr<ID3D11InputLayout> m_inputLayout;
	wil::com_ptr<ID3D11Buffer> m_vb;
	UINT m_vbStride;
	D3D11_VIEWPORT m_viewport;
	bool m_requiresClear = false;
	float m_gameAspect = 1280.0f / 720.0f; // see SetGameAspectRatio
	bool m_fillViewport = false;           // see SetGameStretchToFill
	wil::com_ptr<ID3D11SamplerState> m_blitSampler;

	// Lost Judgment's CRT filter (exact port of its PSO 9775 pixel shader — see LJ_CRT_PS_HLSL),
	// compiled at runtime on first use; latches off for the session if compilation ever fails.
	// Like LJ, the shader is evaluated into a window-sized intermediate target (the pause-menu
	// captures prove LJ's target follows the window), clamped up to >= 1080p tall so small
	// windows don't alias the 384 scanlines into broken segments.
	bool EnsureCrtResources();
	wil::com_ptr<ID3D11PixelShader> m_psCrt;
	wil::com_ptr<ID3D11Texture2D> m_crtTex;
	wil::com_ptr<ID3D11RenderTargetView> m_crtRTV;
	wil::com_ptr<ID3D11ShaderResourceView> m_crtSRV;
	UINT m_crtTexW = 0, m_crtTexH = 0;
	bool m_crtCompileFailed = false;

	uint32_t m_width, m_height;

	YAMPUserInterface m_ui;

	// 12-level device & queue + 11on12 bridge
	wil::com_ptr<ID3D12Device>        m_d3d12Device;
	wil::com_ptr<ID3D12CommandQueue>  m_cmdQueue;
	wil::com_ptr<ID3D11On12Device>    m_d3d11on12;

	static constexpr UINT kBufferCount = 2;
	wil::com_ptr<ID3D12Resource> m_backbuffers[kBufferCount];
	wil::com_ptr<ID3D11Resource> m_wrappedBackbuffers[kBufferCount];

	std::atomic<uint64_t> m_pendingResize { 0 }; // packed (w << 32) | h, see RequestResize
	wil::com_ptr<ID3D12Fence> m_resizeFence;
	uint64_t m_resizeFenceValue = 0;

	// d3d11on12 only emits the Release-side barrier (RENDER_TARGET->PRESENT); it never transitions the
	// backbuffer INTO RENDER_TARGET on Acquire (confirmed via the ResourceBarrier trace). So the backbuffer
	// stays COMMON, the composite/ImGui draws to it are dropped, and a black frame is presented. This small
	// dedicated D3D12 list issues the missing COMMON->RENDER_TARGET barrier each frame before the blit.
	wil::com_ptr<ID3D12CommandAllocator>    m_bbBarrierAlloc;
	wil::com_ptr<ID3D12GraphicsCommandList> m_bbBarrierList;
	void TransitionBackbufferToRenderTarget(UINT idx);

	void CreateWrappedBackbuffers();
	void ResizeOn12(UINT w, UINT h);
};