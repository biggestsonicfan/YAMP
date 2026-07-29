#pragma once

// Shared m2ftg ("Model 2 fighting game") module-hosting structures, used by every m2ftg-family
// module (stf/fv/mr from Lost Judgment, vf2 from Yakuza: Like a Dragon). Layouts reverse-engineered
// from LJ's host driver (FUN_142494450) and confirmed line-for-line against YLAD's symbolized
// cscene_minigame_m2ftg::method_pre_render (0x1426a78b0) + cgame_module<m2ftg_module_t, ...>.

#include <cstdint>
#include <cstddef>

namespace m2ftg
{
        class cgs_device_context;

        // module_start copies 0x100C bytes from params+0x38 into its config global (DAT_1801ed490).
        // Field names from YLAD's symbolized scene ctor (config.kind/difficulty/country/
        // is_acf_skip/is_vf20/is_disable_pepsi/is_freeplay/is_vs_mode/is_sram_restore);
        // byte offsets corrected 2026-07-27 against the StF DLL's actual field readers —
        // the settings fields are u8 each, so country is +0x05, NOT +0x08:
        //   +0x00 kind: {0=vf2, 1=fv, 2=stf, 3=omg, 4=mr} — also selects "%s/rom/%s_rom.par"
        //   +0x04 difficulty (1 = normal) — FUN_1800529d0 -> backup SRAM 0x1D03342
        //   +0x05 country {0=JAPAN, 1=USA "Sonic Championship", 2=EXPORT} — FUN_1800529d0
        //          writes it into the game-assignments image: backup SRAM 0x1D03352 + working
        //          RAM 0x59C352 (both addresses user-verified against the live game)
        //   +0x06 is_acf_skip — gates the "rom/sound/stf.acf" load (FUN_180041570 et al.)
        //   +0x07 is_vf20, +0x08 is_disable_pepsi — VF2-only, zero readers in the StF DLL
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
            uint8_t is_disable_pepsi = 0;
            uint8_t is_freeplay = 1;
            uint8_t is_vs_mode = 0;
            uint8_t is_sram_restore = 0;
            std::byte settings[0x1000]{};
        };
        static_assert(sizeof(m2ftg_config_t) == 0x100C);
        static_assert(offsetof(m2ftg_config_t, country) == 0x05);
        static_assert(offsetof(m2ftg_config_t, is_freeplay) == 0x09);
        static_assert(offsetof(m2ftg_config_t, settings) == 0x0C);

        // Per-pad input block the m2ftg host copies into execute_info each frame (LJ FUN_1426bd820
        // copies 0x190 bytes per pad from the engine pad object). The first 0xE0 bytes are the same
        // pxd sl pad layout as csl_pad (now/push/pull/prev, 4 float axes, button_frame[32],
        // buttons[32], prev_buttons[32], port/user/connected); the LJ variant is just longer.
        // Button bits use the same sl::BUTTON numbering (START = bit 8 = 0x100).
        struct m2ftg_pad_t
        {
            unsigned int m_now;
            unsigned int m_push;
            unsigned int m_pull;
            unsigned int m_prev;
            float m_x1;
            float m_y1;
            float m_x2;
            float m_y2;
            int m_button_frame[32];
            uint8_t m_buttons[32];
            uint8_t m_prev_buttons[32];
            unsigned int m_port;
            int m_user_id;
            bool m_is_connected;
            bool m_is_remote;
            std::byte tail[0x190 - 0xEA];
        };
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

