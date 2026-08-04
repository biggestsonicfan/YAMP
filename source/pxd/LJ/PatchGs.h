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
		//
		// Templated over the build-variant gap (gs.h) because most of what it writes lives below
		// it: the Gaiden module's context puts those fields +0xBA8 further along. Only the two
		// shipped layouts are instantiated, so passing anything else is a link error rather than a
		// silently wrong offset. Callers that do not know which build is loaded should reach these
		// through gs::with_tail, which picks the instantiation from the running module.
		template <size_t Gap18>
		void PatchGs(gs::context_tmpl<Gap18>* context, const RenderWindow& window);

		// Reset the CBV/SRV descriptor-copy ring cursors each frame (they are per-frame transient;
		// without this they grow past the heap and CopyDescriptors AVs — id=646). Call at the start
		// of each frame.
		template <size_t Gap18>
		void ResetCbvSrvRingCursors(gs::context_tmpl<Gap18>* context);
	}
