# D3D12MemoryAllocator (vendored)

AMD's D3D12 Memory Allocator, **v2.0.1 (2022-04-05)**, MIT — verbatim upstream except for one
local patch.

The version pin is LOAD-BEARING: the arcade module DLLs ship with their own embedded build of
this same allocator version, and YAMP constructs a `D3D12MA::Allocator` whose `AllocatorPimpl`
layout must match the DLL's byte-for-byte (the DLL reads fields like `m_Device` at `+0x80` and
`m_BlockVectors` at `+0x350` out of the object YAMP built). Do not upgrade without re-verifying
those offsets against the module.

## Local patch (the only one)

`D3D12MemAlloc.cpp`, the block ending in:

```cpp
#include "../../pxd/LJ/DllMutex.h"
#define D3D12MA_MUTEX StfDllMutex
```

The DLL locks the allocator's `std::mutex` members with its baked-in VS2019 `_Mtx_unlock`, so
the mutex must be VS2019-ABI-shaped (0x50 bytes over an SRWLOCK) — `StfDllMutex` in
`source/pxd/LJ/DllMutex.h` (first-party) reproduces that layout. `D3D12MA_RW_MUTEX` is
deliberately left stock; the rationale is in the comment at the patch site.
