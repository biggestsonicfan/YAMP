#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

class RenderWindow;

namespace m2ftg
{
	namespace VF2
	{
		HMODULE LoadDLL();
		void PreInitialize();
		void Run(RenderWindow& window);
	}
}
