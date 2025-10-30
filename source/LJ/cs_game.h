#pragma once

namespace LJ
{
	namespace StF
	{
		inline void (*shift_next_mode)(int mode);
		inline void (*shift_next_mode_sub)(int modeSub);

		bool dest_cs_autoload();
	}
}