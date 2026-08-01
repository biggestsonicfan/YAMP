#pragma once

#include <cstdint>

namespace m2ftg
{
	// Every m2ftg module (stf/fv/mr from LJ, vf2 from YLAD, vf2/omg from K2) carries pxd's own
	// command-line option parser, reachable from module_start via the app module's init:
	//
	//     module_start -> AppModule::vftable[0] -> AppInit -> ParseArgs(argc, argv)
	//
	// module_start hardcodes argc = 0 / argv = nullptr into the globals AppInit forwards, so the
	// parser's first test (`if (argv != nullptr)`) fails and no option is ever read. The params
	// block YAMP fills has no argv field - the DLL simply never asks the host for one.
	//
	// Install() re-points that one call at YAMP.exe's own argv, so switches on YAMP's command line
	// reach the module's parser unchanged. This is the module's own parser doing the work; nothing
	// here reimplements or second-guesses it.
	//
	// Only the display-mode switches actually drive anything in a retail module - the rest
	// (-debug, -m, -s, -ve, -fs, -aa, -ss, -ss4x) parse into globals with no reader left, the
	// same stripping that hollowed out the dw debug menu's handlers. Which display modes are
	// usable, and why the oversized ones are not offered, is in DisplayModes.h.
	//
	// Call after LoadLibrary and before module_start. Idempotent per module load.
	namespace ModuleArgs
	{
		// Returns false when the module has no parser call site to hook, which leaves the module
		// running exactly as it did before (default 1024x768).
		bool Install(void* dll);

		// The render size the module actually settled on, valid once module_start has run (the parse
		// happens inside it). Reads the module's own resolved display-mode global rather than the
		// YAMP setting, so an explicit switch on the command line is reflected too. False when no
		// parser was found or the module reported an index outside its table.
		bool ResolvedRenderSize(uint32_t& width, uint32_t& height);
	}
}
