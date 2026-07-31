#pragma once

// Unified debug logging. All of YAMP's diagnostic output goes through these instead of ad-hoc
// sprintf_s + OutputDebugStringA pairs:
//  - DebugLog(fmt, ...)      printf-style, to the debugger (OutputDebugStringA)
//  - DebugLogFile(fmt, ...)  same, but ALSO appended (and flushed per line) to d3d12_debug.log
//                            in the CWD — for output that must survive a process death with no
//                            debugger attached (D3D12 validation messages, DRED dumps)
//  - DebugLogV(fmt, va_list) forwarding entry for local variadic wrappers (e.g. AtomEngine's Log)
// Debug builds only: in Release/Master the macros compile away entirely (arguments are not
// evaluated), DebugLogV is an empty inline, and no log file is opened or even named — the
// filenames do not survive into the binary.
//
// YAMP_DEBUG_LOGGING is the single gate for every diagnostic file YAMP writes; RenderWindow.cpp's
// pso_stream.log / heaps.log dumps use it too. It tests BOTH defines on purpose:
//   DEBUG  is premake's own (`filter "configurations:Debug"` in premake5.lua) — the authoritative
//          one, since it tracks the configuration rather than a compiler switch;
//   _DEBUG comes from the MSVC debug CRT (staticruntime "on" + runtime "Debug").
// Keying off _DEBUG alone would silently disable ALL logging in Debug builds if the Debug config
// were ever switched to the release runtime, which is a common speed tweak.
#if defined(DEBUG) || defined(_DEBUG)
#define YAMP_DEBUG_LOGGING 1
#else
#define YAMP_DEBUG_LOGGING 0
#endif

#if YAMP_DEBUG_LOGGING

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
