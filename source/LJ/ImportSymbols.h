#pragma once

namespace LJ
{
	namespace StF
	{
		enum class ImportSymbol
		{
			SL_CONTEXT_INSTANCE,
			GS_CONTEXT_INSTANCE,
			GS_CONTEXT_PTR,
			D3DDEVICE,

			SL_FILE_CREATE,
			SL_FILE_OPEN,
			SL_FILE_READ,
			// file_write is implemented by us, this is not a mistake
			SL_FILE_CLOSE,

			SL_HANDLE_CREATE,
			SL_FILE_HANDLE_DESTROY,
			PRJ_TRAP,

			ARCHIVE_LOCK_WLOCK,
			ARCHIVE_LOCK_WUNLOCK,

			DEVICE_CONTEXT_RESET_STATE_ALL,
			VB_CREATE,
			IB_CREATE,

			TRAP_ALLOC_INSTANCE_TBL,
		};

		class Imports BuildSymbolMap(void* dll);
	}
}