workspace "YAMP"
	platforms { "Win64" }

project "YAMP"
	kind "WindowedApp"
	language "C++"

	include "source/VersionInfo.lua"
	files { "**/MemoryMgr.h", "**/Trampoline.h", "**/Patterns.*" }


workspace "*"
	configurations { "Debug", "Release", "Master" }
	location "build"

	vpaths { ["Headers/*"] = "source/**.h",
			["Sources/*"] = { "source/**.c", "source/**.cpp" },
			["Resources"] = "source/**.rc"
	}

	-- 2026-07-28 restructure: source/Y6 -> source/vf5fs, source/LJ (+StF hosting) -> source/m2ftg
	-- (which has LJ/ and YLAD/ subdirectories, hence the recursive globs), source/DX12 removed.
	-- Utils gained .cpp files (DebugLog.cpp, Patterns.cpp) and is globbed as a directory now.
	-- NB build/YAMP.vcxproj has been hand-maintained while this file was stale; before trusting a
	-- regen, diff its file list against the vcxproj.
	files { "source/*.h", "source/*.cpp", "source/resources/*.rc", "source/criware/*", "source/wil/*",
			"source/imgui/*", "source/Utils/*",
			"source/m2ftg/**.h", "source/m2ftg/**.cpp",
			"source/vf5fs/*" }

	cppdialect "C++17"
	staticruntime "on"
	buildoptions { "/sdl" }
	warnings "Extra"

	-- Automated defines for resources
	defines { "rsc_Extension=\"%{prj.targetextension}\"",
			"rsc_Name=\"%{prj.name}\"" }

filter "configurations:Debug"
	defines { "DEBUG" }
	runtime "Debug"

 filter "configurations:Master"
	defines { "NDEBUG", "RESULT_DIAGNOSTICS_LEVEL=0", "RESULT_INCLUDE_CALLER_RETURNADDRESS=0" }
	symbols "Off"

filter "configurations:not Debug"
	optimize "Speed"
	functionlevellinking "on"
	flags { "LinkTimeOptimization" }

filter { "platforms:Win32" }
	system "Windows"
	architecture "x86"

filter { "platforms:Win64" }
	system "Windows"
	architecture "x86_64"

filter { "toolset:*_xp"}
	defines { "WINVER=0x0501", "_WIN32_WINNT=0x0501" } -- Target WinXP
	buildoptions { "/Zc:threadSafeInit-" }

filter { "toolset:not *_xp"}
	defines { "WINVER=0x0601", "_WIN32_WINNT=0x0601" } -- Target Win7
	buildoptions { "/permissive-" }

-- The audio decoders (and the engine's PCM-conversion loops in AtomEngine.cpp) chew through
-- whole BGM streams at cue start; keep them optimized even in Debug so first-play decode stays
-- in the tens of milliseconds and the chunked-start tail never misses its splice.
filter { "files:**HcaDecoder.cpp or **AdxDecoder.cpp or **AtomEngine.cpp" }
	optimize "Speed"

filter {}