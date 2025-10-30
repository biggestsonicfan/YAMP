// LJ/StF_DX12.cpp
// DirectX 12 renderer for LJ::StF with ImGui overlay drawn via RenderWindow's D3D11on12.
// Build target: call LJ::StF::DX12::Run(RenderWindow&).

#include <windows.h>
#include <cstdint>
#include <vector>
#include <chrono>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include "../wil/com.h"
#include "../RenderWindow.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

namespace LJ {
namespace StF {
namespace DX12 {

namespace {

// small helpers
inline D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

inline void WaitForGPU(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64& fv, HANDLE e)
{
    const UINT64 v = ++fv;
    q->Signal(f, v);
    if (f->GetCompletedValue() < v) {
        f->SetEventOnCompletion(v, e);
        WaitForSingleObject(e, INFINITE);
    }
}

// in-memory shaders (simple MVP + vertex color)
wil::com_ptr<ID3DBlob> gVS, gPS;
bool CompileShaders()
{
    static const char* kVS = R"(
        cbuffer CB : register(b0) { float4x4 uMVP; };
        struct VSIn  { float3 pos : POSITION; float4 col : COLOR; };
        struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };
        VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.pos,1), uMVP); o.col = i.col; return o; }
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
    if (FAILED(D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, gVS.put(), err.put()))) {
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        return false;
    }
    err.reset();
    if (FAILED(D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, gPS.put(), err.put()))) {
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        return false;
    }
    return true;
}

} // anon

void Run(RenderWindow& window)
{
    // Grab RenderWindow’s DX12 + swapchain; it already created D3D11on12 + ImGui backends for us.
    wil::com_ptr<ID3D12Device>       device = window.GetD3D12Device();
    wil::com_ptr<ID3D12CommandQueue> queue  = window.GetD3D12Queue();
    wil::com_ptr<IDXGISwapChain3>    swap   = window.GetSwapChain3();
    if (!device || !queue || !swap) return;  // sanity

    DXGI_SWAP_CHAIN_DESC1 scd{}; swap->GetDesc1(&scd);
    const UINT frameCount = scd.BufferCount ? scd.BufferCount : 2;
    const DXGI_FORMAT bbFmt = scd.Format ? scd.Format : DXGI_FORMAT_B8G8R8A8_UNORM;

    // RTV heap + backbuffers
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{}; rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvDesc.NumDescriptors = frameCount;
    wil::com_ptr<ID3D12DescriptorHeap> rtvHeap; device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
    const UINT rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    std::vector<wil::com_ptr<ID3D12Resource>> backbuffers(frameCount);
    {
        auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < frameCount; ++i) {
            swap->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i]));
            D3D12_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format = bbFmt; rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(backbuffers[i].get(), &rtv, h);
            h.ptr += rtvInc;
        }
    }

    // Command allocators + list + fence
    std::vector<wil::com_ptr<ID3D12CommandAllocator>> allocs(frameCount);
    for (auto& a : allocs) device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    wil::com_ptr<ID3D12GraphicsCommandList> cmd; device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0].get(), nullptr, IID_PPV_ARGS(&cmd));
    cmd->Close();

    wil::com_ptr<ID3D12Fence> fence; device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fenceValue = 0; HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Root signature (CBV @ b0)
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 1; rsd.pParameters = &rp; rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    wil::com_ptr<ID3DBlob> rsBlob, rsErr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) {
        if (rsErr) OutputDebugStringA((char*)rsErr->GetBufferPointer());
        return;
    }
    wil::com_ptr<ID3D12RootSignature> rootSig; device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));

    if (!CompileShaders()) return;

    // PSO
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_BLEND_DESC blend{}; for (auto& rt : blend.RenderTarget) { rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; }
    D3D12_RASTERIZER_DESC rast{}; rast.FillMode = D3D12_FILL_MODE_SOLID; rast.CullMode = D3D12_CULL_MODE_BACK; rast.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC dss{}; dss.DepthEnable = FALSE; dss.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; dss.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.get();
    psoDesc.VS = { gVS->GetBufferPointer(), gVS->GetBufferSize() };
    psoDesc.PS = { gPS->GetBufferPointer(), gPS->GetBufferSize() };
    psoDesc.BlendState = blend;
    psoDesc.RasterizerState = rast;
    psoDesc.DepthStencilState = dss;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.InputLayout = { layout, _countof(layout) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1; psoDesc.RTVFormats[0] = bbFmt; psoDesc.SampleDesc.Count = 1;
    wil::com_ptr<ID3D12PipelineState> pso; device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));

    // Placeholder geometry (spinning cube).
    struct V { XMFLOAT3 p; XMFLOAT4 c; };
    const V verts[] = {
        {{-1,-1, 1},{1,0,0,1}}, {{ 1,-1, 1},{1,0,0,1}}, {{ 1, 1, 1},{1,0,0,1}}, {{-1, 1, 1},{1,0,0,1}},
        {{-1,-1,-1},{0,1,0,1}}, {{ 1,-1,-1},{0,1,0,1}}, {{ 1, 1,-1},{0,1,0,1}}, {{-1, 1,-1},{0,1,0,1}},
    };
    const uint16_t idxs[] = { 0,1,2, 0,2,3, 1,5,6, 1,6,2, 5,4,7, 5,7,6, 4,0,3, 4,3,7, 3,2,6, 3,6,7, 4,5,1, 4,1,0 };

    auto CreateUpload = [&](SIZE_T bytes, ID3D12Resource** out) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD; hp.CreationNodeMask = hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(out));
    };
    wil::com_ptr<ID3D12Resource> vb, ib, cb; CreateUpload(sizeof(verts), vb.put()); CreateUpload(sizeof(idxs), ib.put()); CreateUpload(256, cb.put());
    { void* p=nullptr; vb->Map(0,nullptr,&p); memcpy(p, verts, sizeof(verts)); vb->Unmap(0,nullptr);
      ib->Map(0,nullptr,&p); memcpy(p, idxs, sizeof(idxs)); ib->Unmap(0,nullptr); }
    D3D12_VERTEX_BUFFER_VIEW vbv{ vb->GetGPUVirtualAddress(), (UINT)sizeof(verts), (UINT)sizeof(V) };
    D3D12_INDEX_BUFFER_VIEW  ibv{ ib->GetGPUVirtualAddress(), (UINT)sizeof(idxs),  DXGI_FORMAT_R16_UINT };
    uint8_t* cbPtr = nullptr; cb->Map(0, nullptr, (void**)&cbPtr);

    // Timing
    auto t0 = std::chrono::high_resolution_clock::now();

    // Main loop
    while (!window.IsShuttingDown()) {
        const UINT idx = swap->GetCurrentBackBufferIndex();
        const float W = (float)window.GetWidth(), H = (float)window.GetHeight();

        // Update MVP (spin)
        float t = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - t0).count();
        XMMATRIX world = XMMatrixRotationY(t * XM_PIDIV4) * XMMatrixRotationX(t * (XM_PI / 8.0f));
        XMMATRIX view  = XMMatrixLookAtLH(XMVectorSet(0,0,-5,0), XMVectorZero(), XMVectorSet(0,1,0,0));
        XMMATRIX proj  = XMMatrixPerspectiveFovLH(XM_PIDIV4, (W>0&&H>0)? (W/H):1.0f, 0.1f, 100.0f);
        XMMATRIX mvp   = XMMatrixTranspose(world * view * proj);
        memcpy(cbPtr, &mvp, sizeof(mvp));

        // DX12 pass (present->rt)
        allocs[idx]->Reset();
        cmd->Reset(allocs[idx].get(), pso.get());

        D3D12_RESOURCE_BARRIER toRT = MakeTransition(backbuffers[idx].get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &toRT);

        // RTV + viewport
        D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtvStart.ptr + SIZE_T(idx) * SIZE_T(rtvInc) };
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const FLOAT clear[4] = { 0.07f, 0.07f, 0.12f, 1.0f };
        cmd->ClearRenderTargetView(rtv, clear, 0, nullptr);
        D3D12_VIEWPORT vp{ 0.f, 0.f, W?W:1.f, H?H:1.f, 0.f, 1.f };
        D3D12_RECT sc{ 0, 0, (LONG)(W?W:1), (LONG)(H?H:1) };
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);

        // Bind + draw
        cmd->SetGraphicsRootSignature(rootSig.get());
        cmd->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0, 1, &vbv);
        cmd->IASetIndexBuffer(&ibv);
        cmd->DrawIndexedInstanced(36, 1, 0, 0, 0);

        // Submit cube pass
        cmd->Close();
        ID3D12CommandList* lists[] = { cmd.get() };
        queue->ExecuteCommandLists(1, lists);

        // ---- ImGui overlay via RenderWindow’s D3D11on12 bridge ----
        window.BeginFrame();
        window.NewImGuiFrame();
        window.RenderImGui();
        window.EndFrame();

        // Present
        if (FAILED(swap->Present(1, 0))) break;
        WaitForGPU(queue.get(), fence.get(), fenceValue, fenceEvent);
    }

    if (cbPtr) cb->Unmap(0, nullptr);
    if (fenceEvent) CloseHandle(fenceEvent);
}

}}} // namespace LJ::StF::DX12
