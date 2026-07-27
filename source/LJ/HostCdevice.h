#pragma once

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace LJ
{
	namespace StF
	{
		// Builds the pxd host "cdevice" object that the Sonic the Fighters DX12 DLL
		// expects behind cdevice_common::g_pD3DDevice.
		//
		// The DLL treats that global as a POINTER to this object and reads, among others:
		//   +0x08   ID3D12Device*                       (device vtable calls)
		//   +0x38   intermediate-buffer freelist head   (packed: lo48=node, hi16=ABA)
		//   +0x40   { 0x20, 0x20, 0x20, 0x08 } metadata
		//   +0x68   allocator object (vtable alloc/free)
		//   +0x16e0 0x20-block node pool (deferred; not on the boot path)
		//   +0x17b0 embedded resource factory (vf+8 = create ID3D12Resource)
		//   +0x18d8/+0x18dc rwspinlocks (zeroed)
		//
		// Layout + seeding were reverse-engineered from a live Lost Judgment dump
		// (see cdevice-spec.md). Returns the pointer to store into the DLL global.
		// cdeviceCtor is the DLL's own cdevice constructor (imported via CDEVICE_CTOR):
		// void* __fastcall ctor(void* this). We call it on our buffer so the pxd engine
		// initializes every field (incl. the real cd3d12_mem_allocator factory at +0x17b0).
		void* BuildHostCdevice(ID3D12Device* device, ID3D12CommandQueue* queue, void* (*cdeviceCtor)(void*));

		// Returns the host cdevice built by BuildHostCdevice (null if not yet built).
		// gs::initialize_module copies context->export_context.sbgl_context.p_value[0] into
		// the cdevice_common::g_pD3DDevice global, so PatchGs must place THIS pointer there
		// (not the D3D11 device) — the DX12 renderer reads g_pD3DDevice+8 as the ID3D12Device.
		void* GetHostCdevice();

		// Builds a dedicated pxd command context (same 0x280 layout as the cdevice+0x28 pool
		// contexts, list left recording) for the cgs_device_context+0xC8 "command recording"
		// slot, which the LJ host normally binds. Requires BuildHostCdevice to have run first
		// (needs the ID3D12Device); returns null otherwise.
		void* BuildRenderCommandContext();
	}
}
