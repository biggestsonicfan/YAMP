# Sega Racing Classic 2 — the TEST switch soft-resets instead of opening the service menu

**OPEN. Not fixed. Handed off 2026-08-08.**

Pressing TEST on SRC2 shows a screen identical to a soft reset instead of the board's service
menu. This is a REGRESSION: it works in a Release build from 2026-08-06 and fails at `b53294b`.

The value of this document is mostly **negative results**. A long session eliminated most of the
obvious surface; repeating any of it is wasted time.

## The two reference points

| | build | behaviour |
|---|---|---|
| GOOD | `build/bin/Win64/Release/YAMP-known-good-0806.exe` (built 2026-08-06 12:37, so a commit at or
before `7415d0d`, 08-04) | TEST opens the service menu |
| BAD | `b53294b`, both Debug and Release | TEST shows the reset screen |

The known-good binary was recovered from `YAMP - Cryo.zip` in that folder after being overwritten.
**Do not rebuild over it** — keep it, it is the only known-good artifact.

27 commits separate `7415d0d` from `b53294b`.

## What the board actually does (measured)

TEST *is* reaching the board — it is not an input or gating problem:

* `switches: TEST=1 SERVICE=0 (paused=0 netplayLocked=0)` — the switch arrives ungated
* the board responds: the tilemap control register gains a layer, `0x1F00 -> 0x3F00`
* the guest's master frame counter at `0x737978` **freezes, and in a manual run went to 0**
* `execute_info.status` loses bit 6 (the start-screen bit): `0x40 -> 0x0`
* the 3D draw count collapses `~180 -> 8` and stays there
* the scroll buffer keeps uploading every frame, and TILEMAP writes continue

So the board leaves attract, something new is enabled, and then it sits in a state drawing almost
nothing. Whether that state is "the menu, unrendered" or "the boot sequence, restarted" is **not
settled** — the user reads the screen as a reset.

## ELIMINATED — do not re-test these

Each was disabled at `b53294b` and the reset still happened.

| ruled out | how |
|---|---|
| `CommBoard::DriveRoomRole` / `RestoreResetSnapshot` (this session's soft-reset work) | a log inside `RestoreResetSnapshot` itself fired **0 times**; also broken at `92ef242` and `5df70cb`, which predate it |
| `ArcadeSettings` writes (working copy + NVRAM) | env kill-switch |
| `SecurityBoard` install | env kill-switch |
| `execute_info.src2_scalars` (the 1.0f motion/bonus write) | env kill-switch |
| `InstallBootRender` | env kill-switch AND `YAMP_PRE3_BOOTRENDER=0`; byte-identical behaviour either way |
| ADC serving on port `0x3C` | env kill-switch (pass-through to the module's own accessor) |
| **all five of the above at once** | still resets |
| **every SRC2 HLE hook** | `DisabledHleHooksLo/Hi.SRC2 = FFFFFFFFFFFFFFFF` in `settings.ini` — still resets |
| the SYSTEM-port bit being wrong | forcing each bit in turn; `0x04` and `0x40` both reset on screen |

### The bit map does NOT transfer from FV2, which is worth knowing anyway

`SystemSwitches.cpp` documents `BIT_TEST = 0x04` as measured on a headless `-fv2` run, keyed on
`execute_info`'s game-mode pair moving to `14/00` / `15/01`. **On SRC2 the mode pair never leaves
`00/00` for any bit**, so that readout does not transfer. Forcing each bit on SRC2 gives three
groups by draw count:

```
0x01 0x02 0x08 0x20   attract continues, draws climb 179 -> 237
0x04 0x80             draws collapse to 8 and stay
0x10 0x40             collapse to 8, then recover to ~100-118 and hold
```

The `0x10`/`0x40` group looked like a live screen and is **not** — the user confirmed `0x40` is
also a reset on screen. The bit assignment is unchanged between the good and bad builds, so a wrong
bit cannot be *this* regression, but the FV2-derived mapping is unverified on SRC2 regardless.

## NOT yet eliminated — start here

Everything above is reachable by a runtime switch. What is not:

1. **The `pre3_config_t` block passed to `module_start`.** Read before any hook exists, so no
   kill-switch can reach it. The SRC2-specific difficulty fold is NEW in the range:
   `SRC2_TO_BOARD[4] = { 0, 2, 3, 4 }` (`Pre3Host.cpp`), which turns settings Difficulty=1 into
   board byte 2 where it used to pass 1. Also new: `link_node_id` / `link_peer_count`.
   The module's own injector folds these into the board's NVRAM, and the service menu is what edits
   that NVRAM.
2. **`Determinism`'s clock redirect**, installed on vtable slot 17 for *every* game (pinned only for
   FV2). Inert in principle - it tail-calls the original - but it is a vtable patch.
3. **`BoardVtables` / `ImportSymbols`** pattern-based resolution, changed by `2df9408`.
4. **The rest of `Patch.cpp`** (+615 lines in the range) beyond `InstallBootRender`.
5. **Anything outside `source/pre3`.** The kill-switches only covered pre3.

## Tooling notes that cost hours

* **`DebugLogFile` compiles away outside Debug** (`DebugLog.h` gates on `DEBUG || _DEBUG`). Release
  runs therefore produce **no `d3d12_debug.log` at all**. The known-good reference is a RELEASE
  build, so any probe used to compare against it must be raw `fopen`/`fprintf`, not `DebugLogFile`.
  Four rebuilds were spent bisecting Debug before this was noticed.
* **`git checkout <sha> -- source` is not enough** to build an old commit: it restores the files
  that existed then but leaves every file added since, so a newer `.cpp` compiles against an older
  header. A clean bisect must also delete anything under `source/` not in that commit (leaving
  `source/Utils`, a submodule, alone) and then re-run `premake5.exe vs2022`, because the generated
  vcxproj lists HEAD's file set.
* **A fixed-frame automated TEST press does not work across commits** - older builds have not
  started drawing by then, so the "before" sample is already 0 draws. Trigger on the board actually
  drawing (`draws > 100`) and press N frames later.

A working Release bisect harness with the adaptive trigger got as far as classifying `b53294b`
correctly (`before=404 after=8 -> BAD`) but `7415d0d` never reaches 100 draws, so the oldest end of
the range needs a different criterion before the bisect can close.

## Suggested next step

Bisect the 27 commits in Release with the adaptive-trigger harness, from the NEWEST end where
`draws > 100` is reachable, and find the first BAD. If the range bottoms out in commits that never
draw the attract, fall back to the config block in (1) above - force the difficulty byte back to the
pre-fold value and re-test, which is a two-line change and the single most suspicious item left.
