#include "sl.h"

// Everything that used to live here - the mutex/spinlock/thread/file primitives, csl_pad's
// raw-XInput set_state, the handle machinery - was a byte-identical copy of the base layer
// (pxd/LJ/sl.cpp) and was deleted in the 2026-08-09 de-fork; see sl.h. What remains is the
// one definition that is genuinely this generation's: its context pointer.

namespace vf5fs
{
	namespace Y6
	{
		namespace sl
		{
			context_t* sm_context;
		}
	}
}
