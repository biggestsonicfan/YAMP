#include "cs_game.h"

namespace vf5fs
{
	namespace Y6
	{
		bool dest_cs_autoload()
		{
			shift_next_mode(4);
			shift_next_mode_sub(48); // MODE_SUB_MAX
			return true;
		}
	}
}