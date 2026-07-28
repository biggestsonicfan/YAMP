#pragma once

// LJ-specific (but m2ftg-generic) hosting facts, shared by every Lost Judgment m2ftg module
// YAMP can run through the source/LJ path: Sonic the Fighters and Fighting Vipers. The two
// DLLs are near-identical builds of the same Model 2 emulator engine — same module protocol
// (m2ftg.h), same context sizes (sl 0xF000 / gs 0x388A00 / ct 0x30), same execute_info
// (0x1760) and the same ImportSymbols byte patterns — so the whole hosting path is shared
// and everything that actually differs between the games lives in the GameDesc table below.
//
// The cross-host module structures (m2ftg_config_t / m2ftg_pad_t / m2ftg_execute_info_t)
// stay in m2ftg.h, which the YLAD VF2 host (source/Y6) includes too; this header is only
// for the LJ side.

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "../YAMPGeneral.h"

namespace LJ
{
    // Kept in the LJ::StF namespace (not a new one): the whole LJ hosting path predates FV
    // support and lives there; renaming it would churn every file for no behavioral gain.
    namespace StF
    {
        struct GameDesc
        {
            const wchar_t* dll_name;
            const wchar_t* subdir;           // fallback subfolder next to YAMP.exe
            const char* display_name;
            uint32_t kind;                   // m2ftg_config_t.kind {1=fv, 2=stf}
            // Loose-ROM debug feature (file_access.cpp): the archive to hide and the ROM
            // images the DLL's boot loader requests (paths inside "rom/<archive stem>/").
            const char* rom_archive_name;    // "stf_rom.par" / "fv_rom.par"
            const wchar_t* const* rom_files;
            size_t rom_file_count;
            // i960 emulator globals by DLL RVA, used by InstallRamExecFetch (Patch.cpp):
            // CPU context pointer, opcode dispatch table, work-RAM host base pointer.
            uintptr_t rva_cpu_ctx_ptr;
            uintptr_t rva_opcode_table;
            uintptr_t rva_ram_base_ptr;
        };

        // ROM image lists were read out of each DLL's own path strings ("rom/stf_rom/..." /
        // "rom/fv_rom/..." — note FV splits the EP ROM into rom_ep1/rom_ep2). The i960 RVAs
        // come from each DLL's fetch dispatcher + work-RAM base global (FV values verified
        // against fv-pxd-w64-d3d12_retail.dll in Ghidra).
        inline constexpr const wchar_t* STF_ROM_FILES[] = {
            L"rom_code1.bin", L"rom_data.bin", L"rom_ep.bin", L"rom_pol.bin", L"rom_tex.bin",
        };
        inline constexpr const wchar_t* FV_ROM_FILES[] = {
            L"rom_code1.bin", L"rom_data.bin", L"rom_ep1.bin", L"rom_ep2.bin", L"rom_pol.bin", L"rom_tex.bin",
        };
        inline constexpr GameDesc GAME_STF = {
            L"stf-pxd-w64-d3d12_retail.dll", L"stf", "Sonic the Fighters", 2,
            "stf_rom.par", STF_ROM_FILES, std::size(STF_ROM_FILES),
            0x58A960, 0x168630, 0x8F7CC8,
        };
        inline constexpr GameDesc GAME_FV = {
            L"fv-pxd-w64-d3d12_retail.dll", L"fv", "Fighting Vipers", 1,
            "fv_rom.par", FV_ROM_FILES, std::size(FV_ROM_FILES),
            0x58CF60, 0x166720, 0x8FA2C8,
        };

        inline const GameDesc& CurrentGame()
        {
            return gGeneral.GetGameId() == YAMPGeneral::GameId::FV ? GAME_FV : GAME_STF;
        }
    }
}
