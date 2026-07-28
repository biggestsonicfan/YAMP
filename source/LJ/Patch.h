#pragma once

#include "../RenderWindow.h"
#include "sl.h"
#include "gs.h"

#include "Imports.h"

namespace LJ
{
	namespace StF
	{
		void PatchSl(sl::context_t* context);
		void PatchGs(gs::context_t* context, const RenderWindow& window);
		// Reset the CBV/SRV descriptor-copy ring cursors each frame (they are per-frame transient; without
		// this they grow past the heap and CopyDescriptors AVs — id=646). Call at the start of each frame.
		void ResetCbvSrvRingCursors(gs::context_t* context);
		void ReinstateLogging(void* dll, const Imports& symbols);
		void InjectTraps(const Imports& symbols);
		// Replaces the DLL's i960 instruction fetch with a region-aware version so code in
		// emulated work RAM (the ROM debug menu's trampoline at 0x59F270) can execute.
		void InstallRamExecFetch(void* dll, const Imports& symbols);

		void Patch_SysUtil(void* dll, const Imports& symbols);
		void Patch_CsGame(void* dll, const Imports& symbols);
		void Patch_Misc(void* dll, const Imports& symbols);
	}
}