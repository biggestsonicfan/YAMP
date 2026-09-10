#include "ModuleLoad.h"

#include "GameVerify.h"
#include "YAMPGeneral.h"

#include <memory>
#include <string>

HMODULE LoadModuleDll(const wchar_t* dllName, const wchar_t* subdir, ModuleSearch search,
	std::filesystem::path& gamePath, const wchar_t* missingMessage)
{
	{
		DWORD dwSize = GetCurrentDirectoryW(0, nullptr);
		auto buf = std::make_unique<wchar_t[]>(dwSize);
		GetCurrentDirectoryW(dwSize, buf.get());
		gamePath.assign(buf.get());
	}

	std::filesystem::path dllFile;
	if (search == ModuleSearch::CwdThenSubdir)
	{
		dllFile = gamePath / dllName;
		if (!std::filesystem::is_regular_file(dllFile))
		{
			gamePath.append(subdir);
			dllFile = gamePath / dllName;
		}
	}
	else
	{
		gamePath /= subdir;
		dllFile = gamePath / dllName;
	}

	if (!std::filesystem::is_regular_file(dllFile))
	{
		// Same shape as the verification refusals (GameVerify.cpp): what happened, where YAMP
		// looked, what to do. The launcher is the first suggestion because it does the looking.
		const std::wstring str = missingMessage != nullptr
			? (L"Could not find " + std::wstring(dllName) + L".\n\n" + missingMessage)
			: (L"Could not find " + std::wstring(dllName) + L".\n\n"
				L"YAMP looked in the folder it was started from and in that folder's \"" + subdir +
				L"\" subfolder.\n\n"
				L"What to do:\n"
				L"  \u2022 Run YAMP.exe with no arguments: the launcher finds games by itself - next to "
				L"YAMP.exe, in any folder beside it, and in your Steam and GOG installs.\n"
				L"  \u2022 Or start YAMP from the folder that holds the module, or copy that folder (the "
				L"DLL with its rom and sound files) next to YAMP.exe.");
		MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
		return nullptr;
	}

	gGeneral.SetDLLName(WcharToUTF8(dllName));

	// Hard gate: the module's SHA-256 must match a build YAMP knows how to patch, and the
	// parent game must actually be installed. CheckBeforeLoad sets the DLL timestamp for the
	// About panel and explains whichever check failed to the user.
	if (!Verify::CheckBeforeLoad(gGeneral.GetGameId(), dllFile))
	{
		return nullptr;
	}

	HMODULE dll = LoadLibraryW(dllFile.c_str());
	if (dll == nullptr)
	{
		const std::wstring str(L"Could not load " + std::wstring(dllName) +
			L"!\n\nThe file exists but Windows refused to load it.");
		MessageBoxW(nullptr, str.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
	}
	return dll;
}
