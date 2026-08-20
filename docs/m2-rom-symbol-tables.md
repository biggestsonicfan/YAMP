# Model 2 ROM symbol tables - where they are, and what the names share

Every Model 2 module ships the ROM's own symbol table. Same record shape everywhere:
`{addr, name_ptr}`, pointer-sized, monotonic in guest address, no terminator - it simply
stops. 32-bit records in the Ringwide `d1a.exe`, 64-bit in the x64 pxd modules.

| game | module | RVA | records | first | last |
|---|---|---|---|---|---|
| Daytona/SRC | `d1a.exe` (x86) | `0x3368E0` | 651 | `start_ip` @ `0x860` | `FlagFreeplay` @ `0x5FE040` |
| StF | `stf-pxd-w64-d3d12_retail.dll` | `0x1742D0` | 800 | `start_ip` @ `0xB0` | `am_num` @ `0x5004C6` |
| FV | `fv-pxd-w64-d3d12_retail.dll` | `0x172390` | 742 | `start_ip` @ `0xB0` | `finish_coli_num` @ `0x574014` |
| VF2 | `vf2-pxd-w64-retail.dll` | `0x15DD40` | 301 | `start_ip` @ `0xB0` | `check_sram_all` @ `0x6DD4C` |
| MR | `mr-pxd-w64-d3d12_retail.dll` | `0x17A700` | 15248 | `_SAT` @ `0x0` | `_wallCollisionHead` @ `0x6CD4AC0` |
| VON | `omg-pxd-w64-gog_retail.dll` | `0x450160` | 276 | `start_ip` @ `0x930` | `zig07` @ `0x2FB5AD0` |

MR's 15248 records are not a mis-parse - Motor Raid's ROM was built with full symbols,
course geometry and all. VON's 276 match the count already recorded in
[von-rom-symbols.md](von-rom-symbols.md), extracted independently from the PS3 EBOOT; the RVA
differs because that doc read a different build (0x4507E0 vs 0x450160 in the GOG module),
so **locate the table by shape, never by a remembered RVA**.

## Finding it in a new module

Scan read-only sections for runs of `{uintptr addr, uintptr name_ptr}` where `addr` is small
(< 0x8000000) and `name_ptr` resolves to an identifier-shaped ASCII string; keep runs of 32+
that are monotonic in `addr`. Merge runs separated by one or two rejected entries - a name
your filter dislikes otherwise splits one table in two (this is what made FV read as 669+72
instead of 742). Nothing else in these images has that shape.

## Names shared across ROMs

Compared **case-sensitively**, with only a leading underscore stripped - that underscore is a
per-ROM toolchain artefact (`_VsyncScr` in Daytona, `VsyncScr` in Virtual On). Do not fold
case: Daytona carries `VsyncScr` @ `0xE00` *and* `_VsyncScr` @ `0x1E2D0` as distinct symbols,
likewise `RANK_INT`/`rank_int` and `WINDOW_A`/`WINDOW_a`. Of 17435 distinct
names across the six ROMs, **396 appear in two or more**, 155 in three or more, 25 in four or more.

### The shared Model 2 runtime (4+ ROMs)

This is the part that transfers: the board-services and geometry layer AM2 carried between
titles. A hook in one game usually has a counterpart at the address in the same row.

| symbol | Daytona | StF | FV | VF2 | MR | VON |
|---|---|---|---|---|---|---|
| `start_ip` | `0x860` | `0xB0` | `0xB0` | `0xB0` | `0x5E0` | `0x930` |
| `VsyncScr` | `0xE00` / `0x1E2D0` | `0xC40` | - | `0xBC0` | `0x36E30` | `0x1424` |
| `main` | `0x113C` | `0x6928` | `0x7040` | `0x9798` | `0x3C9E0` | - |
| `copro_down` | `0xFA0` | - | `0x1904` | `0xE60` | `0x16380` | - |
| `cop_initialize` | `0xFE4` | - | `0x1908` | `0xEA4` | `0x16440` | - |
| `geo_func` | `0x1000` | `0x1550` | - | `0xED0` | `0x2E540` | - |
| `main_loop` | `0x1330` | `0x73A8` | `0x7958` | `0x9FB0` | - | - |
| `interrupt_wait_b` | `0x13A0` | `0x11580` | `0x1184C` | `0x10F08` | - | - |
| `init_ram` | `0x1794` | `0x7440` | `0x79EC` | `0xA048` | - | - |
| `event_loop` | `0x1918` | `0x113B0` | `0x1167C` | `0x10D54` | - | - |
| `mode_control` | `0x1978` | `0x7A68` | `0x8028` | `0xA6C0` | - | - |
| `ADV_INT` | `0x1A5C` | `0x7FB0` | `0x8574` | `0xAB0C` | - | - |
| `ADV_DSP` | `0x1D9C` | `0x82EC` | `0x87CC` | `0xACF8` | - | - |
| `INFO_INT` | `0x2424` | `0xA200` | `0xAA80` | `0xC45C` | - | - |
| `GAME_DSP` | `0x402C` | `0xB614` | `0xBC20` | `0xD380` | - | - |
| `OVER_INT` | `0x4E8C` | `0xF6F0` | `0xF988` | `0x10088` | - | - |
| `NAME_INT` | `0x53B4` | `0x10700` | `0x10994` | `0x10784` | - | - |
| `NAME_DSP` | `0x5594` | `0x10C44` | `0x10E00` | `0x10898` | - | - |
| `init_fix` | `0x5CFC` | `0x12280` | `0x12324` | `0x11B48` | - | - |
| `rand` | - | `0x66B0` | `0x6DC8` | `0x94D0` | `0x21D890` | - |
| `set_obj` | `0x17FB8` | `0x4E88` | `0x5A48` | `0x7C60` | - | - |
| `set_window_data` | `0x183D8` | `0x35E0` | `0x41EC` | `0x2F5C` | - | - |
| `set_end_mark` | `0x183F0` | `0x354C` | `0x4158` | `0x2EDC` | - | - |
| `set_viewing_data` | `0x18448` | `0x5630` | `0x5F74` | `0x7F7C` | - | - |
| `print_mes` | `0x18910` | `0x5674` | `0x5FB8` | `0x7FC0` | - | - |

### The AM2 fighter trio

StF, FV and VF2 are one codebase: 140 names line up across all three, whole subsystems at a
time - `ADV_*` attract states, `JUDGE_*`, the `os_set_*` / `osage_*` skeleton pipeline, the
replay recorder (`key_rec_*`, `key_play_*`, `put_last_replay`), collision (`area_coli`,
`decide_coli_kind`, `coli_cont_cop`). Work done on one of those three should be checked
against the other two before it is called game-specific.

| symbol | StF | FV | VF2 |
|---|---|---|---|
| `start_ip` | `0xB0` | `0xB0` | `0xB0` |
| `make_lay_col_256_tbl_demon` | `0x5A8` | `0x594` | `0x530` |
| `player_entry` | `0x19A8` | `0x24BC` | `0x1344` |
| `check_credit` | `0x1B70` | `0x27A0` | `0x1610` |
| `set_end_mark` | `0x354C` | `0x4158` | `0x2EDC` |
| `set_window_data` | `0x35E0` | `0x41EC` | `0x2F5C` |
| `set_mmode` | `0x4C94` | `0x58F0` | `0x7B18` |
| `set_obj` | `0x4E88` | `0x5A48` | `0x7C60` |
| `set_obj_fifo` | `0x52C8` | `0x5D38` | `0x7DC4` |
| `window_data_init` | `0x54F0` | `0x5ED4` | `0x7EF0` |
| `set_window` | `0x5564` | `0x5F08` | `0x7F24` |
| `set_viewing_data` | `0x5630` | `0x5F74` | `0x7F7C` |
| `print_mes` | `0x5674` | `0x5FB8` | `0x7FC0` |
| `rand` | `0x66B0` | `0x6DC8` | `0x94D0` |
| `main` | `0x6928` | `0x7040` | `0x9798` |
| `main_loop` | `0x73A8` | `0x7958` | `0x9FB0` |
| `init_ram` | `0x7440` | `0x79EC` | `0xA048` |
| `init_adv_rep_ram` | `0x7444` | `0x79F0` | `0xA0C4` |
| `clear_buffer_ptr` | `0x74C0` | `0x7A80` | `0xA154` |
| `mode_control` | `0x7A68` | `0x8028` | `0xA6C0` |
| `test_sw_chk` | `0x7BA4` | `0x8164` | `0xA748` |
| `ADV_INT` | `0x7FB0` | `0x8574` | `0xAB0C` |
| `ADV_DSP` | `0x82EC` | `0x87CC` | `0xACF8` |
| `ADV_SEGA_PIC_INT` | `0x8714` | `0x8B90` | `0xB0D8` |
| `ADV_SEGA_PIC_DSP` | `0x8BAC` | `0x9078` | `0xB394` |
| `ADV_MOVIE_INT` | `0x8C08` | `0x90FC` | `0xB3F8` |
| `ADV_MOVIE_DSP` | `0x8DC8` | `0x9330` | `0xB4F0` |
| `ADV_REPLAY_PIC` | `0x8E90` | `0x93F8` | `0xB588` |
| `ADV_REPLAY_INT` | `0x8F80` | `0x9588` | `0xB66C` |
| `ADV_REPLAY_WAIT1A` | `0x9584` | `0x9B30` | `0xB9B8` |
| `ADV_REPLAY_WAIT1B` | `0x96E4` | `0x9CAC` | `0xBAEC` |
| `ADV_REPLAY_DSP` | `0x97B8` | `0x9D70` | `0xBC10` |
| `ADV_REPLAY_WAIT2` | `0x9864` | `0x9E5C` | `0xBCB0` |
| `ADV_REPLAY_PIC2` | `0x98B8` | `0x9EF4` | `0xBCE4` |
| `ADV_CPU_BATTLE_INT` | `0x99AC` | `0xA078` | `0xBDC8` |
| `ADV_CPU_BATTLE_WAIT1` | `0x9DE8` | `0xA46C` | `0xC0A4` |
| `ADV_CPU_BATTLE_DSP` | `0x9FF0` | `0xA6A4` | `0xC268` |
| `ADV_CPU_BATTLE_WAIT2` | `0xA19C` | `0xA9EC` | `0xC414` |
| `ADV_REPEAT` | `0xA1D8` | `0xAA58` | `0xC448` |
| `INFO_INT` | `0xA200` | `0xAA80` | `0xC45C` |
| `SEL_INT` | `0xA218` | `0xAA98` | `0xC474` |
| `SEL_DSP` | `0xA7F4` | `0xAF1C` | `0xC7EC` |
| `GAME_INT` | `0xB0F4` | `0xB77C` | `0xD014` |
| `GAME_DSP` | `0xB614` | `0xBC20` | `0xD380` |
| `ROUND_MASK_INT` | `0xB820` | `0xBDB0` | `0xD4AC` |
| `ROUND_MASK_DSP` | `0xBD88` | `0xC418` | `0xD844` |
| `ROUND_INT` | `0xC34C` | `0xC4E0` | `0xD94C` |
| `ROUND_DSP` | `0xC7C0` | `0xC7D4` | `0xDB80` |
| `SET_INT` | `0xC7D8` | `0xC7EC` | `0xDB98` |
| `SET_DSP` | `0xCE18` | `0xCD7C` | `0xDE64` |
| `READY_INT` | `0xCE84` | `0xCDC8` | `0xDEB0` |
| `FIGHT_DSP` | `0xD188` | `0xD5FC` | `0xE474` |
| `JUDGE_INT` | `0xD638` | `0xDB44` | `0xE544` |
| `JUDGE_WAIT` | `0xD66C` | `0xDB68` | `0xE590` |
| `JUDGE_DSP_INT` | `0xD684` | `0xDB80` | `0xE5A8` |
| `JUDGE_DSP` | `0xDC7C` | `0xE15C` | `0xEB44` |
| `REPLAY_INT` | `0xDEFC` | `0xE3DC` | `0xEDF4` |
| `game_sub_ex` | `0xE21C` | `0xE754` | `0xF0D8` |
| `REPLAY_DSP` | `0xE238` | `0xE770` | `0xF0F4` |
| `next_program` | `0xE374` | `0xE8B0` | `0xF208` |
| `VIC_INT` | `0xE6EC` | `0xEC88` | `0xF61C` |
| `CONTINUE_INT` | `0xF088` | `0xF398` | `0xFAE0` |
| `CONTINUE_DSP` | `0xF308` | `0xF5BC` | `0xFCF4` |
| `vs_conti` | `0xF524` | `0xF7BC` | `0xFF24` |
| `OVER_INT` | `0xF6F0` | `0xF988` | `0x10088` |
| `ENDING_DSP` | `0xFC08` | `0xFD88` | `0x10404` |
| `ENDSUB_WAIT_1` | `0x101D8` | `0x101FC` | `0x10548` |
| `NAME_INT` | `0x10700` | `0x10994` | `0x10784` |
| `NAME_DSP` | `0x10C44` | `0x10E00` | `0x10898` |
| `all_end` | `0x10D18` | `0x10FA8` | `0x10958` |
| `TEST_INT` | `0x10DCC` | `0x1105C` | `0x10A0C` |
| `event_loop` | `0x113B0` | `0x1167C` | `0x10D54` |
| `interrupt_wait_b` | `0x11580` | `0x1184C` | `0x10F08` |
| `start_check` | `0x11744` | `0x11A14` | `0x110B0` |
| `variable_diff_calc` | `0x11A04` | `0x11C80` | `0x112F8` |
| `init_scroll` | `0x121D0` | `0x12268` | `0x11A8C` |
| `init_fix` | `0x12280` | `0x12324` | `0x11B48` |
| `rob_action` | `0x12974` | `0x12B3C` | `0x142F4` |
| `decide_command` | `0x12A94` | `0x12C54` | `0x1442C` |
| `exec_command` | `0x13040` | `0x13284` | `0x146EC` |
| `exec_action` | `0x13510` | `0x136A4` | `0x1499C` |
| `calc_unit_mat` | `0x16C44` | `0x17160` | `0x16504` |
| `set_coli_ball_data` | `0x17C14` | `0x18304` | `0x176A0` |
| `rob_revise_yang` | `0x17CA0` | `0x18394` | `0x17710` |
| `rob_spd_control` | `0x17F78` | `0x18674` | `0x1791C` |
| `action_after` | `0x18404` | `0x18AEC` | `0x180BC` |
| `rob_disp` | `0x19BB0` | `0x19F10` | `0x18EF8` |
| `rob_area_check` | `0x1AF3C` | `0x1B574` | `0x19EBC` |
| `set_motion` | `0x1AF78` | `0x1B5B0` | `0x19EF8` |
| `set_mot_dat` | `0x1B26C` | `0x1B880` | `0x1A1E4` |
| `play_motion` | `0x1BDA4` | `0x1C0FC` | `0x1AB74` |
| `shift_mot_control` | `0x1CD00` | `0x1CC4C` | `0x1B568` |
| `air_mot_control` | `0x1CE08` | `0x1CCAC` | `0x1B5C8` |
| `camera_control` | `0x1F290` | `0x1EEA0` | `0x1D458` |
| `area_check` | `0x288A8` | `0x24360` | `0x214DC` |
| `name_char_disp` | `0x28DD4` | `0x51A44` | `0x21864` |
| `collision` | `0x293EC` | `0x24988` | `0x221E8` |
| `decide_coli_kind` | `0x2CFBC` | `0x27BBC` | `0x233D0` |
| `coli_cont_cop` | `0x2D36C` | `0x27E18` | `0x23524` |
| `decide_dir` | `0x2D588` | `0x28034` | `0x2364C` |
| `coli_recalc_pos` | `0x2D67C` | `0x28128` | `0x23694` |
| `calc_attack_flag` | `0x2DC48` | `0x286F4` | `0x238A4` |
| `area_coli` | `0x2DCA4` | `0x28750` | `0x2396C` |
| `smooth_int` | `0x2F2B0` | `0x29F48` | `0x27130` |
| `rear_smooth_int` | `0x2F878` | `0x2A53C` | `0x2776C` |
| `calc_rob_angle` | `0x2FF08` | `0x2AA84` | `0x27CE0` |
| `calc_rob_angle_cont` | `0x2FF2C` | `0x2AAA8` | `0x27D04` |
| `get_frame_dat` | `0x304C8` | `0x2AF28` | `0x28184` |
| `get_start_value` | `0x30A8C` | `0x2B404` | `0x28780` |
| `get_end_value` | `0x30B54` | `0x2B4CC` | `0x28848` |
| `get_fcurve_value` | `0x30C24` | `0x2B59C` | `0x28918` |
| `get_fcurve_value_f` | `0x30C28` | `0x2B5A0` | `0x2891C` |
| `set_mirror` | `0x30E04` | `0x2B77C` | `0x28AF8` |
| `calc_unit_1` | `0x30FE8` | `0x2BBE0` | `0x2901C` |
| `sel_disp` | `0x354E8` | `0x380A0` | `0x31ADC` |
| `select_init` | `0x35B80` | `0x38AE4` | `0x32090` |
| `select_pl` | `0x35EB0` | `0x38E24` | `0x324A0` |
| `key_rec_init` | `0x37CB0` | `0x3AA64` | `0x39C40` |
| `key_rec_disp` | `0x37CE0` | `0x3AA94` | `0x39C6C` |
| `key_play_init` | `0x37D80` | `0x3AB34` | `0x39D0C` |
| `key_play_disp` | `0x37DC4` | `0x3AB78` | `0x39D5C` |
| `save_ending_rep_data` | `0x38464` | `0x3B218` | `0x39E34` |
| `ending_send_next_tex` | `0x38564` | `0x3B318` | `0x39F08` |
| `load_ending_rep_data` | `0x385B4` | `0x3B370` | `0x39F60` |
| `play_init` | `0x39254` | `0x3C00C` | `0x3A824` |
| `rep_start_chk` | `0x3947C` | `0x3C214` | `0x3A9A8` |
| `play_disp` | `0x395B0` | `0x3C348` | `0x3AADC` |
| `put_last_replay` | `0x3972C` | `0x3C518` | `0x3AC10` |
| `enemy_control` | `0x3B654` | `0x3EF88` | `0x3D4B8` |
| `sound_queue_output` | `0x3F44C` | `0x420B4` | `0x43ABC` |
| `unp_send_tex_req` | `0x4AC18` | `0x4B244` | `0x4B9B8` |
| `osage_init` | `0x67554` | `0x62768` | `0x640F4` |
| `osage_dsp` | `0x67640` | `0x62854` | `0x6428C` |
| `os_set_matrix` | `0x67D28` | `0x62EE0` | `0x64864` |
| `os_set_tsukene` | `0x680E0` | `0x63298` | `0x64CA0` |
| `os_set_coli` | `0x6829C` | `0x63454` | `0x64E5C` |
| `os_set_osage` | `0x685FC` | `0x637B4` | `0x651BC` |
| `os_set_etc` | `0x68758` | `0x63910` | `0x65318` |
| `osage_copro` | `0x687C4` | `0x6397C` | `0x65384` |
| `object_control` | `0x719EC` | `0x6AD6C` | `0x6CA84` |
