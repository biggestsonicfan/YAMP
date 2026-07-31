#pragma once

// The pxd DX12 gs bring-up shared by every Lost-Judgment-era host (the m2ftg modules and VF5FS).
// Implementation + the RE notes for each field are in PatchGs.cpp.

#include "gs.h"

class RenderWindow;

namespace pxd
{
		// Fill in everything the LJ host normally puts into a fresh gs::context_t before the module
		// renders: the descriptor blocks and shader-visible copy rings, the tex-id tables, the
		// device context, and the host cdevice behind cdevice_common::g_pD3DDevice.
		void PatchGs(gs::context_t* context, const RenderWindow& window);

		// Reset the CBV/SRV descriptor-copy ring cursors each frame (they are per-frame transient;
		// without this they grow past the heap and CopyDescriptors AVs — id=646). Call at the start
		// of each frame.
		void ResetCbvSrvRingCursors(gs::context_t* context);
	}
