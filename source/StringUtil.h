#pragma once

// UTF-8 <-> UTF-16 conversion, the two helpers every layer uses. They lived as `static`
// functions in YAMPGeneral.h (with the author's own "TODO: Move") - duplicated into every
// translation unit that read settings, and compiling only by include-order luck because that
// header never included <Windows.h> itself. `inline` here, with the include they need.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>
#include <string_view>

inline std::wstring UTF8ToWchar(std::string_view text)
{
	std::wstring result;

	const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if ( count != 0 )
	{
		result.resize(count);
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
	}

	return result;
}

inline std::string WcharToUTF8(std::wstring_view text)
{
	std::string result;

	const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if ( count != 0 )
	{
		result.resize(count);
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
	}

	return result;
}
