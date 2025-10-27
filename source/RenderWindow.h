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

// A simple manager for the window handle and the D3D11 device
class RenderWindow
{
public:
    RenderWindow(HINSTANCE instance, HINSTANCE dllInstance, int cmdShow);
    ~RenderWindow();

    ID3D11Device* GetD3D11Device() const { return m_device.get(); }
    ID3D11DeviceContext* GetD3D11DeviceContext() const { return m_deviceContext.get(); }
    IDXGISwapChain* GetSwapChain() const { return m_swapChain.get(); }

    ID3D12Device* GetD3D12Device() const { return m_d3d12Device.get(); }
    ID3D12CommandQueue* GetD3D12CommandQueue() const { return m_cmdQueue.get(); }
    ID3D11On12Device* GetD3D11On12Device() const { return m_d3d11on12.get(); }

    HWND GetHWND() const { return m_window.get(); }
    IDXGIFactory4* GetDXGIFactory() const { return m_dxgiFactory.get(); }

    void BlitGameFrame(ID3D11ShaderResourceView* src);

    void NewImGuiFrame();
    void RenderImGui();

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

    bool IsShuttingDown() const { return m_shuttingDownWindow.load(std::memory_order_relaxed); }

    // No-ops in pure DX11; active in DX11ON12
    void BeginFrame();
    void EndFrame();

private:
    struct hwnd_deleter {
        void operator()(HWND hwnd) const noexcept { if (hwnd) DestroyWindow(hwnd); }
    };

    wil::com_ptr<IDXGISwapChain> CreateSwapChainForWindow(ID3D11Device* device, HWND window);
    void CreateRenderResources();
    void EnumerateDisplayModes();
    void CalculateViewport();

    void CreateWrappedBackbuffers(); // DX11-on-12: wrap DX12 backbuffers for DX11
    void ResizeOn12(UINT w, UINT h);

    std::atomic_bool m_shuttingDownWindow { false };
    std::thread m_windowThread;
    wil::unique_hwnd m_window;

    wil::com_ptr<ID3D11Device> m_device;
    wil::com_ptr<ID3D11DeviceContext> m_deviceContext;
    wil::com_ptr<IDXGISwapChain> m_swapChain;

    // Render resources required to render to backbuffer (DX11)
    wil::com_ptr<ID3D11RenderTargetView> m_backBufferRTV; // single RTV recreated per-frame from wrapped resource
    wil::com_ptr<ID3D11VertexShader> m_vs;
    wil::com_ptr<ID3D11PixelShader> m_ps;
    wil::com_ptr<ID3D11InputLayout> m_inputLayout;
    wil::com_ptr<ID3D11Buffer> m_vb;
    UINT m_vbStride = 0;
    D3D11_VIEWPORT m_viewport{};
    bool m_requiresClear = false;

    uint32_t m_width = 0, m_height = 0;

    YAMPUserInterface m_ui;

    // --- D3D12 + 11on12 bridge
    wil::com_ptr<ID3D12Device> m_d3d12Device;
    wil::com_ptr<ID3D12CommandQueue> m_cmdQueue;
    wil::com_ptr<ID3D11On12Device> m_d3d11on12;
    wil::com_ptr<IDXGIFactory4> m_dxgiFactory;

    static constexpr UINT kBufferCount = 2;
    wil::com_ptr<ID3D12Resource> m_backbuffers[kBufferCount];       // native 12 backbuffers
    wil::com_ptr<ID3D11Resource> m_wrappedBackbuffers[kBufferCount]; // DX11-wrapped views of above

    // Acquire/Release bookkeeping
    UINT m_curBackbufferIndex = 0;
    bool m_acquiredThisFrame = false;
};