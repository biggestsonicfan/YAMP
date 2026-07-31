#pragma once

#include <cstdint>
#include <cstddef>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "pxd_types.h"
#include "sl_internal.h"

#include <windows.h>

namespace pxd
{

		class isl_file_access;
		class csl_file_access_archive;

		// sl definitions for Yakuza 6
		class csl_file_async_request;

		struct csl_pad
		{
		public:
			void set_state(unsigned int index);

			unsigned int m_now;
			unsigned int m_push;
			unsigned int m_pull;
			unsigned int m_prev;
			float m_x1;
			float m_y1;
			float m_x2;
			float m_y2;
			int m_button_frame[32];
			uint8_t m_buttons[32];
			uint8_t m_prev_buttons[32];
			unsigned int m_port;
			int m_user_id;
			bool m_is_connected;
			bool m_is_remote;
			std::byte gap[132];
		};
		static_assert(sizeof(csl_pad) == 0x170);

		// The LJ-era per-pad block a host copies into execute_info each frame (LJ FUN_1426bd820
		// copies 0x190 bytes per pad from the engine pad object). The first 0xE0 bytes are exactly
		// the csl_pad layout above — now/push/pull/prev, 4 float axes, button_frame[32],
		// buttons[32], prev_buttons[32], port/user/connected — the LJ variant is just longer, so a
		// host fills a csl_pad via set_state and memcpy's that 0xE0 prefix in. Button bits use the
		// same sl::BUTTON numbering (START = bit 8 = 0x100).
		//
		// Shared because BOTH LJ-era hosts consume it at execute_info+0x20 with a 0x190 stride:
		// m2ftg (see m2ftg.h) and VF5FS-LJ, whose reader FUN_1801F22C0 walks
		// `execute_info + 0x20 + i*0x190` for i < 2, taking m_now, m_x1/m_y1/m_x2/m_y2 and
		// m_buttons[4]/[5] (the analog triggers) before remapping the bits to its own scheme.
		struct lj_pad_t
		{
			unsigned int m_now;
			unsigned int m_push;
			unsigned int m_pull;
			unsigned int m_prev;
			float m_x1;
			float m_y1;
			float m_x2;
			float m_y2;
			int m_button_frame[32];
			uint8_t m_buttons[32];
			uint8_t m_prev_buttons[32];
			unsigned int m_port;
			int m_user_id;
			bool m_is_connected;
			bool m_is_remote;
			std::byte tail[0x190 - 0xEA];
		};
		static_assert(sizeof(lj_pad_t) == 0x190);
		static_assert(offsetof(lj_pad_t, m_buttons) == 0xA0);
		static_assert(offsetof(lj_pad_t, m_port) == 0xE0);

		// How much of a csl_pad a host copies into an lj_pad_t: the shared prefix, up to (but not
		// including) m_port — the host sets port/user/connected itself afterwards.
		inline constexpr size_t kLjPadCopyBytes = 0xE0;

		namespace sl {

			enum BUTTON
			{
				BUTTON_A = 0x0,
				BUTTON_B = 0x1,
				BUTTON_X = 0x2,
				BUTTON_Y = 0x3,
				BUTTON_LB = 0x4,
				BUTTON_RB = 0x5,
				BUTTON_LT = 0x6,
				BUTTON_RT = 0x7,
				BUTTON_START = 0x8,
				BUTTON_BACK = 0x9,
				BUTTON_L_THUMB = 0xA,
				BUTTON_R_THUMB = 0xB,
				BUTTON_UP = 0xC,
				BUTTON_DOWN = 0xD,
				BUTTON_LEFT = 0xE,
				BUTTON_RIGHT = 0xF,
				BUTTON_L_UP = 0x10,
				BUTTON_L_DOWN = 0x11,
				BUTTON_L_LEFT = 0x12,
				BUTTON_L_RIGHT = 0x13,
				BUTTON_R_UP = 0x14,
				BUTTON_R_DOWN = 0x15,
				BUTTON_R_LEFT = 0x16,
				BUTTON_R_RIGHT = 0x17,
				BUTTON_ALL_UP = 0x18,
				BUTTON_ALL_DOWN = 0x19,
				BUTTON_ALL_LEFT = 0x1A,
				BUTTON_ALL_RIGHT = 0x1B,
				BUTTON_DA_UP = 0x1C,
				BUTTON_DA_DOWN = 0x1D,
				BUTTON_DA_LEFT = 0x1E,
				BUTTON_DA_RIGHT = 0x1F,
				BUTTON_MAX = 0x20,
				BUTTON_UNKNOWN = 0xFF,
				BUTTON_FORCE_32BIT = 0x7FFFFFFF,
			};

			// Imported function
			extern handle_t* (*handle_create_internal)(handle_t* obj, void* ptr, uint32_t type);
			handle_t handle_create(void* ptr, uint32_t type);

			extern void* (*kernel_calloc_internal)(uint64_t bytes, uint32_t flags_or_zero);

			inline void* kernel_calloc(uint64_t bytes, uint32_t flags_or_zero) {
				void* _Dst;
				_Dst = kernel_calloc_internal(bytes, flags_or_zero);
				return _Dst;
			}

			handle_t semaphore_create(uint32_t initialCount);
			handle_t thread_create(uint32_t(*p_routine)(uint64_t), uint64_t arg, const char* name);

			inline handle_t* (*file_open_internal)(handle_t* obj, const char* in_sz_file_path);
			inline handle_t* (*file_create_internal)(handle_t* obj, const char* in_sz_file_path);
			handle_t file_open(const char* in_sz_file_path);
			handle_t file_create(const char* in_sz_file_path);

			inline int64_t(*file_read)(handle_t h_file, void* p_buffer, unsigned int read_size);
			inline int (*file_close)(handle_t h_file);
			int64_t file_write(handle_t h_file, const void* p_buffer, unsigned int write_size);

			struct alignas(16) file_handle_internal_t : public file_handle_t
			{
				volatile unsigned int m_flags;
				unsigned int m_buffer_size;
				unsigned int m_archive_file_no;
				unsigned int m_error_code;
				handle_t m_async_event;
				handle_t m_h_basefile;
				unsigned __int64 m_basefile_offset;
				void* mp_buffer;
				void* mp_cache; // TODO: csl_filecache_name
				uint64_t m_read_offset;
				uint64_t m_real_file_size;
				void (*mp_callback_func)(handle_t, unsigned int, void*);
				void* mp_callback_param;
				volatile uint64_t m_callback_execute_thread;
				FILE_ASYNC_METHOD m_callback_method;
				unsigned int m_req_item_index;
				unsigned int m_last_async_status;
				rwspinlock_t m_locked;
				file_handle_internal_t* mp_link;

				void begin_async_request();
				void end_async_request();
				void callback(FILE_ASYNC_METHOD type, uint32_t status);

			public:
				void _afterConstruct();
			};
			static_assert(sizeof(file_handle_internal_t) == 0x4D0);
			static_assert(offsetof(file_handle_internal_t, m_buffer_size) == 1116);

			inline void (*file_handle_destroy)(sl::file_handle_internal_t* p_handle);

			struct export_context_t
			{
				size_t size_of_struct;
				void* p_context;
			};

			struct alignas(16) semaphore_internal_t
			{
				uint32_t tag_id = 0x4D455348;
				void* h_semaphore;
			};

			struct alignas(16) thread_internal_t
			{
				uint32_t tag_id = 0x44525448;
				char sz_name[28];
				void* h_thread;
				uint64_t thread_id;
				uint64_t arg;
				uint32_t(*p_routine)(uint64_t arg);
			};
			static_assert(sizeof(thread_internal_t) == 0x40);

			struct context_t
			{
				uint32_t tag_id;
				uint32_t version;
				uint32_t size_of_struct;
				uint32_t unknown_0;
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
					handle_internal_buffer_t* p_handle_buffer; //node_base 0x70
					handle_internal_t* p_handle_tbl;
				} handles;
				uint32_t handle_max; //node count 0x78
				uint32_t file_handle_max;  //0x7c
				uint32_t file_callback_thread_stack_size; //0x80
				file_handle_internal_t* p_file_handle_tbl; //0x88
				isl_file_access* p_file_access;
				csl_file_async_request* p_file_async_request;
				std::byte gap1[120];
				csl_file_access_archive* p_archive_access;
				csl_file_async_request* p_archive_async_request;
				csl_allocator* p_csl_allocator;
				csl_allocator* p_csl_allocator_with_pool;
				std::byte gap2[32];
				uint64_t* _p_csl_allocator;
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
				char sz_fs_root[0x410];
				// Offsets below verified against pxd::sl::context_t::context_t (0x18006c990)
				// in the StF DLL: queue @0x6C0, tag table @0x800..0x1800, pool sync @0x1C00,
				// pool @0x1C08, tail fields @0xEFD0, total size 0xF000.
				std::byte gap3a[24];
				t_locked_queue<handle_internal_buffer_t> handle_free_queue; //0x6C0
				std::byte gap4[5408]; //contains the 256*16 tag table at 0x800
				uint32_t sync_file_handle_pool; //0x1C00, spinlock
				std::byte gap4a[4];
				t_fixed_deque<file_handle_internal_t*> file_handle_pool; //0x1C08
				uint32_t sync_archive_condvar; //0x1C20 recursive spinlock word (archive_lock_wlock reads *ptr, not a handle)
				std::byte gap6[54188];
				void* allocated_heap;  //0xEFD0
				uint64_t heap_size;    //0xEFD8
				std::byte gap7[32];
			};
			// Validate important offsets against the DLL's context_t constructor
			static_assert(offsetof(context_t, handles) == 0x70);
			static_assert(offsetof(context_t, p_file_handle_tbl) == 0x88);
			static_assert(offsetof(context_t, p_file_access) == 0x90);
			static_assert(offsetof(context_t, p_file_async_request) == 0x98);
			static_assert(offsetof(context_t, p_archive_access) == 0x118);
			static_assert(offsetof(context_t, p_archive_async_request) == 0x120);
			static_assert(offsetof(context_t, _p_csl_allocator) == 0x158);
			static_assert(offsetof(context_t, sz_fs_root) == 0x298);
			static_assert(offsetof(context_t, handle_free_queue) == 0x6C0);
			static_assert(offsetof(context_t, sync_file_handle_pool) == 0x1C00);
			static_assert(offsetof(context_t, file_handle_pool) == 0x1C08);
			static_assert(offsetof(context_t, allocated_heap) == 0xEFD0);
			static_assert(sizeof(context_t) == 0xF000);

			// TODO: Consider changing this to a pointer to real sm_context
			extern context_t* sm_context;

			// Custom types
			struct archive_lock
			{
				uint32_t tag_id = 0x4C575248;
				handle_t eventHandle1;
				handle_t eventHandle2;
				uint32_t unk1 = 0;
				uint32_t unk2 = 0;
				uint32_t unk3 = 0;
				uint32_t unk4 = 0;
				sl::mutex_t critSec1;
				sl::mutex_t critSec2;

			public:
				void _afterConstruct();
			};
			static_assert(sizeof(archive_lock) == 0x80);
			static_assert(offsetof(archive_lock, critSec1) == 32);
			static_assert(offsetof(archive_lock, critSec2) == 80);

			struct file_handle_event
			{
				void* gap;
				HANDLE eventHandle;

			public:
				void _afterConstruct();
			};
			static_assert(sizeof(file_handle_event) == 16);

			// Y:LAD changed this to a recursive_rwspinlock. These take a POINTER to the lock word
			// (they read *lock), NOT a handle — create_instance passes the archive condvar.
			// Kiwami 2 predates the recursive variant and uses a plain rwspinlock; its host imports
			// that generation's rlock/runlock pair into these two slots.
			extern void (*archive_lock_wlock)(uint32_t* lock);
			extern void (*archive_lock_wunlock)(uint32_t* lock);

			// The lock WORD itself moves between generations (LJ/YLAD 0x1C20, Kiwami 2 0x1FE0), so a
			// host whose context is not pxd::sl::context_t points this at its own field. Null means
			// "use sm_context->sync_archive_condvar", which keeps every existing host unchanged.
			extern uint32_t* p_sync_archive_condvar;
			uint32_t* sync_archive_condvar();


			template<typename T>
			inline T* handle_instance(handle_t handle, uint32_t type) //This fails for StF, why?
			{
				T* result = nullptr;
				if (handle.h.data.m_bank < sm_context->handle_max)
				{
					const handle_internal_t& internalHandle = sm_context->handles.p_handle_tbl[handle.h.data.m_bank];
					if (internalHandle.intern.data.m_serial == handle.h.data.m_serial && internalHandle.intern.data.m_type == type)
					{
						result = reinterpret_cast<T*>(internalHandle.intern.data.m_ptr << 4);
					}
				}

				return result;
			}

			inline file_handle_internal_t* file_handle_instance(handle_t handle)
			{
				return handle_instance<file_handle_internal_t>(handle, 5);
			}

			inline semaphore_internal_t* semaphore_handle_instance(handle_t handle)
			{
				return handle_instance<semaphore_internal_t>(handle, 2);
			}

			extern const char* const g_tag_names[14];

			extern const uint8_t* gDefaultTag128Ptr;

			int handle_initialize(uint32_t max_handles);
			// context_size is the value the module's own sl::initialize_module size-checks at +8.
			// The Lost-Judgment and Like-a-Dragon generations use 0xF000 (the default, so their
			// callers are unchanged); Yakuza Kiwami 2's is 0xF3C0 — the SAME struct layout, just
			// grown at the tail, verified field-by-field against a live host sl context.
			int initialize(uint32_t context_size = 0xF000);

			struct HevtRec {
				uint32_t tag;   // "HEVT" = 0x54564548
				uint32_t pad;   // unused/alignment
				HANDLE   h;     // OS event handle (at +8)
			};

			void heap_free(void* p);

		};

		// Host-side sl-context setup: handle/file-handle pools, file + archive access objects and
		// the async request workers. Generic across every m2ftg host (LJ StF/FV and YLAD VF2 call
		// it right after resolving/initializing the DLL's sl context). Defined in sl.cpp.
		void PatchSl(sl::context_t* context);
	}

