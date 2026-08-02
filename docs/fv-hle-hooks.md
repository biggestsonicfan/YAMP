# Fighting Vipers: the HLE ROM hook table

Recon of `fv-pxd-w64-d3d12_retail.dll` (Ghidra, static RVAs against the preferred base
`0x180000000` — the DLL ships with ASLR, so at runtime every address here is
`module_base + RVA`). Written while porting the Sonic the Fighters netplay work to FV.

FV runs the same Model 2 emulator engine as StF, so the mechanism is identical to the one
already implemented in [`source/m2ftg/LJ/HleHooks.cpp`](../source/m2ftg/LJ/HleHooks.cpp) —
only the addresses and the hook set differ. **FV has 95 hooks to StF's 76.**

## The globals

| What | FV | StF | Note |
|------|-----|-----|------|
| Installer (board bring-up) | `FUN_180049C50` | `FUN_18004B070` | ROM load + trap install + debug-menu init |
| Hook table | `0x1E5840` | `0x1E8870` | 95 × 16-byte `{u32 romOffset, u32 pad, u64 handler}`, in `.data` (writable, no `VirtualProtect`) |
| Hook count | **95** (`0x5F`) | 76 (`0x4C`) | the installer's own loop bound |
| Saved originals | `0x690B40` | `0x68E540` | `u64[]`, filled before the traps are written |
| Program ROM base | `0x9FA2D0` | `0x9F7CD0` | 1 MB i960 ROM, slot 0 |
| Boot state | `0x6BB900` | `0x6B9300` | `2` = board booted; the installer runs at stage 2 |
| "DEBUG MENU" root window | `0x1E5820` | `0x1E8850` | table sits `+0x20` past it in both DLLs |
| ROM symbol table | `0x172390` | `0x1742D0` | **732** × `{u64 addr, char* name}`, sorted; first record `{0xB0, "start_ip"}` |
| Composite-enable flag | `0x6BB772` | `0x6B9172` | exactly the `+0x2600` delta the boot-state global has |
| Exec-original tail | `0x39E0` | `0x39D0` | decodes the saved word and tail-jumps the opcode handler |
| Skip-original tail | `0x3A80` | `0x3A70` | length classifier only (returns 4 or 8) |
| Inert stub (bare `jmp` to the exec tail) | `0x51880` | `0x52D70` | 23 hooks in FV, 14 in StF |

The installer's contract is byte-for-byte StF's, so the whole reversible-restore trick carries
over unchanged: for every record with `romOffset < 0x200000` it saves
`*(u64*)(rom + romOffset)` and writes the trap word `0x4000000 | (i * 4)`. Writing
`(u32)savedWords[i]` back into the ROM word un-does that hook completely, at any time.
A handler's return value is the original instruction's **length** (4 = `MEMA`/`REG`,
8 = `MEMB` with a displacement); `-8` means re-execute, and is used by the vsync spin.

## i960 context offsets (`[DLL+0x58CF60]`)

Confirmed by the vsync trio, which is the same trio StF has:
`+0x64` = `r3`, `+0x68` = `r4`, `+0x6C` = `r5`, `+0x70`/`+0x74`/`+0x78`/`+0x7C` = float regs,
`+0x88`, `+0x8C`, `+0x90`, `+0x94` = further registers, `+0x98` = `g0` (the return register),
`+0x188` = pending-interrupt word, `+0x18C` = interrupt-enable, `+0x1B0`/`+0x1B1` = yield flags.

## Netplay-critical hooks

Everything StF's determinism layer pins has an FV counterpart, and they were all found:

- **Texture-upload budget — hooks 39-42, handler `0x51C20`.** The only hook that reads a wall
  clock: `g0 = (elapsed_us >= budget)`, budget 12 ms normally / 8 ms while master state
  (`DLL+0xAFA30B`) is 6 or 7, timed with `FUN_180064D90()` against `DLL+0x690130`. This is
  `SetTextureBudgetDeterministic`'s target — StF's equivalent is handler `0x52FD0`, also 4 sites.
- **RNG — two streams, not one.** StF has a single host Mersenne Twister behind `rand`; FV has
  two objects hanging off `DLL+0x68E188`:
  - `[+0x20]` — hook 43 (`rand`), whole-function HLE, generator `FUN_180008D00`. StF's is
    `[DLL+0x68BB88 + 0x20]`.
  - `[+0x08]` — hook 30 (`set_vs_cnt_and_stage_num_sel+0x3C`), the VS stage picker (`% 9`).
  **`SeedHostRng` must seed both**, or the stage choice desyncs even when the fighters do not.
- **Backup-RAM / DIP injection — hook 12, handler `0x515E0`.** Writes the whole 0x60-byte block
  from the module config (`DLL+0x1EA590`) into board backup RAM at `[DLL+0x6CC188]+0x91+0x3320`.
  This is where GAME ASSIGNMENTS comes from, and the place to look for FV's analogue of StF's
  seconds-written-into-an-index TIME bug.
- **LJ progress reporting — hook 65, handler `0x52020`** (`DLL+0x1EB5A0 +0x1674/+0x1678/+0x167C`).
- **Self-test / checksum bypass — hooks 15 and 16, handler `0x51770`** (force `r3 = 1`). Two
  sites in FV where StF has three. A modified ROM will not boot without them.
- **Frame yield / vsync — hooks 2 and 5-7**, host frame counter `DLL+0xAFA2E0`.

## The table

`Repl` = the handler replaces the original instruction (ends in the inline 4/8 length
classifier); `n` = it runs additively, ending in a call or jump to the exec-original tail.

| # | ROM | Handler | Site (AM2 symbol) | Kind | Repl | What it does |
|---|-----|---------|-------------------|------|------|--------------|
| 0 | `0x012398` | `0x51240` | `init_fix+0x74` | Core | Y | Forces g0 = 0 in the board hardware-init check. |
| 1 | `0x007954` | `0x512A0` | `main+0x914` | Core | Y | REQUIRED FOR ANY PICTURE. Sets the module's composite-enable flag (DLL+0x6BB772 - StF's 0x6B9172 plus the same +0x2600 delta as the boot-state global) and clears the 0x2000-byte host tile/sprite buffer at [DLL+0xAFA2D8]. |
| 2 | `0x007958` | `0x51320` | `main_loop` | Core | Y | Per-frame yield: clears r3, raises ctx+0x1B0 and calls the host yield (FUN_18004E990) every main-loop iteration. |
| 3 | `0x002238` | `0x513B0` | `interrupt_wait` | Core | Y | Interrupt handshake: raises the pending-interrupt bit (ctx+0x188 \|= 1) when ctx+0x18C bit 0 is armed. |
| 4 | `0x002240` | `0x51420` | `interrupt_wait+0x8` | Core | Y | Second half of the interrupt handshake: host yield + ctx+0x1B0, then skips the original (returns 8). |
| 5 | `0x0118D4` | `0x51450` | `interrupt_wait_b+0x88` | Core | Y | Vsync wait: supplies the host frame counter (DLL+0xAFA2E0) in g0 and raises the pending-interrupt bit. |
| 6 | `0x0118DC` | `0x514E0` | `interrupt_wait_b+0x90` | Core | Y | Vsync wait: supplies the host frame counter in r3. |
| 7 | `0x0118E4` | `0x51550` | `interrupt_wait_b+0x98` | Core | Y | Vsync wait loop: returns -8 to re-execute until the host frame counter advances (r3 != g0). |
| 8 | `0x001A88` | `0x03A80` | `ucb_adr_init+0x4` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 9 | `0x04AA74` | `0x515C0` | `send_tex_default+0x17C` | Core | Y | Texture DMA completion: copies r4 into r3 and skips the original. |
| 10 | `0x04AABC` | `0x515C0` | `send_tex_default+0x1C4` | Core | Y | Texture DMA completion: copies r4 into r3 and skips the original. |
| 11 | `0x04AB04` | `0x515C0` | `send_tex_default+0x20C` | Core | Y | Texture DMA completion: copies r4 into r3 and skips the original. |
| 12 | `0x00708C` | `0x515E0` | `main+0x4C` | Host | Y | Injects the whole 0x60-byte backup-RAM / DIP block from the module config (DLL+0x1EA590: +4, +5, +9, +0xA) into the board's backup RAM at [DLL+0x6CC188]+0x91+0x3320..0x3378, mirroring it into DLL+0xB96600. THE GAME ASSIGNMENTS / DIP INJECTOR - StF's equivalent is its hook 8 (set_window_data+0x564). |
| 13 | `0x004128` | `0x03A80` | `chg_pol_color_send+0xC8` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 14 | `0x063DF8` | `0x51760` | `kill_osage_sub+0x30` | Content | Y | Returns 0x28, skipping ten instructions of the osage (cloth/hair) term - FV's twin of StF's calc_kaze hook. |
| 15 | `0x007944` | `0x51770` | `main+0x904` | Core | Y | Forces r3 = 1 to pass the board self-test / checksum path. A modified ROM will not boot without these. |
| 16 | `0x008234` | `0x51770` | `WARNING_INT+0x14` | Core | Y | Forces r3 = 1 to pass the board self-test / checksum path. A modified ROM will not boot without these. |
| 17 | `0x0089F0` | `0x51810` | `ADV_FBI_PIC_INT+0x88` | Content | Y | Forces r3 = 0 in the FBI-warning attract picture. |
| 18 | `0x041F04` | `0x517E0` | `sound_request_special` | Host | Y | WHOLE-FUNCTION HLE: routes the special (voice / announcer) sound request in g0 to the host mixer, then performs the i960 ret itself (FUN_1800248D0) and returns 0. |
| 19 | `0x00B784` | `0x51880` | `GAME_INT+0x8` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 20 | `0x0024D0` | `0x51880` | `player_entry+0x14` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 21 | `0x008864` | `0x51890` | `ADV_DSP+0x98` | Host | n | In the module's VS/host context (config+0xA), writes 0x30 into emulated RAM 0x500704, then runs the original. |
| 22 | `0x0025A8` | `0x518B0` | `pushed_st1_data_bd_ex+0x54` | Host | n | In the module's VS/host context (config+0xA), zeroes ctx+0x88 and skips the original; otherwise runs it unchanged. |
| 23 | `0x0025D8` | `0x518B0` | `random_check1` | Host | n | In the module's VS/host context (config+0xA), zeroes ctx+0x88 and skips the original; otherwise runs it unchanged. |
| 24 | `0x002714` | `0x518B0` | `pushed_st2_data_bd_ex+0x54` | Host | n | In the module's VS/host context (config+0xA), zeroes ctx+0x88 and skips the original; otherwise runs it unchanged. |
| 25 | `0x002750` | `0x518B0` | `random_check2` | Host | n | In the module's VS/host context (config+0xA), zeroes ctx+0x88 and skips the original; otherwise runs it unchanged. |
| 26 | `0x00AA98` | `0x51920` | `SEL_INT` | Host | n | Raises host event flag 0x2000 and flushes the queued host sound commands on entry to character select, then runs the original. |
| 27 | `0x00AF2C` | `0x51880` | `SEL_DSP+0x10` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 28 | `0x00B098` | `0x51880` | `sel_dsp_next+0x20` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 29 | `0x00B124` | `0x519E0` | `sel_wait_chk+0x40` | Host | n | Raises host event flag 0x4000 (select wait), then runs the original. |
| 30 | `0x00B648` | `0x51A00` | `set_vs_cnt_and_stage_num_sel+0x3C` | Content | n | VS mode only: picks the stage with the HOST MT RNG (generator [DLL+0x68E188+0x08], value % 9 through a 9-byte table) instead of the ROM's own sequence, then runs the original. NETPLAY-CRITICAL: second RNG stream, must be seeded. |
| 31 | `0x00B4C4` | `0x51A70` | `_draw_lp_d+0x78` | Content | n | Rewrites the draw-loop word at [RAM 0x500814]: clears bits 1-2 and ORs 0x1FC000, then runs the original. |
| 32 | `0x00E150` | `0x51880` | `JUDGE_DSP_INT+0x5D0` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 33 | `0x00EC88` | `0x51880` | `VIC_INT` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 34 | `0x00F014` | `0x51880` | `VIC_INT+0x38C` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 35 | `0x00F150` | `0x51880` | `VIC_INT+0x4C8` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 36 | `0x00EAC0` | `0x51B10` | `vs_game_continue_check_ex+0x18` | Host | Y | VS mode only: 2P continue/credit bypass - redirects the i960 IP to 0xF7BC and ORs bit 0/2 into the credit words at RAM 0x500248 / 0x50024C. |
| 37 | `0x00F814` | `0x51880` | `vs_conti+0x58` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 38 | `0x0349EC` | `0x51880` | `MES_ROUND_INT+0x4` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 39 | `0x04C3D8` | `0x51C20` | `send_beta_data+0x138` | Core | Y | TEXTURE-UPLOAD BUDGET, AND THE ONE HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us >= budget), where the budget is 12 ms normally and 8 ms while master state (DLL+0xAFA30B) is 6 or 7. Times FUN_180064D90() against DLL+0x690130. A fast machine and a slow one do different amounts of work in the same emulated frame - the exact divergence StF's SetTextureBudgetDeterministic pins. |
| 40 | `0x04C550` | `0x51C20` | `send_lod_data+0xE0` | Core | Y | TEXTURE-UPLOAD BUDGET, AND THE ONE HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us >= budget), where the budget is 12 ms normally and 8 ms while master state (DLL+0xAFA30B) is 6 or 7. Times FUN_180064D90() against DLL+0x690130. A fast machine and a slow one do different amounts of work in the same emulated frame - the exact divergence StF's SetTextureBudgetDeterministic pins. |
| 41 | `0x04C7E4` | `0x51C20` | `send_lod_data_q_sub_norm+0x54` | Core | Y | TEXTURE-UPLOAD BUDGET, AND THE ONE HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us >= budget), where the budget is 12 ms normally and 8 ms while master state (DLL+0xAFA30B) is 6 or 7. Times FUN_180064D90() against DLL+0x690130. A fast machine and a slow one do different amounts of work in the same emulated frame - the exact divergence StF's SetTextureBudgetDeterministic pins. |
| 42 | `0x04C920` | `0x51C20` | `send_lod_data_q_sub_anim+0x54` | Core | Y | TEXTURE-UPLOAD BUDGET, AND THE ONE HOOK THAT READS A WALL CLOCK: g0 = (elapsed_us >= budget), where the budget is 12 ms normally and 8 ms while master state (DLL+0xAFA30B) is 6 or 7. Times FUN_180064D90() against DLL+0x690130. A fast machine and a slow one do different amounts of work in the same emulated frame - the exact divergence StF's SetTextureBudgetDeterministic pins. |
| 43 | `0x006DC8` | `0x51CE0` | `rand` | Host | Y | WHOLE-FUNCTION HLE of the ROM's rand: draws a 16-bit value from the host Mersenne Twister (generator FUN_180008D00, state object [DLL+0x68E188+0x20]), writes it into g0 and performs the i960 ret itself. NETPLAY-CRITICAL: primary RNG stream. |
| 44 | `0x038B60` | `0x51EA0` | `select_init_wait+0x4C` | Content | Y | Zeroes ctx+0x94 in the select-init wait. |
| 45 | `0x038B70` | `0x51EA0` | `select_init_wait+0x5C` | Content | Y | Zeroes ctx+0x94 in the select-init wait. |
| 46 | `0x038E54` | `0x03A80` | `old_set_skip+0x1C` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 47 | `0x038E58` | `0x03A80` | `old_set_skip+0x20` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 48 | `0x038E5C` | `0x03A80` | `old_set_skip+0x24` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 49 | `0x038E70` | `0x03A80` | `old_set_skip+0x38` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 50 | `0x039E04` | `0x03A80` | `old_set_skip_P2+0x1C` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 51 | `0x039E08` | `0x03A80` | `old_set_skip_P2+0x20` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 52 | `0x039E0C` | `0x03A80` | `old_set_skip_P2+0x24` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 53 | `0x039E20` | `0x03A80` | `old_set_skip_P2+0x38` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 54 | `0x038E74` | `0x03A80` | `old_set_skip+0x3C` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 55 | `0x039E24` | `0x03A80` | `old_set_skip_P2+0x3C` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 56 | `0x0531E4` | `0x51D10` | `adv_fade_in6+0x8` | Host | n | When r3 == 0x15, clears 0xC00 bytes of the host tile buffer at [DLL+0xAFA2D8]+0x9024 and raises its +0x98895 flag, then runs the original. |
| 57 | `0x07BAF8` | `0x51D60` | `fcc_loop+0xBC` | Content | n | Divide-by-zero guard: if either float register ctx+0x70 / +0x74 is zero, returns 0x30 (skips twelve instructions); otherwise runs the original. |
| 58 | `0x07B9B4` | `0x51D90` | `finish_coli_check+0xB8` | Content | n | Divide-by-zero guard: if either float register ctx+0x78 / +0x7C is zero, returns 0x38; otherwise runs the original. |
| 59 | `0x05046C` | `0x51DC0` | `name_init+0x68` | Content | n | Name-entry: when r3 == 8, redirects the i960 IP to ROM 0x504A4; otherwise runs the original. |
| 60 | `0x009CBC` | `0x03A80` | `ADV_REPLAY_WAIT1B+0x10` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 61 | `0x009CC8` | `0x51880` | `ADV_REPLAY_WAIT1B+0x1C` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 62 | `0x009CCC` | `0x03A80` | `ADV_REPLAY_WAIT1B+0x20` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 63 | `0x031BA8` | `0x51F00` | `kuc_arm+0x18` | Host | n | 2P join check: lets the round start when either player's state byte is 9 and the 8-slot host credit array (DLL+0xC92BB8) has a free slot, by copying ctx+0x8C into ctx+0x90; then runs the original. |
| 64 | `0x00C4E0` | `0x51FE0` | `ROUND_INT` | Host | n | Banks the 2P-join flag (DLL+0x6C4058+2) once the credit word at RAM 0x5000A2 exceeds 9, then runs the original. |
| 65 | `0x00ED0C` | `0x52020` | `VIC_INT+0x84` | Host | n | Reports the match result to the Lost Judgment host for minigame progress: raises event flag 0x200 and writes round+1 and both players' character ids to DLL+0x1EB5A0 +0x1674/+0x1678/+0x167C, when master state (DLL+0xAFA30B) is 9. StF's twin writes the same three fields to DLL+0x1EE4A0+0x1674. |
| 66 | `0x00E014` | `0x51880` | `JUDGE_DSP_INT+0x494` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 67 | `0x0114BC` | `0x51880` | `RANK_INT+0xC` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 68 | `0x00B4C0` | `0x51880` | `_draw_lp_d+0x74` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 69 | `0x026B64` | `0x51880` | `damage_unit+0x120` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 70 | `0x026B70` | `0x51880` | `damage_unit+0x12C` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 71 | `0x026C44` | `0x51880` | `damage_unit+0x200` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 72 | `0x026C9C` | `0x51880` | `damage_unit+0x258` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 73 | `0x078888` | `0x51880` | `cage_break_init` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 74 | `0x027160` | `0x51880` | `zibaku_ckeck+0x64` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 75 | `0x031D58` | `0x51880` | `kuc_motion+0x74` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 76 | `0x0182E0` | `0x51880` | `calc_unit_mat+0x1180` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 77 | `0x005D6C` | `0x03A80` | `set_obj_fifo+0x34` | Removed | Y | Instruction deleted - the handler IS the bare skip tail (0x3A80), so nothing runs in its place. |
| 78 | `0x052C40` | `0x51880` | `adv_special_command+0x1C` | Inert | n | Trap runs the original unchanged - the handler is a bare jmp to the exec-original tail (0x39E0). A debug probe whose body was compiled out of the retail build. |
| 79 | `0x01EFD0` | `0x52110` | `camera_control+0x130` | Content | Y | Camera clamp: ctx+0x94 = min(ctx+0x90 + K, limit), using two rdata float constants. |
| 80 | `0x01B2EC` | `0x521A0` | `option_control+0xAC` | Content | n | When config+0x8 is set, forces ctx+0x90 = 2. |
| 81 | `0x01CB44` | `0x521A0` | `copy_option_data+0x18` | Content | n | When config+0x8 is set, forces ctx+0x90 = 2. |
| 82 | `0x012678` | `0x521A0` | `efc_rob_set_set+0x1E0` | Content | n | When config+0x8 is set, forces ctx+0x90 = 2. |
| 83 | `0x06D128` | `0x521C0` | `kanban_dsp:+0x64` | Content | n | When config+0x8 is set, forces r3 = 2. |
| 84 | `0x06D338` | `0x521E0` | `trailer_dsp+0x3C` | Content | n | When config+0x8 is set, forces ctx+0x68 = 2. |
| 85 | `0x06D748` | `0x521E0` | `trailer_dsp_b0+0x64` | Content | n | When config+0x8 is set, forces ctx+0x68 = 2. |
| 86 | `0x02F3CC` | `0x52200` | `adv_subobj_set+0xF00` | Content | n | When config+0x8 is set, forces ctx+0x6C = 2. |
| 87 | `0x02FF3C` | `0x52200` | `adv_subobj_set+0x1A70` | Content | n | When config+0x8 is set, forces ctx+0x6C = 2. |
| 88 | `0x0300C4` | `0x52200` | `adv_subobj_set+0x1BF8` | Content | n | When config+0x8 is set, forces ctx+0x6C = 2. |
| 89 | `0x0337D4` | `0x52220` | `set_rank_total_win` | Content | n | Ranking arithmetic, stash half: saves ctx+0x70 into host scratch DLL+0xC92BC8. |
| 90 | `0x033814` | `0x52260` | `time_points_zero+0x4` | Content | n | Ranking arithmetic, stash half: saves ctx+0x78 into host scratch DLL+0xC92BC4. |
| 91 | `0x033824` | `0x522A0` | `time_points_zero+0x14` | Content | n | Ranking arithmetic, stash half: saves ctx+0x70 into host scratch DLL+0xC92BC0. |
| 92 | `0x020504` | `0x52240` | `cc_ranking+0x184` | Content | n | Ranking arithmetic, restore half: g0 = host scratch DLL+0xC92BC8. |
| 93 | `0x02054C` | `0x52280` | `cc_ranking+0x1CC` | Content | n | Ranking arithmetic, restore half: g0 = host scratch DLL+0xC92BC4 * 2. |
| 94 | `0x02059C` | `0x522C0` | `cc_ranking+0x21C` | Content | n | Ranking arithmetic, restore half: g0 = host scratch DLL+0xC92BC0. |
**Kind counts:** Core 17, Content 25, Inert 23, Host 15, Removed 15.

## The rest of the per-game addresses

Everything `HleHooks.cpp` and `DebugWindows.cpp` used to hardcode for StF, now resolved for FV
and living in the `GameHooks` / `DwGame` descriptors:

| What | FV | StF | How it was found |
|------|-----|-----|------------------|
| DEBUG MENU "RESET" handler | `0x4B3E0` | `0x4C840` | menu item 5; writes run-state 0, calls the board init |
| Board init (what RESET re-runs) | `FUN_180049840` | — | sets the ctx/ROM/RAM base globals |
| Run state | `0x6C3FE0` | `0x6C19E0` | written 2 by the installer, 0 by RESET |
| Stripped `return 0` stub | `0x4B320` | `0x4C780` | FV's is the REGS item |
| Emulated memory map | `0x170750` | `0x172660` | read out of the dispatch in `FUN_1800522E0` |
| i960 context pointer | `0x58CF60` | `0x58A960` | already in `GameDesc` |
| RNG holder | `0x68E188` | `0x68BB88` | `[+0x20]` = `rand`, `[+0x08]` = VS stage picker |
| Texture-budget handler | `0x51C20` | `0x52FD0` | 4 records apiece |
| ROM `frame_counter` | `0x500020` | `0x500020` | **the same address in both** — measured, see below |

The memory map deserves a note, because a plausible-looking wrong answer was available. Scanning
`.rdata` for a `0x70`-stride table of code pointers matches at dozens of offsets — the region is
densely packed with pointers, so almost any alignment "works". The real base came from the DLL's
own dispatch in `FUN_1800522E0`, which indexes an array of 8-byte pointers by `index * 0xE`:

```c
uVar1 = (uVar4 >> 0x1c) + 0x30;
if (uVar4 < 0x3000000) uVar1 = uVar4 >> 0x14;
(*(code *)(&PTR__guard_check_icall_180170768)[(ulonglong)uVar1 * 0xe])(uVar4, local_28);
```

Two such call sites — one writing `u16`, one writing `u32`, `0x10` apart — pin the slot layout
(`r8 w8 r16 w16 r32 w32` at `+0x00 +0x08 +0x10 +0x18 +0x20 +0x28`) and therefore the base at
`0x170750`. The scan's best guess was `0x170730`, off by exactly one slot. The confirmation that
it is right: at `0x170750` the ROM regions have all three write slots pointing at the bare-`ret`
stub — a read-only ROM, which is what a Model 2 program ROM is — and 28 unmapped regions share
that stub. Verified again at runtime: `ReadEmulatedRam32` goes through this table and returns
live values for FV.

**`frame_counter` is at `0x500020` in both games.** That is not a coincidence — Sonic the
Fighters was built on Fighting Vipers' engine and inherited the low-RAM global block — but it is
also not something the rest of the layout would let you assume, since the module reads
`0x50004C` / `0x500064` / `0x5000A2` / `0x50016C` / `0x500248` / `0x500704` in FV and none of
those in StF. Measured rather than assumed: both games logged advancing it by exactly 1 per
`module_main` call, over two sample bursts 340 frames apart.

## Status

Netplay for Fighting Vipers is enabled and **verified synced over RPCN on a LAN** (2026-08-01).
The determinism set is the same one Sonic the Fighters uses — board reset, host-RNG seeding, the
texture-budget pin, the hook mask forced to the compiled-in default, and `frame_counter` as both
round anchor and desync canary — with one addition FV needed: seeding *both* host twisters.

Worth re-checking as play widens, because a short session will not necessarily reach them: VS
mode specifically (hook 30 is the only consumer of the second RNG stream, so the stage choice is
the thing that would disagree if that seeding ever regressed), and long matches across several
rounds, where a slow divergence would show as the desync detector firing on a named frame rather
than as anything visible.

## Still open

- **GAME ASSIGNMENTS → DAMAGE / TIME.** The block itself is located: hook 12 (`0x515E0`) writes
  the operator settings to board SRAM at `[DLL+0x6CC188]+0x91+0x3320`, and the guest-side copy
  lives at emulated RAM `*(u32*)0x50016C + 0x3320` — the same `+0x3320` pairing StF has. The
  injector supplies block bytes `0x04`, `0x08-0x0E`, `0x20-0x22`, `0x30-0x31`, `0x36-0x3C` and
  leaves `0x32-0x35` verbatim, which is where StF keeps its `game_assignments_flag` (`+0x33`,
  DAMAGE = bit `0x80`). Whether FV agrees needs one TEST-menu experiment: change DAMAGE in the
  service menu and watch which byte of the block moves. Until then `UpdateDamageAssignment()`
  stays StF-only.
- **FV's game debug flag** (StF: emulated RAM `0x508000`, XOR `0x24`) is unverified, so
  `UpdateGameDebugFlag()` and the `dw` DEBUG MENU panes stay StF-only. FV *has* the tree — root
  window `0x1E5820`, its own 732-entry symbol table — so porting the panes is possible; the
  `RVA_NOTES` annotations are StF handler addresses and simply would not match.
