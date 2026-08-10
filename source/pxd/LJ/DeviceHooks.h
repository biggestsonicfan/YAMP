#pragma once

// The D3D12 module-DLL interop and crash-diagnostics layer for the LJ-era hosts, extracted
// from RenderWindow.cpp (2026-08-09) where it had grown to a third of the file: the
// CreatePipelineState capture/fixup hook for the module's stream PSOs, the descriptor-heap
// capture the gs rings are wired against, DRED dumping with its vectored exception handler,
// and the debug-layer message sink. Its consumers were always here in pxd/LJ (HostCdevice,
// PatchGs) - they used to re-declare these entry points by hand rather than include
// RenderWindow.h.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>

// Install the whole layer on the real device: the DRED breakpoint handler and the
// CreatePipelineState / CreateDescriptorHeap vtable hooks. Called once by the RenderWindow
// constructor, right after D3D12CreateDevice.
void InstallDeviceHooks(ID3D12Device* device);

// D3D12 debug-layer message sink: every validation message goes through DebugLogFile so the
// message emitted right before a driver crash is on disk. The RenderWindow constructor
// registers it via ID3D12InfoQueue1::RegisterMessageCallback when the debug layer is on.
void CALLBACK D3D12DebugMessageCallback(D3D12_MESSAGE_CATEGORY category,
	D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void* context);

// The root signature built by the PSO fixup. The DLL sets the PSO but never
// SetGraphicsRootSignature on the list it draws on (that is the host's job in LJ); the
// SetPipelineState hook re-binds this after each SetPipelineState.
ID3D12RootSignature* GetCapturedRootSignature();

// The DLL's own large shader-visible heaps (CBV/SRV/UAV ~1M + SAMPLER 2048), captured from
// the CreateDescriptorHeap hook - the gs descriptor rings must reference THESE so the DLL's
// root-table GPU handles resolve into the bound heap. See PatchGs.
ID3D12DescriptorHeap* GetDllRingCbvSrvHeap();
ID3D12DescriptorHeap* GetDllRingSamplerHeap();

// True only for PSOs the DLL created. The command-list vtable is shared with d3d11on12's
// blit list; gate the module heap/root-sig injection on this so the blit is not corrupted.
bool IsModulePso(void* pso);

// Dump DRED (last GPU op + page-fault resource) on device removal, appending to
// d3d12_debug.log. Used by the frame-submit path to record a GPU hang once.
void DumpDredNow();
