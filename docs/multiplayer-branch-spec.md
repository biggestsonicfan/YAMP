# YAMP — `multiplayer` branch specification

**Handoff document for reimplementation.**

| | |
|---|---|
| Baseline commit | `45c0d9ae273096679833d540a3714399c8a1d6e0` — *"Update ModUtils to fix compilation under VS2022"*, 2024-05-12 |
| Branch | `master` (tip: *"Virtua Fighter 2 netplay, shared cabinet switches, and the 2.0/2.1 room flag"*) |
| Delta | 29 commits, 159 files, **+50,242 / −4,628** lines (see §16 for the one change still outside git) |
| Platform | Windows x64, MSVC (VS2022), C++17, premake5 |

> **Note, 2026-09-04.** Every `plugin/yampnet/...` path in this document — and in the other
> `docs/` files that mention one — described the netplay plugin while it still lived inside this
> repository. It has since moved out to its own: <https://github.com/biggestsonicfan/YAMPnet>,
> where those files sit under `source/`. Nothing about the design changed, only the location; the
> paths are left as written because this is a record of the work as it was done.

At the baseline, YAMP was a single-purpose launcher: it hosted **one** arcade module — Virtua Fighter 5: Final Showdown extracted from Yakuza 6 — with no audio, one hard-coded keyboard layout, and a DX11 render path. This branch turns it into a **multi-title, multi-engine-generation arcade module host** with audio, configurable input, integrity verification, a game launcher, homebrew ROM support and online play.

---

## Table of contents

1. [What "hosting a module" means](#1-what-hosting-a-module-means)
2. [Scope of the delta](#2-scope-of-the-delta)
3. [Build system](#3-build-system)
4. [Source layout](#4-source-layout)
5. [Process entry & game selection](#5-process-entry--game-selection)
6. [Cross-cutting services](#6-cross-cutting-services)
7. [Presentation layer (`RenderWindow`)](#7-presentation-layer-renderwindow)
8. [The `pxd` platform layer — three engine generations](#8-the-pxd-platform-layer--three-engine-generations)
9. [The `m2ftg` host family (Model 2 arcade boards)](#9-the-m2ftg-host-family-model-2-arcade-boards)
10. [The VF5FS hosts](#10-the-vf5fs-hosts)
11. [CRIWARE audio](#11-criware-audio)
12. [Homebrew Model 2B ROM hosting](#12-homebrew-model-2b-rom-hosting)
13. [Debug tooling](#13-debug-tooling)
14. [Netplay](#14-netplay)
15. [Settings file reference](#15-settings-file-reference)
16. [Working-tree state at hand-off](#16-working-tree-state-at-hand-off)
17. [Suggested reimplementation order](#17-suggested-reimplementation-order)
18. [Verification methodology used](#18-verification-methodology-used)

---

## 1. What "hosting a module" means

Every game YAMP runs is a **DLL extracted from a retail Yakuza/Judgment game** (`*-pxd-w64-*.dll`). Inside the parent game those DLLs are driven by the host executable's own scene code. YAMP replaces that host: it builds the engine context structures the module expects, hands them to the module's `module_start`, then calls `module_main` once per frame and composites the module's output texture into its own swapchain.

The protocol is not documented anywhere. **Every structure layout, symbol RVA and byte pattern in this branch was reverse-engineered** from the module DLLs and from the parent games' host code (Ghidra + live debugging). The governing engineering rule throughout the codebase, stated repeatedly in comments:

> **Always find and port the game's own implementation. Never invent a YAMP-side approximation.**

Practical consequences a reimplementer must respect:

- Structure layouts are pinned by `static_assert` on `sizeof` and every meaningful `offsetof`. Those asserts are the specification — do not relax them.
- Hosts resolve module symbols by **byte pattern scan**, not by export name (the modules export almost nothing). A wrong-build DLL therefore mis-patches *silently*; this is the reason §6.4 exists.
- Some DLLs are ASLR'd (`DYNAMIC_BASE`) and some are not. **Never assume base `0x180000000`.** This was the cause of the Fighting Vipers black screen.

---

## 2. Scope of the delta

### 2.1 Games supported

| Switch | Game | Parent game | Engine gen. | Renderer | Host |
|---|---|---|---|---|---|
| *(default)* / `-stf` | Sonic the Fighters | Lost Judgment | LJ | DX12 | `m2ftg/LJ` |
| `-fv` | Fighting Vipers | Lost Judgment | LJ | DX12 | `m2ftg/LJ` |
| `-mr` | Motor Raid | Lost Judgment | LJ | DX12 | `m2ftg/LJ` |
| `-vf2` | Virtua Fighter 2 | Yakuza: Like a Dragon | YLAD | DX11 | `m2ftg/YLAD` |
| `-vf2-k2` | Virtua Fighter 2 | Yakuza Kiwami 2 (GOG) | K2 | DX11 | `m2ftg/K2` |
| `-von-k2` | Virtual On (*"omg"* = Operation Moon Gate) | Yakuza Kiwami 2 (GOG) | K2 | DX11 | `m2ftg/K2` |
| `-vf5fs` | VF5: Final Showdown | Yakuza 6 | Y6 | DX11on12 | `vf5fs/Y6` |
| `-vf5fs-lj` | VF5: Final Showdown | Lost Judgment | LJ | DX12 | `vf5fs/LJ` |
| `-vf5fs-ylad` | VF5: Final Showdown | Yakuza: Like a Dragon | YLAD | DX11 | `vf5fs/YLAD` |
| `-fv2` | Fighting Vipers 2 | Like a Dragon Gaiden | LJ | DX12 | `pre3/Gaiden` |
| `-src2` | Sega Racing Classic 2 (Daytona USA 2) | Like a Dragon Gaiden | LJ | DX12 | `pre3/Gaiden` |
| *(no argument)* | Game-select launcher | — | — | — | `GameLauncher` |

Only `-vf5fs` existed at the baseline. **Eleven playable titles** exist at the tip — the last two on a different arcade board entirely (Model 3, §9.7).

### 2.2 Feature areas added

| Area | Files | Approx. size |
|---|---|---|
| CRIWARE audio (HCA + ADX decoders, Atom engine, XAudio2) | `source/criware/` | ~3,400 lines |
| `pxd` platform layers ×3 generations | `source/pxd/` | ~5,300 lines + 12.8k vendored D3D12MA |
| m2ftg host family (4 hosts) | `source/m2ftg/` | ~6,200 lines |
| VF5FS hosts (3) | `source/vf5fs/` | ~2,500 lines |
| Netplay plugin + loader + ABI | `plugin/yampnet/` (~3,980) + `source/net/` (~1,070) | ~5,050 lines |
| Input binding layer | `source/input/` | ~1,100 lines |
| Verification / launcher / settings / UI | `source/*.cpp` | ~5,000 lines |

---

## 3. Build system

`premake5.lua` defines **two projects** in the `YAMP` workspace.

### 3.1 `YAMP` (WindowedApp)

```lua
files { "source/*.h", "source/*.cpp", "source/resources/*.rc", "source/criware/*",
        "source/wil/*", "source/imgui/*", "source/Utils/*", "source/input/*",
        "source/net/YampNet.h", "source/net/NetPlugin.h", "source/net/NetPlugin.cpp",
        "source/pxd/**.h",   "source/pxd/**.cpp",
        "source/m2ftg/**.h", "source/m2ftg/**.cpp",
        "source/vf5fs/**.h", "source/vf5fs/**.cpp" }
links { "bcrypt", "dinput8", "dxguid" }
```

- `bcrypt` — SHA-256 for module integrity (`GameVerify.cpp`).
- `dinput8` / `dxguid` — the non-XInput half of the controller layer.
- The source list lives **in the project**, not in the shared `workspace "*"` block. Once the netplay plugin became a second project, a shared block made it inherit the entire emulator.

### 3.2 `YampNet` (SharedLib → `yampnet.dll`)

```lua
files { "plugin/yampnet/**.h", "plugin/yampnet/**.cpp", "source/net/YampNet.h" }
includedirs { "source/net", "source" }
links { "ws2_32", "crypt32", "secur32" }
```

Deliberately a **separate project**. YAMP never links against it — it is `LoadLibrary`'d at runtime, so a release ships without netcode simply by omitting the DLL. `YAMP.vcxproj` has no dependency on it. It does include `source/m2ftg` + `source/pxd` headers because the plugin writes `execute_info.pad[]` directly; the `yampnet_layout` handshake (§14.2) converts that coupling from a corruption risk into a clean load-time refusal.

### 3.3 Shared settings

- `cppdialect "C++17"`, `staticruntime "on"`, `/sdl`, `warnings "Extra"`, `/permissive-`.
- `disablewarnings { "4324" }` — "structure padded due to alignment specifier" fires ~130× because the RE'd structs use `alignas` deliberately; every one is pinned by `static_assert`, so the warning is never actionable and muting it keeps real warnings visible.
- Configurations: `Debug`, `Release`, `Master`. `Master` adds `NDEBUG`, `RESULT_DIAGNOSTICS_LEVEL=0`, `symbols "Off"`. Non-Debug gets `optimize "Speed"` + LTO.
- **Per-file override:** `HcaDecoder.cpp`, `AdxDecoder.cpp`, `AtomEngine.cpp` are compiled with `optimize "Speed"` even in Debug — they decode whole BGM streams at cue start, and an unoptimised build misses the chunked-start splice.

### 3.4 Submodules

| Path | Repo | Status |
|---|---|---|
| `source/Utils` | `CookiePLMonster/ModUtils` | Pre-existing and still the only submodule. Provides `MemoryMgr.h`, `Trampoline.h`, `Patterns.*`, `ScopedUnprotect.hpp`. See §16 for one uncommitted fix inside it that a fresh clone will need. |

> **Build note.** `build/` is gitignored; premake is all a fresh clone has. `premake5.lua` is the source of truth — never hand-edit `build/YAMP.vcxproj`. Config names are `"Debug Win64"` / `"Master Win64"` etc. with platform `x64`.

> **Historical trap, now fixed:** `DebugLog.{h,cpp}` originally lived inside `source/Utils`, a third-party submodule tracked by neither repo — so it existed on one machine only and a clone could not build. Moved to `source/` and tracked.

---

## 4. Source layout

The tree was restructured twice (commits `b2c84a2`, `bc9bfaf`). The final rule: **platform layers, hosts, and cross-cutting services are separate, and nothing generation-specific sits in a folder implying it is shared.**

```
plugin/yampnet/        the netplay DLL (own project, plain-C ABI to YAMP)
source/
  *.cpp/h              process entry, window, UI, settings, verification, launcher, logging
  criware/             CRI Atom reimplementation (HCA/ADX decoders, ACB/AWB, XAudio2)
  imgui/  wil/  Utils/ vendored
  input/               binding layer shared by EVERY game (keys, XInput, DirectInput, pad policy)
  net/                 netplay plugin LOADER + the shared YampNet.h ABI. No netcode here.
  pxd/                 the pxd PLATFORM layer, one folder per engine generation
    (root)             generation-NEUTRAL only: Imports.h, pxd_shader.h
    LJ/                Lost Judgment era: sl 0xF000 / gs 0x388A00 / ct 0x30, DX12 host cdevice
    Y6/                Yakuza 6 era: its own sl/gs/file_access/async_request/cs_game/sys_util
    K2/                Kiwami 2 era: sl 0xF3C0 (gs 0x202140 stays opaque)
  m2ftg/               the Model 2 arcade host family
    (root)             m2ftg.h (module protocol), ImportSymbols.h, ModuleArgs, DisplayModes,
                       HostUI, file_access.cpp, and the pieces EVERY m2ftg host shares:
                       HleHooks, DebugWindows, NetSession, SystemSwitches
    ELF/               homebrew ROM support: ElfRom, CharRamFix
    LJ/                StF / FV / MR host + Patch
    YLAD/              VF2 (Like a Dragon) host
    K2/                VF2 + Virtual On (Kiwami 2) host
  vf5fs/               VF5FS hosts ONLY (no platform code)
    (root)             vf5fs.h — the shared module_start protocol
    Y6/  LJ/  YLAD/    one host per build
```

`HleHooks`, `DebugWindows`, `NetSession` and `SystemSwitches` were lifted out of `m2ftg/LJ/` in
2026-08-02 when Virtua Fighter 2 needed them: none is LJ-specific, and leaving them there would have
forced the YLAD host to include `LJ/` headers and reach for the LJ `GameDesc` table, which has no VF2
entry. Each now carries its own per-game descriptor keyed off `GameId`, including the module's DLL
name, so nothing in them assumes a particular host.

Each move was verified **by include graph first**. `pxd/Y6` in particular looked like shared VF5FS code, but nothing outside `vf5fs/Y6` included any of it.

> Older code comments and commit messages reference pre-restructure paths (`source/LJ`, `source/Y6`, `source/DX12`, `m2ftg/M2Input`). Map them onto the table above.

---

## 5. Process entry & game selection

`source/Main.cpp` (`wWinMain`):

1. Debug-only `SuppressDebugCrtAsserts()` — the pxd engine's trap/log path formats with exotic and often mismatched varargs. Under the debug UCRT this raises `_CrtDbgReport` assertions in a tight logging loop, which with a debugger attached becomes an inline `int3` and halts forever. Route CRT reports to the debugger and force `_CrtDbgReport` to return "continue" via `_CrtSetReportHook2`/`W2`.
2. `TerminateProcess` scope-exit hack — shutdown crashes on mismatched allocators. **Known outstanding issue.**
3. ImGui context creation; `io.IniFilename = nullptr`.
4. Command-line parse. **Ordering matters**: `-vf2-k2` must be tested before `-vf2`; `-vf5fs-lj` and `-vf5fs-ylad` before `-vf5fs`. Each is a prefix of the shorter switch.
5. `-frames N` → `gGeneral.SetFrameLimit(N)`. After N frames a host leaves its loop **normally** and shuts the module down. This exists because a killed process cannot be distinguished from a crashed one: terminating a debuggee mid-frame faults whichever worker thread was inside an allocator, which looks exactly like a real module bug. Smoke tests get a deterministic length *and* a real teardown path.
6. No game argument → `Launcher::Run()`.
7. Otherwise: `gGeneral.SetGameId(...)` → `<host>::LoadDLL()` (which runs the verification gate) → `<host>::PreInitialize()` → construct `RenderWindow` → `<host>::Run(window)`.
8. The LJ m2ftg path additionally seeds settings before the DLL check (so the UI has something to read), then `net::ParseCommandLine()` + `net::Load()` before `Run`, and `net::Unload()` after.

---

## 6. Cross-cutting services

### 6.1 `YAMPGeneral` (`source/YAMPGeneral.{h,cpp}`)

Global singleton `gGeneral`. Added on this branch:

- `enum class GameId { VF5FS, StF, VF2, FV, MR, VF5FS_LJ, VF2_K2, VON_K2, VF5FS_YLAD, Launcher }`.
- `GetArcadeGameName()`, `GetParentGameName()`, `GetGameTag()` — display names and a short log prefix (`"StF"`, `"FV"`, `"VF5FS-LJ"`, …) so logs from shared code paths name the game actually running.
- `IsModel2ArcadeGame()` — true only for StF/FV/MR/VF2/VF2_K2/VON_K2. These render a 4:3 240p-era image on a CRT cabinet, so aspect boxing and the CRT filter belong to them. Every VF5FS build is a modern widescreen game that must **not** get either treatment. The settings are shared (one `[StF]` section), so the presentation path gates on the *game*, not on the flag.
- `SetDataPath()` — **portable mode**: user data (settings.ini + saves) lives next to `YAMP.exe`, shared by every game. Resolved from the module path, **not the CWD** — games booted from the launcher run with the game folder as CWD.
- `GetFrameLimit()` / `SetFrameLimit()`.

### 6.2 `DebugLog` (`source/DebugLog.{h,cpp}`)

Unified diagnostics replacing ad-hoc `sprintf_s` + `OutputDebugStringA` pairs.

- `DebugLog(fmt, ...)` → `OutputDebugStringA`.
- `DebugLogFile(fmt, ...)` → same, plus appended and flushed per line to `d3d12_debug.log` in the CWD (survives a process death with no debugger).
- `DebugLogV(fmt, va_list)` → forwarding entry for local variadic wrappers.

Gate: `YAMP_DEBUG_LOGGING` is `1` iff `DEBUG || _DEBUG`. **Both** are tested deliberately: `DEBUG` is premake's own (tracks the configuration), `_DEBUG` is the MSVC debug CRT. Keying off `_DEBUG` alone would silently disable all logging if the Debug config were switched to the release runtime — a common speed tweak. In Release/Master the macros compile away entirely, arguments are not evaluated, and the log filenames do not survive into the binary. `RenderWindow.cpp`'s `pso_stream.log` / `heaps.log` dumps use the same gate.

### 6.3 `GameLauncher` (`source/GameLauncher.{h,cpp}`, ~680 lines)

The no-argument path. Full-screen ImGui window that:

- Discovers which arcade modules are present, via `Verify::GameInstallRoots()` (below) plus the folders beside `YAMP.exe`.
- Shows per-game verification status (module hash verdict + parent-game verdict).
- Offers the Model 2 **Render resolution** picker and `Model2WindowMatchesRender` inline.
- **Play** launches a *child process* — the same `YAMP.exe` with the game's switch and **CWD set to the game directory**.
- `Rescan` re-runs discovery.
- F1 opens the shared settings window.
- **Escape / Exit quit path.** With the `Fullscreen` setting on, the launcher window is a borderless `WS_POPUP` — no title bar, no close button — and the launcher loop originally had no key handling at all, so the process could only be killed. Escape raises a *"Do you really want to quit?"* modal, with a matching `Exit` button beside Play/Rescan for mouse and pad users.

  Escape's **precedence** matters, so it cannot quit out from under something else: the F1 settings window closes first (hence the `IsSettingsOpen()` / `CloseSettings()` accessors on `YAMPUserInterface`), then the prompt itself cancels on a second press, and any other open ImGui popup is left to ImGui's own Escape handling. That last step is needed because the key still reaches `WndProc` regardless; **the modal case is YAMP's own because this ImGui build's `NavCancel` only closes non-modal popups.** The modal's default nav focus is **Cancel** — the destructive button must never be one Enter away.

### 6.4 `GameVerify` (`source/GameVerify.{h,cpp}`, ~740 lines)

Two questions, answered with two **deliberately different** methods.

**"Is this arcade module a build YAMP can host?"** → full **SHA-256** of the file against a known-build table. Because every host resolves symbols by byte pattern and hard-coded RVA, a DLL from a different build mis-patches silently instead of failing cleanly. An exact hash is the only honest answer. A non-matching DLL is **blocked before `LoadLibrary` ever runs its `DllMain`**.

**"Does the user own the parent game?"** → **PE header identity** of the parent executable (`TimeDateStamp` + `SizeOfImage` + file size). Those executables are hundreds of megabytes; hashing them would add seconds to every boot for no extra certainty. A missing parent game **blocks**; a parent game of an unrecognised build only **warns** — the arcade DLL is the file compatibility actually depends on.

Statuses: `ModuleStatus{Verified, UnknownBuild, OutdatedBuild, Unreadable, NotChecked}`, `ParentStatus{Verified, UnknownBuild, NotFound, NotChecked}`.

Known-good module table (all values are load-bearing):

| Game | SHA-256 | Size | TimeDateStamp | Build |
|---|---|---|---|---|
| StF | `DF7FE561ED3B2954066CC138C179B9DD5CE2F65D0EC4A4104A7AD161236A07BB` | 2,086,896 | `0x637B11A3` | Lost Judgment (2022-11-21) |
| FV | `0F1DAD193533C4C250EDA54BD3C5D63751D1E3431E4DD8F702010777B9754EC0` | 2,078,704 | `0x637B119D` | Lost Judgment |
| MR | `9FCBAE38DD7DC04DD2B29BF09576594FA0ED5A6089D82272E200E7A1C59A9623` | 2,699,248 | `0x637B1190` | Lost Judgment |
| VF5FS-LJ | `2A83D302768D7AFA5F2EF0B0D0481CF281F081FCE1D9CCFF3CA34EE94358E36B` | 6,152,688 | `0x637B1259` | Lost Judgment |
| VF2 (YLAD) | `3B3AF23E2075C84F996F7194BE477600C1D887965311702272A324FCBE7B1818` | 1,648,128 | `0x601763D1` | Like a Dragon (2021-02-01) |
| VF5FS-YLAD | `A022DDD4185489146B9D757B8E8590467C5B1F18A1139144FCCA3676DB359B69` | 5,946,880 | `0x601766BE` | Like a Dragon |
| VF5FS (Y6) | `48787216D767F3566ACA129D848A7931E6D2797A59D01216D6F92C24702ED654` | 5,389,312 | `0x60AB8422` | Yakuza 6 (2021-05-24) |
| VF2-K2 | `4D9473F052822CFA1EA621F7C10D0CE46A0B18178533EFCB325B13E0C03DCB6A` | 1,614,848 | `0x5D91BBC9` | Kiwami 2 GOG (2019-09-30) |
| VON-K2 | `99AD0D31AE5949F6C5F521119FC9E26FF5CA602B1629583CBF478D1FCDC4D39C` | 4,734,976 | `0x5D91BBCA` | Kiwami 2 GOG |

Explicitly-rejected pre-update builds, recognised by timestamp alone (they get "update your game" rather than the generic unknown-build text): `0x603E22E3`, `0x606D6969`, `0x6075A65A`.

Known parent executables: `LostJudgment.exe` (`0x641412B9` / `0x1AE88000` / 422,131,184), `YakuzaLikeADragon.exe` (`0x6141B906` / `0x18C91000` / 387,532,936), `YakuzaKiwami2.exe` (`0x644A6712` / `0x0416A000` / 44,795,392), `Yakuza6.exe` (`0x60AB8368` / `0x039DC000` / 38,833,000). LJ and YLAD keep theirs in `runtime/media/`; Yakuza 6 and Kiwami 2 at the install root.

`Verify::GameInstallRoots()` — **shared** by launcher discovery and the ownership search: every Steam library's `steamapps/common/*` (registry + `libraryfolders.vdf`), every GOG game from the registry, plus `YAMP.exe`'s own folder and each folder beside it (one level, directories only). A source missing here makes its games both undiscoverable *and* unverifiable.

### 6.5 Input layer (`source/input/`)

Shared by **every** game (all four m2ftg boards and all three VF5FS builds). Moved up out of `source/m2ftg` because it was never m2ftg-specific — `YAMPSettings.h` itself includes it.

**`Input.h` — actions.** `Action_{Up,Down,Left,Right,Punch,Kick,Guard,Start,Coin,PG,PK,KG,PKG,Back,Test,Service}`. The first `WIZARD_ACTION_COUNT` (= `Action_Coin + 1` = 9) are what the "Program All Inputs" wizard walks through, in that prompt order. `Action_Test` / `Action_Service` are the **cabinet service-panel switches**, wired to the emulated I/O board's system port rather than to a player's buttons (m2ftg boards only). They are per-player only because bindings are stored that way — either player's binding closes the one cabinet switch.

**Two disjoint controller ranges.** `PadButton` values 1..16 are XInput and are **frozen** (settings.ini stores them by number). DirectInput inputs occupy their own range: `Pad_Btn1 + n - 1` for numbered buttons (24 of them), then the POV hat's four directions, then each of 6 axes as a pair of digital directions (`Pad_Axis1Minus + (n-1)*2`, +1 for positive). `Pad_Count <= 64` — `PadState::buttons` is a 64-bit mask.

Rationale: an XInput pad has a known button contract and keeps real names; a DirectInput pad (arcade encoders, fight sticks, PSX/Saturn adapters, most third-party pads) does not, so its inputs are numbered. A binding therefore only fires on the *kind* of pad it was made on — "A" and "Button 1" must not silently alias. Axes must be bindable because plenty of panels have no hat at all; they are numbered rather than named X/Y/Z because DirectInput does not guarantee an axis lands in the `DIJOYSTATE2` slot matching its reported name.

**Defaults.** Keyboard follows MAME's arcade layout — arrow keys, Z/X/C = P/K/G, `1` = Start, Tab = Back, I/O/U/M = P+G / P+K / K+G / P+K+G; Coin/Test/Service use MAME's `5`, F2, F3, **Player 1 only** (one cabinet fixture). Player 2 has no keyboard defaults — run the wizard. Pad defaults follow the module's own slot template: A=P, B=K, Y=G, LT=P+G, LB=P+K+G, RT=P+K, RB=K+G; X free.

**`MODULE_ASSIGN[8]` — the module-facing table.** This is the single source of truth for what each button bit *means* to the module, and it is fixed: remapping is entirely host-side. Two module facts it depends on, both read out of the StF DLL:

- `assign_t` → M2 button code (lookup `0x180177B90`, consumed by `FUN_180003AC0`): 1=none, 2=p(0x07), 3=k(0x08), 4=g(0x09), 5=pg(0x6F), 6=pkg(0x72), 7=pk(0x71), 8=kg(0x70).
- The slot table (`0x180126770`) keys each slot by a button mask **in the module's bit order**, and the engine's pad conversion (`FUN_180062470`) *permutes* the four face bits on the way in: our A(bit0)→module bit2, B(bit1)→bit1, X(bit2)→bit3, Y(bit3)→bit0. So the slots run **Y, B, X, A, LT, LB, RT, RB** in our terms; shoulders pass through unpermuted.

```cpp
inline constexpr uint8_t MODULE_ASSIGN[8] = { 4, 3, 5, 2, 6, 7, 8, 1 };
//  slot0 mask0x01 <- our Y  = Guard -> g       slot4 mask0x40 <- LT = P+K+G -> pkg
//  slot1 mask0x02 <- our B  = Kick  -> k       slot5 mask0x10 <- LB = P+K   -> pk
//  slot2 mask0x08 <- our X  = P+G   -> pg      slot6 mask0x80 <- RT = K+G   -> kg
//  slot3 mask0x04 <- our A  = Punch -> p       slot7 mask0x20 <- RB = none
```

Reading the mask order as our own order is what used to rotate Punch / Barrier / Punch+Barrier between each other. Identical mask order confirmed in FV and both VF2 builds (`0x123740` / `0x106860` / `0x10EB50`). Motor Raid and Virtual On ship no table of this shape — their slot meanings are unconfirmed.

**Device identity.** A `PadDevice` is identified by `id` — `"xinput:<slot>"` or `"dinput:<instance guid>"` — **never a list position**. The list shifts as devices come and go, and an index would hand a player whoever moved into that slot. settings.ini stores the id; the old integer key migrates silently.

**Polling economics.** `RefreshDevices()` costs ~100 ms (`IDirectInput8::EnumDevices` walks the whole HID stack, dominated by any installed virtual-pad driver) — that is six dropped frames at 60 fps. It runs at startup, from the Controls page's **Rescan** button, on an XInput slot appearing/vanishing (free — the poll already reports it), and when a device stops answering. **Deliberately not** on `WM_DEVICECHANGE`, which fires for any device node on the system (a headset, a phone charging) and would stutter a match over something unrelated. Per-frame `PollPads()` of already-open devices costs ~0.005 ms.

**`DirectInputPad.cpp`.** Cooperative level `DISCL_BACKGROUND | DISCL_NONEXCLUSIVE`. XInput pads are *also* enumerated by DirectInput as generic controllers with mangled triggers — filtered out via the Microsoft-standard marker: every XInput device's interface path (`DIPROPGUIDANDPATH`) contains `"ig_"`. Axes: `DIPROP_RANGE` is requested at ±1000, but a device may refuse it and keep its own scale (usually 0..65535 **resting in the middle**) — normalising that as if centred on zero would peg the stick in a corner, so the real range is read back per axis. `dwOfs / sizeof(LONG)` is the authoritative `DIJOYSTATE2` slot. Deadzone 0.25 radial (matching the XInput reader); digital-direction threshold 0.6, well past the deadzone so a drifting stick cannot capture a binding prompt. POV hat treated as the 8-way switch it physically is (diagonals set both directions); centred is reported as `-1` or low word `0xFFFF` depending on driver. A stick reported *only* as a hat falls back to the hat for steering.

**`Pad.cpp` — `pxd::csl_pad::set_state(index)`.** The policy that turns active bindings into engine pad state. Notable rule: the analog stick also steers, like the cabinet lever — **except** when movement is bound to axis directions. A DirectInput encoder may land its axes in slots that do not match their meaning (one tested "USB Gamepad" encoder puts its vertical axis in the X slot), so feeding raw values in alongside explicit bindings can add deflection at ninety degrees. **Where the bindings are explicit, they are the authority.**

---

## 7. Presentation layer (`RenderWindow`)

`RenderWindow.cpp` grew from ~380 to 1,902 lines.

**Device model.** `RenderWindow` **always** creates a D3D12 device and drives the swapchain from the D3D12 queue; the `ID3D11Device` it hands out is the **11-on-12** one. (This was clarified in `de7796c`, which removed unreachable DX11 branches from the LJ pxd layer that could never win against the always-present D3D12 device.)

**Threading.** The window runs on its own thread; `m_shuttingDownWindow` is an atomic. `RequestResize(w,h)` is called from `WndProc` on `WM_SIZE` and applied by the render thread at the top of `BeginFrame` (packed `(w<<32)|h` atomic). Without this the swapchain stayed at creation size forever — DXGI stretched the stale backbuffer (breaking aspect) and ImGui rendered at backbuffer scale while the mouse reported window coordinates.

**The 11-on-12 backbuffer barrier.** `d3d11on12` only emits the *Release*-side barrier (`RENDER_TARGET → PRESENT`); it never transitions the backbuffer **into** `RENDER_TARGET` on Acquire (confirmed via `ResourceBarrier` trace). The backbuffer stays `COMMON`, the composite/ImGui draws are dropped, and a black frame is presented. A small dedicated D3D12 command list issues the missing `COMMON → RENDER_TARGET` barrier each frame before the blit.

**Blit / compositing.**

- `BlitGameFrame(SRV, alphaBlend)` — the game frame.
- `BlitDX12Texture(ID3D12Resource*)` — composites a DX12-native texture (StF's output RT) via 11on12 wrap → D3D11 SRV → `BlitGameFrame`.
- `ClearBackbuffer()` — clear + bind for ImGui-only frames (the launcher).
- `SetGameAspectRatio(ar)` — the *game's* native aspect. Model 2 titles are 496×384 internal upscaled to a 1024×768 display texture = 4:3, and must be **pillarboxed** in a widescreen window, not stretched.
- `SetGameStretchToFill(bool)` — "Fill Window" mode.
- `SetModuleSourceRect(w,h)` — **critical for render-resolution support.** The m2ftg modules always render into a 1024×768 texture but lay their viewport out at whatever their own `-model2`/`-vga`/… option selected, leaving a smaller picture in the top-left corner with the remainder black. Sampling the full texture would stretch that black in with it; this restricts the blit triangle's UVs to the drawn sub-rect, and the existing aspect-corrected viewport then scales and centres it as before.

**CRT filter.** An **exact port of Lost Judgment's own PSO 9775 pixel shader**, compiled at runtime on first use, latching off for the session if compilation ever fails. Like LJ, the shader evaluates into a **window-sized intermediate** target (LJ's pause-menu captures prove its target follows the window), clamped up to ≥1080p tall so small windows don't alias the 384 scanlines into broken segments.

Two fixes the source sub-rect exposed:
1. The CRT's **second pass samples the full-size intermediate**, not the module's output — so it needs an unscaled vertex buffer (`m_vbFull`) or it magnifies a corner of the finished frame over the whole window.
2. Its scanline/grille ramps are anchored to the **picture**, so they now work in normalised picture space with only the fetch scaled back.
3. LJ's 16 px side crop is **dropped** — it only makes sense for LJ's own 1024-wide layout, and once render resolution became selectable it ate into the picture instead of the padding.

**Descriptor-heap plumbing (free functions, for the DX12 hosts).** `GetDllRingCbvSrvHeap()`, `GetDllRingSamplerHeap()`, `GetCapturedRootSignature()`. The StF DLL creates its own large shader-visible heaps (CBV/SRV/UAV ~1M + SAMPLER 2048) and binds them; `PatchGs` wires the `gs+0x7550` descriptor rings to reference **these** (captured by the device `CreateDescriptorHeap` hook) so the DLL's root-table GPU handles resolve into the bound heap. The DLL never calls `SetGraphicsRootSignature` — the host must, re-binding after each `SetPipelineState` in the command-list hook.

**Per-frame COM churn removed** (`de7796c`): RTVs over wrapped backbuffers are now built once alongside the wrappers in `CreateWrappedBackbuffers` (and dropped with them in `ResizeOn12`, which must not hold references across `ResizeBuffers`); `IDXGISwapChain3` is queried once at creation instead of twice per frame.

---

## 8. The `pxd` platform layer — three engine generations

`pxd` is the SEGA engine the modules run on. YAMP reimplements the parts of it a host must provide.

### 8.1 Generation-neutral (`source/pxd/`)

**`Imports.h` — `pxd::ImportsT<SymbolT>`.** The resolved-symbol map every host builds by pattern-scanning its module DLL. The *container* is generic; the *set* of symbols is per-DLL-family, so this is a template over the host's own enum. Each host writes its own `enum class ImportSymbol`, `using Imports = pxd::ImportsT<ImportSymbol>`, and `Imports BuildSymbolMap(void* dll)`. Backed by a `std::multimap`. `GetSymbol` asserts on absence; **`TryGetSymbol` returns null** for symbols that exist only in some DLLs (e.g. `I960_FETCH_EXEC`).

**`pxd_shader.h`** — shared by both `gs.h` variants.

### 8.2 Generation comparison

| | **Y6** (Yakuza 6) | **LJ** (Lost Judgment) | **YLAD** (Like a Dragon) | **K2** (Kiwami 2 GOG) |
|---|---|---|---|---|
| `sl` context | own | **0xF000** | 0xF000 | **0xF3C0** |
| `gs` context | own | **0x388A00** | 0x3820C0 | **0x202140** |
| `ct` context | — | 0x30 | 0x30 | 0x30 |
| Renderer | DX11 | DX12 | DX11 | DX11 |
| Folder | `pxd/Y6` | `pxd/LJ` | *(uses `pxd/LJ` sl)* | `pxd/K2` (sl only) |

### 8.3 `pxd/LJ` — the Lost Judgment generation (largest)

Files: `sl.{h,cpp}` (1,117 lines), `gs.{h,cpp}` (1,063), `pxd_types.h` (454), `file_access.h`, `async_request.{h,cpp}`, `cs_game.{h,cpp}`, `sys_util.{h,cpp}`, `sl_internal.h`, plus the host-side bring-up: `PatchGs.{h,cpp}` (632), `HostCdevice.{h,cpp}` (1,117), `DllMutex.h`, and vendored `D3D12MemAlloc.{h,cpp}` (12,792).

Used by: the m2ftg LJ hosts, the LJ VF5FS host, and (sl only) the YLAD hosts.

**`csl_pad`** — the engine pad: `m_now/m_push/m_pull/m_prev` bitfields, `m_x1/m_y1/m_x2/m_y2` floats, `int m_button_frame[32]`, `uint8_t m_buttons[32]`, `uint8_t m_prev_buttons[32]`, `m_port`, `m_user_id`, `m_is_connected`, `m_is_remote`, tail gap. `sizeof == 0x170`. `set_state(index)` is implemented in `source/input/Pad.cpp`.

**`pxd::lj_pad_t`** — the LJ-era *extended* pad, `sizeof == 0x190`, `m_buttons` at `+0xA0`, `m_port` at `+0xE0`. This is what m2ftg's `execute_info.pad[]` holds and what the netplay pad codec encodes.

**`PatchGs(gs::context_t*, const RenderWindow&)`** — fills in everything the LJ host normally puts into a fresh `gs::context_t` before the module renders: the descriptor blocks and shader-visible copy rings, the tex-id tables, the device context, and the host cdevice behind `cdevice_common::g_pD3DDevice`.

**`ResetCbvSrvRingCursors(gs::context_t*)`** — call at the start of each frame. The CBV/SRV descriptor-copy ring cursors are per-frame transient; without the reset they grow past the heap and `CopyDescriptors` access-violates (issue id 646).

**`HostCdevice.cpp` — `BuildHostCdevice(device, queue, cdeviceCtor)`.** Builds the pxd host "cdevice" object the DX12 modules expect behind `cdevice_common::g_pD3DDevice`. Layout reverse-engineered from a live Lost Judgment dump:

```
+0x08   ID3D12Device*
+0x38   intermediate-buffer freelist head (packed: lo48 = node, hi16 = ABA)
+0x40   { 0x20, 0x20, 0x20, 0x08 } metadata
+0x68   allocator object (vtable alloc/free)
+0x16e0 0x20-block node pool (deferred; not on the boot path)
+0x17b0 embedded resource factory (vf+8 = create ID3D12Resource)
+0x18d8 / +0x18dc  rwspinlocks (zeroed)
```

The DLL's **own** constructor (`CDEVICE_CTOR`) is called on our buffer so the pxd engine initialises every field, including the real `cd3d12_mem_allocator` resource factory at `+0x17b0`. `gs::initialize_module` copies `context->export_context.sbgl_context.p_value[0]` into `g_pD3DDevice`, so `PatchGs` must place **this** pointer there, not the D3D11 device.

Also here: `BuildRenderCommandContext()` (a dedicated 0x280-layout pxd command context for the `cgs_device_context+0xC8` "command recording" slot, left recording) and **`SetGameDllRange(void* dllBase)`** — registers the loaded module's real base+size for the "did this call come from the module?" return-address checks in the hooks. **This was the Fighting Vipers black screen**: StF loads at its fixed preferred base `0x180000000`, but FV is built `DYNAMIC_BASE` and relocates, so hardcoded range checks never matched and every RA-gated hook (shadow-copy list, resolve-dst tracking, exec-caller attribution) silently did nothing.

Per-frame host duties exported as free functions and called by the DX12 hosts:

- `SetModuleRenderActiveNow(bool)` — marks the DLL's render window so the `ResourceBarrier` hook only corrects the module's own `StateBefore` values, not d3d11on12's blit barriers.
- `SubmitModuleFrameListNow()` — close + `ExecuteCommandLists` the lists the module recorded into and left **open**. `module_main` only *records*; in Lost Judgment the engine's render-system loop submits, and YAMP does not run that loop.
- `AdvanceFrameStampNow()` — advance the upload-frame stamp (`cdevice+0x58` → `+0x67D8`) and recycle the upload-buffer pool's in-use nodes back to available. Fixes a pool-exhaustion crash.
- `ModuleExecDisabledNow()` — true once a submit hung or removed the device (stop the loop; DRED already dumped).

**Command-list vtable hooks (post-`c24cbd1` — only 7 load-bearing slots remain).** Previously YAMP patched **17 slots** of the process-wide `ID3D12GraphicsCommandList` vtable, so every draw, viewport, scissor, topology, IB/VB bind, RTV clear and render-target bind **in the whole process** — including d3d11on12's own blit lists — routed through a YAMP thunk that did nothing in a shipping build. (`DebugLog` compiles away; the *hooks feeding it* did not.) What is kept:

| Slot | Why it is load-bearing |
|---|---|
| `Reset` | Clears a list's module flag |
| `DrawInstanced` / `DrawIndexedInstanced` | Flag the list for the frame submit |
| `CopyBufferRegion` | Shadow-copy replay |
| `ResolveSubresource` | 3D-layer display source |
| `SetPipelineState` | Heap + root-signature injection |
| `ResourceBarrier` | `StateBefore` correction + render-target catalog |

`Close` is now *read*, not replaced. Removed: `SetDescriptorHeaps`, `SetGraphicsRootSignature`, `SetGraphicsRootDescriptorTable`, `RSSetViewports`, `RSSetScissorRects`, `IASet*`, `OMSetRenderTargets`, `ClearRenderTargetView`, `CopyResource`, the queue-level `ExecuteCommandLists` hook, and the device-level `CreateGraphicsPipelineState` / `CreateConstantBufferView` hooks — all diagnostic-only.

**`DllMutex.h` — `StfDllMutex`.** The StF DLL statically links the **VS2019 STL** and its embedded D3D12MA locks `std::mutex` objects that live *inside* the `D3D12MA::Allocator` YAMP builds and hands it at `cdevice+0x17B8`. It locks them with its own baked-in `mtx_do_lock` / `_Mtx_unlock`, which expect the classic `_Mtx_internal_imp_t` layout:

```
+0x00 int _Type (std::mutex == _Mtx_try == 2)
+0x08 polymorphic _Stl_critical_section — first qword is a VTABLE POINTER
+0x10 SRWLOCK
+0x48 long _Thread_id (-1 = none)
+0x4C int  _Count
```

Verified: `mtx_do_lock @0x1800C7304` calls `cs->vtable[0]` and `cs->vtable[0x10]`; `_Mtx_unlock @0x1800C7478` calls `cs->vtable[0x18]`; the icall goes through `0x1800D02D0` → a bare `jmp rax` (no CFG), so our callbacks are fine. **VS2022 17.10+ replaced that polymorphic critical section with an inline SRWLOCK**, so a `std::mutex` *we* construct stores a null where the DLL expects a vtable pointer and the DLL crashes at `stfDLL+0xC7378`. `StfDllMutex` reproduces the VS2019 layout with a 5-slot critical-section vtable backed by an SRWLOCK. No external dependency.

**Struct transcription bugs fixed in `de7796c`** (present identically in the LJ *and* Y6 copies):
- `cgs_shader_uniform::initialize` assigned `m_clip_far` twice; the first was meant for `m_clip_near`. `PatchGs` builds these with plain `new cgs_shader_uniform`, which does not value-initialise, so `m_clip_near` held allocator garbage (`0xCD` fill under the debug CRT).
- `ccontext_native::desc_st::reset` stored `0` into `m_height` instead of its `height` argument. Latent only — the sole caller passes 0.

### 8.4 `pxd/Y6` — the Yakuza 6 generation

`sl`, `gs`, `file_access`, `async_request`, `cs_game`, `sys_util`, `pxd_types`, `Imports.h`. Moved out of `source/vf5fs/Y6` where it *looked* like shared VF5FS code — the LJ and YLAD VF5FS hosts include none of it. Largely the baseline code, relocated and namespaced (`pxd`).

### 8.5 `pxd/K2` — the Kiwami 2 generation (sl only)

`pxd/K2/sl.h` is the authoritative layout, every offset pinned by `static_assert` and read out of the module's own accessors.

**The layout rule:** K2's sl context is **LJ's with `0x3C0` bytes inserted immediately before `handle_free_queue`**. Every field up to and including `sz_fs_root` sits at exactly the LJ offset; everything from `handle_free_queue` onward shifts by `+0x3C0` as one block. Three independent landmarks pin the delta, each read out of this DLL's own code:

| Landmark | LJ | K2 |
|---|---|---|
| `handle_create_internal` (`FUN_180066710`) free-queue pop | `sl+0x6C0` | `sl+0xA80` |
| `file_handle_create` (`FUN_180064550`) spinlock | `sl+0x1C00` | `sl+0x1FC0` |
| …file handle pool | `sl+0x1C08` | `sl+0x1FC8` |
| archive condvar | `0x1C20` | `0x1FE0` |
| per-type handle counters | `0x1800` | `0x1BC0` |
| heap | `0xEFD0` | `0xF390` |
| `size_of_struct` | `0xF000` | `0xF3C0` |

`0xF3C0 − 0xF000 == 0x6C0→0xA80 == 0x1C00→0x1FC0 == 0x3C0`.

**Why this needed its own header:** reusing `pxd::sl::context_t` for K2 does **not** crash — it fails *silently*, which is worse. The module's `file_handle_create` finds a pool the host filled `0x3C0` bytes too low, sees a zero count, returns a null handle, and every file open fails; resource loads come back as null blobs and the module faults far away (`DLL+0x6D190`, reading the SLLZ magic of a null buffer).

The **primitives** (`handle_t`, `file_handle_internal_t`, `t_locked_queue`, `t_fixed_deque`, the `csl_file_access` family) are byte-identical to LJ's and are reused, not cloned. `pxd::sl` gained **`p_sync_archive_condvar`** so a host can point `csl_archive::create_instance` at its own generation's lock word — K2 predates YLAD's recursive archive lock.

The K2 **gs** context (`0x202140`) does **not** carry over and is kept as an opaque block, with only the individual fields the module reads filled in.

---

## 9. The `m2ftg` host family (Model 2 arcade boards)

"m2ftg" = the module family name used by the parent games ("Model 2 fighting game"). Six modules across three engine generations share one protocol.

### 9.1 The module protocol (`source/m2ftg/m2ftg.h`)

Layouts reverse-engineered from LJ's host driver `FUN_142494450` and confirmed line-for-line against YLAD's symbolized `cscene_minigame_m2ftg::method_pre_render (0x1426a78b0)` + `cgame_module<m2ftg_module_t, …>`.

**`m2ftg_config_t`** — `module_start` copies `0x100C` bytes from `params+0x38` into its config global (`DAT_1801ed490`). Field names from YLAD's symbolized scene ctor; byte offsets corrected against the StF DLL's actual field readers (the settings fields are `u8` each, so `country` is `+0x05`, **not** `+0x08`):

| Off | Field | Meaning |
|---|---|---|
| `+0x00` | `uint32 kind` | `{0=vf2, 1=fv, 2=stf, 3=omg, 4=mr}` (LJ table order) — also selects `"%s/rom/%s_rom.par"` |
| `+0x04` | `difficulty` | 1 = normal → `FUN_1800529d0` → backup SRAM `0x1D03342` |
| `+0x05` | `country` | `{0=JAPAN, 1=USA "Sonic Championship", 2=EXPORT}` → game-assignments image: SRAM `0x1D03352` + working RAM `0x59C352` (both user-verified live) |
| `+0x06` | `is_acf_skip` | gates the `rom/sound/stf.acf` load |
| `+0x07` | `is_vf20` | VF2-only, zero readers in StF |
| `+0x08` | `is_disable_pepsi` | VF2-only |
| `+0x09` | `is_freeplay` | |
| `+0x0A` | `is_vs_mode` | LJ's "2P quick match": `FUN_180052ec0` force-inserts 5 credits into **both** coin counters (game RAM `+0x500248` / `+0x50024C` \|= 5) and skips the normal boot flow; `FUN_180052e50` picks a random stage 0..8; `FUN_1800529d0` writes SRAM mode byte 3 instead of 2. Clear = authentic arcade boot (attract, coin/start, 1P ladder). |
| `+0x0B` | `is_sram_restore` | `FUN_1800529d0` copies the `+0x0C` blob into emu SRAM`+0x3000` |
| `+0x0C` | `settings[0x1000]` | SRAM settings image (all zero in live LJ captures) |

**`m2ftg_execute_info_t`** — embedded at LJ scene`+0x13A0`; every offset verified there. **Must persist across frames** — the module keeps state in it.

```
+0x00   size_of_struct      (must be 0x1760)
+0x08   cgs_device_context*
+0x10   status
+0x14   result              (host presets 0x80004005)
+0x18   output_texid        (host zeroes)
+0x1C   sound_volume        (float; 0.0f mutes ALL audio)
+0x20   pad[0]              (m2ftg_pad_t = pxd::lj_pad_t, 0x190 each)
+0x1B0  pad[1]
+0x340..+0x1660             module-visible workspace, never host-written
+0x1660 work_kind           (indexes host volume table; LJ trophy code switches on it)
+0x1664 assign[2][8]        (host writes from settings; StF FUN_180003ac0 consumes)
+0x167C event_param         (rob id / stage id, alongside status event bits)
        sizeof == 0x1760
```

**`status` bits:** host→module bit0 = **pause**, bit5 (`0x20`) = **coin inserted this frame**. Module→host bit6 (`0x40`) = "insert coin / press start" screen active (`m_is_coin_wait`); bits `0x100`/`0x200`/`0x400`/`0x1000` = game events with payloads in the work block.

`assign_t`: `invalid=0, none=1, p=2, k=3, g=4, pg=5, pkg=6, pk=7, kg=8`.

**Generation deltas to this struct:** K2's `execute_info` is **`0x16E0`** with **plain `csl_pad` at `+0x20`, stride `0x170`** (not LJ's extended `0x190`), assigns at `+0x15E0`. Volume is still the `+0x1C` float in *both* — Lost Judgment's VF5FS module is the odd one out with a `0..20` byte at `+0x663`. Getting that wrong mutes every cue while the rest of the audio path works.

### 9.2 Symbol resolution (`m2ftg/ImportSymbols.h`, `m2ftg/LJ/ImportSymbols.cpp`)

One pattern table serves all four LJ/YLAD modules. Symbols:

`SL_CONTEXT_INSTANCE`, `GS_CONTEXT_INSTANCE`, `GS_CONTEXT_PTR`, `D3DDEVICE`, `SL_KERNEL_CALLOC`, `SL_FILE_{CREATE,OPEN,READ,CLOSE}` (file **write** is YAMP's own — deliberate), `SL_HANDLE_CREATE`, `SL_FILE_HANDLE_DESTROY`, `PRJ_TRAP`, `ARCHIVE_LOCK_{WLOCK,WUNLOCK}`, `DEVICE_CONTEXT_RESET_STATE_ALL`, `VB_CREATE`, `IB_CREATE`, `TRAP_ALLOC_INSTANCE_TBL`, `CDEVICE_CTOR`, plus:

- **`STF_FRAME_SUBMIT`** (`FUN_18003b530` = `FUN_18003a1e0` + tail-jmp `FUN_18003a540`) — `module_main` only *records*; in handler mode its inline submit stage is a no-op and **this** is the real submit, normally driven by the engine's render-system loop that YAMP doesn't run.
- **`STF_RENDER_EXECINFO`** (`DAT_1801ee4a0`) — the "live execute_info" global. `module_main` sets it on entry and clears it on return; `STF_FRAME_SUBMIT` dereferences it, so the host restores it around the call.
- **`I960_FETCH_EXEC`** (`FUN_1800255F0`) — the i960 CPU core's fetch/decode dispatcher. Its fetch is hard-wired to the program-ROM host buffer (`ctx->codeBase + IP`, no memory map), so code executing from emulated RAM — which the ROM's own debug menu does via a trampoline at `0x59F270` — reads past the 1 MB ROM image and crashes. Optional (`TryGetSymbol`).
- **`I960_IO_REFRESH_CALL`** — the CALL to the emulated Model 2 I/O board's per-frame refresh, at the top of the frame step (StF `FUN_180055760+0x25` → `FUN_18004D840`; FV `FUN_180053DC0+0x25`). Anchored on the whole prologue; payload is `match + 0x25`. **Optional and unique where it exists**: StF and FV share this shape; Motor Raid and the YLAD VF2 build do not.

### 9.3 `m2ftg/LJ` — the StF / FV / MR host

`GameDesc` in `LJHost.h` holds everything that differs between the three games; the entire hosting path is otherwise shared (the DLLs are near-identical builds of the same emulator: same protocol, same context sizes sl `0xF000` / gs `0x388A00` / ct `0x30`, same `execute_info` `0x1760`, same byte patterns — all re-verified against MR's own `initialize_module` checks).

```cpp
struct GameDesc {
    const wchar_t* dll_name;  const wchar_t* subdir;  const char* display_name;
    uint32_t kind;                       // m2ftg_config_t.kind
    const char* rom_archive_name;        // loose-ROM debug feature
    const wchar_t* const* rom_files;  size_t rom_file_count;
    uintptr_t rva_cpu_ctx_ptr, rva_opcode_table, rva_ram_base_ptr;  // i960 globals
};
```

| | StF | FV | MR |
|---|---|---|---|
| DLL | `stf-pxd-w64-d3d12_retail.dll` | `fv-pxd-w64-d3d12_retail.dll` | `mr-pxd-w64-d3d12_retail.dll` |
| `kind` | 2 | 1 | 4 |
| Archive | `stf_rom.par` | `fv_rom.par` | `mr_rom.par` |
| ROM images | `rom_code1, rom_data, rom_ep, rom_pol, rom_tex` | `rom_code1, rom_data, rom_ep1, rom_ep2, rom_pol, rom_tex` | `rom_code_tw, rom_ep1, rom_ep2, rom_data, rom_pol, rom_tex, rom_cop` (the DLL's own 7-entry `{path,size}` table order at `0x1802771A0`) |
| `rva_cpu_ctx_ptr` | `0x58A960` | `0x58CF60` | **0** |
| `rva_opcode_table` | `0x168630` | `0x166720` | **0** |
| `rva_ram_base_ptr` | `0x8F7CC8` | `0x8FA2C8` | **0** |

MR's RVAs are zero **on purpose**: its CPU core inlines fetch/decode into its execution *loop* (`FUN_18002CAC0`) instead of shipping the single-instruction dispatcher StF and FV have, so there is no equivalent hook point — and the RAM-exec patch only exists for StF's ROM debug menu. All-zero means the patch is skipped entirely.

**`Patch.cpp` responsibilities:**
- `ReinstateLogging` — `prj_trap` replacement. The pxd trap/log uses custom format directives (`%~`, `%(`) the CRT formatter rejects; in a Debug build `vsprintf_s` would raise "Incorrect format specifier" and kill the process. Format leniently (swallow invalid-parameter asserts via `_set_thread_local_invalid_parameter_handler`, fall back to the raw string) and **never** `__debugbreak`.
- `InjectTraps`, `Patch_SysUtil`, `Patch_CsGame`, `Patch_Misc`.
- `InstallRamExecFetch` — YAMP's region-aware reimplementation of the i960 fetch/decode dispatcher, so code in emulated work RAM can execute. Also the hook point for HLE hit counting and the i960 profiler.
- **`InstallSystemSwitches` / `SetSystemSwitches(test, service)`** — the cabinet TEST/SERVICE switches. The emulated I/O board keeps one byte of state (`io[9]`, the bank-0 copy; `io[0x0A]` is the DIP bank, hard-wired `0xFF`), **active low**, laid out like the hardware. `read_sw` folds that byte into the low 8 bits of the ROM's flag longs at RAM `0x500700` (held) / `0x500704` (momentary), inverted — which is why the DLL's `ADV_DSP` handler can fake "both players pressed Start" by writing `0x30` straight to `0x500704`. The DLL rebuilds `io[9]` from scratch once per emulated frame and drives only coin 1 (from the `execute_info` coin bit, via a one-shot flag at `io+0x4098`) and the two Start bits; TEST and SERVICE have **no host source anywhere in the module protocol**. Since nothing about `io[9]` survives a frame, a host write between `module_main` calls is simply overwritten — so YAMP intercepts the CALL to the refresh and pulls the two lines low **immediately after it**, before the frame's first i960 instruction runs. The I/O-state global is decoded out of the instruction at `match+0x2B` (StF → `0x1806C9B88`, FV → `0x1806CC188`) rather than hardcoded per DLL, which is what keeps it correct under ASLR.
- **`FixBackupRamTimeIndex(dll)`** — corrects one byte of the backup-RAM block the module injects, without which the service menu's **GAME ASSIGNMENTS** page hard-freezes the whole application. This is a bug in SEGA's module, not in YAMP; it only became reachable once `InstallSystemSwitches` made the TEST switch work, which is why Lost Judgment itself never hit it. The chain is worth stating in full because it is the clearest example of the emulator's most dangerous divergence from hardware:
  - The page's value column is drawn by `game_assign_bk_ram_thing` (ROM `0x637C8`). For an item whose flag word has bit 8 set and bit 2 clear, it uses that item's backup-RAM byte as a **raw index with no bounds check**: `r9 = item[0x10]; g0 = r9[value * 4]; print_mes(g0)`.
  - The `TIME` item reads backup `+0x3351` (`time_var_array_num`), which indexes a **10-entry** table at ROM `0x5E088` — the display strings `"10"`…`"99"` for `time_vars[]` at ROM `0x8F3C4` (`0A 14 1E 28 32 3C 46 50 5A 63`). The ROM's own `init_game_assignments` (`0x626A4`) writes **2** there, and the page's edit handler `ga_TIME_sub` (`0x5D648`) clamps the field to 0–9.
  - The module's injector writes **`0x1E`** — the time in *seconds*, not the index. Index 30 reads 80 bytes past a 40-byte table, into i960 code, and yields the pointer `0x8C703000`.
  - **That guest address is unmapped, and an unmapped read in this emulator leaves the destination register *unchanged*** (the memory map's miss slot is a bare `ret` stub) rather than returning zero or bus garbage. So `print_mes`'s `ldib` returns the same non-zero byte forever and its stop-at-NUL loop can never terminate. On real hardware the read would eventually hit a zero, or the watchdog would reset the board — **this hang is created by the emulator.** Treat "unmapped reads are inert" as true only of *crashing*: any ROM loop that scans for a terminator will spin instead.
  - A ROM spin freezes **all of YAMP**, not just the game: the i960 only returns control at the HLE'd `main_loop` yield and the `interrupt_wait_b` traps (§12.2), so `module_main` never returns and the host frame pump is starved. Symptom is a fully unresponsive window with a busy CPU.
  - The fix patches the injector's immediate `0x1E` → `0x02` at load, anchored on a pattern covering the neighbouring `TST_*_ADD` / `TST_*_MUL` constants (themselves confirmed against `init_game_assignments`), and verifies the byte still reads `0x1E` before writing. Corrected **at the source** rather than by re-asserting the backup byte per frame, because that byte is the operator's setting and the service menu must stay free to change it. A module that does not match — a different m2ftg build, or a future one SEGA fixed — logs and keeps its own value. `TIME` is the only one of the eighteen items whose injected value is out of range; every other byte in the block matches the ROM's defaults exactly, which is why only this one page locked up. Gated by the `FixBackupRamTimeIndex` setting (§15), default on.
  - **The byte has two more consumers, and fixing it alone is not enough.** Both are HLE hooks, and both read backup `+0x3351` as **raw seconds**:
    - **Hook 16** (`GAME_INT+0x4`, handler `+0x52CA0`) copies it **raw** into `time` (RAM `0x500090`) on every game interrupt, then runs the original instruction. Those are different units: `time` is the round length in **seconds** — the ROM's own writers put a literal `30` there (`0x6CEC`) and its debug setter clamps to `0..0x63` — reached from the index via `time = time_vars[+0x3351]` in `main` (`0x7200`). Because the hook fires continuously it always beats `main`'s one-shot conversion, which is *why* the module could ship a seconds value in an index field and never notice. With the injector corrected, that same hook forces `time = 2` — **≈2-second matches**.
    - **Hook 17** (`ADV_REPLAY_WAIT1A+0x128`, handler `+0x52CD0`) sets the **attract demo's** round timer: `game_timer = min(backup +0x3351, 30) * 64` (RAM `0x500028`, a 16-bit **1/64-second** counter). It *replaces* the ROM instruction at `0x96AC`, which is `shlo 7, 0xF, r15; stis r15, game_timer` — a **hardcoded 1920 = 30 seconds**. So the arcade board always runs a 30-second demo whatever the operator's TIME setting says, and this hook exists purely to make the demo follow that setting instead. With the injector corrected it computes `2 * 64` — **a 2-second demo, both fighters still at full health with `01.73` on the clock**, which is exactly how it was found. Disabled, the ROM writes its own `1920` and the demo is 30 seconds again, which is also the authentic behaviour.
  - This one took a while to pin down because the DLL reaches the byte as `[rax + r8 + 0xAFB031]` — base + index — so it does **not** register as a cross-reference to that address in a decompiler, and a literal-address search finds only hook 16. The i960 side is no better: the ROM's only literal references to `0x59C351` are `ga_TIME_sub` and `init_game_assignments`, because everything else addresses the block through its base at `0x59C320`. What settled it was a live experiment — writing `0x1E` back into work RAM `0x59C351` on a running board made the *next* demo start at 30 — plus ruling out an accidental table lookup, since `time_vars[0x1E]` reads `0x80` = 128, not 30.
  - YAMP therefore ships **hooks 16 and 17 disabled by default** (`HleHooks::DEFAULT_DISABLE_MASK`), which hands both timers back to the ROM. Neither hook has any other effect, so disabling them costs nothing.
  - The three parts are one fix and are documented as such in both tooltips and in each hook's own description, because **fix on + either hook on is the combination that breaks a timer** (matches for 16, the attract demo for 17) and it is otherwise invisible — nothing errors, the clock just starts at the wrong number. All three off is the module's stock behaviour: working timers and a page that freezes the emulator. The hook-list preset formerly labelled "Disable none" is now **"Restore defaults"** and sets `DEFAULT_DISABLE_MASK` rather than `0`, so the preset cannot silently re-arm a bad combination; each hook's own checkbox still re-enables it in one click.
  - **Existing profiles do not pick this up automatically.** `DisabledHleHooks*` is an explicit ini value and an explicit value always beats the default, so a `settings.ini` written before hook 17 joined the mask carries bit 16 only and keeps the 2-second demo until **Restore defaults** is pressed or those two lines are deleted.
  - `DEFAULT_DISABLE_MASK` is also what the netplay override restores instead of `{0,0}`. That does not weaken the determinism argument in §14.8 — it is a compiled-in constant, not a setting, so peers on the same build agree on it without exchanging anything, which is exactly the property the all-hooks-restored rule buys. Restoring hooks 16 or 17 there would only make both peers agree on a two-second timer.

**Presentation helpers (`m2ftg/HostUI.h`)**, shared with the VF2 host: `ApplyAspectSetting(window, mode)` (0 = 4:3 pillarboxed, 1 = 16:9 stretched, 2 = fill window; applied live) and `DrawPauseMenu(window, menuOpen)` mirroring Lost Judgment's — the host draws the menu shell while the module's own pause path (`execute_info` status bit0) freezes emulation. Returns false on Quit.

### 9.4 `ModuleArgs` + `DisplayModes` — Model 2 render resolution

Every m2ftg module carries pxd's own command-line option parser, reachable from `module_start` via the app module's init:

```
module_start -> AppModule::vftable[0] -> AppInit -> ParseArgs(argc, argv)
```

`module_start` **hardcodes `argc = 0` / `argv = nullptr`** into the globals `AppInit` forwards, so the parser's first test (`if (argv != nullptr)`) fails and no option was ever read. The params block YAMP fills has no argv field — the DLL simply never asks the host for one.

`ModuleArgs::Install(dll)` re-points that one call at YAMP.exe's own `argv`, anchored on the instruction pair that presets the default display mode (**unique in `.text` in all five modules**: stf/fv/mr/vf2/omg). Call after `LoadLibrary`, before `module_start`. Idempotent. Returns false when there is no parser call site, which leaves the module at its default 1024×768.

`ModuleArgs::ResolvedRenderSize(w,h)` reads the module's **own** resolved display-mode global after `module_start`, not the YAMP setting — so an explicit switch on the command line is reflected.

Only the resolution switches still drive anything in a retail module; `-debug`, `-m`, `-s`, `-ve`, `-fs`, `-aa`, `-ss`, `-ss4x` parse into globals with no reader left (the same stripping that hollowed out the dw debug menu's handlers).

**`DisplayModes.h`** mirrors the module's 17-entry table (byte-identical in all five modules). Critically: **the mode does not change the output texture** — measured live at modes 2, 6, 15 and 16, the composited texture is 1024×768 every time. The mode only moves the *viewport* the module draws with inside that fixed target. Therefore:

- Smaller than 1024×768 → picture in the top-left sub-rect → `RenderWindow::SetModuleSourceRect` restricts the blit UVs.
- **Larger than 1024×768 → draws past the target and is clipped, losing the right and bottom of the frame permanently.** Checked rather than assumed: `-wxga/-wxga_dbd/-wxga2/-sxga/-uxga` were briefly added back and run, and they do crop — the module clips, it does not rescale its projection. Those rows are **not offered**.

Offered modes: `""` (module default 1024×768), `-model2` (496×384), `-model2x2` (992×768), `-vga` (640×480), `-svga` (800×600), `-wvga` (800×480), `-wsvga` (1024×600).

Persisted as the **switch text**, not an index, so reordering the list cannot silently change what an existing ini means. An unknown switch (including an oversized mode left in an old ini) falls back to the module default. `IntendedDisplayMode(settingIndex)` resolves the mode **before** the module is loaded (explicit command-line switch wins over the setting), because the window has to be sized from it and the window exists long before `module_start` runs the parse.

`Model2WindowMatchesRender` sizes the window to the picture — the render height at the board's 4:3 display aspect, so 496×384 gives a 512×384 window and the ordinary aspect-correct viewport fills it. Ignored in fullscreen and for the VF5FS builds.

### 9.5 `m2ftg/YLAD` — Virtua Fighter 2 (Like a Dragon, DX11)

Module `vf2-pxd-w64-retail.dll` from `runtime/media/m2ftg`. The DX11 build of the same emulator family, hosted with the same m2ftg protocol. The sl layer matches Lost Judgment exactly (tag `'LBsl'`, version `0x40601`, size `0xF000`), so the sl reconstruction is reused wholesale.

Key DLL facts (base `0x180000000`):
- `module_start @0x18005d500`, `module_stop @0x18005d780`, `module_main = 0x18005d8a0`
- `sl init FUN_180065e00`: needs `{size 0x10, ctx+8 == 0xF000}`; embedded sl ctx `@+0x187100`
- `ct init FUN_1800afad0`: needs `{size 0x10, ctx+8 == 0x30}`
- `gs init FUN_180089220`: needs `{size 0x58, ctx+8 == 0x3820C0}`; embedded gs ctx `@+0x196a40`, then `import_shared_symbols(ctx+0x20) = FUN_180092440` consuming **six** slots (named via YLAD's `sbgl::cdevice::export_shared_symbols @0x140245e70`):

```
[0] g_pD3DDevice (ID3D11Device*)   [1] g_p_device_native (sbgl cdevice)
[2] g_p_swap_chain                 [3] g_FeatureSupport (dword, BY VALUE)
[4] g_p_num_swap_chains (int*)     [5] g_p_allocator (pxd allocator object)
```

### 9.6 `m2ftg/K2` — Virtua Fighter 2 + Virtual On (Kiwami 2 GOG, DX11)

Two modules built **one second apart** on 2019-09-30 — the same engine build, so they share one host and differ only in `GameDesc`:

| | VF2 | "omg" = Virtual On |
|---|---|---|
| DLL | `vf2-pxd-w64-gog_retail.dll` | `omg-pxd-w64-gog_retail.dll` |
| `kind` | 0 | 3 |
| netplay | lockstep (§14.3-14.9) | **linked-cabinet (§14.10)** — the ROM's own link on the wire |

The ROM-name table `module_start` indexes with `config.kind` is at `0x18015DF9C..` and reads `{0:"vf2", 1:"vf", 2:"stf", 3:"omg", 4:"mr"}` — **a different order from the Lost Judgment table** (where 1=fv). Combined with `"%s/rom/%s..."` it picks `m2ftg/rom/<name>_rom.par`, which is what Kiwami 2 ships.

Host-relevant differences from LJ and YLAD, each found by **running and reading the actual fault** rather than extrapolating:

- `module_main` is neither exported nor returned through params — YAMP **pattern-scans** it. (The only difference between the two modules is one spill-slot byte in `module_main`'s prologue, now wildcarded.)
- `module_start` is handed **only** `params+0x20` (ICriWare) and `params+0x38` (the `0x100C` config). No sl/gs/ct module blocks — the module owns its own contexts.
- `execute_info` is `0x16E0`; volume is the `+0x1C` **float**.
- The **sbgl shadow block**, attached to the `ID3D11DeviceContext` by `SetPrivateData`.
- `cgs_device_context` `+0x28`/`+0x30` cb/up pools and `+0x38` render-state block.
- `p_ib_quad` / `p_ib_fan` at `gs+0x1418` / `gs+0x1420` — `primitive_initialize` is the host's job here too.
- The archive rwspinlock pair (this generation predates YLAD's recursive one — hence `pxd::sl::p_sync_archive_condvar`).
- Input: plain `csl_pad` at `execute_info+0x20`, stride `0x170`, assigns at `+0x15E0` — all read out of the module's own pad reader `FUN_18005E410`. Size gate verified both statically (`CMP RCX, 0x16E0` at the top of `module_main`) and live under x64dbg.

**Which dip switches Virtual On actually honours.** The two modules share `m2ftg_config_t`, but they do not read the same fields out of it, and the settings panel had been assuming they did. Counting direct references into the module's config block at `0x7A7FB0`:

| field | refs in `omg` | |
|---|---|---|
| `+0x00` kind | 1 | picks the ROM name |
| `+0x04` difficulty | 1 | **honoured** — see below |
| `+0x05` country / region | **0** | never read |
| `+0x09` free play | 1 | honoured |
| `+0x0B` sram | 1 | |

**Difficulty is honoured, through a translation table rather than directly.** The module reads `config+4`, adds one, and indexes a five-entry table (`{3,3,0,1,2}` at RVA `0x4500E0`) to produce the value it injects into backup RAM `0x1D00021`, which the ROM's own operator menu decodes as `0=NORMAL 1=HARD 2=VERY HARD 3=EASY`. So YAMP's `m_m2Difficulty` 0..3 lands on EASY / NORMAL / HARD / VERY HARD in order — its four labels, exactly. Confirmed live: `Difficulty=1` shows NORMAL on the cabinet's GAME ASSIGNMENTS page.

> Worth noting how nearly this was "fixed" into being wrong. Read statically, the table looks like it is indexed by the raw config value, which makes the mapping appear off by one; and the byte feeding it looks like a module private until the RIP-relative displacement resolves to `CONFIG_BASE + 4`. A screenshot of the cabinet's own menu is what settled it. **On a question like this the game's operator page is the oracle, not the disassembly.**

**Region does not exist on this game at all**, which is why the combo is hidden for it (`YAMPUserInterface.cpp`). Six independent checks agree: the module never reads `config+5`; the ROM contains no region string; the operator menu has no region item (PLAY TIME, MATCH COUNT, NETWORK LINK ATTRIBUTE, WINNING BY DECISION, GAME DIFFICULTY, ADVERTIZE SOUND, CONTINUE, REPLAY AND POSING, RANKING, VERSUS ALWAYS FINISH, DISPLAY BRIGHTNESS, INITIALIZE, EXIT is the whole list); no byte of backup RAM `0x1D00000..0xA0` holds one; the Warning-notice handler has no region test; and the ROM signature `SEGA.AM#3.VRON.` carries no region code. The program ROM's text is already English (`THIS VERSION IS NOT A SIMULATORY SYSTEM.`, `BEAM RIFLE`, `GAME ASSIGNMENTS`) — SEGA shipped region as separate ROM *sets*, and this is the set Kiwami 2 bundles.

> **The boot notice is not a region tell.** `0x5024D4` — the "notice already shown" flag — has exactly three accesses in the whole ROM: the Warning handler reads it (`0x3C6C`), the Warning handler sets it after its 564 frames (`0x3D44`), and `BlackOut` clears it (`0x1871C`). Nothing else writes it and nothing gates it. The only way it is ever pre-set is **HLE hook 5**, which replaces `BlackOut`'s clearing store with a write of 1 — a module hook, not a ROM behaviour, and disabled by default so the arcade boot runs.

**Still unchecked:** Model 2 boards commonly carry a country jumper on the I/O board, read through the I/O port window rather than backup RAM or the config block. That would sit outside all six checks above. If a Japanese *graphic* ever needs explaining, that and the texture ROMs are where to look — not the program ROM.


### 9.7 `pre3` — the Model 3 host family (a sibling of `m2ftg`, not part of it)

> Numbered inside §9 to avoid renumbering §10-18. It is a **separate family**: a different arcade board, a different module, a different protocol. Its own reconnaissance lives in `docs/pre3-model3-support.md`, `docs/pre3-netplay.md`, `docs/pre3-hle-hooks.md`, `docs/src2-hle-hooks.md` and `docs/src2-netplay-recon.md`; only what §14.11 needs is summarised here.

**One module, six games, two reachable.** Like a Dragon Gaiden ships `pre3-pxd-w64-d3d12_retail.dll`, a **Model 3** emulator (PowerPC 603e + Real3D), on the same LJ `pxd` generation as `m2ftg/LJ` — so it reuses the whole `pxd/LJ` platform layer and the DX12 renderer, and only the arcade half is new. Of its six ROM classes, `-fv2` (Fighting Vipers 2) and `-src2` (Sega Racing Classic 2, the Daytona USA 2 engine) are playable from a stock install.

| | Fighting Vipers 2 | Sega Racing Classic 2 |
|---|---|---|
| Netplay mechanism | **lockstep** (§14.3-14.9) | **linked cabinet** (§14.11) |
| ROM class | `M3ERomFv2` | `M3ERomSrc2`, which *embeds a real comm board* |
| Round start | the module's own `vs_start.bin` savestate | n/a — cabinets link at boot and stay linked |

**`source/pre3/`** — `Gaiden/Pre3Host.cpp` (the host loop), `HleHooks` (decoded-trace substitution, **not** ROM patching), `Determinism`, `CommBoard`, `ArcadeSettings`, `SystemSwitches`, `SecurityBoard`, `BoardVtables`, `Patch`, `NetSession`.

Three pieces §14.11 depends on directly:

- **`Determinism`** owns the machine. `IsBoardBooted()` (phase word `machine+0x88` reaching `0x10`), and two different resets driven through the module's own request bitfield at `machine+0x120`, which the frame step drains at the top of every frame: bit 4 restores a **preloaded** state (FV2's shipped VS start), bit 0 saves and bit 1 restores YAMP's **power-on snapshot**. That snapshot is requested on the first host frame that sees the machine RUNNING — the board has reached its terminal phase but **has not stepped a single guest frame** — so what it captures is the pristine pre-boot machine, and restoring it makes the guest re-run its entire boot chain. Requested unconditionally for every pre3 game, because making *when* it was taken depend on whether a session happened to be up is exactly what must not vary.
- **`ArcadeSettings`** writes the board's GAME ASSIGNMENTS rows the module's own injector cannot reach, into both copies (working at guest `0x100180`, NVRAM at `0x72629C`). It applies once and then **latches**, leaving the rows to the game's service menu — `Reset()` re-arms it.
- **`SystemSwitches`** funnels TEST/SERVICE and the 8-channel ADC ring through `M3EInput::read_port`, sampled at read time, which is also what the hardware does.

**The emulator runs on its own thread** (`m3e_ctrl`), reachable only via `machine+0x148`. Every host-thread read of board state races it; `WaitForEmulatedFrame` is the only coherent sampling point, and **using it unconditionally is itself a bug** — it puts the host loop in lockstep with the worker and took a board from ~395 draws a frame to 8.

---

## 10. The VF5FS hosts

`source/vf5fs/vf5fs.h` holds the **shared** module_start protocol; each host keeps only its own generation's structures.

**`game_config_t`** (`params+0x38`, 8 bytes shared core):

```cpp
uint16_t energy; int8_t round, time, diff;
int8_t game_mode;   // +0x05. LJ: 0/1/2, drives two derived globals in module_start
int8_t lang;
bool is_triangle_start : 4;  bool is_dural_unlocked : 4;
```

LJ's `module_start` loads **16** bytes (one `VMOVUPS` of `params+0x38` into its config global `0x18054D4A0`) but decodes only the same first 8. The upper 8 (`lj_game_config_t::unknown`) are copied through but have no decoder — unknown-but-preserved.

**`module_params_t`** — templated over the config and module-block types; identical in both generations otherwise:

```
+0x00 size    +0x08 sl module   +0x10 gs module   +0x18 ct module
+0x20 ICriWare implementation   +0x28 root path (UTF-8, copied out, max 0x103 bytes)
+0x30 out: module_main pointer  +0x38 config
```

Each `*_module` block is `{size_t size; context*}` and is size-checked by the module's own `initialize`. LJ: sl `0x10`/ctx `0xF000`, gs `0x58`/ctx `0x388A00`, ct `0x10`/ctx `0x30` — the same values the m2ftg LJ modules require.

**Shared `execute_info` header** (the pxd `base_execute_info_t`):

```
+0x00 size (module rejects a mismatch: Y6 0x320, LJ 0x690, m2ftg 0x1760)
+0x08 cgs_device_context*   +0x10 status   +0x14 result   +0x18 output_texid
+0x1C sound_volume   (0.0f mutes ALL audio — this was the VF5FS mute bug)
status bit0 = pause (host -> module)
```

| Host | DLL | Location | Generation | Notes |
|---|---|---|---|---|
| `vf5fs/Y6` | `vf5fs-pxd-w64-Retail Steam.dll` | `<install>/vf5fs/` (flat) | Y6 | DX11on12; the baseline host, moved + namespaced |
| `vf5fs/LJ` | `vf5fs-pxd-w64-d3d12_retail.dll` | `runtime/media/vf5fs/` | LJ | DX12. `module_start @0x1EDEF0`, `module_main @0x1EDCC0`, `module_stop @0x1EE3D0`. Uses `pxd/LJ` in full (`PatchGs`, `HostCdevice`, per-frame submit/frame-stamp). `execute_info` `0x690`; **volume is a 0..20 byte at `+0x663`**, not the `+0x1C` float. |
| `vf5fs/YLAD` | `vf5fs-pxd-w64-retail.dll` | `runtime/media/vf5fs/` | YLAD | DX11. **VF2's engine generation running the LJ module protocol.** Uses `pxd/LJ`'s *sl* only; its gs context is the module's own embedded `0x3820C0` template, handled as an opaque block (`FillSharedSymbols`). `ct init FUN_1802297E0` checks only `{0x10, ctx+8 == 0x30}`. |

**Both LJ and YLAD DLLs are built `DYNAMIC_BASE` (ASLR)** — resolve everything against the runtime module base.

All three VF5FS hosts share the m2ftg titles' P/K/G control scheme, so they share `source/input` bindings; the YLAD one additionally reuses `m2ftg/HostUI` for pause/aspect rather than duplicating it. The VF5FS modules do their own remap of the same button bits, so `MODULE_ASSIGN` does not apply to them.

---

## 11. CRIWARE audio

Replaces the baseline's `CriStub` (which returned success and did nothing). ~3,400 lines across `source/criware/`.

### 11.1 `Cri.{h,cpp}` — the `icri` implementation

Implements the full `icri` vtable the modules call (the `criAtomEx*` player/ACB surface, plus the `criMana*` movie surface which remains stubbed). Hands allocation through to `cri::atom::Alloc/Free`, which **must return real memory** — the game builds player and ACB work buffers with it.

### 11.2 `AtomEngine.{h,cpp}` (1,542 lines) — clean-room CRI Atom playback

Scope: the cue-based path StF/VF2/FV/MR actually uses —
`criAtomExAcb_LoadAcbData` → player `SetCueName(acb-or-NULL, name)` → `Start`/`GetStatus`/`SetVolume`/`Pause`/`Stop` — with HCA waveforms resolved from the ACB's `@UTF` tables: in-memory SFX from the ACB's internal `"AwbFile"` AFS2 blob, streamed BGM from the sibling `.awb` on disk (located by matching the loaded ACB bytes against `rom/sound/*.acb`).

Semantics follow the CRI runtime in Yakuza 6: **registry insertion order** for NULL-acb cue lookup, synth `ReferenceItems` fallback list, first non-zero wins.

Also supports the VF5FS non-cue path: `PlayerSetFile(path)` / `PlayerSetData(blob)` with ADX-or-HCA auto-detection (the file's own header is authoritative over the game's format/channel/rate hints).

Output: **XAudio2**, one mastering voice, one source voice per player (recreated on format change). Waveforms are **fully decoded to PCM16 at `Start()`** — the largest BGM is ~25 MB of PCM and the decoder does ~6k frames in well under a second. HCA loop points map to the `XAUDIO2_BUFFER` loop region. *Known limitation:* encoder delay is not trimmed, so loops are accurate to within the ~45 ms delay.

### 11.3 `HcaDecoder.{h,cpp}` + `HcaTables.h` (~1,225 lines)

Clean-room HCA (CRI ADX2 "High Compression Audio") decoder. Originally reconstructed from the CRI runtime statically linked into Yakuza 6 (Ghidra, image base `0x140000000`, per-stage source functions cited in the .cpp), then finished and validated **stage-for-stage against the ClHcaSharp reference decoder**. Constants in `HcaTables.h` are bit-exact.

Format facts verified against the shipped stf/vf2/fv/mr AWB assets:
- Container: AFS2 (`.awb`) or the in-ACB `"AwbFile"` blob; each entry is a standalone HCA stream.
- Header: `'HCA\0'` + chunk list; **all tags compared with the high bit masked off (`0x7F`)** so the header survives header-encryption. CRC16 over the header must be 0.
- Frames: fixed `blockSize` bytes, `blockCount` total, 1024 samples/channel per frame (8 sub-frames × 128), CRC16 over the frame must be 0.
- All shipped data is **ciph type 0** (unencrypted) — no key handling implemented. All streams are v1.03 `dec` / v2.00 `comp` with zero stereo bands, no HFR groups, `minResolution` 1 — so the intensity/MS-stereo, high-frequency and noise-substitution stages are implemented but never exercised.

### 11.4 `AdxDecoder.{h,cpp}`

CRI ADX, standard 4-bit ADPCM (encoding type 3) — the format VF5FS streams its BGM and voice in. Big-endian header; each 18-byte block holds a 16-bit scale plus 32 4-bit deltas for one channel, blocks alternating channels; the two prediction coefficients derive from the header's highpass cutoff. Encrypted files (flags `0x08`/`0x09`) are rejected. **Validated sample-exact against ffmpeg's decoder** on the shipped VF5FS files.

### 11.5 Status

Audio is **solved and user-verified in both StF and VF5FS**: HCA 173/173 and ADX 632/632 bit-exact against the oracles. `sound_volume` being left at `0.0f` was the VF5FS mute bug.

---

## 12. Homebrew Model 2B ROM hosting

Lets the Lost Judgment m2ftg module run a homebrew i960 program instead of Sonic the Fighters.

### 12.1 `m2ftg/ELF/ElfRom` — ELF program-ROM override

Drop **`game.elf`** beside the loose ROM images and it replaces `rom_code1.bin`; the `.bin` need not exist.

- `PT_LOAD` segments are flattened by **`p_paddr`, NOT `p_vaddr`** — `.data`'s initial image lives in ROM and is copied to RAM at boot — with `0xFF` fill, padded to `PROGRAM_ROM_SIZE = 0x100000` (StF, FV and MR all give the i960 a 1 MB image and the DLL reads exactly that much).
- Verified byte-exact against a toolchain-produced cart image (`roms/bin/apps/pengo`).
- Served through the module's existing file path via a `RomOverride` check at the top of `csl_file_access` (`m2ftg/file_access.cpp`), so the DLL's loader and boot state machine are untouched. Memory-backed handles use a sentinel `HANDLE` of `-2`, distinct from `INVALID_HANDLE_VALUE` (`-1`), so existing "is this handle usable" tests keep working.
- Symbols are parsed too: `ResolveSymbol("sym+0x1c")` and the reverse `SymbolizeAddress(addr, name, off)` for the 960STAT call stack.

**Gating fix** (`1796401`): `game.elf` is parsed **only when loose ROM files are enabled**. It reaches the i960 solely through the loose-ROM path, so loading it regardless left `ElfRom::IsLoaded()` true while the game ran from the `.par` — which relabelled Sonic the Fighters' own ROM with a homebrew's symbols in the 960STAT panes (every address past its `_etext` collapsing onto that one marker) and pointed the HLE retarget at a program that was not executing.

### 12.2 `m2ftg/LJ/HleHooks` — the 76 HLE ROM hooks

Sonic the Fighters' module **does not run the arcade ROM unmodified**. During board bring-up it overwrites **76 individual i960 instructions** in the program ROM image with a trap word, each dispatching to a native x64 handler inside the DLL.

The installer (`FUN_18004B070`, board bring-up stage 2) does, for every record whose `romOffset < 0x200000`:

```c
savedWords[i] = *(uint64*)(romBase + romOffset);        // original instruction(s)
*(uint32*)(romBase + romOffset) = 0x4000000 | (i * 4);  // trap word
```

The trap word's low bits are the record index. Restoring the ROM word from `savedWords` therefore un-does a hook completely and reversibly, **at any time**.

Key RVAs (StF, fixed base `0x180000000`): table `0x1E8870` (76 × `{u32 romOffset, u32 pad, u64 handler}`, in `.data` — writable, no `VirtualProtect`), saved words `0x68E540`, ROM base `0x9F7CD0`, boot state `0x6B9300` (2 = booted), ROM size `0x100000`, trap opcode `0x4000000`. Handler tails: `0x39D0` = "execute the saved original", `0x3A70` = "return only its length" (skip it). Sites symbolised with the module's own 800-entry ROM symbol table at `DLL+0x1742D0` (AM2's naming).

**Classification** (`HleHooks::Kind`):

| Kind | Meaning | Safe to disable? |
|---|---|---|
| `Core` | Frame yield, vsync wait, render sync, self-test bypass, texture-upload timeout | **No** — hangs or black-screens |
| `Host` | Audio bridging, input, backup RAM / arcade settings, progress reporting | Boots, but loses the feature |
| `Content` | Hidden character select, Honey's portraits and head tilt, VS-mode rules, attract timings | Yes — gives plain arcade behaviour |
| `Removed` | Handler is the bare "skip original" tail — the instruction is deleted | Yes |
| `Inert` | Handler is a bare jump to "execute original" — debug probes compiled out of retail | Yes (changes nothing) |

`MODDING_KINDS = Content | Removed` is the preset a ROM modder wants. **`SESSION_ONLY_KINDS = Core`**: disabling a Core hook hangs the board, and the hang takes the settings UI with it — a Core bit that survived into settings.ini would make YAMP unbootable with no way back. Core bits are therefore stripped on both save *and* load, so they can be ticked live but a restart always recovers.

`HleHooks::Update()` is called once per frame; it restores or re-applies each hook to match the setting, working live off the DLL's own save area. The setting does not default to "nothing disabled": `DEFAULT_DISABLE_MASK` ships **hooks 16 (`GAME_INT+0x4`) and 17 (`ADV_REPLAY_WAIT1A+0x128`)** off, because both contradict the backup-RAM TIME correction — 16 collapses match length, 17 the attract demo. See §9.3.

### 12.3 Pre-install retargeting

For a program ROM that is **not** Sonic the Fighters, the installer still stamps all 76 traps at StF's addresses — 36 of them land in live code and corrupt it before the i960 executes a single instruction. `Update()` cannot undo that in time: it runs from the UI draw, a full `module_main` behind the first frame.

The fix is to change **what the installer is told to patch, before it runs**. The `.data` table is rewritten ahead of `module_start`; the installer skips any record with `romOffset >= 0x200000`, so a rewrite either drops a hook or moves it. Nothing is corrupted and there is no race, because the trap for a suppressed hook is never written at all.

Per-hook retarget values: `0` = leave the DLL's own offset alone (offset 0 doubles as "no change" because it is the i960 initial boot record, never a hook site); `RETARGET_SUPPRESS` (`0xFFFFFFFF`) = never install; anything else = install at that ROM offset.

`ResolveRetarget(text, i)` accepts: `""` → 0; `"off"`/`"none"`/`"-"` → suppress; `"1A2B"`/`"0x1A2B"` → that offset; `"geo_wait"`/`"geo_wait+8"` → ELF symbol + hex offset. An unknown name is **reported and treated as "leave it alone"** — guessing an address would corrupt the ROM.

**The convention: a ROM that declares its own hook sites.** Hand-written `[HleRetarget]` offsets rot on every relink and fail *silently* — hook 1 on no site at all is a black screen with every counter healthy; hook 2 on an init-only site is a 98 % spin at 2 fps with no error anywhere. A homebrew ELF can instead name its sites:

```cpp
{ "__yamp_hook_composite_enable", 1,  0 },
{ "__yamp_hook_frame_yield",      2,  0 },
{ "__yamp_hook_geo_wait",         3,  0 },
{ "__yamp_hook_geo_wait",         4,  8 },
{ "__yamp_hook_vblank",           5,  0 },
{ "__yamp_hook_vblank",           6,  8 },
{ "__yamp_hook_vblank",           7, 16 },
{ "__yamp_hook_rand",            33,  0 },
```

**These byte offsets are the handlers' wire contract, not decoration:**

- `frame_yield` **replaces** its instruction and returns the real length → needs a sacrificial 4-byte instruction on a site reached **every frame**.
- `geo_wait` hook 4 returns a hardcoded `8` → must be followed by exactly two 4-byte instructions it can skip (`+0` is the 8-byte MEMB load for hook 3).
- `vblank` hooks 5/6 write `g0` and `r3` specifically, 8-byte MEMB loads apiece, with hook 7 on the 4-byte compare that closes the spin.
- `composite_enable` is **additive** (the original still runs) → no sacrificial slot; any instruction executed once at the end of init will do.
- `rand` is **whole-function HLE**: the handler writes the host RNG into `g0` and performs the i960 `ret` itself — unwinding the register frame through PFP/FP — then returns 0 as its IP delta because it has already set the IP. Its site must therefore be the **first instruction of a real, called function** (a bare `ret` body suffices), and it must not be inlined away: with no `call` there is no frame for the handler's return to unwind.

Resolution order: an explicit `[HleRetarget]` line always wins; otherwise the convention symbol. When the ELF declares **any** convention symbol it is taken as opting in, and every hook it did not name is **suppressed** rather than left at StF's address. An ELF with no convention symbols behaves exactly as before.

### 12.4 Invocation counters

Whether a retargeted hook actually *runs* is the hardest thing to observe from outside — the flags its handler sets are consumed and cleared within the frame. The count is taken in `RamExecFetch::FetchExec` (YAMP's own reimplementation of the fetch dispatcher), so nothing in the module is patched to get it. Only a trap word reaches the counter, and trap words are only fetched when the CPU really executes one — a `0x04` byte in a data table is never fetched as an instruction and cannot inflate the count. Motor Raid has no dispatcher to reimplement, so counts stay at 0 there.

Health checks in the settings pane catch the two otherwise-invisible failures: **hook 1 never firing** (composite disabled — black screen, every counter healthy) and **hook 2 below frame rate** (yield not on a per-frame path — the board spins).

### 12.5 `m2ftg/ELF/CharRamFix`

The module's 16-bit char-RAM write handler **nibble-reverses each halfword** on ingest (`0xABCD → 0xDCBA` at `DLL+0x508B8`) — the hardware byte-lane + nibble convention baked into the write path, matched by a linear left-pixel-low-nibble decoder (`FUN_180036060`). Its **8-bit handler stores bytes plain**, with no transform. Sonic the Fighters only ever writes char RAM with 16-bit stores, so the 8-bit path was never exercised and never right; a homebrew ROM using byte stores (the SDK's `m2_loadfont` / `tfb_putpixel`) gets every 4-pixel group rendered mirrored (`x → 3-x`).

Per byte the w16 transform is `internal[i] = nibble_swap(guest[i ^ 1])`. So the corrected 8-bit **write** forwards to the DLL's own handler at `offset^1` with swapped nibbles, and the corrected 8-bit **read** undoes the same (the guest does read-modify-write) — keeping the DLL's bounds checks, buffer mapping and dirty flags in play rather than reimplementing them.

Install after `module_start` (the memory-map table must exist). Idempotent. **StF-safe by construction**: only the 8-bit slots change. Verified end to end — both guests' char data decodes clean under the hardware convention; StF's w16 data is transformed on ingest, pengo's w8 data is not.

### 12.6 Loose ROM loading

The `LoadLooseRomFiles` debug toggle bypasses `rom/<game>_rom.par`: YAMP hides the archive from the DLL so its mount fails and the engine's **own archive-miss fallback** opens `rom/<archive stem>/*.bin` as plain files. Only honoured when all the game's extracted ROM images are present on disk. Live-verified.

---

## 13. Debug tooling

### 13.1 `m2ftg/LJ/DebugWindows` — the DLL's own dw debug menu

`DrawDebugWindows()` renders the StF DLL's own **DEBUG MENU / CONFIG / PERFORMANCE / 960STAT** windows as ImGui windows. The window layout, item labels, bound variables and action handlers **all come from the descriptor tree inside the game DLL itself** — YAMP only interprets that data. (The menu is dead code in retail; the descriptors survive, most handlers were compiled out.) No-op unless the game is StF, the DLL is loaded and booted, and "Display debugging features" is enabled.

`FUN_18004b070` is the combined ROM load + i960 DEBUG MENU init.

**960STAT fixes (`1796401`).** The i960 register file is embedded in the CPU context at `+0x58` as 64 u32 slots, indexed straight off the instruction's register field. By the i960 ABI that makes `+0x58` = r0/pfp, `+0x5C` = r1/**sp**, `+0x60` = r2/rip and `+0xD4` = g15/fp — all confirmed against the DLL's own call/ret pair. `+0x5C` had been used as the *frame* pointer; it is the **stack** pointer. The pane now seeds the walk with the running procedure and its caller, which live in registers and are not yet in any frame save area — so both were missing from every stack. **DISASM** is a real pane instead of a stub: the IP is at `ctx+0x08`, bank-relative, reconstructed the way the DLL's own call handler does.

### 13.2 `I960Profile` — sampling profiler for the emulated i960

The call-stack pane can only read the machine **between** frames, and once the module yields per frame the board is always parked in the yield handler — so there is never a live call frame to walk. The work happens *inside* `module_main`.

`RamExecFetch::FetchExec` runs for every instruction executed, which is exactly the inside view. Sampling the IP there and bucketing it profiles where the program actually spends its frame — which is what identifies a per-frame code path a between-frames sample can never see.

64-byte buckets over the 1 MB ROM (`BUCKET_COUNT = 16384`, 64 KB table, hot path is one increment); one sample every 1024 instructions (`SAMPLE_MASK = 0x3FF`). Symbolised via the ELF. **A spin shows as one symbol above 90 %, which is the fastest triage available for a homebrew bring-up.**

### 13.3 Other

- `UpdateGameDebugFlag()` — keeps the game's own debug flag in emulated RAM (dword at `0x508000`, flipped by XOR with `0x24`) in sync with the setting, writing through the DLL's own memory-map dispatch. Once per frame.
- `ReadEmulatedRam32(addr, out)` — reads 32 bits of emulated i960 memory through the DLL's own memory-map dispatch; false if the address has no reader in the map.
- **Hidden character select** (Honey / Metal / Eggman): 6 HLE traps on `select_pl` re-enable the ROM's dormant `bbs`-bit3 path; a DLL array copy at `0x1801742C8` overrides `0xDACAC`. Honey's VS portrait needs traps 68–75 on `rm_*` / `MES_ROUND_MASK_DSP` forcing `g0 = 0x19A / 0x19C / 0x96 / 0x98`.
- Diagnostic log files `d3d12_debug.log`, `pso_stream.log`, `heaps.log` all gate on `YAMP_DEBUG_LOGGING`.

---

## 14. Netplay

The headline feature of the branch. **Delay-based lockstep modelled directly on the Sonic the Fighters PS3 port (NPUB30927)** — reverse-engineered from that build, not invented. An earlier GGPO attempt was abandoned and wiped.

### 14.1 Architecture

```
YAMP.exe                                     yampnet.dll (optional)
  source/net/NetPlugin.cpp   ── LoadLibrary ──►  plugin/yampnet/Plugin.cpp
  source/net/YampNet.h  ◄──── shared plain-C ABI ────►
  m2ftg/NetSession.cpp       ── step(frame, &execute_info) ──►  Lockstep + PadCodec
  m2ftg/DebugWindows.cpp     (determinism helpers)               RpcnTransport → RPCN
    ▲ driven by LJ/LJHost.cpp (StF, FV) and YLAD/VF2.cpp (VF2)
```

**Two mechanisms, not one.** Everything in §14.3-14.9 is the lockstep path, which is what StF, FV, VF2 and Fighting Vipers 2 use. **Virtual On and Sega Racing Classic 2 use none of it** — it is a *linked-cabinet* game whose hardware already has a link protocol, so YAMP carries that protocol's own bytes and lets the ROM do the synchronising. No barrier, no seed, no frame numbering, no determinism requirement. §14.10 is that path on Model 2 and §14.11 the same idea on Model 3; the lockstep and linked-cabinet paths share only the session, the room and the socket.

**Why a plugin.** Netcode is expected to churn long after the rest of YAMP is stable, and a release must be able to ship with **no netcode at all**. Omitting `yampnet.dll` is the "exclude it" switch: `IsAvailable()` stays false, `Api()` stays null, every netplay entry point in the UI hides itself, and nothing else notices.

### 14.2 The ABI (`source/net/YampNet.h`, `YAMPNET_ABI_VERSION 10`)

Plain C — no STL, no exceptions, no C++ classes across the boundary. **One exported symbol**, `YampNet_GetApi(uint32_t requested_abi)`, returning a `yampnet_api` function table, so the loader does exactly one `GetProcAddress` and every later addition is a version bump rather than a new symbol.

**Layout handshake.** The plugin writes `execute_info.pad[]` directly (a deliberate scope choice, keeping pad conversion out of YAMP). `yampnet_layout` carries `execute_info_size` (0x1760), `pad_size` (0x190), `pad0_offset` (0x20), `pad1_offset` (0x1B0), `pad_buttons_offset` (0xA0), `pad_port_offset` (0xE0). YAMP fills every field from `offsetof`/`sizeof` on its own headers; the plugin compares against what it was built with. **A mismatch fails the load loudly instead of silently writing at the wrong offsets.**

**States:** `IDLE → CONNECTING → ONLINE → IN_ROOM → SYNCING → IN_MATCH`, plus `FAILED`.

**The hot path:** `yampnet_step step(session, frame, execute_info)` returns `READY` / `WAIT` / `TIMEOUT` / `DISCONNECTED`. **YAMP must not advance the emulator unless it is `READY`.** On `WAIT` nothing is written and the caller re-polls. `execute_info` must be the same object across the whole match.

**ABI evolution:**
- ABI 2 added `get_room_id()` — the lobby needs it because there is no room browser on this transport, and before that the id existed only as a line in `yampnet.log`.
- ABI 3 added **desync detection**: `submit_state_check(session, frame, value)` and `get_desync(session, &frame, &local, &remote)`. Lockstep guarantees identical *inputs*; it cannot guarantee the two emulators agree on what those inputs produced. The value YAMP submits is the ROM's own `frame_counter` at emulated `0x500020`, which advances exactly once per emulated frame and is therefore free of timing noise. The plugin carries the most recent one on every input packet. `get_desync` is **latched** — only the *first* disagreement is reported, because everything after it is a consequence rather than a cause.
- ABI 7/8 concern the **match seed**: 8 strengthens *when* `get_match_seed` is promised valid (from `IN_ROOM`, not `SYNCING`) and adds the host's `kPacketSeed` heartbeat, because a guest that pressed Start before the host had published anything seeded its emulator with 0.
- ABI 9 added the **linked-cabinet channel** — `link_ready` / `link_send` / `link_take` — see §14.10. Appended to the table and adding no struct, but a bump all the same: a stale plugin simply would not have the three entries, and calling through a null tail pointer is not a failure mode worth allowing.
- ABI 10 changed **no signature at all** — it changed the linked-cabinet channel's WIRE format to an RLE-coded payload (§14.10). The function table is untouched, so the bump exists purely to stop a `YAMP.exe` and a `yampnet.dll` from different builds pairing up: they would agree on every call and disagree on the bytes. Note what a version cannot do here — it guards YAMP against ITS plugin, not one machine against another. **Two peers still have to be updated together.**
- ABI 6 added **room game flags**: `yampnet_room_config::game_flags` (in), `yampnet_room_info::game_flags` (out, per browser row) and `get_room_flags(session)`. These carry the **cabinet settings a match is played under**, which are properties of the *room*, not of a machine — see §14.9.

> **Adding a flag BIT is not an ABI change.** The plugin carries `game_flags` verbatim between the
> room and its peers (`dst.game_flags = src.flag_attr`) and never interprets it, so
> `YAMPNET_ROOM_FLAG_VF2_VERSION20` (`0x2`, added 2026-08-02) needed no version bump and no plugin
> rebuild — an existing `yampnet.dll` carries it correctly. Only a change to the *struct layouts* or
> the function table forces a bump.

Other notable config: `yampnet_rpcn_config::cert_fingerprint` (see §14.6), `yampnet_room_config::forced_seed` (0 = plugin generates and distributes; non-zero forces one, for replay/debug), `yampnet_match_config{frame_delay, input_redundancy, stall_timeout_ms}`.

### 14.3 `plugin/yampnet/Lockstep.{h,cpp}` — the core

Deliberately free of Windows, sockets and m2ftg types so it can be reasoned about and unit-tested on its own.

| Constant | Value | Rationale |
|---|---|---|
| `kRingSize` | 1024 | As on PS3 (~17 s at 60 Hz). Power of two — ring index is `frame & kRingMask`. |
| `kRedundancy` | 10 | The PS3 value; the loss burst absorbable without stalling. |
| `kMaxPlayers` | 2 | This build is 1v1. The PS3 relay walked 8 slots. |

Four load-bearing properties:

1. **Delay-based lockstep.** Inputs keyed by **absolute frame number** into a per-player ring. The sim may advance frame N only once every player's input for N is known. **No rollback and no state snapshotting** — the PS3 had none, so none is modelled.
2. **Redundancy.** Every packet re-carries the last `kRedundancy` frames of that player's input, so a dropped datagram is repaired by the next one rather than stalling the round. This is why the transport may be lossy and unordered and still never desync.
3. **Newest-wins insertion.** A ring slot is overwritten only when the incoming frame is newer, making ingest idempotent and safe against duplicates and reordering — the property that makes the redundancy free.
4. **Generation.** A 5-bit round counter fences off inputs belonging to a previous round, so late packets from the round that just ended cannot poison the new one.

**Wire format** (little-endian, memcpy'd straight into the datagram):

```c
struct PacketHeader {          // 8 bytes
    uint8_t type;              // 0 = input, 1 = announce
    uint8_t player, generation, reserved;
    uint32_t session;          // low 32 bits of the room id
};
struct InputRecord {           // 8 + 10*4
    uint32_t frame;            // newest frame carried
    uint32_t packed;           // player << 29 | generation << 24
    uint32_t inputs[10];       // [0] = frame, [1] = frame-1, ... [9] = frame-9
};
struct InputPacket { PacketHeader header; InputRecord record;
                     uint32_t check_frame, check_value; };   // kNoCheck = 0xFFFFFFFF
struct AnnouncePacket { PacketHeader header; uint32_t seed; };
```

The `session` field exists because game traffic is plain P2P between two addresses: a **leftover process from a previous test** — same machines, same port, same player ids, same generation — was indistinguishable from the real peer and could join a session it was never in. Stamping the room makes cross-room traffic self-identifying and free to drop.

The **announce packet doubles as seed distribution**: the host's announce carries the authoritative match seed and the guest adopts it.

### 14.4 `plugin/yampnet/PadCodec.{h,cpp}` — the determinism rule

> **Every byte written into a pad must be a pure function of (player index, input words). Nothing may depend on "am I the local player" or on data only the local machine has.**

That is why `DecodePad` is applied to **both** players — *including our own* — rather than leaving the local pad as the input layer produced it. The local machine has richer information (true analog axes, per-button pressure, real controller ids) than it transmits; if it fed that to the module while the peer fed a reconstruction, the two simulations would drift apart within a few frames even though both "had the same inputs". Round-tripping our own pad through the same lossy encode/decode keeps the machines bit-identical.

Fields deliberately **not** derived from local state: `m_is_remote` forced false on both machines (if the module ever branched on it, a true/false split would be an instant desync); `m_user_id` = player index; `m_port` = player index.

`kInputMask = 0x00FFFFFF` — the transmitted bits of `lj_pad_t::m_now`: face/shoulder/start (`0x0-0xB`), d-pad (`0xC-0xF`), digitised stick directions (`0x10-0x17`). **Analog axes are re-derived from these bits rather than sent**, so both machines compute them identically. `PadHistory` (held-frame counters + previous word) is cleared at the start of every round.

### 14.5 `plugin/yampnet/RpcnClient` + `RpcnTransport` — the RPCN protocol

Verified **against the RPCN server source** (`RipleyTom/rpcn`), not guessed.

**Header, 15 bytes, little-endian:**

```
[0]      u8  packet_type    Request=0 Reply=1 Notification=2 ServerInfo=3
[1..3]   u16 command
[3..7]   u32 packet_size    TOTAL, INCLUDING this header
[7..15]  u64 packet_id      echoed back on the reply
```

A Reply carries `u8 ErrorType` at `[15]`, then payload. Strings in payloads are NUL-terminated raw bytes. **Unauthenticated clients are dropped after 10 s** — log in promptly.

Commands used (values match the server's `CommandType` enum by declaration order): `Login=0`, `Terminate=1`, `Create=2`, `GetServerList=12`, `GetWorldList=13`, `CreateRoom=14`, `JoinRoom=15`, `LeaveRoom=16`, `SearchRoom=17`, `GetRoomDataInternal=20`, `SetRoomDataInternal=21`, `SetRoomMemberDataInternal=23`, `RequestSignalingInfos=27`.

Ports: TLS `31313` (default), UDP signaling helper `3657`, **P2P game traffic `3658`**.

**Room commands** are framed as `[12-byte ComId][u32 LE protobuf length][protobuf]`. The ComId's first 9 bytes must be ASCII uppercase/digits (e.g. `NPWR02113_00`), and the **(comId, worldId) pair must exist in the server's `servers.cfg`** or the server answers `InvalidInput` — it looks the pair up in its `world` table and does not invent defaults. Hence the discovery order `GetServerList → GetWorldList → CreateRoom`; with the server's `CreateMissing=true` these also *register* a previously unknown title.

**Account creation rules** (worth knowing before calling `Create`): npid and online_name must be 3–16 chars of `[A-Za-z0-9_-]`; **none** of the five fields may be empty (the server reads them all with `get_string(false)`, so an empty avatar_url alone is rejected as `Malformed`); email must parse as a real address even when validation is disabled.

**`Protobuf.{h,cpp}`** — a minimal wire-format reader/writer rather than a vendored protobuf runtime (RPCN's room commands use prost; it is only varints and length-delimited blobs). **One trap:** `np2_structs.proto` defines `uint8` and `uint16` as **messages** (`message uint16 { uint32 value = 1; }`, kept from the flatbuffers port), not scalars — so a field declared `uint16 serverId = 1` is a length-delimited *submessage* containing a varint. `WriteWrapped`/`ReadWrapped` exist for exactly that.

**Connection model, and why it is asymmetric:** the **guest** resolves the host through `RequestSignalingInfos` and **transmits first**; the **host** learns the guest's address from the first datagram it receives. That avoids parsing room notifications to discover a join, and it is also what makes NAT traversal work at all — the guest's outbound packet opens the return path.

**Game traffic deliberately shares the signaling socket.** That socket's NAT mapping is the one the server observed and advertised to peers; a second socket would get a different mapping no peer could reach. (Corollary: overriding `local_p2p_port` is for same-process tests only — RPCN hardcodes 3658 when handing out a peer's *local* address.)

`RPCN never relays game data` — it is pure P2P with no relay fallback.

`RoomListing::has_password` is **derived from `privateSlotNum`**: RPCN has no "has password" flag, but a room created with a password marks its slots private, and a joiner without the password can only take a public slot — so private slots *are* the lock.

**Room game flags ride in `flagAttr`, not in a searchable int attribute.** `flagAttr` is a bare `uint32` and is the only room field carried by *all three* replies YAMP reads: `CreateRoomResponse` and `JoinRoomResponse` both wrap a `RoomDataInternal` (`flagAttr` = field 10) and `SearchRoomResponse` carries `RoomDataExternal` (`flagAttr` = field 14). So the host, the guest and the room browser all learn the same word with no extra round trip. `roomSearchableIntAttrExternal` cannot do this: `to_RoomDataExternal` fills those arrays only for the ids listed in the request's `attrId`, and they appear in the **search** reply only — a guest joining by ID would never see them. The server stores `flagAttr` verbatim apart from `SCE_NP_MATCHING2_ROOM_FLAG_ATTR_FULL` (`0x20000000`), which it masks out on create and sets itself when the room fills, so **never read that bit**. YAMP's own bits start at `0x00000001`, well clear of the SCE flags (all of which live in the top nibbles), so a stock RPCN server and a stock RPCS3 client are unaffected. Both sides read the value back **from the reply** rather than remembering what they sent, so the two peers take it from the same source.

`Transport.h` keeps `UdpTransport` (a direct address:port link, no matchmaking) as a dependency-free LAN/loopback dev backend behind the same `ITransport`; swapping backends changes only the member type in `yampnet_session`.

### 14.6 `plugin/yampnet/TlsClient` — Schannel TLS

Built on Schannel/SSPI because it is what Windows already ships — the plugin needs no vendored crypto and links only `secur32`/`crypt32`, which matters for a DLL meant to be dropped next to YAMP.exe and updated on its own.

**Two trust modes, chosen by whether a fingerprint is configured:**

- **VALIDATED (no fingerprint)** — full chain + host-name validation against the system trust store, as any HTTPS client. This is the mode for a server with a real certificate on a real domain.
- **PINNED (fingerprint given)** — RPCN's own `--cert-gen` produces a **self-signed certificate with `CN="RPCN"` and no subjectAltName**, which ordinary validation can never accept. The certificate's SHA-256 is compared against the configured value instead.

**Never pin a publicly issued certificate** — it is reissued at every renewal (~60 days for Let's Encrypt) and the pin would then reject the very server it protects. Connecting unpinned to a self-signed server fails *with the fingerprint named in the error*, which is how you obtain the value to pin.

Schannel's own validation stays **off** in both cases (`SCH_CRED_MANUAL_CRED_VALIDATION`) — doing the check in-process is what lets a failure say which mode was in force and what to fix.

### 14.7 `source/net/NetPlugin.{h,cpp}` — the YAMP-side loader and lobby

Loads `yampnet.dll` from the YAMP.exe directory, negotiates the ABI, builds the layout struct and creates the session. Never throws; failures are logged and leave the plugin unavailable. **`IsAvailable()` is the only thing the rest of YAMP should test.**

**Two drive paths, sharing one session** (only one may be in use at a time — the UI refuses to act while `Config().enabled` is set):

1. **Command line** — the two-machine regression harness, deliberately hands-free and auto-starting:
   `-net-host`, `-net-join <roomId>`, `-net-server <host>`, `-net-user <npid>`, `-net-pass <secret>`, `-net-fp <64 hex>`, `-net-comid <id>` (a comm id or a game key; omitted = this game's own lobby space). One of `-net-host`/`-net-join` arms the path. Keeping this means adding the lobby cannot silently break the only test that proves the netcode end to end.
2. **Lobby** (Settings → Netplay) — the same steps under explicit control, one click at a time. Sits in the lobby until the host presses Start (`RequestStartRound()`).

`DriveSession()` (connect → discovery → host/join, idempotent, called every frame), `Connect/Disconnect/HostRoom/JoinRoom/RefreshRooms/GetRooms/LeaveRoom`, and a flattened `Status` struct so the UI never touches the plugin ABI directly (state, room id, local player, stall count, `peer_lost` + reason, `desynced` + frame/local/remote, status text, error text).

**`SessionInProgress()` — the feature kill-switch.** True while this machine is in a netplay room (lobby, barrier or live match).

> **Any feature that can touch the emulated board, the frame pacing or the HLE hook table must be inert while this holds.** The two peers stay in step only because they run identical code over identical inputs, so a local board reset (the DLL's DEBUG MENU has one), a locally paused emulator, or a hook toggled on one side is an instant desync that looks like a network fault. There is no way to make these safe per-feature: the answer is to switch them off for the duration.

Concretely suppressed for the whole of a session: **coin insert, TEST/SERVICE, pause, the DLL's debug windows, the HLE hook mask, and the game debug flag.** Players use START, which travels in the synchronised pad. The hook mask falls back to `DEFAULT_DISABLE_MASK`, not to zero — a compiled-in constant every peer on the build already agrees on, so it costs nothing here and avoids both sides agreeing on a two-second timer (§9.3).

**Suppression is not the only answer, though — a setting that both peers genuinely need can be *synchronised* instead.** That is what §14.9 does for DAMAGE: rather than forcing it off for the duration (which would make every online match play differently from every offline one), the host's value is published with the room and every peer adopts it. The test is whether a single agreed value exists: pause and coin insert are per-machine actions with no such value, a cabinet assignment has exactly one.

`net::Logf()` appends one line to `yampnet.log` next to the CWD, **opened and closed per line** so it can be read live over a share while YAMP runs — unlike `d3d12_debug.log`, which `DebugLogFile` holds open exclusively for the whole process.

### 14.8 Determinism — the part that actually keeps two machines in sync

**None of this is in the netcode.** All of it lives in `m2ftg/LJ/DebugWindows.cpp` and the host loop.

Every helper is gated on **`IsBoardBooted()`** (the DLL's phase dword at `+0x6B9300` reaching 2) and quietly returns false before it, so **a netplay round must not begin until it is true**. A guest joining an already-waiting host reaches the barrier within a couple of hundred milliseconds of launching — long before its board is up — and would then start a match with no reset, no shared seed and no budget pin while the host applied all three. The result is an emulated CPU rolling its own random numbers on one side only: an AI desync that looks like a network fault and is not one.

| Helper | What it does |
|---|---|
| **`ResetBoard()`** | Re-runs the DLL's own i960 CPU/board initialisation — the DEBUG MENU's `RESET` item (handler `DLL+0x4C840`), which unlike STEP/GO is *not* a stub in retail. Lockstep keeps two emulators in step only if they **start** in the same state; beginning to exchange inputs at the same moment does nothing if one side has already run attract mode for thirty seconds. Resetting both at the barrier makes "frame 0" mean the same thing. |
| **`SeedHostRng(seed)`** | The ROM's `rand` is HLE'd: handler `DLL+0x53070` calls the DLL's generator `DLL+0x8D40` and writes the result into i960 `g0`. That generator is a **Mersenne Twister** (N=624, M=397, standard tempering) whose state object lives at `*(*(DLL+0x68BB88) + 0x20)`: `u32 state[624]` at `+0x08`, circular index at `+0x9C8`. Normally seeded per process, so two machines roll different numbers. This re-runs the standard `init_genrand` with the shared match seed. |
| **`SetTextureBudgetDeterministic(bool)`** | Four Core HLE hooks (all sharing handler `DLL+0x52FD0`) answer the ROM's "have 9 ms elapsed?" question by reading a **wall clock** and writing the boolean into `g0`. The unpack loop yields on that answer, so a fast machine and a slow one do different amounts of work in the same emulated frame — a desync no amount of input synchronisation can fix, because the divergence is not in the inputs. Enabling repoints those entries at a wrapper that runs the original handler (for its length return value) then overwrites `g0` with a constant "budget not expired". Costs smoothness during big uploads; buys a reproducible simulation. |

**Round-start state machine** (`LJHost.cpp`), which anchors both peers to the same ROM state rather than to wherever the reset happened to land:

```
Idle      -> (IN_ROOM && ShouldStartRound() && IsBoardBooted())
             ResetBoard() + SetTextureBudgetDeterministic(true)
Resetting -> wait for ROM frame_counter to restart (proving the reset landed)
Settling  -> wait for frame_counter to reach ANCHOR — the same value on both peers
(barrier) -> announce; on release seed the RNG (instantaneous, needs no settling)
IN_MATCH  -> frame 0
```

The generation counter wraps at 32 (5 bits). If **RNG seeding fails on this peer**, the round is *refused* rather than played — an unseeded generator is guaranteed to diverge. On round end (peer lost, timeout, or desync) the budget pin is released and the host returns to local play.

### 14.8b `m2ftg::NetSession` — the shared session driver

Netplay originally lived inline in `LJHost.cpp::GameLoop`, which is why Fighting Vipers inherited it
for free (same loop) and Virtua Fighter 2 could not (`YLAD/VF2.cpp` has its own ~700-line loop). The
round-prep state machine, barrier, pad injection and desync canary — about 250 lines — were extracted
into `m2ftg::NetSession` (`source/m2ftg/NetSession.{h,cpp}`), leaving `LJHost::GameLoop` 283 lines
lighter. Five function statics became members and the two duplicated round-teardown paths
(timeout/disconnect and desync) collapsed into one `EndRound()`.

A host drives **four call points, and the order is part of the contract**:

```cpp
const auto st = net.GetStatus();     // (1) top of frame, BEFORE input is polled
... poll input, fill execute_info.pad[] ...
net.Drive();                         // (2) poll the plugin + run the round-start machine
const bool advance = net.Step(info); // (3) after the pads are filled; overwrites them
... arcade coin/start protocol, off the SYNCHRONISED pads ...
if (advance) { module_main(...); net.EndFrame(); }   // (4) after every call that ran
```

- `GetStatus()` deliberately returns **last frame's** state, read before `Drive()` can advance it, so
  pad routing and the coin protocol see one stable answer for the whole frame. Merging (1) and (2)
  would shift `inMatch` a frame earlier on the barrier-release frame, changing both.
- `EndFrame()` is separate from `Step()` because the netplay frame index must advance **only** on
  frames that actually ran — a lockstep stall must not tick it.
- **`EndFrame()` also decides for itself whether the call counted**, which is not the same question.
  A lockstep stall is visible to the host (`advance` is false and `module_main` is skipped), but a
  `module_main` call that *runs and does nothing* is not: the i960 loop returns as soon as the ROM
  sets its yield flag, so a call can execute nothing at all. Measured over ~12,000 frames on two
  machines, **~5% of VF2's calls do**, and which ones depends on host timing rather than simulation
  state. Counting those as frames is what put two identical VF2 simulations one emulated frame apart
  (§14.8d). So `EndFrame()` submits the canary and advances the index only when the emulated board
  actually moved; otherwise it returns and the next host frame re-runs the same netplay frame with
  the same inputs — the same path a stall already takes. A host still calls it after every
  `module_main`; it must not try to make this judgement itself.

A host that never calls these behaves exactly as it did before netplay existed.

**Which games can sustain a session** is derived, not listed: `m2ftg::RomFrameCounterAddress()`
returns the game's ROM `frame_counter` address (0 = unmeasured), and both `NetSession::Drive()` and
the UI's `IsNetplayGame()` test it. A game whose counter has been measured starts offering the
netplay page automatically, and the UI cannot drift from what `NetSession` will actually agree to run.

> `frame_counter` is at emulated `0x500020` in **all three** Model 2 games. That is inheritance, not
> coincidence — VF2 (1994) is the ancestor, and FV and StF were built on its engine — but the rest of
> the low-RAM layout did *not* carry over, so each was **measured** advancing by exactly 1 per
> `module_main` call rather than assumed.

### 14.8c Host RNG seeding — all five streams, before the reset

The ROM's `rand` is HLE'd onto a host Mersenne Twister, so two machines roll different numbers unless YAMP seeds them from the shared match seed. Three defects in that, all found 2026-08-02 and all fixed:

**There are FIVE generators, not one.** Each module builds its holder identically (StF `FUN_180064820`, VF2 `FUN_18005EF00`): allocate, write a count of **5**, vector-construct five `0x18`-byte objects, seed them from the performance counter. The state pointer sits at `+0x08` within each object, so the slots are `0x08 + N*0x18` — `0x08, 0x20, 0x38, 0x50, 0x68`. YAMP seeded one (StF) or two (FV, VF2); the rest kept their **wall-clock** seed and therefore differed between peers from the moment the module loaded. They were missed because the streams were derived by inspecting the HLE handlers, which only reveals what the ROM itself draws from — module code uses the others. The holder states its own count outright.

**Seeding must happen BEFORE the board reset.** It used to run at the barrier, reasoned as "seeding consumes no emulated frames, which is why it can happen here while the reset could not". That was about cost and missed what matters: order. The reset makes the ROM re-run its whole initialisation, and that initialisation draws from the RNG — so every one of those draws came from the wall-clock seed. Measured: the two boards' high-score tables came out in different orders. It is now seeded before the reset, and **again** at the barrier — the second pass is a safety net that puts both peers in an identical generator state at frame 0 regardless of how many draws each boot consumed, since the post-reset boot is free-running rather than lockstepped.

**All streams must seed, not any.** `SeedHostRng` returned "any succeeded", so a game whose second generator failed its sanity check reported success and started a half-seeded round — the exact silent desync the per-stream list exists to prevent. It is now all-or-nothing and logs each stream.

> StF hid all three: its second ROM-facing stream is the VS stage picker (hook 22), gated on `is_vs_mode`, off by default. Latent, not absent. **StF and FV have not been re-verified on two machines since these fixes.**

### 14.8d Frame accounting — `module_main` returning is not a frame

VF2 spent two sessions marked "wired but diverging", on the reading that its two peers ran one
emulated frame apart while StF and FV did not. **They were never diverging.** The simulations were
bit-identical and only the frame *numbering* disagreed.

`module_main` returning is a HOST event; the emulated board completing a frame is a GUEST one. The
i960 CPU loop (VF2 `FUN_1800210C0`) executes instructions in batches of twelve and returns the
moment the ROM's yield flag `ctx+0x1B0` is set — which it checks *inside* the batch — so a call can
execute one instruction, or none that matter. Netplay counted every call as a frame, so each peer's
netplay-frame-to-emulated-frame mapping drifted independently and the pair sat a frame apart,
oscillating as each stalled at different moments.

Caught by the timer trace (§15, `[Netplay] TimerTrace`) on a live round. Host and guest are
identical in every field through netplay frames 0 and 1 — same ROM counter, same canary, same 2,988
instructions, same timer counts — and then:

```
host   f=2 rom=11 chk=0x6A9A79A0 ins=2988 rearm=0 t3=6363,1,2988
guest  f=2 rom=10 chk=0x4647F415 ins=0    rearm=0 t3=9351,1,0      <- executed NOTHING
guest  f=1 rom=10 chk=0x4647F415                                    <- ...so it resent frame 1's value
```

The desync report confirms it arithmetically: `local 1788508576, peer 1179120661` is `0x6A9A79A0`
against `0x4647F415` — the guest submitted, as frame 2, the value the host computed at frame **1**.
A 64 KB FNV hash matching bit for bit is not coincidence.

**Why StF and FV never showed it.** They stall the same way, but only during boot: measured on the
same build, all 52 of StF's stalls occur at ROM frame 0-1, after which it advances exactly +1 per
call for 1,428 consecutive calls. A round cannot start until the board is booted and anchored at
frame 8, so their stalls are always outside a round. VF2's are spread evenly across the whole run.

**The test** is deliberately conservative: a call counts as a stall only when the canary is
unchanged AND no timer channel counted down AND none was re-armed (a re-arm is a guest store, so it
proves the CPU ran). Against those 12,000 frames it caught 227 and 171 stalls with **zero false
positives**; the ~14 per run it misses are counted as real frames, which is the safe direction to be
wrong in. StF and FV were re-verified in sync after the change.

> **What this retires.** VF2's desync canary is a work-RAM hash rather than the ROM frame counter,
> and the reason recorded for that was wrong: it argued the counter "jitters" and is "bookkeeping
> ABOUT the frame, not part of the simulation". The counter was accurate — those calls really did
> run no frame — and treating an honest signal as noise is what kept the real fault hidden. Keep the
> hash, because it is the stronger canary and it is what caught this within two frames; discard the
> justification.

### 14.9 Room game flags — `DAMAGE`, `VERSION`, `VS MODE`, and the pattern for cabinet settings

Sonic the Fighters' service menu has a **GAME ASSIGNMENTS → DAMAGE** item, `NORMAL` by default and `REAL` for the harder damage scaling most competitive players prefer. It is exposed as a YAMP arcade dip switch and, because it changes what the ROM computes from a hit, it is also the first setting that had to be made a property of the **room**.

**Where the ROM keeps it.** The service page edits a block of eighteen operator settings that lives in work RAM at `0x59C320`. `game_assignments_flag` is byte `+0x33` of that block — **RAM `0x59C353`** — and **bit `0x80` is REAL**: with every item at its default the byte reads `0x00`, and picking REAL makes it `0x80`.

**Nothing in the module supplies it, which is what makes it a live RAM write.** The block reaches backup SRAM through HLE hook 8 (`set_window_data+0x564`, handler `DLL+0x529D0`), which copies `0x59C320…0x59C382` to SRAM `+0x3320…+0x3382` and substitutes exactly four values out of `m2ftg_config_t`: difficulty (`+0x3342`), country (`+0x3352`), free play and VS mode. DAMAGE is **not** one of them — byte `+0x33` is copied verbatim from whatever `init_game_assignments` left in RAM. A sweep of all 76 HLE hooks found no other writer. Two of them *read* the neighbouring TIME byte `+0x31` — `GAME_INT+0x4`/`DLL+0x52CA0` into RAM `0x500090`, and `ADV_REPLAY_WAIT1A+0x128`/`DLL+0x52CD0` into `game_timer` (§9.3) — and those are the only other touchers of the block. So there is no config field to set and no immediate to patch: the honest mechanism is the game's own byte.

**`m2ftg::UpdateDamageAssignment()`** (`DebugWindows.cpp`) read-modify-writes bit `0x80` of the aligned dword at `0x59C350` through the same memory-map dispatch as `ReadEmulatedRam32`, touching no other bit — the rest of that byte is other items' flags and the three bytes beneath it are `TST_*`/TIME/COUNTRY. The dword's *top* byte is `0x59C353` because emulated RAM is a **flat little-endian host buffer**: the DLL's own 32-bit reader (`DLL+0x4F150`) copies four bytes from `ramBase+address` in ascending order with no swap. It is re-asserted every frame but only *written* when it differs, which is what survives the ROM reloading its assignments on a board reset — i.e. at the start of every netplay round.

> **It is called from the module thread, inside the `advanceFrame` branch, immediately before `module_main`** — not from the UI thread where the other live setting (the game debug flag) is driven. This writes emulated RAM, so under lockstep it has to happen exactly once per **emulated** frame; driving it from the UI thread would tie it to host frame pacing instead, and a write landing a frame earlier on one peer than the other is a divergence even when the value is identical.

**Over the network.** `YAMPNET_ROOM_FLAG_REAL_DAMAGE` (`0x00000001`) is published in the room's `flagAttr` when the host creates it (§14.5) and read back from the create/join reply on both sides. `net::EffectiveRealDamage(localSetting)` is the **single** place the choice is made: the room's value while `SessionInProgress()`, the local dip switch otherwise. Sourcing it from the room rather than from `create_room`'s argument means a host that moves its own switch mid-session cannot drift away from the room it is hosting.

**In the UI.** The room browser gains a **Damage** column, because it is not a preference a joiner keeps — it is how that match will play, and the two settings are very different games. The lobby's in-room panel states it for both players (`(set by the host)` for the guest). The dip switch itself is **frozen once a room exists** and shows the room's live value read-only: it is published at creation, so from that moment it describes the match rather than the machine, and letting it move could only mean a value that is silently ignored or one peer changing a damage rule mid-match. Everything before a room — offline, connecting, or logged in and browsing — stays editable, which is where the choice belongs. The `-net-host` harness path publishes the local setting too, so a command-line pair plays under the same rules the lobby would have produced.

**The second one: Virtua Fighter 2's 2.0 / 2.1 version.** VF2 ships as two mechanically different games, so a mismatch desyncs. The setting is `m2ftg_config_t.is_vf20` (config `+0x07`, `DLL+0x6263F7`), published as `YAMPNET_ROOM_FLAG_VF2_VERSION20` and resolved by `net::EffectiveVf2Version20`.

Two things made it cheaper than DAMAGE. **Adding a flag bit is not an ABI change** — the plugin carries `game_flags` verbatim (`dst.game_flags = src.flag_attr`) and never interprets it, so an existing `yampnet.dll` handles the new bit correctly. And it needs **no relaunch** despite `is_vf20` being a launch-time config field: its only reader is HLE hook 8 (`check_sram_all+0x47C`), the backup-RAM injector, which re-reads it every time the ROM initialises the board — so `NetSession` writes the config byte immediately **before** the round-start `ResetBoard()` and the game's own injector does the rest. Poking the operator block directly would have been the wrong mechanism.

**The third one: VS mode** (`YAMPNET_ROOM_FLAG_VS_MODE`, `0x4`, added 2026-08-03), and it is the one with the strongest claim of the three. `m2ftg_config_t.is_vs_mode` (config `+0x0A`) is read by **all three games**, and it does not tune the simulation — it selects a different one: the module force-credits both coin counters, skips the attract boot, writes SRAM cabinet mode 3 instead of 2, and draws the stage from the **second host RNG stream** (StF 0-8, FV `%9`, VF2 `%11`) instead of the ROM's fixed sequence. A peer with it clear never touches that generator at all, so a mismatch is not a difference of degree. It follows the VF2-version shape exactly — config byte, written before the round-start `ResetBoard()`, resolved by `net::EffectiveVsMode` — plus a **VS** column in the browser, a line in the in-room panel, and the same freeze-while-in-a-room treatment DAMAGE has.

> **Config writes are now checked.** All three games' config bases are recorded (`DwGame::rvaConfigBase` + `configKind`: StF `0x1ED490`/2, FV `0x1EA590`/1, VF2 `0x6263F0`/0) rather than one RVA per field, because the block's first dword is `kind` — a small enum naming the game. **StF's base is confirmed against the DLL** (2026-08-03): `module_start` does `memcpy(&DAT_1801ed490, params+0x38, 0x100C)` and then indexes the ROM-name table with `DAT_1801ed490` itself, which is `kind` by definition; `FUN_1800529d0` reads `+0x04` difficulty, `+0x05` country, `+0x09` freeplay, `+0x0A` VS mode (SRAM cabinet byte 3 vs 2) and `+0x0B`/`+0x0C` sram-restore and its `0x1000` blob — the same shape as VF2's `FUN_18004ECA0` at `0x6263F0`, field for field. `is_vs_mode` has six readers in StF. **All three bases are confirmed against the shipped binaries** (2026-08-03). Each DLL contains exactly one RIP-relative `lea` referencing its base, inside a byte-identical construct — the preceding sixteen bytes match across all three and only the displacement differs:

```
48 8D 53 38        lea  rdx, [rbx+0x38]    ; source = params+0x38
41 B8 0C 10 00 00  mov  r8d, 0x100C        ; size   = sizeof(m2ftg_config_t)
48 8D 0D <disp32>  lea  rcx, [rip+disp32]  ; dest   = THE CONFIG BASE
E8 <rel32>         call memcpy
```

resolving to StF `0x1ED490` (site `0x6290F`) and FV `0x1EA590` (site `0x60F6F`) — both of which are *exactly* the addresses Ghidra independently reports as `module_start`'s reference — and VF2 `0x6263F0` (site `0x5D5DF`). In both StF and FV, `module_start` then indexes the ROM-name table with the config's first field (`local_150[DAT_1801ed490]` / `[DAT_1801ea590]`), which is what makes `+0x00` the `kind` the guard checks. FV's `is_vs_mode` at `0x1EA59A` has four readers and they line up with the FV hook table: `FUN_1800515E0` twice (hook 12, the DIP injector), `FUN_180051A00` (hook 30, the VS stage picker) and `FUN_180051B10` (the VS 2P continue bypass). The `kind` guard stays regardless — it costs one compare and turns any future mis-transcription into a setting that visibly does not apply. `WriteConfigByte` verifies it before writing, so a mis-transcribed base makes a setting visibly not apply instead of flipping a random byte in the module's `.data`. `NetSession` logs `vsMode=N(applied=M)` at round prep for exactly that reason: a peer silently ignoring the room's VS setting would otherwise look like a desync several seconds later.

> **The gap that remains.** Of the five VF2 config bytes that reach the simulation — difficulty (`+0x04`), country (`+0x05`), version (`+0x07`), free play (`+0x09`), VS mode (`+0x0A`) — version and VS mode are now published; difficulty, country and free play are not. The same holds for StF and FV. Two installs that happen to agree hide it. Difficulty and country need more than one bit each, so finishing this is a small packing job rather than a flag apiece.

**Generalising.** Any future cabinet setting that changes the simulation follows this shape: a bit in `flagAttr`, published at create, adopted from the reply, resolved by one `net::Effective*` function, frozen in the UI for the life of the room, and applied where the module will actually read it — for a live RAM value that is the module thread once per emulated frame; for a config byte it is before the board reset.

---

### 14.10 The linked-cabinet path — Virtual On (ABI 9)

**A different mechanism from everything above.** Virtual On's arcade cabinets are one Model 2 board each, joined by a serial ring, and the ROM already contains the whole protocol: a boot-time network check, a per-frame state exchange, a versus handshake, and its own failure paths. So YAMP does not synchronise anything — it carries the ROM's bytes and gets out of the way. **No lockstep, no barrier, no seed, no frame numbering, no determinism requirement.** Lockstep was attempted for this game and failed four two-machine runs in four different ways before the architecture was changed; the banner in `docs/von-netplay-recon.md` records why it is not to be re-litigated.

**The payload.** The emulated comm board's window is 0x700 bytes per cabinet per frame. The ROM stages it itself: it memcpys `cSend` (i960 `0x5032F0`) into comm RAM, and comm RAM into `cRecn` (`0x5024F0`) via a validation buffer. **YAMP's entire job is the comm-RAM window in between** — it must never touch `cSend`/`cRecn`.

The packet's own fields, all decoded from the ROM:

| offset | meaning |
|---|---|
| `+0x002` | running counter, `1823 * (prev + 3)` |
| `+0x004` | **the sender's current mode handler** — `0x10` `Advertize`, `0x20` `WaitChallenger`, `0x21` `SelectV`, `0x24` "I have the stage", `0x30`/`0x31` `InitGame`/`Game_01` |
| `+0x008` | **the stage**, when `+4` is `0x24` |
| `+0x556` | `counter ^ 0xAE5E` |

> **The counter is NOT a sequence number.** The receiver's test is `packet[+0x556] == packet[+2] ^ 0xAE5E`, and *both operands come out of the datagram in hand* — `0x502236` is `staging + 0x556`, not a locally-tracked expectation. Scanned across the whole 2 MB image it is loaded exactly once and stored never. So it is a self-consistency stamp, loss/duplication/reordering are all legal, and **gaps are DETECTABLE rather than fatal**. This was the standing objection to the entire design and it does not hold. Note carefully what it does NOT license: see the stage handshake below, where skipping a packet is fatal for a different reason.

**`kPacketLink` (`plugin/yampnet/Lockstep.h`, `Plugin.cpp`).** A raw datagram type on the same P2P socket the lockstep traffic uses, touching none of it — not the input rings, not the barrier, not the state-check machinery. One receive QUEUE, drained oldest-first, because a newest-wins slot silently eats one-frame protocol events (below). Three entry points: `link_ready` (is a peer reachable), `link_send`, `link_take`. **Usable from `IN_ROOM` onwards**, not from a match: the cabinets link during their boot-time check, which is long before anyone presses Start, and they stay linked between matches.

> Two traps worth keeping. The plugin's receive buffer was `uint8_t[512]` against an 1808-byte link packet — `recvfrom` does **not** truncate, it drops the remainder and reports `WSAEMSGSIZE`, so link packets would have vanished while every lockstep packet kept working. And `link_ready` means "the peer's address is known", which is the right gate for *sending* and the wrong answer for "is the ring up": a cabinet that has stopped sending still has an address, so K2Host ages its own 30-host-frame liveness timeout on top.

**`K2Host` pumps the session itself** (`DriveNetSession`, once per host frame). Every other game reaches `poll()` through `m2ftg::NetSession::Drive`, which also runs the lockstep round flow — Virtual On calls none of that, which left the session's connect, RPCN signalling, transport update and socket drain with nothing driving them. Without this the room never forms, on the lobby path as well as the command-line one.

**The comm-board firmware model** (`DriveCommFirmware`). The Kiwami 2 module fakes a healthy two-node ring with immediates and writes it *once*, at board release, from a latch — real firmware reports continuously, because a ring can go down. So YAMP owns byte 0 (ring up), byte 2 (node id, **from the cabinet role, not the board index** — the index is 0 on every one-board machine) and byte 3 (node count) in both banks, every `module_main` call.

**Delivery position is load-bearing.** `DeliverCommPayload` runs from the link-transfer shim *immediately after* the module's own `g_origLinkTransfer()` and before either CPU steps. The module's transfer is a writer into the same window, and on a one-board cabinet it copies board 1's never-executed, all-zero send buffer straight over the peer's packet. Delivering before `module_main` produced valid packets sitting in comm RAM, `stage=0000/0000`, and `cRecn` never written once in a 1200-frame run. It writes **both banks** and touches the flag register not at all — the bank selector is the module's business and YAMP's attempts to drive bit 0 were simply overwritten each frame. The packet is held **resident** and re-laid after every transfer, which is what a DPRAM does between arrivals and what makes a dropped datagram cost nothing.

**The room assigns the role.** `local_player` 0 = host = MASTER, 1 = guest = SLAVE, applied through `SoftResetIntoRole` (which pulses the cabinet's TEST switch — the ROM's own re-handshake path, chosen because `MainMode` is a request the mainloop only acts on between handlers, not a jump). While a room is up this overrides the `VonCabinetRole` setting entirely: two players who both picked MASTER would otherwise get no link and no explanation. A live room also implies `VonHoldLink`.

**Game speed is a competitive advantage.** `VirtualClock::PaceToVirtualTime`'s policy — *"a host that cannot keep up runs slow rather than stuttering"* — is right for one player and backwards for a linked pair: a cabinet presenting at 32 Hz runs its **board** at 32 Hz, so its pilot moves and fires at half the rate of the one at 60. `VirtualClock::BoardFramesDue(linkLive)` is a fixed-timestep accumulator that steps the board more than once when a host frame overran its 1/60 s budget, capped at 4 (past which the debt is dropped rather than carried). **Solo play keeps the old policy.** Safe here for exactly the reason it was unsafe under lockstep: there is no frame numbering to violate and no peer to stay bit-identical with.

> **This is not only about fairness — it broke the protocol.** The stage handshake works by one cabinet rolling `rand() & 7` (gated on `VersusMode != 0`) and publishing it at `+8` alongside state `0x24` at `+4`, and the other adopting both from `cRecn+8` on seeing `0x24`. **State `0x24` lives for a single board frame.** A cabinet at 32 Hz read its comm RAM half as often as YAMP replaced the resident packet at 60, so the one frame carrying the stage could be overwritten before its ROM ever saw it — and the two cabinets loaded different stages. It presented as an RPCN-vs-LAN difference and was nothing of the kind. **Carry this to Motor Raid and Sega Rally 2: a single-frame handshake state is only safe between boards running at the same rate.**

**A game with no round needs its own overlay.** The netplay overlay hides itself on `YAMPNET_STATE_IN_MATCH`, which a linked-cabinet session never reaches — so "Both players press Start match" sat over an entire match, telling both players to press a button that does nothing for this game. `m2ftg::K2::GetLinkedCabinet` reports the ROM's own answers (ring up, node id/count, `net_flag`) and the overlay renders those instead; the lobby hides the Start-match button for the same reason.

**Diagnostics.** `[von]` lines in `yampnet.log` behind the "Log linked-cabinet state" setting, emitted on change of the discrete state plus a 120-frame heartbeat. They carry the ROM's verdict in words, `net`/`id`/`node`/`main`, own and peer state (`tx=`/`rx=`), the stage fields, `vs=`/`sel=`/`field=`, and the packet's `seq/check` pair at every point along its path (`send0`/`send1` → `data0`/`data1` → `stg`) with `*` marking a pair that passes the ROM's test. That last row turned "it does not work" into two specific mistakes in a single run. **Read board rate from `xfer=` (the module's frame-driver tick), not `f=` — `f=` counts host frames, and with catch-up stepping those are no longer the same thing.**

**Removed 2026-08-07: the loopback harness.** `VonLink` was a direct address:port transport compiled into YAMP so two instances on ONE machine could exercise the link, because RPCN cannot do loopback — it hardcodes 3658 in the address it hands a peer. It brought up the firmware model, the bank discipline, the delivery window and the whole probe, and it is gone, along with `VonLinkPeer`, `VonLinkPort`, YAMP's copy of `plugin/yampnet/Transport.{h,cpp}` and the `ws2_32` link on the YAMP project. RPCN is the protocol this ships on, and a second transport that nothing tests is a second set of behaviours to keep true. **Testing a link now requires two machines** (`-net-host` / `-net-join <roomId>`).

**Wire efficiency, and the trap under it.** The raw payload is 1804 bytes on the wire against a 1500-byte path MTU — it fragmented, at ~200 KB/s each way. Measured on a live match it is ~1600/1792 zero bytes and RLE-codes to ~326, so the channel now sends a **stateless byte-RLE** (`kLinkRle`, falling back to raw when coding would expand): datagram avg ~120 B, max 241 B, and nothing fragments. Stateless rather than delta-coded on purpose — only ~5 bytes change per frame so a delta would win more, but it needs an acknowledged baseline, and that destroys the property the whole design rests on: every packet is a complete snapshot, so loss, duplication and reordering are free.

> Rate-limiting the send to once per board frame (off the ROM's own `+2` stamp) is what made those figures possible, and **it reintroduced the stage desync**. Several of the ROM's handshake states last exactly ONE board frame — 0x24, which carries the rolled stage, among them — and sending two or three times per frame had been covering that *by accident*. `kLinkRedundancy = 3` makes it deliberate, which is the same answer the lockstep path reaches with `kRedundancy`. **Compressing and rate-limiting a link like this is only safe if the redundancy then goes back explicitly**: the snapshot property makes loss free for the STATE a packet carries, not for the one-frame EVENTS the protocol signals through that state. Worth carrying to Motor Raid.

**Status.** Verified on two machines over RPCN: both cabinets reach "Network Check Success", exchange the ROM's payload continuously, run the versus handshake, agree on a stage and play a match through. Untested: real internet RTT and jitter — the LAN figures say nothing about either.

### 14.11 The linked-cabinet path — Sega Racing Classic 2, on Model 3 (2026-08-08)

**The same idea as §14.10 and almost none of the same work**, because on this board the module already emulates the comm hardware. `M3ERomSrc2` **embeds a `CXComm` subobject at `rom+0x588`** with two 64 KB banks — the reason SRC2 and Spikeout allocate 0x20610 bytes where every other game in the module allocates 0x5E0 — and the guest's `0xC0xxxxxx` window reaches it live:

```
guest 0xC0xxxxxx -> CM3Mem window entry 9 -> the device pointer at mem+0x360, which
                    CM3Mem::init fills with the ROM OBJECT -> M3ERomSrc2 vtable slots
                    10-13 -> rom+0x588, the CXComm banks, byte-swapped and addressed ^ 2
```

This is the exact opposite of Virtual On, where the module's comm firmware was a stub and `K2Host` had to *drive* one (`DriveCommFirmware`). Worth stating because the **same game contains the opposite arrangement**: SRC2's security board (slots 2/3) *is* a stub that returns 0 and discards writes, and that stub is why two of its HLE hooks exist at all.

**So the wire format is the module's, not YAMP's.** `FUN_180035BA0` runs once per emulated frame from SRC2's own per-frame task and moves bytes through three pointers the host puts in `execute_info.p_work_ptr[0..2]` — three nullable slots YAMP had always left null, which is the entire reason nothing ever linked:

| slot | meaning |
|---|---|
| `p_work_ptr[0]` | **TX array** — the board copies its outgoing packet *into* it, at slot `[nodeId]` |
| `p_work_ptr[1]` | **RX array** — the board copies every node's packet *out* of it, indexed by node — **including this node's own**. The ingest walks sources `(me-1-i) mod count`, so `p1[me]` is read every frame into the last of the **count+1** window slots the guest programs (`0xC0020804` = size × (count+1)) — the ring handing a cabinet its packet back after a lap, which the master's comm service uses as its ring-lap timing reference. `CommBoard::Update` mirrors the slot; measured from the module disasm, no longer an open question |
| `p_work_ptr[2]` | a **u64 rendezvous word**, shared between all cabinets |

Both arrays are `nodeCount * packetSize`, and **packetSize is a register the GUEST programs** (`0xC0020808` → `comm+0x20024`; SRC2 chooses 848). The host never picks it, which is why `CommBoard`'s buffers are sized for the largest a bank can hold rather than for an expected value — the module memcpys with a length YAMP does not control.

**The rendezvous word is four bit-fields in one, and the groups are DIRECTIONAL.** Getting this backwards is not a subtle failure:

| bits | direction | meaning |
|---|---|---|
| `0 .. n-1` | host → module | node *i*'s packet is present in RX. Transfer state 3 counts these |
| `16 .. 16+n-1` | host → module | node *i* has finished booting. The machine's **boot barrier** counts these |
| `nodeId+0x20` | module → host | this node answered the guest's `0xF000` ready command |
| `nodeId+0x30` | module → host | this node has finished booting |

The module never signals itself through it: it **reports** in the high groups and **waits** on the host in the low ones, because the host is what knows about the others. Setting `0x30` where the module reads `0x10` stalls the board before its running phase — 800 frames, zero draws, no other symptom. Both waits are spelled as an early return out of the frame, which is what makes this path need no lockstep, no barrier, no seed and no determinism contract. One scope fact, measured from the transfer's disassembly: **the ready bits gate the 3→4 transition once — state 4 transfers every frame without re-reading them** — so the staleness window on the host protects a board that is *linking* (and any guest-triggered re-latch through `0xC0010180`), not one that is racing; mid-race freshness is the guest's own job, done through the per-node stamp at packet `+0x36`.

#### The role comes from the ROOM, and the board is REBOOTED into it

`CommBoard::Configure()` runs **once**, immediately before `module_start`, because that is where the module latches the node id and peer count out of the config block and never re-reads either. An RPCN room does not exist at that instant and cannot be made to — connect, TLS, login, discovery and create/join are seconds of round trips. So "ask the room what this cabinet is" answered *no room* on every launch and the cabinet silently came up STAND-ALONE, every time.

**Guessing the role earlier from the launch flags is the wrong fix** and was rejected: a room is what decides it, and a player who joins from the lobby never touched a launch flag. `CommBoard::DriveRoomRole()` (once per host frame, from `Pre3Host`) changes it *afterwards*, and every mechanism it uses is the module's own:

1. **write the config globals** — `+0x100C` node id, `+0x1010` peer count. The comm board re-latches both when its state machine passes back through state 1, which the guest does for itself during boot by writing 0 to `0xC0010180`.
2. **`ArcadeSettings::Reset()`** — re-arms the LINK ID row. The restore is about to wipe the guest RAM both copies live in, and without this the rebooted board reads back the SINGLE it powered on with and its check returns before printing anything.
3. **`RestoreResetSnapshot()`** — the power-on snapshot of §9.7, so the guest re-runs its whole boot chain including the network check `FUN_00093DB4`.

Virtual On does the equivalent by pulsing TEST, because its ROM re-enters `Net_check` on operator-menu exit. **SRC2's check is called once from the boot chain and nothing re-enters it**, so the equivalent here is bigger and, usefully, more literal: put the board back to power-on and let it boot again.

> `s_appliedRole` is deliberately a **separate variable** from `s_nodeId`: the first is what the guest last BOOTED as, the second what the host currently wants it to be. Collapsing them would make the role change believe its work was done the instant it asked for it — the same mistake §14.10's `ApplyCabinetRole` documents from the other direction.

**A cabinet with no room is untouched.** A plain `-src2` launch emits no `[SRC2 link]` line at all, leaves `Src2LinkId` at whatever the operator set, and its check runs SINGLE.

#### Three wire defects that shared memory structurally could not expose

Like a Dragon Gaiden runs its own cabinets in one process against literal shared memory, so `Mode::Shared` (a named section, both instances mapping it, TX and RX the *same* array) is not a model of the link — it *is* the link, and it linked cleanly. It also hides everything about delivery. Over a real wire:

- **The plugin de-dupes byte-identical link payloads** (`LinkPush`), which is right for Virtual On — it rate-limits itself to one datagram per board frame — and wrong here, where the host transmits every host frame and needs every transmission to count as a sign of life. A cabinet whose packet has not changed is not a cabinet that has gone away, but with the de-dupe in the way it looks like one: nothing arrives, the staleness window expires, the peer's ready bit is cleared, and the board waits forever for a peer that is transmitting perfectly. Fixed with a **`seq` byte that moves every frame** — which leaves the three REDUNDANCY copies of one datagram byte-identical, so they still collapse to one. That is exactly the split wanted.
- **The RX slot was indexed by the SENDER's packet size**, where the module reads that array as `rx + ourPacketSize * i`. The two agree in every healthy session, so it worked and would have gone on working right up until they disagreed — at which point the peer's data lands at an offset the board never reads, and the symptom is a silent peer on a link with traffic on it.
- **`STALE_FRAMES` was 6**, which under shared memory could never fire. Over a wire the two cabinets do not run at the same rate — measured ~2.4:1 between a debug build with probes on and one without — so a window measured in OUR frames has an unknown length in THEIRS. Now 30. Liveness is also no longer tied to the peer having programmed a packet size: any valid datagram refreshes the clock, because "is the peer there" and "has the peer's guest programmed a size yet" are unrelated facts.

**The wire header** is 8 bytes — magic, node, flags (`ready`/`ack`/`booted`, the sender's own contribution to the rendezvous word, re-expanded into the peer's positions on arrival), the `seq` byte, size, and the board's own transfer counter for the log. Carrying the flags rather than inferring them is what lets the **boot barrier** work at all: the module sets its own bit `0x30` and waits for everyone's, and no other channel exists to learn that a peer has booted. **A packet size of zero is not a reason not to send** — the guest cannot programme a size until its board runs, the board cannot run until the barrier releases, and the barrier cannot release until each peer has heard the other has booted, which rides in this header. That is a deadlock if the header is withheld.

#### Verified

Two machines, a real RPCN room, `-net-host` / `-net-join <roomId>`:

```
MASTER  the boot network check ran as MASTER CONTROLLER (LINK ID = 1), settled on
        id=1 nodes=2, net=0xE8 -> agreed on a ring
SLAVE   the boot network check ran as SLAVE (LINK ID = 2), settled on
        id=2 nodes=2, net=0xE8 -> agreed on a ring
```

Both held comm `state=4` for ~3500 frames with the wire up, and **both machines' RX arrays carried identical contents** — node 0's slot 227 non-zero bytes, node 1's 45-47, with matching per-frame churn on each side. The datagram is 856 raw bytes (8-byte header + the guest's 848), RLE-coded to 55-199 on the wire. Node results land at guest `0x10062A` (node COUNT) and `0x10062B` (this cabinet's id, 1-based) — measured from the pair, which is how they came out labelled backwards the first time.

> **A direct-UDP transport was built for this and then removed.** Before RPCN was wired, the same `Mode::Link` path ran over a plain address pair (a `link_direct` ABI entry driving `Transport.h`'s `UdpTransport`), two instances on one machine over loopback. It reached `state=4` for 2300+ frames and passed the ROM's check on both cabinets — and it is what caught all three defects above, which is why the RPCN run then worked first try. It is **gone**, for §14.10's reason: a second transport that nothing tests is a second set of behaviours to keep true. It had one job — separating "does the game play linked over a real wire" from "does the RPCN client bring a peer pair up" — and it did it.

**A RACE HAS BEEN DRIVEN, AND THE AI CARS APPEARED UNSYNCED** — two players on the two machines
over RPCN: at 0x40000 and 0x100 granularity, no region of guest RAM that changes on both cabinets
ever agreed at matched guest frames, the link itself carrying byte-identical RX arrays throughout,
and the race packets symmetric (487 and 475 non-zero bytes of 848; the 227-vs-45 asymmetry above is
the boot handshake). The two players see each other; they each saw a different CPU field. The
conclusion first drawn from that — *"the protocol has no authority for a shared field, so this is
by design"* — **was WRONG, and a static dive into the guest ROM overturned it** (2026-08-08,
`docs/src2-netplay-recon.md` §9):

**THE PROTOCOL SHARES THE AI FIELD, AND THE 848 BYTES ALREADY CARRY IT.** At race formation
`FUN_00092f04` fills the ownership table (guest `0x72cad4`, one entry per car slot): one player car
per linked cabinet, then **every CPU car dealt round-robin across the live cabinets as its owner**.
Each cabinet simulates only the CPU cars it owns and broadcasts them **every frame inside the
packet at +0xD0** (format byte, count at +0xD2, then count × 0x18-byte records; the player car
rides at +0x60, the lobby's bulk channel — car-number grid, settings, rankings — at +0x3C/+0x48).
The receiver applies the peer's records to its remote-owned cars, whose update function is swapped
to a dead-reckoner. A symmetric packet is therefore what a WORKING shared field looks like. The
block-hash probe was structurally blind to this: the apply writes absolute positions sampled at
different wall instants, so matched-gframe hashes disagree even while the sync works. An
instruction-level audit of the module transfer against `CommBoard` found **no wire divergence** —
the remaining suspect for the on-screen divergence is **frame pacing** (the cabinets measured
~2.4:1; real twins share a 57.5 Hz crystal), which lives outside the packet.

**VERIFIED BY A SECOND RACE (2026-08-08, `docs/src2-netplay-recon.md` §10).** Same machines, same
probe: the seeding ran linked on both cabinets (40 cars, `owned=19/19`, the two ownership tables
identical with the remote flags exactly inverted), the +0xD0 broadcast carried its valid bit
through the racing phase, and re-running the ORIGINAL matched-gframe block comparison gave
**240 of 678 dynamic blocks bit-identical in >95% of samples** — against the first race's 0 of
592 — including a contiguous ~27 KB per-car state table at 0x120100. The never-agreeing blocks
are precisely the layers that cannot hash equal (both players' own cars and the applied-position
regions). **The CPU field is shared over the link**; the first race's divergence is attributed,
tentatively, to mismatched race configuration before the room-published game assignments landed.
One module gap found and measured harmless: the broadcast's format/count bytes are written with
`stb` and the comm window discards byte writes — the receiver takes both from its local ownership
mirrors and gates only on the valid bit, which travels as a u32.

A third race (recon §10a) confirmed the corrected decode end to end (`m=16` + `ai=Y` symmetric on
every racing heartbeat) and taught the measurement lesson: **align the block comparison on race
FORMATION, not raw gframe** — this race formed 930 gframes apart, raw-gframe read 0 strong blocks
while formation-aligned read **492 of 688**, a plateau at ±1 sample. §8's zero is thereby fully
explained. Frame pacing then landed (recon §10b): while a link is live both cabinets are pinned
to 60 Hz wall time — the host loop's 60 Hz limiter is forced regardless of the FPS-cap setting,
and `CommBoard::BoardFramesDue()` steps up to 4 catch-up board frames after a hitch (comm
re-pumped between steps, render on the last only, debt dropped past the cap). Virtual On's
`VirtualClock` policy minus the QPC patch pre3 does not need; deliberately not ping-coupled.

> **A linked cabinet is not a "netplay game" by the lockstep definition, and two features assumed it
> was.** `IsNetplayGame()` was `m2ftg::NetplaySupported() || pre3::NetplaySupported()` - both of
> which ask *can this sustain a LOCKSTEP round*, derived from a measured ROM frame counter or a
> pinnable clock (an FV2-only whitelist). SRC2 fails both forever, so it got **no Netplay page, no
> overlay and no room id** while its link worked perfectly. And `netplayLocked` disabled the
> **coin/start protocol** for the whole session on FV2's reasoning that a round starts from a
> savestate past the credit screen - so with free play off a linked cabinet could not be credited at
> all. `CommBoard::LinkedCabinetSupported()` / `GetLinkedCabinet()` and a `roundLocked` that excludes
> a linked cabinet fix both. **Carry this to Motor Raid and Sega Rally 2:** every gate spelled
> "netplay" on this branch means "lockstep round", and a linked-cabinet game needs each one re-read.

**Diagnostics.** `[SRC2 link]` lines carry the comm state machine, node id/count, the guest's packet size, sequence, the rendezvous word, wire tx/rx counters per node and the peer's board sequence — three counters rather than one because they fail separately: *nothing sent* = this cabinet is not producing; *sent but nothing received* = the wire or the far end; *received but the peer's board sequence frozen* = the peer's process is alive and its BOARD is not transferring, which no other field distinguishes. Each node's slot line now also **decodes the packet against the ROM's own map**: `m=` (the sender's comm mode, header word top 5 bits — the header lands at window +4, not +0), `pc=` (player-car snapshot valid, the `0x81` pair on the word at +0x64), and `ai=<valid>:<count>/<format>` (the +0xD0 CPU-car broadcast) — so "is the AI channel live" is one log line per side. Byte order matters and cost one race's readings: the bank stores guest bytes with a **halfword-lane swap** (guest packet byte `A` ↔ array byte `A^2`), not a full reversal — read single bytes at `A^2`, never LE words. The car probe (`YAMP_SRC2_CARPROBE`, its own `src2_car.log`) additionally dumps the seeding itself: `grp=`/`owned=` and `tbl=`, the per-slot ownership table with `*` marking remote-owned cars. `[SRC2 linkgate]` prints the ROM's own verdict on its boot check. All are gated to change-plus-heartbeat; the frame-boundary probe (`YAMP_PRE3_SYNCPROBE`) records to a memory ring and dumps at teardown, because a `DebugLogFile` per frame was expensive enough to **change the outcome it was measuring**.

---

## 15. Settings file reference

`settings.ini` next to `YAMP.exe` (portable mode; `SetDataPath()` resolves from the module path, not the CWD). One file, per-game sections; save files carry the game tag, so one folder holds them all.

| Section | Key | Type | Default | Notes |
|---|---|---|---|---|
| `General` | `Version` | int | — | Mismatch discards the whole file |
| | `Disclaimer` | int | 0 | Build that last showed it |
| `Graphics` | `ResolutionX` / `ResolutionY` | int | 1280 / 720 | Window + swapchain size |
| | `RefreshRate` | float | 60.0 | |
| | `Fullscreen` | 0/1 | 0 | Borderless `WS_POPUP` |
| | `FPSCap` | 0/1 | 1 | |
| | `Model2RenderMode` | **switch text** | `""` | `-model2`, `-vga`, … (§9.4) |
| | `Model2WindowMatchesRender` | 0/1 | 0 | |
| `Audio` | `Volume` | int 0..100 | 100 | Applied through each module's **own** volume mechanism, never by scaling samples host-side |
| `VF5FS` | `ArcadeMode` | 0/1 | 0 | |
| | `CircleConfirm` | 0/1 | 0 | |
| | `Language` | int | 1 (English) | |
| `StF` | `AspectRatio` | 0/1/2 | 0 | 4:3 / 16:9 / fill. Live |
| | `CRTFilter` | 0/1 | 0 | Live |
| | `Difficulty` | 0..3 | 1 | Restart |
| | `Country` | 0/1/2 | 0 | Japan / USA / Export. Restart |
| | `FreePlay` | 0/1 | 1 | Restart |
| | `VersusMode` | 0/1 | 0 | Restart |
| | `RealDamage` | 0/1 | 0 | GAME ASSIGNMENTS → DAMAGE (`0` = NORMAL, `1` = REAL). **Live** — it is a RAM byte, not a module config field. Overridden by the room's value during netplay and frozen in the UI for the life of a room (§14.9) |
| | `P<n>ControllerId` | string | `xinput:0` / `xinput:1` | `Input::PadDevice::id` |
| | `P<n>Controller` | int | — | **Legacy** XInput slot; honoured only when the id key is absent, rewritten in the new form on the next save |
| | `P<n>Key<Action>` | int (VK) | see `DEFAULT_KEY_BINDS` | Live |
| | `P<n>Pad<Action>` | int (`PadButton`) | see `DEFAULT_PAD_BINDS` | Live |
| `VF2` | `Version20` | 0/1 | 0 | `is_vf20`. Applied live at a netplay round start (before the board reset); otherwise restart. Published as a room flag — see §14.9 |
| `Debug` | `DoNotApplyPatches` | 0/1 | 0 | |
| | `UseDebugD3D` | 0/1 | 0 | |
| | `ShowDLLDebugFeatures` | 0/1 | 0 | dw debug windows |
| | `LoadLooseRomFiles` | 0/1 | 0 | Also gates `game.elf` parsing |
| | `SetGameDebugFlag` | 0/1 | 0 | Live |
| | `FixBackupRamTimeIndex` | 0/1 | **1** | Corrects the module's backup-RAM TIME byte (§9.3). Restart. Off = GAME ASSIGNMENTS freezes the emulator |
| | `DisabledHleHooksLo` / `...Hi`<br>(and `.FV` / `.VF2`-suffixed twins) | hex u64 | **`HleHooks::DefaultDisableMask()`** — StF: bits 16 **and 17**; FV/VF2: 0 | Bit i = hook i. **Keyed per game**, because the value is a set of bit INDICES into one game's hook table and every game's table differs — `settings.ini` is shared by every title, so one key would carry StF's choices into Fighting Vipers and disable whatever FV keeps at those indices (a Content hook in one game is a Core hook in the other). StF keeps the unsuffixed names so existing files still load. **Core bits stripped on save and load.** Default is no longer 0 — see §9.3; an ini from an older build carries an explicit mask (`0`, or bit 16 only) and keeps hook 16 and/or 17 enabled until **Restore defaults** is pressed or the lines are deleted |
| `HleRetarget` | `Hook<i>` | text | `""` | Per-hook site: literal offset, `off`/`none`/`-`, or `symbol[+hexoff]`. Read once before `module_start`; **never written back**, so a hand-authored section survives Apply |
| `Netplay` | `Server` | string | `rpcn.sonicthefighte.rs` | RPCN host. Defaulted rather than blank: it is where YAMP's netplay is played, and **Create a new account** needs a server before an account exists to configure one with |
| | `Npid` | string | `""` | |
| | `Token` | string | `""` | Account **password** — RPCN's Login takes (npid, password, token) and the token is only used with email validation enabled, which is off by default |
| | `CertFingerprint` | 64 hex | `""` | **Leave empty** unless the server is self-signed (§14.6) |
| | `CommunicationId` | string | `""` | **Empty is the normal value**: each game gets a lobby space of its own, because `net::AutoComIdKey()` sends the running game's arcade name and the plugin turns it into a per-game comm id (yampnet `source/ComId.h`). A value here overrides that with a literal comm id or another game's key, which is only wanted to meet someone outside your game. An ini still carrying the old `NPWR02113_00` default is read as empty — it was one shared lobby list for every game YAMP hosts, and nobody chose it |
| | `FrameDelay` | int | 3 | Higher hides more latency; too low stalls rather than desyncs |
| | `TimerTrace` | 0/1 | 0 | **Diagnostic, no UI.** One line per emulated frame to `yampnet.log` carrying the i960's four timer channels and the instructions charged to them. The timers count INSTRUCTIONS, not wall clock, so the line is a direct readout of instructions-per-frame and two peers' logs diff line-for-line (§14.8d). Runs in and out of a round, so it can be validated on one machine. Costs a file open/append/close per frame — leave off for ordinary play |
| | `ForceUnsupported` | 0/1 | 0 | **Diagnostic, no UI.** Lets a game whose `netplayReady` is false still open a round, so a game that is measured but not yet trusted can be observed at all — which is what §14.8d was found with. A round run under it is expected to be wrong |
| | `VonCabinetRole` | 0/1/2 | 0 | Virtual On only: NOLINK / MASTER / SLAVE, applied before `module_start`. **A live room overrides it** (§14.10) |
| | `VonHoldLink` | 0/1 | 0 | Virtual On only: hold the cabinet in its boot-time link check until a partner answers. Implied by a live room |
| | `VonLinkLog` | 0/1 | 0 | Virtual On only: the `[von]` link probe into `yampnet.log` (§14.10) |

---

## 16. Working-tree state at hand-off

**Uncommitted changes on top of the tip:**

| Path | Change |
|---|---|
| `source/Utils/Patterns.h` | Adds `#include <string>`. This is a one-line fix **inside the `ModUtils` submodule**, so it cannot be recorded by this repository — the gitlink only stores a commit SHA, and there is no upstream commit containing it. It must be pushed to `CookiePLMonster/ModUtils` (or a fork) and the submodule pointer then bumped here. Until that happens the file shows as `Submodule source/Utils contains modified content` and a fresh clone gets the unfixed header. **A fresh clone needs this to build.** |

Everything else described in this document is committed.

### Known-incomplete, deliberately

- **VF2's service menu corrupts the geometry on exit** — a board soft-reset the host does not survive. Same doc.
- **Three cabinet settings per game are still unpublished:** difficulty (`+0x04`), country (`+0x05`) and free play (`+0x09`), for all three games (§14.9). DAMAGE, the VF2 version and VS mode are published. Difficulty and country need more than one bit each, so finishing this is a packing job rather than a flag apiece.
- **The stall test in §14.8d is conservative and not provably exact** — it misses ~14 stalls per 4,000 frames (ROM counter unmoved but the canary or a timer moved), counting them as real frames. That is the safe direction and no round has drifted because of it, but it is the first place to look if a long match ever does.
- **Netplay credentials share `settings.ini`** with per-game settings, so resetting game settings destroys the account configuration. They belong in their own file.
- **HLE hook hit-counters read 0 for VF2**, because the instruction-fetch hook that feeds them is LJ-only. Absent rather than wrong, but it looks broken in the settings panel.

**Virtual On / linked-cabinet (§14.10), as of 2026-08-08:**

- ~~**The payload fragments against a 1500-byte MTU** at ~200 KB/s each way.~~ **Done.** Both levers named here were implemented: the send is rate-limited to one datagram per board frame off the ROM's own `+2` stamp, and the payload is stateless byte-RLE coded. Datagram avg ~120 B, max 241 B, nothing fragments. The rate limit **reintroduced the stage desync** on its own — see the warning in §14.10 — which `kLinkRedundancy = 3` then fixed deliberately.
- **Still untested beyond a LAN.** Real internet RTT and jitter say nothing that the LAN figures cover, and the ROM's protocol expects a partner one frame away on a serial ring.
- **Input mapping for a linked match is unfinished** — the cabinets link and play, but which physical control reaches which pod has not been worked through.
- **`SoftResetIntoRole` has never been driven from the settings UI at runtime**, only from a room join and from a direct call. Same code path, but the setting-change route is unexercised.
- **The catch-up pacer is capped at 4 board frames** and drops the debt past that. A host that is persistently slower than 60 Hz therefore still runs slow, just less so; the cap exists because a long hitch repaid in one burst freezes the host.

**Sega Racing Classic 2 / linked-cabinet on Model 3 (§14.11), as of 2026-08-08:**

- ~~**Nobody has driven a race.**~~ **Done** — and the first reading of it ("the AI cars are
  simulated independently, plausibly by design") was **overturned by the ROM disassembly**: the
  protocol seeds the CPU cars by round-robin ownership and broadcasts each cabinet's share every
  frame at packet +0xD0, all inside the 848 bytes YAMP already relays, and the block-hash probe was
  structurally unable to see that kind of sync. See §14.11 and `docs/src2-netplay-recon.md` §9.
  **Open:** a live race read through the new `ai=` / `tbl=` instruments, to confirm the seeding and
  broadcast run over our link; and **frame pacing** — the cabinets measured ~2.4:1 where real twins
  share a crystal, which would spread the two halves of a correctly-shared field apart on screen.
- ~~**The AI car array has not been located.**~~ Located: car list base pointer at guest
  `0x737a7c` (stride in each record's word at +8, record 0 = the local player's via the slot swap),
  ownership table at `0x72cad4`, car count at `0x105018`.
- **Untested beyond a LAN**, exactly as §14.10.
- **The lobby path is unexercised.** `DriveRoomRole` accepts a room formed at any time, so F1 → Netplay should now work as well as `-net-host` / `-net-join` does. It has only been driven from the command line.
- **Two cabinets only.** The plugin's link channel is point-to-point, so a ring of three or more is not expressible on it. `CommBoard`'s arrays and rendezvous groups are sized for `MAX_NODES` because the MODULE's are, not because this transport can fill them.
- **No desync canary and no determinism contract**, by design — the ROM's own protocol does the synchronising. There is correspondingly nothing that would *tell* you the two cabinets had diverged; the guest-RAM digest in `CommBoard.cpp` is a manual diff tool, keyed on the guest's own frame counter at `0x737978` rather than the host frame (keying it on the host frame put the cabinets 29 guest frames apart and reported a desync on every run).

## 17. Suggested reimplementation order

Ordered so each stage is independently runnable and verifiable.

**Stage 0 — foundation (no game runs yet)**
1. `premake5.lua` two-project layout, `Debug`/`Release`/`Master`, warning policy, per-file audio optimisation.
2. `DebugLog` + the `YAMP_DEBUG_LOGGING` gate.
3. `YAMPGeneral` `GameId` machinery, portable `SetDataPath`, `IsModel2ArcadeGame`.
4. `YAMPSettings` load/save incl. all sections in §15.
5. `RenderWindow`: D3D12 device + queue, 11-on-12 bridge, the missing `COMMON → RENDER_TARGET` barrier, threaded window + deferred resize, blit with aspect/fill/source-rect, `ClearBackbuffer`.

**Stage 1 — one game end to end (VF5FS/Y6)**
6. `pxd/Y6` (largely the baseline, relocated into `namespace pxd`).
7. `vf5fs/vf5fs.h` shared protocol + `vf5fs/Y6` host. **Set `sound_volume` non-zero.**
8. `GameVerify` + the module/parent tables. Gate `LoadDLL`.

**Stage 2 — audio** *(independently testable against the oracles)*
9. `AdxDecoder` → validate sample-exact vs ffmpeg on shipped VF5FS files.
10. `HcaDecoder` + `HcaTables` → validate stage-for-stage vs ClHcaSharp.
11. `AtomEngine` (ACB `@UTF` tables, AFS2, XAudio2) + `Cri`.

**Stage 3 — the DX12 platform and the first Model 2 board**
12. `pxd/LJ` sl/gs/pxd_types/file_access/async_request/sys_util/cs_game.
13. `DllMutex` (VS2019 `std::mutex` shim) + vendored `D3D12MemAlloc`.
14. `HostCdevice` — `BuildHostCdevice`, `SetGameDllRange` (**do this before touching FV**), the 7 command-list hooks, per-frame submit / frame-stamp.
15. `PatchGs` + `ResetCbvSrvRingCursors`.
16. `m2ftg.h` protocol, `m2ftg/ImportSymbols`, `m2ftg/LJ/{LJHost,Patch}`. Ship StF first, then FV and MR via `GameDesc`.

**Stage 4 — input and UI**
17. `source/input` (Input, DirectInputPad, Pad) with `MODULE_ASSIGN`.
18. `YAMPUserInterface` pages: Graphics, Game, Controls (+ wizard + capture), Debug, About.
19. `m2ftg/HostUI` aspect + pause menu. The CRT filter port.

**Stage 5 — the remaining titles**
20. `ModuleArgs` + `DisplayModes` + `SetModuleSourceRect`.
21. `m2ftg/YLAD` (VF2), `vf5fs/LJ`, `vf5fs/YLAD`.
22. `pxd/K2` sl + `m2ftg/K2` (VF2 + Virtual On).
23. `GameLauncher` + `Verify::GameInstallRoots()`.

**Stage 6 — homebrew ROM hosting** *(StF only; optional)*
24. `ElfRom` + the `RomOverride` hook in `file_access`.
25. `HleHooks` table, `Update()`, `ApplyRetarget`, the convention symbols, hit counters.
26. `CharRamFix`. `DebugWindows` + `I960Profile`.

**Stage 7 — netplay** *(everything above must be stable first)*
27. `YampNet.h` ABI + `NetPlugin` loader + layout handshake.
28. `Lockstep` + `PadCodec` + `UdpTransport`. **Prove it over loopback with the command-line harness before writing a line of RPCN.**
29. `TlsClient`, `Protobuf`, `RpcnClient`, `RpcnTransport`.
30. The determinism helpers (`ResetBoard`, `SeedHostRng`, `SetTextureBudgetDeterministic`) and the anchored round-start state machine.
31. `SessionInProgress()` and the suppression of every non-transmitted input.
32. The lobby UI + overlay + desync/peer-lost dialogs.

### Risk register

| Risk | Mitigation |
|---|---|
| A module DLL of a different build | `GameVerify` blocks before `LoadLibrary`. Do not skip it — silent mis-patching is the failure mode. |
| Assuming base `0x180000000` | FV, and both VF5FS LJ/YLAD modules, are ASLR'd. Always resolve against the runtime base; call `SetGameDllRange`. |
| Reusing an sl/gs layout across generations | K2's `+0x3C0` displacement fails **silently** (null file handles, fault far away). One header per generation, every offset `static_assert`ed. |
| MSVC STL version drift | `StfDllMutex` exists because VS2022 17.10+ changed `std::mutex`'s internals. Re-check this if the toolchain moves. |
| Netplay determinism | Not a netcode problem. The three helpers in §14.8, the `IsBoardBooted` gate and the frame-counter anchor are what make it work. |
| "The two peers diverged" | Check they are not simply *numbering* frames differently first (§14.8d). Two bit-identical simulations one frame apart look exactly like a desync, and the give-away is cheap: a peer's canary matching its neighbour's at an ADJACENT frame is not a divergence. |
| Intel integrated GPU | StF's `CreatePipelineState` `E_FAIL`s on Intel iGPUs — force the NVIDIA adapter. (RDP forces Intel.) |
| Shutdown | `wWinMain` still ends in `TerminateProcess` because clean shutdown crashes on mismatched allocators. **Outstanding.** |

---

## 18. Verification methodology used

Worth mirroring, because most of the facts in this document were *measured*, not derived:

- **Automated smoke runs.** `-frames N` gives a deterministic run length **and** exercises the real teardown path. The standard regression is a **StF 2000-frame** and an **FV 600-frame** run under `cdb`, checked for exit 0, `module_stop -> 0x0`, no device removal, and matching draw counts, render-target catalogs and CRI cue sets against a baseline. `de7796c` records FV as **byte-identical** to its pre-change run.
- **Live capture over static reading.** Several K2 facts (execute_info size, pad stride, host-provided gaps) were found by *running and reading the actual fault* rather than extrapolating from another generation. The `0x16E0` size gate was confirmed both statically (`CMP RCX, 0x16E0`) and live under x64dbg.
- **Checking negative results.** The oversized display modes were added back and run to confirm they crop; the mode's effect on the output texture was measured at four different modes.
- **Sampling the emulated CPU to locate a freeze.** A frozen YAMP is usually a *ROM* spin, not a host deadlock (§9.3), so the useful stack is the i960's. Read `ctx = *(DLL + rva_cpu_ctx_ptr)` and sample **`ctx+8`, the i960 IP**, in a loop over a live process; a handful of adjacent IPs cycling is a spin, and the module's own 800-entry symbol table (`DLL+0x1742D0`, `{u64 addr, char* name}`) names the function immediately. Two traps that each cost time here: the StF DLL **is ASLR'd**, so resolve the base from the loaded module list rather than assuming `0x180000000`; and if the debugger is **paused**, `ctx+8` reads constant and is easily misread as "stuck on one instruction" — check the run state before believing a single-valued sample. The GAME ASSIGNMENTS freeze was localised to `print_mes` this way in minutes, after which the ROM disassembly answered the rest.
- **Decoder oracles.** HCA against ClHcaSharp, ADX against ffmpeg — 173/173 and 632/632 bit-exact, then listener-verified.
- **Include-graph-first restructuring.** Every folder move in `bc9bfaf` was justified by the include graph before any file was touched.
- **Remote second box.** A second test machine (`DESKTOP-GHRIIHN`) driven by PsExec + cdb over a share, with token-filter and first-chance-AV traps — which is also the two-machine netplay harness the `-net-*` switches exist for.

---

*Generated from the `multiplayer` branch at `9baa02e`, 2026-08-01. Amended the same day with the
GAME ASSIGNMENTS freeze fix (§9.3, §16) and the i960 IP-sampling method that found it (§18).
Amended 2026-08-08: §14.11 and §16 rewritten for the SRC2 AI-car mechanism read out of the guest
ROM — round-robin CPU-car ownership, the +0xD0 per-frame broadcast, the measured own-echo mirror
contract, and the retirement of the "no authority for a shared field" conclusion
(`docs/src2-netplay-recon.md` §9).*
