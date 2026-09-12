# Proving ownership through Steam

Added 2026-09-10 on the `steamworks-ownership` branch.

## The problem it solves

YAMP refuses to host an arcade module unless the user owns the Yakuza title it came from. Until
this branch the only proof it accepted was **finding the title's executable on disk** and
identifying it by PE header (`source/GameVerify.cpp`). That is fine for an installed game, but it
made people who had moved or uninstalled the parent game copy `LostJudgment.exe`,
`YakuzaLikeADragon.exe` and friends next to `YAMP.exe` purely to satisfy the check — a
400 MB executable kept around as a licence token.

Steam already knows the answer. The client holds every licence the signed-in account has, and the
Steamworks SDK exposes it as `ISteamApps::BIsSubscribedApp(appId)`, whose header comment says it
exists precisely to "check ownership of another game related to yours". So the gate now accepts
**either** proof:

| proof | what has to be on disk | who it serves |
|-------|------------------------|---------------|
| the signed-in Steam account owns the title | only the arcade module folder (DLL + `rom/` + `w64/` or `sound/`) | every Steam owner |
| the title's executable is found and identified | the game, or its executable | GOG installs, offline machines, no Steam |

A verified executable still wins the *label* (it says which build is installed, which a licence
cannot), and a present executable of an unrecognised build still only warns. Neither proof =
block, exactly as before.

## How it works

`source/SteamOwnership.{h,cpp}` (namespace `Steamworks`):

1. `steam_api64.dll` is loaded **by hand** from the folder `YAMP.exe` is in, by absolute path.
   It is never linked: a missing DLL costs nothing but this feature, so a GOG-only user never needs
   Steam, and `YAMP.exe` starts without it.
2. `SteamAppId` and `SteamGameId` are set to **480** for the duration of the query and restored
   afterwards. 480 is Spacewar, Valve's test app that every account may use; it is the documented
   way for something that is not itself a Steam product to talk to the client. (The account shows
   as "playing Spacewar" for the milliseconds the session is open.)
3. `SteamAPI_InitFlat` connects — the export meant for callers that loaded the DLL themselves, and
   the one that explains a failure ("Steam is not running", "the Steam client is out of date").
4. `ISteamApps` (`SteamInternal_FindOrCreateUserInterface` with the header's interface version
   string) answers `BIsSubscribedApp`, `BIsAppInstalled` and `GetAppInstallDir` for every app id
   the parent tables name — one connection for all of them. `ISteamFriends::GetPersonaName` and
   `ISteamUser::GetSteamID` are read for the UI and the log.
5. `SteamAPI_Shutdown`. The whole thing happens before any window or D3D device exists, so the
   Steam overlay has nothing to attach to.

The answers are cached for the process; the launcher's **Rescan** calls
`Verify::RefreshSteamOwnership()` so someone who signed in after opening YAMP gets a fresh answer.
`-nosteam` on the command line skips the client entirely, which is how the executable-only path
is exercised on a machine that has Steam. `-noownership` (and `-noverify`, which implies it) skips
it as well, and for a stronger reason: the only thing YAMP ever asks the client is the ownership
question those switches waive, so the session would open, name the account, and be thrown away.
The failure line then reads `not asked - ownership was bypassed on the command line`, and
`steam_api64.dll` is never loaded into the process at all.

The Steamworks SDK comes in as a git submodule, `external/SteamworksSDK`
(https://github.com/rlabrecque/SteamworksSDK, v1.65 at the time of writing). Only its headers are
compiled against (`externalincludedirs` in `premake5.lua`, warnings off), and the post-build step
copies `redistributable_bin/win64/steam_api64.dll` next to the exe. The CI artifact carries the
DLL for the same reason.

## App ids

Checked against the store pages on 2026-09-10. Kiwami 2 has **two** listings: the original
(927380) was renamed "Yakuza Kiwami 2 (Legacy)" when a new listing (3717340) appeared, and a
licence for either counts. Note the Kiwami 2 modules YAMP hosts are the **GOG** build: a Steam
owner passes ownership and then meets the module hash gate, which is a separate question and
unchanged.

| title | app id(s) |
|-------|-----------|
| Lost Judgment | 2058190 |
| Yakuza: Like a Dragon | 1235140 |
| Yakuza 6: The Song of Life | 1388590 |
| Yakuza Kiwami 2 | 927380, 3717340 |
| Like a Dragon Gaiden: The Man Who Erased His Name | 2375550 |

They live in `source/GameVerify.cpp` next to the executable identities, one `steamAppIds` list per
`ParentTitle`.

## Where it shows

* **Launcher** — redesigned around the two questions the gate asks. It is a tree: each top-level
  row is a parent title with its ownership verdict (`Installed`, `Owned on Steam`, `Installed,
  owned on Steam`, `Installed, unrecognised version`, `Not owned` when Steam answered, `Not found`
  when it could not) and where that came from (the install folder, or the Steam account); the
  rows beneath are the games that title supplies, each with its module verdict (`Verified`,
  `Not found`, `Wrong build`, `Outdated build`, `Unreadable`) and where the module was found.
  Ownership is checked once per title even when none of its modules turned up, because "you own
  it, copy the module folder here" is the useful thing to say in that case. The details block
  under the tree ends with one **what-to-do** sentence for any row that cannot play: which
  folder to copy (named from the title's own install layout, e.g. `runtime\media\m2ftg`), which
  account to sign in with, or which game to update. The line under the heading says whether Steam
  answered; Rescan asks again.
* **About panel** (F1): the same verdict for the running game.
* **Log**: `[steam] app <id>: owned/not owned, installed at …`, `[steam] signed in as …`,
  `[launcher] title <name>: <verdict>`, and the existing `[verify] parent game:` line now ends
  with `; owned on Steam` when it applies.
* **The refusal boxes** at boot all have one shape now — what happened, what YAMP found, what to
  do — for the three module verdicts, the missing-module case in `ModuleLoad.cpp`, and the
  ownership refusal, which lists both proofs (the signed-in account does not own it / Steam could
  not be asked; no executable anywhere it looked) and the three remedies.

## Verified on 2026-09-10

* Debug build, Steam running: `-stf -frames 300` from a scratch folder holding only `YAMP.exe`,
  `steam_api64.dll` and the copied `m2ftg/` module folder (no parent executable anywhere the
  search looks) boots, runs 300 frames and exits through `module_stop`, with the log reading
  `parent game: Owned on Steam (executable not located; owned on Steam)`.
* Same folder with `-nosteam`: blocked, `parent game: Not found`, the refusal explains that Steam
  was disabled — the pre-existing behaviour, intact.
* From the build folder, where the executables do sit next to `YAMP.exe`: `Verified — Lost
  Judgment 1.0.0.12 (Steam) (…; owned on Steam)`, i.e. the executable still wins the label.

## Verified on 2026-09-11 (the command-line bypasses)

* `-stf -frames 100 -noownership`, module folder only: `parent game: Not found`, then
  `*** ownership gate BYPASSED on the command line (-noownership) ***`, and the game runs to
  `module_stop -> 0x0`. `steam_api64.dll` does not appear in the debugger's module list, where an
  unswitched run of the same folder loads it and reports `Owned on Steam`.
* A copy of the StF module with **one byte appended** (SHA-256
  `E41C1239…`, 2086897 bytes) under `-noverify`: `module … Unrecognised build`, both bypass lines,
  and `module_start` runs. The same scratch folder crashes identically with the *pristine*,
  fully-verified DLL, so the fault is the incomplete folder, not the bypass.
* Unswitched `-stf -frames 300` before and after the change: identical — `Verified — Lost Judgment
  (2022-11-21)`, `Owned on Steam`, `module_stop -> 0x0`, exit 0, the same six first-chance AVs.
* The launcher started as `YAMP.exe -noverify`: no `steam_api64.dll`, ownership falls back to the
  on-disk search (Like a Dragon Gaiden `Verified`, Lost Judgment `Not found`), and the rows the
  missing proof would have blocked stay playable.

## Trap found on the way

A folder reached through a **junction** is canonicalised by the search (`fs::weakly_canonical` in
`AddDir`), so a scratch junction into `build/bin/Win64/Debug/m2ftg` still found the
`LostJudgment.exe` sitting in `build/bin/Win64/Debug` and reported Verified. A real copy of the
module folder is what tests the Steam-only path.
