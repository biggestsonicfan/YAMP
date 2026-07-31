#include "sys_util.h"

#include "sl.h"
#include "../../YAMPGeneral.h"

#include "../../wil/resource.h"

namespace fs = std::filesystem;

namespace pxd
{
		// One shared data folder for every game (portable mode, next to YAMP.exe): the save
		// file carries the game tag so StF and FV don't clobber each other's SRAM.
		static fs::path SaveFilePath()
		{
			return gGeneral.GetDataPath() / (std::string(u8"system_") + gGeneral.GetGameTag() + u8".dat");
		}

		struct sys_util_save_header
		{
			uint32_t version; // 100
			std::byte unk[8];
			uint32_t size;
		};
		static_assert(sizeof(sys_util_save_header) == 16);

		bool sys_util_check_enable_storage(int port)
		{
			// TODO: Implement properly
			return true;
		}

		void sys_util_start_load_systemdata_task(int port, void* buf, unsigned int buf_size, bool autoload, bool create, void(*cbfunc)(bool success, bool creator))
		{
			bool cbSuccess = false;
			auto cbOnExit = wil::scope_exit([&] {
				if (cbfunc != nullptr)
					cbfunc(cbSuccess, create);
				});

			// TODO: Do we need to use the port parameter?
			// TODO: Should we call the callback if 'create' is set to off and the file doesn't exist?
			const auto utf8PathToFile = SaveFilePath().u8string();
			sl::handle_t file = create ? sl::file_create(utf8PathToFile.c_str()) : sl::file_open(utf8PathToFile.c_str());
			if (file == sl::INVALID_HANDLE) return;

			sys_util_save_header header;
			if (sl::file_read(file, &header, sizeof(header)) == sizeof(header))
			{
				// TODO: What about the unknown fields? Should the size check be == and not <=?
				if (header.version == 100 && header.size <= buf_size)
				{
					cbSuccess = sl::file_read(file, buf, header.size) == header.size;
				}
			}
			sl::file_close(file);
		}

		void sys_util_start_save_systemdata_task(int port, const void* buf, unsigned int buf_size, bool autosave, void(*cbfunc)(bool success, bool unknown))
		{
			bool cbSuccess = false;
			auto cbOnExit = wil::scope_exit([&] {
				if (cbfunc != nullptr)
					cbfunc(cbSuccess, false);
				});

			// Create the entire directory structure if needed. NOTE: create_directories returns
			// false (with a clear error code) when the directory already exists, so only a real
			// error may abort the save.
			std::error_code ec;
			fs::create_directories(gGeneral.GetDataPath(), ec);
			if (ec) return;

			const auto utf8PathToFile = SaveFilePath().u8string();
			sl::handle_t file = sl::file_create(utf8PathToFile.c_str());
			if (file == sl::INVALID_HANDLE) return;

			sys_util_save_header header{};
			header.version = 100;
			header.size = buf_size;
			if (sl::file_write(file, &header, sizeof(header)) == sizeof(header))
			{
				cbSuccess = sl::file_write(file, buf, buf_size) == buf_size;
			}
			sl::file_close(file);
		}

		bool sys_util_is_enter_circle()
		{
			return gGeneral.GetSettings()->m_circleConfirm;
		}

	}

