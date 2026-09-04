# Yakuza Arcade Machines Player

YAMP runs the arcade games embedded in RGG Studio's Yakuza / Judgment titles as standalone,
native PC games — no Yakuza save file, no in-game arcade cabinet, no emulator front-end. It
loads the game's own module DLL, stands in for the parts of the Yakuza engine that module
expects to be talking to, and hands it a window, a swap chain, a sound device and a controller.

This is a fork of [Silent's original YAMP](https://github.com/CookiePLMonster/YAMP), which
established the technique and shipped Virtua Fighter 5: Final Showdown from Yakuza 6. This fork
carries it to **thirteen game/title combinations across five parent games and three arcade
boards**, and adds audio, netplay, a rebindable input layer and a launcher.

**No game files are redistributed. You must own the parent game.**

## Supported games

| Game | Board | Comes from | Switch |
| --- | --- | --- | --- |
| Sonic the Fighters | Model 2 | Lost Judgment | `-stf` |
| Sonic the Fighters | Model 2 | Like a Dragon Gaiden | `-stf-gaiden` |
| Fighting Vipers | Model 2 | Lost Judgment | `-fv` |
| Motor Raid | Model 2 | Lost Judgment | `-mr` |
| Motor Raid | Model 2 | Like a Dragon Gaiden | `-mr-gaiden` |
| Virtua Fighter 2 | Model 2 | Yakuza: Like a Dragon | `-vf2` |
| Virtua Fighter 2 | Model 2 | Yakuza Kiwami 2 (GOG) | `-vf2-k2` |
| Cyber Troopers Virtual-On | Model 2 | Yakuza Kiwami 2 (GOG) | `-von-k2` |
| Fighting Vipers 2 | Model 3 | Like a Dragon Gaiden | `-fv2` |
| Sega Racing Classic 2 | Model 3 | Like a Dragon Gaiden | `-src2` |
| Virtua Fighter 5: Final Showdown | native | Yakuza 6: The Song of Life | `-vf5fs` |
| Virtua Fighter 5: Final Showdown | native | Lost Judgment | `-vf5fs-lj` |
| Virtua Fighter 5: Final Showdown | native | Yakuza: Like a Dragon | `-vf5fs-ylad` |

A game that ships in two titles gets two rows on purpose. The builds differ, they verify against
different checksums, and each row checks that *its* parent game is the one installed.

## Requirements

* Windows 10/11, x64
* A GPU with Direct3D 12
* A legally owned copy of at least one parent game above, installed. Steam for all of them
  except the Kiwami 2 modules, which come from the GOG build.

## Getting started

### Getting a build

You do not have to build YAMP yourself. Every push to `master` is compiled by GitHub Actions and
the resulting binaries are attached to the run:

1. Open the [**Actions**](https://github.com/biggestsonicfan/YAMP/actions) tab.
2. Click the newest **build** run with a green tick. A red cross means that commit did not
   compile — take the newest green run below it instead.
3. Scroll to **Artifacts** at the bottom of the run summary and download **`YAMP-Release`**.

`YAMP-Release` is the one to play with. `YAMP-Debug` is the same commit built unoptimized with
debug logging compiled in — bigger and slower, but the one to grab when something misbehaves and
you want a log to attach to an issue.

GitHub hands you a `.zip` containing `YAMP.exe` and `YAMP.pdb`. Only the `.exe` is needed to run;
the `.pdb` carries the symbols that turn a crash address into a function name and a line number,
so keep it beside the `.exe` if you might report a crash.

Two things that routinely catch people out, neither of which is a setting in this repository:

* **You must be signed in to GitHub to download an artifact.** The links do nothing for logged-out
  visitors, even though this repository is public.
* **Artifacts are deleted after 90 days.** Past that the run is still listed but its downloads are
  gone; use a newer run, or build from source.

These builds are not code-signed, so Windows may show a SmartScreen warning the first time you run
one. That is expected for an unsigned executable downloaded from the internet.

### Running it

Put `YAMP.exe` anywhere and run it with no arguments. It opens a launcher that goes looking for
what you own: the folder it is sitting in, the working directory, **every Steam library** (read
out of `libraryfolders.vdf`), **every GOG install** from the registry, and every folder sitting
beside `YAMP.exe`. Matching is by module layout rather than folder name, so a renamed or moved
install is still found.

Each row shows where the game was found and whether it verified. Pick one and press **Play** —
YAMP relaunches itself with that game's switch and the working directory set to the game's own
folder, which is what the module needs in order to find its ROM and sound assets.

You can skip the launcher entirely by passing a switch from the table above.

## Features

### Rendering

The Model 2 and Model 3 games run through a D3D12 host; the Yakuza 6 generation runs the DX11
path its module was built for. This is not upscaled emulator output — the module's own renderer
draws, through a host device standing in for the Yakuza engine's.

* **Render resolution** driven through the module's own internal mode table, from Model 2's
  native 496×384 upward. The Model 3 host, which has no such table, offers 1×–6× of native.
* **Match window to render resolution** for a pixel-exact 1:1 window.
* **Aspect ratio** — 4:3, 16:9, or fill.
* **CRT filter** — a port of Lost Judgment's own CRT shader, the one its in-game cabinets use,
  rather than a generic scanline overlay. Available to the Model 3 games too.
* Display resolution, refresh rate, fullscreen, and a 60 FPS cap.

### Audio

A clean-room implementation of the CRI Atom interface the modules call into, roughly 3,000 lines
under `source/criware/`: **HCA** and **ADX** decoders, **@UTF / ACB / AFS2 / AWB** parsing, cue
playback by name (Model 2) and by numeric ID (Model 3), streamed BGM located by matching the
loaded ACB against the game's `rom/sound/*.acb`, and **XAudio2** output with per-voice volume,
pitch, pause and looping.

Large BGM cues start gapless — the first half-second plays while the remainder decodes — so
fight music starts on the beat instead of after a hitch.

### Input

Four backends: **keyboard**, **XInput**, **DirectInput** (arcade encoders, fight sticks, pads
XInput cannot see) and **Bliss-Box** (4-Play / Gamer-Pro / GPA / BlisSTer), the last of which can
identify what is plugged into each of its ports.

* **Per-player bindings**, two players, each with an independent keyboard map *and* pad map *and*
  a chosen device. Devices are stored by stable ID rather than list index, so bindings survive a
  replug, and a configured-but-absent pad keeps its row instead of being silently reassigned.
* **Binding wizard** — "Program All Inputs" walks Up/Down/Left/Right, Punch, Kick, Guard, Start,
  Coin. Or click any single cell to capture one input.
* 17 bindable actions, including the combo buttons (P+G, P+K, K+G, P+K+G) and Back.
* **Keyboard defaults follow MAME** for player 1 — arrows, Z/X/C, `1` to start, `5` to coin.
  Player 2 has no keyboard defaults by design; run the wizard.
* **Driving axes** for the Model 3 racer, with a ramped keyboard steering curve so a key
  approximates a wheel instead of pinning full lock.

**A real Sega Saturn Twin Stick drives Virtual On** through a Bliss-Box, auto-detected and mapped
to the cabinet's twelve inputs. It is an override rather than a fifth mapping — it replaces the
pad fill entirely and pins the module to its own twin-stick control entry. Without one, Virtual
On's five native pad-to-lever schemes are all selectable (two of them are partial in the module's
own table, and are labelled as such).

### Cabinet controls

**TEST** and **SERVICE** are wired to the emulated board's system input port — the Model 2 I/O
board, the Model 3 JAMMA registers — so they open the board's *own* service menus and dip switch
screens, not a YAMP imitation. They default to F2/F3 on player 1's keyboard and are deliberately
left unbound on pads, where a stray press would drop the board into a menu mid-match.

Cabinet settings the board reads — difficulty, region, free play, versus mode, damage, and Sega
Racing Classic 2's full GAME ASSIGNMENTS block — are exposed in the settings UI and written where
the board expects to find them.

**F1** opens settings; **Escape** pauses through the module's own pause path.

### Netplay — experimental

Netplay ships as an **optional** `yampnet.dll`, built from its own repository,
[YAMPnet](https://github.com/biggestsonicfan/YAMPnet). If it is absent the entire feature
disappears cleanly and everything else works.

To add it, download **`yampnet-Release`** from that repository's
[Actions](https://github.com/biggestsonicfan/YAMPnet/actions) tab — the same procedure as
[Getting a build](#getting-a-build) — and drop `yampnet.dll` beside `YAMP.exe`. Restart YAMP; the
Netplay settings page reports whether it loaded.

**Pair it with a YAMP of about the same age.** The plugin writes the emulator's pad structures
itself, so it declares the struct layouts it was compiled against and YAMP refuses one whose
layouts or ABI version disagree. The refusal is deliberate — the alternative is silent memory
corruption — and it is visible rather than mysterious: the Netplay page says *"The plugin was found
but rejected"* and names the reason. If you hit it, take a newer `yampnet.dll`, or a YAMP build
from nearer the plugin's date.

Matchmaking runs over **RPCN**, the community server for PlayStation Network emulation: a TLS
session for login and the room list, a UDP address-discovery exchange, and then **direct
peer-to-peer game traffic that never passes through the server**. There is a lobby with a room
browser, per-game room settings owned by the host, passworded rooms, and join-by-ID.

Two different netcodes, chosen per game by what the board actually needs:

* **Delay-based lockstep** for the fighting games, modelled on the Sonic the Fighters PS3 port:
  a start barrier, a shared match seed across all of the board's RNG streams, an adjustable frame
  delay, and a per-frame desync canary that reports the exact frame and both hashes.
* **The arcade's own linked-cabinet protocol**, tunnelled, for the games designed as two machines
  with a comm board between them. No lockstep and no shared seed — the boards talk to each other
  the way they did in the arcade.

| Game | Netplay |
| --- | --- |
| Sonic the Fighters (both titles) | lockstep |
| Fighting Vipers | lockstep |
| Virtua Fighter 2 (Yakuza: Like a Dragon) | lockstep |
| Fighting Vipers 2 | lockstep |
| Sega Racing Classic 2 | linked cabinet |
| Motor Raid (Lost Judgment) | linked cabinet |
| Cyber Troopers Virtual-On | linked cabinet |
| Virtua Fighter 2 (Kiwami 2), Motor Raid (Gaiden) | none |

Virtual On took the longest road. Its lockstep netplay was **abandoned and removed** after four
failed two-machine attempts, and replaced by a linked-cabinet implementation that has been run
across two machines — the ROM's own link check is what starts play, so there is no barrier to
open and no shared seed to agree on.

### Module verification

Before a module DLL is loaded, YAMP takes its **SHA-256** and matches it against a table of
builds it knows how to patch. This is a hard gate that runs *before* `LoadLibrary`, so an
unrecognised build never gets to execute. Every host resolves symbols by byte pattern and by
hardcoded offsets, and a wrong build does not fail cleanly — it silently mis-patches, which is a
far worse outcome than being told to update.

The parent game is checked separately and more cheaply, by PE header identity rather than a hash:
hashing a several-hundred-megabyte executable would cost seconds at every boot for no extra
certainty. A missing parent game blocks; an unrecognised *build* of a parent game that is present
is only a warning, because the module is what compatibility actually depends on.

### Research and debugging tools

Behind an acknowledgement on the Debug page, and locked out entirely during netplay:

* **Draw limit** and **skip draw** sliders that drop module draws live, by index, matching a PIX
  capture of the same frame — for bisecting a rendering artifact.
* **HLE hook tables** for both emulator families, with per-hook enable/disable and live hit
  counts — dozens of hooks per game, 120 of them for Virtual On. Boot-critical hooks are stripped
  on the way to and from the ini so the panel cannot brick a game, and hooks marked Core apply
  live but are never saved, so a restart always boots.
* **Sonic the Fighters' own developer windows** — DEBUG MENU, CONFIG, PERFORMANCE, 960STAT —
  reconstructed from descriptor data still present inside the retail DLL.
* **Loose ROM loading**, which hides the packed `rom/*.par` so the engine's own archive-miss
  fallback opens loose images instead. That is what makes homebrew Model 2B program ROMs
  hostable, together with an ini-only `[HleRetarget]` section that relocates hooks by address or
  by ELF symbol.

`docs/` carries the reverse-engineering write-ups behind all of this: the decoded comm packets for
Virtual On and Motor Raid, the per-game HLE hook tables, the Model 3 netplay design, the Model 2
ROM symbol tables, and the recon on `d1a.exe` — Sega Racing Classic — which turns out to be the
direct ancestor of every Model 2 module here.

## Settings

Everything lives in one `settings.ini` next to `YAMP.exe`, in sections
`[General] [Graphics] [Debug] [Audio] [StF] [VF2] [VirtualOn] [VF5FS] [Netplay] [HleRetarget]`.
Most of it is reachable from the F1 settings UI; a few deliberately hand-authored escape hatches
are ini-only and are documented in the comments beside them.

Settings that need a restart — render mode, netplay mode, cabinet role, loose ROM loading — say
so in the UI rather than pretending to apply.

## Known issues and limitations

* **Netplay is experimental.** It needs an RPCN account, and that account's password is stored in
  plain text in `settings.ini`. Two players / two machines only.
* **Virtua Fighter 2 from Kiwami 2 and Motor Raid from Like a Dragon Gaiden have no netplay.** The
  Gaiden Motor Raid build is recognised and explicitly refused by the link layer rather than being
  allowed to half-work.
* **No video playback.** CriMana is stubbed out entirely, as are DSP buses and Aisac. Encrypted
  HCA streams are unsupported (nothing shipped uses them).
* HCA encoder delay is not trimmed, so loop points are accurate to within about 45 ms.
* During a netplay session the Debug page, the module's debug windows, TEST/SERVICE and the pause
  menu are all disabled.
* **Modules are hash-pinned.** A game update that changes an arcade DLL will block YAMP until YAMP
  learns the new build's checksum.
* Sega Racing Classic 2 has an **open regression**: TEST soft-resets the board instead of opening
  the service menu. What has already been ruled out is written up in
  `docs/src2-service-menu-regression.md`.
* Windows x64 only, MSVC only.

## Building

Only if you want to — [Getting a build](#getting-a-build) has prebuilt binaries for every commit.
CI builds the `Debug` and `Release` configurations; `Master` is source-only.

```
premake5.exe vs2022
msbuild build/YAMP.sln -p:Configuration="Debug Win64" -p:Platform=x64 -m
```

C++17, Visual Studio 2022, `Debug` / `Release` / `Master` configurations on the `Win64` platform.
Output is `YAMP.exe`.

The netplay plugin is **not** built here — it lives in
[YAMPnet](https://github.com/biggestsonicfan/YAMPnet) and builds against this checkout's headers.
YAMP does not link against it, so a build with no `yampnet.dll` beside the `.exe` is simply a
build without netplay. Drop the DLL next to `YAMP.exe` to turn the feature on.

`premake5.lua` is the source of truth for the project files. Do not hand-edit anything under
`build/`; edit the Lua and regenerate.

One submodule, `source/Utils` ([Silent's
ModUtils](https://github.com/CookiePLMonster/ModUtils)) — `git clone --recursive`, or
`git submodule update --init` after the fact. ImGui, WIL and D3D12MemoryAllocator are vendored
in-tree. There is no package manager step and nothing to install beyond the Windows SDK.

## On the use of AI in this project

This section exists because the honest answer is "a great deal of it", and that should be stated
plainly rather than discovered.

**The split is visible in the git history**, which is why it is worth quoting exactly. Of the 101
commits on this fork since the upstream base at `45c0d9ae`, **90 carry a `Co-Authored-By: Claude`
trailer** and 11 do not. The 11 that do not are the earliest, from October and November 2025: the
move from DX11 to D3D12, the namespace and folder restructure, `handle_initialize`, and the first
attempt at Sonic the Fighters. That work is hand-written. Everything from 2026-07-27 onward — which
is to say Sonic the Fighters actually running, CRIWARE audio, Fighting Vipers, Motor Raid, Virtua
Fighter 2, Virtual On, the Model 3 host and its two games, netplay, the input layer, the launcher,
the settings UI, the verification gate, and every file in `docs/` — was written in
[Claude Code](https://claude.com/claude-code) sessions, mostly Claude Opus 5 and some Fable 5.

**What the AI actually did.** It read decompiler output and reasoned about it; it wrote most of the
code in this repository; it wrote the documentation and the commit messages, including this README.
On a good day it could take a module function from "unnamed blob in Ghidra" to a working host
implementation in a single sitting.

**What it did not do, and could not have.** It never saw a frame or heard a note. Every claim here
that something *works* — that a game is playable, that the audio is right, that a Twin Stick drives
the cabinet, that two cabinets link across two machines — rests on a human running it on real
hardware against real, legally purchased installs, and saying so. The AI can run the binary under a
debugger and read a log; it cannot tell you whether the game feels right. The hardware findings in
particular were only possible because someone owns a Bliss-Box, a Saturn Twin Stick and a second
test machine.

**It was wrong, repeatedly, and the history says so.** The log contains commits titled `correct the
VBlank finding - pre3 does raise it, my scan looked at CLEAR`, `a coherent frame-boundary probe,
and it retracts the VBlank theory`, and `the network check PASSES on a hung board - correcting the
last commit`. Those are the AI's own retractions of the AI's own confident conclusions from a day
or two earlier, caught by testing. Several write-ups in `docs/` are largely negative results for
the same reason. Treat anything in here that has not been run on real hardware accordingly.

**The direction was not the AI's.** Which game to attack next, what counted as done, what to throw
away, and the standing rule this project is built on — *find and port the game's own
implementation, never invent a YAMP-side approximation of it* — are human calls. The AI is good at
following that rule and has no instinct for setting it.

**Nothing generated was passed off as original game data.** All reverse engineering was done
against legally owned installs, and no game asset, module or executable is redistributed here.

## Disclaimer

**YAMP does not redistribute ANY copyrighted files.** You must own an original copy of the parent
game to play its arcade games through YAMP. Pirated copies will not receive support.

All rights to the games listed above belong to SEGA and RGG Studio.

## Credits

* **[Silent (Adrian Zdanowicz)](https://github.com/CookiePLMonster)** — the original YAMP, 87
  commits from 2021 to 2024, and the technique this entire fork is built on.
* [ModUtils](https://github.com/CookiePLMonster/ModUtils), [Dear ImGui](https://github.com/ocornut/imgui),
  [WIL](https://github.com/microsoft/wil),
  [D3D12MemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator).
* [RPCN](https://github.com/RipleyTom/rpcn) — the matchmaking server protocol netplay speaks.

Licensed under the MIT License. See [LICENSE](LICENSE).

---

Screenshots below are from the original YAMP, showing Virtua Fighter 5: Final Showdown.

[![Preview](https://i.imgur.com/wN49APOl.jpg)](https://i.imgur.com/wN49APO.jpg "Preview")
[![Preview](https://i.imgur.com/gim5Q58l.jpg)](https://i.imgur.com/gim5Q58.jpg "Preview")
[![Preview](https://i.imgur.com/aJH3SALl.jpg)](https://i.imgur.com/aJH3SAL.jpg "Preview")
[![Preview](https://i.imgur.com/hqdC9ikl.jpg)](https://i.imgur.com/hqdC9ik.jpg "Preview")
