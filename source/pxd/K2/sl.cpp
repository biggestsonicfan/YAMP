#include "sl.h"

#include "../LJ/file_access.h"
#include "../LJ/async_request.h"
#include "../../DebugLog.h"
#include "../../wil/resource.h"

namespace pxd
{
	namespace K2
	{
		static context_t* s_context = nullptr;

		context_t* context() { return s_context; }

		void set_context(void* p_context)
		{
			s_context = static_cast<context_t*>(p_context);
			// pxd::sl's own helpers (initialize, handle_instance, the csl_file_access family) only
			// ever touch fields at or below sz_fs_root, which this generation keeps at the LJ
			// offsets — so they can share the very same object. Anything reaching the shifted tail
			// goes through K2::context() instead.
			sl::sm_context = reinterpret_cast<sl::context_t*>(p_context);
		}

		bool InitHandles(uint32_t max_handles)
		{
			static_assert(sizeof(sl::handle_internal_buffer_t) == 8);
			if (s_context == nullptr) return false;

			const size_t bytes = static_cast<size_t>(max_handles) * sizeof(sl::handle_internal_buffer_t);
			auto* buf = static_cast<sl::handle_internal_buffer_t*>(_aligned_malloc(bytes, 16));
			if (buf == nullptr) return false;
			memset(buf, 0, bytes);

			// handle_create_internal derives a handle's index as ((node - *(int*)(sl+0x70)) >> 3),
			// so +0x70 is the array base and the nodes must be one contiguous 8-byte-strided run.
			s_context->handles.p_handle_buffer = buf;
			s_context->handle_max = max_handles;

			auto& queue = s_context->handle_free_queue;
			queue.reset_bootstrap();
			for (uint32_t i = 0; i < max_handles; i++)
			{
				queue.append_unlocked(&buf[i]);
			}

			DebugLogFile("[pxd::K2] handles: %u nodes at %p, free queue at sl+0xA80 (size=%u)\n",
				max_handles, static_cast<void*>(buf), queue.size());
			return true;
		}

		void PatchSl()
		{
			if (s_context == nullptr) return;

			// A spinlock WORD, not a handle: the lock reads *ptr (high 16 = writer, low 16 = reader
			// count). Start it unlocked, and point pxd::sl at THIS generation's field — the shared
			// csl_archive::create_instance would otherwise take the LJ offset (0x1C20), which lands
			// 0x3C0 bytes short of where this context keeps it.
			s_context->sync_archive_condvar = 0;
			sl::p_sync_archive_condvar = &s_context->sync_archive_condvar;

			s_context->p_file_access = new csl_file_access;
			s_context->p_archive_access = new csl_file_access_archive;

			// The file handle pool the module pops from in file_handle_create (FUN_180064550):
			// it locks sl+0x1FC0, reads the array at +0x1FC8, the capacity at +0x1FD0, the live
			// count at +0x1FD4 and the head index at +0x1FD8 — exactly t_fixed_deque's member
			// order. An unreserved or wrongly-placed pool reads as count 0, which makes every
			// single file open fail with no diagnostic of its own.
			static constexpr uint32_t kPoolCapacity = 0x800;
			static constexpr uint32_t kNumFileHandles = 250;

			s_context->file_handle_max = kPoolCapacity;
			s_context->file_handle_pool.reserve(kPoolCapacity);

			auto* handles = new sl::file_handle_internal_t[kNumFileHandles] {};
			for (auto& handle : wil::make_range(handles, kNumFileHandles))
			{
				handle._afterConstruct();
				const auto handlePtr = &handle;
				s_context->file_handle_pool.push_back(&handlePtr);
			}

			static constexpr uint32_t kNumRequests = kNumFileHandles + 64;
			s_context->p_file_async_request =
				new csl_file_async_request(&s_context->p_file_access, kNumRequests);
			s_context->p_archive_async_request = new csl_file_async_request(
				reinterpret_cast<isl_file_access**>(&s_context->p_archive_access), kNumRequests);

			DebugLogFile("[pxd::K2] sl file subsystem: pool at sl+0x1FC8 (%u handles), access=%p archive=%p\n",
				kNumFileHandles, static_cast<void*>(s_context->p_file_access),
				static_cast<void*>(s_context->p_archive_access));
		}
	}
}
