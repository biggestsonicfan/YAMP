# Virtual On (`omg`) HLE hooks

120 hooks, from the module's own table at RVA **0x476520**, cross-referenced against its ROM symbol
table (RVA 0x4507E0, 202 entries). **Classified 2026-08-05** by reading every handler in Ghidra
(port 5678, `omg-pxd-w64-gog_retail.dll`) and every hook site in IDA (port 7331, `von_prog.bin`).
The live table is in `source/m2ftg/VonHooks.inc`; `-von-hlelist` dumps it, and the settings UI
lists it with per-hook toggles.

## Two corrections this pass, both of which change indices

**The table base was one record low.** It is 0x476520, not 0x476510. The installer settles it:

```c
// FUN_1800048e0(rom_base) - the whole installer
for (i = 0; i < 0x78; i++) {                       // 120 records
    if (*(u32*)(&DAT_180476520 + i * 0x10) < 0x200000) {
        p = rom_base + *(u32*)(&DAT_180476520 + i * 0x10);
        memcpy(&DAT_18068e8c0 + i * 8, p, 8);       // save the original instruction
        *p = i * 4 | 0x4000000;                     // stamp the trap word
    }
}
```

The 16 bytes below that base are `{0, &free_thunk}` — a neighbour, not a record. Reading from
0x476510 invented a 121st hook whose "handler" was the CRT's `free()` and pushed every real hook
one index up. It was not cosmetic: the disable path **writes** the handler column, so ticking that
phantom in the settings list stored the execute-original tail over a live data pointer with many
readers. **Every index below is now one lower than in notes written before 2026-08-05**, and it
matches the index the module encodes in its own trap word (`0x04 << 24 | i * 4`) — which also
silently fixes the invocation counter, since `NoteFetchedWord` decodes that operand directly.

**Nothing in the module ever writes the handler column.** The installer above reads only
`romOffset`. So the table is meaningful the moment the DLL is mapped, and `HleHooks::Update()` now
runs Virtual On's branch **ahead of the boot-state gate** — which is what makes a disabled hook
take effect during boot rather than one frame into it. (The ROM-word games still wait for the gate:
their restore reads the module's save area, which does not exist until the installer has filled it.)

## Reading a handler

Three shapes, and telling them apart is most of the classification:

| the handler … | means |
|---|---|
| ends `FUN_180004980(index)` | does its own work, **then** runs the original instruction |
| returns a length (4 or 8) | has **replaced** the original — it never runs |
| calls `FUN_1800446A0`, returns 0 | performed the i960 `ret` itself — whole-function HLE |

and two shared tails, which is why 67 of the 120 need no reading at all:

- **`+0x070FB0`** — `jmp FUN_180004980`. The ROM instruction runs unchanged: a debug probe whose
  body was compiled out of retail. **63 hooks**, provably Inert from the data.
- **`+0x004A10`** — returns the instruction length without decoding it. The ROM instruction is
  **deleted**. 4 hooks.

`FUN_180004980`'s argument is the hook index, and the original instruction lives at
`DAT_18068e8c0 + index * 8` — the array the installer filled.

## THE BOOT PATH — hooks 0-3 and 17, of which only 1 and 2 are load-bearing

Five hooks sit on the boot path. Which of them the boot actually *needs* was **measured**, not
argued: each was disabled from before `module_start` with `-von-hleoff=<n>` and given 600 frames
under cdb, against an unmodified baseline of 14 attract audio cues and a clean `module_stop -> 0x0`.

| # | site | 600 frames with it disabled | verdict |
|---|---|---|---|
| 0 | `BlackOut+0x190` | reached the frame limit, 14 cues | Host |
| **1** | `SyncGeometrizer.lf+0x44` | **hung: 0 frames, 0 cues, 100% CPU** | **Core** |
| **2** | `synch+0x4` | **hung: 0 frames, 0 cues, 100% CPU** | **Core** |
| 3 | `InitIO+0x24` | reached the frame limit, 14 cues | Host |
| 17 | `InitSelect+0x1C` | reached the frame limit, 14 cues | Host |

The two that hang are the vblank pair, and they fail the same way: the ROM's frame wait is a spin
with no yield in it, so a spin that cannot terminate never returns from `module_main` and takes the
whole host with it. This is the same failure mode as an unmapped read (see `unmapped-read-hangs`),
reached from the other direction.

**Hook 0 is the one the static reading got wrong**, and it is worth recording why. It sets the
flag that the module's own bring-up state machine waits on, which reads exactly like "the board
cannot finish booting without it" — and the board boots anyway. That state machine gates the
module's presentation, not the emulated board.

### 0 — `BlackOut+0x190`, the bring-up handshake (Host, not Core)

The module's own bring-up state machine is `FUN_180072FB0`, a switch on `param+0x58`:

```c
case 0x10:  DAT_1806910df = 0;  DAT_1806910de = 0;  ...   // clear the flags (and the 2-board gate)
case 0x11:  if (FUN_18006a100()) state = 0x12;            // <-- runs the HLE INSTALLER + board reset
case 0x12:  if (DAT_1806910df != 0) { FUN_1800727a0(); state = 0x13; }   // <-- WAITS
```

`DAT_1806910df` is set by exactly one thing: **hook 0's handler**, which the emulated i960 reaches
when the ROM runs `BlackOut+0x190` (the other two writers, `FUN_18005a2c0` and state 0x10, both
write **0**). So the module installs the traps and then sits in state 0x12 until the ROM tells it,
through an HLE hook, that it got as far as BlackOut. The handler also clears 0x2000 bytes of text
RAM, then runs the original `bal sub_F50A8`.

**Disabling it does not stop the board**, though — measured above. Whatever `FUN_1800727a0` and
states 0x13-0x16 do, the emulated ROM reaches attract mode and plays its audio without them. The
handshake reports boot; it does not perform it.

### 1 — `SyncGeometrizer.lf+0x44`, the vblank edge

The ROM's frame wait is a four-instruction spin with no yield in it:

```
28E14  ld    0x98000C, g4        ; sample the vblank port
28E1C  lda   0x98000C, g6
28E28  and   g4, 4, g5           ; remember bit2
28E2C  ld    (g6), g4            <-- HOOKED
28E30  and   4, g4, g4
28E34  cmpi  g5, g4
28E3C  be    loc_28E2C           ; spin until bit2 CHANGES
```

The handler toggles bit 2 of the module's per-board copy of that port
(`DAT_1807dab84 + board * 0x11c`), raises the render request, and runs the original load. It is the
only thing that makes the spin terminate. Disabled, the ROM spins forever on a value that never
changes — and because the spin never yields, `module_main` never returns and the whole host stops.

### 2 — `synch+0x4`, the frame yield

`synch` (ROM 0x18AB0) is the ROM's wait-one-frame primitive; every wait in boot and in play goes
through it. The hook sits on the instruction *after* the spin returns:

```
18AB0  sub_18AB0:  bal sub_28DE8   ; the spin above
18AB4  ld  0x5039F4, g4            <-- HOOKED
```

The handler raises the CPU stop flag (context +0x1A8), flips a double-buffered 16-byte record and
advances the vblank parity counter (`FUN_180055680`), then runs the original. This is the yield
that ends the host's emulated frame.

### 3 — `InitIO+0x24`, cabinet detection (Host, not Core)

Handler is the bare skip tail, so this **deletes** an instruction:

```
274C  cmpi  g1, g4
2750  mov   0, g4
2754  be    loc_275C     <-- DELETED, so the fall-through always runs
2758  mov   1, g4
275C  cmpi  0, g4
2760  st    g4, 0x5023E0  ; BoardType
2768  bne   loc_2774      ; taken, because g4 is now always 1
276C  bal   sub_26E8      ; InitIO_Basic - never reached
```

Deleting the branch forces `BoardType = 1` and skips `InitIO_Basic`. Restoring it hands cabinet
detection back to a real I/O read, which boots — what a `BoardType` of 0 changes later has not been
chased.

### 17 — `InitSelect+0x1C` (Host, not Core)

The same render-request + CPU-yield pair as hook 1, on the character-select init. Not needed to
boot — hook 2 already yields every frame — but the 600-frame test never reached the select screen,
so what it does there is untested.

## Host integration

| # | site | what it does |
|---|---|---|
| 4 | `InitCoinSetting+0x34` | writes backup `0x1D00034` (**FREE PLAY**) from the YAMP setting plus a coin-settings block, and `0x1D00024` when two-board is on — replacing the ROM's `stob g14, 0x1D00034` |
| 5 | `BlackOut+0xCC` | **skips the warning screen — DISABLED BY DEFAULT.** `0x5024D4` is the `Warning` mode's already-shown flag; the ROM clears it here and the hook writes 1 instead. See below |
| 6 | `InitNetwork` | MASTER/SLAVE injector: backup `0x1D00028` = 2 standalone, else 1 on board 0 / 0 on board 1. The link handshake itself runs raw |
| 7 | `__mainloop__` | after the original `call synch`, if the module's request flag is raised (set by its board reset) redirects the i960 to `ClearBackup` (0x2350) or 0xE37F0 |
| 35-43 | `entrySoundCode` ×8, `setSoundSlot` | the audio bridge — issue the command to the host mixer, then perform the i960 `ret`. **41, 42 and 43 are STUBS**: they return without issuing anything |
| 44 | `Advertize+0x170` | attract audio: commands 0x1123 / 0x1147 / 0x1122 / 0x1146, then the original |
| 48 | `Game_00+0x38` | sound command `0xA00F0000 \| halfword at 0x504CC0` |
| 52 | `InitTGP+0x4` | builds the display ramp table natively, replacing `call sub_28840` (which picks RGB gamma factors off backup `0x1D00027` and fills the same table a word at a time) |
| 53 | `MakeRampIntensityTable.lf+0x108` | builds the second ramp table natively, then runs the original |
| 77-79 | `SelectMove`, `NameEntry` | raise the Kiwami 2 progress-report flag; skip the original on the input bits |
| 86 | `SelectMove+0x324` | reports the chosen VR (`machine_P`, 0x503A98) to the K2 host block, flag bit 0x100 |
| 87 | `Game_30` | reports the match result (0x503A80) to the K2 host block, flag bit 0x200, once the set is decided |

### The linked-cabinet set — 19-21, 23-25, 27-31

Every one of these is gated on the **two-board gate** (`DAT_1806910DE`) and the game mode, and they
are the module's own answer to "what changes when a second cabinet is present". Directly relevant
to the netplay rebuild in `von-netplay-recon.md`:

| # | site | while linked |
|---|---|---|
| 19 | `Advertize+0x14C` | reads the emulated `CommData+4` (0x1A12704) and sets the register to `(value == 0x21)`; board 1 forces 1 |
| 20 | `InitSelectV+0xDC` | clears a register on the versus-select init |
| 21 | `InitSelectV+0x16C` | `VersusMode` (0x503A7C) = 1 and `cSend+4` (0x5032F4) = 0x21, replacing the original |
| 23 | `LinePut+0x8` | clears a register |
| 24 | `WaitChallenger+0xCC` | on board 0, replaces the original |
| 25 | `WaitChallenger+0x120` | `RoundnumberVS` (0x503A84) takes the halfword at 0x5024F8, on board 1 |
| 27 | `Game_30+0x13C` | compares `SetWin`/`SetLose` against `NumOfSets`, i960 `ret` when the match is not over |
| 28 | `InitGameOverReal+0x24` | raises a register flag and calls the module's own game-over routine |
| 29-31 | `InitGameMain+0x8`, `SelectV+0x138`, `WaitAnother+0x24` | replace the original |

## The warning screen — why Virtual On looked like it booted straight to the SEGA logo

`Warning` (i960 0x3C40) is **MainMode 0** — the first entry of the mainloop's mode table at
0x18680, with `Advertize` (the SEGA screen) at [1]. It is guarded by an already-shown flag:

```
3C6C  ld   0x5024D4, g4      ; the flag
3C74  lda  0x234, g2         ; 564 frames ~ 9.4 s
3C7C  st   g2, 0x503A04      ; the mode timer
3C84  bne  loc_3CFC          ; SET -> skip the text, MainMode++ immediately
      ...print the notice, hold for the timer...
3D44  st   g2, 0x5024D4      ; ...then mark it shown and MainMode++
```

The ROM CLEARS that flag during `BlackOut` (`st g14, 0x5024D4` at 0x1871C). **Hook 5 replaces that
instruction with a write of 1**, pre-marking the notice as already seen, so mode 0 falls straight
through to mode 1 — which is the whole of "it boots directly to the SEGA screen".

Measured on MASTER over 1200 frames: with the hook enabled `MainMode` is already 1 at the first
sample; disabled it stays 0 for ~564 frames and then advances — the ROM's own `0x234`, to the
frame. Confirmed on screen.

**It is therefore disabled by default** (`HOOK_VON_WARNING_SKIP`), which restores the arcade boot.
Same trap as StF's defaults: an existing settings.ini carries an explicit
`DisabledHleHooksLo.VON-K2=0`, and an explicit value beats a default — such a profile keeps
skipping the warning until "Restore defaults" is pressed or those two lines are deleted.

## Content and deleted

Attract and demo: **8, 9** (`Game_00`, off the module's guest counter 0x504D10), **10, 11**
(`Demoplay_00`, which branches the i960 to 0x226B0 and sets the demo timer 0x503A04 = 0x78),
**12** (`Advertize+0x154`), **46** (`Demoplay_10+0x64A4`, a float compare that skips 0x4CC bytes).
Presentation: **14** (`PutPhalanx+0x204`, a loop clamp at 60), **15, 16, 18**, **75, 76** (select
and name-entry input shortcuts), **104, 105** (replay), **118** (`SetBackground.lf+0x8`, scales the
RGB555 background by the module's brightness factor), **119** (`SetScrollcol+0x80`, colour remap).

Deleted outright (the `+0x004A10` tail): **13** `__mainloop__+0xCC`, which removes
`stob 4, 0x1400000` — an I/O write the module does not model — once per mainloop pass; and
**110, 111** in `execRader`.

## Kind totals

| kind | count | persists to settings.ini? |
|---|---|---|
| Core | 2 | no — session-only, see `SESSION_ONLY_KINDS` |
| Host | 36 | yes |
| Content | 16 | yes |
| Removed | 3 | yes |
| Inert | 63 | yes (and changes nothing) |

Core is hooks **1 and 2** only. Because they stay session-only they can be ticked live and take
effect, but never survive a restart — so the only way to boot with one off is
**`-von-hleoff=<indices>`**, which is why that flag is kept. Everything else can now be disabled
from the settings UI and will be in force from the first ROM instruction of the next run, since
`K2Host` applies the mask once before `module_start`.

There are **no Unclassified hooks left** in Virtual On, which is what makes the paragraph above
true: `Unclassified` is session-only precisely because an unread handler might be Core, and every
handler here has now been read.

## What has NOT been tested

The 600-frame runs above cover boot and attract. They say nothing about character select, a match,
the endings, the name-entry screen, or the linked-cabinet paths (19-21, 23-25, 27-31), all of which
need the two-board gate on and a round in progress. A hook marked Host or Content here is
"survives boot", not "harmless".
