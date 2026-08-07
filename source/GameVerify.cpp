// GameVerify.cpp — see GameVerify.h for what each of the two checks is for and why they use
// different methods.
//
// The hashes and PE identities below were taken from a live Steam install of Lost Judgment
// (runtime/media/, game version 1.0.0.12, module DLLs built 2022-11-21). To add a build:
// hash the file with `Get-FileHash <file> -Algorithm SHA256` and read TimeDateStamp/
// SizeOfImage out of its PE header, then append an entry — the tables are lists, so several
// builds of the same game can be known-good at once.

#include "GameVerify.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <vector>

#include "DebugLog.h"
#include "wil/resource.h"

namespace fs = std::filesystem;

namespace Verify
{
	namespace
	{
		// ---- Known-good arcade module DLLs ------------------------------------------------
		struct KnownModuleBuild
		{
			const char* sha256;    // uppercase hex
			uint64_t size;
			uint32_t timestamp;    // PE FileHeader.TimeDateStamp, for the log/About panel
			const char* label;
		};

		struct ModuleEntry
		{
			YAMPGeneral::GameId id;
			const KnownModuleBuild* builds;
			size_t buildCount;
			// Builds YAMP explicitly cannot host, recognised by timestamp alone (these predate
			// the hash table — they are the pre-update DLLs the old check in LJHost rejected).
			// Matching one produces "update your game" instead of the generic unknown-build text.
			const uint32_t* outdatedTimestamps;
			size_t outdatedCount;
		};

		// Sonic the Fighters ships in two titles. Both builds are hostable: the ROM, the sound
		// archives and the shader archive are byte-identical between them, and the module protocol
		// is unchanged (execute_info 0x1760, config 0x100C) — only the host DLL was recompiled.
		// The per-build RVAs and the two pxd context sizes that differ live in m2ftg/ModuleBuild.h
		// and the tables that key off it.
		constexpr KnownModuleBuild STF_BUILDS[] = {
			{ "DF7FE561ED3B2954066CC138C179B9DD5CE2F65D0EC4A4104A7AD161236A07BB", 2086896, 0x637B11A3,
				"Lost Judgment (2022-11-21)" },
			{ "991383EB33CA7E119425C69C6FBB48C9B09D3EC027BBC0264D505D449B6CB26F", 2132984, 0x65647FB5,
				"Like a Dragon Gaiden (2023-11-27)" },
		};
		constexpr KnownModuleBuild FV_BUILDS[] = {
			{ "0F1DAD193533C4C250EDA54BD3C5D63751D1E3431E4DD8F702010777B9754EC0", 2078704, 0x637B119D,
				"Lost Judgment (2022-11-21)" },
		};
		constexpr KnownModuleBuild MR_BUILDS[] = {
			{ "9FCBAE38DD7DC04DD2B29BF09576594FA0ED5A6089D82272E200E7A1C59A9623", 2699248, 0x637B1190,
				"Lost Judgment (2022-11-21)" },
		};
		constexpr KnownModuleBuild VF5FS_LJ_BUILDS[] = {
			{ "2A83D302768D7AFA5F2EF0B0D0481CF281F081FCE1D9CCFF3CA34EE94358E36B", 6152688, 0x637B1259,
				"Lost Judgment (2022-11-21)" },
		};
		// Yakuza: Like a Dragon ships its module in runtime/media/m2ftg/ (NOT the "vf2" folder
		// this host's LoadDLL expects — the DLL has to be assembled into one with its rom/ and
		// w64/ folders, which is why the launcher's candidate paths look the way they do).
		constexpr KnownModuleBuild VF2_BUILDS[] = {
			{ "3B3AF23E2075C84F996F7194BE477600C1D887965311702272A324FCBE7B1818", 1648128, 0x601763D1,
				"Yakuza: Like a Dragon (2021-02-01)" },
		};
		// YLAD's OWN VF5FS (runtime/media/vf5fs/vf5fs-pxd-w64-retail.dll) — a third build of that
		// game, distinct from both Yakuza 6's "vf5fs-pxd-w64-Retail Steam.dll" and Lost Judgment's
		// DX12 one. Like a Dragon is Yakuza *7*, so it gets its own host in source/vf5fs/YLAD
		// (mirroring source/m2ftg/YLAD for VF2) rather than anything under vf5fs/Y6.
		constexpr KnownModuleBuild VF5FS_YLAD_BUILDS[] = {
			{ "A022DDD4185489146B9D757B8E8590467C5B1F18A1139144FCCA3676DB359B69", 5946880, 0x601766BE,
				"Yakuza: Like a Dragon (2021-02-01)" },
		};

		// Yakuza 6's own VF5FS, the DX11 original: <install>/vf5fs/ next to vf5fs_media/. Note the
		// flat install layout — Yakuza6.exe sits at the install ROOT, not under runtime/media/ the
		// way Lost Judgment and Like a Dragon put theirs.
		constexpr KnownModuleBuild VF5FS_BUILDS[] = {
			{ "48787216D767F3566ACA129D848A7931E6D2797A59D01216D6F92C24702ED654", 5389312, 0x60AB8422,
				"Yakuza 6: The Song of Life (2021-05-24)" },
		};

		// Yakuza Kiwami 2 (GOG), <install>/m2ftg/. Both modules were built one second apart on
		// 2019-09-30 — the same engine build, which is why they share one host.
		constexpr KnownModuleBuild VF2_K2_BUILDS[] = {
			{ "4D9473F052822CFA1EA621F7C10D0CE46A0B18178533EFCB325B13E0C03DCB6A", 1614848, 0x5D91BBC9,
				"Yakuza Kiwami 2 GOG (2019-09-30)" },
		};

		// Like a Dragon Gaiden's Model 3 emulator, runtime/media/pre3/. ONE module for both
		// Fighting Vipers 2 and Sega Racing Classic 2 (and four more games it has no data
		// for), so the two GameIds below share this table — which game runs is a config byte,
		// not a different file. Built 2024-03-10, four months after the same title's m2ftg
		// modules, but the same pxd generation as them.
		constexpr KnownModuleBuild PRE3_BUILDS[] = {
			{ "B44ED5A7D95076EDF84F48FC02773EB42F1830FEBDF32DEA33E4F0C639FDB98E", 1665008, 0x65EE7212,
				"Like a Dragon Gaiden (2024-03-10)" },
		};

		// "omg" = Operation Moon Gate, i.e. Virtual On.
		constexpr KnownModuleBuild VON_K2_BUILDS[] = {
			{ "99AD0D31AE5949F6C5F521119FC9E26FF5CA602B1629583CBF478D1FCDC4D39C", 4734976, 0x5D91BBCA,
				"Yakuza Kiwami 2 GOG (2019-09-30)" },
		};

		// Pre-update module builds YAMP cannot host, recognised by timestamp alone. These three
		// came from the old post-load checks that used to live in m2ftg::LoadDLL and
		// vf5fs::Y6::LoadDLL — both of which said "update your game to the latest version", which
		// is the wording matching one of these keeps. An unknown hash is blocked either way; this
		// only buys the more specific message. VF2 has no known-bad list of its own.
		constexpr uint32_t OUTDATED_TIMESTAMPS[] = { 0x603E22E3, 0x606D6969, 0x6075A65A };

		constexpr ModuleEntry MODULE_ENTRIES[] = {
			{ YAMPGeneral::GameId::StF, STF_BUILDS, std::size(STF_BUILDS),
				OUTDATED_TIMESTAMPS, std::size(OUTDATED_TIMESTAMPS) },
			{ YAMPGeneral::GameId::FV, FV_BUILDS, std::size(FV_BUILDS),
				OUTDATED_TIMESTAMPS, std::size(OUTDATED_TIMESTAMPS) },
			{ YAMPGeneral::GameId::MR, MR_BUILDS, std::size(MR_BUILDS),
				OUTDATED_TIMESTAMPS, std::size(OUTDATED_TIMESTAMPS) },
			{ YAMPGeneral::GameId::VF5FS_LJ, VF5FS_LJ_BUILDS, std::size(VF5FS_LJ_BUILDS),
				OUTDATED_TIMESTAMPS, std::size(OUTDATED_TIMESTAMPS) },
			{ YAMPGeneral::GameId::VF2, VF2_BUILDS, std::size(VF2_BUILDS), nullptr, 0 },
			{ YAMPGeneral::GameId::VF5FS_YLAD, VF5FS_YLAD_BUILDS, std::size(VF5FS_YLAD_BUILDS), nullptr, 0 },
			{ YAMPGeneral::GameId::VF2_K2, VF2_K2_BUILDS, std::size(VF2_K2_BUILDS), nullptr, 0 },
			{ YAMPGeneral::GameId::VON_K2, VON_K2_BUILDS, std::size(VON_K2_BUILDS), nullptr, 0 },
			{ YAMPGeneral::GameId::FV2, PRE3_BUILDS, std::size(PRE3_BUILDS), nullptr, 0 },
			{ YAMPGeneral::GameId::SRC2, PRE3_BUILDS, std::size(PRE3_BUILDS), nullptr, 0 },
			{ YAMPGeneral::GameId::VF5FS, VF5FS_BUILDS, std::size(VF5FS_BUILDS),
				OUTDATED_TIMESTAMPS, std::size(OUTDATED_TIMESTAMPS) },
		};

		// ---- Known parent-game executables ------------------------------------------------
		struct KnownExeBuild
		{
			uint32_t timestamp;    // PE FileHeader.TimeDateStamp
			uint32_t sizeOfImage;  // PE OptionalHeader.SizeOfImage
			uint64_t fileSize;
			const char* label;
		};

		// One title that can supply a given arcade game. Most games have exactly one, but an
		// arcade machine reused across releases has several — Sonic the Fighters ships in both
		// Lost Judgment and Like a Dragon Gaiden, and owning EITHER is enough.
		struct ParentTitle
		{
			const wchar_t* exeName;
			const char* exeNameUtf8;   // same name, for the UI and message text
			// Where the executable sits relative to an install root, for the folders we probe
			// that are roots rather than the module's own directory.
			const wchar_t* const* subdirs;
			size_t subdirCount;
			const KnownExeBuild* builds;
			size_t buildCount;
		};

		struct ParentEntry
		{
			YAMPGeneral::GameId id;
			const ParentTitle* titles;
			size_t titleCount;
		};

		// Both parent games keep their executable in runtime/media/, so one subdir list serves.
		constexpr const wchar_t* RUNTIME_MEDIA_SUBDIRS[] = { L"runtime\\media" };
		constexpr KnownExeBuild LJ_EXE_BUILDS[] = {
			{ 0x641412B9, 0x1AE88000, 422131184, "Lost Judgment 1.0.0.12 (Steam)" },
		};
		constexpr KnownExeBuild YLAD_EXE_BUILDS[] = {
			{ 0x6141B906, 0x18C91000, 387532936, "Yakuza: Like a Dragon (Steam, 2021-09-15)" },
		};
		// Yakuza 6 keeps its executable at the install root, so it needs no subdir list: the search
		// already probes the module folder's parent (<install>/vf5fs -> <install>) and every Steam
		// install root directly.
		// Kiwami 2 is a GOG install and puts its executable at the install root, like Yakuza 6.
		constexpr KnownExeBuild K2_EXE_BUILDS[] = {
			{ 0x644A6712, 0x0416A000, 44795392, "Yakuza Kiwami 2 (GOG, 2023-04-27)" },
		};
		constexpr KnownExeBuild Y6_EXE_BUILDS[] = {
			{ 0x60AB8368, 0x039DC000, 38833000, "Yakuza 6: The Song of Life (Steam, 2021-05-24)" },
		};

		// Like a Dragon Gaiden, which ships stf/vf2/mr modules in runtime/media/m2ftg/ like
		// Lost Judgment does. The executable is lowercase on disk; the search is case-insensitive
		// on Windows, but the name is spelled as shipped so log lines match what the user sees.
		constexpr KnownExeBuild GAIDEN_EXE_BUILDS[] = {
			{ 0x67A1DC3D, 0x18D15000, 387517984, "Like a Dragon Gaiden (Steam)" },
		};

		// Sonic the Fighters is the first game YAMP can source from more than one title: Lost
		// Judgment and Like a Dragon Gaiden ship byte-identical ROM and asset files, so owning
		// EITHER is proof of ownership and the module that gets loaded decides which build runs.
		constexpr ParentTitle STF_TITLES[] = {
			{ L"LostJudgment.exe", "LostJudgment.exe",
				RUNTIME_MEDIA_SUBDIRS, std::size(RUNTIME_MEDIA_SUBDIRS),
				LJ_EXE_BUILDS, std::size(LJ_EXE_BUILDS) },
			{ L"likeadragongaiden.exe", "likeadragongaiden.exe",
				RUNTIME_MEDIA_SUBDIRS, std::size(RUNTIME_MEDIA_SUBDIRS),
				GAIDEN_EXE_BUILDS, std::size(GAIDEN_EXE_BUILDS) },
		};
		// The Model 3 games come only from Like a Dragon Gaiden — Lost Judgment ships no pre3
		// module at all, so unlike Sonic the Fighters there is no second source for them.
		constexpr ParentTitle GAIDEN_TITLES[] = {
			{ L"likeadragongaiden.exe", "likeadragongaiden.exe",
				RUNTIME_MEDIA_SUBDIRS, std::size(RUNTIME_MEDIA_SUBDIRS),
				GAIDEN_EXE_BUILDS, std::size(GAIDEN_EXE_BUILDS) },
		};
		// FV has no Gaiden module (Gaiden ships fv_rom.par but no fv DLL), and MR's Gaiden build
		// has not been brought up yet, so both stay Lost-Judgment-only.
		constexpr ParentTitle LJ_TITLES[] = {
			{ L"LostJudgment.exe", "LostJudgment.exe",
				RUNTIME_MEDIA_SUBDIRS, std::size(RUNTIME_MEDIA_SUBDIRS),
				LJ_EXE_BUILDS, std::size(LJ_EXE_BUILDS) },
		};
		constexpr ParentTitle YLAD_TITLES[] = {
			{ L"YakuzaLikeADragon.exe", "YakuzaLikeADragon.exe",
				RUNTIME_MEDIA_SUBDIRS, std::size(RUNTIME_MEDIA_SUBDIRS),
				YLAD_EXE_BUILDS, std::size(YLAD_EXE_BUILDS) },
		};
		constexpr ParentTitle K2_TITLES[] = {
			{ L"YakuzaKiwami2.exe", "YakuzaKiwami2.exe",
				nullptr, 0, K2_EXE_BUILDS, std::size(K2_EXE_BUILDS) },
		};
		constexpr ParentTitle Y6_TITLES[] = {
			{ L"Yakuza6.exe", "Yakuza6.exe",
				nullptr, 0, Y6_EXE_BUILDS, std::size(Y6_EXE_BUILDS) },
		};

		constexpr ParentEntry PARENT_ENTRIES[] = {
			{ YAMPGeneral::GameId::StF, STF_TITLES, std::size(STF_TITLES) },
			{ YAMPGeneral::GameId::FV, LJ_TITLES, std::size(LJ_TITLES) },
			{ YAMPGeneral::GameId::MR, LJ_TITLES, std::size(LJ_TITLES) },
			{ YAMPGeneral::GameId::VF5FS_LJ, LJ_TITLES, std::size(LJ_TITLES) },
			{ YAMPGeneral::GameId::VF2, YLAD_TITLES, std::size(YLAD_TITLES) },
			{ YAMPGeneral::GameId::VF5FS_YLAD, YLAD_TITLES, std::size(YLAD_TITLES) },
			{ YAMPGeneral::GameId::VF2_K2, K2_TITLES, std::size(K2_TITLES) },
			{ YAMPGeneral::GameId::VON_K2, K2_TITLES, std::size(K2_TITLES) },
			{ YAMPGeneral::GameId::FV2, GAIDEN_TITLES, std::size(GAIDEN_TITLES) },
			{ YAMPGeneral::GameId::SRC2, GAIDEN_TITLES, std::size(GAIDEN_TITLES) },
			{ YAMPGeneral::GameId::VF5FS, Y6_TITLES, std::size(Y6_TITLES) },
		};

		const ModuleEntry* FindModuleEntry(YAMPGeneral::GameId id)
		{
			for (const ModuleEntry& entry : MODULE_ENTRIES)
			{
				if (entry.id == id) return &entry;
			}
			return nullptr;
		}

		const ParentEntry* FindParentEntry(YAMPGeneral::GameId id)
		{
			for (const ParentEntry& entry : PARENT_ENTRIES)
			{
				if (entry.id == id) return &entry;
			}
			return nullptr;
		}

		// ---- File readers -----------------------------------------------------------------

		// SHA-256 of a whole file, as uppercase hex. Streams the file in 64 KiB chunks so a
		// multi-megabyte module never needs a full copy in memory.
		bool Sha256File(const fs::path& path, std::string& hexOut, uint64_t& sizeOut)
		{
			wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!file) return false;

			LARGE_INTEGER fileSize {};
			if (!GetFileSizeEx(file.get(), &fileSize)) return false;
			sizeOut = static_cast<uint64_t>(fileSize.QuadPart);

			BCRYPT_ALG_HANDLE alg = nullptr;
			if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
			{
				return false;
			}
			auto closeAlg = wil::scope_exit([&alg] { BCryptCloseAlgorithmProvider(alg, 0); });

			BCRYPT_HASH_HANDLE hash = nullptr;
			if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) return false;
			auto destroyHash = wil::scope_exit([&hash] { BCryptDestroyHash(hash); });

			std::vector<uint8_t> buffer(64 * 1024);
			for (;;)
			{
				DWORD read = 0;
				if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
				{
					return false;
				}
				if (read == 0) break;
				if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0))) return false;
			}

			uint8_t digest[32] {};
			if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) return false;

			hexOut.clear();
			hexOut.reserve(sizeof(digest) * 2);
			for (const uint8_t byte : digest)
			{
				char pair[3];
				sprintf_s(pair, "%02X", byte);
				hexOut += pair;
			}
			return true;
		}

		struct PeIdentity
		{
			bool valid = false;
			uint32_t timestamp = 0;
			uint32_t sizeOfImage = 0;
			uint64_t fileSize = 0;
		};

		// Reads a PE image's identity straight out of the file's headers, WITHOUT mapping or
		// loading it — a few hundred bytes even for a 400 MB executable.
		PeIdentity ReadPeIdentity(const fs::path& path)
		{
			PeIdentity identity;

			std::ifstream file(path, std::ios::binary);
			if (!file) return identity;

			IMAGE_DOS_HEADER dosHeader {};
			file.read(reinterpret_cast<char*>(&dosHeader), sizeof(dosHeader));
			if (!file || dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew < 0) return identity;

			file.seekg(dosHeader.e_lfanew, std::ios::beg);
			IMAGE_NT_HEADERS64 ntHeaders {};
			file.read(reinterpret_cast<char*>(&ntHeaders), sizeof(ntHeaders));
			if (!file || ntHeaders.Signature != IMAGE_NT_SIGNATURE) return identity;

			std::error_code ec;
			const uintmax_t size = fs::file_size(path, ec);
			if (!ec) identity.fileSize = static_cast<uint64_t>(size);

			identity.valid = true;
			identity.timestamp = ntHeaders.FileHeader.TimeDateStamp;
			identity.sizeOfImage = ntHeaders.OptionalHeader.SizeOfImage;
			return identity;
		}

		// ---- Parent executable search -----------------------------------------------------

		void AddDir(std::vector<fs::path>& dirs, const fs::path& raw)
		{
			if (raw.empty()) return;

			std::error_code ec;
			fs::path dir = fs::weakly_canonical(raw, ec);
			if (ec || dir.empty()) dir = raw;
			if (!fs::is_directory(dir, ec) || ec) return;
			for (const fs::path& existing : dirs)
			{
				if (existing == dir) return;
			}
			dirs.push_back(std::move(dir));
		}

		// Every folder that could hold the parent executable, nearest first: the module's own
		// folder and its two parents (LJ: runtime/media/m2ftg -> runtime/media), then YAMP.exe's
		// folder and the CWD, then every Steam install. Roots also get the entry's subdirs
		// appended, since a Steam install keeps the executable in runtime/media/.
		std::vector<fs::path> ParentSearchDirs(const ParentTitle& entry, const fs::path& dllDir)
		{
			std::vector<fs::path> dirs;

			auto addRootAndSubdirs = [&dirs, &entry](const fs::path& root) {
				AddDir(dirs, root);
				for (size_t i = 0; i < entry.subdirCount; i++)
				{
					AddDir(dirs, root / entry.subdirs[i]);
				}
			};

			if (!dllDir.empty())
			{
				AddDir(dirs, dllDir);
				AddDir(dirs, dllDir.parent_path());
				AddDir(dirs, dllDir.parent_path().parent_path());
				addRootAndSubdirs(dllDir.parent_path().parent_path());
			}

			wchar_t exePath[MAX_PATH];
			if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
			{
				addRootAndSubdirs(fs::path(exePath).parent_path());
			}

			{
				const DWORD size = GetCurrentDirectoryW(0, nullptr);
				auto buf = std::make_unique<wchar_t[]>(size);
				if (GetCurrentDirectoryW(size, buf.get()) != 0)
				{
					addRootAndSubdirs(buf.get());
				}
			}

			// Every installed game on the system, Steam AND GOG (see GameInstallRoots).
			for (const InstallRoot& install : GameInstallRoots())
			{
				addRootAndSubdirs(install.path);
			}
			return dirs;
		}

		// ---- Last results, for the About panel --------------------------------------------
		ModuleResult g_lastModule;
		ParentResult g_lastParent;

		std::wstring FormatModuleFailure(const fs::path& dllPath, const ModuleResult& result)
		{
			const std::wstring name = dllPath.filename().wstring();
			const std::wstring parent = UTF8ToWchar(gGeneral.GetParentGameName());

			switch (result.status)
			{
			case ModuleStatus::Unreadable:
				return name + L" could not be read.\n\nCheck that the file exists and is not locked "
					L"by another program.";

			case ModuleStatus::OutdatedBuild:
				return name + L" is of an unsupported version!\n\nPlease update your copy of " + parent +
					L" to the latest version.";

			default:
				return name + L" does not match any build of " + parent + L" that YAMP supports, so it "
					L"cannot be hosted safely.\n\nThis file:\n  " + UTF8ToWchar(result.sha256) +
					L"\n\nExpected:\n  " + UTF8ToWchar(result.expectedSha256 ? result.expectedSha256 : "") +
					L"\n\nUse the original, unmodified DLL from your own installation of " + parent +
					L". If the game has been updated, YAMP needs an update to support the new build.";
			}
		}

		std::wstring FormatParentFailure(const ParentEntry& entry)
		{
			const std::wstring parent = UTF8ToWchar(gGeneral.GetParentGameName());
			// A game that ships in several titles lists all of them, so the message never tells
			// someone who owns Like a Dragon Gaiden to go and buy Lost Judgment.
			std::wstring exeNames = entry.titles[0].exeName;
			for (size_t i = 1; i < entry.titleCount; i++)
			{
				exeNames += (i + 1 == entry.titleCount) ? L" or " : L", ";
				exeNames += entry.titles[i].exeName;
			}
			return parent + L" could not be found.\n\nYAMP looked for " + exeNames +
				L" next to the arcade module, next to YAMP.exe, in every folder beside YAMP.exe, and "
				L"in every Steam and GOG install on this system, and found no copy of it.\n\nYAMP does "
				L"not redistribute any game files: you must own " + parent + L" to play its arcade "
				L"games. Install it through Steam or GOG, or put YAMP.exe inside your existing "
				L"installation — or alongside it, with the game in a folder next to YAMP.exe.";
		}
	}

	std::vector<fs::path> SteamLibraryRoots()
	{
		std::vector<fs::path> bases;

		wchar_t steamPath[512];
		DWORD steamPathSize = sizeof(steamPath);
		if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
			RRF_RT_REG_SZ, nullptr, steamPath, &steamPathSize) == ERROR_SUCCESS)
		{
			AddDir(bases, steamPath);
		}
		AddDir(bases, L"C:\\Program Files (x86)\\Steam");
		AddDir(bases, L"C:\\Program Files\\Steam");

		// Secondary libraries: every "path" value in each base's libraryfolders.vdf.
		const size_t primaryCount = bases.size();
		for (size_t i = 0; i < primaryCount; i++)
		{
			std::ifstream vdf(bases[i] / L"steamapps" / L"libraryfolders.vdf");
			if (!vdf) continue;
			std::stringstream buffer;
			buffer << vdf.rdbuf();
			const std::string text = buffer.str();

			size_t pos = 0;
			while ((pos = text.find("\"path\"", pos)) != std::string::npos)
			{
				pos += 6;
				const size_t open = text.find('"', pos);
				if (open == std::string::npos) break;
				const size_t close = text.find('"', open + 1);
				if (close == std::string::npos) break;

				const std::string value = text.substr(open + 1, close - open - 1);
				std::string unescaped;
				unescaped.reserve(value.size());
				for (size_t j = 0; j < value.size(); j++)
				{
					if (value[j] == '\\' && j + 1 < value.size() && value[j + 1] == '\\')
					{
						unescaped += '\\';
						j++;
					}
					else
					{
						unescaped += value[j];
					}
				}
				AddDir(bases, fs::u8path(unescaped));
				pos = close + 1;
			}
		}
		return bases;
	}

	std::vector<InstallRoot> GameInstallRoots()
	{
		std::vector<InstallRoot> roots;
		auto add = [&roots](const fs::path& raw, const std::string& label) {
			if (raw.empty()) return;
			std::error_code ec;
			fs::path dir = fs::weakly_canonical(raw, ec);
			if (ec || dir.empty()) dir = raw;
			if (!fs::is_directory(dir, ec) || ec) return;
			for (const InstallRoot& existing : roots)
			{
				if (existing.path == dir) return;
			}
			roots.push_back({ std::move(dir), label });
		};

		// ---- Steam: every install under each library's steamapps/common ----------------
		for (const fs::path& library : SteamLibraryRoots())
		{
			std::error_code ec;
			for (const fs::directory_entry& install :
				fs::directory_iterator(library / L"steamapps" / L"common", ec))
			{
				std::error_code installEc;
				if (!install.is_directory(installEc) || installEc) continue;
				add(install.path(), "Steam: " + WcharToUTF8(install.path().filename().wstring()));
			}
		}

		// ---- GOG: the registry names every installed game and its exact path ------------
		// HKLM\SOFTWARE\[WOW6432Node\]GOG.com\Games\<productId>\{path, gameName}. Unlike Steam
		// there is no library to walk: each subkey IS an install. Both registry views are tried
		// because a 32-bit Galaxy writes under WOW6432Node while a 64-bit one may not.
		for (const wchar_t* gamesKey : { L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", L"SOFTWARE\\GOG.com\\Games" })
		{
			HKEY games = nullptr;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, gamesKey, 0, KEY_READ, &games) != ERROR_SUCCESS)
			{
				continue;
			}

			for (DWORD index = 0; ; index++)
			{
				wchar_t subKeyName[256];
				DWORD subKeyLen = static_cast<DWORD>(std::size(subKeyName));
				if (RegEnumKeyExW(games, index, subKeyName, &subKeyLen,
					nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
				{
					break;
				}

				wchar_t path[512] {};
				DWORD pathSize = sizeof(path);
				if (RegGetValueW(games, subKeyName, L"path", RRF_RT_REG_SZ, nullptr,
					path, &pathSize) != ERROR_SUCCESS)
				{
					continue;
				}

				wchar_t name[256] {};
				DWORD nameSize = sizeof(name);
				const bool haveName = RegGetValueW(games, subKeyName, L"gameName", RRF_RT_REG_SZ,
					nullptr, name, &nameSize) == ERROR_SUCCESS;

				add(path, "GOG: " + WcharToUTF8(haveName ? name : subKeyName));
			}
			RegCloseKey(games);
		}

		// Fallback for a Galaxy install whose registry entries are missing: scan its own Games
		// folder, wherever the client reports it lives (plus the default locations).
		{
			std::vector<fs::path> galaxyRoots;
			wchar_t clientPath[512];
			DWORD clientSize = sizeof(clientPath);
			if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\GalaxyClient\\paths",
				L"client", RRF_RT_REG_SZ, nullptr, clientPath, &clientSize) == ERROR_SUCCESS)
			{
				galaxyRoots.emplace_back(clientPath);
			}
			galaxyRoots.emplace_back(L"C:\\Program Files (x86)\\GOG Galaxy");
			galaxyRoots.emplace_back(L"C:\\Program Files\\GOG Galaxy");

			for (const fs::path& galaxy : galaxyRoots)
			{
				std::error_code ec;
				for (const fs::directory_entry& install : fs::directory_iterator(galaxy / L"Games", ec))
				{
					std::error_code installEc;
					if (!install.is_directory(installEc) || installEc) continue;
					add(install.path(), "GOG: " + WcharToUTF8(install.path().filename().wstring()));
				}
			}
		}

		// ---- Portable: YAMP.exe's own folder, and every folder sitting next to it -------
		// For anyone whose games are not registered with Steam or GOG at all (a moved install,
		// a second copy, a manually extracted one). YAMP.exe's folder was already probed as a
		// single directory by the parent-executable search, but that only finds a game whose
		// executable sits DIRECTLY beside YAMP.exe. Enumerating the folder's immediate
		// subdirectories the same way steamapps/common is walked is what makes the natural
		// layout work too:
		//
		//     <somewhere>\YAMP.exe
		//     <somewhere>\Yakuza Kiwami 2\YakuzaKiwami2.exe        <- found by this
		//     <somewhere>\Lost Judgment\runtime\media\...           <- and this, via the
		//                                                             entry's subdir list
		//
		// One level only, and only directories: this runs on every boot, and a deep walk of an
		// arbitrary folder is exactly the kind of thing that makes a launcher feel slow.
		// It feeds game DISCOVERY as well as the ownership check, since the launcher iterates
		// these same roots.
		{
			wchar_t exePath[MAX_PATH];
			if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
			{
				const fs::path exeDir = fs::path(exePath).parent_path();
				add(exeDir, "Next to YAMP");

				std::error_code ec;
				for (const fs::directory_entry& sibling : fs::directory_iterator(exeDir, ec))
				{
					std::error_code siblingEc;
					if (!sibling.is_directory(siblingEc) || siblingEc) continue;
					add(sibling.path(),
						"Next to YAMP: " + WcharToUTF8(sibling.path().filename().wstring()));
				}
			}
		}

		return roots;
	}

	ModuleResult CheckModule(YAMPGeneral::GameId id, const fs::path& dllPath)
	{
		ModuleResult result;

		const ModuleEntry* entry = FindModuleEntry(id);
		if (entry == nullptr) return result;  // NotChecked: no table for this game yet

		result.expectedSha256 = entry->builds[0].sha256;

		const PeIdentity identity = ReadPeIdentity(dllPath);
		result.timestamp = identity.timestamp;

		if (!Sha256File(dllPath, result.sha256, result.size) || !identity.valid)
		{
			result.status = ModuleStatus::Unreadable;
			return result;
		}

		for (size_t i = 0; i < entry->buildCount; i++)
		{
			const KnownModuleBuild& build = entry->builds[i];
			if (result.size == build.size && result.sha256 == build.sha256)
			{
				result.status = ModuleStatus::Verified;
				result.buildLabel = build.label;
				return result;
			}
		}

		// Not a known build. A recognised pre-update timestamp gets the more specific verdict.
		result.status = ModuleStatus::UnknownBuild;
		for (size_t i = 0; i < entry->outdatedCount; i++)
		{
			if (identity.timestamp == entry->outdatedTimestamps[i])
			{
				result.status = ModuleStatus::OutdatedBuild;
				break;
			}
		}
		return result;
	}

	ParentResult CheckParentGame(YAMPGeneral::GameId id, const fs::path& dllDir)
	{
		ParentResult result;

		const ParentEntry* entry = FindParentEntry(id);
		if (entry == nullptr) return result;  // NotChecked: no table for this game yet

		result.exeName = entry->titles[0].exeNameUtf8;
		result.status = ParentStatus::NotFound;

		// Owning ANY of the titles that ship this arcade game is enough, so keep looking until one
		// verifies. A title that is present but of an unrecognised build is remembered and used
		// only if nothing else verifies, so "I own Gaiden but my Lost Judgment is an odd build"
		// still passes on the strength of the copy that did verify.
		for (size_t t = 0; t < entry->titleCount; t++)
		{
			const ParentTitle& title = entry->titles[t];
			for (const fs::path& dir : ParentSearchDirs(title, dllDir))
			{
				const fs::path candidate = dir / title.exeName;
				const PeIdentity identity = ReadPeIdentity(candidate);
				if (!identity.valid) continue;

				ParentStatus status = ParentStatus::UnknownBuild;
				const char* label = nullptr;
				for (size_t i = 0; i < title.buildCount; i++)
				{
					const KnownExeBuild& build = title.builds[i];
					if (identity.timestamp == build.timestamp && identity.sizeOfImage == build.sizeOfImage
						&& identity.fileSize == build.fileSize)
					{
						status = ParentStatus::Verified;
						label = build.label;
						break;
					}
				}

				// The nearest copy of THIS title wins even when it is an unrecognised build: a
				// second, older install elsewhere should not decide the verdict for this one.
				if (result.status == ParentStatus::NotFound || status == ParentStatus::Verified)
				{
					result.exePath = candidate;
					result.exeName = title.exeNameUtf8;
					result.status = status;
					result.buildLabel = label;
				}
				break;
			}
			if (result.status == ParentStatus::Verified) break;
		}
		return result;
	}

	bool CheckBeforeLoad(YAMPGeneral::GameId id, const fs::path& dllPath)
	{
		g_lastModule = CheckModule(id, dllPath);
		g_lastParent = CheckParentGame(id, dllPath.parent_path());

		// Report the DLL's build stamp even when the check fails, so the About panel and the
		// log identify whatever the user actually has.
		if (g_lastModule.timestamp != 0)
		{
			gGeneral.SetDLLTimestamp(g_lastModule.timestamp);
		}

		// The build LABEL is the useful half of this line once a game ships in more than one
		// title: "Verified" alone does not say whether Sonic the Fighters came out of Lost
		// Judgment or Like a Dragon Gaiden, and those are different DLLs with different RVAs.
		DebugLog("[verify] module %s: %s%s%s (sha256 %s, %llu bytes)\n",
			WcharToUTF8(dllPath.filename().wstring()).c_str(), Describe(g_lastModule.status),
			g_lastModule.buildLabel ? " — " : "", g_lastModule.buildLabel ? g_lastModule.buildLabel : "",
			g_lastModule.sha256.empty() ? "n/a" : g_lastModule.sha256.c_str(),
			static_cast<unsigned long long>(g_lastModule.size));
		DebugLog("[verify] parent game: %s%s%s (%s)\n", Describe(g_lastParent.status),
			g_lastParent.buildLabel ? " — " : "", g_lastParent.buildLabel ? g_lastParent.buildLabel : "",
			g_lastParent.exePath.empty() ? "not located"
				: WcharToUTF8(g_lastParent.exePath.wstring()).c_str());

		if (g_lastModule.Blocks())
		{
			const std::wstring message = FormatModuleFailure(dllPath, g_lastModule);
			MessageBoxW(nullptr, message.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
			return false;
		}

		if (g_lastParent.Blocks())
		{
			const ParentEntry* entry = FindParentEntry(id);
			if (entry != nullptr)
			{
				const std::wstring message = FormatParentFailure(*entry);
				MessageBoxW(nullptr, message.c_str(), L"Yakuza Arcade Machines Player", MB_ICONERROR | MB_OK);
			}
			return false;
		}

		return true;
	}

	const ModuleResult& LastModuleResult() { return g_lastModule; }
	const ParentResult& LastParentResult() { return g_lastParent; }

	const char* Describe(ModuleStatus status)
	{
		switch (status)
		{
		case ModuleStatus::Verified:      return "Verified";
		case ModuleStatus::UnknownBuild:  return "Unrecognised build";
		case ModuleStatus::OutdatedBuild: return "Outdated build";
		case ModuleStatus::Unreadable:    return "Unreadable";
		default:                          return "Not verified";
		}
	}

	const char* Describe(ParentStatus status)
	{
		switch (status)
		{
		case ParentStatus::Verified:     return "Verified";
		case ParentStatus::UnknownBuild: return "Unrecognised build";
		case ParentStatus::NotFound:     return "Not found";
		default:                         return "Not verified";
		}
	}
}
