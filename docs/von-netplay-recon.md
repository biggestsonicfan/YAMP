# Virtual On (`omg`) netplay

## *** RESUME HERE — state as of 2026-08-03 ***

**Where it stands:** everything up to and including a live two-machine round is built and wired.
Both boards run, master/slave assign themselves, inputs route per board, the determinism descriptor
is complete and measured, and `NetSession` is wired into the K2 host. A match starts. It then
**desyncs at frame 1**, and there is one known, understood reason why it must.

### THE BLOCKER — the guest seeds its RNG with 0

Not a timing accident: `YampNet.h` states the match seed is *"valid once state is SYNCING or
IN_MATCH"*, but `NetSession::Drive` reads it during round prep at `IN_ROOM`, **before** the barrier.
The host has it (it generated it); every guest reads 0. Measured on two machines — guest logged
`RNG seeded 0x00000000, board reset`, then `barrier released; round 1 seed 0x813AC110`.

The two peers therefore reset their boards drawing from differently seeded generators, which is a
divergence before frame 0 and exactly what the seeding exists to prevent.

**This is in SHARED code and affects StF/FV/VF2 guests too**, not just Virtual On. It may have gone
unnoticed there because less of their post-reset init draws from the RNG.

**DO NOT fix it by waiting for a non-zero seed in prep — that deadlocks** (prep waits for the seed,
the seed comes with the barrier, the barrier needs both peers to finish prep). Tried; the guest's
Start Match silently did nothing. The comment at the read site says so too.

Two real options, needing a decision:
1. **Publish the seed with the room** so a guest has it at `IN_ROOM`. It is the host's value from
   room creation anyway and the room already carries `game_flags` the same way. Best design, but a
   `yampnet.dll` change.
2. **Second seed+reset+re-anchor cycle** after the barrier releases. Self-contained in YAMP, but
   makes round start slower and more intricate.

### Also unresolved

- **The guest crashed on connect** (guest first, then host) in the run before the seed work. No
  stack — the guest was not under a debugger. Reproduce with the guest under cdb:
  `cdb -g -G -c "sxd av; g; .lastevent; r; kb; q" <path>\YAMP.exe -von-k2`
- **`-von-render1` is unverified on screen.** It patches the frame step to leave board 1 selected so
  the guest sees its own cabinet. It runs stably and is now driven by `localPad`, but nobody has
  confirmed the picture actually changes. The DLL skips the "THIS IS MASTER/SLAVE SITE" boot screen,
  so there is no easy visual tell.
- **`GlobCntr`'s four non-unit jumps** per ~900 calls (~+11 total). Harmless so far, but the round
  anchor must be reached at the SAME value on both peers, so a jump straddling it would misalign a
  round.
- **Two-board equivalence is verified for boot/attract only.** The ROM has `WaitAnother` /
  `WaitChallenger`; gameplay with a second cabinet present has not been compared. `-von-1board`
  forces the old single-board behaviour for A/B.

### Hard-won lessons worth not relearning

- **Twice** a `DwGame` field dismissed as "StF debug panes only" turned out to be load-bearing, and
  both failed SILENTLY: `rvaMemmapTbl` (the anchor read returns 0 forever, so no round can start)
  and `rvaCpuCtxPtr` (two of the stall test's three signals go dead). Do not assume a field is
  optional.
- **The launcher passes only the game's `bootArg`.** Any behaviour behind a command-line flag is
  invisible to a launcher-started run — which is how the first two-machine tests silently ran
  single-board on both peers, and how the guest produced no `[vonmap]` lines. Diagnostics must not
  be flag-gated.
- **Per-frame writes into module `.text` need `Memory::VP::Patch`** (note the `Memory::` qualifier).
  A bare store faults because the `ScopedUnprotect` lives in `ResolveSymbolsAndPatch`. Worse, the
  "verify before writing" guard meant it only stored on the frame the value changed — so YAMP died
  exactly on "Start Match", and only for the guest.
- **Breakpoint it and read the actual value.** Reading the decompiler and extrapolating has a poor
  hit rate on this DLL; every genuine answer here came from running it.

### Diagnostics available

- **`[vonmap]`** — 16 per-chunk work-RAM hashes PER BOARD into `yampnet.log`, right after each
  frame's `timers f=N` line, whenever a round is live. Diff the two peers' logs at equal `f=`; it
  localises to a 64 KB chunk and names which board diverged first.
- **`ins=`** in the timer trace — 6120 on a frame that advanced, 0 on a hold. Should match on both
  peers at equal `f=`. Ignore `ip=` and `yield=`; those offsets do not fit this generation.
- **`-von-findctr`** — sweeps every dword in the work-RAM window and classifies each `module_main`
  call as +1 / hold / other. How `GlobCntr` was found.
- Log locations differ by game because the CWDs do: StF `m2ftg/yampnet.log`, K2
  `build/bin/Win64/Debug/yampnet.log`.

---

## Recon from the PS3 build

Reversed 2026-08-03 from `C:\m2\PS3\dev_hdd0\game\NPJB00321\USRDIR\EBOOT.elf` (Ghidra on port 5678).

**Why the PS3 build at all:** the Kiwami 2 `omg` module has no two-player mode, so there is nothing
on the PC side to copy. The PS3 release *does* have versus, and it is the same `m2ftg` emulator
family as the Sonic the Fighters PS3 port — so it is the reference for how SEGA ran a
**linked-cabinet** Model 2 game, which is the one thing StF/FV/VF2 never had to answer.

Build identity: `X:/p130/prj130pnt/branch/vsomg_root/m2ftg`, task class `TaskM2E`, loading
`rom/omg_rom/{rom_code,rom_data,rom_pol,rom_tex,rom_cop}.bin` — the same five images the PC module
loads. Netstack is the StF one: `sceNp`, `sceNp2`, `cellRudp`, `NPMatching2Session`, plus
`TaskSyncIo` / `CSyncIoMsg` / `CSyncIoTcpMsg` / `CSyncStartMsg` / `CResponseSyncStartMsg`.

---

## THE ANSWER: both boards run on every peer; the link cable is a memcpy

The per-frame driver is **`FUN_00184DF8`**, and it is unambiguous:

```c
S = 0x00CA1C20;                       // emulator link/session state
if (S->[0x14] != 0) {                 // link enabled
    S->[0x30]++;                      // frame counter
    if (!FUN_0011DF64() || (S->[0x30] & 1)) {
        FUN_00185988();               // <-- THE LINK CABLE (see below)
        FUN_00184D68(0);              // select board 0
        FUN_00189614(); FUN_000FC984();   // step board 0
        if (S->[0x08] != 0) {         // two-board mode
            FUN_00184D68(1);          // select board 1
            FUN_00189614(); FUN_000FC984();   // step board 1
            FUN_00184D68(0);          // back to board 0
        }
    }
}
```

So a single process holds **two complete Model 2 board contexts**, steps **both** every frame, and
implements the inter-cabinet link as a local buffer copy. There is no network read anywhere in this
path. Nothing about "one screen per player" changes the simulation topology — it only changes which
board you *render*.

**Consequence for YAMP: this is the lockstep model we already have.** Both peers simulate the whole
match from both players' pads; the wire carries pads, not link traffic. `m2ftg::NetSession` applies
essentially unchanged. What is new is the *board* work: instantiating two boards, cross-copying
their comm buffers each frame, routing pad[N] to board N, and presenting board[localPlayer].

### Confidence

Directly verified: the frame driver, the link copy, the board switch, the comm-board device model,
the two-board flag. **Inferred** (consistent, not yet proven): that the wire payload is pads only.
Supporting evidence — the classes are named *SyncIo* ("synchronise I/O"), the `sync_io_buf` pool is
allocated as `0x2000`-byte rings (`FUN_000E5ECC`, a ring buffer, not a `0x700` comm payload), and
`TaskSyncIo` init (`FUN_000E4840`) takes a local index clamped to `< 2`. The one probe that would
close it is decoding `CSyncIoMsg`'s serializer (typeinfo `0x00446B20`, vtable just below it).

---

## The comm board, as the PS3 emulates it

Device handlers occupy `0x00185340`–`0x00185B33`; all 17 of them reach the board through the TOC
slot at `r2-0x5D50` (`= 0x00461AE0`), which holds the pointer global **`0x00CA1C58`**. The comm RAM
block itself begins at `0x00CA1C5C`, and the **second board's block is `+0x8008`**.

Layout of one block, read straight out of the access handlers:

| offset | meaning |
|--------|---------|
| `+0x0000` | shared RAM, bank 0 (16 KB) |
| `+0x4000` | shared RAM, bank 1 (16 KB) |
| `+0x8000` | `CommBoardReset` register (byte) |
| `+0x8001` | `CommFlagReg` register (byte) — **bit 0 selects the bank the i960 sees**, bit 7 = data ready |

The i960 sees the window at `0x01A10000`–`0x01A13FFF`; the two registers sit *above* it, which is
why they are special-cased instead of falling through the window path. Effective address is
`block + (addr & 0x3FFF) + (flag.bit0 ? 0x4000 : 0)`.

Access handlers (each re-derives the bank from the flag byte every access):

| address | operation |
|---------|-----------|
| `0x00185574`, `0x00185794` | 32-bit read / write through the window (`lwbrx` — byte-swapped, big-endian ROM data) |
| `0x001855C4` | 16-bit read, special-cases `CommBoardReset` / `CommFlagReg` |
| `0x0018565C` | 8-bit read, same special cases |
| `0x001856F4` | write, same special cases |
| `0x00185988` | **link transfer** |
| `0x00185B34` | reset (vtable neighbour) |

Device vtable at `0x00455160`.. (PS3 `.opd` descriptors, TOC `0x00467830`).

### The link transfer — `FUN_00185988`

With `P1 = 0x00CA1C5C` and `P2 = P1 + 0x8008`:

```c
memcpy(P2 + 0x2700 + (P2.flag.bit0 ? 0 : 0x4000),   // P2.CommData, BACK bank
       P1 + 0x2000 + (P1.flag.bit0 ? 0x4000 : 0),   // P1.CommSend, front bank
       0x700);
memcpy(P1 + 0x2700 + (P1.flag.bit0 ? 0 : 0x4000),   // P1.CommData, BACK bank
       P2 + 0x2000 + (P2.flag.bit0 ? 0x4000 : 0),   // P2.CommSend, front bank
       0x700);
// then, on both flag bytes: set bit 7 (data ready), flip bit 0 (swap banks)
```

**`0x700` = 1792 bytes is the per-frame per-cabinet link payload.** Each side writes its outgoing
state to `CommSend` and reads the peer's from `CommData`; the copy always targets the bank the
reader is *not* currently on, which is what makes the dual-port swap safe. This runs once per
frame, *before* either board is stepped.

---

## ROM symbol table — and it transfers to the PC DLL

The PS3 build carries a full `{u32 i960_addr, u32 name}` table (8-byte records) at
**`0x00441070`–`0x00441910`**, ~250 entries covering the whole Virtual On ROM.

**The PC module carries the same table, with byte-identical i960 addresses.** In
`m2ftg/omg-pxd-w64-gog_retail.dll` it is at RVA **`0x4507E0`** (`va 0x1804507E0`) as 16-byte
`{u64 i960_addr, u64 name_ptr}` records — the 64-bit form of the PS3's 8-byte `{u32, u32}`.
Spot-checked against the PS3 across the comm/net block and every address agrees exactly:
`resetCommBoard` `0xC5600`, `.lf` `0xC5608`, `netCheckFail` `0xC57E0`, `Net_check` `0xC5870`,
`getCommStatus` `0xC5CA0`, `.lf` `0xC5CA8`. Same ROM, same emulator family — **the address map
below is directly usable against the PC module**, and `scratchpad/dllsym.py` dumps it.

### Netplay-relevant ROM functions

| i960 addr | name |
|-----------|------|
| `0x00018488` | `InitBB` |
| `0x000184E8` | `entryBB` |
| `0x00018538` | `sendBB` |
| `0x00018620` | `NextGame` |
| `0x000187E4` | `__mainloop__` |
| `0x00018918` | `initGame` |
| `0x00018960` | `InitAll` |
| `0x00018A10` | **`InitNetwork`** |
| `0x00018AB0` | **`synch`** |
| `0x00018C00` | `InitSelectV` |
| `0x00018DA0` | `SelectV` |
| `0x00019030` | **`WaitAnother`** |
| `0x000190D0` | `InitGameMain` |
| `0x00019180` | `GameMain` |
| `0x00019660` | `InitWaitChallenger` |
| `0x000196C0` | **`WaitChallenger`** |
| `0x00019830` | `InitGame` |
| `0x00019B50` | `numBB` |
| `0x00026CB8` | **`SendNetRobot`** |
| `0x000C5600` | **`resetCommBoard`** (`.lf` `0x000C5608`) |
| `0x000C57E0` | `netCheckFail` |
| `0x000C5870` | **`Net_check`** |
| `0x000C5CA0` | **`getCommStatus`** (`.lf` `0x000C5CA8`) |
| `0x000F5058` | `rand.lf` |

`BB` = battle board / broadcast board (`InitBB`/`entryBB`/`sendBB`/`numBB`) — the ranking-exchange
side channel, distinct from the per-frame link.

### Netplay-relevant ROM globals

| i960 addr | name | note |
|-----------|------|------|
| `0x005023E0` | `BoardType` | which cabinet this board is |
| `0x005024E0` | `SynchFlag` | |
| `0x005024E4` | `SynchTime` | |
| `0x005024E8` | `GlobCntr` | shared frame counter — **desync canary candidate** |
| `0x005024F0` | `cRecn` | receive staging |
| `0x005032F0` | `cSend` | send staging (`cSend - cRecn` = `0xE00` = 2 × `0x700`) |
| `0x005039F4` | `MainMode` | |
| `0x00503A00` | `SubMode` | |
| `0x00503A14` | `GameTime` | |
| `0x00503A20` | `GlobTime` | |
| `0x00503A6C`/`70`/`74`/`78` | `SetWin` / `SetLose` / `SetNo` / `NumOfSets` | |
| `0x00503A7C` | `VersusMode` | |
| `0x00503A98`/`9C` | `machine_P` / `machine_E` | selected VRs |
| `0x00503AB0` | `GameResult` | |
| `0x00503AD0` | `PL1EV` | player 1 entity |
| `0x005040D0` | `PL2EV` | player 2 entity |
| `0x005770B0` | **`net_flag`** | |
| `0x005770B1` | **`link_ID`** | cabinet id (0/1) — adjacent byte to `net_flag` |
| `0x005770F0` | `FieldNo` | stage |
| `0x00577170` | `SelectType` | |
| `0x00577590` | `r_num` | RNG state — **must be seeded identically, as in the other three games** |

Emulated MMIO (these are the addresses the ROM touches, not host pointers):

| i960 addr | name |
|-----------|------|
| `0x01A12000` | `CommSend` |
| `0x01A12700` | `CommData` |
| `0x01A14000` | `CommBoardReset` |
| `0x01A14002` | `CommFlagReg` |
| `0x00884000` | `CoproDataPort` |

**`.lf` convention:** entries suffixed `.lf` are consistently the base symbol **+8**
(`resetCommBoard` `0xC5600` / `.lf` `0xC5608`, `getCommStatus` `0xC5CA0` / `.lf` `0xC5CA8`,
`ClearScroll`, `SetBackground`, `FadeSet`, `SyncGeometrizer`, …). Reading this as the i960 *leaf*
entry point — the `bal`-reachable one that skips the register-frame setup — fits the +8 spacing.
Anything that hooks one of these must hook **both** entries or the leaf path slips past the trap.
Not independently confirmed; flagging it because it is an easy way to install a hook that silently
does nothing.

---

## Emulator-side state (PS3 native addresses)

| address | meaning |
|---------|---------|
| `0x00CA1C20` | link/session state struct |
| `... + 0x00` | current board index (`FUN_00184AD0` returns it) |
| `... + 0x08` | two-board mode flag (`FUN_00184AF4`) |
| `... + 0x14` | link enabled |
| `... + 0x30` | link frame counter |
| `0x00CA1C58` | pointer to comm RAM block 0 (`0x00CA1C5C`); block 1 at `+0x8008` |
| `0x00184D68` | **board bank switch** — writes the index, then re-points eight subsystems (CPU, memory map, comm board, video, I/O) at that board |
| `0x0019F368` | native 16-bit read of `CommData+4`; callers `FUN_000FA114` / `FUN_000FA1C4` compare it against **`0x21`** (a link handshake/state value) |
| `0x00467830` | TOC base |

Note `FUN_00184D68`: the emulator is **not** two independent instances. It is one emulator holding
two banks of board state that it switches between. Whatever YAMP does on the PC side has to provide
the same capability, and the K2 module currently does not expose it — that is the real work.

---

---

## THE PC MODULE HAS ALL OF IT — verified 2026-08-03

`m2ftg/omg-pxd-w64-gog_retail.dll`, loaded in Ghidra. The two-board machinery was **not** compiled
out for the single-player Kiwami 2 cabinet. Every piece of the PS3 design is present:

### Comm-board device — same model, same layout

Four access handlers at `0x18006A580`/`0x18006A5F0`/`0x18006A660`/… gate on the identical constants
(`cmp edx,1A14000h`, `cmp ecx,1A14002h`), reached through a memory-map descriptor at
**`0x18044EFE0`** (two head functions, then twelve read/write handlers, then a repeat).

Both comm blocks live at fixed `.data` addresses, and the **`0x8008` stride is identical to the
PS3's**:

| address | meaning |
|---------|---------|
| `0x1807C2730` | comm block 0 base (P1) |
| `0x1807C4730` | `P1.CommSend` (P1 + 0x2000) |
| `0x1807CA730` / `31` | `P1.CommBoardReset` / `P1.CommFlagReg` (P1 + 0x8000 / +0x8001) |
| `0x1807CC738` | `P2.CommSend` (P2 = P1 + `0x8008`) |
| `0x1807D2738` / `39` | `P2.CommBoardReset` / `P2.CommFlagReg` |

### Link transfer — `0x180069E48`–`0x18006A577`, inlined and unrolled with SSE

Every reference to those globals lies in this one contiguous, self-contained block. The copies are
`MOVUPS` loops of 14 × 0x80 = **`0x700` bytes**, sourced from `CommSend + bank*0x4000` and written
to `CommData + !bank*0x4000` — the same "write into the bank the reader is not on" rule as the PS3.
The tail then does, on **both** flag bytes:

```
or  cl,0x80     ; set bit 7 (data ready)
xor bit 0       ; toggle the bank select
```

Note the PC build performs **more than the two cross-copies** decoded on PS3: it also copies each
board's own `CommSend` into `CommData + 0x700` (`P1 + 0x2E00`). Reading `CommData` as an array of
`0x700` slots indexed by cabinet — so every board receives *every* cabinet's data including its own
— fits both builds; the PS3 tail was only partially decoded, so treat this as the fuller picture
rather than a divergence.

### Board-bank switch — `0x180069D30` (the analogue of PS3 `FUN_00184D68`)

```asm
MOVSXD R8,ECX                  ; board index (0/1)
IMUL   RAX,R8,0x1B0            ; per-board struct, 0x1B0 stride
MOV    [0x1806911B4],R8D       ; <-- CURRENT BOARD INDEX
MOV    [0x180691148],RAX
CMP    R8D,0x1  / JNZ ...      ; board 1 work RAM 0x180F37010, board 0 0x180C37010
SHL    RAX,0x11                ; further per-board state at 0x20000 stride
IMUL   RAX,R9,0x118            ; and 0x118 stride from 0x1807D2750
```

So the DLL, like the PS3, is **one emulator holding two banks of board state**, with a switch that
re-points work RAM, the comm block and the per-board structs. `0x1806911B4` is the live
"which board am I" global.

### PC frame driver — `0x180073910` (analogue of PS3 `FUN_00184DF8`)

```asm
call 0x1800856B0
cmp  byte [0x1806921DF],0        ; gate A — link enabled
jz   done
inc  dword [0x180C22720]         ; link frame counter
call 0x18006A310                 ; LINK TRANSFER
xor  ecx,ecx / call 0x180069D30  ; select board 0
call 0x18005C7E0 / call 0x1800440C0    ; step board 0
cmp  byte [0x1806910DE],0        ; gate B — TWO-BOARD MODE
jz   done
mov  ecx,1  / call 0x180069D30   ; select board 1
call 0x18005C7E0 / call 0x1800440C0    ; step board 1
xor  ecx,ecx / call 0x180069D30  ; back to board 0
```

Line for line the PS3 driver, minus its half-rate check.

### *** THE ONE THING THAT IS OFF: `gateB` is never set ***

Every access to the two gates, decoded exactly (matching the real rip-relative byte forms rather
than guessing instruction lengths):

| gate | reads | writes |
|------|-------|--------|
| **`0x1806910DE` two-board mode** | **15** | `0x18005A2F0` → 0, `0x18006BACA` → 0, `0x180073340` → `al` (0, in teardown state 0x10) |
| `0x1806921DF` link enabled | 1 | `0x18007384F` → **1**, `0x180073328` → `al` (0, teardown) |
| `0x1806910DF` companion | 3 | `0x1800707DD` → **1**, `0x18006BAB9`/`0x18005A2DF` → 0 |

So: the link path **is** enabled by module code, and the two-board machinery is **fully compiled in
and gated in 15 places** (frame driver, plus clusters at `0x180070900`–`0x180071180` that look like
the render/input per-board branches) — but **nothing in the DLL ever writes `gateB = 1`**. It is
only ever cleared. That is precisely what "the DLL has no two-player mode" means at the byte level:
not missing code, just a switch that is never thrown.

**Implication: YAMP throws the switch.** Everything else already exists.

### *** VERIFIED WORKING — 2026-08-03, board 1 runs ***

Implemented as `ImportSymbol::TWO_BOARD_GATE` (K2 `ImportSymbols`) plus `-von-2board` in the K2
host, and measured against a `-von-2probe` control arm that logs the same state without throwing
the switch.

**One correction to the "one byte" framing: a single write is not enough.** Writing the gate once
after `module_start` is undone — measured `gate=1` at frame 0 and `gate=0` by frame 200, because
the module's own mode machine passes through state 0x10 during bring-up and that state zeroes the
gate along with the board index and the link-enabled gate. The gate has to be **held**: the host
reasserts it each frame before `module_main`. (Crude on purpose — the proper fix is to write it at
the right point in that state machine, once board-1 bring-up is settled.)

With it held, board 1 comes alive, and the ROM itself notices:

| | control (`-von-2probe`) | two-board (`-von-2board`) |
|---|---|---|
| `B1 BoardType` / `GlobCntr` / `MainMode` | 0 / 0 / 0 | **1 / advancing / 1** |
| `net_flag` (both boards) | 0 | **1** |
| `link_ID` | 3 (standalone) | **B0 = 1, B1 = 2** |
| `CommSend` non-zero bytes | 0 / 0 | **24 / 24** |

`GlobCntr` advances **identically on both boards** (68/68, 141/141, 214/214, 287/287, 379/379), so
the two boards are stepping in lockstep. `net_flag` going to 1 and `link_ID` splitting into two
distinct cabinet identities are the ROM's *own* reactions — nothing in YAMP writes either — which
is the strongest confirmation that this is the genuine linked-cabinet path and not a byte that
merely happens to start some code. Run stays clean: `module_stop -> 0x0`, only the four by-design
SRV-probe AVs.

Probe RVAs used (safe to hardcode only because GameVerify pins the module by SHA-256): board work
RAM for i960 `0x500000` is at RVA `0x1337020` (board 0) and `0x0E37010` (board 1) — note the bank
switch stores **bias** pointers (`region - i960_base`), not region bases; reading its `LEA` targets
as bases is wrong and was the first probe's error.

RVAs (the module is ASLR'd — resolve against the live base or by pattern, never absolute):

| RVA | what |
|-----|------|
| `0x6910DE` | **two-board gate — write 1** |
| `0x6921DF` | link-enabled gate (module sets this itself) |
| `0x6910DF` | companion flag (set by `0x1800707D0`) |
| `0x6911B4` | current board index |
| `0xC22720` | link frame counter |
| `0x6A310` | link transfer |
| `0x69D30` | board-bank switch — `switch(int board)` |
| `0x73910` | frame driver |
| `0x72FB0` | 23-state mode machine (state at `[obj+0x58]`, jump table `0x18007344C`) |

---

## Network Link Attribute — the service-menu setting (from the ROM, IDA on 7331)

`von_prog.bin` (i960, 0x0-0x200000, md5 `70BAFDB791097248DCBAA81EAA1FAC6E`).

**Backup-RAM byte: `0x01D00028`** (backup base `0x01D00000`, item `+0x28`).

| value | menu string | → `link_ID` (`0x005770B1`) |
|-------|-------------|---------------------------|
| **0** | ` SLAVE` | 2 |
| **1** | ` MASTER` | 1 |
| **2** | ` NOLINK` | 3 (STANDALONE) |

`InitNetwork` (`0x18A10`) is the whole mapping:

```c
u8 attr = *(u8*)0x01D00028;
if      (attr == 0)          { link_ID = 2; *(u32*)0x503A08 = 1; }   // SLAVE
else if ((attr & 0xFF) == 1) { link_ID = 1; *(u32*)0x503A08 = 0; }   // MASTER
else                         { link_ID = 3; *(u32*)0x503A08 = 0; }   // STANDALONE
Net_check();                                                          // sub_C5870
```

`link_ID` values are named by `Net_check` itself, which branches on them to print
"THIS IS RELAY SITE" (0) / "MASTER SITE" (1) / "SLAVE SITE" (2) / "STANDALONE MACHINE" (3), and
"Illegal link_ID Number." otherwise. **`Net_check` skips the entire sync path when `link_ID == 3`**
— which is why NOLINK cannot be left in place for netplay.

Corroboration: the menu value strings are an 8-byte-stride table at `0x0F0D20` in the order
SLAVE / MASTER / NOLINK, so the menu index *is* the backup byte. **Offset user-verified against
another emulator, 2026-08-03.**

### The module writes it itself — PROVEN, so YAMP injects nothing

`FUN_18006D1E0(u32 i960_addr, u8* value)` is the module's **backup-RAM injector**, called from 14
sites. It maps i960 to native as `DAT_1806912A0 + 0x91 + (addr - 0x1D00000)`, so the live backup
RAM base is **`DAT_1806912A0 + 0x91`**. Addresses written: `0x01D00016`-`0x01D0001C`, `0x01D0001D`,
`0x01D0001F`, `0x01D00021`, `0x01D00024` (itself gated on two-board mode), `0x01D00028`,
`0x01D00034`.

The `0x01D00028` write is at `0x180070A3B`, and it is exactly the linked-cabinet assignment:

```asm
mov  byte [rsp+0x20], 2          ; default = 2 = NOLINK
cmp  byte [0x1806910DE], 0       ; the two-board gate
jz   write
cmp  dword [0x1806911B4], 0      ; current board index
sete byte [rsp+0x20]             ; board 0 -> 1 (MASTER), board 1 -> 0 (SLAVE)
write:
mov  ecx, 0x01D00028
call 0x18006D1E0
```

**The whole chain is now closed with measurement, not inference**: DLL write -> backup byte ->
`InitNetwork` -> `link_ID`, and every value matches the live run (gate off -> 2 -> `link_ID` 3;
gate on -> board 0 = 1 -> `link_ID` 1, board 1 = 0 -> `link_ID` 2). Throwing the gate is sufficient;
there is nothing for YAMP to inject.

`0x00503A08` is an unnamed "this cabinet is the slave" flag (1 for SLAVE, 0 otherwise). Adjacent
Game Assignments live at `0x01D0001D`, `0x01D0001F`, `0x01D00020`.

### Why this is per-BOARD and not per-PEER

Because both peers simulate both boards, the attribute is a property of the **board**: both peers
must have board 0 = MASTER and board 1 = SLAVE *identically*, or the two simulations diverge. What
host/guest selects is only **which board you render and drive** (`NetSession`'s `localPad`). So this
is NOT a room flag like `REAL_DAMAGE` / `VS_MODE` — those exist because peers must agree on a
negotiated value; this one is fixed by the two-board topology.

---

## What the bank switch actually re-points (the per-board state map)

Full body of `0x180069D30`, indexed by the board index in `R9`. This is the map that decides where
input and rendering have to be intercepted:

| global | value | stride | what |
|--------|-------|--------|------|
| `0x1806911B4` | board index | — | "which board am I" |
| `0x180691148` | `0x1807C21D0 + b*0x1B0` | 0x1B0 | per-board struct |
| `0x180C36FF8` | `0x180C37010 / 0x180F37010` less 0x200000 | — | i960 0x200000 window (bias) |
| `0x180C37000` | `0x181337020 / 0x180E37010` less 0x500000 | — | i960 0x500000 window (bias) |
| `0x1807D2980/88` | `0x180BEEEC0 + (b << 17)` | 0x20000 | |
| `0x1807D2748` | `0x1807D2750 + b*0x118` | 0x118 | |
| `0x1807D2740`, `0x1807DADB8` | `0x1807DAB80 + b*0x11C` | 0x11C | |
| `0x1806908A8` | `0x1806927A0 + b*0x9058` | 0x9058 | |
| `0x1807D2B78` | `0x1807DADC0 + b*0x20A080` | **0x20A080 (~2 MB)** | video/framebuffer-sized — the presentation lead |
| **`0x1806912A0`** | **`0x180C2EEC0 + b*0x409C`** | **0x409C** | **backup RAM + I/O buffer** |
| `0x1807C2728` | `0x1807C2730 + b*0x8008` | 0x8008 | comm block (matches the link transfer) |
| `0x180C37008` | `0x181437020 + b*0x988A0` | 0x988A0 | per-board state |

**Backup RAM and the I/O area are per-board.** That is why `backup_write` lands a different link
attribute on each board — it writes through `DAT_1806912A0`, which points at whichever board is
currently selected. `module_main`'s coin byte (`DAT_1806912A0 + 0x4098`) is per-board for the same
reason.

## Input path

- `FUN_180081140(int player)` — the pad reader. `IMUL RSI, player, 0x170` then
  `[execute_info + RSI + 0x20]`, i.e. the K2 `pad[player]` layout (0x170 stride at +0x20), into a
  per-player block at `0x180691A00 + player*0x54`. `execute_info + 0x15E4` selects a 0xC0-byte
  control-mapping template from a table at `0x18012ABB0`.
- `FUN_180004BE0` drives it, and **reads BOTH pads every frame regardless of two-board mode**:
  `for (i = 0; i < 2; ++i) pad_read(i);` then `for (i = 0; i < 2; ++i) FUN_180004CC0(i);`
  (per-player structs of 0x158 bytes).

So both players' inputs are always built. The reader's *output* goes only into the per-player block
(`[0x180691A00 + player*0x54 + n]`, after remapping pad bits to i960 I/O bits) — it never touches
the per-board I/O buffer directly. The only static references to `0x180691A00` are the initialiser
at `0x180081060` (which zeroes both blocks, confirming 2 players × 0x54) and the reader's own
`LEA RBP`. So the consumer reaches those blocks through a computed pointer, and **which board reads
which player's block is still unproven.**

## Measured, three arms × 1000 frames (2026-08-03)

`-von-2probe` (control) / `-von-2board` / `-von-2board -von-padtest` (pad[1] held `0x000F`,
pad[0] forced idle).

### Presentation — board 1 IS rendering, into its own region

| | control | two-board |
|---|---------|-----------|
| `output_texid` | 2 | **2 (unchanged)** |
| `vid0` hash (`0x1807DADC0`) | changes every sample | changes, **identical to control at the same frames** |
| `vid1` hash (`+0x20A080`) | **frozen** at `21FDBCF8` | **changes every sample** |

Two conclusions. **`0x1807DADC0 + b*0x20A080` is confirmed per-board video state** — dead when the
gate is off, live when it is on. And **`output_texid` never changes**, so the host is only ever
handed board 0's texture: presenting board 1 needs an explicit second output, it does not fall out
of enabling the board.

Bonus determinism signal: board 0's video hashes in the two-board arm are *bit-identical* to the
control's at the same frames, so bringing board 1 up does not perturb board 0.

### Why `output_texid` never changes — rendering is NOT per-board

`module_main`'s shape explains it:

```c
if ((*(code**)(*DAT_180691318 + 0x10))(obj))   // the FRAME STEP: steps board 0, then board 1
{
    (*(code**)(*obj + 0x18))(obj);              // render slots - run ONCE, after both boards
    (*(code**)(*obj + 0x20))(obj);
    (*(code**)(*obj + 0x28))(obj);
}
*(u32*)(exec + 0x18) = *(u32*)(DAT_180691300 + 0x14);   // output_texid, from a SINGLE object
```

Both boards step inside slot `0x10`; the render then runs **once**, against whichever board is
still selected. The frame step's final act is `XOR ECX,ECX; CALL bank_switch` — it re-selects
board 0 — so board 0 is always what gets drawn. `DAT_180691300` is a single render-output object
and is **not** in the bank switch's re-point list, which is the direct cause.

**`ImportSymbol::RENDER_BOARD_SELECT` + `-von-render1`** patches those two bytes to `MOV CL,1`
(same length), leaving board 1 selected so the render draws the other cabinet. Implemented, byte
pattern verified before writing, and **runs stably** (1000 frames, `module_stop -> 0x0`, only the
four by-design SRV AVs). **NOT yet confirmed on screen** — `output_texid` stays 2 either way
because it is the same output object; only the CONTENT should differ, which needs a human to look.

Good visual tell: `Net_check` prints "THIS IS MASTER SITE" vs "THIS IS SLAVE SITE" from `link_ID`,
so the two boards should differ visibly during the link check at boot.

### Input routing — the probe was inconclusive, but static tracing SOLVED it

The `-von-padtest` arm showed `io0 == io1` and no response to holding pad[1]. That was **my probe's
fault, not a result**: the window sampled (`+0x4091`) reads `0000007F0000...` in the control too and
never moves — dip/coin state, not pad input.

Tracing the path properly settles it, and the answer is that **routing already works and YAMP has
nothing to do**:

```
execute_info.pad[N]
  -> per-player block  0x180691A00 + N*0x54    FUN_180081140(player): [exec + N*0x170 + 0x20],
                                               remapping pad bits to i960 I/O bits
  -> input struct      0x1807A5DA0 + N*0x9A0   at 0x180058E11, immediately after the both-pads read:
                                               `for (i=0;i<2;++i) FUN_1800691A0(&s[i], i);`
  -> board N's I/O buf DAT_1806912A0           FUN_18006C7E0 reads the BOARD INDEX from
                                               `0x1807A8FDC` (written by the bank switch), selects
                                               `0x1807A5DA0 + board*0x9A0`, scales and writes buf[0..]
  -> i960 I/O read     0x1C00002/4/6           FUN_18006CBE0, off DAT_1806912A0 +8..+0xC.
                                               Port 0x1C00002 is a Model 2 multiplex: buf[8] bit 0
                                               selects buf[9] or buf[10].
```

The pad index fills the struct and the **board index** selects it, so the binding is 1:1 and
per-board by construction. **Filling `execute_info.pad[0]` and `pad[1]` is sufficient** — identical
to StF/FV/VF2, so `NetSession`'s overwrite-both-pads behaviour applies unchanged.

---

## The ROM frame counter — measured, not guessed (`-von-findctr`)

`NetSession` needs a value that advances exactly once per emulated frame: it is both the round
anchor and the desync canary, and getting it wrong is what put VF2's peers a frame apart. Rather
than trust the symbol table, `-von-findctr` sweeps every dword in the i960 `0x500000` window (512 KB)
and classifies each `module_main` call as `+1`, `hold`, or anything else.

Over 900 calls, exactly three dwords behave like a frame counter, and they move in lockstep:

| i960 | symbol | `+1` | hold | other |
|------|--------|------|------|-------|
| `0x5024E4` | `SynchTime` | 311 | 585 | 4 |
| **`0x5024E8`** | **`GlobCntr`** | **311** | **585** | **4** |
| `0x5039FC` | unnamed (between `MainMode` and `SubMode`) | 311 | 585 | 4 |

**311 advances in 900 calls is correct, not broken.** The host loop was running uncapped (~170 fps)
and the board at ~59 Hz, which is Virtual On's real rate; the 585 holds are calls where the board
did not advance. `NetSession::EndFrame` already only counts a netplay frame when the canary moves,
which is precisely the VF2 fix — VF2 just had 5% holds where VON has 65%.

`GlobCntr` also reads **identically on both boards** (66/66, 141/141, 214/214, 287/287, 379/379), so
one value anchors and canaries the pair.

=> `DwGame.romFrameCounter = 0x5024E8`, `DwGame.rvaRamBasePtr = 0xC37000` (the bank switch stores
`region - 0x500000` there, so "guest G at base+G" works verbatim, and it is re-pointed per board).

**Caveat before using it as the anchor:** the 4 `other` events. The counter reached 322 with 311
single steps, so the jumps total ~+11 — small, and most likely boot/state transitions rather than
the simulation skipping. They must be explained (or the anchor moved past them) before a round can
rely on it, because the anchor's whole job is to be reached at the same value on both peers.

The earlier "~0.37 per frame" reading of `GlobCntr` was this same behaviour seen through a 200-frame
sampling window — it looked like a counter that was not per-frame, when it is one that holds.

---

## `rvaResetHandler` — RVA `0x6BA90`, and it clears the gate

omg carries the same stripped "dw" debug-window descriptor tree as StF/FV. Root **DEBUG MENU**
window header at RVA **`0x476248`** (title string at `0x455B38`), 8 items:

| item | action RVA | |
|------|-----------|---|
| STEP | `0x06BA70` | `mov [run_state],0; mov eax,1; ret` — stub |
| STEP OUT / GO | `0x06BA80` | `mov [run_state],2; mov eax,2; ret` — stub, shared |
| REGS | `0x06B9D0` | |
| **RESET** | **`0x06BA90`** | **the real board init** |
| 960STAT / PERFORMANCE / CONFIG | `0x476228` / `0x4761E0` / `0x4761C0` | submenus |

```c
run_state = 0;                                        // _DAT_1807D2B10, RVA 0x7D2B10
FUN_1800610D0(); FUN_180061590(5); FUN_180012D40();   // the real init
DAT_1806910DF = 0;                                    // companion flag
DAT_180691290 = 0;
DAT_1806910DE = 0;                                    // <-- THE TWO-BOARD GATE
*(u32*)(DAT_180C37008 + 0x98897) = 0;
FUN_180069E80();
```

=> `DwGame.rvaResetHandler = 0x6BA90`. Run-state global is RVA `0x7D2B10`.

**RESET CLEARS THE TWO-BOARD GATE.** `NetSession` resets the board at round start, so without care
every round would drop VON back to one cabinet. The per-frame gate reassert in `K2Host` — added as
a crude workaround for the mode machine zeroing it during bring-up — is therefore **required**, not
a shortcut. Left unhandled this would have presented as "netplay works, but the second cabinet
disappears the moment a match starts".

**Open:** whether RESET re-initialises BOTH boards or only the selected one. Board state is
per-bank, so the likely answer is "only the selected one", meaning round start needs
reset-board-0 / switch / reset-board-1 / switch back. Measure before building it.

---

## `rvaRngHolder` — RVA `0x690810`, and it is SHARED between the boards

Built at `0x18000A990`, the same shape as StF/FV/VF2:

```asm
CMP  qword [0x180690810], 0      ; built once
MOV  ECX,0x80 / CALL allocator   ; 0x80-byte block
MOV  [RAX], 5                    ; count = FIVE
LEA  RBX,[RAX+8]                 ; objects start at +8
LEA  R9,[0x18000A860]            ; per-object ctor  (dtor 0x18000A880)
MOV  EDX,0x18                    ; element size
LEA  R8D,[RDX-0x13]              ; count = 0x18-0x13 = 5   (a cute way to write 5)
CALL 0x18009D478                 ; vector ctor
MOV  [0x180690810], RBX          ; <- the holder global
```

The stored pointer is `alloc+8` = object 0; objects are `0x18` apart and each holds its Mersenne
Twister state pointer at `+0x08`, so the slots are `0x08, 0x20, 0x38, 0x50, 0x68` — **exactly the
shared `RNG_STREAMS` table**. No new stream list is needed.

=> `DwGame.rvaRngHolder = 0x690810`, `rngStreams = RNG_STREAMS`, `rngStreamCount = 5`.

**SHARED, NOT PER-BOARD.** `0x180690810` is absent from the bank switch's re-point list, so both
boards draw from one set of five generators. Seeding once covers the pair — which corrects the
earlier note in this file warning that RNG seeding would have to be done per board. (Both boards
drawing from one stream is still deterministic: every peer steps board 0 then board 1 in the same
order, so the draw sequence is identical.)

---

## `rvaBootState` — RVA `0x7ADCA8`

The boot-phase machine is `0x18006A100`:

```asm
MOV ECX,[0x1807ADCA8]        ; the boot phase
TEST ECX,ECX / JZ  phase0
SUB  ECX,1    / JZ  phase1
CMP  ECX,1    / JZ  phase2   ; anything else -> return false
phase1:  CALL 0x180070580            ; ROM load step; on success:
         INC dword [0x1807ADCA8]     ; advance the phase, fall into phase 2
phase2:  set the i960 window biases (0x180C36FF8, 0x180C37000)
         CALL 0x18006E620 / 0x1800526D0
         CALL 0x18006BAF0(0)  and  CALL 0x18006BAF0(1)    ; PER-BOARD INIT, both boards
         memset(0x1807D2B80, 0, 0x8000)
         LEA RAX,[0x180476248] / MOV [0x1807D2B18],RAX    ; the DEBUG MENU root window
         MOV dword [0x1807D2B10], 2                       ; run state = 2
         MOV byte  [0x1806910EA], 1
```

=> `DwGame.rvaBootState = 0x7ADCA8`. Identified rather than guessed: phase 2 installs
`0x180476248`, the exact debug-menu root header found above, which is the same "ROM load + i960
DEBUG MENU init" pairing documented for StF.

Other globals named here: `0x7D2B10` run state, `0x7D2B18` debug-menu root pointer, `0x7D2B14`.

**`0x18006BAF0(board)` is a per-board init**, called for board 0 AND board 1 at boot. That is very
likely what the open "does RESET re-init both boards?" question needs: RESET calls a different
path, so a two-board round start may simply need this called for each board.

---

## HLE hook table — RVA `0x476510`, **121 hooks** (the largest of the four games)

Found by record shape rather than by pattern: 16-byte records `{u64 rom_offset, u64 handler}` where
the offset is < 0x200000 and the handler points into `.text`. One run of 121 in the whole image.

=> `DwGame.rvaHleTable = 0x476510`, `hleCount = 121` (StF 76, FV 95, VF2 67).

Cross-referencing the ROM offsets against the module's own symbol table (RVA `0x4507E0`) names most
sites — `0x0187E0` ≈ `__mainloop__`, `0x018AB4` ≈ `synch`, `0x018A10` = **`InitNetwork`**,
`0x0F5060` = `rand.lf+8`, plus clusters in `SelectMove`, `LinePut`, `dispJunk`, `Ending2/3`,
`execObstacle`, `ai_init`, `Replay`, `execRader`. **`0x070FB0` recurs dozens of times** — the shared
"execute original" tail, the same arrangement StF and FV use, so most of the 121 are Inert.

This is the basis for a future `docs/omg-hle-hooks.md` in the style of `fv-hle-hooks.md`; note that
hook 7 (`InitNetwork`) and hook 3 (`synch`) sit directly on the linked-cabinet path.

## `rvaTexBudgetHandler` — appears to be ABSENT, which is a result, not a gap

The wall-clock wrapper is `0x1800856B0` (it owns one of the module's two `QueryPerformanceCounter`
call sites; the other is `0x18009E062`). It has **49 call sites**, and they cluster in
`0x180058xxx` (frame pacing/profiling), `0x180069xxx`, `0x180072xxx`, `0x18000Axxx` (the RNG
seeder's three consecutive reads), `0x180073xxx` (the frame step), `0x18005Cxxx`/`0x18005Exxx` and
`0x180001xxx`.

**Not one of them is an HLE handler.** Every handler lives in `0x180070xxx`-`0x180071xxx` plus
`0x180081050` and `0x180004A10`, and none of those appear among the callers. So on the evidence
there is no wall-clock budget check inside the emulated simulation to repoint, and
`DwGame.rvaTexBudgetHandler = 0`.

**Caveat, stated rather than hidden:** this rules out a DIRECT call. A handler could still reach the
timer through one level of indirection — the `0x180072xxx` callers sit suspiciously close to the
handler range and could be helpers. The definitive check is a breakpoint on `0x1800856B0` during a
run, reading the return address; that "breakpoint it and read the actual value" method is what has
settled every other ambiguity on this DLL, and static call-graph reasoning is what has repeatedly
misled. Do that before relying on the zero.

---

## Open items

1. **Decode `CSyncIoMsg`'s payload** (PS3 typeinfo `0x00446B20`) to prove the wire carries pads
   only. Everything else in the design hangs off this. Needs the PS3 program reloaded in Ghidra.
2. **Verify the one-byte theory by running it.** Set `gateB` after `module_start` and see whether
   the second board actually steps. Expect fallout: board 1's work RAM (`0x180F37010`) and its comm
   block must be initialised, and `output_texid` will still be board 0's — presenting the *other*
   board is a separate problem (see 3).
3. **Which board renders, and how to get the other one.** The 15 gated sites at
   `0x180070900`–`0x180071180` are the place to look; one of them almost certainly selects the
   presented framebuffer per board.
4. **Pad routing.** Confirm the `0x16E0` execute_info's `pad[player]` (+0x20, 0x170 stride, per
   [[kiwami2-recon]]) reaches board N — i.e. whether the bank switch re-points input, or whether
   both boards read `pad[0]`.
5. Which of these are HLE-hooked in the **PC** module (the hook table is a DLL artifact —
   `HleHooks` currently covers StF 76 / FV 95 only). Enumerate `omg`'s table the way
   `docs/fv-hle-hooks.md` was built.
4. `GlobCntr` (`0x005024E8`) looks like the natural desync canary — it is the ROM's own shared
   counter. Confirm it advances exactly once per emulated frame before using it, the way VF2's
   frame counter had to be rejected in favour of a work-RAM hash.
