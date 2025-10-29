// DX12Example.cpp
// Pure-DX12 spinning cube + ImGui overlay via D3D11on12,
// wrapped in namespace DX12::Example and exposed as Run(RenderWindow&).

#include <windows.h>
#include <cstdint>
#include <vector>
#include <array>
#include <chrono>
#include <cmath>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include "../wil/com.h"
#include "../RenderWindow.h"

// ImGui (expect context already created in Main.cpp)
#include "../imgui/imgui.h"
#include "../imgui/imgui_impl_win32.h"
#include "../imgui/imgui_impl_dx11.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

namespace DX12 {
    namespace Example {

        namespace { // ————— internal helpers/state (not exported) —————

            inline D3D12_RESOURCE_BARRIER MakeTransition(
                ID3D12Resource* res,
                D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
            {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                b.Transition.pResource = res;
                b.Transition.StateBefore = before;
                b.Transition.StateAfter = after;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                return b;
            }

            void WaitForGPU(
                ID3D12CommandQueue* queue,
                ID3D12Fence* fence,
                UINT64& fenceValue,
                HANDLE fenceEvent)
            {
                const UINT64 v = ++fenceValue;
                queue->Signal(fence, v);
                if (fence->GetCompletedValue() < v) {
                    fence->SetEventOnCompletion(v, fenceEvent);
                    WaitForSingleObject(fenceEvent, INFINITE);
                }
            }

            // in-memory shaders
            wil::com_ptr<ID3DBlob> gVS, gPS;

            bool CompileShaders()
            {
                static const char* kVS = R"(
        cbuffer CB : register(b0) { float4x4 uMVP; };
        struct VSIn  { float3 pos : POSITION; float4 col : COLOR; };
        struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };
        VSOut VSMain(VSIn i) {
            VSOut o; o.pos = mul(float4(i.pos,1.0), uMVP); o.col = i.col; return o;
        }
    )";

                static const char* kPS = R"(
        struct PSIn { float4 pos : SV_Position; float4 col : COLOR; };
        float4 PSMain(PSIn i) : SV_Target { return i.col; }
    )";

                UINT flags = 0;
#if defined(_DEBUG)
                flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

                wil::com_ptr<ID3DBlob> err;
                if (FAILED(D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr,
                    "VSMain", "vs_5_0", flags, 0, gVS.put(), err.put()))) {
                    if (err) OutputDebugStringA((char*)err->GetBufferPointer());
                    return false;
                }
                err.reset();
                if (FAILED(D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr,
                    "PSMain", "ps_5_0", flags, 0, gPS.put(), err.put()))) {
                    if (err) OutputDebugStringA((char*)err->GetBufferPointer());
                    return false;
                }
                return true;
            }

        } // anonymous namespace

        // ————— public entry —————

        void Run(RenderWindow& window)
        {
            // Pull DX12 objects and window
            wil::com_ptr<ID3D12Device>       device = window.GetD3D12Device();
            wil::com_ptr<ID3D12CommandQueue> queue = window.GetD3D12Queue();
            wil::com_ptr<IDXGISwapChain3>    swap = window.GetSwapChain3();
            HWND hwnd = window.GetHWND();

            if (!device || !queue || !swap || !hwnd) return;

            DXGI_SWAP_CHAIN_DESC1 scd{};
            swap->GetDesc1(&scd);
            const UINT frameCount = scd.BufferCount ? scd.BufferCount : 2;
            const DXGI_FORMAT backbufferFormat = scd.Format ? scd.Format : DXGI_FORMAT_B8G8R8A8_UNORM;

            // RTV heap + backbuffers (DX12)
            D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
            rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvDesc.NumDescriptors = frameCount;
            wil::com_ptr<ID3D12DescriptorHeap> rtvHeap;
            device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
            const UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

            std::vector<wil::com_ptr<ID3D12Resource>> backbuffers(frameCount);
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
                for (UINT i = 0; i < frameCount; ++i) {
                    swap->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i]));
                    device->CreateRenderTargetView(backbuffers[i].get(), nullptr, h);
                    h.ptr += rtvStride;
                }
            }

            // Command allocators + list (DX12)
            std::vector<wil::com_ptr<ID3D12CommandAllocator>> allocs(frameCount);
            for (auto& a : allocs)
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
            wil::com_ptr<ID3D12GraphicsCommandList> cmd;
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0].get(), nullptr, IID_PPV_ARGS(&cmd));
            cmd->Close();

            // Fence
            wil::com_ptr<ID3D12Fence> fence;
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            UINT64 fenceValue = 0;
            HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

            // Root signature (CBV @ b0)
            D3D12_ROOT_PARAMETER rootParams[1] = {};
            rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootParams[0].Descriptor.ShaderRegister = 0;
            rootParams[0].Descriptor.RegisterSpace = 0;
            rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

            D3D12_ROOT_SIGNATURE_DESC rsd = {};
            rsd.NumParameters = 1;
            rsd.pParameters = rootParams;
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

            wil::com_ptr<ID3DBlob> rsBlob, rsErr;
            if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) {
                if (rsErr) OutputDebugStringA((char*)rsErr->GetBufferPointer());
                return;
            }
            wil::com_ptr<ID3D12RootSignature> rootSig;
            device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));

            // Shaders
            if (!CompileShaders()) return;

            // Input layout + PSO (no d3dx12 helpers)
            D3D12_INPUT_ELEMENT_DESC layout[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,     0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            };

            D3D12_BLEND_DESC blend = {};
            blend.AlphaToCoverageEnable = FALSE;
            blend.IndependentBlendEnable = FALSE;
            for (int i = 0; i < 8; ++i) {
                auto& rt = blend.RenderTarget[i];
                rt.BlendEnable = FALSE;
                rt.LogicOpEnable = FALSE;
                rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }

            D3D12_RASTERIZER_DESC rast = {};
            rast.FillMode = D3D12_FILL_MODE_SOLID;
            rast.CullMode = D3D12_CULL_MODE_BACK;
            rast.FrontCounterClockwise = FALSE;
            rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
            rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
            rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
            rast.DepthClipEnable = TRUE;
            rast.MultisampleEnable = FALSE;
            rast.AntialiasedLineEnable = FALSE;
            rast.ForcedSampleCount = 0;
            rast.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

            D3D12_DEPTH_STENCIL_DESC dss = {};
            dss.DepthEnable = FALSE;
            dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            dss.StencilEnable = FALSE;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
            psoDesc.pRootSignature = rootSig.get();
            psoDesc.VS = { gVS->GetBufferPointer(), gVS->GetBufferSize() };
            psoDesc.PS = { gPS->GetBufferPointer(), gPS->GetBufferSize() };
            psoDesc.BlendState = blend;
            psoDesc.RasterizerState = rast;
            psoDesc.DepthStencilState = dss;
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.InputLayout = { layout, _countof(layout) };
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = backbufferFormat;
            psoDesc.SampleDesc.Count = 1;

            wil::com_ptr<ID3D12PipelineState> pso;
            device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));

            // Geometry: colored cube
            struct V { XMFLOAT3 p; XMFLOAT4 c; };
            const V vertices[] = {
                {{-1,-1, 1},{1,0,0,1}}, {{ 1,-1, 1},{1,0,0,1}}, {{ 1, 1, 1},{1,0,0,1}}, {{-1, 1, 1},{1,0,0,1}},
                {{-1,-1,-1},{0,1,0,1}}, {{ 1,-1,-1},{0,1,0,1}}, {{ 1, 1,-1},{0,1,0,1}}, {{-1, 1,-1},{0,1,0,1}},
            };
            const uint16_t indices[] = {
                0,1,2, 0,2,3,  1,5,6, 1,6,2,  5,4,7, 5,7,6,
                4,0,3, 4,3,7,  3,2,6, 3,6,7,  4,5,1, 4,1,0
            };

            auto CreateUpload = [&](SIZE_T bytes, ID3D12Resource** out) -> HRESULT {
                D3D12_HEAP_PROPERTIES hp = {};
                hp.Type = D3D12_HEAP_TYPE_UPLOAD;
                hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                hp.CreationNodeMask = 1;
                hp.VisibleNodeMask = 1;

                D3D12_RESOURCE_DESC rd = {};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Alignment = 0;
                rd.Width = bytes;
                rd.Height = 1;
                rd.DepthOrArraySize = 1;
                rd.MipLevels = 1;
                rd.Format = DXGI_FORMAT_UNKNOWN;
                rd.SampleDesc.Count = 1;
                rd.SampleDesc.Quality = 0;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                rd.Flags = D3D12_RESOURCE_FLAG_NONE;

                return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(out));
                };

            wil::com_ptr<ID3D12Resource> vb, ib, cb;
            CreateUpload(sizeof(vertices), vb.put());
            CreateUpload(sizeof(indices), ib.put());
            const UINT cbSize = 256; // constant buffer (aligned)
            CreateUpload(cbSize, cb.put());

            // Upload geometry
            {
                void* p = nullptr;
                vb->Map(0, nullptr, &p); memcpy(p, vertices, sizeof(vertices)); vb->Unmap(0, nullptr);
                ib->Map(0, nullptr, &p); memcpy(p, indices, sizeof(indices));  ib->Unmap(0, nullptr);
            }
            D3D12_VERTEX_BUFFER_VIEW vbv{};
            vbv.BufferLocation = vb->GetGPUVirtualAddress();
            vbv.SizeInBytes = (UINT)sizeof(vertices);
            vbv.StrideInBytes = sizeof(V);

            D3D12_INDEX_BUFFER_VIEW ibv{};
            ibv.BufferLocation = ib->GetGPUVirtualAddress();
            ibv.SizeInBytes = (UINT)sizeof(indices);
            ibv.Format = DXGI_FORMAT_R16_UINT;

            // Persistently map CB
            uint8_t* cbPtr = nullptr;
            cb->Map(0, nullptr, reinterpret_cast<void**>(&cbPtr));

            // ——— D3D11on12 + ImGui setup ———

            wil::com_ptr<ID3D11Device>           d3d11;
            wil::com_ptr<ID3D11DeviceContext>    d3d11ctx;
            UINT d3d11Flags = 0;
#if defined(_DEBUG)
            d3d11Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

            ID3D12CommandQueue* queues[] = { queue.get() };
            D3D_FEATURE_LEVEL flOut = {};
            const bool have11on12 = SUCCEEDED(D3D11On12CreateDevice(
                device.get(),
                d3d11Flags,
                nullptr, 0,
                reinterpret_cast<IUnknown**>(queues), 1,
                0,
                d3d11.put(),
                d3d11ctx.put(),
                &flOut));

            wil::com_ptr<ID3D11On12Device> d3d11on12;
            std::vector<wil::com_ptr<ID3D11Resource>> wrapped;
            std::vector<wil::com_ptr<ID3D11RenderTargetView>> wrappedRTVs;

            if (have11on12) {
                d3d11->QueryInterface(IID_PPV_ARGS(&d3d11on12));
                wrapped.resize(frameCount);
                wrappedRTVs.resize(frameCount);

                for (UINT i = 0; i < frameCount; ++i) {
                    D3D11_RESOURCE_FLAGS rf = {};          // (fix for SDK struct size)
                    rf.BindFlags = D3D11_BIND_RENDER_TARGET;

                    d3d11on12->CreateWrappedResource(
                        backbuffers[i].get(),
                        &rf,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, // InState for Acquire
                        D3D12_RESOURCE_STATE_PRESENT,       // OutState for Release
                        IID_PPV_ARGS(wrapped[i].put()));

                    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc11 = {};
                    rtvDesc11.Format = backbufferFormat;
                    rtvDesc11.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    d3d11->CreateRenderTargetView(wrapped[i].get(), &rtvDesc11, wrappedRTVs[i].put());
                }

                // Backends (context already exists)
                ImGui_ImplWin32_Init(hwnd);
                ImGui_ImplDX11_Init(d3d11.get(), d3d11ctx.get());
            }

            // Timing
            auto t0 = std::chrono::high_resolution_clock::now();

            // ——— main loop ———
            while (!window.IsShuttingDown()) {
                const UINT idx = swap->GetCurrentBackBufferIndex();
                const float w = (float)window.GetWidth();
                const float h = (float)window.GetHeight();

                // Update MVP (spin)
                float t = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - t0).count();
                float angle = t * XM_PIDIV4;
                XMMATRIX world = XMMatrixRotationY(angle) * XMMatrixRotationX(angle * 0.5f);
                XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -5, 0), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
                XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (w > 0 && h > 0) ? (w / h) : 1.0f, 0.1f, 100.0f);
                XMMATRIX mvp = XMMatrixTranspose(world * view * proj);
                memcpy(cbPtr, &mvp, sizeof(mvp));

                // Record DX12 cube
                allocs[idx]->Reset();
                cmd->Reset(allocs[idx].get(), pso.get());

                // PRESENT -> RT
                D3D12_RESOURCE_BARRIER toRT = MakeTransition(backbuffers[idx].get(),
                    D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
                cmd->ResourceBarrier(1, &toRT);

                // Set RT + viewport
                D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = rtvHeap->GetCPUDescriptorHandleForHeapStart();
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtvStart.ptr + SIZE_T(idx) * SIZE_T(rtvStride) };
                cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                FLOAT clear[4] = { 0.07f, 0.07f, 0.12f, 1.0f };
                cmd->ClearRenderTargetView(rtv, clear, 0, nullptr);
                D3D12_VIEWPORT vp{ 0.f, 0.f, w ? w : 1.f, h ? h : 1.f, 0.f, 1.f };
                D3D12_RECT sc{ 0, 0, (LONG)(w ? w : 1), (LONG)(h ? h : 1) };
                cmd->RSSetViewports(1, &vp);
                cmd->RSSetScissorRects(1, &sc);

                cmd->SetGraphicsRootSignature(rootSig.get());
                cmd->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress());
                cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmd->IASetVertexBuffers(0, 1, &vbv);
                cmd->IASetIndexBuffer(&ibv);
                cmd->DrawIndexedInstanced(36, 1, 0, 0, 0);

                // Submit cube work
                cmd->Close();
                ID3D12CommandList* lists[] = { cmd.get() };
                queue->ExecuteCommandLists(1, lists);

                if (have11on12) {
                    // ImGui frame
                    ImGui_ImplDX11_NewFrame();
                    ImGui_ImplWin32_NewFrame();
                    ImGui::NewFrame();

                    ImGui::SetNextWindowBgAlpha(0.35f);
                    if (ImGui::Begin("DX12 Fallback Overlay", nullptr,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
                        ImGui::Text("DX12 cube + ImGui (D3D11on12)");
                        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
                        ImGui::Text("Backbuffer: %u × %u", (unsigned)window.GetWidth(), (unsigned)window.GetHeight());
                    }
                    ImGui::End();
                    ImGui::Render();

                    // Acquire wrapped backbuffer (handles RT->Present transitions for us)
                    ID3D11Resource* toAcquire[] = { wrapped[idx].get() };
                    d3d11on12->AcquireWrappedResources(toAcquire, 1);

                    ID3D11RenderTargetView* rtvs[] = { wrappedRTVs[idx].get() };
                    d3d11ctx->OMSetRenderTargets(1, rtvs, nullptr);
                    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

                    d3d11on12->ReleaseWrappedResources(toAcquire, 1);
                    d3d11ctx->Flush();
                }
                else {
                    // If 11on12 failed, do explicit RT->PRESENT transition here (not needed when using wrapped resources)
                    allocs[idx]->Reset();
                    cmd->Reset(allocs[idx].get(), nullptr);
                    D3D12_RESOURCE_BARRIER toPresent = MakeTransition(backbuffers[idx].get(),
                        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                    cmd->ResourceBarrier(1, &toPresent);
                    cmd->Close();
                    ID3D12CommandList* lists2[] = { cmd.get() };
                    queue->ExecuteCommandLists(1, lists2);
                }

                // Present
                if (FAILED(swap->Present(1, 0))) break;
                WaitForGPU(queue.get(), fence.get(), fenceValue, fenceEvent);
            }

            if (have11on12) {
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
            }

            if (cbPtr) cb->Unmap(0, nullptr);
            if (fenceEvent) CloseHandle(fenceEvent);
        }

    }
} // namespace DX12::Example
