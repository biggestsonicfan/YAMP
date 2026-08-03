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
| Work-RAM bank (guest `0x500000`) | `0xA80050` | `0xAFA2E0` | — |
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
- **Frame yield / vsync — hooks 2 and 5-7.** `DLL+0xA80050` is **not** a host frame counter, as
  this said until 2026-08-02. It is the host storage of **guest `0x500000`**, the ROM's own vblank
  byte: board init (`FUN_180047610`) memsets 1 MB there, points the guest-`0x500000` bank pointer
  `ctx+0x18` at it, and sets the RAM base `DLL+0x880030` to `0x580050` — and `0x580050 + 0x500000`
  is exactly `0xA80050`. Hooks 5 and 6 are plain emulated `ldob byte_500000` into g0 (`ctx+0x98`)
  and r3 (`ctx+0x64`); hook 7 returns `-8` to re-execute hook 6 until the two differ. The byte is
  incremented by the ROM's **own timer ISR** (ROM `0xCE0`-`0xCEC`, `ldib`/`addi 1`/`stib`).
  Nothing outside the instruction stream writes it. The mislabel is what sent the previous pass
  looking for a host thread to race against; there is none.

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

## SOLVED 2026-08-02: it was never a divergence, it was frame NUMBERING

**The two simulations were always bit-identical. Only their frame indices disagreed.**

Caught by the timer trace below, on a live two-peer round. Host and guest at netplay frames 0 and
1 are *identical in every field* - same ROM counter, same work-RAM hash, same 2,988 instructions
executed, same timer counts. Then:

```
host   f=2 rom=11 chk=0x6A9A79A0 ins=2988 rearm=0 t3=6363,1,2988
guest  f=2 rom=10 chk=0x4647F415 ins=0    rearm=0 t3=9351,1,0      <- executed NOTHING
guest  f=1 rom=10 chk=0x4647F415                                    <- ...so it resent frame 1's value
```

The desync report confirms it exactly: `local 1788508576, peer 1179120661` is
`0x6A9A79A0` vs `0x4647F415` - the guest submitted, as frame 2, the value the host computed at
frame **1**. A 64 KB FNV hash matching bit for bit is not a coincidence.

**`module_main` returning is a HOST event; the emulated board completing a frame is a GUEST one,
and they are not the same.** The CPU loop returns the moment the ROM's yield flag is set, so a call
can execute almost nothing. Measured over ~12,000 frames on two machines, **~5% of VF2's
`module_main` calls execute nothing at all** - canary, all four timer channels and the ROM frame
counter all unmoved - and *which* calls stall is host-timing-dependent. Netplay counted every call
as a frame, so each peer's netplay-frame-to-emulated-frame mapping drifted independently. One frame
apart, oscillating, both peers taking turns in the lead: exactly the reported symptom.

**Why StF and FV never showed it:** they stall too, but *only during boot*. Measured on the same
build - StF's 52 stalls all occur at ROM frame 0-1, then it advances exactly +1 per call for 1,428
consecutive calls. A round cannot start until the board is booted and anchored at frame 8, so their
stalls are always outside a round. VF2's are spread evenly across the whole run (16-17 per decile).

**The fix** is in `NetSession::EndFrame`: a call that advanced nothing no longer consumes a netplay
frame. The test is conservative - a stall requires the canary unchanged AND no timer counted down
AND none was re-armed (a re-arm is a guest store, so it proves the CPU ran). On those 12,000 frames
it caught 227 and 171 stalls with **zero false positives**; the ~14 per run it misses are treated as
real frames, which is the safe direction. Skipping leaves `m_frame` put and the next host frame
re-runs the same netplay frame with the same inputs - the identical path an ordinary lockstep stall
already takes.

**This also retires the reasoning behind VF2's work-RAM hash canary.** The doc used to argue VF2's
ROM frame counter "jitters" and is "bookkeeping ABOUT the frame, not part of the simulation". The
counter was telling the truth - those calls really did run no frame - and switching to a hash hid
the signal rather than fixing it. The hash is still the stronger canary and should stay; the
*justification* recorded for it was wrong.

Still to confirm on two machines: that a round now survives past frame 2.

## Netplay status: ON

`DwGame::netplayReady = true` since 2026-08-02. VF2 drives the shared `m2ftg::NetSession`, every
determinism helper works, and with the frame-accounting fix above a two-peer round runs with **no
reported desyncs** - verified on two machines the same day, with each peer stalling where it should.

`[Netplay] ForceUnsupported` is no longer needed for VF2 and goes back to being what it was meant to
be: a diagnostic for the *next* game that is measured but not yet trusted. `TimerTrace` should be
left off for ordinary play - it costs a file open/append/close per emulated frame.

Still worth doing: **StF and FV have not been re-verified on two machines** since either the RNG
seeding fixes or this frame-accounting change. Neither should be affected (their stalls are
boot-only, before a round can start) but neither has been shown. A VS-mode round is the case to
run, because that is what exercises StF's second RNG stream.

### What the divergence looked like from outside

The evidence below is what was collected before the cause was known. It is all consistent with the
frame-numbering explanation above and is kept because the *shape* of it is what a numbering fault
looks like from a RAM diff - worth recognising again.

Measured by dumping the full 1 MB of work RAM on both peers at the same netplay frame and diffing
byte for byte. The differences are *small deltas on moving values* - floats a hair apart, counters
off by one - not garbage and not unrelated values:

```
0x500020   0E -> 0F    frame_counter, delta +1
0x500024   F5 -> F4    countdown, delta -1
0x503030   0B -> 0C    delta +1
0x59C174+              the ranking display, scrolled by one entry
```

Everything else follows from that one-frame offset. It **oscillates** rather than drifts - the two
peers trade the lead, each missing a VBLANK every ~20 frames at different frames - which is why
gameplay looks identical (CPU behaviour and round timers matched across two full rounds) and why
it still must not ship: inputs applied a frame apart can change an outcome.

### The mechanism, as far as it is understood

The emulated i960's timers are driven by **instruction count, not wall clock**:

- `FUN_18004C170` handles writes to guest `0xF00000 + idx*4` and converts the period the ROM asks
  for into instructions: `count = ticks * (576000 / 25000000)`, i.e. 0.02304 instructions per
  25 MHz tick. Constants at `0x180166208` (576000, instructions/sec) and `0x18016620C` (25000000).
  It stores the count at `ctx+0x194 + idx*8` and sets an enable byte at `ctx+0x190 + idx*8`.
- `FUN_1800210C0` is the CPU loop. It executes instructions in batches of **12**, then decrements
  all four timer channels by 12 and raises an interrupt (`FUN_180021370`) for any that expire and
  whose mask bit is set in `ctx+0x18C`. It returns - ending the frame - the moment the ROM's
  frame-yield flag `ctx+0x1B0` is set (by hook 2 `main_loop` and hook 4).

Two details of that loop matter and were not obvious from the outside. The **yield check is inside
the batch**, so a frame ends mid-batch and the instructions of that final partial batch are never
charged to any timer. And the batch counter is a local reset on entry, so each frame starts a fresh
batch. Both are deterministic given an identical instruction stream - they are quantisation, not
noise - but they mean "instructions executed" and "instructions charged" differ by up to 11 per
frame.

**A small difference in instructions executed flips whether the timer interrupt lands before or
after the yield** - which is exactly a one-frame counter difference.

### The gap

**What host-dependent input changes the instruction count is still not identified**, but the search
space is now much smaller, because the obvious suspect is eliminated *by mechanism* rather than by
not having been shown:

The vsync wait spins (hook 7 returns `-8` to re-execute) on **guest `0x500000`**, and that byte is
incremented by the **ROM's own timer ISR** at ROM `0xCE0`-`0xCEC`. The spin therefore exits on an
emulated interrupt raised by the emulated instruction counter. There is no host thread in it. The
earlier reading of hooks 5-7 as "supplies the host frame counter" was a mislabel of `DLL+0xA80050`
(see the note under Netplay-critical hooks); so was the older `gs::sm_context->frame_counter`
guess. Neither describes anything real.

What is left that can still differ per host: the **texture-upload budget** (hooks 48-51), the only
wall-clock reader in the module. Netplay pins it, and that pin has been verified applied to 4 of 4
sites - but "applied" is not "effective", and it is now the only known candidate. Verifying it
means checking that a pinned peer's per-frame instruction count is *insensitive* to how long an
upload actually took, which the trace below measures directly.

### Ruled out - do not re-investigate

| Hypothesis | How it was eliminated |
|---|---|
| RNG not seeded | All five streams now seeded (see below), before the reset AND at the barrier |
| Cabinet settings differ | Verified identical on both machines; divergence persisted |
| Board reset leaves stale RAM | `FUN_180047500` → `FUN_180047610` memsets the full 1 MB |
| Texture budget not pinned | Verified patching 4 of 4 sites on the right handler |
| Wrong desync canary | Work-RAM hash diverges too; it is a real difference, not a measurement artefact |
| Anchor / barrier misalignment | Both peers anchor at `frame_counter=8` and hold; no leak during the hold |
| Reset differs from StF/FV | Same routine compiled three times - same call sequence and constants |
| The vsync spin races a host thread | Its exit condition is guest `0x500000`, incremented by the ROM's own timer ISR at ROM `0xCE0`-`0xCEC`. `DLL+0xA80050` is that byte's host storage, not a host counter - see above. No host thread is involved. |
| Timer-channel layout was guessed | Read out of the CPU loop's own four decrement blocks, then confirmed live: counts step in multiples of 12, enables read 0/1, IP parks at `0x009FB4` (the `main_loop` yield site), mask reads `0x421`. |

### The timer trace: BUILT, and validated on one machine

The per-frame timer readout this section used to propose now exists.

- `m2ftg::ReadI960Timers` (`source/m2ftg/DebugWindows.cpp`) reads the live context: enable byte
  `ctx+0x190 + n*8`, count `ctx+0x194 + n*8`, plus `ctx+0x188` pending, `ctx+0x18C` mask,
  `ctx+0x08` IP and `ctx+0x1B0` yield. The layout is not pattern-matched - it is read straight out
  of the CPU loop's four identical decrement blocks.
- `NetSession::EndFrame` emits one line per emulated frame to **`yampnet.log`**, in and out of a
  round. That sink survives a Release build and opens/closes per line, so the log is complete even
  if the process is killed and can be read live over a share.
- Two INI-only switches under `[Netplay]`, both default 0: **`TimerTrace=1`** turns the trace on,
  and **`ForceUnsupported=1`** lets VF2 open a round despite `netplayReady = false` (it is held
  back *because* it diverges, so observing that divergence needs the override). Neither has UI.

Line format, fixed field order so two peers diff line-for-line:

```
timers f=<netplay frame, or - outside a round> rom=<ROM frame_counter> chk=<state hash>
       ins=<largest decrease> rearm=<bitmask of channels that ROSE> ip=<IP> yield=<flag>
       irq=<pending>/<mask> t0=<count>,<enabled>,<delta> t1=... t2=... t3=...
```

**Baseline measured 2026-08-02**, VF2 attract mode, ~5,200 frames, one machine:

| Channel | Behaviour |
|---|---|
| `t0` | expires early in boot, never re-armed (`count=-9, enabled=0` forever) |
| `t1`, `t2` | re-armed to a constant every frame; alternate between two values (`21987`/`22227` and `21759`/`21999`). Carry no per-frame instruction signal. |
| `t3` | **the live frame timer.** Counts down 11,000-21,600 per frame; expires and is re-armed by the ROM roughly every second frame (`rearm=8`). |

The headline number: **instructions per emulated frame are not constant even on one machine** -
they ranged from ~11,000 to ~21,600 across the run. Against a `t3` reload period of ~24,000
instructions, a frame-to-frame swing that large is far more than enough to move the expiry across
the yield boundary. So the question the two-peer run has to answer is precisely: **is that
variation a function of the simulation (fine - both peers reproduce it) or of the host (fatal)?**

`rearm` is the field to diff first. Two peers that re-arm different channels on the same `f=` have
already diverged, and it says so in one character.

### Running it on two peers

1. Both machines: build, then add `TimerTrace=1` and `ForceUnsupported=1` under `[Netplay]` in
   `settings.ini` **next to YAMP.exe** (not the game's directory).
2. Delete `yampnet.log` on both, start VF2 on both, run a round.
3. `grep '^\[.*\] timers f=[0-9]' yampnet.log` on each - that drops the pre-round `f=-` lines - and
   diff. The first differing `f=` is the frame to investigate, and `ins`/`rearm`/`t3` say by how
   much and in which channel.

Note the round is *expected* to be wrong under `ForceUnsupported`: that is the point.

### Still missing: the work-RAM snapshot ring

Separately from the trace, this doc used to say per-frame 1 MB work-RAM snapshots could be
recovered from history. **They cannot** - checked 2026-08-02: `git log --all -S"yamp_ram"` matches
only this file's own text, no source; `-S` over `Snapshot`/`WorkRam`/`DumpWorkRam` finds nothing in
`source/`; the stash is empty, the reflog has no intermediate commit, and none of the 41 dangling
blobs from `git fsck` contain any of those strings. The functions were deleted from the working
tree before `bce4b1e` was made and never committed. Rewriting them, if needed, starts from
`DW_VF2.rvaRamBasePtr = 0x880030` (host base of the 1 MB, `0x580050`, so guest G is at `0x580050+G`)
and the same `EndFrame` hook point the trace uses. The ring is the part that matters: a divergence
surfaces a few frames after the frame it names, so dumping at detection time captures two different
instants and the diff is noise.

### RNG: three real defects fixed along the way

Not the cause of the remaining lag, but genuine and worth knowing about:

1. **Only some streams were seeded.** Each module builds its generator holder the same way (VF2
   `FUN_18005EF00`, StF `FUN_180064820`): allocate, write a count of **five**, vector-construct
   five `0x18`-byte objects, seed them from the performance counter. State pointers sit at `+0x08`
   within each object, so the slots are `0x08 + N*0x18` = `0x08, 0x20, 0x38, 0x50, 0x68`. YAMP
   seeded one (StF) or two (FV, VF2) - the rest kept their **wall-clock** seed. Found by inspecting
   only the HLE handlers, which reveals just the streams the ROM itself draws from.
2. **Seeding happened after the reset.** The reset makes the ROM re-run its whole initialisation,
   and that initialisation draws from the RNG - so those draws came from the wall-clock seed. Now
   seeded before the reset, with a re-seed at the barrier so both peers enter frame 0 identically
   regardless of how many draws each boot consumed.
3. **`SeedHostRng` returned "any stream seeded" rather than "all".** A game whose second generator
   failed its sanity check reported success and started a half-seeded round.

StF hid all of this because its second ROM-facing stream is the VS stage picker (hook 22), gated on
`is_vs_mode` - off by default, so the unseeded generator was never drawn from. **StF and FV have
not been re-verified on two machines since these fixes; a VS-mode round is the case worth testing.**

## Known issue: leaving the service menu corrupts the geometry

**Leaving the board's service menu renders the 3D geometry wrong until YAMP is restarted** -
models still draw, but distorted. StF and FV exit the same menu cleanly, so this is a gap in the
VF2 host, not in the switch wiring or the emulated I/O board.

Established by measurement: exiting soft-resets the emulated board. The ROM's `frame_counter` runs
continuously to ~1215 and then restarts from 0 on the exit press. The i960 therefore re-runs its
init and re-uploads its geometry believing it is on a fresh machine, while the host's boot-time
state is never rebuilt - the two disagree about what is loaded, which is why the damage is
geometry-shaped rather than a crash or a blank screen.

Two leads are already eliminated, so nobody re-walks them:

- The I/O hook resolves `0x180880020` - the same board pointer the port reader (`FUN_18004A4E0`)
  and the DIP injector (hook 8) use - and only ever ANDs `io[9]`.
- The upload-budget timebase is **not** a stale load-time stamp. It is `TaskM2E`'s own field,
  re-stamped at the top of every frame step (`0x180051045: mov [rbx+0x60],rax`).

Next measurement: whether the DLL's boot-state dword (`0x641890`) stays at 2 while the ROM's
counter restarts. If it does, the DLL's board init never re-ran, and the fix is to notice the
counter going backwards and rebuild whatever the host establishes at boot. Reproduces headlessly
in about three minutes by scripting two short TEST presses (TEST is momentary, not held).

## Netplay and the 2.0 / 2.1 version flag

VF2 ships as two mechanically different games, and two peers on different versions diverge from
identical inputs. The version is `m2ftg_config_t.is_vf20` (config `+0x07`, so `DLL+0x6263F7`), and
the room now carries it: the host publishes its setting as `YAMPNET_ROOM_FLAG_VF2_VERSION20` and
every peer adopts it, exactly as Sonic the Fighters does with DAMAGE.

Two things made this cheap. Adding the bit needed **no plugin change and no ABI bump**, because
yampnet carries `game_flags` verbatim between the room and its peers and never interprets it. And
applying it needs no relaunch, despite `is_vf20` being a launch-time config field: the only reader
is HLE hook 8 (`check_sram_all+0x47C`, handler `0x4ECA0`), the backup-RAM injector, which re-reads
it every time the ROM initialises the board -

```c
bVar5 = bStack_37 | 0x10;
if (DAT_1806263F7 == '\0') {      // is_vf20 clear = version 2.1
    bVar5 = bStack_37 | 0x50;     // ...adds bit 0x40 to the operator byte
}
```

so writing the config byte and letting the board init do the rest uses the game's own mechanism
rather than poking SRAM directly. `NetSession` writes it immediately **before** the round-start
`ResetBoard()`, which is the only window where it can reach the simulation.

## Also still open

- ~~GAME ASSIGNMENTS -> DAMAGE~~ **does not exist in VF2** - that was an assumption carried over
  from Sonic the Fighters and it is wrong. VF2's desync-relevant cabinet setting is the **2.0 /
  2.1 version flag**, which is handled: see below.
- **VF2's game debug flag** (StF: emulated RAM `0x508000`, XOR `0x24`) is unverified, so
  `UpdateGameDebugFlag()` and the `dw` DEBUG MENU panes stay StF-only. VF2 *has* the tree - root
  window `0x186590`, 8 items at `0x185480`, its own 301-entry symbol table.
- **Hook hit-counters read 0 for VF2**, because the instruction-fetch hook that feeds them
  (`Patch.cpp`) is LJ-only. Absent rather than wrong, but it looks broken in the settings panel.
