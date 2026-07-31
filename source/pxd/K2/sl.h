#pragma once

// The sl half of the pxd platform layer for the **Yakuza Kiwami 2** generation
// (sl context 0xF3C0 / gs context 0x202140 — a third generation, distinct from Lost Judgment's
// 0xF000/0x388A00 and Like-a-Dragon's 0xF000/0x3820C0).
//
// Only the CONTEXT LAYOUT is generation-specific; the primitives (handle_t, file_handle_internal_t,
// t_locked_queue, t_fixed_deque, the csl_file_access family) are byte-identical to the Lost
// Judgment ones, so this header reuses them rather than cloning them.
//
// ## The layout, and why it needed its own header
//
// K2's context is LJ's with **0x3C0 bytes inserted immediately before handle_free_queue**. Every
// field up to and including sz_fs_root sits at exactly the LJ offset (all verified against a live
// Kiwami 2 host context); everything from handle_free_queue onward is shifted by +0x3C0. Three
// independent landmarks pin that delta, each read out of THIS DLL's own code rather than inferred:
//
//   * `handle_create_internal` (FUN_180066710) pops the free queue at **sl+0xA80**   (LJ 0x6C0)
//   * `file_handle_create`     (FUN_180064550) takes the spinlock at **sl+0x1FC0** and pops the
//     file handle pool at **sl+0x1FC8**                                    (LJ 0x1C00 / 0x1C08)
//   * the context's own size_of_struct is **0xF3C0**                                 (LJ 0xF000)
//
// and 0xF3C0 - 0xF000 == 0x6C0 -> 0xA80 == 0x1C00 -> 0x1FC0 == 0x3C0, so the whole tail moves as
// one block. The per-type handle counters follow the same rule: LJ 0x1800 -> K2 0x1BC0.
//
// Reusing `pxd::sl::context_t` for K2 does NOT crash — it fails SILENTLY, which is worse. The
// module's `file_handle_create` finds a pool the host filled 0x3C0 bytes too low, sees a zero
// count, returns a null handle, and every file open fails; resource loads then come back as null
// blobs and the module faults far away (DLL+0x6D190, reading the SLLZ magic of a null buffer).

#include <cstddef>
#include <cstdint>

#include "../LJ/sl.h"

namespace pxd
{
	namespace K2
	{
		// The head of the struct is a field-for-field copy of pxd::sl::context_t on purpose: this
		// header is meant to be readable on its own as "the K2 sl context", and every offset that
		// matters is pinned by a static_assert below, so the two cannot drift silently.
		struct context_t
		{
			uint32_t tag_id;                                  // 'LBsl'
			uint32_t version;
			uint32_t size_of_struct;                          // 0xF3C0 for this generation
			uint32_t unknown_0;
			pxd::sl::export_context_t export_context;
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
				pxd::sl::handle_internal_buffer_t* p_handle_buffer;   // 0x70
				pxd::sl::handle_internal_t* p_handle_tbl;
			} handles;
			uint32_t handle_max;                              // 0x78
			uint32_t file_handle_max;                         // 0x7C
			uint32_t file_callback_thread_stack_size;         // 0x80
			pxd::sl::file_handle_internal_t* p_file_handle_tbl;  // 0x88
			isl_file_access* p_file_access;                   // 0x90
			csl_file_async_request* p_file_async_request;     // 0x98
			std::byte gap1[120];
			csl_file_access_archive* p_archive_access;        // 0x118
			csl_file_async_request* p_archive_async_request;  // 0x120
			csl_allocator* p_csl_allocator;
			csl_allocator* p_csl_allocator_with_pool;
			std::byte gap2[32];
			uint64_t* _p_csl_allocator;                       // 0x158 (the host's allocator object)
			uint64_t* __p_csl_allocator;
			void* p_libc_alloc;
			void* p_libc_realloc;
			void* p_pxd_std_free;
			void* p_libc_msize;
			uint64_t unknown_0_1;
			uint64_t unknown_value;
			std::byte gap3[240];
			uint64_t thread_sid_index;
			uint32_t is_utf8_file_path;
			uint32_t fs_root_len;
			char sz_fs_root[0x410];                           // 0x298 .. 0x6A8

			// ---- THE DIVERGENCE ----------------------------------------------------------------
			// Lost Judgment has 24 bytes of padding here. Kiwami 2 has 24 + 0x3C0. Whatever the
			// module keeps in the extra 0x3C0 is not mapped; nothing in YAMP touches it, and its
			// only job here is to place everything below at the offsets the module reads.
			std::byte gap_k2[24 + 0x3C0];

			t_locked_queue<pxd::sl::handle_internal_buffer_t> handle_free_queue;   // 0xA80
			std::byte gap4[5408];                             // holds the 256*16 tag table (0xBC0)
			                                                  // and the per-type counters (0x1BC0)
			uint32_t sync_file_handle_pool;                   // 0x1FC0, spinlock word
			std::byte gap4a[4];
			t_fixed_deque<pxd::sl::file_handle_internal_t*> file_handle_pool;      // 0x1FC8
			uint32_t sync_archive_condvar;                    // 0x1FE0, recursive spinlock WORD
			std::byte gap6[54188];
			void* allocated_heap;                             // 0xF390
			uint64_t heap_size;                               // 0xF398
			std::byte gap7[32];
		};

		// Head offsets — identical to the LJ generation, each confirmed against a live Kiwami 2
		// host sl context (the host's own, at exe+0x291EC00, which has all of these populated).
		static_assert(offsetof(context_t, handles) == 0x70);
		static_assert(offsetof(context_t, p_file_handle_tbl) == 0x88);
		static_assert(offsetof(context_t, p_file_access) == 0x90);
		static_assert(offsetof(context_t, p_file_async_request) == 0x98);
		static_assert(offsetof(context_t, p_archive_access) == 0x118);
		static_assert(offsetof(context_t, p_archive_async_request) == 0x120);
		static_assert(offsetof(context_t, _p_csl_allocator) == 0x158);
		static_assert(offsetof(context_t, sz_fs_root) == 0x298);
		// Tail offsets — the +0x3C0 block, each read out of the module's own accessors.
		static_assert(offsetof(context_t, handle_free_queue) == 0xA80);
		static_assert(offsetof(context_t, sync_file_handle_pool) == 0x1FC0);
		static_assert(offsetof(context_t, file_handle_pool) == 0x1FC8);
		static_assert(offsetof(context_t, sync_archive_condvar) == 0x1FE0);
		static_assert(offsetof(context_t, allocated_heap) == 0xF390);
		static_assert(sizeof(context_t) == 0xF3C0);

		// The module's own size gate, and what the host passes to pxd::sl::initialize().
		inline constexpr uint32_t kContextSize = 0xF3C0;

		// The K2 context, as the module's globals see it. pxd::sl::sm_context points at the same
		// object (the head fields it touches are at identical offsets, so pxd::sl's initialize(),
		// handle_instance() and the csl_file_access family all work unchanged); this accessor is
		// how anything reaching the SHIFTED tail must get there.
		context_t* context();
		void set_context(void* p_context);

		// Fills the free-handle queue at 0xA80. Replaces pxd::sl::handle_initialize(), which builds
		// LJ's queue at 0x6C0 and would leave the module's queue empty.
		bool InitHandles(uint32_t max_handles);

		// The K2 counterpart of pxd::PatchSl: same objects (YAMP's csl_file_access /
		// csl_file_access_archive / csl_file_async_request), written through THIS layout.
		void PatchSl();
	}
}
