#include "../pxd/LJ/file_access.h"

#include <string_view>
#include "../YAMPGeneral.h"
#include "../DebugLog.h"
#include "ELF/ElfRom.h"
#include "LJ/LJHost.h" // GameDesc / CurrentGame()

namespace pxd
{
		// ---- ELF program-ROM override -------------------------------------------------
		// When game.elf is present, the module still opens rom_code1.bin exactly as it always
		// has; these helpers serve the flattened ELF image for that one path instead of a file
		// on disk. Every entry point below keeps its original native implementation - the
		// override is a check at the top, so a handle that is not the program ROM takes the
		// same code path it always did.
		namespace RomOverride
		{
			// Distinct from INVALID_HANDLE_VALUE (-1) so the existing "is this handle usable"
			// tests keep working unchanged for real files.
			static HANDLE const MEMORY_BACKED = reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2));

			// True when this path is the game's program ROM (rom_files[0]: rom_code1.bin, or
			// rom_code_tw.bin for Motor Raid) and an ELF override is loaded for it.
			static bool IsProgramRom(const char* path)
			{
				if (!m2ftg::ElfRom::IsLoaded() || path == nullptr)
				{
					return false;
				}
				const m2ftg::GameDesc& game = m2ftg::CurrentGame();
				if (game.rom_file_count == 0)
				{
					return false;
				}
				const std::string name = WcharToUTF8(game.rom_files[0]);
				const std::string_view pathView(path);
				return pathView.size() >= name.size() &&
					_strnicmp(pathView.data() + (pathView.size() - name.size()), name.data(), name.size()) == 0;
			}

			static bool IsMemoryBacked(const sl::file_handle_internal_t* internalHandle)
			{
				return internalHandle != nullptr &&
					reinterpret_cast<HANDLE>(internalHandle->m_h_native) == MEMORY_BACKED;
			}

			static int64_t Read(sl::file_handle_internal_t* internalHandle, void* buffer, unsigned int size)
			{
				const uint32_t imageSize = m2ftg::ElfRom::ImageSize();
				const uint64_t pos = internalHandle->m_file_pointer;
				if (pos >= imageSize)
				{
					return 0;
				}
				const uint64_t available = imageSize - pos;
				const unsigned int count = size < available ? size : static_cast<unsigned int>(available);
				memcpy(buffer, m2ftg::ElfRom::Image() + pos, count);
				internalHandle->m_file_pointer += count;
				return count;
			}

			static int64_t Seek(sl::file_handle_internal_t* internalHandle, int64_t offset, sl::FILE_SEEK seekMode)
			{
				const int64_t base = seekMode != sl::FILE_SEEK_SET
					? static_cast<int64_t>(internalHandle->m_file_pointer) : 0;
				const int64_t target = base + offset;
				if (target < 0)
				{
					return -1;
				}
				internalHandle->m_file_pointer = static_cast<uint64_t>(target);
				return target;
			}
		}

		// Debug feature: when "load loose ROM files" is enabled and a fully extracted
		// rom/<game>_rom directory sits next to rom/<game>_rom.par, refuse to open the archive.
		// module_start treats the failed mount as "no archive" (the handle it stores is only
		// ever released in module_stop, which tolerates 0), and the DLL's path resolver then
		// falls back from the archive-mount tree to plain file opens - so rom/<game>_rom/*.bin
		// load straight from disk through the engine's own loose-file path. Archive name and
		// the per-game ROM image list come from the GameDesc table (StF.cpp).
		static bool ShouldBypassRomArchive(const char* path)
		{
			const auto* settings = gGeneral.GetSettings();
			if (settings == nullptr || !settings->m_stfLooseRomFiles)
			{
				return false;
			}

			const m2ftg::GameDesc& game = m2ftg::CurrentGame();
			const std::string_view pathView(path);
			const std::string_view archiveName(game.rom_archive_name);
			if (pathView.size() < archiveName.size() ||
				_strnicmp(pathView.data() + (pathView.size() - archiveName.size()), archiveName.data(), archiveName.size()) != 0)
			{
				return false;
			}

			// Only bypass when every ROM image the boot loader requests is present, as the boot
			// state machine polls forever waiting for a file that never opens.
			const std::wstring directory = UTF8ToWchar(pathView.substr(0, pathView.size() - 4)); // strip ".par"
			for (size_t i = 0; i < game.rom_file_count; i++)
			{
				const std::wstring romPath = directory + L'/' + game.rom_files[i];
				// game.elf stands in for the program ROM, so the .bin need not exist at all.
				if (i == 0 && m2ftg::ElfRom::IsLoaded())
				{
					continue;
				}
				const DWORD attributes = GetFileAttributesW(romPath.c_str());
				if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				{
					DebugLog("[file] loose-rom: '%ls' is missing, loading the archive as usual\n", romPath.c_str());
					return false;
				}
			}

			DebugLog("[file] loose-rom: hiding '%s', ROM images will load from the extracted directory\n", path);
			return true;
		}

		bool csl_file_access::open(const char* path, sl::handle_t handle)
		{
			if (ShouldBypassRomArchive(path))
			{
				return false;
			}

			if (RomOverride::IsProgramRom(path))
			{
				sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
				if (internalHandle != nullptr)
				{
					internalHandle->m_h_native = reinterpret_cast<decltype(internalHandle->m_h_native)>(RomOverride::MEMORY_BACKED);
					internalHandle->m_file_pointer = 0;
					DebugLog("[file] open '%s' -> served from '%ls' (h=%u)\n", path,
						m2ftg::ElfRom::LoadedPath(), handle.h.m_handle);
					return true;
				}
				return false;
			}

			HANDLE file = CreateFileW(UTF8ToWchar(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

			// TEMP diagnostic: trace every host file open (loader-loop investigation)
			DebugLog("[file] open '%s' -> %s (h=%u)\n", path,
				file == INVALID_HANDLE_VALUE ? "FAIL" : "ok", handle.h.m_handle);

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
			if (RomOverride::IsMemoryBacked(internalHandle))
			{
				internalHandle->m_h_native = reinterpret_cast<decltype(internalHandle->m_h_native)>(INVALID_HANDLE_VALUE);
				internalHandle->m_file_pointer = 0;
				return true;
			}
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
			if (RomOverride::IsProgramRom(path))
			{
				return true;
			}
			const bool exists = GetFileAttributesW(UTF8ToWchar(path).c_str()) != INVALID_FILE_ATTRIBUTES;
			// TEMP diagnostic (loader-loop investigation)
			DebugLog("[file] is_exist '%s' -> %d\n", path, exists ? 1 : 0);
			return exists;
		}

		int64_t csl_file_access::read(sl::handle_t handle, void* buffer, unsigned int size)
		{
			int64_t result = -1;
			sl::file_handle_internal_t* internalHandle = sl::file_handle_instance(handle);
			if (RomOverride::IsMemoryBacked(internalHandle))
			{
				return RomOverride::Read(internalHandle, buffer, size);
			}
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
			if (RomOverride::IsMemoryBacked(internalHandle))
			{
				return m2ftg::ElfRom::ImageSize();
			}
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
			if (RomOverride::IsMemoryBacked(internalHandle))
			{
				return RomOverride::Seek(internalHandle, offset, seekMode);
			}
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
			DebugLog("[file] archive::open '%s' -> false (stub)\n", path != nullptr ? path : "(null)");
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
			uint32_t* const lock = sl::sync_archive_condvar();
			sl::archive_lock_wlock(lock);
			csl_archive* archive = sl::handle_instance<csl_archive>(handle, 6);
			if (archive != nullptr)
			{
				archive->add_ref();
			}
			sl::archive_lock_wunlock(lock);
			return archive;
		}
	}

