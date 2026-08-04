#pragma once

// The Lost Judgment m2ftg host: entry points, shared host helpers, and the per-game facts
// (GameDesc) for every LJ m2ftg module YAMP can run through this path — Sonic the Fighters,
// Fighting Vipers and Motor Raid. The three DLLs are near-identical builds of the same Model 2
// emulator engine — same module protocol (m2ftg.h), same context sizes (sl 0xF000 /
// gs 0x388A00 / ct 0x30, all re-verified against the MR DLL's own initialize_module checks),
// same execute_info (0x1760) and the same ImportSymbols byte patterns — so the whole hosting
// path is shared and everything that actually differs between the games lives in the GameDesc
// table below.
//
// The cross-host module structures (m2ftg_config_t / m2ftg_pad_t / m2ftg_execute_info_t)
// stay in ../m2ftg.h, which the YLAD VF2 host (../YLAD/VF2.cpp) includes too; this header
// is only for the LJ side (plus the ApplyAspectSetting/DrawPauseMenu helpers VF2 reuses).

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "../../RenderWindow.h"
#include "../../YAMPGeneral.h"
#include "../ModuleBuild.h"

using module_func_t = int(*)(size_t args, const void* argp);

namespace m2ftg
{
    // ---- Per-game facts -------------------------------------------------------------------

    // i960 emulator globals by DLL RVA, used by InstallRamExecFetch (Patch.cpp): CPU context
    // pointer, opcode dispatch table, work-RAM host base pointer. All three zero = this DLL has
    // no hookable single-step fetch dispatcher and the RAM-exec patch is skipped entirely
    // (see GAME_MR).
    //
    // These are per module BUILD, not per game: the same arcade game recompiled for a different
    // Yakuza/Judgment release puts every global somewhere else. `timestamp` says which build a
    // row describes — see ../ModuleBuild.h.
    struct I960Build
    {
        uint32_t timestamp;
        uintptr_t rva_cpu_ctx_ptr;
        uintptr_t rva_opcode_table;
        uintptr_t rva_ram_base_ptr;
    };

    struct GameDesc
    {
        const wchar_t* dll_name;
        const wchar_t* subdir;           // fallback subfolder next to YAMP.exe
        const char* display_name;
        uint32_t kind;                   // m2ftg_config_t.kind {1=fv, 2=stf, 4=mr}
        // Loose-ROM debug feature (file_access.cpp): the archive to hide and the ROM
        // images the DLL's boot loader requests (paths inside "rom/<archive stem>/").
        const char* rom_archive_name;    // "stf_rom.par" / "fv_rom.par" / "mr_rom.par"
        const wchar_t* const* rom_files;
        size_t rom_file_count;
        const I960Build* i960_builds;
        size_t i960_build_count;
    };

    // ROM image lists were read out of each DLL's own path strings ("rom/stf_rom/..." /
    // "rom/fv_rom/..." / "rom/mr_rom/..." — note FV and MR split the EP ROM into
    // rom_ep1/rom_ep2, and MR adds a Model 2 coprocessor ROM). The i960 RVAs come from each
    // DLL's fetch dispatcher + work-RAM base global (FV values verified against
    // fv-pxd-w64-d3d12_retail.dll in Ghidra).
    inline constexpr const wchar_t* STF_ROM_FILES[] = {
        L"rom_code1.bin", L"rom_data.bin", L"rom_ep.bin", L"rom_pol.bin", L"rom_tex.bin",
    };
    inline constexpr const wchar_t* FV_ROM_FILES[] = {
        L"rom_code1.bin", L"rom_data.bin", L"rom_ep1.bin", L"rom_ep2.bin", L"rom_pol.bin", L"rom_tex.bin",
    };
    // MR's boot loader walks a 7-entry {path, size} table (DLL 0x1802771A0) — this is that
    // table's order. Its program ROM is named rom_code_tw.bin, not rom_code1.bin.
    inline constexpr const wchar_t* MR_ROM_FILES[] = {
        L"rom_code_tw.bin", L"rom_ep1.bin", L"rom_ep2.bin", L"rom_data.bin",
        L"rom_pol.bin", L"rom_tex.bin", L"rom_cop.bin",
    };
    // Sonic the Fighters ships in two titles. The ROM and every asset are byte-identical between
    // them (see ../ModuleBuild.h) — only the host DLL was recompiled, moving every global. The
    // Gaiden row was read out of that build's fetch dispatcher: `mov rax,[cpu_ctx]` and
    // `mov rax,[r10+rax*8+opcode_table]` give the first two directly, and the third was
    // confirmed against its HLE installer, which sets the work-RAM base pointer. Both routes
    // agree that every .data global moved by a uniform +0xB9C0.
    inline constexpr I960Build STF_I960[] = {
        { build::LJ_STF,     0x58A960, 0x168630, 0x8F7CC8 },
        { build::GAIDEN_STF, 0x596320, 0x1704F0, 0x903688 },
    };
    inline constexpr I960Build FV_I960[] = {
        { build::LJ_FV, 0x58CF60, 0x166720, 0x8FA2C8 },
    };
    // MR's CPU core inlines fetch/decode into its execution LOOP instead of shipping the
    // single-instruction dispatcher StF and FV have, so there is no equivalent hook point.
    inline constexpr I960Build MR_I960[] = {
        { build::LJ_MR, 0, 0, 0 },
    };

    inline constexpr GameDesc GAME_STF = {
        L"stf-pxd-w64-d3d12_retail.dll", L"stf", "Sonic the Fighters", 2,
        "stf_rom.par", STF_ROM_FILES, std::size(STF_ROM_FILES),
        STF_I960, std::size(STF_I960),
    };
    inline constexpr GameDesc GAME_FV = {
        L"fv-pxd-w64-d3d12_retail.dll", L"fv", "Fighting Vipers", 1,
        "fv_rom.par", FV_ROM_FILES, std::size(FV_ROM_FILES),
        FV_I960, std::size(FV_I960),
    };
    // config.kind 4 = mr: the DLL's rom-name table (0x18020E870 "mr", 0x88C "stf", 0x890
    // "omg", 0x894 "vf2", 0x898 "fv") is indexed by kind to build "%s/rom/%s_rom.par".
    // i960 RVAs are zero on purpose (MR_I960 above): MR's CPU core inlines the fetch/decode
    // into its execution LOOP (FUN_18002CAC0, ctx ptr 0x618618 / opcode table 0x16EAA0), so
    // there is no equivalent hook point — and the RAM-exec patch only exists for StF's ROM
    // debug menu.
    inline constexpr GameDesc GAME_MR = {
        L"mr-pxd-w64-d3d12_retail.dll", L"mr", "Motor Raid", 4,
        "mr_rom.par", MR_ROM_FILES, std::size(MR_ROM_FILES),
        MR_I960, std::size(MR_I960),
    };

    inline const GameDesc& CurrentGame()
    {
        switch (gGeneral.GetGameId())
        {
        case YAMPGeneral::GameId::FV: return GAME_FV;
        case YAMPGeneral::GameId::MR: return GAME_MR;
        default: return GAME_STF;
        }
    }

    // The running module build's i960 globals. Falls back to the game's first row when the build
    // is unknown, which only happens if a module reaches here without going through GameVerify —
    // that gate blocks unrecognised builds, so an unknown build is a YAMP bug, not a user file.
    inline const I960Build& CurrentI960()
    {
        const GameDesc& game = CurrentGame();
        const uint32_t running = CurrentModuleBuild();
        for (size_t i = 0; i < game.i960_build_count; i++)
        {
            if (game.i960_builds[i].timestamp == running) return game.i960_builds[i];
        }
        return game.i960_builds[0];
    }

    // ---- Host entry points (DLL-based path, DirectX 12 game) ------------------------------
    HMODULE LoadDLL();
    void PreInitialize();                 // sets DataPath/loads settings (kept for parity)
    void Run(RenderWindow& window);       // run via the StF/FV game DLL
    bool GameLoop(module_func_t func, RenderWindow& window);

    // (ApplyAspectSetting / DrawPauseMenu live in ../HostUI.h — shared with the VF2 host.)
}
