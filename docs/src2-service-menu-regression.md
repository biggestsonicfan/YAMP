# Sega Racing Classic 2 — the TEST switch soft-reset regression

**RESOLVED 2026-08-08.** Root cause: `DefaultDisableMask` bit 3, introduced by `cee1bb0`. Fix:
hook 3 moved into `SRC2_BOOT_CRITICAL` (`{1, 2, 3, 5}`) and removed from the default disable mask
(`0x3F6008` → `0x3F6000`) in `source/pre3/HleHooks.cpp`. Verified on a build of `b53294b` + fix:
TEST opens the game's TEST MENU directly, SERVICE/TEST navigate it, and the boot network check
still runs — nothing else about the mask (scalars, boot presentation, netplay oracle) changed.

## What the symptom actually was

Pressing TEST looked like a soft reset. It was one: with hook 3 disabled, the per-frame
`bla 0x55D800` in the game's main loop (guest 0x09A3FC) is restored, but hooks 1 and 2 — which are
boot-critical and stay enabled — excise the *loader* of the overlay it calls, so 0x55D800 is all
zeros. Attract and races never take the branch; TEST-menu entry does. The guest crash-reboots into
the MODEL3 SYSTEM PROGRAM status screen ("PRESS START OR TEST BUTTON TO CONTINUE" — the string that
named this screen; it is the BIOS's, which is why it is not in the game-ROM Ghidra db). Pressing
TEST there continues into a boot that hangs in the IRQ-ack spin at guest 0x1E78 — the same
reused-device-table failure hook 1's diagnosis in `HleHooks.cpp` describes — with the CPU register
file and RAM checksums bit-frozen while the module's frame pump keeps running.

## Why the first hunt (2026-08-08, earlier session) missed it

Documented because each is a reusable trap:

* **The kill-switch matrix was never wired.** `src2_menu_test.cmd` set `YAMP_SRC2_NO*` environment
  variables that no code reads, so every "eliminated via env kill-switch" row in the earlier
  version of this document was void. Only `YAMP_PRE3_TILEMAP` and `YAMP_PRE3_BOOTRENDER` exist —
  both re-verified innocent at HEAD.
* **"All SRC2 HLE hooks disabled" cannot exonerate the mask.** `FFFF…` and the default mask both
  have bit 3 set, so that experiment reproduced the bug's precondition instead of removing it. The
  discriminating run was mask `0` (everything enabled), which fixed TEST on the first try.
* **The classifier judged the wrong symptom.** "The reset screen still appears" was used to mark
  commits BAD, but the informative signal was the *hang after continuing* — measurable as the guest
  master frame counter (0x737978) pinned at 0/1 with a frozen CPU checksum, or as the tilemap name
  tables (bare ASCII at shadow 0xF8000+, swizzle `offset ^ 2`) reading "TEST MENU" vs the SYSTEM
  PROGRAM banner. With that classifier the bisect converged in five builds:
  455eb62 GOOD → 62c6251 GOOD → 5df70cb GOOD → d1d979e GOOD → cee1bb0 BAD.
* **The per-frame sweep's "fine" verdicts only cover attract.** Hook 3 measured 658 draws / no
  effect over 400 frames because nothing on the attract path calls the overlay. A verdict for a
  hook that guards an operator path needs the operator path exercised.

## The instrument that closed it

A ~30-line temporary probe in `Pre3Host.cpp` (see git history of this doc's session, not shipped):
arms when `ModuleDrawsLastFrameNow() > 100`, holds TEST for 12 frames, presses again at +150, and
logs — per 16 frames — draws, `execute_info.status`, guest 0x737978, `StateCheckParts` CPU/RAM
checksums, machine phase/request words, and on snapshot frames dumps all four tilemap name tables
as ASCII plus the PPC register file (cpu object at DLL+0x62AA90, regs at +8; the hang showed
PC 0x1EBC/0x1EC0). Reading the screen text from the shadow is what let a headless `-frames` run be
classified without a human watching the window.
