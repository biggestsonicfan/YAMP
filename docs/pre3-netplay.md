# Netplay: the pre3 Model 3 boards (Fighting Vipers 2)

Status as of 2026-08-04, branch `pre3-model3-fv2`. Everything below is **verified against the
shipped module** (`pre3-pxd-w64-d3d12_retail.dll`, 2024-03-10, sha `B44ED5A7…B98E`) either by
static analysis or by a headless `-fv2 -frames N` run; anything not verified says so.

**NETPLAY WORKS.** Two machines, a round against a CPU opponent, and **the AI behaved identically on
both screens** — which is the test that matters, because the AI is the most sensitive thing two
emulated boards can be seen to disagree about and it is downstream of everything else being right.
`source/pre3/NetSession.{h,cpp}` drives the round, the host loop calls it, and the plugin ABI carries
the pre3 `execute_info`. §7a-§7c are what the two-machine rounds established, including the two
shared-plugin bugs they exposed; §8 is what is still open.

---

## 1. The short version

**The pre3 board is frame-deterministic by construction, there is exactly one host-varying input
(the real-time clock), and the board already carries its own savestate machinery — so a round is
"pin the clock, restore a saved board, exchange pads" and very little else.**

The one thing the original recon missed, and it cost a round: **the emulator runs on its own thread**
(§3.8), so the board is only safe to read inside the update stage.

This is a much better starting position than the m2ftg boards, where instruction-count-driven
timers are what made VF2 netplay hard (see `docs/vf2-hle-hooks.md`). Nothing here samples elapsed
time.

| | m2ftg (StF / FV / VF2) | pre3 (FV2 / SRC2) |
|---|---|---|
| CPU advance per frame | driven by emulated timers | **fixed instruction count** |
| RNG | `rand` is an HLE hook fed by a host RNG, must be re-seeded per round | **no RNG hook at all** |
| host-varying input | host RNG + timers | **the RTC, and nothing else** |
| hooks | trap words written into the ROM image | handler substitution in a decoded trace |
| round reset | re-run the ROM's init, then settle ~40 frames | **restore a savestate: one frame** |
| when the seed is applied | before the barrier, where a guest reads 0 | **after it, where it is real** |

---

## 2. Fighting Vipers 2 is NOT a linked-cabinet game

Worth stating because the module has a link interface and it is easy to reach for.

* **FV2** is one board, two players, **one shared screen**. Netplay is therefore ordinary
  **input lockstep on a single simulated board**: both peers run the whole board, each supplies
  one pad, and the two must agree frame for frame. Exactly the model the existing
  `YampNet` plugin ABI already describes (delay-based lockstep, no rollback, no state save).
* **Sega Rally Championship 2** *did* use linked cabinets, and that is what the module's
  `params + 0x1070` interface is for. It is not the FV2 path and should not be pressed into
  service as one — see §6.

---

## 3. Determinism inventory for FV2

### 3.1 CPU advance per frame — deterministic ✅

The board computes its per-frame instruction budget once, at DLL `0x180038D60`:

```
clock  = rom->vtable[33]()            ; the ROM object's CPU clock — FV2: 133,000,000
per    = clock / 60                   ; the 0x88888889 / shr 5 divide-by-60 idiom
[b+0x78] = (int)((float)per * K1 / K2)   ; K1..K4 are compile-time float constants
[b+0x7C] = (int)((float)per * K3 / K4)
[b+0x80] = per - [b+0x7C] - [b+0x78]
```

The frame step (`FUN_18003B0A0`) then runs the CPU for `[b+0x7C]`, `[b+0x78]` and two literal
`0xC8` slices. **No wall clock, no elapsed time, no host-speed dependence anywhere in the frame
path.** FV2 advances 2,216,666 instructions per emulated frame on every machine.

### 3.2 Boot state — deterministic ✅, and it is also the RESET

The board does not cold-boot **in VS mode**. `FUN_1800382F0` restores a **saved machine state**
from `image/fv2/vs_start.bin` — CPU registers, both 0xC00000 RAM banks, 0x800000 of VRAM, the
scroll buffer — then applies a small patch list (`M3ERomFv2` vtable slot 26, `FUN_180036F70`).
Same file, same bytes, same start state on both peers.

That mechanism is what a round now starts from, and it belongs to the module — nothing about it is
YAMP's invention. It is driven by a **request bitfield** at `machine + 0x120` which the frame step
drains at the top of every frame (`FUN_180037E50`):

| bit | request |
|---|---|
| 0 | save state (push onto the undo vector at `machine+0xF8`) |
| 1 | restore the newest saved state |
| 2 | pop the newest saved state |
| 3 | discard the whole save vector |
| **4** | **restore a PRELOADED state; bits 5-6 select the slot** |
| 7 | set by the handler once a preloaded restore has happened |

`vs_start.bin` is read into the preloaded vector at `machine+0x108` (count at `machine+0x114`) late
in bring-up regardless of VS mode; the module's *own* auto-restore is what is gated on the config's
VS byte (`if (config.is_vs_mode) request = (request & 0x9F) | 0x10;`, `FUN_1800393D0` case 0xE). So
`pre3::ResetBoard()` writes exactly that same value, and the game's own dip switch is irrelevant.

**Measured** on a headless run, requesting the restore at frame 300: the request was outstanding for
exactly one frame, the canary changed across it, and the board's game-mode pair left the attract
sequence (`01/00` → `02/00` → `04/00` → `05/00`). One frame is the contract `NetSession` relies on.

**THE MACHINE OBJECT IS A STATIC, NOT A HEAP POINTER**, and getting this wrong costs a session. The
obvious candidate — the global `module_start` fills in at `0x180527BF8` — is `M2FTGAppModule`, an
**0x18-byte wrapper** whose slot 2 `module_main` calls. Reading the fields above off it lands in
three unrelated qwords and reads as "the board never booted". The real object is `TaskM3E`, a static
instance at **`0x1801895D0`**, whose vtable (`0x18010C3B8`, slot 2 = the frame step `0x1800393D0`)
is written by the module's C++ initialiser at `0x180001140`. The free corroboration:
`0x1801895D0 + 0x90 = 0x180189660`, the ROM-object global `module_main` independently uses for the
coin latch and the "press start" query.

### 3.3 Arcade settings — largely moot now, but watch it ⚠️

HLE hook 12 (handler `0x180028180`) writes the coin setting (free play → `0x1B`) and the
difficulty into the game's own RAM at `0x1205xx`, taken from YAMP's config
(`config+0x01` difficulty, `config+0x04` free play). `FUN_180036F70` additionally writes the
region byte to `0x120557` from `config+0x02`.

Starting the round from the savestate **overwrites all of it**: those addresses are inside the
0xC00000 RAM bank the restore rewrites, so after the reset both peers hold the bytes that were in
`vs_start.bin`, not their own `settings.ini`. That is why nothing here is negotiated in the room —
publishing a setting that gets overwritten a frame later would be theatre.

**The residue to watch** is anything the hooks re-inject *after* the restore. Hook 12 fires when the
ROM reaches guest `0x038B84`, which a state captured past initialisation should never do again — but
that is an inference, not a measurement, and it is the first thing to check if two peers with
different dip switches desync. The canary (§3.7) is what would catch it.

### 3.4 The HLE hook mask — settled by force, not by negotiation ✅

`source/pre3/HleHooks.cpp` lets the user disable any of FV2's 36 hooks live. Most are `Speed`
(native reimplementations of ROM routines, semantics unchanged) and are harmless to mismatch in
principle — but `Patch`, `Removed` and `Host` hooks change what the board does.

Rather than put a `uint64_t[2]` on the wire, **YAMP holds the board at the shipped default mask for
as long as `net::SessionInProgress()`** (the reconcile in `YAMPUserInterface::Draw` substitutes
`HleHooks::DefaultDisableMask()`), and the settings panel says so. Both peers compute the same value
locally, so there is nothing to negotiate and nothing to refuse. It is also the same rule every
other board-facing control already follows during a session: switched off for the duration.

Note the shipped default already disables hooks 7 and 10 (the start-up warning screen), so "both
peers on defaults" is not the same as "both peers with everything enabled" — which is exactly why
the default, and not "all on", is what gets forced.

### 3.5 The real-time clock — **the one hazard**, now solvable ✅

`M3EInput::get_time` (DLL `0x180033C40`, vtable slot 17) is the **only caller of `_time64` in the
entire module**:

```c
if (DAT_180527BE8 != 0) return *(__time64_t*)(rom + 0x470);  // board's own clock, fixed epoch
return _time64(NULL);                                        // HOST WALL CLOCK
```

`DAT_180527BE8` is written exactly once — `module_start` at `0x180041E90`, from
**`params + 0x1070`**, the link interface. YAMP passes null, so today **every machine seeds from
its own clock**.

**Fixed host-side** in `source/pre3/Determinism.{h,cpp}`: the same vtable-redirect mechanism the
TEST/SERVICE switches use (`pre3::FindBoardVtables` + `RedirectBoardSlot`) points slot 17 at a host
function that returns a fixed epoch when pinned and defers to the module otherwise.

```cpp
pre3::SetDeterministicClock(true, matchSeed);   // netplay: both peers, before the round
pre3::SetDeterministicClock(false);             // stand-alone (the default)
```

`epoch` defaults to the module's own reset value (`0x386D4380` = 2000-01-01 UTC); **the session
passes `YampNet_GetMatchSeed()`** — both peers then agree, and two matches do not replay
identically. This is the pre3 analogue of the m2ftg path's "re-seed the `rand` hook from the match
seed"; pre3 has no `rand` hook, and the clock is the only thing downstream of it.

Installed unconditionally at patch time and reported in the log:
`[FV2] deterministic clock available (8 vtable entries, ...)`. **Off until a round pins it**, and
un-pinned again when the round ends.

### 3.7 The desync canary ✅

`pre3::StateCheckValue()`, submitted once per emulated frame, is **two halves** because neither is
sufficient alone (both measured on a 400-frame headless run):

* the PowerPC **register file** — `cpu + 8`, where `cpu` is the same object the HLE hook table walks
  (`0x18062AA90`). Confirmed at runtime, not assumed: the module's own register-file global at
  `0x18052AA88` reads back equal to `cpu + 8` on a live board. It changed on **every single frame**
  of the run, because the frame ends after a fixed instruction count.
* the **main RAM bank**, one dword per 256 bytes across the whole 0xC00000. Comprehensive, but on a
  static screen it repeated its value for **five frames at a stretch** — a 256-byte stride does not
  land on the handful of words a title screen touches.

The register half is hashed from GPR0 (`+0x10`) through LR (`+0x1A0`) and **deliberately skips the
first 16 bytes** of the file (the PC and one unidentified qword). An unnamed field is the one place
a host-derived value could hide, and a canary that disagreed for *that* reason would end every round
with a false desync — a much worse failure than a canary that is merely slow.

Evidence that the register half is in fact simulation state and not host state: across three
separate headless runs — with the module at three *different* ASLR bases — the canary at the same
emulated frame came out identical (`0x2CF07F1D`) every time. That is the board being reproducible
run-to-run on one machine, which is the strongest single-machine proxy there is for two machines
agreeing, and it is the check to re-run if the canary is ever extended.

### 3.8 THE EMULATOR IS ON ITS OWN THREAD ⚠️ — the fact that was missing

This was not in the original recon, and its absence cost a two-machine round. **The board does not
run inside the update stage.** `FUN_18003B0A0` — the frame step that runs the CPU for its fixed
instruction budget — is the body of a **worker thread**, `FUN_18003B270`, created under the name
`"m3e_ctrl"` (DLL `0x1800393D0` case 0xF). Its loop is:

```
run one frame step (FUN_18003B0A0)
machine+0x16C = 0            ; clear the busy flag
sleep against a WALL CLOCK to hit the frame period at machine+0x150
machine+0x148 = timestamp    ; stamped ONCE per completed frame
ReleaseSemaphore(done)       ; -> the update stage's wait at machine+0x168
WaitForSingleObject(go)      ; <- the update stage's SetEvent at machine+0x15C
machine+0x16C = 1
```

The update stage is the other half: it waits for the worker at the top, does its own work (the
savestate pump, the screen capture) **while the worker is parked**, and releases it on the way out.

Two consequences, and they pull in opposite directions:

* **Frame accounting is fine.** One update stage is exactly one emulated frame, by handshake. The
  m2ftg "did this call actually advance a frame?" test genuinely is not needed here.
* **The safe read window is the update stage's own body, which the host is never inside.** Reading
  board state immediately after the update stage *returns* reads it while the worker is running the
  next frame.

**That race was the frame-4 desync.** Two machines with identical `settings.ini` (diffed: only the
account name differed), an identical match seed, and a restore the canary itself confirmed was
bit-identical — the plugin logged no baseline offset at frame 0, which means the two hashes were
*equal* — nonetheless disagreed at frame 4. They were not disagreeing about the simulation; they
were sampling it at different points inside it.

The module publishes everything needed to do it properly, and its **own savestate handler leans on
the same flags** (`FUN_1800382F0` refuses to restore while either is set), so respecting them is the
module's contract rather than an invention:

| field | meaning |
|---|---|
| `machine+0x16C` | the `m3e_ctrl` worker is inside the frame step |
| `machine+0x16D` | the `m3e_disp` worker is inside the memory object |
| `machine+0x148` | a timestamp the worker stamps once per completed frame — **a sequence marker** |

`pre3::WaitForEmulatedFrame` captures the marker before the update stage releases the worker and
waits for it to move afterwards. **Waiting on the busy flags alone is not enough**: finding them
clear cannot distinguish "the frame finished" from "the frame has not started yet", and picking
wrong puts the sampled frame back under host timing — which is the bug, not the fix.

### 3.6 What is NOT a hazard

* **No RNG hook.** Searched exhaustively: no `rand` symbol; `MtRandom`/`RandState` RTTI exists but
  belongs to the pxd platform layer, not the emulator; none of FV2's 36 HLE handlers touches a host
  entropy source.
* **No analogue inputs.** FV2 never reads the ADC ring (port `0x3C`); it is digital-only. SRC2 does,
  and will need its wheel/pedals synchronised.
* **Audio** runs through the host CRI engine and feeds nothing back into the board.

---

## 4. The input path

Both pads live in `execute_info` and are written by `pre3::GameLoop`
(`source/pre3/Gaiden/Pre3Host.cpp`):

| field | offset | what |
|---|---|---|
| `pad[0..3]` | `+0x20`, stride `0x190` | the pxd `csl_pad`. FV2 reads the first two |
| `assign[2][8]` | `+0x1684` | button slot → P/K/G table, `Input::MODULE_ASSIGN` |
| `status` bit 5 | `+0x10` | coin inserted (one-shot) |
| `status` bit 6 | `+0x10` | module → host: "insert coin / press start" screen |

The board copies both pads into its own object each frame (`FUN_1800336D0`) and caches
`pad[n].m_now` at `rom+0x360` / `rom+0x364`, which the game's mapper turns into the JAMMA
registers. **For netplay the transport only has to carry `m_now` (and the analogue axes for SRC2):
everything else is derived.**

`pre3::SetSystemSwitches` (TEST/SERVICE) writes a host-side mask sampled at read time and is **not**
part of the pad — so, like the m2ftg path, it is suppressed for the duration of a session or one
machine drops into the operator menu while the other plays.

---

## 5. What was built

### 5.1 The ABI change — one handshake, two struct families

`yampnet_layout` used to compare `execute_info_size` for **equality** against m2ftg's 0x1760, which
would have refused pre3's 0x1780 outright. The fix is not a pre3 variant: the two families' **pad
geometry is byte-for-byte identical** (same `pxd::lj_pad_t`, same 0x20 / 0x1B0 offsets, because they
are the same pxd generation), and the pads are the *only* thing the plugin touches.

So since **ABI 7** the host declares the SMALLEST `execute_info` it may ever pass, the plugin
compares the five pad fields exactly and bounds-checks the size (`pad1_offset + pad_size` must fit),
and `source/net/NetPlugin.cpp` carries `static_assert`s that stop the build if a future module ever
moves a pad. The session is created before YAMP knows which game will run, which is why an identity
test was the wrong shape in the first place.

### 5.2 The round — determinism AFTER the barrier, not before

`pre3::NetSession` is a deliberate sibling of `m2ftg::NetSession`, not a reuse: no RNG seeding, no
texture-budget pin, no "did this call actually advance a frame?" test, no settling window. All four
exist on the m2ftg path for problems this board does not have (§1).

The sequencing difference is the one that matters, and it **fixes a real m2ftg bug rather than
inheriting it**. There, the RNG must be seeded *before* the reset (the ROM's re-init draws from it)
while the ABI only promises the match seed from `SYNCING` onwards — so a guest seeds with zero and
the peers diverge before frame 0, a known bug documented in that file. A pre3 reset is a single
savestate restore, so the whole sequence fits *after* the barrier where the seed is real:

```
Idle      -> announce once the board has booted, and HOLD the emulator
(barrier) -> pin the clock to the match seed, ask for the savestate restore
Resetting -> run frames with NEUTRALISED pads until the restore lands (one frame)
Live      -> frame 0
```

**The neutralised frame is not a detail.** The restore happens at the top of the update stage and
that stage then simulates a whole frame — so the local pads would be the first thing two
just-made-identical boards disagreed about. `Step()` zeroes both pads and the coin/pause status bits
for exactly that frame.

### 5.3 Per-frame integration in `pre3::GameLoop`

Four call points, order fixed (`GetStatus()` is deliberately last frame's answer, so pad routing
sees one stable view for the whole frame — see `NetSession.h`):

1. `GetStatus()` at the top, before input is polled.
2. Pad routing: **online you always play as Player 1 locally**, whichever slot you own on the wire.
3. `Drive()`, then `Step()` — which overwrites *both* pads, so the local fill must come first.
4. `entries.update` / `render_begin` / `render_end` only when `Step()` says so, then `EndFrame()`.

A netplay stall skips **all three** stages, not just the update: the render stages read what the
update produced, so re-recording them against an unchanged board costs a full frame of GPU work to
draw the picture already on screen. The last output texture is re-presented instead.

Suppressed for the whole session (none of them are in the transmitted pad): the Escape pause menu,
the coin binding, TEST/SERVICE, and the arcade coin/start protocol. The last one differs from the
m2ftg path, which re-runs the protocol *after* `step()` on synchronised pads — here it simply does
not run, because a round starts from a savestate that is already past the credit screen.

### 5.4 The trap that was actually hit

`source/Main.cpp`'s pre3 branch **never called `net::Load()`**. Without it `net::IsAvailable()` stays
false, the whole session driver is inert and the Netplay page never appears — which reads exactly
like "the plugin will not load" when nothing ever asked it to. This is the same fault Virtual On
presented with; the warning was already written on the Kiwami 2 branch and still got missed.

---

## 6. The link interface at `params + 0x1070` (for SRC2 later)

Not needed for FV2, and deliberately still null there. Recorded because it is the module's own
multi-board mechanism and SRC2 will want it.

`module_start` stores it in `DAT_180527BE8`. Sixteen vtable slots are called on it:

| slot | byte | called from |
|---|---|---|
| 4 | `+0x20` | `FUN_1800341C0`, `FUN_180038BBD` |
| 5 | `+0x28` | `FUN_1800341C0` ×3, `FUN_180039D8C` |
| 6 | `+0x30` | `FUN_1800341C0` ×3, `FUN_180038BBD`, `FUN_18003976F`, `FUN_180039D8C` |
| 7 | `+0x38` | `FUN_180026B70`, `FUN_1800341C0`, `FUN_18003976F`, `FUN_180039D8C` |
| 11 | `+0x58` | `FUN_180036420`, `FUN_18003976F` |
| 17 | `+0x88` | `FUN_1800341C0` (r8=2), `FUN_18003976F` — board run-state change notification |
| 18 | `+0x90` | `FUN_1800341C0` |
| 19 | `+0x98` | 8 sites incl. `FUN_1800336D0` — returns a flag that vetoes the start/coin latch |
| 20–27 | `+0xA0`…`+0xD8` | `FUN_1800341C0`, `FUN_180039D8C` |
| 30 | `+0xF0` | `FUN_180034AA0`, `FUN_1800362E0` |

Its mere presence also changes board behaviour, which is why FV2 must not be handed one casually:

* the RTC goes deterministic (§3.5);
* `FUN_1800341F0` **zeroes `rom+0x364` = player 2's cached pad** — P2 is expected to arrive over the
  link, not from the local pad. (This is a *per-game task*; FV2's own task `FUN_180036BF0` does
  **not** do this, so the earlier claim that FV2 would lose its second player was wrong.)
* the NVRAM initialiser `FUN_180026840` **forces VS mode on**. Again per-game — that handler is not
  in FV2's hook table.

**Semantics of the individual slots are NOT reversed.** Establishing them is the first task for
SRC2 netplay, and `FUN_1800341C0` (which uses 12 of the 16) is the place to start.

---

## 7. What a single machine has verified

A headless `-fv2 -frames N` run reports one line, `LogBoardStateOnce()`, precisely because none of
this is observable by playing the game — a wrong offset produces a round that starts and then
quietly diverges on two machines, which is the most expensive way there is to find out:

```
[FV2 net] machine=00007FF8FC9795D0 phase=0x10 preloaded states=1 request=0x00
          canary=0x2CF07F1D clock pin=1 netplay=1
```

Every field in it is a claim being checked. `machine` resolved and `phase` reached 0x10 (the
`TaskM3E` static, §3.2); `preloaded states=1` is `vs_start.bin` loaded and the reset therefore
possible; a non-zero `canary` means the whole `mem → bank descriptor → RAM` chain resolved; the last
two are the clock redirect and the per-game verdict.

Separately measured: the reset lands in **one frame** and moves the board out of attract (§3.2), the
canary's register half moves **every frame** (§3.7), the plugin now loads on this path
(`netplay plugin loaded (ABI 7)` in `yampnet.log`), and a 900-frame run is unchanged against the
pre-netplay baseline with no access violations.

## 7a. What the first two-machine round established (2026-08-04)

Host `yamptest` / guest `yamptest2` over RPCN, room 65, seed `0xBF545799`. Both logs agree line for
line through the whole handshake:

* room, P2P link, barrier, **shared seed** — all fine;
* **`board restored after 1 frame(s)`** on both peers, matching the single-machine measurement;
* **the restore is bit-identical across machines.** The plugin logged no baseline-offset warning for
  that round, which means the two canaries were *equal* at the first compared frame. That answers
  what was the biggest open question here, and it answers it in the good direction.
* then `DESYNC at frame 4` — which turned out to be the read race in §3.8, not a divergence.

Also fixed as a consequence: the plugin compared the two peers' values by **baselining their
difference**, which is right for a counter (StF/FV submit the ROM's frame counter, where a constant
offset is harmless) and meaningless for a **hash**. `yampnet_match_config::state_check_exact` now
carries which rule applies. **Virtua Fighter 2 was already affected** — it has submitted a work-RAM
hash since ABI 3 and was being compared the counter way.

## 7b. Second round: sync held, and a pad bug fell out of it

With §3.8 fixed the round **stayed in sync**. What surfaced instead was input, and it was not a pre3
bug at all — the shared plugin's pad codec had the **vertical axis inverted**.

`m_y1` is POSITIVE when the player pushes DOWN. That is the sl convention throughout the host:
`source/input/Input.h` documents it on `PadState` ("y+ = down"), the XInput reader negates
`sThumbLY` to get there, and `csl_pad::set_state` writes `-1.0f` for UP. `plugin/yampnet/PadCodec.cpp`
assumed the opposite in both `EncodePad` and `DecodePad`.

The round trip therefore turned a vertical press into **nothing**, not into its opposite: pressing UP
set `BUTTON_UP` in `m_now` and, through the sign error, `BUTTON_L_DOWN` as well — so the receiver saw
up and down held together, cancelled them to `m_y1 = 0.0f`, and handed the board a pad with both
directions asserted. Horizontal was unaffected (x is negative-left on both sides), which is why it
survived being played at all.

**This has been wrong for every game on this plugin since the codec was written**, not just for the
Model 3 boards — Sonic the Fighters, Fighting Vipers and Virtua Fighter 2 netplay all round-trip
their pads through it, and up/down is jump/crouch in all three.

## 7c. Round start is now a room setting, and the clock pin moved

`vs_start.bin` is the only machine state the module SHIPS, which is why a round used to always
begin in VS. YAMP now also takes one of its own, through the same request bitfield: **bit 0 is
SAVE**, pushing the whole machine onto a second vector (`machine+0xF8`, count at `machine+0x104`),
and **bit 1 restores its newest entry without popping** - so one snapshot starts any number of
rounds.

**When it is taken is the whole design.** It is requested on the first host frame that sees the
machine RUNNING, which is before the frame step has executed a single guest frame: the board reaches
phase 0x10 at the END of one update stage and only begins stepping on the next. What gets captured
is therefore the pristine post-boot state, which two peers reach identically by construction. A
round from it plays forward through the boot, the attract demo and the credit screen - which is
where the AI runs, and so where a divergence shows itself.

Which of the two a round uses is `YAMPNET_ROOM_FLAG_PRE3_VS_START`, published by the host and
adopted by the guest, because the two peers must restore the *same* bytes. Clear (the default) is
the power-on snapshot; set is `vs_start.bin`. Local preference lives in `[Netplay] Pre3VsStart`.

### THE CLOCK PIN MOVED, and this is a correction

It was documented as "off until a round turns it on". That is wrong, and the snapshot is what
exposes it: **the board reads its clock while it BOOTS**, and the snapshot is captured at the end of
that boot - so an unpinned boot hands the two peers different snapshots, a divergence baked into the
state the round starts from. It is now pinned at install time, before `module_start`; a round still
re-pins it to the match seed at the barrier, which is a change of value at a moment both peers are
held at the same emulated frame.

Measured, two runs of the same build on one machine:

| | clock unpinned | clock pinned |
|---|---|---|
| boot state at the same point | **differs** | identical |

### The measurement that says the snapshot works

Two independently-launched processes, probed with the settled read of §3.8 and the clock pinned.
They start **one emulated frame apart** (the number of host frames a boot takes varies with async
file loading) and then the restore lands:

```
run 1  f=400 pending=1 canary=0xBE9AD8DC     run 2  f=400 pending=1 canary=0xAF3A8543   <- differ
run 1  f=401 pending=0 canary=0x5D7E64EC     run 2  f=401 pending=0 canary=0x5D7E64EC   <- restore
run 1  f=402 pending=0 canary=0x8EABCB8A     run 2  f=402 pending=0 canary=0x8EABCB8A
run 1  f=403 pending=0 canary=0x14FBACAD     run 2  f=403 pending=0 canary=0x14FBACAD
   ...identical for every frame measured...
```

Two boards with different histories converge at the restore and stay bit-identical. That is exactly
what a round needs, demonstrated without a second machine.

## 8. Open questions — the ones only two machines can answer

1. ~~**Is the savestate restore bit-identical across peers?**~~ **Answered yes** — see §7a, and
   confirmed end to end by an AI-vs-player round in which both screens ran the same fight.
2. **Do the `Speed` hooks produce bit-identical results to the interpreted ROM code?** They are
   native float routines standing in for PowerPC ones. Forcing the default mask (§3.4) makes this
   moot for netplay *as shipped* — but if they are not bit-identical, the mask is part of the
   determinism contract rather than a convenience, and that matters the moment anyone wants a room
   option for it.
3. **Does anything re-inject the arcade settings after the restore?** §3.3 argues not, from where
   hook 12 sits in guest code; it is an inference. Two peers with deliberately different difficulty
   dip switches would settle it in one round. Untested — the rounds so far ran with identical
   `settings.ini` on both machines (diffed: only the account name differed), so this has never been
   exercised. It is the most likely remaining source of a desync for two players who are not
   deliberately matched, and the canary is what would catch it.
4. **SRC2 has not been run at all**, let alone networked, and its analogue inputs go through a path
   FV2 never touches. `pre3::NetplaySupported()` excludes it deliberately.
5. **Frame pacing.** `-frames` runs are deterministic, but the host loop's FPS cap is wall-clock
   driven. Lockstep keys on the netplay frame index and never on host time; nothing here should
   depend on the cap, and the cap being off on one peer should be harmless.

---

## 8a. Rendering faults found alongside this work

Two module-hosting bugs surfaced while the netplay tooling was being used, both in shared code and
so both affecting every game on the LJ-generation DX12 path.

**icri vtable slot 80 was NULL** — Fighting Vipers 2 crashed with `rip=0` on one stage after minutes
of play. `UseGaidenVtable` built 80 entries because "the highest slot the Gaiden module actually
calls is 76"; that was measured on Sonic the Fighters and pre3 calls slot **80**
(`criAtomExAcb_GetCueInfoById`). A full sweep of all 19 functions touching the icri global put the
next-highest at 76, so exactly one method was missing. The vtable now self-checks for holes at
start-up: `[cri] ... (SetCueId at slot 9, 81 slots, 0 holes)`.

**Texture uploads executed AFTER the draws that sampled them.** `SubmitModuleFrameList` ran the
module's command lists in the order they were first touched that frame. The module records texture
uploads onto a draw-free list and its scene onto another, so whenever the scene list was touched
first, that frame's uploads landed too late and those draws read the previous contents.

Invisible while a texture is unchanged; visible exactly when textures are swapped — the stage
background flickering through a round transition, and hit effects (which live one or two frames, so
one frame late is most of their life) coming out black or a flat colour. The file already stated the
principle for buffer copies, whose shadow list "goes FIRST so one-time uploads land before the draws
that consume them"; the module's own upload lists now follow it. They cannot be replayed onto the
shadow list the same way because their copy sources are transient placed footprints rather than
persistent mapped buffers.

Confirmed by a counter rather than by eye: `[pathb] upload-before-draw reorder applied on N frames`
counts frames the old order got wrong. A session that reproduced the flicker before the fix logged
64+ such frames after it, and the flicker was gone.

Worth recording as method, because three earlier leads were wrong: sampler filtering (an artifact of
comparing whole descriptor heaps), an uninitialised descriptor (PIX does not emit a view for every
descriptor — Gaiden's capture has the same gaps), and the round-1 waterfall being absent at all
(Gaiden does that too; it is the game's own behaviour). Comparing two PIX C++ exports mechanically
ruled out geometry, PSOs, samplers, textures, mips and SRVs, but could not see the frame ORDERING
that turned out to be the fault.

## 9. Files

| file | role |
|---|---|
| `source/pre3/NetSession.{h,cpp}` | **the round driver** — barrier, clock pin, reset, lockstep |
| `source/pre3/Determinism.{h,cpp}` | the clock pin, the machine object, the reset, the canary |
| `source/pre3/BoardVtables.{h,cpp}` | locate/redirect the board's vtables (shared) |
| `source/pre3/SystemSwitches.{h,cpp}` | TEST/SERVICE; suppressed during a session |
| `source/pre3/HleHooks.{h,cpp}` | the 36-hook table; held at the default mask during a session |
| `source/pre3/Gaiden/Pre3Host.cpp` | `GameLoop` — the four call points, pads, input suppression |
| `source/Main.cpp` | the pre3 branch's `net::Load()` (§5.4) |
| `source/net/YampNet.h` | the plugin ABI — now 7, see §5.1 |
| `source/net/NetPlugin.cpp` | host-side layout fill + the cross-family `static_assert`s |
| `plugin/yampnet/Plugin.cpp` | plugin-side `LayoutMatches` |
