#include "file_access.h"

#include <string_view>
#include "../YAMPGeneral.h"

namespace LJ
{
	namespace StF
	{
		// Debug feature: when "load loose ROM files" is enabled and a fully extracted
		// rom/stf_rom directory sits next to rom/stf_rom.par, refuse to open the archive.
		// module_start treats the failed mount as "no archive" (the handle it stores is only
		// ever released in module_stop, which tolerates 0), and the DLL's path resolver then
		// falls back from the archive-mount tree to plain file opens - so rom/stf_rom/*.bin
		// load straight from disk through the engine's own loose-file path.
		static bool ShouldBypassRomArchive(const char* path)
		{
			const auto* settings = gGeneral.GetSettings();
			if (settings == nullptr || !settings->m_stfLooseRomFiles)
			{
				return false;
			}

			const std::string_view pathView(path);
			constexpr std::string_view archiveName("stf_rom.par");
			if (pathView.size() < archiveName.size() ||
				_strnicmp(pathView.data() + (pathView.size() - archiveName.size()), archiveName.data(), archiveName.size()) != 0)
			{
				return false;
			}

			// Only bypass when every ROM image the boot loader requests is present, as the boot
			// state machine polls forever waiting for a file that never opens.
			static constexpr const wchar_t* ROM_FILES[] = {
				L"rom_code1.bin", L"rom_data.bin", L"rom_ep.bin", L"rom_pol.bin", L"rom_tex.bin",
			};
			const std::wstring directory = UTF8ToWchar(pathView.substr(0, pathView.size() - 4)); // strip ".par"
			for (const wchar_t* romFile : ROM_FILES)
			{
				const std::wstring romPath = directory + L'/' + romFile;
				const DWORD attributes = GetFileAttributesW(romPath.c_str());
				if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				{
					char buf[600];
					sprintf_s(buf, "[file] loose-rom: '%ls' is missing, loading the archive as usual\n", romPath.c_str());
					OutputDebugStringA(buf);
					return false;
				}
			}

			char buf[600];
			sprintf_s(buf, "[file] loose-rom: hiding '%s', ROM images will load from the stf_rom directory\n", path);
			OutputDebugStringA(buf);
			return true;
		}

		bool csl_file_access::open(const char* path, sl::handle_t handle)
		{
			if (ShouldBypassRomArchive(path))
			{
				return false;
			}

			HANDLE file = CreateFileW(UTF8ToWchar(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

			// TEMP diagnostic: trace every host file open (loader-loop investigation)
			{
				char buf[600];
				sprintf_s(buf, "[file] open '%s' -> %s (h=%u)\n", path,
					file == INVALID_HANDLE_VALUE ? "FAIL" : "ok", handle.h.m_handle);
				OutputDebugStringA(buf);
			}

			if (file == INVALID_HANDLE_VALUE)
			{
				return false;
			}

			sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			internalHandle->m_h_native = file;

			return true;
		}

		bool csl_file_access::create(const char* path, sl::handle_t handle)
		{
			HANDLE file = CreateFileW(UTF8ToWchar(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				return false;
			}

			sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			internalHandle->m_h_native = file;

			return true;
		}

		bool csl_file_access::remove(const char*)
		{
			// Deliberately unimplemented for now
			assert(!"csl_file_access::remove unimplemented!");
			return false;
		}

		bool csl_file_access::close(sl::handle_t handle)
		{
			sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			if (internalHandle != nullptr)
			{
				HANDLE& nativeHandle = reinterpret_cast<HANDLE&>(internalHandle->m_h_native);
				CloseHandle(nativeHandle);
				nativeHandle = INVALID_HANDLE_VALUE;
				return true;
			}
			return false;
		}

		bool csl_file_access::is_exist(const char* path)
		{
			const bool exists = GetFileAttributesW(UTF8ToWchar(path).c_str()) != INVALID_FILE_ATTRIBUTES;
			// TEMP diagnostic (loader-loop investigation)
			{
				char buf[600];
				sprintf_s(buf, "[file] is_exist '%s' -> %d\n", path, exists ? 1 : 0);
				OutputDebugStringA(buf);
			}
			return exists;
		}

		int64_t csl_file_access::read(sl::handle_t handle, void* buffer, unsigned int size)
		{
			int64_t result = -1;
			sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			if (internalHandle != nullptr)
			{
				HANDLE nativeHandle = reinterpret_cast<HANDLE>(internalHandle->m_h_native);
				if (nativeHandle != INVALID_HANDLE_VALUE)
				{
					DWORD numBytesRead;
					if (ReadFile(nativeHandle, buffer, size, &numBytesRead, nullptr) != FALSE)
					{
						internalHandle->m_file_pointer += numBytesRead;
						result = numBytesRead;
					}
				}
			}
			return result;
		}

		int64_t csl_file_access::write(sl::handle_t handle, const void* buffer, unsigned int size)
		{
			int64_t result = -1;
			sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			if (internalHandle != nullptr)
			{
				HANDLE nativeHandle = reinterpret_cast<HANDLE>(internalHandle->m_h_native);
				if (nativeHandle != INVALID_HANDLE_VALUE)
				{
					DWORD numBytesWritten;
					if (WriteFile(nativeHandle, buffer, size, &numBytesWritten, nullptr) != FALSE)
					{
						internalHandle->m_file_pointer += numBytesWritten;
						result = numBytesWritten;
					}
				}
			}
			return result;
		}

		int64_t csl_file_access::get_size(sl::handle_t handle)
		{
			int64_t size = -1;
			const sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			if (internalHandle != nullptr)
			{
				HANDLE nativeHandle = reinterpret_cast<HANDLE>(internalHandle->m_h_native);
				if (nativeHandle != INVALID_HANDLE_VALUE)
				{
					LARGE_INTEGER s;
					if (GetFileSizeEx(nativeHandle, &s) != FALSE)
					{
						size = s.QuadPart;
					}
				}
			}
			return size;
		}

		int64_t csl_file_access::seek(sl::handle_t handle, int64_t offset, sl::FILE_SEEK seekMode)
		{
			int64_t newPointer = -1;
			sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			if (internalHandle != nullptr)
			{
				HANDLE nativeHandle = reinterpret_cast<HANDLE>(internalHandle->m_h_native);
				if (nativeHandle != INVALID_HANDLE_VALUE)
				{
					LARGE_INTEGER newOffset;
					LARGE_INTEGER off;
					off.QuadPart = offset;
					if (SetFilePointerEx(nativeHandle, off, &newOffset, seekMode != sl::FILE_SEEK_SET ? FILE_CURRENT : FILE_BEGIN) != FALSE)
					{
						newPointer = newOffset.QuadPart;
						internalHandle->m_file_pointer = newOffset.QuadPart;
					}
				}
			}
			return newPointer;
		}

		int64_t csl_file_access::read_offset(sl::handle_t handle, void* buffer, unsigned int size, uint64_t offset)
		{
			seek(handle, offset, sl::FILE_SEEK_SET);
			return read(handle, buffer, size);
		}

		uint64_t csl_file_access::burst_read(sl::handle_t, void*, unsigned int, unsigned __int64)
		{
			return 0;
		}

		uint64_t csl_file_access::burst_read_flush(sl::handle_t)
		{
			return 0;
		}

		uint64_t csl_file_access::burst_read_wait(sl::handle_t)
		{
			return 0;
		}

		void csl_file_access::burst_status_write(unsigned int)
		{
		}

		bool csl_file_access::burst_status_is_complete(unsigned int)
		{
			return false;
		}

		void csl_file_access::burst_status_wait(unsigned int)
		{
		}

		// ===================================================================

		csl_file_access_archive::csl_file_access_archive()
		{
			// TODO: Allocator, if needed
			void* buf = ::operator new(0x88000);
			mp_sector_cache_buffer = buf;
			char* decode = static_cast<char*>(buf) + 0x8000;
			mp_decode_buffer[0] = decode;
			mp_decode_buffer[1] = decode + 0x40000;
			for (auto& table : m_sector_cache_tbl)
			{
				m_sector_cache_list.push_back(&table);
			}
		}

		bool csl_file_access_archive::open(const char* path, sl::handle_t handle)
		{
			// TEMP diagnostic (loader-loop investigation)
			{
				char buf[600];
				sprintf_s(buf, "[file] archive::open '%s' -> false (stub)\n", path != nullptr ? path : "(null)");
				OutputDebugStringA(buf);
			}
			return false;
		}

		bool csl_file_access_archive::create(const char* /*path*/, sl::handle_t /*handle*/)
		{
			// Deliberately unimplemented
			return false;
		}

		bool csl_file_access_archive::remove(const char*)
		{
			// Deliberately unimplemented
			return false;
		}

		bool csl_file_access_archive::close(sl::handle_t /*handle*/)
		{
			return true;
		}

		bool csl_file_access_archive::is_exist(const char* path)
		{
			return false;
		}

		int64_t csl_file_access_archive::read(sl::handle_t handle, void* /*buffer*/, unsigned int size)
		{
			sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			if (internalHandle != nullptr)
			{
				sl::handle_t nativeHandle;
				nativeHandle.h.m_handle = reinterpret_cast<uint64_t>(internalHandle->m_h_native);
				csl_archive* archive = csl_archive::create_instance(nativeHandle);
				if (archive != nullptr)
				{
					const int64_t offset = archive->read_file(handle, size);
					archive->release();
					if (offset != -1)
					{
						internalHandle->m_file_pointer += offset;
					}
					return offset;
				}
			}
			return -1;
		}

		int64_t csl_file_access_archive::write(sl::handle_t, const void*, unsigned int)
		{
			return -1;
		}

		int64_t csl_file_access_archive::get_size(sl::handle_t handle)
		{
			return int64_t();
		}

		int64_t csl_file_access_archive::seek(sl::handle_t handle, int64_t offset, sl::FILE_SEEK seekMode)
		{
			sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			if (internalHandle != nullptr)
			{
				if ((internalHandle->m_flags & 0x100000) == 0 && (internalHandle->m_flags & 0x10000) != 0)
				{
					sl::rwspinlock_wlock(internalHandle->m_locked);

					if (seekMode != sl::FILE_SEEK_SET)
					{
						offset += internalHandle->m_file_pointer;
					}
					if (offset >= 0)
					{
						offset = std::min<uint64_t>(offset, internalHandle->m_real_file_size);
					}
					else
					{
						offset = 0;
					}
					internalHandle->m_file_pointer = offset;

					sl::rwspinlock_wunlock(internalHandle->m_locked);
				}
			}
			return -1; // Returns -1 even on success - original bug?
		}

		int64_t csl_file_access_archive::read_offset(sl::handle_t handle, void* buffer, unsigned int size, uint64_t offset)
		{
			seek(handle, offset, sl::FILE_SEEK_SET);
			return read(handle, buffer, size);
		}

		uint64_t csl_file_access_archive::burst_read(sl::handle_t, void*, unsigned int, unsigned __int64)
		{
			return uint64_t();
		}

		uint64_t csl_file_access_archive::burst_read_flush(sl::handle_t)
		{
			return uint64_t();
		}

		uint64_t csl_file_access_archive::burst_read_wait(sl::handle_t)
		{
			return uint64_t();
		}

		void csl_file_access_archive::burst_status_write(unsigned int)
		{
		}

		bool csl_file_access_archive::burst_status_is_complete(unsigned int)
		{
			return false;
		}

		void csl_file_access_archive::burst_status_wait(unsigned int)
		{
		}

		// ===================================================================

		csl_archive* csl_archive::create_instance(sl::handle_t handle)
		{
			sl::archive_lock_wlock(&sl::sm_context->sync_archive_condvar);
			csl_archive* archive = sl::handle_instance<csl_archive>(handle, 6);
			if (archive != nullptr)
			{
				archive->add_ref();
			}
			sl::archive_lock_wunlock(&sl::sm_context->sync_archive_condvar);
			return archive;
		}
	}
}