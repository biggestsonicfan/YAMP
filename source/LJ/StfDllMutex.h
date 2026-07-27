#pragma once
//
// StfDllMutex.h — a std::mutex (VS2019 ABI) look-alike for the StF DLL's embedded D3D12MA.
//
// The StF DLL (stf-pxd-w64-d3d12_retail.dll) statically links the VS2019 STL and its embedded
// copy of D3D12MA locks std::mutex objects that live INSIDE the D3D12MA::Allocator we build and
// hand it at cdevice+0x17B8. It locks/unlocks them with its own baked-in mtx_do_lock /
// _Mtx_unlock, which expect the classic MSVC _Mtx_internal_imp_t layout:
//
//   +0x00  int   _Type                     (std::mutex == _Mtx_try == 2)
//   +0x08  <polymorphic _Stl_critical_section> : its first qword is a vtable pointer
//   +0x10  SRWLOCK  (the critical section's actual lock, at cs+0x08)
//   +0x48  long  _Thread_id                 (owner tid, -1 = none)
//   +0x4C  int   _Count                     (recursion count)
//
// Verified in the DLL disassembly:
//   mtx_do_lock  @0x1800C7304 : calls cs->vtable[0]  (lock)      and cs->vtable[0x10] (try_lock_for)
//   _Mtx_unlock  @0x1800C7478 : calls cs->vtable[0x18] (unlock)
//   the icall goes through 0x1800D02D0 -> a bare `jmp rax` (no CFG), so our callbacks are fine.
//
// Modern VS2022 (17.10+) replaced that polymorphic critical section with an inline SRWLOCK, so a
// std::mutex WE construct stores a null where the DLL expects the vtable pointer and the DLL
// crashes dereferencing it (stfDLL+0xC7378). This type reproduces the VS2019 layout with our own
// 5-slot critical-section vtable backed by an SRWLOCK, so the DLL's built-in lock code drives our
// SRWLOCK directly. Our own locking (Lock/Unlock/LockRead/...) uses the same SRWLOCK, which is the
// real mutual-exclusion gate; the _Type/_Thread_id/_Count fields are bookkeeping the DLL keeps for
// itself. No external dependency — the "lock" is just an SRWLOCK.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstddef>
#include <cstring>

class StfDllMutex
{
public:
	StfDllMutex()
	{
		std::memset(this, 0, sizeof(StfDllMutex));
		_Type   = 2;                                   // std::mutex: _Flags(0) | _Mtx_try(2)
		_CsVptr = const_cast<void*>(static_cast<const void*>(s_csVtable)); // cs vtable pointer @ +0x08
		InitializeSRWLock(&_Srw);
		_ThreadId = -1;                                // no owner
		_Count    = 0;
	}

	StfDllMutex(const StfDllMutex&) = delete;
	StfDllMutex& operator=(const StfDllMutex&) = delete;

	// D3D12MA_MUTEX interface.
	void Lock()   { AcquireSRWLockExclusive(&_Srw); }
	void Unlock() { ReleaseSRWLockExclusive(&_Srw); }

	// D3D12MA_RW_MUTEX interface. The DLL's build maps this to the same std::mutex (its lock body
	// tracks a single owner tid), so read == write == exclusive here to keep the layouts identical.
	void LockRead()    { AcquireSRWLockExclusive(&_Srw); }
	void UnlockRead()  { ReleaseSRWLockExclusive(&_Srw); }
	void LockWrite()   { AcquireSRWLockExclusive(&_Srw); }
	void UnlockWrite() { ReleaseSRWLockExclusive(&_Srw); }

	// --- _Mtx_internal_imp_t (VS2019 ABI). The DLL operates directly on these bytes. Public
	//     only so the layout static_asserts below (namespace scope) can offsetof() them. ---
	int     _Type;                 // +0x00
	void*   _CsVptr;               // +0x08  first qword of the inline _Stl_critical_section (its vtable)
	SRWLOCK _Srw;                  // +0x10  _Stl_critical_section::_M_srw_lock (cs + 0x08)
	char    _CsPad[0x48 - 0x18];   // +0x18  rest of the 0x40-byte critical-section storage
	long    _ThreadId;             // +0x48
	int     _Count;                // +0x4C

private:
	// The critical-section object the DLL sees at +0x08 is `this + 8`; its SRWLOCK is at +0x08 of it.
	static SRWLOCK* Srw(void* cs) { return reinterpret_cast<SRWLOCK*>(reinterpret_cast<char*>(cs) + 8); }

	static void          CsLock(void* cs)                       { AcquireSRWLockExclusive(Srw(cs)); }
	static unsigned char CsTryLock(void* cs)                    { return static_cast<unsigned char>(TryAcquireSRWLockExclusive(Srw(cs))); }
	static unsigned char CsTryLockFor(void* cs, unsigned int)   { return static_cast<unsigned char>(TryAcquireSRWLockExclusive(Srw(cs))); }
	static void          CsUnlock(void* cs)                     { ReleaseSRWLockExclusive(Srw(cs)); }
	static void          CsDestroy(void* /*cs*/)                {}

	static void* const s_csVtable[5];
};

// 5-slot _Stl_critical_section vtable: lock / try_lock / try_lock_for / unlock / destroy.
inline void* const StfDllMutex::s_csVtable[5] = {
	reinterpret_cast<void*>(&StfDllMutex::CsLock),
	reinterpret_cast<void*>(&StfDllMutex::CsTryLock),
	reinterpret_cast<void*>(&StfDllMutex::CsTryLockFor),
	reinterpret_cast<void*>(&StfDllMutex::CsUnlock),
	reinterpret_cast<void*>(&StfDllMutex::CsDestroy),
};

static_assert(offsetof(StfDllMutex, _CsVptr) == 0x08, "cs vtable ptr must be at +0x08");
static_assert(offsetof(StfDllMutex, _ThreadId) == 0x48, "_Thread_id must be at +0x48");
static_assert(offsetof(StfDllMutex, _Count) == 0x4C, "_Count must be at +0x4C");
static_assert(sizeof(StfDllMutex) == 0x50, "_Mtx_internal_imp_t is 0x50 bytes on x64");
