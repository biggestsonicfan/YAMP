#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <filesystem>

// The one way a game module DLL is located and loaded - every host (m2ftg LJ/YLAD/K2, pre3,
// all three VF5FS) used to carry its own copy of this sequence, identical MessageBox strings
// and all. The contract:
//
//   * The DLL is located as a FILE first, because the integrity check has to run before
//     LoadLibrary - a module YAMP is about to reject must not get to run its DllMain.
//   * CwdThenSubdir probes the CWD, then `subdir` beneath it (how the real game installs are
//     laid out); SubdirOnly goes straight to the subdirectory (the YLAD VF2 layout).
//   * On success `gamePath` holds the directory the DLL was found in - the module's root
//     path, which several hosts hand to module_start.
//   * Failure paths tell the user what is missing and return null: a missing file (standard
//     text naming the subdir, or `missingMessage` when the host has a more specific one),
//     a failed Verify::CheckBeforeLoad (which explains itself), or a LoadLibrary refusal.
enum class ModuleSearch { CwdThenSubdir, SubdirOnly };

HMODULE LoadModuleDll(const wchar_t* dllName, const wchar_t* subdir, ModuleSearch search,
	std::filesystem::path& gamePath, const wchar_t* missingMessage = nullptr);
