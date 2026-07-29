#include "sl.h"

#include "file_access.h"
#include "async_request.h"
#include "M2Input.h"
#include "../YAMPGeneral.h"
#include "../wil/resource.h"
#include <Windows.h>
#include <mmsystem.h>   // timeBeginPeriod
#pragma comment(lib, "winmm.lib")

#include <algorithm>

#pragma comment(lib, "xinput.lib")

namespace m2ftg
{

        namespace sl {

            context_t* sm_context;
            handle_t* (*handle_create_internal)(handle_t* obj, void* ptr, uint32_t type);
            void (*archive_lock_wlock)(uint32_t* lock);
            void (*archive_lock_wunlock)(uint32_t* lock);
            void* (*kernel_calloc_internal)(uint64_t bytes, uint32_t flags_or_zero);

            extern void (*spinlock_lock_internal)(spinlock_t*);
            extern void (*spinlock_unlock_internal)(spinlock_t*);

            // string table of initial tag names (e.g., "Unknown", "Thread", ...).
            extern const char* const g_tag_names[]; // = PTR_s_Unknown_143458080 in target

            static const uint8_t kDefaultTag128_Fallback[16] = {
                0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
                0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
            };

            const char* const g_tag_names[14] = {
                "Unknown", "Thread", "Semaphore",
                "Event", "RwLock", "File", "Archive",
                "FindFile", "ArchiveDirectory", "Module",
                "Notify", "CtNode", "Buffer", "Texlib" // 14th
            };


            // 128-bit default tag “GUID” pattern copied into the remaining slots:
            const uint8_t* gDefaultTag128Ptr = kDefaultTag128_Fallback;

            inline void write_tag16_cstr(uint8_t* dst, const char* s) {
                // cap at 15 to leave room for the NUL terminator
                const size_t n = std::min<size_t>(15, std::strlen(s));
                std::memcpy(dst, s, n);
                // pad the rest with zeros (guarantees dst[15] == 0)
                std::memset(dst + n, 0, 16 - n);
            }

            inline long long initialize_timing()  // HRESULT-style: 0 on success, 0x80004005 on fail
            {
                LARGE_INTEGER qpf{};
                if (QueryPerformanceFrequency(&qpf)) {
                    const uint64_t freq = static_cast<uint64_t>(qpf.QuadPart);
                    sm_context->count_frequency = freq;

                    const double f = static_cast<double>(static_cast<unsigned long long>(qpf.QuadPart));
                    sm_context->count_per_milli_second = 1000.0 / f;   // seconds→ms scale per tick
                    sm_context->count_per_micro_second = 1'000'000.0 / f;
                    sm_context->count_per_nano_second = 1'000'000'000.0 / f;
                    sm_context->count_per_tick = 3000.0 / f;    // 3 ms per tick (used by the game)

                    timeBeginPeriod(1);  // request 1 ms timer resolution
                    return 0;            // S_OK
                }
                return 0x80004005;       // E_FAIL
            }

            static int scan_bytes_00_01_FF(const uint8_t* buf, size_t len) {
                if (!buf || len == 0) return -1;

                // 0x0000FF01 repeated → bytes {01,FF,00,00} across the lane
                const __m128i needles = _mm_set1_epi32(0x0000FF01);

                const uint8_t* p = buf;
                const uint8_t* end = buf + len;

                // process full 16B chunks
                while (size_t(end - p) >= 16) {
                    __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
                    // imm=0x14: equal-any, unsigned bytes
                    int idx = _mm_cmpistri(needles, block, 0x14);
                    // if any match in block, cmpistrz (ZF==0) → we found it
                    if (!_mm_cmpistrz(needles, block, 0x14)) {
                        return int((p - buf) + idx);
                    }
                    p += 16;
                }

                // tail (≤15 bytes) — do a scalar check (or a masked load if you prefer)
                for (; p < end; ++p) {
                    if (*p == 0x00 || *p == 0x01 || *p == 0xFF) return int(p - buf);
                }
                return -1;
            }

            void* __fastcall heap_calloc(size_t size, unsigned long long align)
            {
                // choose at least 16-byte alignment (matches the pattern in your decomp)
                size_t a = (align < 16) ? 16 : static_cast<size_t>(align);

                auto* self = sm_context ? sm_context->p_csl_allocator : nullptr;
                if (!self || !self->alloc) return nullptr;

                void* p = self->alloc(self, size, a);
                if (p) {
                    std::memset(p, 0, size);
                }
                return p;
            }

            void heap_free(void* p) {
                using free_fn = void (*)(void*);
                if (!p || !sm_context->p_pxd_std_free) return;
                auto fn = reinterpret_cast<free_fn>(sm_context->p_pxd_std_free);
                fn(p);
            }

            // Returns a1 for parity with the decomp; writes the numeric id to *a1 (0 on failure).
            int* __fastcall event_create(int* a1, unsigned char initial)
            {
                *a1 = 0;

                // 16-byte record: "HEVT" tag + OS handle
                struct HevtRec { uint32_t tag; uint32_t pad{}; HANDLE h; };

                auto* rec = static_cast<HevtRec*>(heap_calloc(sizeof(HevtRec), sizeof(HevtRec)));
                if (!rec) return a1;

                HANDLE h = CreateEventA(nullptr, TRUE, initial != 0, nullptr); // or your imported pointer
                if (!h || h == INVALID_HANDLE_VALUE) {
                    heap_free(rec);
                    return a1;
                }

                rec->tag = 0x54564548u; // "HEVT"
                rec->h = h;

                // Your project's signature: handle_create returns handle_t
                handle_t hand = handle_create(/*record*/rec, /*type*/3u);
                int id = static_cast<int>(hand.h.m_handle);
                *a1 = id;                       // <-- write back to the out param

                if (id == 0) {                  // registration failed: clean up
                    CloseHandle(h);
                    heap_free(rec);
                }
                return a1;
            }

            int file_system_initialize(uint32_t param1, uint64_t param2)
            {
                if (reinterpret_cast<int*>(sm_context->is_utf8_file_path) == 0) {
                    auto* path_buf = reinterpret_cast<char*>(sm_context->sz_fs_root); // char[0x410] lives here
                    DWORD len = GetCurrentDirectoryA(0x410, path_buf); // returns length, excl NUL
                    if (len > 0 && len < 0x410) {
                        sm_context->is_utf8_file_path = 1;
                    }
                }
                else
                {
                    // TO DO: Convert utf16_to_utf8
                    auto* path_wbuf = reinterpret_cast<wchar_t*>(sm_context->sz_fs_root); // char[0x410] lives here
                    DWORD len = GetCurrentDirectoryW(0x104, path_wbuf); // returns length, excl NUL
                    if (len > 0 && len < 0x410) {
                        sm_context->is_utf8_file_path = 1;
                    }
                }

                // Example if you’re scanning a fixed buffer in sm_context:
                // say sm_context->sz_fs_root starts at sm_context+0x298
                auto* base = reinterpret_cast<const uint8_t*>(sm_context->sz_fs_root);
                int pos = scan_bytes_00_01_FF(base, 0x410);  // bounded to 0x410 bytes

                auto* str_base = reinterpret_cast<const char*>(sm_context->sz_fs_root);
                uint32_t len = static_cast<uint32_t>(strnlen_s(str_base, 0x410)); // returns <= 0x410

                sm_context->fs_root_len = len;

                auto* idk = kernel_calloc((static_cast<uint64_t>(param1) * 4) + 32 * ((static_cast<uint64_t>(param1) * 0x4d0)+2048), 0);

                // Opaque dispatch object
                struct libc_alloc_dispatch { void** table; };

                // Function signature (x64): size_t is 64-bit
                using libc_alloc_fn = void* (*)(libc_alloc_dispatch* self, size_t size, size_t align);

                // Get the function from slot 1 (+8) and call it
                auto* self = reinterpret_cast<libc_alloc_dispatch*>(sm_context->__p_csl_allocator);
                auto** tab = *reinterpret_cast<void***>(self);
                auto  c_alloc = reinterpret_cast<libc_alloc_fn>(tab[1]);

                sm_context->allocated_heap = c_alloc(self, (static_cast<size_t>(param1) + 1) << 3, 8);
				sm_context->heap_size = param1;

                if(idk == 0) {
                    sm_context->allocated_heap = reinterpret_cast<void*>(0x80004005); // E_FAIL
				}
                auto* tbl = static_cast<file_handle_internal_t*>(sm_context->p_file_handle_tbl);
                //tbl[0].m_async_event.h.m_handle = static_cast<uint32_t>(idk);

                // pool lives in sm_context->file_handle_pool and is a t_fixed_deque<file_handle_internal_t*>
                using FH = file_handle_internal_t;

                // Ensure the pool is reserved once somewhere before use:
                sm_context->file_handle_pool.reserve(sm_context->file_handle_max);

                FH* pfVar17 = sm_context->p_file_handle_tbl;                                    // current entry
                unsigned remaining = sm_context->file_handle_max;

                while (remaining--) {
                    // event_create(1) → numeric handle id at +0x468
                    int id = 0;
                    event_create(&id, /*initial*/1);
                    pfVar17->m_async_event.h.m_handle = static_cast<uint32_t>(id);

                    // overflow check (matches the assert/printf/break in disasm)
                    //assert(sm_context->file_handle_pool_size() < sm_context->file_handle_pool_capacity());

                    // enqueue pointer to this entry
                    sm_context->file_handle_pool.push_back(&pfVar17);

                    // next table entry
                    ++pfVar17;                                         // if contiguous array of FH
                    // If it’s a raw byte blob with 0x4D0 stride, use:
                    // pfVar17 = reinterpret_cast<FH*>(reinterpret_cast<uint8_t*>(pfVar17) + 0x4D0);
                }


                return 0;

            }

            int initialize()
            {
                // sl::initialize_module in the DLL (0x18006d2e0) validates the context
                // header: it requires size_of_struct (+8) == 0xF000, silently leaving
                // sm_context unset otherwise.
                sm_context->size_of_struct = 0xF000;
                sm_context->export_context.size_of_struct = 0x10;
                sm_context->export_context.p_context = &sm_context;
                sm_context->processor_num = 0x10;
                sm_context->main_thread_id = GetCurrentThreadId();
                sm_context->processor_affinity_mask = 0x0FFFF;
                sm_context->temp_work_size = 0x80000;
                sm_context->p_temp_work = kernel_calloc(sm_context->temp_work_size, 0);
                initialize_timing();
                // TODO: file_system_initialize is a WIP reconstruction: it never assigns
                // p_file_handle_tbl (the "idk" allocation), so its handle loop derefs null.
                // PatchSl builds the file handle pool the proven VF5FS way instead for now.
                //file_system_initialize(0x800, 0x800000);

                return 0;
            }

            int handle_initialize(uint32_t max_handles)
            {
                // 1) Allocate the per-handle buffer and stash it.
                auto* buf = static_cast<handle_internal_buffer_t*>(
                    kernel_calloc(static_cast<size_t>(max_handles) * 8, 0)); // matches ref: count*8
                sm_context->handles.p_handle_buffer = buf;
                if (sm_context->handles.p_handle_buffer == nullptr) {
                    // matches ref’s -0x7fffbffb (0x80004005)
                    return 0x80004005; // E_FAIL
                }

                sm_context->handle_max = max_handles;


                constexpr unsigned kFileHandlePool = 0x800;
                sm_context->file_handle_max = kFileHandlePool;
                sm_context->file_handle_pool.reserve(kFileHandlePool);

                // 2) Populate the free-handle queue with nodes backed by the buffer.
                auto* ctx = sm_context;

                // The buffer is an array of embedded nodes laid out every 8 bytes.
                using Node = t_locked_queue_node<handle_internal_buffer_t>;
                auto* q = &ctx->handle_free_queue;
                q->reset_bootstrap();

                auto* node = reinterpret_cast<Node*>(ctx->handles.p_handle_buffer);
                static_assert(sizeof(Node) == 8, "node must be 8 bytes");

                for (uint32_t i = 0; i < max_handles; ++i) {
                    // owner record starts at m_node (offset 0), so rec == node
                    auto* rec = reinterpret_cast<handle_internal_buffer_t*>(node);
                    q->append_unlocked(rec);
                    node = reinterpret_cast<Node*>(
                        reinterpret_cast<std::byte*>(node) + sizeof(Node)); // +8
                }

                // 3) Initialize the 256 tag_id entries (each 16 bytes) in sm_context.
                // First: copy explicit names from g_tag_names into tag_id[0 .. N-1].
                // Target copies 14 names (0xE) at offset +0x800: ref copies 13 (0xD) at +0x800.
                // Both finish with total 256 entries by filling the remainder with the default 128-bit value.
                uint8_t* out = reinterpret_cast<uint8_t*>(sm_context) + 0x800;
                for (const char* s : g_tag_names) {
                    write_tag16_cstr(out, s);
                    out += 16;
                }

                // 4) Fill the remaining entries with the default 128-bit pattern.
                // Target starts at +0x8e0 and writes 0xF2 entries (242),
                // which with 14 named entries yields 256 total. (Ref uses +0x8d0, 0xF3 entries.)

                uint8_t* tail = reinterpret_cast<uint8_t*>(sm_context) + 0x8e0;
                for (int i = 0; i < 0xF2; ++i) {        // 242 entries
                    std::memcpy(tail, gDefaultTag128Ptr, 16);
                    tail += 0x10;
                }

                return 0;
            }

            void mutex_construct(mutex_t& mutex)
            {
                InitializeCriticalSectionAndSpinCount(&mutex.m_cs, 4096);
            }

            void mutex_destruct(mutex_t& mutex)
            {
                DeleteCriticalSection(&mutex.m_cs);
            }

            void spinlock_lock(spinlock_t& spinlock)
            {
                uint32_t itersToPause = 4;
                while (true)
                {
                    uint32_t iters = 0;
                    while (spinlock.m_lock_status != 0)
                    {
                        if (++iters > 0x10000)
                        {
                            iters = 0;
                            SleepEx(0, TRUE);
                        }
                        _mm_pause();
                    }
                    if (InterlockedCompareExchange(&spinlock.m_lock_status, 1, 0) == 0)
                    {
                        break;
                    }
                    for (uint32_t pause = 0; pause != itersToPause; pause++)
                    {
                        _mm_pause();
                    }
                    itersToPause = std::max(itersToPause * 2, 512u);

                }
                _mm_mfence();
            }

            void spinlock_unlock(spinlock_t& spinlock)
            {
                _mm_mfence();
                spinlock.m_lock_status = 0;
            }

            void rwspinlock_wlock(rwspinlock_t& spinlock)
            {
                uint32_t itersToPause = 4;
                while (true)
                {
                    uint32_t iters = 0;
                    while (spinlock.m_lock_status != 0)
                    {
                        if (++iters > 0x10000)
                        {
                            iters = 0;
                            SleepEx(0, TRUE);
                        }
                        _mm_pause();
                    }
                    if (InterlockedCompareExchange(&spinlock.m_lock_status, 0x80000000, 0) == 0)
                    {
                        break;
                    }
                    for (uint32_t pause = 0; pause != itersToPause; pause++)
                    {
                        _mm_pause();
                    }
                    itersToPause = std::max(itersToPause * 2, 512u);

                }
                _mm_mfence();
            }

            void rwspinlock_wunlock(rwspinlock_t& spinlock)
            {
                // TODO: This might be wrong
                _mm_mfence();
                spinlock.m_lock_status = 0;
            }

            void rwspinlock_rlock(rwspinlock_t& spinlock)
            {
                uint32_t itersToPause = 4;
                while (true)
                {
                    int32_t curStatus = spinlock.m_lock_status;
                    uint32_t iters = 0;
                    while ((spinlock.m_lock_status & 0x800000) != 0)
                    {
                        if (++iters > 0x10000)
                        {
                            iters = 0;
                            SleepEx(0, TRUE);
                        }
                        _mm_pause();
                        curStatus = spinlock.m_lock_status;
                    }
                    if (InterlockedCompareExchange(&spinlock.m_lock_status, curStatus + 1, curStatus) == curStatus)
                    {
                        break;
                    }
                    for (uint32_t pause = 0; pause != itersToPause; pause++)
                    {
                        _mm_pause();
                    }
                    itersToPause = std::max(itersToPause * 2, 512u);

                }
                _mm_mfence();
            }

            void rwspinlock_runlock(rwspinlock_t& spinlock)
            {
                InterlockedExchangeAdd(&spinlock.m_lock_status, 0xFFFFFFFF);
            }


            handle_t semaphore_create(uint32_t initialCount)
            {
                handle_t result;
                HANDLE semaphore = CreateSemaphoreW(nullptr, initialCount, LONG_MAX, nullptr);
                if (semaphore != INVALID_HANDLE_VALUE)
                {
                    semaphore_internal_t* sema = new semaphore_internal_t;
                    sema->h_semaphore = semaphore;
                    result = handle_create(sema, 2);
                }
                return result;
            }

            namespace {

                const DWORD MS_VC_EXCEPTION = 0x406D1388;
#pragma pack(push,8)
                typedef struct tagTHREADNAME_INFO
                {
                    DWORD dwType; // Must be 0x1000.
                    LPCSTR szName; // Pointer to name (in user addr space).
                    DWORD dwThreadID; // Thread ID (-1=caller thread).
                    DWORD dwFlags; // Reserved for future use, must be zero.
                } THREADNAME_INFO;
#pragma pack(pop)
                void SetThreadName(DWORD dwThreadID, const char* threadName) {
                    THREADNAME_INFO info;
                    info.dwType = 0x1000;
                    info.szName = threadName;
                    info.dwThreadID = dwThreadID;
                    info.dwFlags = 0;
#pragma warning(push)
#pragma warning(disable: 6320 6322)
                    __try {
                        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
#pragma warning(pop)
                }
            }

            static DWORD WINAPI stub_thread_start_routine(LPVOID arg)
            {
                // TODO: The original game code is full of XMM stores here
                // Saving XMM state somehow?
                thread_internal_t* thread = reinterpret_cast<thread_internal_t*>(arg);
                SetThreadName(thread->thread_id, thread->sz_name);

                // TODO: thread_sid_register
                DWORD result = thread->p_routine(thread->arg);
                // TODO: thread_sid_unregister

                return result;
            }

            // TODO: Originally this function had more parameters
            handle_t thread_create(uint32_t(*p_routine)(uint64_t), uint64_t arg, const char* name)
            {
                thread_internal_t* thread = new thread_internal_t;
                thread->p_routine = p_routine;
                thread->arg = arg;
                strcpy_s(thread->sz_name, name);
                handle_t result = handle_create(thread, 1);

                DWORD threadId;
                thread->h_thread = CreateThread(nullptr, 0x10000, stub_thread_start_routine, thread, CREATE_SUSPENDED, &threadId);
                if (thread->h_thread != nullptr)
                {
                    thread->thread_id = threadId;
                    // TODO: Affinity and priority
                    ResumeThread(thread->h_thread);
                }
                return result;
            }

            handle_t file_open(const char* in_sz_file_path)
            {
                handle_t result{};
                file_open_internal(&result, in_sz_file_path);
                return result;
            }

            handle_t file_create(const char* in_sz_file_path)
            {
                handle_t result{};
                file_create_internal(&result, in_sz_file_path);
                return result;
            }

            int64_t file_write(handle_t h_file, const void* p_buffer, unsigned int write_size)
            {
                int64_t result = -1;
                file_handle_internal_t* file = file_handle_instance(h_file);
                if (file == nullptr) return result;

                if ((file->m_flags >> 7) & 1)
                {
                    result = sm_context->p_file_access->write(h_file, p_buffer, write_size);
                }
                return result;
            }

            handle_t handle_create(void* ptr, uint32_t type)
            {
                handle_t result{};
                handle_create_internal(&result, ptr, type);
                return result;
            }

            void archive_lock::_afterConstruct()
            {
                sl::file_handle_event* event1 = new sl::file_handle_event{};
                sl::file_handle_event* event2 = new sl::file_handle_event{};
                event1->_afterConstruct();
                event2->_afterConstruct();

                eventHandle1 = sl::handle_create(event1, 3);
                eventHandle2 = sl::handle_create(event2, 3);
            }

            void file_handle_event::_afterConstruct()
            {
                eventHandle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            }

            void file_handle_internal_t::begin_async_request()
            {
                file_handle_event* event = handle_instance<file_handle_event>(m_async_event, 3);
                ResetEvent(event->eventHandle);
                m_flags |= 4;
            }

            void file_handle_internal_t::end_async_request()
            {
                m_req_item_index = 0;
                m_flags &= 0xFFFFFFFB;

                file_handle_event* event = handle_instance<file_handle_event>(m_async_event, 3);
                SetEvent(event->eventHandle);
            }

            void file_handle_internal_t::callback(FILE_ASYNC_METHOD type, uint32_t status)
            {
                auto func = mp_callback_func;
                auto handle = m_handle;
                auto param = mp_callback_param;
                if (m_callback_method == type)
                {
                    if (func != nullptr)
                    {
                        sl::rwspinlock_rlock(m_locked);
                        uint32_t flags = m_flags >> 28;
                        sl::rwspinlock_runlock(m_locked);
                        while (flags & 1)
                        {
                            SleepEx(0, TRUE);
                            sl::rwspinlock_rlock(m_locked);
                            flags = m_flags >> 28;
                            sl::rwspinlock_runlock(m_locked);
                        }
                    }
                }
                else
                {
                    func = nullptr;
                }

                sl::rwspinlock_wlock(m_locked);
                m_last_async_status = status;
                if ((m_flags & (1 << 0x1A)) != 0)
                {
                    func = nullptr;
                    m_flags &= 0xFBFFFFFF;
                }
                else if (func != nullptr)
                {
                    m_flags |= 0x50000000;
                    if (type >= FILE_ASYNC_METHOD_PRELOAD)
                        m_callback_execute_thread = GetCurrentThreadId();
                }
                end_async_request();
                sl::rwspinlock_wunlock(m_locked);
                if (func != nullptr)
                {
                    if (type == FILE_ASYNC_METHOD_READ || type == FILE_ASYNC_METHOD_WRITE)
                    {
                        assert(!"t_lockfree_stack unimplemented!");
                    }
                    else
                    {
                        func(m_handle, status, mp_callback_param);
                        sl::rwspinlock_wlock(m_locked);
                        m_callback_execute_thread = 0;
                        const unsigned int destroyFlag = m_flags >> 27;
                        m_flags &= 0x83FFFFFF;
                        sl::rwspinlock_wunlock(m_locked);
                        if (destroyFlag & 1)
                            sl::file_handle_destroy(this);
                    }
                }
            }

            void file_handle_internal_t::_afterConstruct()
            {
                sl::file_handle_event* event = new sl::file_handle_event{};
                event->_afterConstruct();

                m_async_event = sl::handle_create(event, 3);
            }

            sl::mutex_t::mutex_t()
            {
                mutex_construct(*this);
            }

            sl::mutex_t::~mutex_t()
            {
                mutex_destruct(*this);
            }

            void mutex_t::lock()
            {
                EnterCriticalSection(&m_cs);
            }

            void mutex_t::unlock()
            {
                LeaveCriticalSection(&m_cs);
            }

        }

        // Binding-driven pad state: each player's actions (M2Input, set up on the YAMP
        // Controls page) are routed to the sl button bits whose meaning is fixed by the
        // assign table StF.cpp hands the module (M2Input::MODULE_ASSIGN): A=P, B=K, Y=G,
        // X=P+G, LT=P+K+G, LB=P+K, RT=K+G. Escape stays reserved for the host pause menu
        // (StF.cpp GameLoop), matching Lost Judgment. StF.cpp calls M2Input::PollPads()
        // once per frame before these.
        void csl_pad::set_state(unsigned int index)
        {
            m_prev = std::exchange(m_now, 0);
            m_x1 = m_y1 = 0.0f;

            const unsigned int player = index & 1;
            auto setButton = [this](sl::BUTTON button) {
                m_now |= (1u << button);
                m_buttons[button] = 0xFF;
                };
            auto actionDown = [player](M2Input::Action action) {
                return M2Input::ActionDown(player, action);
                };

            if (actionDown(M2Input::Action_Punch)) setButton(sl::BUTTON_A);
            if (actionDown(M2Input::Action_Kick)) setButton(sl::BUTTON_B);
            if (actionDown(M2Input::Action_Guard)) setButton(sl::BUTTON_Y);
            if (actionDown(M2Input::Action_PG)) setButton(sl::BUTTON_X);
            if (actionDown(M2Input::Action_PKG)) setButton(sl::BUTTON_LT);
            if (actionDown(M2Input::Action_PK)) setButton(sl::BUTTON_LB);
            if (actionDown(M2Input::Action_KG)) setButton(sl::BUTTON_RT);
            if (actionDown(M2Input::Action_Start)) setButton(sl::BUTTON_START);
            if (actionDown(M2Input::Action_Back)) setButton(sl::BUTTON_BACK);
            // Action_Coin is not a pad button - StF.cpp turns it into the coin status bit.

            // Movement: bound digital inputs get the dpad bits + full analog deflection;
            // the player's left stick always steers as well (the cabinet lever).
            const bool up = actionDown(M2Input::Action_Up);
            const bool down = actionDown(M2Input::Action_Down);
            const bool left = actionDown(M2Input::Action_Left);
            const bool right = actionDown(M2Input::Action_Right);
            if (left && !right)
            {
                setButton(sl::BUTTON_LEFT);
                m_x1 = -1.0f;
            }
            else if (right && !left)
            {
                setButton(sl::BUTTON_RIGHT);
                m_x1 = 1.0f;
            }
            if (up && !down)
            {
                setButton(sl::BUTTON_UP);
                m_y1 = -1.0f;
            }
            else if (down && !up)
            {
                setButton(sl::BUTTON_DOWN);
                m_y1 = 1.0f;
            }

            const M2Input::PadState& pad = M2Input::GetPadState(gGeneral.GetSettings()->m_stfPadIndex[player]);
            if (m_x1 == 0.0f)
            {
                m_x1 = pad.x;
            }
            if (m_y1 == 0.0f)
            {
                m_y1 = pad.y;
            }

            m_push = ~m_prev & m_now;
            m_pull = m_prev & ~m_now;
        }

        // Host-side sl-context setup, shared by every m2ftg host (moved from LJ/Patch.cpp —
        // it is generic bring-up, not an LJ patch; the YLAD VF2 host calls it too).
        void PatchSl(sl::context_t* context)
        {
            // Populate handle_free_queue, unless sl::handle_initialize() already did
            static constexpr size_t NUM_HANDLES = 1000;

            if (context->handles.p_handle_buffer == nullptr)
            {
                context->handle_max = NUM_HANDLES;
                context->handles.p_handle_buffer = new sl::handle_internal_buffer_t[NUM_HANDLES];

                for (auto& handle : wil::make_range(context->handles.p_handle_buffer, NUM_HANDLES))
                {
                    context->handle_free_queue.enqueue(&handle);
                }
            }

            // sync_archive_condvar is a recursive spinlock WORD that csl_archive::create_instance
            // passes BY ADDRESS to archive_lock_wlock (= the DLL's recursive_rwspinlock_wlock, which
            // reads *ptr: high 16 = owner thread, low 16 = recursion). It is NOT a handle — the old
            // handle_create() stored a handle value (e.g. 0x100002) that the lock then dereferenced
            // as a pointer -> AV on the first real archive read. Start it unlocked (0).
            context->sync_archive_condvar = 0;

            // Set up file access
            context->p_file_access = new csl_file_access;
            context->p_archive_access = new csl_file_access_archive;

            // Set up file_handle_pool
            // NOTE: the pool was already reserved (0x800) by sl::handle_initialize();
            // reserve() asserts on double use, so only populate entries here.
            static constexpr uint32_t NUM_FILE_HANDLES = 250;

            // TODO: Validate this
            sl::file_handle_internal_t* handles = new sl::file_handle_internal_t[NUM_FILE_HANDLES]{};

            for (auto& handle : wil::make_range(handles, NUM_FILE_HANDLES))
            {
                handle._afterConstruct();
                // TODO: Especially this
                const auto handlePtr = &handle;
                context->file_handle_pool.push_back(&handlePtr);
            }

            // Set up async file requests
            static constexpr uint32_t NUM_REQUESTS = NUM_FILE_HANDLES + 64;

            context->p_file_async_request = new csl_file_async_request(&context->p_file_access, NUM_REQUESTS);
            context->p_archive_async_request = new csl_file_async_request(reinterpret_cast<isl_file_access**>(&context->p_archive_access), NUM_REQUESTS);
        }
    }

