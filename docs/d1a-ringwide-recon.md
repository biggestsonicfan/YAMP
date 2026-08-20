# `d1a.exe` - the Ringwide ancestor of the m2ftg modules

Recon of **Sega Racing Classic** (Daytona USA on Sega Ringwide, 2009), the arcade application our
Yakuza-embedded Model 2 modules descend from. Ghidra has it on port 5678; everything below is
read-only analysis, nothing was written to that database.

## What the binary is

| | |
|---|---|
| machine | **x86-32** (`0x014C`), MSVC 9.0, GUI subsystem |
| built | `TimeDateStamp 0x4B0F667E` = **2009-11-27** |
| layout | ImageBase `0x400000`, **relocs stripped**, no ASLR/DEP, SizeOfImage `0x108E000` |
| sections | `.text` 0x2DE8FB, `PSFD00` 0x1E6A, `.rdata` 0x131704, `.data` **0xC77D78** (13 MB, almost all BSS), `.rsrc` |
| imports | 10 stock DLLs only: `d3d9`, `d3dx9_40`, `DINPUT8`, `WINMM`, `WS2_32`, `SETUPAPI`, `USER32`, `ole32`, `ADVAPI32`, `KERNEL32` |
| exports | **one**: `entry` |

`PSFD00` is an 8 KB code segment reached through a pointer pair at `0x7C600C`/`0x7C6014`; the code
is ordinary byte-stream decoding, no packer signature. Purpose not yet identified.

The arg parser is `FUN_00482080`. Note that **`-model2` and `-model2x2` are display resolution
modes**, not emulator selectors (they set the resolution enum to 0xE / 0xF). The full option set:
`--help -debug -t -m -s -sm -ss -ss4x -ve -fs -aa` and the resolution switches.
`-debug` is documented in the binary as "Debug Mode(use gdb)".

## It really is the parent

Two independent proofs.

**Shared RTTI.** d1a.exe and `m2ftg/stf-pxd-w64-d3d12_retail.dll` carry byte-identical mangled
class names - one source tree, sixteen years apart:

```
.?AVObjectM2DrawingManager@m2@obj@@     .?AVPassObjectM2@render_pass@@
.?AVObjectM2Painter@m2@obj@@            .?AVPassSpriteBack@render_pass@@
.?AVEntity@aet@a2d@@                    .?AVPassSpriteFront@render_pass@@
.?AVEntity@Sprite@spr@@                 .?AVPassClear@render_pass@@
```

d1a additionally has the `dw::` debug-window toolkit (~60 classes: `Display`, `Shell`, `Menu`,
`Graph`, `ListBox`, `ScrollBar`, `ColorAdjust`, plus `DwConsole`/`DwTexture`/`DwSound`/`DwTask`)
and the `test_mode::` menu framework (`MenuRoot`, `MenuTable`, `MenuItemUserFunc`,
`Exec_game_mode`, ...) that `source/m2ftg/Debug/DwGame.h` descends from, and `sys_am::backup::`
(`MBEeprom`, `MBSram`).

**Identical ROM container.** `FUN_00517110` allocates the guest ROM buffers and `FUN_005172B0`
fills them - same names, same sizes, same file layout as `m2ftg/rom/stf_rom/`:

| buffer | size | file |
|---|---|---|
| `ep_rom1` / `ep_rom2` | 0x100000 each | `rom/daytona_rom/rom_ep1.bin`, `rom/fv_rom/rom_ep2.bin` |
| `data_rom` | 0x1000000 | `rom/daytona_rom/rom_data.bin` |
| `pol_rom` | 0x1000000 | `rom/daytona_rom/rom_pol.bin` |
| `tex_rom` | 0x800000 | `rom/daytona_rom/rom_tex.bin` |
| `cpro_rom` | 0x200000 | `rom/daytona_rom/rom_copro.bin` |
| program ROM | static `.data` at `0xBA39E0`, `0xCA39E0` | `rom_code1.bin`, `rom_code2.bin` |

The stray `rom/fv_rom/rom_ep2.bin` path is in the shipped Sega Racing Classic binary.

Init chain: `FUN_004E25A0` (state machine) -> `FUN_004E8DD0` -> `FUN_00517110` (allocate) +
`FUN_005171D0` (8 async file loads) -> `FUN_005172B0` (size-check against the table at `0x74FB34`,
then memcpy into the buffers). The emulator core lives roughly in `0x4E0000-0x540000`.

## What it contains that our modules do not

- A full **i960 disassembler**: 185 `{opcode, handler, mnemonic}` records at `0x737D40`, four
  format printers. Table decoded in [d1a-daytona-symbols.md](d1a-daytona-symbols.md).
- The **Daytona ROM symbol table**, 651 records at `0x7368E0` - also in
  [d1a-daytona-symbols.md](d1a-daytona-symbols.md). (The x64 modules ship the same table for their
  own ROMs; see [m2-rom-symbol-tables.md](m2-rom-symbol-tables.md).)
- A 1595-entry object-name table at `0x74A154` (`o_encar3_00_a_mix`, ...).

## Why YAMP cannot host it

Three blockers, any one of which is decisive.

1. **32-bit vs 64-bit.** YAMP is x64. An x64 process cannot load or call into an x86 image, so the
   `LoadModuleDll` + drive-the-module pattern in `source/ModuleLoad.h` is unavailable.
2. **No module surface.** d1a exports only `entry`. There is no `module_start`, no `execute_info`,
   no game descriptor - that protocol was invented later, for the Yakuza embeds. Everything is
   internal state reached from WinMain.
3. **It brings its own platform.** D3D9 + `rom/shader/m2.fxo`, CRI Atom (ADX/CSB via
   `CriSmpSoundOutputXAudio2`), DirectInput8, and a statically linked Sega AM library. Our
   pxd/gs/D3D12 layer has nothing to plug into: wrapping would mean *replacing* d1a's platform
   layer, not hosting its emulator.

Plus the Ringwide environment gates it expects: keychip (`amDongleBillingGetKeychipId`, the
"Keychip Not Found" screen), the `amEeprom` SETUPAPI device, `amNetwork*` / P-ras, and the
`d1a.ini` hardware checks (`graphic-device = 1002:94cb`, `sound-device = 10ec:0883`, mem/sram/patch
sizes, `resolution = unique | 1280 x 720`).

**The only workable wrapper shape is out-of-process**: YAMP spawns `d1a.exe` (the launcher already
spawns child processes with a per-game CWD) alongside a 32-bit shim DLL that neutralises the
Ringwide gates and hooks D3D9. That runs Daytona, but YAMP would be a front-end with no access to
machine state - no HLE hooks, no lockstep, no CRT filter, no settings UI. Lifting the emulator core
out of `.text` into an x64 module is possible and pointless; we already have working modules for
StF/FV/MR/VON/VF2.

**Treat d1a.exe as a documentation drop, not a hosting target.**

## So what would it take to run Sega Racing Classic in YAMP?

**Not a ROM swap onto an existing module.** Daytona USA is Model 2 *original*; the graphics
hardware is incompatible with Model 2B, so the homebrew trick - drop a ROM into `rom/<stem>/` and
retarget the HLE hooks with `[HleRetarget]` - cannot carry Daytona onto the Sonic the Fighters
module. The ROM set says the same thing before any of the
rendering is examined: Daytona ships **`rom_copro.bin` (2 MB)**, a coprocessor image that
`STF_ROM_FILES` has no slot for at all, and **two program ROM banks** (`rom_code1.bin` 0x40000 +
`rom_code2.bin` 0x80008, the `+8` a header) where every module we host loads one.

The ROM set is on disk at
`build/bin/Win64/Debug/rom/daytona_rom/` (`rom_code1`, `rom_code2`, `rom_copro`, `rom_data`,
`rom_ep1`, `rom_pol`, `rom_tex`) - same file-naming convention the modules use, which is what makes
the mismatch worth stating explicitly rather than assuming.

**What does transfer is the CPU side.** The guest addresses from the symbol table land on valid
i960 code in `rom_code1.bin`, and `interrupt_wait_b` (0x13A0) opens with the same vsync handshake
StF's Core hooks are built around - an 8-byte MEMB `ldob` of absolute `0x00500000` at `+0x4` and
`+0xC` (`80803000 00005000`), the identical shape to StF's `interrupt_wait_b+0x88`/`+0x90` hook
sites. The i960 core, the 0x500000 copro/vsync protocol, the HLE trap mechanics and the
symbol-driven retarget machinery would all apply unchanged.

The boundary is therefore clean: **CPU and board handshake shared, geometry and rendering not.**
Model 2 original's geometry path is not the one any hosted module implements, so HLE-ing SRC means
implementing that path - with d1a.exe as the reference to read, since it is the only Model 2
original emulator we have and it is readable (named `geo_func` / `copro_down` / `cop_initialize`
call sites on the ROM side, `render_pass::PassObjectM2` and `obj::m2::ObjectM2DrawingManager` on
the host side). That is a real project, not an integration.

Which leaves two honest options:

1. **Out-of-process shim** - cheap, runs the game as Sega shipped it, YAMP is only a launcher.
2. **Implement Model 2 original geometry/render in YAMP**, using d1a as the decompilation
   reference. Everything below the geometry processor is already solved.

## Scoping the sidecar: d1a's own emulator API

If YAMP drives d1a out-of-process, the shim calls d1a's internals directly. Relocs are stripped and
ASLR is off, so **every address below is a fixed constant for this build, permanently** - no pattern
scanning, no rebasing. All of it was read statically; none is runtime-confirmed yet.

### Boot

`FUN_004C9710` is the loader task's `main` (task descriptor at `0x6ECD40`; state at object `+0x4C`,
tick at `+0x50`), a 17-state pump. The states that matter: **0xD** kicks the machine
(`FUN_004E2830`, `FUN_004E27B0`), **0xE** polls `FUN_004E27D0` until it returns 1.

    FUN_004E27D0 -> FUN_004E25A0   ROM alloc (FUN_00517110) + async load (FUN_005172B0)
                 -> FUN_004E2680   subsystem init
                 -> FUN_004E2690   machine reset

ROM buffer globals: `DAT_01060E70/74/78/7C/80/84` = ep1 / ep2 / data / pol / tex / copro.
Program ROM banks are static `.data` arrays at `0xBA39E0` and `0xCA39E0`.

### Per frame - the whole surface is two calls

    FUN_004C9DB0            game task main (task descriptor at 0x6ECDA0)
      FUN_004E2880          gated on DAT_00911338 (running); bumps _DAT_00911340 (host frames)
        FUN_004E2870        gated on DAT_00911348 != 1 (pause)
          FUN_004EFED0      >>> STEP THE MACHINE <<<
        FUN_004E3BE0        status overlay, then FUN_004F5E30 (render)

`FUN_004EFED0` reads a mode from `FUN_004E42B0()`; mode 2 is "running", and it brackets the run with
`FUN_004F5E10` / `FUN_004F5E20`, choosing `FUN_004EFEA0` or `FUN_004EFE10` on `DAT_01060BC9`.
Emulated frame counter is `_DAT_01060BE4`.

So a shim that wants frame-accurate control replaces one call site and owns the pacing.

### Guest memory map - the same design as the x64 modules

A table of `{read, write}` handler pairs at **`0x6EE280`**, 0x40 bytes per region: six widths
(1 / 2 / 4 / 8 / 12 / 16 bytes, matching i960 `ldob`/`ldos`/`ld`/`ldl`/`ldt`/`ldq`) plus two unused
slots. Handler signature is `void(void* dst, uint32_t guest_offset)`. The program-ROM region's
read handlers are `0x4E5F10` / `0x4E5F30` / `0x4E5F50` / `0x4E5F80` / `0x4E5FE0` / `0x4E6010` - each
one a five-instruction `mov` from `guest_off + 0xBA39E0`.

This is the same architecture as the memory-map dispatch YAMP already wraps in the x64 modules
(`MemWatch`, `CharRamFix`), so guest-memory interception needs no new technique.

### Geometry output

`FUN_004E2B50(callback)` hands the callback a **double-buffered 0x4000 page** at
`DAT_00911350 + page * 0x4000`, page selected by `DAT_00919351`, gated on `DAT_00919354`. That is
the display-list / BUFF_RAM page, and it is the natural interception point for feeding YAMP's
renderer instead of D3D9.

### What is NOT yet known - this is where the estimate lives

1. **The i960 context layout.** The step entry is found; the register file is not. The x64 modules
   put IP at `ctx+0x08`, r0.. at `+0x58`, g0.. at `+0x98`; d1a is probably similar but that is an
   assumption, not a measurement.
2. **Whether `FUN_004E2B50`'s 0x4000 page is the complete geometry stream** or only one channel of
   it. Everything downstream (`render_pass::PassObjectM2`, `obj::m2::ObjectM2DrawingManager`) is
   still unmapped.
3. **Input injection** - where the emulated I/O board sources switches and analogs.
4. **Whether the machine can free-run**, decoupled from d1a's own vsync/timer thread. Lockstep
   depends entirely on this.

### Runtime check (2026-08-10, x32dbg on the launched process)

The static map above is untested, but the *boot* was checked live, and it moved the estimate.

**d1a blocks before it creates a window.** Launched from its own directory it sits at 0.02 s of CPU
with no top-level window and 7 threads. Attaching and walking the main thread's stack:

    ntdll!<wait>  <-  kernelbase+0x15D652  <-  d1a.exe+0x282947   (VA 0x00682947)

`0x682947` is inside `FUN_006828E0`, which names itself in its own error string:
**`pcptCheckConnectAble`** - the P-ras / ALL.Net billing client (same `pcpp*` / `PCPA_RESULT_*`
family as the strings in `.rdata`). It is parked in `WaitForSingleObject` on a WSA event, waiting on
a TCP connect to a server that does not exist. So **the network/billing handshake is the FIRST gate,
ahead of the keychip screen and ahead of any window** - a sidecar has to stub the `pcp*` client
before the frame driver is reachable at all.

One run appeared to get past that wait and spin at ~100% of a core (24.55 s CPU in 25 s wall), but
**that did not reproduce** - a later scripted run held at 0.02-0.08 s CPU with every thread parked in
ntdll and the main thread still at the `pcptCheckConnectAble` wait. Treat the spin as an unexplained
one-off, not as behaviour. The reliable state is: blocked forever, no window.

### The injection point: a VERSION.dll proxy (confirmed working)

`libamv_nvidia.dll` / `libamv_amd.dll` look like an obvious piggyback - d1a `LoadLibrary`s them by
name (`FUN` at `0x677150`, resolving `amvGetAvailableDriver_Nvidia`, calling it, checking `>= 0`,
then freeing the library) and neither exists on disk anywhere. **They are a dead end**: a stub DLL
under both names was never loaded in a 12-second run, because that code sits *after* the network
gate d1a never gets past. Right technique, wrong hook.

The hook that works is a **statically imported, non-KnownDLL**, which loads during process init
before d1a executes anything:

    d1a.exe  --(static import)-->  d3d9.dll  --(static import)-->  VERSION.dll

`KnownDLLs` on this machine covers advapi32, kernel32, ole32, Setupapi, user32 and WS2_32 - so
d3d9, d3dx9_40, dinput8 and version are all app-directory hijackable, and a dependency's imports
resolve against the **executable's** directory first.

Verified live: a `version.dll` proxy forwarding all 17 exports to a `version_orig.dll` copy loads
cleanly and logs from `DllMain`, reporting `d1a.exe image base = 0x00400000` - which also confirms
at runtime the fixed-base assumption the whole address map above depends on. Generator + source in
the session scratchpad (`genproxy.py` emits `#pragma comment(linker, "/export:NAME=version_orig.NAME")`
for each export). This is the sidecar's beachhead: it runs before the `pcp*` network client, so it
can patch `pcptCheckConnectAble` before that code is ever reached.

### The gate ladder (worked through live, 2026-08-10)

Each gate is patched from the VERSION.dll proxy's `DllMain`, before d1a runs. Addresses are fixed
constants (relocs stripped, base confirmed 0x00400000 at runtime).

| # | gate | where | fix | result |
|---|---|---|---|---|
| 1 | ALL.Net / P-ras connect | `pcptCheckConnectAble` `0x006828E0` | 6-byte `mov eax,-10 / ret` | blocked forever -> **boots**, window `SEGA RACING CLASSIC`, ROMs load (WS 89 -> 225 MB), clean exit ~15 s |
| 2 | keychip = **ERROR 0949** | see below | `E9` detours into the proxy | **SOLVED** - gate reads true, no 0949 raised |
| 3 | JVS I/O = **ERROR 6401** | `0x00480A2D`, the only `push 0x1901` in the image | suppressed at the raise function; a clean fix is still open | app **stops exiting** and runs indefinitely once errors are suppressed |

**Gate 2, what it actually took.** Five detours, not three - and the one that mattered was the one
that looked least relevant:

| target | role | note |
|---|---|---|
| `0x00669990` | `amDongleInit` -> 0 | |
| `0x0066AF70` | `amDongleSetAuthConfig` -> 0 | **stdcall `ret 4`**; without this `FUN_004C2800` jumps past the line that sets the flag, so the other four are dead weight |
| `0x0066AA40` | `amDongleSetupKeychip` -> 0 | |
| `0x00669AA0` | dongle-available predicate -> 1 | this is what actually writes `DAT_00910302` |
| `0x0066CC10` | `amDongleBillingGetKeychipId` -> writes an ID | **stdcall `ret 8`**, `memcpy_s(dst, 0x11, ...)` so <= 16 chars |
| `0x0066E470` | `amDongleUpdate` -> 0 | per-frame pump in `FUN_004C06E0` CLEARS the flag if this fails |

The gate: `FUN_00480A60` does `if (!FUN_004C0900()) FUN_00483A00(0x3B5)`, `FUN_004C0900` returns
`DAT_00910302`, and **0x3B5 = 949**. Verified by instrumenting `FUN_004C0900` - it now reads 1.

**The technique that ended the guessing.** `FUN_00483A00(code)` is "raise app error": it latches
`code` into `0x008523E8` and switches the app task to mode 5 (`TASK_MODE_APP_ERROR`). A `jmp`
detour leaves the stack untouched, so `_ReturnAddress()` inside the hook **is** the d1a caller -
log `(code, caller)` and suppress, and one run enumerates every error the game wants to raise with
its origin. Two builds were wasted stubbing leaves before reaching for this; do it first next time.

Beware a false positive when hunting raise sites by constant: `0x675559` also contains
`push 0x3B5`, but it is `push file / push 949 / push msg / call FUN_0067C2E0` - the three-arg
logger, where 949 is a **line number**.

Gate 1's patch target matters: `pcptOpenClient` (`0x00683090`) is a connect-retry loop that spins
**while `pcptCheckConnectAble` returns 1**. Returning 0 would falsely claim a connected socket;
returning -10 is the function's own connect-failed code and breaks the loop through a path the
caller already handles.

**Why gate 2 resisted, and what it means.** Every `amDongle*` function opens with
`mov eax,[0x823A14] / cmp [eax],...` - a *shared AM-library context*. Detouring `amDongleInit` to
return 0 without letting it run leaves that context null, so every AM entry point that was not
detoured still sees an uninitialised library. Stubbing three functions cannot satisfy a subsystem
that keeps shared state.

The error screen is driven by a paired `{message, code}` table at ~`0x6E8BC0`, reached through
`TASK_MODE_APP_ERROR`:

    Keychip Not Found                             ERROR 0949
    Battery is run down                           ERROR 8110
    P-ras Hardware Error                          ERROR 8109
    P-ras Network Caution                       CAUTION 8106
    P-ras Card Invalid / Auth. Error / Network Error / Card Not Found / Card Malfunctioning
                                            ERROR 8105..8101
    JVS I/O board does not fulfill the game spec  ERROR 6402
    JVS I/O board is not connected to main board  ERROR 6401

That table is the real scope statement: behind the keychip sit P-ras card and network auth, and
behind those the **JVS I/O board** - which a PC does not have either. The Ringwide service layer
(`amDongle`, `amEeprom`, `amBackup`, `amNetwork`, `amSysData`, JVS) is a coherent subsystem that
needs a proper shim, not point patches. Find where error 0949 is raised and work from the table.

Deployed files in the game dir: `version.dll` (proxy), `version_orig.dll` (copy of the real one for
the forwarders). Source: `version_proxy.c` in the session scratchpad.

### Display mode: fullscreen and resolution are just two globals

The arg parser (`FUN_00482080`) writes these; a sidecar can set them in `DllMain` instead of
passing switches. Option strings live at `0x6E86C0`-`0x6E8708`.

| global | switch | meaning |
|---|---|---|
| `0x008521FD` | `-fs` | **fullscreen flag** (byte) |
| `0x00852200` | resolution switches | **resolution enum** |
| `0x00852204` | `-sxga`, `-wxga_dbd` | dot-by-dot / secondary variant |
| `0x008521FC` | `-ve` | vsync emulation |
| `0x00852208` | `-aa` | anti-alias |
| `0x008521EC` | `-debug` | debug mode (gdb) |
| `0x008521F4` / `0x008521F8` | `-m` / `-s` | mode / sub-mode number |

Resolution enum values: 1 VGA 640x480, 2 SVGA 800x600, 3 XGA 1024x768, 4 SXGA (also sets
`0x852204`=3), 6 UXGA, 7 WVGA 800x480, 8 WSVGA 1024x600, 9 WXGA 1280x768 (`-wxga_dbd` also sets
`0x852204`=9), 10 WXGA 1360x768, **14 = `-model2` (Model 2 native)**, **15 = `-model2x2`**.

14 is the interesting one for a YAMP sidecar - it is the board's own 496x384, which is exactly what
the CRT/aspect presentation path expects. `FUN_00482930` also reads/writes `0x852200`, and is the
likely home of the `d1a.ini` `resolution = unique | 1280 x 720` validation, which is a *hardware
check list* and separate from these globals.

### Getting it windowed (done)

**The `-fs` global is a red herring.** `0x008521FD` reads 0 already, and zeroing it changes nothing:
d1a asks D3D9 for fullscreen-exclusive directly. Confirmed from the hook -
`CreateDevice: app asked Windowed=0 1280x720 fmt=22 refresh=60`.

The tell, before any hooking: a sibling window of class **`D3DProxyWindow`**, 1280x720 at 0,0,
`WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE`. d3d9.dll creates that only
for fullscreen-exclusive. While it exists, restyling d1a's own HWND is pointless - presentation is
not going there. **If it is present, you are not windowed, whatever the window styles say.**

Fix, in the proxy: all `IDirect3D9` instances in d3d9.dll share one vtable, so create a throwaway
interface, patch **slot 16 (`CreateDevice`)**, release it - the instance d1a creates later is
already hooked. No code detour, no trampoline. In the hook, force `Windowed=TRUE`,
`FullScreen_RefreshRateInHz=0`, `BackBufferFormat=D3DFMT_UNKNOWN` (required for windowed), then
patch **slot 16 (`Reset`)** on the returned `IDirect3DDevice9` or the first device-lost reset goes
straight back to exclusive. Do this from the watcher thread, not `DllMain`: d3d9's own `DllMain`
has not run when ours does, and d1a does not reach `CreateDevice` until ~2 s in.

Result: `D3DProxyWindow` never appears, `hr=0`, and d1a's window is a normal framed window with
client 1280x720.

Window styling is a separate, one-shot job: `FindWindowA("SEGA RACING CLASSIC", NULL)` from the
watcher thread, set `WS_OVERLAPPEDWINDOW|WS_VISIBLE`, size via `AdjustWindowRectEx` so the CLIENT
keeps the backbuffer size. **Apply once and stop** - re-applying on a timer fights the app's own
resizing and makes the client oscillate.

### Estimate

| tier | work | gated on |
|---|---|---|
| ~weekend | boot + frame-step under external control, d1a still drawing via D3D9 | stubbing `pcptCheckConnectAble` and whatever the post-network spin is - the ~8 emulator functions are all found, but the game does not reach them unaided |
| ~weeks | intercept the display list, YAMP renders (CRT, aspect, upscale) | unknown 2 |
| month+ | `execute_info`-shaped IPC with determinism and netplay | unknowns 1, 3, 4, **and** moving the lockstep boundary across a process boundary |

The honest read: tier 1 is small and well-understood, tier 2 is a normal RE job, and tier 3 is the
one that can go badly - not because of d1a, but because lockstep across IPC is a design change on
YAMP's side.

## What is worth mining

- The symbol tables, done: [d1a-daytona-symbols.md](d1a-daytona-symbols.md) and the cross-game map
  in [m2-rom-symbol-tables.md](m2-rom-symbol-tables.md).
- **Daytona's linked-cabinet stack is named function by function** - `Set_Network`,
  `set_link_parms`, `check_comm_RAM_bank`, `link_ctrl_before`/`_after`, `trans_system_data`,
  `trans_en_data`, `receive_entry_car_data`, `expect_future_pos`, `collect_en_data`,
  `comm_race_clear`. Directly comparable to the MR comm work ([mr-comm-packet.md](mr-comm-packet.md))
  and the SRC2 lockstep path.
- The i960 opcode table is liftable wholesale for a disassembler in the debug UI.
- The `dw::` / `test_mode::` class inventory names the debug windows `Debug/DwGame.h` reimplements.
