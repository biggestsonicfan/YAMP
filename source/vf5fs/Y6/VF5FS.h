#pragma once

#include "../../RenderWindow.h"

using module_func_t = int(*)(size_t args, const void* argp);

namespace vf5fs
{
	namespace Y6
	{
		HMODULE LoadDLL();
		void PreInitialize();
		void Run(RenderWindow& window);
		bool GameLoop(module_func_t func, RenderWindow& window);
	}
}