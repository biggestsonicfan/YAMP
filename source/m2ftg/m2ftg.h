#pragma once

// Shared m2ftg ("Model 2 fighting game") module-hosting structures, used by every m2ftg-family
// module (stf/fv/mr from Lost Judgment, vf2 from Yakuza: Like a Dragon). Layouts reverse-engineered
// from LJ's host driver (FUN_142494450) and confirmed line-for-line against YLAD's symbolized
// cscene_minigame_m2ftg::method_pre_render (0x1426a78b0) + cgame_module<m2ftg_module_t, ...>.

#include <cstdint>
#include <cstddef>

// The per-pad block below is the shared LJ-era pad layout (pxd::lj_pad_t).
#include "../pxd/LJ/sl.h"

// The device context handed to the module belongs to the shared pxd platform layer
// (source/pxd/LJ/gs.h), not to m2ftg — only the pointer is part of this protocol.
namespace pxd
{
        class cgs_device_context;
}

namespace m2ftg
{
        using pxd::cgs_device_context;

        // module_start copies 0x100C bytes from params+0x38 into its config global (DAT_1801ed490).
        // Field names from YLAD's symbolized scene ctor (config.kind/difficulty/country/
        // is_acf_skip/is_vf20/is_freeplay/is_vs_mode/is_sram_restore);
        // byte offsets corrected 2026-07-27 against the StF DLL's actual field readers —
        // the settings fields are u8 each, so country is +0x05, NOT +0x08:
        //   +0x00 kind: {0=vf2, 1=fv, 2=stf, 3=omg, 4=mr} — also selects "%s/rom/%s_rom.par"
        //   +0x04 difficulty (1 = normal) — FUN_1800529d0 -> backup SRAM 0x1D03342
        //   +0x05 country {0=JAPAN, 1=USA "Sonic Championship", 2=EXPORT} — FUN_1800529d0
        //          writes it into the game-assignments image: backup SRAM 0x1D03352 + working
        //          RAM 0x59C352 (both addresses user-verified against the live game)
        //   +0x06 is_acf_skip — gates the "rom/sound/stf.acf" load (FUN_180041570 et al.)
        //   +0x07 is_vf20 — VF2 version 2.0 when set, 2.1 when clear. Zero readers in the StF
        //          DLL; in the YLAD VF2 DLL its ONLY reader is the backup-RAM injector (HLE hook
        //          8, check_sram_all+0x47C), which re-reads it at every board init and folds it
        //          into the operator byte — so it can be changed without relaunching, as long as
        //          the board is reset afterwards. Netplay publishes it as a room flag.
        //   +0x08 WAS is_disable_pepsi, removed 2026-08-02 — it is a DEAD FIELD in every module
        //          YAMP hosts. Zero readers in the StF DLL, and zero in the YLAD VF2 one: the
        //          only reference in the whole VF2 module is the config default-init
        //          (0x18000201B, `mov dword [config+0x08],0x100`), which never reads it back.
        //          Contrast +0x0A and +0x0B, which do have real readers, so this is not an
        //          artefact of how the config is accessed. VF2's arcade release carried Pepsi
        //          stage advertising, so the switch presumably drove a build that still had the
        //          artwork; this one does not, and the setting it fed did nothing at all.
        //          The byte survives as `reserved_08` because the layout is the module's ABI.
        //   +0x09 is_freeplay
        //   +0x0A is_vs_mode — LJ's "2P quick match" switch (StF readers RE'd 2026-07-26):
        //          FUN_180052ec0 force-inserts 5 credits into BOTH coin counters (game RAM
        //          +0x500248/+0x50024C |= 5) and skips the normal boot flow; FUN_180052e50
        //          picks a random stage (0..8); FUN_1800529d0 writes SRAM mode byte 3 instead
        //          of 2. Clear = authentic arcade boot (attract, coin/start, 1P ladder).
        //   +0x0B is_sram_restore — FUN_1800529d0 copies the +0x0C blob into emu SRAM+0x3000
        //   +0x0C.. 0x1000-byte SRAM settings image (all zero in live LJ captures)
        struct m2ftg_config_t
        {
            uint32_t kind = 2;
            uint8_t difficulty = 1;
            uint8_t country = 0;
            uint8_t is_acf_skip = 0;
            uint8_t is_vf20 = 0;
            // WAS is_disable_pepsi. Kept as padding, NOT deleted: this struct is the module's
            // own config ABI, so removing the byte would shift is_freeplay and everything below
            // it by one. Left zero, which is what the module's own default-init writes.
            uint8_t reserved_08 = 0;
            uint8_t is_freeplay = 1;
            uint8_t is_vs_mode = 0;
            uint8_t is_sram_restore = 0;
            std::byte settings[0x1000]{};
        };
        static_assert(sizeof(m2ftg_config_t) == 0x100C);
        static_assert(offsetof(m2ftg_config_t, country) == 0x05);
        static_assert(offsetof(m2ftg_config_t, is_freeplay) == 0x09);
        static_assert(offsetof(m2ftg_config_t, settings) == 0x0C);

        // Per-pad input block the m2ftg host copies into execute_info each frame. Nothing about it
        // is m2ftg-specific — it is the LJ-era pad layout, shared with the VF5FS-LJ host, so the
        // definition (and the reasoning behind it) lives in ../pxd/LJ/sl.h.
        using m2ftg_pad_t = pxd::lj_pad_t;
        static_assert(sizeof(m2ftg_pad_t) == 0x190);
        static_assert(offsetof(m2ftg_pad_t, m_buttons) == 0xA0);
        static_assert(offsetof(m2ftg_pad_t, m_port) == 0xE0);

        // m2ftg execute_info, layout from LJ's per-frame module_main driver FUN_142494450
        // (execute_info is embedded at scene+0x13A0; every offset below verified there):
        //   +0x00 size (must be 0x1760), +0x08 device context, +0x10 status, +0x14 result
        //   (host presets 0x80004005), +0x18 output_texid (host zeroes), +0x1C sound volume,
        //   +0x20/+0x1B0 pad[2] (0x190 each), +0x1660 "work" block (game kind + assigns + events).
        // status bits: host->module bit0 = pause, bit5 (0x20) = coin inserted this frame;
        // module->host bit6 (0x40) = "insert coin / press start" screen active (m_is_coin_wait),
        // bits 0x100/0x200/0x400/0x1000 = game events (payloads in the work block).
        // MUST PERSIST ACROSS FRAMES (YLAD embeds it in the scene; the module keeps state in it).
        struct alignas(16) m2ftg_execute_info_t
        {
            enum assign_t
            {
                assign_invalid = 0x0,
                assign_none = 0x1,
                assign_p = 0x2,
                assign_k = 0x3,
                assign_g = 0x4,
                assign_pg = 0x5,
                assign_pkg = 0x6,
                assign_pk = 0x7,
                assign_kg = 0x8,
            };

            size_t size_of_struct;
            cgs_device_context* p_device_context;
            int status;
            int result;
            unsigned int output_texid;
            float sound_volume;
            m2ftg_pad_t pad[2];
            // +0x340..+0x1660: module-visible workspace — persists across frames, never host-written.
            std::byte gap[0x1660 - 0x340];
            // "work" block (named from YLAD symbols): +0x1660 game kind (indexes the host volume
            // table; LJ trophy code switches on it), +0x1664 assign[2][8] button assignments (host
            // writes from settings; StF FUN_180003ac0 consumes), +0x1674/+0x167C event payloads the
            // module writes alongside status event bits 0x100/0x200/0x1000 (rob id / stage id).
            int work_kind;
            uint8_t assign[2][8];
            std::byte gap2[8];
            uint32_t event_param;
            std::byte tail[0x1760 - 0x1680];
        };
        static_assert(sizeof(m2ftg_execute_info_t) == 0x1760);
        static_assert(offsetof(m2ftg_execute_info_t, sound_volume) == 0x1C);
        static_assert(offsetof(m2ftg_execute_info_t, pad) == 0x20);
        static_assert(offsetof(m2ftg_execute_info_t, pad[1]) == 0x1B0);
        static_assert(offsetof(m2ftg_execute_info_t, work_kind) == 0x1660);
        static_assert(offsetof(m2ftg_execute_info_t, assign) == 0x1664);
        static_assert(offsetof(m2ftg_execute_info_t, event_param) == 0x167C);
    }

