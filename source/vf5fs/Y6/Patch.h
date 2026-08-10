#pragma once

#include "../../RenderWindow.h"
#include "../../pxd/Y6/sl.h"
#include "../../pxd/Y6/gs.h"

#include "ImportSymbols.h"

namespace vf5fs
{
	namespace Y6
	{
		void PatchSl(sl::context_t* context);
		void PatchGs(gs::context_t* context, const RenderWindow& window);
		void ReinstateLogging(void* dll, const Imports& symbols);
		void InjectTraps(const Imports& symbols);

		void Patch_SysUtil(void* dll, const Imports& symbols);
		void Patch_CsGame(void* dll, const Imports& symbols);
		void Patch_Misc(void* dll, const Imports& symbols);
	}
}