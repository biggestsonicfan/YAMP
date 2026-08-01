#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace m2ftg
{
	// The Model 2 modules carry pxd's own command-line option parser, and its resolution switches
	// are the one option family a retail module still acts on: they pick a row in a 17-entry table
	// (byte-identical in stf/fv/mr/vf2/omg) that the emulator lays its viewport and 2D screen out
	// at. module_start calls the parser with an empty argv, so nothing was ever selectable until
	// ModuleArgs re-pointed that call - see ModuleArgs.h.
	//
	// WHAT THE MODE DOES NOT CHANGE: the output texture. Measured on a live StF at modes 2, 6, 15
	// and 16 - the texture YAMP composites is 1024x768 every time. The mode only moves the viewport
	// the module draws with INSIDE that fixed target, so:
	//
	//   * a mode smaller than 1024x768 leaves the frame in the top-left sub-rect, and YAMP has to
	//     sample just that sub-rect and fit it to the window (RenderWindow::SetModuleSourceRect) -
	//     otherwise the black remainder of the texture gets stretched in with the picture;
	//   * a mode LARGER than 1024x768 draws past the target and is clipped by it, losing the right
	//     and bottom of the frame for good. Nothing downstream can recover that, so those rows are
	//     not offered even though the module's table and parser accept them. This was checked
	//     rather than assumed: -wxga/-wxga_dbd/-wxga2/-sxga/-uxga were briefly added back and run,
	//     and they do crop - the module clips, it does not rescale its projection to fit.
	//
	// This is the module's INTERNAL render size, not YAMP's window: the swapchain stays at
	// [Graphics] ResolutionX/Y unless Model2WindowMatchesRender is on, and YAMP scales the module's
	// output into it - so "Model 2 native" means the emulator draws at the arcade board's real
	// 496x384 and YAMP presents it upscaled.

	// The module's output texture, and so the upper bound on a usable mode.
	inline constexpr uint32_t OUTPUT_TEXTURE_WIDTH = 1024;
	inline constexpr uint32_t OUTPUT_TEXTURE_HEIGHT = 768;

	// The module's own table, mirrored. Indexed by the display-mode value the module resolves for
	// itself, which is NOT the order of the offered list below. Rows 11-14 have no option string
	// pointing at them and are unreachable; rows 4-6 and 9-10 are reachable but oversized.
	inline constexpr uint32_t MODULE_MODE_TABLE[17][2] = {
		{  320, 240 }, {  640,  480 }, {  800, 600 }, { 1024,  768 }, { 1280, 1024 },
		{ 1400,1050 }, { 1600, 1200 }, {  800, 480 }, { 1024,  600 }, { 1280,  768 },
		{ 1360, 768 }, { 1920, 1200 }, { 1280, 720 }, { 1920, 1080 }, {  960,  544 },
		{  496, 384 }, {  992,  768 },
	};

	// The modes YAMP offers: the ones that fit inside the output texture. `moduleIndex` is the row
	// the module's parser resolves the switch to, and the only place a size is written down.
	struct DisplayMode
	{
		const wchar_t* arg;    // the module's own switch; empty = leave it on its own default
		const char* label;
		uint32_t moduleIndex;  // row in MODULE_MODE_TABLE
	};

	inline constexpr DisplayMode DISPLAY_MODES[] = {
		{ L"",           "Module default (1024x768)",    3 },
		{ L"-model2",    "Model 2 native (496x384)",    15 },
		{ L"-model2x2",  "Model 2 native x2 (992x768)", 16 },
		{ L"-vga",       "640x480",                      1 },
		{ L"-svga",      "800x600",                      2 },
		{ L"-wvga",      "800x480",                      7 },
		{ L"-wsvga",     "1024x600",                     8 },
	};

	inline constexpr size_t DISPLAY_MODE_COUNT = sizeof(DISPLAY_MODES) / sizeof(DISPLAY_MODES[0]);

	inline constexpr uint32_t DisplayModeWidth(uint32_t index)
	{
		return MODULE_MODE_TABLE[DISPLAY_MODES[index < DISPLAY_MODE_COUNT ? index : 0].moduleIndex][0];
	}
	inline constexpr uint32_t DisplayModeHeight(uint32_t index)
	{
		return MODULE_MODE_TABLE[DISPLAY_MODES[index < DISPLAY_MODE_COUNT ? index : 0].moduleIndex][1];
	}

	// Persisted as the switch text rather than an index, so reordering this list cannot silently
	// change what an existing settings.ini means. An unknown switch (including one of the oversized
	// modes left in an older ini) falls back to the module default.
	inline int DisplayModeFromArg(std::wstring_view arg)
	{
		for (int i = 0; i < static_cast<int>(DISPLAY_MODE_COUNT); i++)
		{
			if (arg == DISPLAY_MODES[i].arg) return i;
		}
		return 0;
	}

	// True for the switches above, used to let an explicit one on YAMP's command line win over
	// the setting instead of both being handed to the parser.
	inline bool IsDisplayModeArg(std::wstring_view arg)
	{
		if (arg.empty()) return false;
		for (int i = 1; i < static_cast<int>(DISPLAY_MODE_COUNT); i++)
		{
			if (arg == DISPLAY_MODES[i].arg) return true;
		}
		return false;
	}

	// The mode YAMP is about to ask for, resolvable BEFORE the module is loaded: an explicit switch
	// on YAMP's command line wins, otherwise the setting's index. ModuleArgs applies the same
	// precedence when it builds the module's argv, so this predicts what the module will resolve
	// to - which is what the window has to be sized from, since the window exists long before
	// module_start runs the parse.
	inline int IntendedDisplayMode(uint32_t settingIndex)
	{
		if (__wargv != nullptr)
		{
			for (int i = 1; i < __argc; i++)
			{
				if (__wargv[i] != nullptr && IsDisplayModeArg(__wargv[i]))
				{
					return DisplayModeFromArg(__wargv[i]);
				}
			}
		}
		return settingIndex < DISPLAY_MODE_COUNT ? static_cast<int>(settingIndex) : 0;
	}
}
