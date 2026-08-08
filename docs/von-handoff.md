# Virtual On netplay — handoff prompt (rewritten 2026-08-06, second pass)

Paste this into a new session. Detail lives in `docs/von-netplay-recon.md` and
`docs/von-hle-hooks.md`; this is the map, not the territory.

---

We're building linked-cabinet netplay for Virtual On (`omg`, hosted on Yakuza Kiwami 2) in YAMP.
Architecture, settled and NOT to be re-litigated: **one board per machine, the ROM's own link
protocol carried over the wire, NO lockstep and no determinism requirement.** Lockstep was tried
and failed four times; see the banner in `docs/von-netplay-recon.md`.

## THE LINK WORKS — first verified on loopback 2026-08-06 (historical; the harness is gone)

Two YAMP instances on one machine, 1200 frames each, both logs agreeing:

```
f=261  net=1 id=1 node=1/2         <- "Network Check Success", the ROM's own step 12
f=268  LINKED - accepting the peer's packets | rx=00 seq tx=6A80 rx=155D
f=1080 LINKED ... rx=10 seq tx=F440 rx=9060 | wire tx=2000 rx=843 stamped=812
```

`rx=` is `cRecn+4`, which the ROM writes ONLY by memcpy'ing a validated peer packet over it, so
anything other than 0xFE/0xFF is the ROM saying it accepted what came off the wire. Both cabinets
reach attract (MainMode 1) with the link live and exit cleanly.

The two questions the last handoff opened are both CLOSED, and both were closed by reading the ROM
rather than by guessing:

- **The "+2 sequence number" is not a sequence number.** The receiver's test is
  `packet[+0x556] == packet[+2] ^ 0xAE5E` and both operands come out of the same datagram — a
  self-consistency stamp, not an ordering contract. Loss, duplication and reordering are all legal;
  a gap is detectable rather than fatal. (0x502236 is loaded once and stored never across the whole image.) That does NOT mean packets may be skipped - see the stage handshake below.
- **There is no second cRecn slot to fill.** The ROM writes slot 0 unconditionally and never writes
  slot 1 anywhere. cSend/cRecn are entirely the ROM's own staging — YAMP's whole job is the 0x700
  comm-RAM window between them.

The bug that was actually stopping it: the peer's packet was delivered **before `module_main`**, and
the module's own link transfer runs inside it and copies board 1's (never-executed, all-zero) send
buffer straight over the window.

**That was fixed twice, and the second fix is the one in the code now.** The first was
`DeliverCommPayload`, running from the link-transfer shim immediately *after*
`g_origLinkTransfer()` and writing both banks of board 0's receive slot. The second - `StageCommPayload`
- inverts it: the peer's packet is written into **board 1's SEND buffer** just *before* the transfer,
and the module's own transfer performs the delivery. See "THE MODULE ALREADY DELIVERS IT" in
`von-netplay-recon.md`.

## IT RUNS OVER RPCN, ON TWO MACHINES (2026-08-07)

Room-based, no address configured anywhere. Verified with the direct-UDP harness disabled, so its
own `wire tx/rx` counters stayed at 0 and every byte went through the plugin. That harness has
since been removed entirely - see "One transport, RPCN" below.

- **`kPacketLink`, ABI 9** — a raw datagram channel on the existing P2P socket carrying the ROM's
  0x700 comm-RAM window verbatim. No barrier, no seed, no frame numbering. Works from IN_ROOM
  onwards, because cabinets link during their boot-time check and stay linked between matches.
- **K2Host pumps `poll()` itself** (`DriveNetSession`). Every other game reaches it through
  `NetSession::Drive`, which also runs the lockstep round flow; VON uses none of that, so nothing
  was driving the session and the room could never form.
- **The room assigns the role**: `local_player` 0 = host = MASTER, 1 = guest = SLAVE, applied via
  `SoftResetIntoRole`. Overrides the `VonCabinetRole` setting while a room is up, and a live room
  also implies `VonHoldLink`.
- **Board rate is held at 60 Hz on a live link** (`VirtualClock::BoardFramesDue`). Game speed is a
  competitive advantage between two cabinets, and it also broke the stage handshake - see below.
- **The overlay no longer demands "Start match"** for a game with no round
  (`m2ftg::K2::GetLinkedCabinet`).

## THE STAGE HANDSHAKE, and the bug that hid in it

One cabinet rolls the stage (`rand() & 7` at i960 0xCDB30, gated on `VersusMode != 0`) and publishes
it at payload **+8** alongside state **0x24** at +4; the other adopts both from `cRecn+8` when it
sees 0x24 (0x19770, in `WaitChallenger`). `InitGame` turns it into `FieldNo`.

**State 0x24 lives for a SINGLE board frame.** A cabinet whose board ran at 32 Hz against a peer at
60 read its comm RAM half as often as YAMP replaced the resident packet, so the one frame carrying
the stage could be overwritten before its ROM ever saw it - and the two cabinets loaded different
stages. Equalising the board rates fixed it, confirmed in both logs (`sel=7`/`field=7` on both,
only one cabinet ever showing `tx=24`).

Carry this to Motor Raid and Sega Rally 2: **a single-frame handshake state is only safe between
boards running at the same rate.**

## What works now

- **Cabinet role** — NOLINK / MASTER / SLAVE, a setting (`[Netplay] VonCabinetRole`), applied before
  `module_start` by patching ONE immediate in HLE hook 6's handler.
- **`DriveCommFirmware`** (K2Host) replaces the module's fake comm-board firmware — byte0 ring up,
  byte2 node id (from the ROLE, not the board index), byte3 node count, every `module_main` call.
- **One transport, RPCN.** The direct address:port harness that let two instances run on ONE
  machine is REMOVED (2026-08-07). It existed because RPCN cannot do loopback - it hardcodes 3658
  in the address it hands a peer - and it did its job: the comm-board firmware model, the bank
  discipline and the delivery window were all brought up on it. RPCN is what this ships on, and a
  second transport nothing tests is a second set of behaviours to keep true. Gone with it:
  `VonLinkPeer`, `VonLinkPort`, and YAMP's copy of the plugin's `Transport.cpp`.
- **Transmission gated on `CommBoardReset` bit0**, so "peer up" means the peer's ROM released its
  comm board, not merely that its process is running.
- **Soft reset into another role on a running board** — `SoftResetIntoRole` pulses the TEST switch.
  Proven NOLINK → MASTER on a live board; still not driven from the settings UI at runtime.
- **`[von]` diagnostics in `yampnet.log`** behind "Log linked-cabinet state", on change plus every
  120 frames. Includes the packet's `seq/check` pair at every point on its path
  (`send0`/`send1` → `data0`/`data1` → `stage`), `*` marking a pair that passes the ROM's test.
  This row is what diagnosed the delivery bug in a single run — do not remove it.

## THE VERSUS HANDSHAKE ALSO WORKS — the cabinets go into a match together

The payload's `+4` field is each cabinet's current mode handler, published to the ring every frame,
and it is the entire versus protocol. `0x10` = `Advertize`, **`0x20` = `WaitChallenger`**, `0x21` =
`SelectV`, `0x30`/`0x31` = `InitGame`/`Game_01`. The scan loop inside `Advertize` (i960 0x2BADC)
hunts the receive slots for a peer in state `0x20` — that is the "a challenger is waiting"
detection, and the banner behind it is a graphic, which is why searching the ROM's strings for it
was never going to work.

Measured: master alone presses START → publishes `0x20` → the slave's attract scan finds it and
answers `0x21`. Both press START → both walk `0x30` → `0x31` together. YAMP drives none of it.

**The START crash did not reproduce** in three cdb runs (standalone, master-only, both), all clean
`module_stop -> 0x0`. The 23 first-chance AVs at `omg+0x6D9F9` are baseline — identical count in the
standalone control. Judge a run by its exit, not its AV count. If it recurs, note which cabinet,
which screen, and whether the log's last `[von]` line shows the peer's state.

## Next, in order

1. ~~**TWO MACHINES.**~~ DONE 2026-08-07, over RPCN, with a match played through. The role comes
   from the room, so the only per-machine setting left is the RPCN account.
2. **Play the match out.** The pair enters `Game_01` together; nobody has played through a round,
   checked that both pods see the right inputs, or watched a round end. Freeplay ON on both (the
   coin line is dead during netplay).
3. **THE STAGE HANDSHAKE NEEDS EVERY PACKET, IN ORDER.** `WaitChallenger` (i960 0x196C0) makes a
   ONE-SHOT decision on the peer's state byte and advances `SubMode` in every branch but the
   `0x21` wait; the roller publishes `0x24` for a single board frame. Miss it and the adopter takes
   the `0x20` branch, keeps its own stage, and the cabinets diverge with everything else looking
   healthy. Hence the ordered receive queue and the one-take-per-board-frame rule. Assume any
   linked-cabinet game does the same until its state machine has been read.
4. ~~**Latency.**~~ Payload now RLE-codes to ~120 B a datagram (max 347) from 1792, so nothing
   fragments and traffic is ~10x lower. Sent 3x (`kLinkRedundancy`). Untested against real internet
   RTT and jitter; the LAN numbers say nothing about that.
5. ~~**Room join.**~~ DONE - the room assigns MASTER/SLAVE and calls `SoftResetIntoRole` itself.

## The harness

`-von-autostart=<frame>` presses START on pad 0 at a known host frame — the only way to make
"pressing START does X" repeatable, bisectable, and simultaneous on two cabinets. `-von-padtest`,
`-von-render1`, `-von-2board`, `-von-2probe` (the latter two also switch on the heavy `[vonmap]`
work-RAM hashes) are the older aids. Two machines are now the only way to test a link:
`-net-host` on one and `-net-join <roomId>` on the other, credentials from settings.ini.

## Settings: what Virtual On actually honours

- **Difficulty works** — `config+4`, +1, through a five-entry table (`{3,3,0,1,2}` at RVA 0x4500E0)
  into backup `0x1D00021`, which the ROM decodes 0=NORMAL 1=HARD 2=VERY HARD 3=EASY. YAMP's 0..3
  lands on EASY/NORMAL/HARD/VERY HARD in order. Verified against the cabinet's own menu.
- **Region does not exist** and the combo is hidden for this game. The module never reads
  `config+5`; there is no region string, no operator-menu item, no backup byte, and the ROM
  (`SEGA.AM#3.VRON.`) is already the English set.
- **The boot notice is not a region tell.** `0x5024D4` has three accesses in the whole ROM (read,
  set-after-display, cleared by BlackOut). Only HLE hook 5 pre-sets it.
- **Versus Mode is unverified** — `config+0xA` shows zero direct references, but that is one weak
  signal and pointer access would not show, so it was left alone.

## Traps that cost time — do not re-learn these

- **A second run directory needs a HARDLINK to `YakuzaKiwami2.exe`** (GameVerify's parent-game
  check) and a JUNCTION for `m2ftg` (442 MB — never copy). Missing the parent exe = silent exit, no
  log at all, looks exactly like a crash. `build/bin/Win64/Debug2` is set up this way already.
- **YAMP is a `WindowedApp`**: stdout is always empty. `net::Logf` → `yampnet.log`; `DebugLogFile`
  → only visible under cdb. Put anything you need to read in `net::Logf`.
- **An explicit settings.ini value beats a compiled-in default.**
- **HLE hook indices were renumbered 2026-08-05** (table base 0x476520, 120 hooks). Every index in a
  note older than that is ONE TOO HIGH.
- **TEST is a momentary push, not a hold.** **`MainMode` is a request, not a jump.**
- **Never express "no link" by withholding flag bit7** — that poll has no frame yield and hangs the
  whole host. Ring-up (byte 0) is the only safe place to say no.
- **Holding the ring starves the module's own bring-up** — `DriveCommFirmware` writes DLL flag
  `0x6910DF` itself while the ring is down, because the ROM cannot reach hook 0 until the check
  completes.
- **Only the SLAVE looks different on screen.** A master is pixel-identical to a standalone cabinet.
- **The module's own link transfer is a WRITER into CommData** on every frame-driver tick, even with
  one board. Anything YAMP puts there before `module_main` is gone.

## Tools

- **IDA on port 7331** = `von_prog.bin`, the i960 ROM. Data xrefs do NOT exist for i960 absolute
  stores — scan the image for the address as a little-endian dword instead (`run_python` +
  `idaapi.get_bytes(0, 0x200000)`); that is how the +0x556 field and the empty slot 1 were settled.
- **Ghidra on port 5678** = `omg-pxd-w64-gog_retail.dll`.
- **x64dbg bridge on port 3000**, JSON-RPC over POST `/`. `bph <addr>, w, 1` for write watchpoints.

## Files

`source/m2ftg/K2/K2Host.cpp` (cabinet role, CommFirmware, soft reset, `ExchangeCommPayload` /
`StageCommPayload`, `LogLinkState`, `GetLinkedCabinet`), `plugin/yampnet/` (`kPacketLink`),
`source/m2ftg/VonHooks.inc` (120 classified hooks), `docs/von-hle-hooks.md`,
`docs/von-netplay-recon.md`.
