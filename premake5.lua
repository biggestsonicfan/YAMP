workspace "YAMP"
	platforms { "Win64" }

project "YAMP"
	kind "WindowedApp"
	language "C++"

	include "source/VersionInfo.lua"
	files { "**/MemoryMgr.h", "**/Trampoline.h", "**/Patterns.*" }

	-- YAMP's own sources. These used to live in the shared `workspace "*"` block below, but that
	-- block applies to EVERY project in the workspace - once the netplay plugin became a second
	-- project it started inheriting the whole emulator, so the list moved here where it belongs.
	files { "source/*.h", "source/*.cpp", "source/resources/*.rc", "source/criware/*", "source/wil/*",
			"source/imgui/*", "source/Utils/*", "source/input/*", "source/cabinet/*",
			"source/net/YampNet.h", "source/net/NetPlugin.h", "source/net/NetPlugin.cpp",
			"source/net/LinkedCabinet.h",
			"source/pxd/**.h", "source/pxd/**.cpp",
			"source/m2ftg/**.h", "source/m2ftg/**.cpp",
			"source/pre3/**.h", "source/pre3/**.cpp",
			"source/vf5fs/**.h", "source/vf5fs/**.cpp" }

	-- bcrypt: SHA-256 for the arcade module integrity check (source/GameVerify.cpp).
	-- dinput8/dxguid: the non-XInput half of the controller layer (source/input/DirectInputPad.cpp)
	-- — arcade encoders, fight sticks and most third-party pads are HID-only and invisible to XInput.
	links { "bcrypt", "dinput8", "dxguid" }


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
	-- 2026-07-29: source/vf5fs gained per-title subdirectories too (Y6/ for the Yakuza 6 DX11
	-- module, LJ/ for the Lost Judgment DX12 one), so its globs are recursive as well.
	-- source/pxd = the pxd PLATFORM layers, one folder per engine generation, kept apart from the
	-- hosts that sit on them (2026-07-30 restructure):
	--   source/pxd/     generation-NEUTRAL only: Imports.h (the pxd::ImportsT container every host
	--                   instantiates with its own symbol enum) and pxd_shader.h (both gs.h's use it).
	--   source/pxd/LJ/  Lost-Judgment era (sl 0xF000 / gs 0x388A00 / ct 0x30, DX12 host cdevice) —
	--                   used by the m2ftg hosts, the LJ VF5FS host and (sl only) the YLAD one.
	--   source/pxd/Y6/  Yakuza 6 era: its own sl/gs/file_access/async_request/cs_game/sys_util/
	--                   pxd_types. Moved out of source/vf5fs/Y6/, where it looked like shared VF5FS
	--                   code — the LJ and YLAD VF5FS hosts include none of it.
	-- Hence the pxd globs are recursive now; source/vf5fs/<title>/ holds hosts only.
	-- source/input = the binding layer EVERY game shares: M2Input (key/pad bindings, read straight
	-- out of YAMPSettings) and M2Pad (pxd::csl_pad::set_state, the policy that turns those bindings
	-- into engine pad state). Moved up out of source/m2ftg on 2026-07-30: all three VF5FS hosts use
	-- it too, and YAMPSettings.h itself includes it, so it was never m2ftg-specific.
	-- source/net = the netplay PLUGIN LOADER only (NetPlugin.cpp + the shared YampNet.h ABI). The
	-- netcode itself is a separate DLL project (see "YampNet" at the bottom of this file) so it can
	-- be rebuilt, replaced or omitted entirely without touching YAMP.

	cppdialect "C++17"
	staticruntime "on"
	buildoptions { "/sdl" }
	warnings "Extra"

	-- C4324 "structure was padded due to alignment specifier" fires ~130 times on a full build and
	-- is never actionable here: the reverse-engineered structs use alignas deliberately to match the
	-- game's own layout, and every one of them is pinned by static_asserts on its size and field
	-- offsets. Muting it keeps the warning list small enough that a real warning is visible.
	disablewarnings { "4324" }

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

-- ------------------------------------------------------------------------------------------------
-- YampNet - the OPTIONAL netplay plugin, built as its own DLL.
--
-- Kept a separate project on purpose: the netcode is expected to churn long after the rest of YAMP
-- is stable, and a release must be able to ship with no netplay in it at all. YAMP never links
-- against this - it LoadLibrary's it at runtime (source/net/NetPlugin.cpp) and disables netplay
-- when it is absent - so simply not shipping yampnet.dll is the "exclude it" switch. Building this
-- project is likewise optional; YAMP.vcxproj does not depend on it.
--
-- It lands in the same output directory as YAMP.exe, which is where the loader looks.
--
-- NOTE it includes source/m2ftg + source/pxd headers: the plugin writes execute_info.pad[] itself
-- (that was a deliberate scope choice), so it is coupled to those layouts. The yampnet_layout
-- handshake in YampNet.h is what turns a stale-layout plugin into a clean refusal at load instead
-- of silent memory corruption - keep it honest.
-- ------------------------------------------------------------------------------------------------
workspace "YAMP"

project "YampNet"
	kind "SharedLib"
	language "C++"
	targetname "yampnet"

	files { "plugin/yampnet/**.h", "plugin/yampnet/**.cpp",
			"source/net/YampNet.h" }

	includedirs { "source/net", "source" }

	-- ws2_32: the UDP/TCP transport. crypt32/secur32: TLS to the RPCN server.
	links { "ws2_32", "crypt32", "secur32" }

	vpaths { ["Headers/*"] = { "plugin/yampnet/**.h", "source/net/*.h" },
			["Sources/*"] = "plugin/yampnet/**.cpp" }

filter {}