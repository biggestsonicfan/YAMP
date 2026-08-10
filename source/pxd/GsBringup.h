#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;

// The host-provided gs bring-up pieces the DX11-era hosts (YLAD VF2, Kiwami 2) duplicated
// line for line (2026-08-09 dedup): the zero-filling allocator trio, the pxd shadow-state
// attach with its embedded GUID, the t_instance_tbl bitmap seeding, the packed-dims helper
// and the cgs_device_context stand-in block. What stays per-host is every ADDRESS and
// ORDERING fact: the vtable slot each module frees through (K2 frees at slot 3 where YLAD
// frees at slot 2 - measured per module), the instance-table RVAs (YLAD gs+0x101718, K2
// gs+0x1808), the capacities, and the shared-symbol slot meanings.
namespace pxd
{
	namespace GsBringup
	{
		// Zero-fill like the host csl_allocator - the pxd engine expects zeroed allocations.
		void* __fastcall ZeroAlloc(void* self, size_t size, size_t align);
		void __fastcall AlignedFree(void* self, void* p);
		void __fastcall AllocNoop(void*);

		// The cswap_chain packed dimension word: (w-1) | (h-1)<<14, read back as the viewport.
		constexpr uint32_t PackDims(uint32_t w, uint32_t h)
		{
			return (w - 1) | ((h - 1) << 14);
		}

		// sbgl keeps its per-device-context shadow state in a block attached to the
		// ID3D11DeviceContext with a pxd GUID (module .rdata; the same bytes in both
		// generations), fetched with GetPrivateData(guid, 8, &blockPtr). The render-target
		// setter writes bound targets and the viewport straight into the block, so an
		// unattached (null) block is an immediate AV write. The host's device-start attaches
		// it in the real game; here YAMP does.
		void AttachCtxShadowState(ID3D11DeviceContext* dc);

		// The handle (instance) table both modules' bitmap allocator walks: object array,
		// free-bitmap, capacity, word cursor. A zeroed table can NEVER allocate (free_top <
		// free_words is 0 < 0), which is why the host must seed them.
		struct InstanceTable
		{
			void** tbl;
			uint64_t* free_tbl;
			uint32_t status;
			uint32_t max;
			uint32_t free_top;
			uint32_t free_words;
		};
		static_assert(sizeof(InstanceTable) == 0x20);

		// Seed `count` contiguous tables starting at `first` (0x20 stride), capacities from
		// `caps`. A table already built (tbl non-null) is left alone, so a re-entrant caller
		// is safe.
		void SeedInstanceTables(uint8_t* first, const uint32_t* caps, size_t count);
		void SeedInstanceTablesUniform(uint8_t* first, uint32_t capacity, size_t count);

		// The cgs_device_context stand-in both hosts hand their module: a 0x40000 zeroed block
		// with the four host-provided fields - +0x18 the immediate ID3D11DeviceContext,
		// +0x28/+0x30 the lazily-filling cb/up buffer pool tables (0x40000 apiece; the module
		// creates D3D11 buffers into empty slots), +0x38 the render-state block (0x8000;
		// reset_state_all writes through it unconditionally). Allocates once through `slot`,
		// refreshes the context pointer every call - idempotent, callable per frame.
		uint8_t* EnsureDeviceContextBlock(uint8_t*& slot, ID3D11DeviceContext* dc);
	}
}
