#pragma once

// The Yakuza 6 generation of the pxd sl layer, DE-FORKED 2026-08-09. This folder used to be a
// full copy of the base layer (pxd/LJ) predating the K2 rule - "the primitives are
// byte-identical to the Lost Judgment ones, so reuse them rather than cloning" - and the copy
// had drifted BEHIND the base (async Open/Create still unimplemented, the t_fixed_deque
// reserve fix missing). Everything byte-identical now comes FROM the base; what remains here
// is genuinely this generation's own, each divergence measured:
//
//   * sl::context_t - the Y6 layout: handle_free_queue @0x600 (base 0x6C0), pool sync @0x1B40,
//     file_handle_pool @0x1B48, and sync_archive_condvar @0x1B80 as a HANDLE to an
//     archive_lock object. LJ/YLAD later replaced that handle with a recursive spinlock WORD
//     (@0x1C20), which is why pxd::sl::archive_lock_wlock takes uint32_t* - the Y6 host
//     therefore imports its DLL's handle-taking wlock/wunlock through value shims (VF5FS.cpp)
//     and points pxd::sl::p_sync_archive_condvar at its handle field.
//
// The HEAD of the context (through p_archive_async_request @0x120) matches the base
// field-for-field - asserted on both sides - which is what lets the Y6 host set
// pxd::sl::sm_context to this context and every shared primitive keep working: the K2
// pattern, applied to the generation that predates it.
//
// csl_pad is the base's now, which also retires this generation's private raw-XInput +
// hardcoded-keyboard set_state: Y6 input goes through the shared binding layer
// (source/input) like every other host.

#include "../LJ/sl.h"

namespace vf5fs
{
	namespace Y6
	{
		using pxd::csl_pad;
		using pxd::sl::handle_t;
		using pxd::spinlock_t;
		using pxd::rwspinlock_t;
		using pxd::sl::mutex_t;
		using pxd::t_locked_queue;
		using pxd::t_fixed_deque;
		using pxd::isl_file_access;
		using pxd::csl_file_access_archive;
		using pxd::csl_file_async_request;

		namespace sl
		{
			// The shared machinery, aliased so the Y6 host still reads as `sl::...` - same
			// names, ONE implementation (and one set of import slots: writing through these
			// aliases writes pxd::sl's pointers, which is the point).
			using pxd::sl::handle_create;
			using pxd::sl::handle_create_internal;
			using pxd::sl::semaphore_create;
			using pxd::sl::thread_create;
			using pxd::sl::file_open;
			using pxd::sl::file_create;
			using pxd::sl::file_open_internal;
			using pxd::sl::file_create_internal;
			using pxd::sl::file_read;
			using pxd::sl::file_close;
			using pxd::sl::file_write;
			using pxd::sl::file_handle_destroy;
			using pxd::sl::file_handle_internal_t;
			using pxd::sl::handle_internal_buffer_t;
			using pxd::sl::export_context_t;
			using pxd::sl::semaphore_internal_t;
			using pxd::sl::thread_internal_t;
			using pxd::sl::archive_lock;
			using pxd::sl::file_handle_event;
			using pxd::sl::handle_instance;
			using pxd::sl::file_handle_instance;
			using pxd::sl::semaphore_handle_instance;

			// This generation's own context layout - see the header comment for what moved.
			struct context_t
			{
				uint32_t tag_id;
				uint32_t version;
				uint32_t size_of_struct;
				export_context_t export_context;
				uint32_t processor_num;
				uint64_t main_thread_id;
				uint64_t processor_affinity_mask;
				void* p_temp_work;
				size_t temp_work_size;
				uint64_t count_frequency;
				double count_per_milli_second;
				double count_per_micro_second;
				double count_per_nano_second;
				double count_per_tick;
				union
				{
					pxd::sl::handle_internal_buffer_t* p_handle_buffer;
					pxd::sl::handle_internal_t* p_handle_tbl;
				} handles;
				uint32_t handle_max;
				uint32_t file_handle_max;
				uint32_t file_callback_thread_stack_size;
				std::byte gap3[12];
				isl_file_access* p_file_access;
				csl_file_async_request* p_file_async_request;
				std::byte gap6[120];
				csl_file_access_archive* p_archive_access;
				csl_file_async_request* p_archive_async_request;
				std::byte gap[1240];
				t_locked_queue<handle_internal_buffer_t> handle_free_queue;
				std::byte gap2[5408];
				spinlock_t sync_file_handle_pool;
				std::byte gap5[4];
				t_fixed_deque<file_handle_internal_t*> file_handle_pool;
				std::byte gap4[32];
				handle_t sync_archive_condvar;
			};
			// Validate important offsets. The HEAD (through 0x120) matches pxd::sl::context_t
			// on purpose - the shared primitives depend on it; the TAIL is this generation's.
			static_assert(offsetof(context_t, handles) == 0x70);
			static_assert(offsetof(context_t, p_file_access) == 0x90);
			static_assert(offsetof(context_t, p_file_async_request) == 0x98);
			static_assert(offsetof(context_t, p_archive_access) == 0x118);
			static_assert(offsetof(context_t, p_archive_async_request) == 0x120);
			static_assert(offsetof(context_t, handle_free_queue) == 0x600);
			static_assert(offsetof(context_t, sync_file_handle_pool) == 0x1B40);
			static_assert(offsetof(context_t, file_handle_pool) == 0x1B48);
			static_assert(offsetof(context_t, sync_archive_condvar) == 0x1B80);
			static_assert(offsetof(pxd::sl::context_t, handles) == offsetof(context_t, handles));
			static_assert(offsetof(pxd::sl::context_t, p_archive_async_request)
				== offsetof(context_t, p_archive_async_request));

			// The Y6-typed view of the running context. The same object is also published as
			// pxd::sl::sm_context (cast; head-compatible) so the shared primitives can read it.
			extern context_t* sm_context;
		}
	}
}
