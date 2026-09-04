#include "ModuleArgs.h"

#include "../Utils/MemoryMgr.h"
#include "../Utils/Patterns.h"
#include "../pxd/PatternHelpers.h"
#include "../Utils/Trampoline.h"
#include "../DebugLog.h"
#include "../YAMPGeneral.h"
#include "../YAMPSettings.h"
#include "DisplayModes.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace m2ftg
{
	namespace ModuleArgs
	{
		namespace
		{
			void (*orgParseArgs)(int argc, char** argv);

			// The module's own argv, kept alive for the life of the process because the parser
			// stores pointers into it (the -ss base name is copied, but nothing guarantees that
			// for a future option).
			std::vector<std::string> s_storage;
			std::vector<char*> s_argv;

			// The display-mode index the anchor pattern presets to 3 ("-xga", 1024x768). Read back
			// after the parse by ResolvedRenderSize, so the compositor follows what the module
			// actually settled on rather than what was asked for - a switch the parser does not
			// recognise silently leaves it at the default.
			const int32_t* s_displayMode = nullptr;

			void ParseArgs_withHostArgv(int /*argc*/, char** /*argv*/)
			{
				orgParseArgs(static_cast<int>(s_argv.size()), s_argv.data());
			}

			// Builds the argv handed to the module from YAMP.exe's own, minus anything that would
			// make the parser misbehave. "--help" writes the usage block to stderr and then
			// executes an int3 - fine for the arcade binary this code came from, fatal here.
			//
			// Source it from __wargv, not __argv. YAMP's entry point is wWinMain, so the CRT
			// startup builds only the wide argv and leaves __argv null - reading it gives an empty
			// command line every time, silently. (The VF5FS app-ctor hook in vf5fs/Y6/Patch.cpp
			// forwards __argc/__argv and so has never actually passed anything either.)
			void BuildArgv()
			{
				s_storage.clear();
				s_argv.clear();
				if (__wargv == nullptr || __argc <= 0)
				{
					return;
				}

				s_storage.reserve(static_cast<size_t>(__argc));
				for (int i = 0; i < __argc; i++)
				{
					const wchar_t* wide = __wargv[i];
					if (wide == nullptr)
					{
						continue;
					}
					if (i > 0 && wcscmp(wide, L"--help") == 0)
					{
						DebugLog("[%s::args] dropping '--help': the module's usage handler ends in int3\n",
							gGeneral.GetGameTag());
						continue;
					}
					const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
					if (bytes <= 0)
					{
						continue;
					}
					std::string narrow(static_cast<size_t>(bytes) - 1, '\0');
					WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow.data(), bytes, nullptr, nullptr);
					s_storage.push_back(std::move(narrow));
				}

				// The render-mode setting, unless the command line already names one - an explicit
				// switch is the more specific instruction and should win rather than being handed
				// to the parser alongside a contradicting one.
				const YAMPSettings* settings = gGeneral.GetSettings();
				const uint32_t modeIndex = settings != nullptr ? settings->m_m2RenderMode : 0u;
				const bool explicitOnCommandLine = std::any_of(s_storage.begin() + 1, s_storage.end(),
					[](const std::string& arg) { return IsDisplayModeArg(UTF8ToWchar(arg)); });
				if (!explicitOnCommandLine && modeIndex > 0 && modeIndex < DISPLAY_MODE_COUNT)
				{
					s_storage.push_back(WcharToUTF8(DISPLAY_MODES[modeIndex].arg));
				}

				// The parser skips argv[0] and walks the rest, so a lone program name is a no-op
				// and there is no point installing the hook for it.
				if (s_storage.size() < 2)
				{
					s_storage.clear();
					return;
				}
				// Only now that s_storage has stopped growing are its buffers stable to point at.
				s_argv.reserve(s_storage.size());
				for (std::string& arg : s_storage)
				{
					s_argv.push_back(arg.data());
				}
			}
		}

		bool ResolvedRenderSize(uint32_t& width, uint32_t& height)
		{
			if (s_displayMode == nullptr) return false;
			const int32_t mode = *s_displayMode;
			if (mode < 0 || mode >= static_cast<int32_t>(std::size(MODULE_MODE_TABLE))) return false;
			width = MODULE_MODE_TABLE[mode][0];
			height = MODULE_MODE_TABLE[mode][1];
			return true;
		}

		bool Install(void* dll)
		{
			BuildArgv();
			if (s_argv.empty())
			{
				return false;
			}

			// pxd's app init presets the default display mode immediately before calling the
			// option parser, and reads it back immediately after:
			//
			//     C7 05 xx xx xx xx 03 00 00 00    mov dword ptr [rip+displayMode], 3
			//     E8 yy yy yy yy                   call ParseArgs
			//
			// Verified unique in .text in all five m2ftg modules (stf/fv/mr/vf2/omg), which is
			// what makes this safe to anchor on: the immediate 3 is the "-xga" table index, so
			// the pattern is really "the one place the default resolution is chosen".
			auto found = hook::pattern(pxd::module_scan(dll), "C7 05 ? ? ? ? 03 00 00 00 E8");
			if (found.size() != 1)
			{
				DebugLog("[%s::args] option-parser call site not found (%zu matches), module keeps its defaults\n",
					gGeneral.GetGameTag(), found.size());
				return false;
			}

			// `mov dword ptr [rip+displayMode], 3` — the RIP-relative target is the mode global.
			auto* const site = found.get(0).get<uint8_t>(0);
			int32_t disp = 0;
			memcpy(&disp, site + 2, sizeof(disp));
			s_displayMode = reinterpret_cast<const int32_t*>(site + 10 + disp);

			void* const callSite = found.get(0).get<void>(10);
			Memory::ReadCall(callSite, orgParseArgs);
			if (orgParseArgs == nullptr)
			{
				return false;
			}

			Trampoline* t = Trampoline::MakeTrampoline(dll);
			Memory::InjectHook(callSite, t->Jump(&ParseArgs_withHostArgv));
			return true;
		}
	}
}
