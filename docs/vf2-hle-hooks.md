# Virtua Fighter 2 (Yakuza: Like a Dragon): the HLE ROM hook table

Recon of `vf2-pxd-w64-Retail.dll` — the **YLAD** VF2 module, hosted by
[`source/m2ftg/YLAD/VF2.cpp`](../source/m2ftg/YLAD/VF2.cpp). Static RVAs against the preferred
base `0x180000000`; the DLL relocates at runtime like every other pxd module, so every address
here is `module_base + RVA` in a live process.

**This is one of two VF2 modules YAMP runs.** Yakuza Kiwami 2 ships its own
(`vf2-pxd-w64-gog_retail.dll`, `GameId::VF2_K2`, hosted by `source/m2ftg/K2/K2Host.cpp`), which is
a different build with its own RVAs and possibly a different hook set. Nothing below has been
checked against it.

VF2 is the ANCESTOR of the other two Model 2 games YAMP hosts — Fighting Vipers (1995) and Sonic
the Fighters (1996) are both built on this engine — and the HLE mechanism is identical in all
three. See [`fv-hle-hooks.md`](fv-hle-hooks.md) for the FV/StF equivalents.

## The globals

| What | VF2 | FV | StF |
|------|-----|-----|-----|
| ROM loader / bring-up | `FUN_180047A70` | `FUN_180049C50` | `FUN_18004B070` |
| Trap installer | **`FUN_180003CD0`** (factored out) | inline | inline |
| Board init (what RESET re-runs) | `FUN_180047610` | `FUN_180049840` | — |
| Board reset routine | `FUN_180047500` | `FUN_180049720` | — |
| Hook table | `0x185640` | `0x1E5840` | `0x1E8870` |
| Hook count | **67** (`0x43`) | 95 (`0x5F`) | 76 (`0x4C`) |
| Saved originals | `0x51D180` | `0x690B40` | `0x68E540` |
| Program ROM base | `0x980040` | `0x9FA2D0` | `0x9F7CD0` |
| Boot state (2 = booted) | `0x641890` | `0x6BB900` | `0x6B9300` |
| "DEBUG MENU" root window | `0x186590` | `0x1E5820` | `0x1E8850` |
| DEBUG MENU items (8) | `0x185480` | — | — |
| RESET handler | `0x490A0` | `0x4B3E0` | `0x4C840` |
| Run state | `0x649E60` | `0x6C3FE0` | `0x6C19E0` |
| Stripped `return 0` stub | `0x48FE0` | `0x4B320` | `0x4C780` |
| ROM symbol table | `0x15DD40` (**301**) | `0x172390` (732) | `0x1742D0` (800) |
| Emulated memory map | `0x15C080` | `0x170750` | `0x172660` |
| i960 context pointer | `0x51F9B8` | `0x58CF60` | `0x58A960` |
| Work-RAM host base ptr | `0x880030` | `0x8FA2C8` | `0x8F7CC8` |
| Opcode dispatch table | `0x152090` | `0x166720` | `0x168630` |
| Composite-enable flag | `0x51F0CF` | `0x6BB772` | `0x6B9172` |
| Host frame counter | `0xA80050` | `0xAFA2E0` | — |
| RNG holder | `0x623788` | `0x68E188` | `0x68BB88` |
| Texture-budget handler | `0x4F740` (4 sites) | `0x51C20` (4) | `0x52FD0` (4) |
| Exec-original tail | `0x3D60` | `0x39E0` | `0x39D0` |
| Skip-original tail | `0x3DF0` | `0x3A80` | `0x3A70` |
| Inert bare-jmp stub | `0x4EF40` (18 hooks) | `0x51880` (23) | `0x52D70` (14) |

The installer contract is unchanged — for every record with `romOffset < 0x200000`, save
`*(u64*)(rom + romOffset)` and write the trap word `0x4000000 | (i * 4)` — so restoring
`(u32)savedWords[i]` still un-does a hook completely and reversibly. The one structural
difference is cosmetic: VF2 factors the loop into `FUN_180003CD0(romBase)` instead of inlining it
in the bring-up function.

The memory map was confirmed from the DLL's own dispatch rather than by pattern-scanning, which
is what caught a one-slot error in the FV pass. Handlers `0x4F400` and `0x4F670` contain
`(&PTR_FUN_18015c0a0)[index * 0xE]`, and `0x15C0A0` is the read32 slot at base `+0x20` — so the
base is `0x15C080`. The table then reads correctly: ROM regions have all three write slots on the
bare-`ret` stub (`0x2550`), record 5 is work RAM, and 28 unmapped regions share that stub.

## Netplay-critical hooks

- **Texture-upload budget — hooks 48-51, handler `0x4F740`.** The only wall-clock read:
  `g0 = (elapsed_us > 7499)`, a flat ~7.5 ms with no master-state variant (FV switches between
  12 ms and 8 ms). Same four ROM sites FV uses — `send_beta_data`, `send_lod_data`,
  `send_lod_data_q_sub_norm`, `send_lod_data_q_sub_anim`.
- **RNG — two streams, exactly as in FV.** Both hang off `DLL+0x623788`:
  - `[+0x20]` — hook 52 (`rand`), whole-function HLE, generator `FUN_180008DA0`.
  - `[+0x08]` — hook 31 (`SEL_DSP+0x75C`), the stage picker, `value % 11` through an 11-byte
    table, gated on config `DLL+0x6263FA`.
  Seeding only the first would leave two peers agreeing on the fight and disagreeing on the
  stage. YAMP's `SeedHostRng` already walks a per-game stream list, so this needs data, not code.
- **Backup-RAM / DIP injection — hook 8, handler `0x4ECA0`** (`check_sram_all+0x47C`): config gate
  `DLL+0x6263FB`, board SRAM at `[DLL+0x880020]+0x91`. VF2's GAME ASSIGNMENTS source.
- **Self-test bypass — hooks 11 and 13, handler `0x4EE40`** (force `r3 = 1`).
- **Frame yield / vsync — hooks 2 and 5-7**, host frame counter `DLL+0xA80050`.

## The table

`Repl` = the handler replaces the original instruction (ends in the inline length classifier);
`n` = it runs additively, ending in a call or jump to the exec-original tail at `0x3D60`.

Kinds and notes are confirmed by decompilation for the tails, the inert stub, and every
multi-site handler plus hooks 0-13, 31, 48-52. The remaining single-site handlers have their
`Repl` flag derived from the tail they end in (which is mechanical and reliable) but carry a
generic note — they are classified `Content` provisionally, not from reading each one.

| # | ROM | Handler | Site (AM2 symbol) | Kind | Repl | What it does |
|---|-----|---------|-------------------|------|------|--------------|
| 0 | `0x011BD4` | `0x4E960` | `init_fix+0x8C` | Core | Y | Forces g0 = 0 in the board hardware-init check. |
| 1 | `0x009FAC` | `0x4E9B0` | `siw_loop+0x8` | Core | Y | REQUIRED FOR ANY PICTURE. Sets the composite-enable flag (DLL+0x51F0CF) and clears the 0x2000-byte host tile/sprite buffer at [DLL+0x880038]. |
| 2 | `0x009FB0` | `0x4EA20` | `main_loop` | Core | Y | Per-frame yield, plus the master-state-dependent tile-buffer mode words at [DLL+0x880038]+0x98898. |
| 3 | `0x000F7C` | `0x4EAD0` | `interrupt_wait` | Core | Y | Interrupt handshake: raises the pending-interrupt bit (ctx+0x188) when ctx+0x18C bit 0 is armed. |
| 4 | `0x000F84` | `0x4EB30` | `interrupt_wait+0x8` | Core | Y | Second half of the interrupt handshake: host yield (FUN_18004C690) + ctx+0x1B0, then skips the original. |
| 5 | `0x010F90` | `0x4EB60` | `interrupt_wait_b+0x88` | Core | Y | Vsync wait: supplies the host frame counter (DLL+0xA80050) in g0 and raises the pending-interrupt bit. |
| 6 | `0x010F98` | `0x4EBE0` | `interrupt_wait_b+0x90` | Core | Y | Vsync wait: supplies the host frame counter in r3. |
| 7 | `0x010FA0` | `0x4EC40` | `interrupt_wait_b+0x98` | Core | Y | Vsync wait loop: returns -8 to re-execute until the host frame counter advances. |
| 8 | `0x06E1C8` | `0x4ECA0` | `check_sram_all+0x47C` | Host | Y | Injects the backup-RAM / DIP block from the module config (gate DLL+0x6263FB) into board SRAM at [DLL+0x880020]+0x91. VF2's GAME ASSIGNMENTS injector. |
| 9 | `0x011378` | `0x4EE20` | `variable_diff_calc+0x80` | Content | Y | Forces ctx+0x68 = 7 in the difficulty calculation, then runs the original. |
| 10 | `0x011348` | `0x03DF0` | `variable_diff_calc+0x50` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3DF0). |
| 11 | `0x064808` | `0x4EE40` | `osage_dsp+0x57C` | Core | Y | Forces r3 = 1 to pass a self-test / checksum path. A modified ROM will not boot without these. |
| 12 | `0x002EAC` | `0x03DF0` | `chg_scr_color_req+0x360` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3DF0) (site 2). |
| 13 | `0x00A80C` | `0x4EE40` | `WARNING_INT+0x8` | Core | Y | Forces r3 = 1 to pass a self-test / checksum path. A modified ROM will not boot without these (site 2). |
| 14 | `0x024588` | `0x4EE90` | `Calc_pos+0x20` | Content | n | Runs the original afterwards. |
| 15 | `0x0245AC` | `0x4EEB0` | `Calc_pos+0x44` | Content | n | Runs the original afterwards. |
| 16 | `0x011138` | `0x03DF0` | `debug_sw_check+0x44` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3DF0) (site 3). |
| 17 | `0x011B44` | `0x4EED0` | `init_scroll+0xB8` | Content | Y | Replaces the original instruction. |
| 18 | `0x0438EC` | `0x4EEF0` | `enemy_control+0x6434` | Content | Y | Replaces the original instruction. |
| 19 | `0x00D030` | `0x4EF20` | `GAME_INT+0x1C` | Content | Y | Replaces the original instruction. |
| 20 | `0x001358` | `0x4EF40` | `player_entry+0x14` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build. |
| 21 | `0x00B0D8` | `0x4EF50` | `ADV_SEGA_PIC_INT` | Content | Y | Replaces the original instruction. |
| 22 | `0x00B57C` | `0x4EF70` | `ADV_MOVIE_DSP+0x8C` | Content | Y | Replaces the original instruction. |
| 23 | `0x00AD2C` | `0x4EF90` | `ADV_DSP+0x34` | Content | Y | Replaces the original instruction. |
| 24 | `0x0013E8` | `0x4EFB0` | `pushed_st1+0x64` | Content | Y | Replaces the original instruction. |
| 25 | `0x001548` | `0x4EFB0` | `pushed_st2+0x70` | Content | Y | Replaces the original instruction (site 2). |
| 26 | `0x001460` | `0x4F000` | `vs_mode1+0x28` | Content | Y | Replaces the original instruction. |
| 27 | `0x0015C0` | `0x4F000` | `vs_mode2+0x28` | Content | Y | Replaces the original instruction (site 2). |
| 28 | `0x00C474` | `0x4F020` | `SEL_INT` | Content | n | Runs the original afterwards. |
| 29 | `0x00C7FC` | `0x4EF40` | `SEL_DSP+0x10` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 2). |
| 30 | `0x00CB64` | `0x4EF40` | `SEL_DSP+0x378` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 3). |
| 31 | `0x00CF48` | `0x4F0E0` | `SEL_DSP+0x75C` | Content | n | Stage select: when config+0xA (DLL+0x6263FA) is set, picks 1 of 11 stages with the SECOND host twister [DLL+0x623788 + 0x08], then runs the original. NETPLAY-CRITICAL: seeding only `rand` leaves this free to disagree. |
| 32 | `0x00CDD4` | `0x4EF40` | `SEL_DSP+0x5E8` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 4). |
| 33 | `0x00DD48` | `0x4EF40` | `SET_INT+0x1B0` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 5). |
| 34 | `0x00EB38` | `0x4EF40` | `JUDGE_DSP_INT+0x590` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 6). |
| 35 | `0x00F61C` | `0x4EF40` | `VIC_INT` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 7). |
| 36 | `0x00F894` | `0x4EF40` | `VIC_INT+0x278` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 8). |
| 37 | `0x00F904` | `0x03DF0` | `VIC_INT+0x2E8` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3DF0) (site 4). |
| 38 | `0x00F910` | `0x4F150` | `VIC_INT+0x2F4` | Content | Y | Replaces the original instruction. |
| 39 | `0x00F438` | `0x4F1A0` | `next_program+0x230` | Content | n | Runs the original afterwards. |
| 40 | `0x00FF6C` | `0x4EF40` | `vs_conti+0x48` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 9). |
| 41 | `0x00C734` | `0x4F400` | `SEL_INT+0x2C0` | Content | n | Runs the original afterwards. |
| 42 | `0x00C898` | `0x4F500` | `SEL_DSP+0xAC` | Content | Y | Replaces the original instruction. |
| 43 | `0x00C8C8` | `0x4F570` | `SEL_DSP+0xDC` | Content | Y | Replaces the original instruction. |
| 44 | `0x00CA0C` | `0x4F5F0` | `SEL_DSP+0x220` | Content | Y | Replaces the original instruction. |
| 45 | `0x00C8E0` | `0x03DF0` | `SEL_DSP+0xF4` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3DF0) (site 5). |
| 46 | `0x00CA24` | `0x03DF0` | `SEL_DSP+0x238` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3DF0) (site 6). |
| 47 | `0x00CB18` | `0x4F670` | `SEL_DSP+0x32C` | Content | Y | Replaces the original instruction. |
| 48 | `0x04CAE4` | `0x4F740` | `send_beta_data+0x108` | Core | Y | TEXTURE-UPLOAD BUDGET, AND THE ONLY HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us > 7499). Times FUN_180061530() against DLL+0x625D60. The divergence netplay has to pin. |
| 49 | `0x04CC44` | `0x4F740` | `send_lod_data+0xE0` | Core | Y | TEXTURE-UPLOAD BUDGET, AND THE ONLY HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us > 7499). Times FUN_180061530() against DLL+0x625D60. The divergence netplay has to pin (site 2). |
| 50 | `0x04CEDC` | `0x4F740` | `send_lod_data_q_sub_norm+0x54` | Core | Y | TEXTURE-UPLOAD BUDGET, AND THE ONLY HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us > 7499). Times FUN_180061530() against DLL+0x625D60. The divergence netplay has to pin (site 3). |
| 51 | `0x04D018` | `0x4F740` | `send_lod_data_q_sub_anim+0x54` | Core | Y | TEXTURE-UPLOAD BUDGET, AND THE ONLY HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us > 7499). Times FUN_180061530() against DLL+0x625D60. The divergence netplay has to pin (site 4). |
| 52 | `0x0094D0` | `0x4F7D0` | `rand` | Host | Y | WHOLE-FUNCTION HLE of the ROM's rand: draws from the host Mersenne Twister [DLL+0x623788 + 0x20] (generator FUN_180008DA0), writes g0 and performs the i960 ret itself. NETPLAY-CRITICAL: primary RNG stream. |
| 53 | `0x00F744` | `0x4F800` | `VIC_INT+0x128` | Content | n | Runs the original afterwards. |
| 54 | `0x00E9C0` | `0x4EF40` | `JUDGE_DSP_INT+0x418` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 10). |
| 55 | `0x010BCC` | `0x4EF40` | `RANK_INT+0xC` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 11). |
| 56 | `0x029674` | `0x4EF40` | `chk_ai_switch+0x2C` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 12). |
| 57 | `0x00CDD0` | `0x4F850` | `SEL_DSP+0x5E4` | Content | n | Runs the original afterwards. |
| 58 | `0x00D94C` | `0x4EF40` | `ROUND_INT` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 13). |
| 59 | `0x00DB98` | `0x4EF40` | `SET_INT` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 14). |
| 60 | `0x054C4C` | `0x4EF40` | `name_entry+0xF94` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 15). |
| 61 | `0x054CC0` | `0x4EF40` | `name_entry+0x1008` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 16). |
| 62 | `0x054D00` | `0x4EF40` | `name_entry+0x1048` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 17). |
| 63 | `0x04BD58` | `0x03DF0` | `unp_send_tex_para_sub+0x84` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3DF0) (site 7). |
| 64 | `0x010DB0` | `0x03DF0` | `event_loop+0x5C` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3DF0) (site 8). |
| 65 | `0x010E60` | `0x03DF0` | `event_loop+0x10C` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3DF0) (site 9). |
| 66 | `0x0313EC` | `0x4EF40` | `chk_input+0x14` | Inert | n | Trap runs the original unchanged - the handler is a bare jump to the exec-original tail (0x3D60). A debug probe compiled out of the retail build (site 18). |

**Kind counts:** Core 14, Content 24 (provisional), Inert 18, Removed 9, Host 2.

## What netplay would still need

Unlike Fighting Vipers, VF2 is not a free ride. FV inherited netplay because it already shared
the entire LJ hosting path — `LJHost.cpp`'s `GameLoop`, which is where the netplay session driver
lives (round-prep state machine, barrier, pad injection, desync canary, ~250 lines). VF2 does not
use that host at all: `source/m2ftg/YLAD/VF2.cpp` has its own 700-line `GameLoop` with no `net::`
calls in it.

So the work splits in two, and only the first half is done:

1. **Data** — the addresses above. Adding a `GameHooks` row and a `DwGame` row is mechanical, but
   both tables key off `m2ftg::CurrentGame()`, which is the **LJ** `GameDesc` table (StF/FV/MR
   only). VF2 has no entry there, and `ModuleBase()` in both files resolves the DLL by that name.
2. **Structure** — the netplay driver has to become callable from a second game loop. The clean
   version is to lift it out of `LJHost.cpp::GameLoop` into something both loops drive (and the
   K2 host after that); the quick version is to duplicate it into `VF2.cpp`, which would leave two
   copies of the determinism sequencing to keep in step.

Until (2) exists, the recon above is inert: every determinism helper would work, and nothing
would call them.
