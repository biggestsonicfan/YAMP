#include "ModuleBuild.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace m2ftg
{
	namespace
	{
		uint32_t g_currentBuild = 0;
	}

	uint32_t ModuleTimestamp(const void* moduleBase)
	{
		if (moduleBase == nullptr)
		{
			return 0;
		}

		const auto* base = static_cast<const uint8_t*>(moduleBase);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			return 0;
		}

		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
		{
			return 0;
		}

		return nt->FileHeader.TimeDateStamp;
	}

	uint32_t CurrentModuleBuild()
	{
		return g_currentBuild;
	}

	void SetCurrentModuleBuild(uint32_t timestamp)
	{
		g_currentBuild = timestamp;
	}

	bool IsGaidenBuild()
	{
		return g_currentBuild == build::GAIDEN_STF || g_currentBuild == build::GAIDEN_MR;
	}
}
