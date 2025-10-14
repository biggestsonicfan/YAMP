#pragma once

#include "YAMPUserInterface.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// A simple manager for the window handle and the D3D11 device
class RenderWindow
{
public:
	RenderWindow(HINSTANCE instance, HINSTANCE dllInstance, int cmdShow);
	~RenderWindow();

	ID3D11Device* GetD3D11Device() const { return m_device.Get(); }
	ID3D11DeviceContext* GetD3D11DeviceContext() const { return m_deviceContext.Get(); }
	IDXGISwapChain* GetSwapChain() const { return m_swapChain.Get(); }

	void BlitGameFrame(ID3D11ShaderResourceView* src);

	void NewImGuiFrame();
	void RenderImGui();

	uint32_t GetWidth() const { return m_width; }
	uint32_t GetHeight() const { return m_height; }

	bool IsShuttingDown() const { return m_shuttingDownWindow.load(std::memory_order_relaxed); }

	// No-ops in DX11; active in DX11ON12
	void BeginFrame();
	void EndFrame();

private:
	struct hwnd_deleter {
		void operator()(HWND hwnd) const noexcept { if (hwnd) DestroyWindow(hwnd); }
	};

	ComPtr<IDXGISwapChain> CreateSwapChainForWindow(ID3D11Device* device, HWND window);
	void CreateRenderResources();
	void EnumerateDisplayModes();
	void CalculateViewport();

	std::atomic_bool m_shuttingDownWindow { false };
	std::thread m_windowThread;
	std::unique_ptr<std::remove_pointer_t<HWND>, hwnd_deleter> m_window;

	ComPtr<ID3D11Device> m_device;
	ComPtr<ID3D11DeviceContext> m_deviceContext;
	ComPtr<IDXGISwapChain> m_swapChain;

	// Render resources required to render to backbuffer
	ComPtr<ID3D11RenderTargetView> m_backBufferRTV;
	ComPtr<ID3D11VertexShader> m_vs;
	ComPtr<ID3D11PixelShader> m_ps;
	ComPtr<ID3D11InputLayout> m_inputLayout;
	ComPtr<ID3D11Buffer> m_vb;
	UINT m_vbStride;
	D3D11_VIEWPORT m_viewport;
	bool m_requiresClear = false;

	uint32_t m_width, m_height;

	YAMPUserInterface m_ui;

	// 12-level device & queue + 11on12 bridge
	Microsoft::WRL::ComPtr<ID3D12Device>        m_d3d12Device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue>  m_cmdQueue;
	Microsoft::WRL::ComPtr<ID3D11On12Device>    m_d3d11on12;

	static constexpr UINT                        kBufferCount = 2;
	Microsoft::WRL::ComPtr<ID3D12Resource>       m_backbuffers[kBufferCount];
	Microsoft::WRL::ComPtr<ID3D11Resource>       m_wrappedBackbuffers[kBufferCount];

	void CreateWrappedBackbuffers();
	void ResizeOn12(UINT w, UINT h);
};