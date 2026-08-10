#include "GsBringup.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>

#include <malloc.h>
#include <cstring>

namespace pxd
{
	namespace GsBringup
	{
		void* __fastcall ZeroAlloc(void* /*self*/, size_t size, size_t align)
		{
			void* p = _aligned_malloc(size, align != 0 ? align : 16);
			if (p != nullptr) memset(p, 0, size);
			return p;
		}

		void __fastcall AlignedFree(void* /*self*/, void* p)
		{
			_aligned_free(p);
		}

		void __fastcall AllocNoop(void*) {}

		void AttachCtxShadowState(ID3D11DeviceContext* dc)
		{
			static const GUID kPxdCtxGuid =
				{ 0xa84b07f7, 0x3bd0, 0x4c4e, { 0x89, 0x90, 0xe9, 0xd5, 0xaf, 0x6e, 0xfb, 0xa2 } };
			static uint8_t s_ctxShadowState[0x1000]{};
			void* blockPtr = s_ctxShadowState;
			dc->SetPrivateData(kPxdCtxGuid, sizeof(blockPtr), &blockPtr);
		}

		void SeedInstanceTables(uint8_t* first, const uint32_t* caps, size_t count)
		{
			for (size_t i = 0; i < count; i++)
			{
				auto* t = reinterpret_cast<InstanceTable*>(first + i * 0x20);
				if (t->tbl != nullptr) continue;   // already built (re-entry safety)
				const uint32_t cap = caps[i];
				const uint32_t words = (cap + 63) / 64;
				t->tbl = new void* [cap]();
				t->free_tbl = new uint64_t[words];
				for (uint32_t w = 0; w < words; w++) t->free_tbl[w] = ~0ull;
				t->status = 0;
				t->max = cap;
				t->free_top = 0;
				t->free_words = words;
			}
		}

		void SeedInstanceTablesUniform(uint8_t* first, uint32_t capacity, size_t count)
		{
			for (size_t i = 0; i < count; i++)
			{
				SeedInstanceTables(first + i * 0x20, &capacity, 1);
			}
		}

		uint8_t* EnsureDeviceContextBlock(uint8_t*& slot, ID3D11DeviceContext* dc)
		{
			if (slot == nullptr) slot = new uint8_t[0x40000]();
			*reinterpret_cast<void**>(slot + 0x18) = dc;
			if (*reinterpret_cast<void**>(slot + 0x28) == nullptr)
			{
				*reinterpret_cast<void**>(slot + 0x28) = new uint8_t[0x40000]();
				*reinterpret_cast<void**>(slot + 0x30) = new uint8_t[0x40000]();
			}
			if (*reinterpret_cast<void**>(slot + 0x38) == nullptr)
			{
				*reinterpret_cast<void**>(slot + 0x38) = new uint8_t[0x8000]();
			}
			return slot;
		}
	}
}
