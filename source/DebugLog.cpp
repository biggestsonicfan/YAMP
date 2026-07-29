#include "DebugLog.h"

#ifdef _DEBUG

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>

namespace
{
	// Shared d3d12_debug.log handle so the D3D12 validation callback AND the DRED dump land in
	// one file (DRED matters most when the process is dying — route it to disk, not just the
	// debugger). Opened "w" by whoever writes first, then kept open and flushed per line so the
	// message emitted right before a driver crash is on disk.
	FILE* g_logFile = nullptr;

	void FormatAndWrite(const char* fmt, va_list args, bool toFile)
	{
		char buf[1024];
		_vsnprintf_s(buf, _TRUNCATE, fmt, args);
		OutputDebugStringA(buf);
		if (toFile)
		{
			if (!g_logFile) fopen_s(&g_logFile, "d3d12_debug.log", "w");
			if (g_logFile) { fputs(buf, g_logFile); fflush(g_logFile); }
		}
	}
}

void DebugLogV(const char* fmt, va_list args)
{
	FormatAndWrite(fmt, args, false);
}

void DebugLogImpl(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	FormatAndWrite(fmt, args, false);
	va_end(args);
}

void DebugLogFileImpl(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	FormatAndWrite(fmt, args, true);
	va_end(args);
}

#endif
