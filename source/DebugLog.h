#pragma once

// Unified debug logging. All of YAMP's diagnostic output goes through these instead of ad-hoc
// sprintf_s + OutputDebugStringA pairs:
//  - DebugLog(fmt, ...)      printf-style, to the debugger (OutputDebugStringA)
//  - DebugLogFile(fmt, ...)  same, but ALSO appended (and flushed per line) to d3d12_debug.log
//                            in the CWD — for output that must survive a process death with no
//                            debugger attached (D3D12 validation messages, DRED dumps)
//  - DebugLogV(fmt, va_list) forwarding entry for local variadic wrappers (e.g. AtomEngine's Log)
// Debug builds only: in Release/Master the macros compile away entirely (arguments are not
// evaluated) and DebugLogV is an empty inline.

#ifdef _DEBUG

#include <cstdarg>

void DebugLogImpl(const char* fmt, ...);
void DebugLogFileImpl(const char* fmt, ...);
void DebugLogV(const char* fmt, va_list args);

#define DebugLog(...) DebugLogImpl(__VA_ARGS__)
#define DebugLogFile(...) DebugLogFileImpl(__VA_ARGS__)

#else

inline void DebugLogV(const char*, ...) {}
#define DebugLog(...) ((void)0)
#define DebugLogFile(...) ((void)0)

#endif
