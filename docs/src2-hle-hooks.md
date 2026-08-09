# Sega Racing Classic 2 (`src2`) HLE hooks

**26 hooks**, from the pre3 module's own per-game table at DLL **0x180108A10** (table index **12** of
the nineteen at DLL 0x1801084A0). Enumerated 2026-08-07 in Ghidra against
`pre3-pxd-w64-d3d12_retail.dll`; every handler was read, none was guessed.

Mechanism is the one already described in [`source/pre3/HleHooks.h`](../source/pre3/HleHooks.h) and
[pre3-netplay.md](pre3-netplay.md) — decoded-trace substitution, nothing in guest memory is patched.
This document only adds SRC2's table to the FV2 one that is already in
[`source/pre3/HleHooks.cpp`](../source/pre3/HleHooks.cpp).

## How index 12 was pinned (the whole chain, each link read out of the module)

```
FUN_1800393D0 (machine build), line ~540
    local_74 = rom->vtable[35]()                      // vtable offset 0x118
    cpu->vtable[2](cpu, local_78, local_74, ...)      // = CPpc603e::init, param_3 = table index

M3ERomSrc2::vftable = 0x18010C3F0                     // located from its COL at 0x18010FED0
    slot 35 (+0x118) = FUN_180037A90:
        return *(u32 *)(this + 0x20600);

FUN_180038720 (ROM factory), case 5 = "image/src2/*"
    180038C59: MOV dword ptr [RBX + 0x20600], 0xC     // => index 12

CPpc603e::init (0x1800306B0)
    if (index < 0x13) table = *(u64 *)(0x1801084A0 + index * 8)
    0x1801084A0[12] = 0x180108A10
```

The table runs 26 records of `{u32 guestAddress, u32 pad, u64 handler}` and terminates with a null
handler at 0x180108BB0 — exactly where the neighbouring table (index 11) begins, so the count is
bounded by the layout as well as by the terminator.

For reference the same walk gives FV2 index 3 / 0x180108FE0 / 36 records, which matches the table
already shipped and runtime-verified in `HleHooks.cpp`.

SRC2 is also **CPU variant 2** (`rom->vtable[34]()` = `*(u32 *)(rom + 0x56C)` = 2, set by the same
factory case), i.e. the same execute loop as FV2 — 0x1800166F0.

## The table

Register-file offsets below are relative to `DAT_18052AA88` (= cpu+8): `+0x08` current PC,
`+0x0C` next PC, `+0x10 + 4n` GPRn (so **r3 = +0x1C**, r10 = +0x38, r16 = +0x50), `+0x98 + 8n`
FPRn (**f0 = +0x98**, f1 = +0xA0), `+0x1A0` LR.

| # | guest | handler RVA | Kind | what the handler does |
|---:|---|---|---|---|
| 0 | 0x04CA40 | 0x26780 | Core | idle-loop cut-out — gives back the rest of the CPU timeslice, then runs the original instruction. Same handler as FV2's three cut-outs. **SRC2 has only one.** |
| 1 | 0x0189EC | 0x01DC0 | Removed | instruction deleted (`_guard_check_icall`, a bare `ret`) |
| 2 | 0x018B28 | 0x01DC0 | Removed | instruction deleted |
| 3 | 0x09A3FC | 0x01DC0 | Removed | instruction deleted |
| 4 | 0x006754 | 0x267E0 | Host | whole routine replaced: `mem->vt[0x40](r3)` → `rom->vt[0x70]` = FUN_180033B60, which **appends r3 to a growable u32 array on the ROM object** (`rom+0x488` data, `+0x490` capacity, `+0x494` count), then returns to LR. Same handler FV2 uses at its three sites. |
| 5 | 0x007480 | 0x2C6A0 | Host | **arcade settings injection.** Calls FUN_18002C410 with guest base 0x72629C (which writes the settings and then runs the original instruction), then stores **50.0f** to guest RAM at 0xC4550, 0xC4590 and 0xC4610. |
| 6 | 0x01922C | 0x2C6E0 | Patch | `r3 = 0`, then run the original |
| 7 | 0x073CC4 | 0x2C720 | Patch | `f0 += 0.05; f1 -= 0.05`, then run the original |
| 8 | 0x073CFC | 0x2C780 | Patch | `f0 += 0.02; f1 += 0.0` |
| 9 | 0x073D70 | 0x2C7E0 | Patch | `f0 += 0.16; f1 -= 0.1` |
| 10 | 0x073DFC | 0x2C840 | Patch | `f0 += 0.17; f1 -= 0.1` |
| 11 | 0x073E34 | 0x2C8A0 | Patch | `f0 += 0.15; f1 -= 0.1` |
| 12 | 0x069C60 | 0x2C900 | Patch | `f0 += 0.0; f1 -= 0.1` |
| 13 | 0x02D2F0 | 0x2C960 | Host | `f0 *= *(float *)(rom + 0x374)`, then run the original — see the caveat below |
| 14 | 0x036E2C | 0x2C9D0 | Host | run the original **first**, then `r3 = (int)((float)r3 * *(float *)(rom + 0x378))` — same caveat |
| 15 | 0x07BC4C | 0x2CB20 | Host | `mem->vt[0x60](0x2000, 0x1000)` → `rom->vt[0xA0]` = FUN_1800379F0: **when the host config's +0x07 flag is set, memcpy the 0x1000-byte settings blob at config +0x08 into guest RAM at 0x2000**, clear the flag, set `rom[0x205BE] = 1`. Then run the original. |
| 16 | 0x091660 | 0x2CAA0 | Patch | **forces a conditional branch taken**: `nextPC = PC + (sext16(word) & ~3)`, then hands control to the interpreter's branch-taken continuation (`cpu+0x2C0`) |
| 17 | 0x091800 | 0x2CAA0 | Patch | same handler, second site |
| 18 | 0x09185C | 0x01DC0 | Removed | instruction deleted |
| 19 | 0x0918A4 | 0x01DC0 | Removed | instruction deleted |
| 20 | 0x0918D0 | 0x01DC0 | Removed | instruction deleted |
| 21 | 0x06A4C4 | 0x2CAD0 | Patch | **forces a branch-to-LR taken**: `nextPC = LR & ~3` — a conditional return made unconditional |
| 22 | 0x0420B8 | 0x2CB00 | Patch | instruction replaced by `r16 = 0x53B` (the handler is a plain `ret`, so the original never runs) |
| 23 | 0x042134 | 0x2CB10 | Patch | instruction replaced by `r3 = 0x53B` |
| 24 | 0x01B834 | 0x01DC0 | Removed | instruction deleted |
| 25 | 0x01B854 | 0x01DC0 | Removed | instruction deleted |

Kind totals: **Core 1, Host 4, Patch 13, Removed 8, Speed 0.**

### SRC2 has NO native-routine replacements

This is the sharpest difference from Fighting Vipers 2, where 20 of the 36 hooks are whole ROM
routines reimplemented in x64 (matrix/vector maths, block memory ops). SRC2 has **none** — its
whole table is behaviour, not speed. So the `NATIVE_ROUTINE_KINDS` preset ("is one of the native
routines wrong?") has nothing to turn off for this game, and every hook here changes what the board
does rather than how fast it does it.

### The handler block is shared with two unshipped Daytona USA 2 tables

Scanning all nineteen tables, the handlers in the 0x2C6A0-0x2CB20 block are used by tables **10, 11
and 12** — the `M3ERomD2Base` family. Tables 10 and 11 are the two Daytona USA 2 variants the ROM
factory never constructs (the factory has six cases; SRC2 is case 5). Four handlers are
**SRC2-exclusive**: 0x2CAA0, 0x2CAD0, 0x2CB00, 0x2CB10 — i.e. hooks 16, 17, 21, 22, 23. Table 10
additionally uses 0x2C640 and 0x2CA40, which SRC2 does not.

## Two things this turned up that are not in the table

### 1. `pre3_config_t::reserved_07` is not reserved — it is the settings-blob upload flag

`source/pre3/pre3.h` documents config `+0x07` as `reserved_07` and `+0x08` as a 0x1000-byte
`settings` block "address-taken by the module rather than read field by field". **Hook 15 is the
consumer.** `FUN_1800379F0` reads `+0x07` as a live "upload pending" flag, copies the 0x1000 bytes
at `+0x08` into guest RAM at 0x2000, and clears the flag. Four other functions in the module
(FUN_180034FF0, FUN_1800358F0, FUN_180036AA0, FUN_180037400) do the same for the other games, so
this is a general mechanism, not an SRC2 quirk.

YAMP currently leaves both zero, so the copy never fires. That is correct-by-default, but the field
should be renamed and documented rather than left as `reserved_07`.

### 2. Hooks 13 and 14 read floats the module never initialises

Both go through `rom->vt[0x98]` = FUN_180033C80, which is just `return this + 0x368`. The ROM base
constructor (FUN_180033390) initialises that struct partially:

```
180033427: MOV byte  ptr [RCX + 0x368], AL          ; 0
18003342D: MOV dword ptr [RCX + 0x36C], 0x3F800000  ; 1.0f
180033437: MOV dword ptr [RCX + 0x370], 0x3F800000  ; 1.0f
```

`+0x374` and `+0x378` — the two fields the hooks actually multiply by — are **not** written there,
and the object is memset to zero before the constructor runs.

Every one of the module's 1839 functions was disassembled and searched for a store to `+0x374` or
`+0x378`. There is exactly one hit, `FUN_180001820`, and it is a bulk-zeroing loop over the global
array at `DAT_18062AB58` where 0x378 is a loop stride — not this object. **Nothing in the module
writes either field.**

### CORRECTION (2026-08-08): the HOST writes them, and it was writing garbage

The sweep above was of the MODULE, and the conclusion drawn from it — that these are a deliberate
"zero this value" pair — was right about the effect and wrong about the mechanism, which matters
because the mechanism is fixable and a deliberate pair would not be.

`M3EInput`'s per-frame update memcpys **0x100 bytes from `execute_info+0x1678` into the object at
`+0x368`**. So `+0x374` and `+0x378` are `execute_info+0x1684` and `+0x1688` — the first eight bytes
of `execute_info::assign`, which `Pre3Host` fills with `Input::MODULE_ASSIGN` **for every game**.
The tell was in front of us the whole time: the constructor presets `+0x36C` and `+0x370`, the two
fields immediately before, to **1.0f**. This is a row of host-supplied scalars, and the assign table
is FV2's meaning for those bytes, not the module's.

`MODULE_ASSIGN` is `02 03 04 05 06 07 08 ...`, which as a float is a denormal of about 1e-36 — so
hook 13 multiplied by ~0 exactly as if the field were unwritten. Same answer, different cause.

### What hook 13 was actually zeroing: the MOTION-INTEGRATION SCALAR

Hook 13's site is not a generic FPU value. `0x0002D2F0` is `stfs f1, 0x1e4(r15)`, and `r15` is the
game's global context at **guest 0x00105000** (proved by `r15+4` / `r15+6` being `DAT_00105004` /
`DAT_00105006`, the screen index and timer the module already mirrors to `rom+0x205BC`). So the
store lands at **guest 0x001051E4**, loaded from a per-course table.

Scanning all of `.text` for `0x1e4(r15)` gives four accesses — two writers (`0x1F9AC`, and hook 13's
`0x2D2F0`) and two readers:

```
0002cf04:  lfs   f2, 0x1e4(r15)      0008dc98:  lfs   f12, 0x1e4(r15)
0002cf08:  fmuls f1, f1, f2          0008dc9c:  fmuls f13, f23, f12   ; velocity X * scalar
                                     0008dca0:  fmuls f14, f24, f12   ; velocity Y * scalar
                                     0008dca4:  fmuls f15, f25, f12   ; velocity Z * scalar
                                     0008dca8:  fadds f29, f29, f13   ; position X += ...
                                     0008dcb0:  fadds f30, f30, f14   ; position Y += ...
```

It is the value that turns velocity into displacement. Zeroed, anything integrating position
through that path stops advancing — **which is why Sega Racing Classic 2's CPU cars slowed to a stop
a few seconds into every race.** User-confirmed by isolating hook 13: with it disabled and all 25
others at their shipped setting, the AI drives.

Hook 14 is the same mechanism on `+0x378` and is genuinely minor: its site (`0x036E2C`,
`lwz r3, 0x6254(r3)`) feeds `DAT_00105010 += DAT_00106254`, a bonus-time/score accumulator.

**So the fix is not to disable hook 13.** Writing **1.0f** to `execute_info+0x1684` and `+0x1688`
makes both hooks the identity, which is the module's own mechanism doing nothing rather than the
hook being switched off to stop it doing harm — and it is presumably what Like a Dragon Gaiden
itself passes. See `pre3_execute_info_t::src2_scalars` in `source/pre3/pre3.h`.

## The three things Gaiden's table suppresses, and how to get each back

Established 2026-08-08, by bisecting the table on a running board — which only became possible once
SRC2 was drivable at all (the ADC ring had never been filled; see `pre3::SetDrivingAxes`). Masks are
`DisabledHleHooksLo.SRC2` in `settings.ini`.

| behaviour | hooks | mask | how established |
|---|---|---|---|
| **CPU car AI** | 13 | `0x0000000000002000` | **MEASURED.** Isolated: hook 13 disabled, all 25 others shipped -> the AI drives. |
| **Start-up WARNING screen** | **6** | `0x0000000000000040` | **MEASURED** (2026-08-08, cold-boot bisection) - see the correction below |
| ~~boot presentation via 16-21~~ | 16-21 | `0x00000000003F0000` | **RETRACTED** - no visible effect alone; see below |

### CORRECTION (2026-08-08): the warning screen is hook 6, and the 16-21 attribution is retracted

The earlier claim - "disabling 16-21 as a group brought back both the warning screen and the title
logo, user-confirmed" - was measured under a **wider mask** (everything off bar the boot-critical
strip), and the effect belonged to a bit outside the group. Re-measured by cold-boot observation,
one mask per launch:

* `16` alone, `17-20` alone, `21` alone, `16-21` together, with and without bit 3, country JAPAN
  and USA, free play on and off: **no warning screen, ever**. The boot walks the presentation
  screens in a one-frame cascade regardless.
* everything-off (`FFFF…`, boot-criticals stripped back on): **warning screen shows**.
* halving the difference: `{22-25}` no, `{0,4,6-12,15}` yes, `{6}` alone **yes** - and without
  16-21 still **yes**.

**The mechanism, read off the ROM at the hook's own site:** hook 6 patches guest `0x1922C`, which
is `cmpwi r3, 1` on the byte the boot chain just loaded from **guest `0x100198` = the settings
working copy `+0x18` = COUNTRY**. `COUNTRY == 1` (JAPAN) is the ROM's own condition for drawing the
start-up warning screen; the hook forces `r3 = 0` (INTERNATIONAL) so the compare always fails.
Two corollaries: with a non-JAPAN COUNTRY the ROM skips the screen natively - so the warning is
only recoverable at all under the default JAPAN assignment - and the screen is not "restored" by
any of 16-21, whose routines are pieces of the composite presentation handler at `0x91598`
(message mutes, the 3D walker) that today's runs could not surface. Hook 21's routine, also
disassembled on the way: not a skip/advance check but a conditional draw of message ids `0x2FE`
and `3`, gated on the one-hot current-screen word at guest `0x105000` having bits `0x03000000`.

What 16-21 visibly do therefore remains **unmeasured**; they stay in the default mask as
original-ROM behaviour, not as a screen switch. The Daytona 2 title-logo attribution is likewise
back to unknown as a single bit — but the full presentation is confirmed working under the
shipping default: a cold boot of `0x3F6040` shows the warning screen, the Daytona 2 logo, the
network boot check AND a working TEST menu, all user-verified on screen (2026-08-08). Whichever
bit(s) gate the logo, they are among the ones that mask already clears.

**These are SRC2's `DefaultDisableMask`** — `0x3F6040` (hooks 6, 13, 14, 16-21) — on exactly the
argument `HleHooks.cpp` already makes for Fighting Vipers 2's hooks 7 and 10: Gaiden wants the boot
sequence gone because the emulator is a minigame inside a menu, and YAMP is the cabinet, so the
board's own power-up sequence is the authentic behaviour. Hook 13 is different in kind — it is a
defect, not a preference, and the better fix is the 1.0f scalar above rather than a mask entry.
Hooks 1, 2, 3 and 5 can never join the mask: they are `SRC2_BOOT_CRITICAL` and the persisted-mask
strip forces them back on (hook 3's story is in the sweep correction further down).

## Which hooks are load-bearing: exactly three (1, 2 and 5)

Every hook disabled on its own, 400 frames each, against a 676-draw baseline. Confirmed at 2000
frames for the three that failed, because a short window cannot tell "has not advanced yet" from
"will never advance".

| # | Kind | guest | draws @400f | verdict |
|---:|---|---|---:|---|
| 0 | Core | 0x04CA40 | 694 | fine — *faster* than baseline |
| **1** | Removed | 0x0189EC | **0** | **boot-critical** — never reaches frame 1 |
| **2** | Removed | 0x018B28 | **23** | **boot-critical** — freezes on its 8th draw |
| 3, 4 | Removed, Host | 0x09A3FC, 0x006754 | 658 | fine |
| **5** | Host | 0x007480 | **0** | **boot-critical** — never reaches frame 1 |
| 6–25 | Patch/Host/Removed | — | 637–667 | fine |

The healthy spread of 637–694 is host/guest pacing noise around the 676 baseline.

**CORRECTION (2026-08-08): hook 3's "fine" is a false negative, and it is now boot-critical.** The
sweep measures the attract path, and on the attract path disabling hook 3 really is invisible — the
board boots, attracts and races. What the sweep cannot see is the **TEST switch**: hook 3 deletes
the per-frame `bla 0x55D800` in the game's main loop, the third call into the security overlay that
hooks 1 and 2 excise the *loader* of, so with hook 3 disabled the restored call lands in memory that
is all zeros. TEST-menu entry reaches it, the guest crash-reboots into the MODEL3 SYSTEM PROGRAM
status screen ("PRESS START OR TEST BUTTON TO CONTINUE"), and continuing from there hangs in the
same reused-device-table status spin at guest 0x1E78 that hook 1's diagnosis describes. Found by
bisecting the service-menu regression this bit caused when it briefly shipped in
`DefaultDisableMask`; the full hunt is docs/src2-service-menu-regression.md. `SRC2_BOOT_CRITICAL`
is therefore `{1, 2, 3, 5}`, and the persisted-mask strip keeps hook 3 enabled even against a stale
settings.ini.

**The `Kind` column is not a risk ranking on this board, and is very nearly inverted.** Hook 0 is
the only `Core` hook and it is the *safest* in the table — disabling it yields 694 draws, more than
the baseline, because without the timeslice cut-out the board gets more CPU per host frame. The
three that actually stop the boot are two `Removed` and one `Host`. The settings panel colours
`Core` rows as the warning, which for SRC2 warns about the wrong hook and stays silent on all three
real ones.

### Hooks 1 and 2 are a matched pair that excises one obfuscated overlay

Read out of the decoded trace at runtime (see "Reading the board's PowerPC" below).

**Hook 1** sits in the board's init chain — a long run of `bl` calls to subsystem initialisers —
and deletes one of them:

```
0189E8  4BFEC719  bl 0x5100
0189EC  4806302D  bl 0x7ba18      <== HOOK 1 deletes this
0189F0  4BFF4CC9  bl 0xd6b8
```

`0x07BA18` is a **loader**:

```
07BA24  lis r3, 8 ; addi r3, r3, -0x4550     ; r3 = 0x0007BAB0  (fallback address)
07BA2C  lis r4, 0x56 ; addi r4, r4, -0x2100  ; r4 = 0x0055DF00
07BA34  stw r3, 0(r4)                        ; *(u32*)0x55DF00 = 0x0007BAB0
07BA38  r3 = 0x0055D000   (dest)
07BA40  r4 = 0x000D98DC   (source)
07BA4C  lwz r5, ...       ; r5 = *(u32*)0x000D98D8 = 0x1D0 = 464 bytes
07BA50  lbz r6, 0(r4) ; xori r6, r6, 0x98 ; stb r6, 0(r3)   ; XOR-0x98 DEOBFUSCATE
07BA68  bne 0x7ba50
07BA7C  bl 0x5024                            ; relocate 0x55D000 -> 0x55D800
```

**Hook 2** deletes the call into what that loader would have produced:

```
018B20  lis r16, 0x73 ; lbz r16, 0x7994(r16)  ; snapshot the byte at 0x737994
018B28  4855D803  bla 0x55d800                 <== HOOK 2 deletes this
018B2C  bl 0x4c9ec
018B30  bl 0x505c                              ; loop head
018B38  lbz r3, 0x7994(r3)                     ; re-read the same byte
018B3C  cmpw r3, r16
018B40  beq 0x18b30                            ; spin until it changes
```

So hook 2 is load-bearing only *because* hook 1 is applied: with the loader deleted, the trace at
0x55D800 is **all zeros** (verified by dumping it), and restoring the `bla` calls into empty memory.

### What the overlay actually is

464 bytes at 0x0D98DC, XOR-0x98. Deobfuscated it is valid PowerPC that links at 0x55D800, and it is
a **hardware transaction against a device in the Model 3 system-register mirror**:

```
55D8E8  lis r4, -0x1e6 ; addi r4, r4, 0x14   ; 0xFE1A0014
55D8F0  stwbrx r3, 0, r4                     ; byte-reversed (little-endian) device write
55D8F4  lis r4, -0x1e8                       ; 0xFE180000
55D900  lhzu r3, 2(r5) ; stwbrx r3, 0, r4    ; stream halfwords OUT
55D92C  stwbrx r16, 0, r4                    ; command word 0x6671 -> 0xFE1A0018
55D94C  addi r8, r8, 0x11a8                  ; poll timeout = 0x411A8 iterations
55D958  lwbrx r3, 0, r5 ; rlwinm. r0, r3, 0x1f, 0, 0 ; bne  ; poll a busy bit
55D968  sthu r3, 2(r6)                       ; stream halfwords BACK
55D9B4  sync ; dcbf 0, r3 ; sync ; addi r3, r3, 0x20 ; bdnz  ; FLUSH THE DCACHE over the result
```

`0xFE180000` and `0xFE1A0000` are the uncached mirrors of `0xF0180000` and `0xF01A0000` — the
standard Model 3 **security board** RAM and register windows, and the protocol matches: stream
halfwords into the security RAM, poke a command word, poll the busy bit, read the result back, then
`dcbf` it so it can be executed. This is the protection device's challenge-response, shipped
XOR-obfuscated and loaded at run time, and the bytes it returns are meant to be RUN.

The game even degrades gracefully by design: on timeout the transaction returns 0 and the caller
jumps to `*(u32*)0x55DF00` = 0x0007BAB0, the fallback the loader stashed in its first three
instructions.

### Why the module excises it: the stub answers "success" with zeros

The obvious guess — pre3 does not emulate the device, so the game hangs waiting on it — is **wrong**,
and worth writing down because it is the guess anyone would make (I made it). pre3 maps that window
twice over:

```
CM3Mem top-byte table (DLL 0x1801077C0, 17 windows):  ... F0 F1 F8 FE FF
FE sub-window table   (DLL 0x180106980, 57 windows):  ... 18 19 1A ...
   0x?018xxxx  security board RAM       -> handlers @ 0x180106AD8
   0x?01Axxxx  security board REGISTERS -> handlers @ 0x180106B48
```

Both mapped. But the register accessors forward to the ROM object's vtable slots 2 and 3, and on
`M3ERomSrc2` those are:

```
slot 2  (register READ)  = FUN_180006680       ->  return 0;
slot 3  (register WRITE) = _guard_check_icall  ->  ret;
```

**Every read returns zero, every write is discarded.** Now walk the overlay's poll loop with that:

```
55D958  lwbrx r3, 0, r5                  ; read the status register -> 0
55D95C  rlwinm. r0, r3, 0x1f, 0, 0       ; test the busy bit -> 0, CR0 EQ set
55D960  bne 0x55d950                     ; NOT taken: "device ready", on the very first poll
55D964  lwbrx r3, 0, r4                  ; read the result -> 0
55D968  sthu r3, 2(r6)                   ; store the zero into the result buffer
...
55D974  li r3, -1                        ; and report SUCCESS
```

The busy-wait never spins, so the 0x411A8 timeout is never reached, so the `li r3, 0` failure path is
never executed, so **the game's own fallback can never fire**. The transaction reports success, hands
back a buffer of zeros, and the caller dutifully `dcbf`s it and carries on as if the protection
device had answered.

That is the real reason for hooks 1 and 2: not a missing device, but a stub that lies. A clean
failure would have been survivable — the game ships a path for exactly that — but "success plus
zeros" defeats it. Deleting the loader and the call is the module's substitute for the fallback its
own stub disabled.

### Why running the loader ALONE is fatal — and why it is NOT a security problem

With only hook 1 disabled, hook 2 still deletes the `bla`, so the overlay never executes; yet the
board stops. Diagnosed 2026-08-07 with the guest PC probe.

The board hangs at guest **0x001EBC**, inside `FUN_00001E78`, which is a frame-sync wait:

```c
void FUN_00001e78(void) {
  FUN_00001fb4();
  if (DAT_0073797c != NULL) (*DAT_0073797c)();
  DAT_007379a8++;                                 // frame counter
  puVar1 = FUN_0000aec4();                        // *(u32*)(0xBB910 + ((*0x737988 >> 10) & 1)*0xC + 8)
  do { *puVar1 = 0x4000000; }                     // kick the device
  while ((DAT_fe100018 & 4) != 0);                // wait for busy to clear
}
```

Cross-checked against MAME's Model 3 driver (`src/mame/sega/model3.cpp`), which names both sides of
this exactly:

* `0xF0100018` is the **IRQ state register** (`model3_sys_r` case `0x18/8` returns `m_irq_state`).
  pre3 serves it from `CM3Mem+0x36` (DLL 0x18001355A).
* Bit **0x04** is one of the **per-scanline interrupts** — `scan_timer_tick` asserts `0x0C` on every
  scanline that is not 384, and `0x02` (VBlank) at 384. It is cleared only when the game's own
  handler services it; writing 0x18 is a strobe, not an acknowledge.
* `0xF1180000-0xF11800FF` is the **video register** window — which independently confirms the
  decode-time table content `{F1180008, F118000C, F1180010}` really is a triple of video registers,
  i.e. the table is right and it is the runtime memory that is wrong.

So the loop is "wait until the scanline IRQ has been serviced", and it hangs because **that
interrupt is never serviced** in this configuration. Separately, the PC probe shows the pointer it
writes is `r5 = 0x30303030` — ASCII `"0000"` — so the board's state is demonstrably corrupt as well.
(An earlier draft of this document blamed the hang on that bogus write. That is wrong: the loop's
exit condition is the IRQ register, not the poked device. The garbage pointer is a second symptom of
the same corrupted state, not the cause.)

The pointer comes from guest memory at 0xBB918, and that memory is **transient**:

| addr | decode-time image | healthy runtime | hook 1 disabled |
|---|---|---|---|
| 0xBB910 | `F1180008` | `90410004` (`stw r2,4(r1)`) | `30303030` |
| 0xBB918 | `F1180010` | `90410008` | `30303030` |
| 0xBB924 | `FE2C0080` | `7C240B78` (`mr r4,r1`) | `30303030` |

At load time it is a clean device-address table (two 0xC-byte structs, matching the stride
`FUN_0000AEC4` indexes). By runtime it has been repurposed — for PowerPC code in a healthy boot,
for `'0'` fill in the broken one. So `FUN_00001E78` is **boot-phase machinery that is only valid
while that table still exists**, and disabling hook 1 causes it to be entered after the memory has
been reused.

**IT IS A PROTECTION FAILURE AFTER ALL — an earlier version of this section said the opposite and
was wrong.** Two measurement errors produced that conclusion, both worth recording:

1. `YAMP_PRE3_PEEK` originally used `CM3Mem+0x18` as the guest RAM base. It is not; the module's
   own handlers use `DAT_18062AA98`. The bad base read zeroes everywhere, including at addresses
   that provably hold code. **Any memory probe must first be validated against a known-fixed
   value** — here `0x0189EC`, which must read `0x4806302D`.
2. Worse, and self-inflicted: once `MaskStripBootCritical` was added on the LOAD path, a
   `settings.ini` asking for hook 1 off silently came back with it **on**. Several runs labelled
   "hook 1 disabled" were plain default-mask runs. `YAMP_PRE3_ALLOW_CRITICAL=1` now exists so the
   strip can be bypassed deliberately.

With both fixed, the table at 0x0BB910 tells the real story across a broken run:

```
0BB918 : 00000000 (x34)  ->  F1180010 (x102)  ->  30303030 (x139)
```

It initialises correctly and is then **overwritten with ASCII "0000"**, after which the frame-sync
routine reads a bogus pointer and spins. Correlating the PC trail with the moment it flips, the
link register at that instant is **0x07BA9C** — inside the loader. Which the rest of the loader
explains:

```
07BA80  lis r3, 0xe ; addi r3, r3, -0x654c   ; r3 = 0x0D9AB4  (encrypted payload)
07BA88  lis r4, 0xe ; lwz r4, -0x6550(r4)    ; r4 = *(u32*)0x0D9AB0 = 0x21C  (input length)
07BA90  lis r5, 0xe ; lwz r5, -0x6554(r5)    ; r5 = *(u32*)0x0D9AAC = 0x218  (output length)
07BA98  bla 0x55d000                          ; CALL THE OVERLAY
07BA9C  ...epilogue                            ; <- the LR seen during the corruption
```

So the loader does not merely *install* the decryption service — it immediately **uses** it, asking
the security chip to decrypt 0x21C bytes into 0x55D800. pre3's stub reports success and returns a
buffer of zeros, the board acts on that garbage, and the video-register table is among the
casualties.

### The honest-failure experiment: built, run, and NEGATIVE

`source/pre3/SecurityBoard.{h,cpp}` implements the cheap half — redirect ROM vtable slot 2 so the
status register reports BUSY, and the overlay should exhaust its 0x411A8 timeout, return 0, and let
the board take its own fallback at `*(0x55DF00)`. Env-gated: `YAMP_PRE3_SECURITY=fail`.

It installs correctly (8 vtable entries) and **changes nothing**: same 0 draws, same spin, same
corruption. The instrumented handlers explain why — with hooks 1 and 2 disabled the guest performs
**no security register accesses at all**, neither reads nor writes, across 300 frames.

So the chain "lying stub -> garbage result -> corruption" is **NOT supported**, and the paragraph
that previously asserted it has been removed rather than softened. The transaction never happens, so
its return value cannot be the cause. That is the third time the causal story here has moved under
new measurement; the facts below are what survive, and no mechanism is claimed beyond them.

**What is measured, with hooks 1 and 2 disabled:**

| observation | value |
|---|---|
| the loader runs | `*(0x55DF00)` = `0007BAB0`, the fallback address it stashes |
| the overlay is installed correctly | `0x55D000` = `3821FFF0 7C0802A6 90010000 …` |
| the overlay IS called | LR = `0x07BA9C`, the return address of `bla 0x55D000` |
| security registers touched | **none**, read or write |
| the overlay's output buffer | `0x55D800` = all zeros |
| a wide region is filled with ASCII `"0000"` | `0x0BB910` (video-register table) **and** `0x0D98DC` / `0x0D9AB4` (the obfuscated overlay source and the encrypted payload) |
| the board then hangs | frame-sync wait at `0x001EBC`, reading a bogus pointer from the wrecked table |

### ROOT CAUSE, proven: the decoded trace is STALE for code the board loads at run time

Found with an x64dbg hardware write breakpoint on the host address backing guest 0x0BB918,
conditioned on the fill value so it fires once instead of on every legitimate write. (Two traps
worth keeping: the window to arm it is only between the table's initialisation and its destruction,
so the script attaches to a already-running process and polls until the address reads `F1180010`;
and an x64dbg `[addr]` condition dereferences POINTER-sized, so comparing against `30303030` needs
`dword:[addr]` or it silently never fires.)

The breakpoint caught the writer with **guest r1 = 0xFFFF897F** — the stack pointer is garbage, and
r0 = 0x30, the `'0'` being stored. So the fill is not a real `sprintf` call; the board is already
executing with a destroyed register file, and the fill is a symptom. Reading the trace against live
RAM at the moment of the catch says why:

| guest addr | LIVE RAM | TRACE word | handler index |
|---|---|---|---|
| `0x55D000` | `3821FFF0` — the overlay, correct | **`00000000`** | `011F` |
| `0x55D004` | `7C0802A6` | **`00000000`** | `011F` |
| `0x55D008` | `90010000` | **`00000000`** | `011F` |
| `0x0189EC` | `30303030` (the fill reached the CODE) | `4806302D` | `902D` |

**The loader writes the overlay into RAM correctly, and the interpreter executes the decoded trace
instead.** The pre-decoder walked 0x55D000 while it was still zeros and never re-ran for that
region, so `bla 0x55D000` runs 464 bytes of "instruction 0x00000000". That wrecks the register file,
execution wanders into the string formatter, and the runaway `'0'` fill destroys the video-register
table — and eventually the hook sites themselves.

So, finally:

* **pre3's decoded-trace interpreter cannot execute code the board writes into RAM at run time.**
  That is a structural limit of the design described at the top of this document, not a bug in any
  one game.
* **Hooks 1 and 2 are the correct fix for it**, and deleting both is the only workable one: hook 1
  stops the loader, hook 2 stops the call to what it would have loaded.
* **Implementing the security chip would NOT help.** The guest never executes the overlay as code,
  so it never performs a transaction — which is exactly why the instrumented handlers recorded zero
  register accesses. `SecurityBoard` is kept because the stub-that-lies is a real defect that would
  matter if the overlay ever ran, but it is not this bug's fix.
* The very first hypothesis in this investigation — stale trace vs. self-modifying code — was
  right. It was retracted mid-way on the strength of a peek reading the wrong RAM base. The lesson
  is the one already recorded next to `RAM_PTR`: validate a memory probe against a known-fixed
  value before believing anything it says.

### Reading the board's PowerPC

There are no ROM symbols and `image/src2.par`'s `eprom.bin` is the raw EPROM set — a different
interleave with no simple mapping to the RAM addresses the hooks use, so disassembling the archive
does not answer "what is at 0x018B28". (For the record the archive *is* readable: PARC container,
6 files, each SLLZ v1; `eprom.bin` is at 0x1BE6000, 0x3E8F8D compressed, 0x800000 out. A faithful
SLLZ v1 decoder has to match the module's own at DLL 0x1800480C0 — the copy pair is little-endian
nibble packed, `length = (b0 & 0xF) + 3` and `distance = ((b1 << 4) | (b0 >> 4)) + 1`, and the flag
byte is refilled mid-item.)

The decoded trace is the better source and needs no debugger: the pre-decoder stores
`{originalWord, handlerIndex}` for **every** word of the region, and the original word is kept
precisely so a hook can be undone. `HleHooks::DumpGuestCode` exposes it:

```
YAMP_PRE3_DUMP=18900:200,7400:96    # comma-separated <hexAddr>[:<wordCount>], count is DECIMAL
```

It is self-timing, and it has to be: on the first frame the table verifies, every word in the trace
is still zero because the board has not yet copied its program into RAM. It retries each frame and
fires once the first requested address decodes non-zero. The trace is a snapshot of *decode* time,
not live RAM — which is why 0x55D800 reads as zeros even in runs where the loader executed.

## Disabling ALL of them stops the boot

Measured 2026-08-07, two 400-frame runs of the same build differing **only** in
`[Debug] DisabledHleHooksLo.SRC2` in `settings.ini`:

| mask | `[draw]` calls in 400 frames |
|---|---|
| `0000000003FFFFFF` (all 26 disabled) | **0** |
| absent / zero (the shipped default) | **676** |

With everything disabled the host is fine — D3D12 comes up, the upload pools and primitive buffers
initialise, the frame loop runs to the limit and `module_stop` returns 0 — but the guest never
issues a draw. Same signature as the clock-pin freeze below: a guest-side stall in the boot phase,
not a host hang.

This contradicts the note that used to sit above `DefaultDisableMask()` in
`source/pre3/HleHooks.h`, which said no pre3 hook is load-bearing enough to stop the board. That
was derived from FV2, where it holds. It does not hold here.

**Practical consequence:** the pre3 mask persists in full — nothing is session-only, unlike the
m2ftg Core hooks — so pressing "Disable all" in the settings panel leaves SRC2 unbootable across
restarts, recoverable only via "Restore defaults". Marking something session-only would fix that,
but it needs a bisect to say *which* hook, and guessing would be the same class of error as the FV2
boot-screen mis-identification recorded in [pre3-netplay.md](pre3-netplay.md).

## Netplay relevance

Nothing in SRC2's table samples host entropy or wall-clock time. The determinism picture is the
same as FV2's, with two caveats specific to this game:

* **The clock pin must not be enabled for SRC2 as-is.** `Determinism::GetTime` returns a constant,
  and SRC2's boot waits for the clock to tick — this is the freeze already recorded in
  [pre3-netplay.md](pre3-netplay.md). The pin stays an FV2-only whitelist until it grows a clock
  that advances deterministically.
* **Hook 5 injects the arcade settings and hook 15 can inject a 4 KB settings blob**, so both peers
  must agree on the YAMP config *and* on the blob/flag, exactly as the FV2 notes require for the
  arcade settings alone.

The HLE disable mask must match between peers, as for every other pre3 game.

## Not yet wired up

`source/pre3/HleHooks.cpp`'s `CurrentTable()` still returns null for anything that is not FV2, and
its comment above the FV2 table calls SRC2 "Sega Rally 2" (it is **Sega Racing Classic 2**) and says
the table "has not been enumerated". Both are now out of date.
