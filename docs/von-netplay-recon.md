# Virtual On (`omg`) netplay

## *** THE LINK RUNS OVER RPCN (2026-08-07) ***

Two machines, an RPCN room, and the cabinets link with **no direct address configured anywhere**.
Verified with the direct-UDP harness deliberately disabled (`VonLinkPeer=` empty on the joiner), so
`wire tx/rx` — VonLink's own counters — stay at 0 on both sides and every byte that moved went
through the plugin:

```
host   [von] f=6840 [rpcn] LINKED | net=1 id=1 node=1/2 | wire rx=0 stamped=3031
join   [von] f=1920 [rpcn] LINKED | net=1 id=2 node=2/2 | wire tx=0 rx=0 stamped=2486
```

`netplay plugin loaded (ABI 9)`, `room 91 ready (hosting)`, and the peer-state field walks the real
mode sequence (`0x30`/`0x31`/`0x40`/`0x60`/`0x61`) throughout.

### THE LOOPBACK HARNESS IS REMOVED (2026-08-07)

`VonLink` - the direct address:port transport that let two YAMP instances on ONE machine exercise
the link - is gone, along with `VonLinkPeer`, `VonLinkPort`, YAMP's copy of the plugin's
`Transport.{h,cpp}` and the `ws2_32` link on the YAMP project. RPCN is the protocol this ships on.

It earned its keep: RPCN cannot do loopback (it hardcodes 3658 in the address it hands a peer), and
the comm-board firmware model, the bank discipline, the delivery window and the whole `[von]` probe
were all brought up on one machine because of it. But a second transport that nothing tests is a
second set of behaviours to keep true, and the two paths had already started to diverge - the
harness aged its peer timeout internally while the RPCN path aged it in K2Host, and the probe had
to carry a `[udp]`/`[rpcn]` tag to say which set of rules was in force.

**Testing a link now requires two machines**: `-net-host` on one, `-net-join <roomId>` on the other,
credentials from `settings.ini`. That is a real cost and it is the right trade - three of the four
historical failures were invisible single-machine anyway.

### What was added

**A raw datagram channel on the existing P2P socket — `kPacketLink` (ABI 9).** It carries the ROM's
0x700 comm-RAM window verbatim and touches nothing else: not the input rings, not the barrier, not
the state-check machinery. Three entry points (`link_ready` / `link_send` / `link_take`), one
newest-wins inbound slot, and no round, generation or frame numbering anywhere near it.

Three details that are not incidental:

- **It must work from IN_ROOM, not from a round.** Cabinets link during their boot-time network
  check, which happens as soon as both are present and long before anyone presses Start, and they
  stay linked between matches. Gating it on a match would mean it could never come up at all.
- **The receive buffer was 512 bytes.** A `LinkPacket` is 1808. `recvfrom` does not truncate — it
  drops the remainder and reports `WSAEMSGSIZE` — so an undersized buffer would have made link
  packets vanish while every lockstep packet kept working.
- **`link_ready` is "the peer's address is known", which is the right gate for SENDING and the
  wrong answer for "is the ring up".** A cabinet that has stopped sending still has an address, so
  K2Host applies the same 30-host-frame liveness rule VonLink uses internally; without it a dead
  ring would never be reported and the ROM would never re-run its check.

**K2Host now pumps the session itself.** Every other game reaches `poll()` through
`NetSession::Drive`, which also runs the whole lockstep round flow. Virtual On uses none of that,
so K2Host never calls NetSession — and that left the session's connect, its RPCN signalling, its
transport update and its socket drain with nothing driving them. `DriveNetSession` is one
`poll()` + `DriveSession()` per host frame, and it is what makes the room form at all, on the lobby
path as well as the command-line one.

**The ROOM decides the cabinet role**: `local_player` 0 = host = MASTER, 1 = guest = SLAVE, applied
through `SoftResetIntoRole` on the transition. While a room is up this overrides the
`VonCabinetRole` setting entirely — two players who both picked MASTER would otherwise get no link
and no explanation. A live room also implies `VonHoldLink`: joining a room *is* the statement that
another cabinet exists.

### HOW THE STAGE IS AGREED — and where a stage desync can come from

Read out of the ROM after the two cabinets came up on DIFFERENT stages over RPCN (they had agreed
over the direct-UDP LAN link, on the same two machines).

**One cabinet rolls the stage; the other adopts it out of the link payload.** The decider is
`VersusMode` (0x503A7C) at i960 0xCDB0C:

```
CDB0C  ld   0x503A7C, g5        ; VersusMode
CDB2C  be   loc_CDB68           ; == 0 -> ADOPT
CDB30  bal  sub_F5058           ; != 0 -> ROLL: rand()
CDB34  and  g0, 7, g0           ;   & 7
CDB38  ld   0x19430[g0*4], g4   ;   stage table
CDB40  st   g4, 0x503A84        ;   my stage
CDB54  stos r7, 0x5032F4        ;   publish state 0x24 at cSend+4
CDB5C  stos g4, 0x5032F8        ;   AND THE STAGE AT cSend+8
```

and the adopting side, in `WaitChallenger` (0x19744), on seeing the peer's state == 0x24:

```
19750  st   g1, 0x503A7C        ; VersusMode = 1
19768  ldis 0x5024FC, g5        ; cRecn+0xC
19770  ldis 0x5024F8, g6        ; cRecn+8  = the peer's stage
1978C  st   g6, 0x503A84        ; adopt it
```

`InitGame` then reads 0x503A84 into `FieldNo` (0x5770F0) at 0x198AC. So the stage rides the link
at payload **+8**, alongside the handshake state at **+4**, and both come out of the same datagram.

**Which means a stage disagreement is one of two things**, and they are distinguishable:
- *both cabinets rolled* — both had `VersusMode != 0` at 0xCDB0C, each published its own stage, and
  each adopted the other's. Symptom: two different stages, each one legitimately drawn.
- *the adopter never saw the 0x24 packet* — a single-frame handshake state that was overwritten
  before the peer's ROM read its comm RAM.

**DIAGNOSED AND FIXED: it was the SECOND case, and the cause was the frame-rate mismatch.** The
probe was extended to log this cabinet's own state and stage (`tx=`, `stage tx=`, `sel=`, `vs=`,
`field=`) next to the peer's, and with both boards held at 60 Hz (below) the handshake is visible
completing across the wire:

```
host   f=7994  tx=24 rx=20 vs=1 sel=7 field=2  stage tx=0007 rx=0000   <- rolled stage 7, published it
host   f=7996  tx=00 rx=20 vs=1 sel=7 field=7  stage tx=0007 rx=0000
join   f=2463  tx=00 rx=24 vs=1 sel=7 field=13 stage tx=0000 rx=0007   <- saw the 0x24, adopted 7
join   f=2464  tx=00 rx=00 vs=1 sel=7 field=7  stage tx=0000 rx=0007
```

Both `sel=7`, both `field=7`, and only ONE cabinet ever showed `tx=24` - so they were never both
rolling. The adopter had simply been missing the packet: **state 0x24 lives for a single board
frame**, and a cabinet whose board runs at 32 Hz reads its comm RAM half as often as YAMP replaces
the resident packet at 60 Hz, so a one-frame state can be overwritten before its ROM ever sees it.
Equalise the board rates and it cannot happen.

That also explains why it worked over the LAN and not over RPCN on the same two machines: it was
never the transport. The LAN runs happened before the second cabinet was left at 32 Hz for a whole
match; nothing about the wire changed.

Two things to keep from this:
- **A single-frame handshake state is only safe between boards running at the same rate.** Any
  future linked-cabinet game (Motor Raid, Sega Rally 2) inherits this hazard exactly.
- The host's `rx=` going to non-protocol values (`63`, `B7`, `94`...) in the failing run is
  explained by the same thing - it was reading a resident packet the peer had long moved past -
  and NOT by corruption. Ruled out at the time and worth keeping ruled out: `RpcnTransport::Recv`
  and `RpcnClient::RecvFrom` pass the caller's capacity straight to `recvfrom`, so the 1808-byte
  payload is not truncated on the way in.

### GAME SPEED IS A COMPETITIVE ADVANTAGE, so the board rate is now held at 60 Hz on a live link

`PaceToVirtualTime`'s policy is "a host that cannot keep up runs slow rather than stuttering", which
is right for one player and backwards for a linked pair: the cabinet presenting at 32 Hz runs its
BOARD at 32 Hz, so its pilot moves, turns and fires at half the rate of the one at 60 and loses for
a reason that has nothing to do with the match. Measured exactly that way on the two-machine
harness, where the second box renders into an RDP virtual display capped at 32 Hz.

`VirtualClock::BoardFramesDue(linkLive)` is an ordinary fixed-timestep accumulator: a host frame
that overran its 1/60 s budget steps the board more than once to make up the difference. Capped at
4 (1/15 s of catch-up), past which the debt is dropped rather than carried - a long hitch must not
become a burst that freezes the host. **Solo play keeps the old policy unchanged**, since nobody is
disadvantaged there but the player who chose the settings.

Safe here for exactly the reason it was unsafe under lockstep: there is no frame numbering to
violate and no peer to stay bit-identical with.

### THE NEXT PROBLEM: the payload fragments

`LinkPacket` is 1808 bytes on the wire against a 1500-byte path MTU, so every link datagram is IP-
fragmented, and the exchange runs once per `module_main` call (~2 per frame) — roughly 200 KB/s
each way. A LAN carries that without complaint, which is what the run above proves and all it
proves. Fragmented UDP is dropped far more readily on the open internet, and a lost fragment loses
the whole datagram. Two obvious levers before internet play is worth attempting: send only when
the ROM has stamped a NEW packet (the `+2` counter makes that free to detect, and it roughly halves
the rate), and delta-code the payload against the last one acknowledged.

## *** THE LINK CARRIES DATA. BOTH CABINETS LINK AND ACCEPT THE PEER'S PACKETS (2026-08-06) ***

Two YAMP instances on one machine, loopback, 1200 frames each. VERIFIED IN `yampnet.log` on BOTH
sides - not inferred from transport counters and not read off a screen:

```
f=240  link check has NOT completed | net=0 id=1 node=0/0 rx=FE | wire tx=320 rx=4
f=261  checked - no packet accepted  | net=1 id=1 node=1/2 rx=FF | ... send1=155D/BB03*
f=268  LINKED - accepting the peer's packets | net=1 id=1 node=1/2 rx=00 seq tx=6A80 rx=155D
       | data0=155D/BB03* data1=155D/BB03* stage=155D/BB03*
f=1080 LINKED ... rx=10 seq tx=F440 rx=9060 | wire tx=2000 rx=843 stamped=812
```

`net=1` is "Network Check Success" - the ROM's own step 12. `rx=` is `cRecn+4`, which the ROM only
ever writes by memcpy'ing a **validated** peer packet over it, so `rx=00`/`rx=10` (rather than 0xFE
or 0xFF) is the ROM saying it accepted what came off the wire. `seq rx=` tracks the peer's `seq tx=`.
Sustained from frame 268 to the end of the run on both cabinets.

### The packet, decoded - and the "sequence number" is NOT one

This was the standing blocker, recorded as "the packet has a running sequence and we violate it".
It is not a sequence number, and the transport needs no ordering or reliability. Read out of the
vsync path at i960 0x14A0:

```
14B4  ldos 0x5032F2, g5           ; counter at cSend+2
      g4 = 1823 * (g5 + 3)
14F8  stos g4, 0x5032F2           ; new counter into the OUTGOING packet, +2
1500  stos g5, 0x503846           ; g4 ^ 0xAE5E into the SAME packet, +0x556
1508  memcpy(CommSend <- cSend, 0x700)
150C  memcpy(0x501CE0 <- CommData, 0x700)
1524  g4 = staged[+2] ^ 0xAE5E
152C  g5 = [0x502236]             ; = 0x501CE0 + 0x556 -> staged[+0x556]
154C  if (g4 - g5) & 0xFFFF  ->  REJECT
1550  memcpy(cRecn <- 0x501CE0, 0x700)
```

**Both operands come out of the datagram in hand.** 0x503846 is `cSend + 0x556` and 0x502236 is
`staging + 0x556` - the same field of the same packet, send side and receive side. Scanned across
the whole 2 MB image: 0x502236 is loaded exactly once and stored never; 0x503846 is stored exactly
once and loaded never. So the test is `packet[+0x556] == packet[+2] ^ 0xAE5E`, a self-consistency
stamp that says "this is a real packet and not stale DPRAM". Gaps, duplicates and reordering are all
legal. **Newest-wins ingest is correct**, and the "it worked once because loopback does not drop"
theory was wrong about the mechanism.

(How to check a claim like this cheaply: i960 absolute stores have no data xrefs, so scan the image
for the ADDRESS as a little-endian dword. One hit = one instruction touches it.)

### There is no second slot to fill, either

The other open suspicion - "cRecn has TWO 0x700 slots and we never write the peer's echo into the
second" - is also closed. The vsync path writes slot 0 unconditionally, and **nothing in the ROM
ever writes slot 1**: 0x502BF0 does not appear in the image at all, and 0x502BF4 appears once, as
slot 1's state field being preset to 0xFE when the link is down (i960 0x1490). The node-scan loop at
0x2BADC walks slots `nodes-1 .. 0`, so on the two-node ring `Net_check` insists on it reads a
permanently empty slot 1 and then the peer in slot 0. The array is vestigial N-node scaffolding.

Corollary: **cSend/cRecn are not YAMP's business at all.** The ROM does both halves of the staging
itself. YAMP's entire job is the 0x700 comm-RAM window in between.

### THE ACTUAL BUG: the peer's packet was being delivered into a window the module then overwrote

With the ring up, `net=1` on both cabinets and valid stamped packets arriving, the ROM still
rejected every one of them - `stage=0000/0000`, `rx=FF`, cRecn never written in 1200 frames. Two
mistakes, and the probe named both:

1. **Wrong bank.** The delivery targeted the bank the reader is NOT on and then flipped bit0, which
   is the module's own transfer discipline. But the ROM's vsync does its send and receive memcpys
   back to back through the same guest window, so it reads CommData from the bank it just wrote
   CommSend into - `flag & 1`. Measured directly: `flag=01 send0=113D/BF63 send1=DD60/733E tx=DD60`
   and `flag=00 send0=FAE0/54BE send1=F060/5E3E tx=FAE0`. The flip did not stick either; the
   module's transfer rewrites the flag register every frame.
2. **Wrong moment, which is the bigger one.** Delivery ran before `module_main`. The module's own
   link transfer runs INSIDE it, before either board's CPU steps, and on a one-board cabinet it
   copies board 1's send buffer - never executed, 0x700 zero bytes - straight over the peer's
   packet. So the ROM staged zeros and rejected them, exactly as the log said.

FIX: `DeliverCommPayload`, called from the link-transfer shim immediately AFTER
`g_origLinkTransfer()` and before the CPU steps - the one window in the frame where the packet
survives to be read. It writes **both banks** and touches the flag register not at all: the bank
selector is the module's business, and writing both removes the question rather than answering it.
Nothing is lost, because the two banks exist to stop a DMA tearing against a CPU read and YAMP
writes between emulated instructions. bit7 (data ready) stays the module transfer's job, which is
what `Net_check` step 9 waits for.

The received packet is also held RESIDENT and re-laid after every transfer rather than consumed on
delivery - otherwise the next frame's transfer zeroes it. That is what a DPRAM does between
arrivals, and it makes a dropped datagram cost nothing.

### THE PAYLOAD'S +4 FIELD IS THE CABINET'S GAME MODE, AND IT IS THE WHOLE VERSUS HANDSHAKE

`cSend+4` (0x5032F4) is written from ~60 sites, and bracketing every one of them against the ROM
symbol table (`docs/von-rom-symbols.md`) shows what it is: **each cabinet publishes its current mode
handler to the ring, every frame.** `cRecn+4` (0x5024F4) is the peer's, read in ~20 places.

| value | written by | meaning |
|---|---|---|
| 0xFE | i960 0x1488 | sentinel: this cabinet's `net_flag` is clear |
| 0xFF | i960 0x1594 / 0x15A0 | sentinel: ring down, or the packet was rejected |
| 0x10 | `Advertize` (0x2B9E0) | attract |
| 0x20 | `InitWaitChallenger` (0x19660), `WaitChallenger` (0x196C0), `GameMain` (0x19180) | **waiting for a challenger** |
| 0x21 | `SelectV` (0x18DA0) | character select |
| 0x23 | `InitSelectV` (0x18C00) | entering select |
| 0x30 / 0x31 | `InitGame` (0x19830) / `Game_01` (0x1A280) | the match |
| 0x13 / 0x14 | `Game_20` (0x1AFE0) / `Game_30` (0x1B470) | later match phases |
| 0x40-0x43 | `Game_10` (0x1A4A0) | |

**0x20 is the value the attract loop hunts for.** The node-scan at i960 0x2BADC — which is *inside
`Advertize`* — walks the receive slots `nodes-1 .. 0` looking for one whose +4 is 0x20, and on a hit
writes the slot index into the comm flag register (`stob g6, (g1)`, guest 0x1A14002); `sub_72EA0`
and 0xD3860 later read it back with `& 1` to pick which slot to display from. That is the
"another cabinet wants a match" detection — the thing behind the banner, which is a graphic and not
a string, so it was never going to be found by searching the ROM's text.

MEASURED, both cabinets, and this is the versus handshake running for real:

```
master alone presses START:
  master publishes 0x20 (WaitChallenger)  ->  slave's Advertize scan finds it
  slave answers 0x21 (SelectV)            ->  master reads rx=21
both cabinets press START:
  both walk 0x00 -> 0x30 -> 0x31, i.e. InitGame -> Game_01, together
```

The master also reached `VersusMode` (0x503A7C) = 1. Nothing in YAMP drives any of this — the ROM's
own protocol does it, which is the whole premise of the architecture.

### THE START CRASH DID NOT REPRODUCE

Reported as "pressing START crashes YAMP/Virtual On during attract". Three runs under cdb
(`sxd av`), driven by `-von-autostart` so the press lands at a known frame:

| run | result |
|---|---|
| standalone NOLINK, START at f=900 | clean, `module_stop -> 0x0`, 23 first-chance AVs |
| linked pair, MASTER presses at f=1400 | clean, `module_stop -> 0x0`, 23 AVs, reached `vs=1` |
| linked pair, BOTH press | clean, `module_stop -> 0x0`, 23 AVs, both reached state 0x31 |

**The 23 first-chance AVs are baseline noise, not the crash**: identical count in the standalone
control and in both linked runs, all at `omg+0x6D9F9` reading guest 0x5024E8, all continued. Judge a
run by its exit, not by its AV count.

`-von-autostart=<frame>` exists because this is otherwise unmeasurable: a `-frames` run never
presses anything, and a human pressing a button under a debugger cannot be repeated, bisected, or
fired at a known frame on both cabinets. The press is injected before the coin/start dance, so
freeplay and everything downstream see exactly what a real press produces.

One thing that LOOKS like a fault and is not: the master logs `peer went quiet ... ring DOWN` and
parks with `rx=FF` when the slave's `-frames` budget runs out. Check the timestamps against the
other cabinet's last line before calling it a bug.

### Observability, and one regression it exposed

`[von]` lines now go to **`yampnet.log`** via `net::Logf`, behind the existing "Log linked-cabinet
state" setting, logged on change plus every 120 frames. Fields: the ROM's verdict in words,
`net`/`id`/`node`/`main`, `rx=` (cRecn+4), the tx/rx counters, wire totals, and the packet's
`seq/check` pair at every point along its path (`send0`/`send1` -> `data0`/`data1` -> `stage`),
with `*` marking a pair that satisfies the ROM's test. That last row is what turned "it does not
work" into two specific mistakes in one run.

Two fixes came with it:
- **`[vonmap]` no longer rides on that setting.** It is 32 CRC32s over 64 KB twice per frame - a
  lockstep-era desync tool with nothing to diff under the current architecture - and it buried the
  link lines in a 1.3 MB log. Command line only now (`-von-2board` / `-von-2probe`).
- **The peer timeout was counted in `module_main` calls, not frames.** `StepOneBoardFrame` makes one
  to sixteen calls per host frame, so the "30 frame" timeout was of unknown, machine-dependent
  length - the mechanism behind "peer went quiet for 30 frames" on a link that was never down, and
  every flap re-runs somebody's `Net_check`. `VonLink::Tick()` is now called once per host frame.

### What is NOT yet done

- **Two machines.** Everything above is loopback on one box. Three of the four historical failures
  were invisible single-machine; that lesson stands.
- **Gameplay.** Both cabinets reach MainMode 1 (attract) with the link live. Nobody has played a
  match, so `WaitAnother` / `WaitChallenger` and the in-match exchange are unexercised.
- **RPCN transport.** `VonLink` is a direct-address UDP harness. Room join still has to call
  `SoftResetIntoRole` and hand the link a peer.
- **Latency.** Loopback has none. The ROM's protocol expects a partner one frame away on a serial
  ring; how it behaves at LAN and then internet RTT is untested.

## *** THE BOOT-TIME LINK WAIT NOW WORKS, AND IT IS NOT AN HLE HOOK (2026-08-06) ***

Three things together make Virtual On boot like a linked cabinet and hold for its partner. All
three are settings; none needs a command-line flag.

1. **HLE hook 5 off — now the DEFAULT.** It pre-marks the ROM's warning screen as already seen
   (guest `0x5024D4`), which is the whole of why the board appeared to boot straight to the SEGA
   logo. `Warning` is *MainMode 0*, the first entry of the mode table at 0x18680, with `Advertize`
   at [1]. Off, the notice holds for the ROM's own `0x234` = 564 frames. Confirmed on screen.
2. **Cabinet role = MASTER or SLAVE** (`[Netplay] VonCabinetRole`). Standalone (`link_ID` 3) exits
   `Net_check` early; 1 or 2 runs the full check.
3. **"Wait for the other cabinet at boot"** (`[Netplay] VonHoldLink`) — the piece that was missing.

**The link check is NOT hooked, and it runs BEFORE the warning screen.** `BlackOut` is explicit:

```
1871C  st   g14, 0x5024D4     ; hook 5's site - the warning flag
18784  call sub_186C0         ; link_ID = 3, net_flag = 0   (re-arm the check every BlackOut)
18788  call sub_18960         ; InitAll
1878C  call sub_18A10         ; InitNetwork -> Net_check    <-- THE LINK CHECK
187E0  bal  sub_F50A8         ; hook 0
187E4  __mainloop__           ; MainMode 0 = Warning
```

Every hook in the `Net_check` address range is Inert, so no combination of hooks can change what
the check does. **What makes it succeed with nothing connected is the module's comm-board device
model**, now measured rather than inferred:

```
[comm] B0 reset=01 flag=00 bank0=01000102 bank1=01010102
                                 ^^  ^^^^
                              byte0=1  byte2=1  byte3=2
```

Those are the FIRMWARE status bytes - ring up, node id, node count - which the ROM only ever READS.
The module leaves a fake healthy TWO-NODE ring there, so the check polls "ring up?" (yes) and "two
nodes?" (yes) and succeeds against a wire that does not exist. (byte1 = 1 in bank1 is the ROM
writing its own `link_ID`, i.e. step 10 of the decode below, confirming it.)

`DriveCommFirmware` (K2Host) is YAMP's replacement for that stub: every module_main call it
writes **byte 0 (ring up), byte 2 (node id) and byte 3 (node count) in both banks** from a single
`ObserveLink()` - which is the one function a transport has to replace, and today honestly answers
"nobody is out there". A standalone cabinet is left alone entirely, since it never runs the check.

**It also fixes a trap that would have wrecked the first two-machine run.** The stub takes node id
from the BOARD INDEX (`movzx ecx, byte [0x6911B4]; inc cl`), which is 0 on every one-board machine -
so both peers would have claimed node id 1, and step 12 copies byte 2 into guest 0x5770D1. The
firmware takes it from the CABINET ROLE instead. Measured: master `bank0=00000101` (node id 1),
slave `bank0=00000201` (node id 2), with byte 1 - the ROM's own `link_ID` write - agreeing in both.
Note byte 2 and `link_ID` (0x5770B1, driven by the backup byte) are different values that have to
agree.

The flag register is deliberately NOT touched. Measured,
MASTER, 1500 frames: `net_flag` stays 0 and `MainMode` stays 0 for the whole run while **GlobCntr
advances normally** (181 -> 1381) and the host exits cleanly - the board is yielding through
`synch` every frame, not hung. That is the free-spin state a cabinet sits in when its partner is
not switched on, and releasing byte 0 is what a peer's arrival will do.

**Hold byte 0, never the bit7 poll.** Step 6's poll is four instructions with no frame yield; that
one takes the whole process down, and is what the reset-semantics shim exists to avoid.

### WHAT WRITES THE FAKE RING — `FUN_18006A790`, the emulated comm-board firmware

Found with a 1-byte hardware WRITE watchpoint on `omg+0x7C2730` under x64dbg (the static route is a
dead end: the block is reached through a pointer, so there are no xrefs). Two writers exist.

The first is the board reset clearing it - `FUN_180069E80` does `memset(block, 0, 0x8000)` then
`*(u16*)(block+0x8000) = 0x7EFE`, reached via boot state 0x11 (`omg+0x73357` -> `0x6A1E7` ->
`0x69F9D`). The second is the one that matters:

```
6A7D1  cmp   dword [r8+0x8004], 0      ; "firmware already booted" latch
6A7D9  jne   ret
6A7DF  test  byte [r8+0x8000], 1       ; CommBoardReset bit0 - has the ROM RELEASED the board?
6A7E7  je    ret
6A7ED  movzx ecx, byte [0x6911B4]      ; the BOARD INDEX
6A7F4  mov   dword [r8+0x8004], 1      ; latch: firmware has booted
6A7FF  inc   cl                        ; node id = board index + 1
6A801  and   byte [r8+0x8001], 0x7E    ; clear bit7 (data ready) and the bank bit
6A818  mov   byte [rax+r8], 1          ; byte0 = 1  RING UP      ] this bank (rax = bank offset)
6A81D  mov   byte [rax+r8+2], cl       ; byte2 = node id         ]
6A822  mov   byte [rax+r8+3], 2        ; byte3 = 2  NODE COUNT   ]
6A83A  ... the same three writes for the other bank ...
```

It is the comm board's **register-write handler**: the emulated firmware's boot response, triggered
by the ROM releasing the board from reset (step 5 of `Net_check`). Ring-up and node count are
IMMEDIATES - it declares a healthy two-node ring with nothing attached. Node id is the board index
plus one, which is why board 0 reads 1.

Two consequences worth keeping:

- **A one-shot zero can never hold.** The latch at `+0x8004` makes this fire once per reset cycle,
  and the ROM resets and releases the comm board on every `Net_check` - which every `BlackOut`
  re-runs. Measured: writing 0 only on the first `module_main` call leaves `bank0=01000102` and the
  board boots straight through.
- **The stub writes the status ONCE, at release, and never again.** Real firmware keeps reporting
  ring health continuously. So YAMP owning those bytes every frame is not a workaround bolted onto
  the module - it is supplying the ongoing status this handler was never written to provide, and it
  is where the transport's liveness belongs.

### The hold STARVES the module's own bring-up, and that is why the screen was blank

First attempt: the board parked correctly (`net_flag` 0, `GlobCntr` advancing, host responsive) and
the window stayed BLACK - no site screen, no "Checking Network Now". The reference emulator draws
both, so the ROM was clearly printing them.

The cause is an ordering consequence of parking inside `Net_check`. `FUN_180072FB0` sits in state
0x12 until DLL flag `0x6910DF` goes non-zero, and the only thing that sets it is **hook 0's
handler**, which the emulated ROM reaches at `BlackOut+0x190` - i.e. AFTER `Net_check` RETURNS. A
board held in the check never gets there, so the module stays one state short of brought-up and
composites nothing. Measured directly: `brought-up=0` for a whole run.

`HoldCommRing` therefore makes that write itself while the hold is active. It is not a shortcut
past anything: it is the same write the hook makes, at the only moment it can be made, because the
ROM is deliberately not going to reach it until a peer answers. **Hook 0's OTHER job must not
happen here** - it also clears 0x2000 bytes of text RAM, which is exactly what wipes the site
screen the instant a check completes.

With that in place, VERIFIED ON SCREEN: "THIS IS MASTER SITE" and the flashing "Checking Network
Now" both render, and the board holds there indefinitely with a clean `module_stop -> 0x0`.

## TWO CABINETS TALKING ON ONE MACHINE (2026-08-06)

`VonLink` (source/m2ftg/K2/VonLink.h) lifts the plugin's `UdpTransport` into YAMP so two instances
on one box can exercise the link - RPCN cannot, it hardcodes 3658 in the address it hands a peer.
Verified: MASTER reports `ring UP, node id 1 of 2`, SLAVE `ring UP, node id 2 of 2`.

**Running the pair.** `settings.ini` is read next to YAMP.exe, so the second cabinet needs its own
directory: YAMP.exe + yampnet.dll + D3D12 + settings.ini, a JUNCTION for `m2ftg` (442 MB of ROMs,
do not copy) and a HARDLINK for **`YakuzaKiwami2.exe`** - GameVerify's parent-game check fails
without it, and the symptom is a silent exit with no log at all. Both get `VonHoldLink=1` and
`VonLinkPort=27015`; they differ only in `VonCabinetRole` (1 / 2) plus `VonLinkPeer=127.0.0.1` on
the slave.

**TRANSMISSION IS GATED ON THE LOCAL COMM BOARD BEING RELEASED, and that is not optional.** Without
it "the peer is up" means only "the other YAMP process is sending", which is true from its first
emulated frame - so the instant the slave booted, the MASTER's ring came up, its check completed
and it walked off into the warning screen while the slave was still booting. The two cabinets ended
up at completely different points in their boot sequences, and the signal flapped (`peer went quiet
for 30 frames` mid-run), every flap re-running somebody's check. Gating `Send` on `CommBoardReset`
bit0 - the ROM's own "I have released the board", step 5 of the check - makes liveness mean what it
means on hardware. After the gate both sides report DOWN once and UP once, with no churn.

STILL UNCONFIRMED: whether the ROM reaches "Network Check Success / Result : Node ID = n /
Total Nodes = 2" on both. The `[comm]` and `id=`/`net=` probe lines go through `DebugLogFile`, which
only lands somewhere readable under a debugger, so `yampnet.log` gives transport-level proof but not
ROM-level proof. Watch the screen.

## SOFT-RESET A RUNNING CABINET INTO ANOTHER ROLE - WORKING (2026-08-06)

`SoftResetIntoRole` / `DriveCabinetRoleChanges` (K2Host). Changing the cabinet setting under a
running board re-patches the role byte and drives the ROM back through its own boot path; room join
will call the same function. Verified only that it does NOT fire spuriously (900 frames, zero
resets, clean stop) - **a live role change has not been exercised**, because it needs the settings
UI driven at runtime. Test it by changing "Virtual On cabinet" while the game is up: the board
should black out, re-run its link check with the new role, and come back through the Warning screen.

**WORKING, via the cabinet's TEST switch.** `SoftResetIntoRole` re-patches the role byte and then
holds TEST for 90 frames; releasing it runs the ROM's own test-exit path (i960 0xF3C80:
`net_flag = 0; SubMode = 0; MainMode = g4+1`), and the cleared `net_flag` forces `Net_check` to run
in full - which is where the new cabinet byte is read. Measured, booting NOLINK and resetting into
MASTER at frame 600:

| GlobCntr | MainMode | net_flag | link_ID | |
|---|---|---|---|---|
| 182-582 | 0 | 0 | **3** | booted NOLINK (standalone) |
| 782-982 | **6** | 0 | 3 | TEST held - the operator menu (mode table [6] = 0xF3FE0) |
| 1212 | 0 | 0 | **1** | released - check re-ran and read the patched byte |
| 1412+ | 0 | **1** | 1 | check completed |
| 1812 | 1 | 1 | 1 | back to attract |

After the reset `net_flag` stays 0 and `link_ID` stays 1 for the rest of the run - the cabinet is
parked in its own link check waiting for a peer, which is the point.

`SetSystemSwitches` drives BOTH boards, which is not optional - TEST into board 0 alone deadlocks a
linked pair.

**A bug this proof caught.** The first run completed the check instead of parking, because
`SoftResetIntoRole` patched the role byte while `DriveCommFirmware` still read the role from the
SETTING - so the ROM had been told it was a master while the firmware went on reporting a standalone
cabinet and never held the ring. The applied role is now a single `s_cabinetRole`, set by
`ApplyCabinetRole` and read by everything else. It matters beyond the harness: room join will call
`SoftResetIntoRole` directly, and would have hit exactly this.

**THE FIRST ATTEMPT FAILED and is worth not repeating.** Booted NOLINK with the ring
held, then fired `SoftResetIntoRole(1)` at frame 600. The role byte patched correctly
(`cabinet role: MASTER - backup 0x1D00028 = 1`) and the reset logged - but the ROM never re-ran its
link check: `link_ID` stayed **3** (standalone) for the rest of the run, and `MainMode` went
2 -> 4 rather than through BlackOut's own zeroing to 0 (Warning). If `InitNetwork` had re-run with
the patched byte, `link_ID` would read 1.

So writing `MainMode = 2` from outside is not sufficient. The likely reason is a race the mode table
hides: the mainloop only dispatches on `MainMode` between handlers, and the handler that was already
running (attract) sets `MainMode` itself on the way out - clobbering our write before the dispatch
ever sees it. `MainMode` is a request, not a jump.

The TEST switch is the right lever precisely because the ROM samples it and decides for itself
when to act, so there is no race to lose.

Both halves of the mechanism already existed.

**1. The role byte can change at any time.** `ApplyCabinetRole` patches ONE immediate in hook 6's
handler (`mov byte [rsp+0x20], 2` at handler+0x1E) and that patch is a `Memory::VP::Patch` of a
single byte - repeatable whenever, with no ordering constraint. The module's own injector then
writes it to backup `0x1D00028` the next time the ROM runs `InitNetwork`.

**2. The ROM re-handshakes on every pass through `BlackOut`, by design.** Verified from the code:

```
18784  call sub_186C0     ; link_ID = 3, net_flag = 0   <- re-arms the check
18788  call sub_18960     ; InitAll
1878C  call sub_18A10     ; InitNetwork -> Net_check    <- reads the role byte again
```

and `BlackOut` is **MainMode 2** in the mainloop's mode table at 0x18680 (`off_18680[2] = 0x18650`).
So the lever is most likely `MainMode` (0x5039F4) = 2, with `net_flag` (0x5770B0) = 0 so
`Net_check`'s step-1 early-out does not skip it. `BlackOut` zeroes SubMode / MainMode / VersusMode /
FieldNo on entry, so the cabinet lands back at MainMode 0 (Warning) and boots through normally with
the new role - which is exactly what "soft reset into slave" should look like.

NOT YET TRIED, and two things to check when it is:
- The ROM has its own soft-restart path in the mainloop at `0x188D8` (clears `net_flag`, branches
  back through `BlackOut`), gated on `0x5039F0 == 0` and the state halfword `0x5024F4 == 0x50`.
  That is the ROM's own trigger and may be the better door - but `0x5024F4` is a widely-read game
  state (elsewhere compared against 0x10), not a spare latch, so do not write it blind.
- The test-menu EXIT routine `0xF3C80` is the third door (`net_flag = 0; SubMode = 0;
  MainMode = g4+1`), and needs its caller's `g4` read before being driven directly.

**The old warning about the test-exit restart no longer applies.** This file records that it
"WIPES NOTHING", which desynced peers at frame 0 - but that was under LOCKSTEP, which needed
bit-identical RAM. The current architecture carries the ROM's own link protocol and has no
determinism requirement, so a soft restart leaving attract-mode leftovers in work RAM is simply
what a real cabinet does.

## *** CABINET ROLE IS NOW A SETTING, AND IT HAS A VISUAL TELL (2026-08-06) ***

NOLINK / MASTER / SLAVE is set from the netplay settings page ("Virtual On cabinet"), persisted as
`[Netplay] VonCabinetRole`, and applied before `module_start` - no command-line flag, so a run
started from the game launcher gets it. Implementation and the three-encoding table are in
`K2Host::ApplyCabinetRole`. Measured, single board, 900 frames each:

| setting | backup `0x1D00028` | `link_ID` | `net_flag` | `MainMode` |
|---|---|---|---|---|
| No link | 2 (the module's own default) | 3 standalone | 0 | 1 |
| MASTER | 1 | 1 | 1 | 1 |
| SLAVE | 0 | 2 | 1 | 1 |

**`0x503A08` IS the "I am the slave" flag, and this file's warning not to rely on that reading can
be retired.** The ROM's `InitNetwork` sets it on the byte==0 arm only and clears it on the master
and standalone arms - and the slave's PICTURE DIFFERS as a result: video-region hashes are
identical across NOLINK and MASTER runs and diverge reproducibly on SLAVE from the third sample
on, which the user independently saw as the attract-mode characters changing colour. The symbol
table's `TempTimer+0x4` is the wrong name for it.

**The asymmetry matters when verifying two machines: only the SLAVE looks different.** A master is
pixel-identical to a standalone cabinet, so "it looks the same" is not evidence the role failed -
turn on "Log linked-cabinet state" (`[Netplay] VonLinkLog`, replacing `-von-2probe`) and read
`id=`.

**Unexplained, seen once:** the first MASTER run of one batch sat at `net=0 main=0 id=1` at frame
900 - i.e. still inside the link check - where three later runs all reached `net=1 main=1`. Not
reproduced; recorded rather than dismissed.

## *** HOOK INDICES IN THIS FILE ARE ONE TOO HIGH (2026-08-05) ***

The HLE table base was read one record low (0x476510 rather than 0x476520), which invented a
phantom hook 0 and pushed every real hook up by one. `VonHooks.inc` and `docs/von-hle-hooks.md`
are renumbered; this file is not. **Subtract one from every hook number below**: the `InitNetwork`
MASTER/SLAVE injector called "hook 7" throughout is now hook **6**, and "hook 3" (`synch+0x4`) is
now hook **2**. The full classification — all 120 handlers read, including what hook 3 does, which
this file records as unread — is in `docs/von-hle-hooks.md`.

## *** NETPLAY STRIPPED 2026-08-04 — read this before touching anything below ***

**All networking was removed from VON_K2** at the user's direction after four two-machine runs
failed in four different ways. The decision: stop layering fixes on a guessed foundation and
start fresh from these notes. `DW_OMG.netplayReady` is `false`, the ABI is back at 8 (the shared
seed-heartbeat fix for StF/FV/VF2 guests is kept), and K2Host no longer touches NetSession at all.

### What was removed
- The lockstep round flow for VON (`HandshakeRoundOps`, the ring hold, parked/release, the
  handshake-as-barrier round start) and every K2Host netplay coupling (pad-slot swap, render-role
  driving, canary board pin, `[vonlink]` heartbeat, netplay input/coin/test suppression).
- ABI 9 (host-forced HLE hook-off adoption) and the half-started `kPacketComm`.
- The MASTER/SLAVE badge overlay.

### What was kept, because it is local and verified
- **VirtualClock + the pacing caps** (3 ticks / 16 calls per host frame, per-call gate assert) —
  game-speed correctness for local play; VON used to run on wall time.
- **The comm-board reset-semantics shim** (`LINK_TRANSFER_CALL` + `LinkTransferWithResetSemantics`)
  — fixes the hard hang on leaving the test menu with the link running. Local two-board play via
  `-von-2board` works and links (site screens reachable through the ROM's own paths).
- **`-von-hleoff=<indices>`** (local boot-time hook disable), `-von-render1`, `-von-2probe`,
  `[vonmap]`, and the whole HLE table/UI.
- Every reversing result in this file — the protocol decode is solid; it was the netplay
  ARCHITECTURE built on it that failed.

### Why it failed, run by run (the part not to repeat)
1. **Soft-restart round start: desync at frame 0.** The test-exit restart wipes no RAM; peers
   parked with different attract leftovers. Real cabinets do not need bit-identical RAM; lockstep
   does — that tension never went away.
2. **Double-step at frame 503.** The virtual-clock loop accrued time across module-internal
   service skips; one call ran two board frames. Fixed (tick cap), but found only on two machines.
3. **Standalone boot, twice.** The two-board gate raced the ROM's `InitNetwork` inside multi-call
   bursts — per-host-frame asserts lost the race; the shim-side assert missed the boot window
   (link disabled). Each "fix" was verified single-machine, where the race happened to be won.
4. **Final run: mechanically clean (parked rom=91 both sides, released together, no desync) —
   but no visible link screens and "both boards share inputs" in play.** Never diagnosed: the
   observability (`[vonlink]`) was added only after. The open suspects are recorded below
   (Net_check possibly standalone despite the gate; the site text possibly drawn to a layer the
   DLL never renders; the CommData slot-readback mirror hypothesis).

Meta-lessons: never validate a link-path change on one machine (three of four failures were
invisible there); module globals need per-CALL asserts, not per-frame; build the observability
BEFORE the experiment; and the per-board vs per-machine role model must be settled in DESIGN, not
argued per run.

### The fresh-start design (agreed with the user)
**One board per machine.** Host machine = MASTER, guest machine = SLAVE — the role belongs to the
MACHINE: write backup `0x1D00028` (1=MASTER host / 0=SLAVE guest) via the module's own injector
`FUN_18006D1E0`, with hook 7 (the per-board injector) repointed inert. Gate stays OFF (single
board). **The link crosses the wire**: replace the local transfer at the already-hooked
`LINK_TRANSFER_CALL` site with a network exchange — ship my `CommSend` (0x700, current bank) each
frame; on the peer's payload, write my `CommData` (peer slot +0, own echo +0x700), set flag bit7,
toggle the bank; maintain the firmware status bytes (byte0 ring-up=1 when exchanging, byte2 node
id 1/2 by role, byte3 = 2). **No payload → bit7 stays clear → the ROM's own wait absorbs it** —
the authentic site screens and "Checking Network Now" wait come for free, and the ROM's own
protocol (SynchFlag/SynchTime/GlobCntr, netCheckFail) does the synchronising and the error
handling. No lockstep, no seeds, no canary, NO DETERMINISM REQUIREMENT. LAN-first: RTT appears
as peer-state lag, negligible on a LAN; internet play is a later tuning question. Transport can
be a thin datagram channel (RPCN P2P for discovery/NAT, or the dormant `UdpTransport` for LAN).

## *** Pre-strip state as of 2026-08-04 (historical from here down) ***

**Where it stands: TWO PEERS STAY IN SYNC.** Confirmed on two machines 2026-08-04. Both boards run
on both peers, master/slave assign themselves, inputs route per board, and the round holds.

Getting there took THREE distinct fixes, each of which had to be found before the next became
visible — and only the first was in netplay code at all:

1. **the guest seeded its RNG with 0** (a plugin/ABI gap, and shared with StF/FV/VF2);
2. **the debug-menu RESET never reset board 1** (it cleared the gate the module's own reset reads,
   three instructions before calling it) — this crashed both peers on the first match frame;
3. **the board is paced by REAL TIME, not by `module_main` calls**, so peers at different frame
   rates put different numbers of emulated frames into the same netplay frame. Root cause: `K2Host`
   is the only host that never read the 60 FPS cap setting.

Each is written up below. The pattern worth carrying to Motor Raid and Sega Rally 2: **every one of
them was the module's own mechanism being mis-driven, not a missing capability.**

Later the same day the whole stack was re-read end to end (plugin seed heartbeat, NetSession round
lifecycle, K2Host gate/render/pin/pads, HleHooks HandlerTail, VirtualClock, SystemSwitches both
banks + both boards, the lobby page) with no defect found, both projects rebuilt clean, and a
600-frame `-von-k2` run under cdb came back clean (`module_stop -> 0x0`, one ROM frame per traced
call). The one gap closed in code: the **cabinet badge** (below), so the next two-machine run can
verify the per-role render on sight.

### THE BLOCKER — the guest seeded its RNG with 0 (FIXED 2026-08-04, UNTESTED)

`NetSession::Drive` reads the match seed during round prep at `IN_ROOM`, **before** the barrier,
because the seed has to be applied before the board reset — the ROM's post-reset initialisation
draws from the generator. The host had it (it generated it); every guest read 0. Measured on two
machines — guest logged `RNG seeded 0x00000000, board reset`, then `barrier released; round 1 seed
0x813AC110`. The two peers therefore reset their boards from differently seeded generators.

**This was in SHARED code and affected StF/FV/VF2 guests too**, not just Virtual On.

**The previous session recorded this as an unavoidable deadlock. That diagnosis was wrong**, and
the correction is the whole fix. The seed does NOT "arrive with the barrier": `Plugin.cpp`'s
`DrainSocket` adopts the host's seed from any announce in **any** state, and announces begin when
the **host** calls `begin_round` — which depends on nothing the guest does. The actual defect was
narrower: *the host published nothing until it pressed Start*, so whichever player pressed first
lost. "The guest's Start Match silently did nothing" was a guest waiting correctly while the host
had not started yet.

Fixed as option 1 (publish the seed with the room), which turned out to be far smaller than the
handoff assumed because the guest already stored `match_seed` from any announce:

- **`kPacketSeed`** (`Lockstep.h`) — same payload as an announce, deliberately a **different
  type**. The receiver feeds announces to `Lockstep::OnPeerAnnounce`, and a heartbeat that did the
  same could release a barrier for a round the peer has not prepared.
- The **host** publishes it every 250 ms while `IN_ROOM` (`ApiPoll`). Cheap, repeated because a
  datagram may be lost and because the peer's address is not known the instant the room appears.
- The host's seed is **clamped away from 0** at room creation. Zero is the "not known yet" value,
  so a host that rolled 0 would publish something indistinguishable from silence and hang its guest
  at Start — one tick in 2^32, presenting as a network fault.
- **`NetSession::Drive` now waits** for a non-zero seed before entering prep, and says so once in
  `yampnet.log` (`waiting for the host's match seed...`). The Start press is not consumed while it
  waits. Only a guest ever waits, for a few hundred ms at worst.
- **ABI 7 → 8.** No struct changed; what changed is when `get_match_seed` is promised valid
  (`IN_ROOM`, not `SYNCING`). Bumped anyway because against an ABI 7 plugin the new wait would hang
  a guest at Start with no explanation — a rejected DLL says so, a silent wait does not.

Verified so far: both projects build, VON boots 600 frames clean (`module_stop -> 0x0`, no
second-chance AV), `netplay plugin loaded (ABI 8)`. **A two-machine round has not been run.**

### THE SECOND BLOCKER — RESET never reset board 1 (FIXED 2026-08-04, verified single-machine)

With the seed fixed, the first real round crashed **both peers on the first match frame** — zero
`timers f=<n>` and zero `[vonmap]` lines, so not one frame completed. Two cdb captures:

| peer | rip | rdi |
|---|---|---|
| host | `omg+0x44103` | `base+0x7C2380` |
| guest | `omg+0x6E4E0` | `base+0x7C2380` |

`0x7C2380` = `0x7C21D0 + 1*0x1B0` = **board 1's i960 context**, with IP `0x500440` — inside work
RAM. Board 1 executing its own data, on both peers, at two different instructions.

**The cause, from the module's own code.** `FUN_180069E80` is the shared board reset that BOTH the
debug-menu RESET and boot phase 2 end with, and it is already two-board aware:

```c
memset(board0_ram, 0, 0x100000);  memset(board1_ram, 0, 0x100000);
select(0);  reset_cpu();          // board 0
bool two_board = DAT_1806910DE;   // <-- THE GATE
if (two_board) { select(1); reset_cpu(); ...; select(0); }   // board 1
```

and the RESET handler `FUN_18006BA90` does `DAT_1806910DE = 0;` **three instructions before
calling it**. So board 1's CPU is never reset, and YAMP's per-frame gate reassert then starts it
from a stale context. The module was never missing the code — the same shape as the gate itself.

FIX: `m2ftg::ResetBoard` samples the gate, invokes RESET, then **restores the gate and re-runs
`0x69E80`**, which takes the two-board path. Two new `DwGame` fields, `rvaTwoBoardGate` (0x6910DE)
and `rvaBoardResetAll` (0x69E80), both 0 in single-board games. Sampled-and-restored rather than
forced, so `-von-1board` still works and a reset never changes the shape of the machine. This also
fixes the manual debug-menu RESET, which was a live crash in single-player.

Consequently K2Host's prep-window gate guard is **gone** — the gate now stays on continuously,
which also removes an OFF→ON transition from the middle of round prep.

**`ImportSymbol::PER_BOARD_INIT` was REMOVED — it never did what its name said.** It was matched
from boot phase 2's `XOR ECX,ECX / CALL / MOV ECX,1 / CALL` pair, which is a real for-both-boards
call — just the wrong one. `0x18006BAF0` is the per-board **VIDEO** init: it walks the 2 MB
framebuffer at `0x1807DADC0 + board*0x20A080` building a 6-bit palette table and touches nothing
the i960 owns. It returned cleanly, logged `board 1 re-initialised after reset`, and left board 1
exactly as broken — the log line actively hid the bug. **A pattern that matches uniquely only
proves the SHAPE is unique.** This one was never checked against the function's body.

VERIFIED single-machine (`-von-k2 -net-host -von-2probe`): a full reset → settle → anchor with the
gate on throughout, no fault, clean `module_stop -> 0x0`, and at the barrier
`B0 type=1 glob=8 id=1` / `B1 type=1 glob=8 id=2` — both boards alive, at the same counter, master
and slave assigned by the ROM itself. **A two-machine round has still not been played.**

### THE THIRD BLOCKER — the board is paced by REAL TIME (FIXED 2026-08-04, single-machine)

With both boards alive, the first round that actually ran frames desynced at frame 0 — and the logs
showed the peers were **not** diverging. Their state hashes MATCHED at equal ROM frames
(`0x8B2A0E97` at rom=8, `0x0CC55BF6` at rom=11, on both machines). What differed was which ROM
frame each peer had reached when it submitted netplay frame 0: host `rom=9`, guest `rom=11`.

```
HOST   f=0 rom=8→9   f=1 rom=9→10   f=2 rom=10→11      one ROM frame per call
GUEST  f=- rom=6→8   f=0 rom=8→11   f=- rom=12→14      two and THREE per call
```

**Virtual On's board advances with WALL TIME, not with `module_main` calls.** The i960 core is
clean — `FUN_1800441a0` counts instructions (`-0xC` per batch, no clock) and `FUN_1800440c0` runs
until the ROM yields — but the task layer above it (`module_main` → `M2FTGAppModule` slot 0x10
`FUN_1800831b0` → task callback → `FUN_180072420`) is saturated with reads of `FUN_1800856B0`, a
bare `QueryPerformanceCounter` wrapper with 49 call sites. So `GlobCntr` advances ~60 times per
REAL second whatever the host's frame rate.

This is VF2's fault class in the opposite direction. VF2's calls sometimes advanced ZERO frames,
which `EndFrame`'s stall test handles. **Nothing can reconcile a call that advanced THREE**: those
three board frames share one pad while the peer applies three different ones, so the simulations
genuinely part company.

**ROOT CAUSE, and it is embarrassing: `K2Host` is the only host that never reads
`m_enableFpsCap`.** `LJHost.cpp:376`, `YLAD/VF2.cpp:825`, `Pre3Host.cpp:594` and all three VF5FS
hosts honour it; this one never did, so `-von-k2` has always run uncapped. The test host at ~170 fps
against a guest at its own rate is what turned real-time pacing into 0.35 vs 2–3 frames per call.

FIX, two halves:
1. **`VirtualClock`** (`source/m2ftg/VirtualClock.{h,cpp}`) patches the QPC wrapper to return a
   counter YAMP owns (14 bytes: `MOV RAX,imm64 / MOV RAX,[RAX] / RET` — absolute, because YAMP.exe
   and the module need not be within a RIP displacement of each other). `StepOneBoardFrame` then
   ticks it in **sub-frame** steps (freq/120), calling `module_main` after each, and stops the
   moment the ROM counter moves. A step shorter than the board's period is what makes one frame the
   maximum per call; terminating on the ROM COUNTER rather than a tick count is what makes two peers
   reach identical state even though they need a different NUMBER of calls (QPC frequency is a
   property of the machine). No reverse-engineered period constant is required, which matters for
   Motor Raid and Sega Rally 2.
2. **The host loop is now the limiter**, at a stated 60 Hz. Taking the clock away also took away
   what decided game SPEED — the board now advances one frame per loop iteration, so uncapped meant
   Virtual On ran ~3× too fast. Deliberately not behind the cap setting: there is nothing left to
   turn off.

**A self-tuning limiter was tried first and does not work.** Pacing against the virtual time each
frame consumed looks exact — that is the module's own period, no constant needed — but the consumed
time is quantised to the sub-frame step, so a period near 1/60 s reads as either two steps (60 Hz)
or one (120 Hz) and the average landed at 63.7 Hz. **A signal quantised coarser than the thing being
measured cannot measure it.**

MEASURED after the fix: ROM-counter delta per call over 3600 frames = **+1 ×3569, 0 ×17, >1 ×none**
(before: 311 advances in 900 calls on this host), at ~60 fps.

**`GlobCntr` WRAPS AT 256** — 13 deltas of −255 in those 3600 frames, one per 256. That is the
"four non-unit jumps per ~900 calls" listed below as an anchor risk: a wrap, not a jump.

### THE BOOT HANDSHAKE, DECODED — and the comm-board RESET fix (2026-08-04)

Prompted by the question "what does `synch` actually wait for when the pair freezes on leaving the
test menu": the whole master↔slave boot conversation is now read out of the ROM (IDA on 7331,
`von_prog.bin`), and the freeze is fixed. Diagnosed and validated ON A LIVE HUNG PROCESS via the
x64dbg bridge (port 3000), not from static reading alone.

**`synch` (i960 `0x18AB0`) is NOT the link handshake.** It is the ROM's wait-one-frame primitive:
`bal 0x28DE8` spins until bit2 of `0x98000C` (vblank edge) flips, toggling `0x803008`/`0x801008` by
frame parity, and the spin count goes to `0x504C80` plus min/max profiling stats in backup RAM at
`0x1D0020C/0x1D00210` (only when `MainMode == 4`). Every wait in the link check yields through it.

**`Net_check` (`0xC5870`) talks to the COMM BOARD, not to the peer.** The comm board is an
intelligent card with its own firmware; the CPU drives it through the two registers and reads its
answers out of the low bytes of shared comm RAM:

| step | what the ROM does |
|---|---|
| 1 | if `net_flag` (0x5770B0) already set, skip everything (why retail never re-runs this) |
| 2 | `resetCommBoard` (`.lf 0xC5608`): write 0 to BOTH registers — **hold the board in reset** |
| 3 | read `CommBoardReset` back; bit0 still set ⇒ "THERE IS NO COMMUNICATION BOARD!" |
| 4 | wait 60 frames (`synch` × 0x3C) |
| 5 | write `CommBoardReset = 1` — release the firmware |
| 6 | **poll `CommFlagReg` until bit7 reads 0** — the firmware's boot acknowledgment. A FOUR-INSTRUCTION LOOP WITH NO FRAME YIELD |
| 7 | clear `cRecn` (0x5024F0) and `cSend` (0x5032F0) staging, 0x700 bytes each |
| 8 | print "THIS IS MASTER/SLAVE SITE" from `link_ID`; standalone (3) exits here — but note it exited AFTER step 6 |
| 9 | poll `CommFlagReg` until bit7 SETS (first transfer done; `synch`-yielding, abortable via the TEST-request check `0xEADE8`) |
| 10 | write own `link_ID` → comm RAM byte 1; read halfword +0x12, halve it → +0x14 |
| 11 | poll comm RAM **byte 0 == 1** (ring up, `getCommStatus.lf 0xC5CA8`), "Checking Network Now" blinking; then **byte 3 == 2** (node count, `0xC5CE8`) or "Illegal Nodes: %d" → reset board and restart the whole check |
| 12 | success: "Network Check Success", **`net_flag = 1`**, node id (byte 2) → `0x5770D1`, total nodes → `0x5770D0`, nodes-1 → `0x5770D4` |

The ROM never WRITES bytes 0/2/3 (xrefs: read-only) — **on hardware the comm-board firmware writes
them; in the module the device model seeds them** (live: `01 / id / 02` present in both banks of
both blocks). The wire never appears in any of this: the "handshake" is CPU↔firmware, local to the
cabinet, which is why it needs no netplay awareness — both peers run it identically.

**THE BUG: the module models the transfer but not RESET.** The link transfer sets flag bit7 on both
blocks every frame regardless of the reset registers (retail is standalone; nothing ever ran this
path). So step 6 — wait for bit7 to READ 0 after releasing the board — can never terminate while
the link runs, and because that poll never yields, the CPU step never returns, `module_main` never
returns, and the whole host spins. **Cold boot escapes by ordering alone**: `Net_check` completes
before the module enables the link (that is why `net_flag=1` shows in healthy linked runs — the
raw handshake DOES run and pass at boot). Leaving the debug/test menu re-runs the check with the
link live and hard-hangs both boards.

Live capture of the hang (x64dbg bridge): both boards frozen, `GlobCntr` static at 642, board 0 at
`Net_check+0x64` (the step-6 poll) with `reset=01 flag=81`, host rip inside the i960 interpreter at
`omg+0x46BE7`. **Writing flag bit7 to 0 by hand un-froze it instantly**: GlobCntr resumed, both
site screens printed, `net_flag → 1` on both boards, `MainMode → 1`. That manual poke is exactly
what the fix automates.

**THE FIX — `ImportSymbol::LINK_TRANSFER_CALL` + `LinkTransferWithResetSemantics` (K2Host).** The
frame driver's transfer call (`FF 05 ?? / E8 ?? / 33 C9 / E8` — verified to hit EXACTLY ONCE, only
in omg across all six module DLLs, at RVA 0x7392B calling 0x6A310) is wrapped: before the transfer,
sample each block's reset register; after it, a block whose register the ROM has dropped to 0 gets
its flag register held at 0 — a board in reset drives nothing. Armed only after the register has
been SEEN at 1 (the ROM's own release), so cold boot is bit-identical (verified: same chk hashes at
the same ROM frames over 600 frames, `module_stop -> 0x0`). The ROM's release write happens inside
the following CPU step, after the pin was applied, which is precisely what lets its step-6 poll
pass on the first read. Arm/disarm transitions are logged to yampnet.log.

Consequences: leaving the test menu now re-links instead of hanging; a round-start reset that
re-runs the check is robust regardless of link-enable ordering; and the authentic
"THIS IS MASTER SITE" / "SLAVE SITE" screens are reachable in-emulator — the ROM-native tell that
corroborates the overlay badge. (Correction while verifying the pattern: the link frame counter the
driver increments is `0x1807C2720`, not the `0x180C22720` listed in the frame-driver walkthrough
below.)

**Hook 7 is NOT a Net_check skip.** The backup-RAM link-attribute write documented at
`0x180070A3B` sits inside hook 7's handler (`+0x070A00`, the `InitNetwork` trap): the hook is the
MASTER/SLAVE assignment injector, and the handshake itself runs raw — confirmed by catching the
live board executing it. What hook 3 (`synch+0x4`, handler `+0x070840`) does is still unread.

**Architecture decision (asked and settled 2026-08-04): the handshake is NOT the netplay sync
mechanism — lockstep stays.** The link protocol assumes a partner one frame away on a dedicated
serial ring; carried over the internet, its own failure paths (`netCheckFail`, "Illegal Nodes",
the 1-second reset hold) become the user experience, the 0x700-per-frame payload needs hard
real-time delivery, and the two emulators still need frame-rate agreement underneath — i.e.
lockstep gets rebuilt anyway, with worse failure modes. The PS3 port faced the same choice and
runs both boards locally with pads on the wire; the handshake's job here is to bind the two LOCAL
boards, and with the reset fix it now does that reliably.

### IMPLEMENTED: round start via the TEST-MODE-EXIT restart, with the handshake as the barrier

Decided with the user and IMPLEMENTED 2026-08-04, replacing the frame-8 anchor for VON
(`m2ftg::HandshakeRoundOps`, registered by K2Host when the transfer shim installs; every other
game keeps the legacy flow). The insight: the ROM already contains the round-alignment mechanism,
because two REAL cabinets power on at different times and the ring only comes up when both are
ready.

**FIRST TWO-MACHINE RUN (2026-08-04): parked and released correctly on both peers - and desynced
at frame 0.** The cause is recorded in the logs, not guessed: that version triggered the ROM's
test-exit soft restart (`MainMode=0/SubMode=0/net_flag=0`), which WIPES NOTHING - each peer parked
with everything its attract mode had accumulated in work RAM, and the host anchored at rom=98 vs
the guest's rom=197. Real cabinets do not need bit-identical RAM to link; lockstep does. The flow
now starts from `m2ftg::ResetBoard` (the full two-board wipe) with the ring held through the
post-reset boot - the wiped ROM runs `Net_check` raw (net_flag wiped to 0) and parks in the same
ring poll. Two robustness details that came with the change: the hold is applied HOST-SIDE each
frame as well as in the transfer shim (the post-reset bring-up passes through the state that
disables the link, and with the link off the shim never runs), and parked() requires each comm
board's reset register to have been seen at 0 SINCE begin() (the registers live in .data, so
right after the wipe they still read 1 from the previous session, and a cycle-blind test declares
"parked" before the ROM has booted).

**SECOND TWO-MACHINE RUN (2026-08-04): the wipe fix is PROVEN - frames 0-502 were BIT-IDENTICAL
on both peers** (every chunk hash, every timer count, every instruction count matched at equal
netplay frames; the users saw both machines reach "ready to link"). The round then desynced at
frame 503, and the logs name the mechanism exactly: off a timer-rearm boundary, the host's call
ran rom 82->83 with 7,800 instructions while the guest's ran 82->84 with 15,648 - **one
module_main call ran TWO board frames** against a single pad. Root cause: `StepOneBoardFrame`
ticked the virtual clock on EVERY loop iteration until the counter moved, so a call the module
internally declines to service (the VF2-measured ~5%, machine-local in WHICH calls) still accrued
time - and the call that finally serviced ran every accrued frame at once, unstoppably mid-call.
FIX: only the first THREE iterations tick; the rest retry with time frozen. 3/120 s guarantees at
least one frame due for any board slower than 40 Hz and fewer than two for any faster than 80 Hz
(Model 2 is ~57.5-60). Verified single-machine: 1800 frames clean, counter delta +1 x399 / else
x0 over the tail. The same ~500-frame signature (legacy desyncs at 481/626/536) says this was
also the legacy flow's killer, not just the handshake flow's.

**THIRD RUN's regression (no link, one board, two displays) - CAUSE FOUND AND FIXED.** The tick
cap turned boot into many-call bursts per host frame with almost no time flowing, and the
two-board gate was only reasserted once per HOST frame - so the module's own bring-up state
zeroed the gate and the ROM's `InitNetwork` read it as 0 WITHIN one burst, assigned NOLINK, and
the machine came up standalone. Fixes, verified single-machine (2400 frames: net=1, id=1/id=2,
both GlobCntrs in lockstep, comm traffic flowing, clean stop):
- the gate is now asserted INSIDE the transfer shim, which runs before every emulated instruction
  of every frame (all board stepping sits behind the same link-enabled guard), so no burst can
  interleave a gate-down window with the ROM reading it;
- the frozen-retry budget dropped from 240 to 16 calls - a skipped service lasts a call or two,
  and a counter that simply is not moving yet should hand the host frame back (EndFrame's stall
  test already treats that as a non-frame) instead of distorting bring-up with hundreds of
  no-time calls;
- unrelated but found in the same session: `LogTwoBoardState`'s io hex buffers were [32] for a
  0x11-port window (35 bytes needed) - an RTC-caught stack overrun on any `-von-2probe` run since
  the port-window correction, presenting as an int3 "crash" at frame 0.

What the logs should show, in order, on BOTH peers: `comm ring HELD DOWN` -> `board reset + ring
held (handshake start)` -> reset-semantics arm/disarm lines as Net_check cycles the registers ->
`both boards parked in the ROM's link check (rom=N); barrier opened` -> `comm ring RELEASED` +
`match start`. On screen: the link check completes ("Checking Network Now" -> site screens ->
"Network Check Success") and the pair plays THROUGH the ROM's own flow - attract, start, select -
with pads lockstepped from the release frame. Both machines need Freeplay ON (as with the other
games: the coin line is dead during netplay; Start rides the transmitted pad). REMEMBER TO COPY
THE NEW BUILD TO BOTH MACHINES - the frame-0 run's logs also contain three older-flow rounds
desyncing at frames 481/626/536, which is the separate open issue below. The design:

1. **Round prep** = seed the RNG, then trigger the ROM's own soft restart on both peers —
   NOT the debug-menu RESET. The test-menu EXIT routine is `0xF3C80`:
   `net_flag = 0; SubMode = 0; MainMode = g4+1; ret` — clearing `net_flag` is what forces
   `Net_check` to run in full. (The mainloop has its own equivalent at `0x188D8`, which clears
   `net_flag` and branches back through BlackOut — the ROM re-handshakes on every soft restart by
   design.) The MainMode value at exit needs its caller context read before this is driven
   directly; alternatiely drive the actual TEST switch path.
2. **The hold** = YAMP keeps the handshake from completing until the lockstep barrier releases.
   The natural knob is comm RAM **byte 0 (ring up)**: held at 0, both boards sit in `Net_check`'s
   "Checking Network Now" loop — which yields through `synch` every frame, so module_main keeps
   returning, the host stays alive, and the overlay can say what is being waited on. (Do NOT hold
   at the bit7 poll — that one never yields.)
3. **The release** = at barrier release, write byte0 = 1 (both banks, both blocks) on the same
   netplay frame on both peers. Both ROMs complete the check identically and the match proceeds
   frame-locked. No frame-8 anchor, no reset-detection on GlobCntr — the ROM's own wait replaces
   both. (GlobCntr keeps ticking through all of this — the test-exit restart does NOT zero it,
   which is why the old romFrame<=1 reset-detection cannot survive this change.)
4. **MASTER/SLAVE needs no wire signal**: hook 7's handler writes board 0 = MASTER / board 1 =
   SLAVE identically on every peer before InitNetwork reads it (the attribute is per-BOARD, fixed
   by topology). The only per-peer agreement is which pad/render you own, which the room join
   already assigns.

Determinism caveats to verify when building this: the wait loop's blink text ("Checking Network
Now" toggling every 30 frames) writes display state whose phase differs per peer at release —
check whether it lands inside the canary range; and stale-attract RAM differs per peer exactly as
the narrowed canary already accounts for.

### OPEN — "the two boards share inputs" (user-observed in play, 2026-08-04)

The input SELECTOR is not the cause: `0x1807A8FDC` is written with the board index by the bank
switch itself (`MOV [rip],R8D` at RVA `0x69D99` — the doc's re-point table above simply missed
it), cleared/set per board by the board reset (RVAs `0x69F39`, `0x6A02D`), and read by the io
refresh (`0x6C7E5`). So pad[N] → input struct N → board N holds statically all the way down.

The live hypothesis is the LINK READBACK instead: the PC transfer writes the peer's send into
`CommData+0` and each board's OWN send into `CommData+0x700`. If the ROM indexes those slots by
cabinet id (master expecting its own at slot 0 / slave's at slot 1, or the reverse), a mismatch
makes each board read ITS OWN input echo as the peer's — every pod mirrors the local sticks, which
reads exactly as "the boards share inputs and the serial wire carries nothing". The ROM's runtime
exchange (`cRecn` 0x5024F0 / `cSend` 0x5032F0 staging, `cSend - cRecn = 0xE00` = TWO 0x700 slots)
is the thing to decode next; IDA has no data xrefs for the `lda` constants, so search the code for
`lda 0x5024F0` / the CommData window addresses directly. The attract-mode io probe cannot decide
this (nothing responds on either board's ports during attract; the twin sticks are the ANALOGUE
ports io[0..7]) — the operator input test, or in-match observation, is the measurement.

### LOOPBACK TESTING IS NOT AVAILABLE — the previous "NEXT" step cannot be done

The handoff said to re-test over loopback with two YAMP instances on one machine. That is not
possible with the RPCN transport, and `RpcnClient.h` already says why at `OpenSignaling`: a second
bind of **3658 fails with WSAEADDRINUSE**, and **RPCN hardcodes 3658 when it hands out a peer's
local address**, so a non-default port is not usable for same-NAT play. The `local_p2p_port`
override exists for two peers inside ONE process and is not plumbed through the ABI.

Either test on two machines, or revive **`UdpTransport`** (`Transport.h`) — the direct
address:port development backend that predates RPCN. It is still compiled but unreachable:
`yampnet_session::transport` is a concrete `RpcnTransport`, not the `ITransport` it implements.
Reviving it would give a LAN link with no server in the loop, which also removes RPCN as a variable
when diagnosing a desync.

### Also unresolved

- **The guest crashed on connect** (guest first, then host) in the run before the seed work. No
  stack — the guest was not under a debugger. Reproduce with the guest under cdb:
  `cdb -g -G -c "sxd av; g; .lastevent; r; kb; q" <path>\YAMP.exe -von-k2`
- **`-von-render1` is unverified on screen.** It patches the frame step to leave board 1 selected so
  the guest sees its own cabinet. It runs stably and is now driven by `localPad`, but nobody has
  confirmed the picture actually changes. The DLL skips the "THIS IS MASTER/SLAVE SITE" boot screen,
  so there is no easy visual tell.
  **The tell now exists (2026-08-04): a cabinet badge** — during a linked-cabinet netplay match the
  overlay shows `MASTER cabinet - you are Player 1` / `SLAVE cabinet - you are Player 2` from
  `local_player` (`DrawNetplayOverlay`, presentation-only so it cannot desync the pair). On the next
  two-machine run: badges differ and pictures differ → the per-role render works; badges differ but
  the pictures are identical → the RENDER_BOARD_SELECT patch is not taking effect on the guest.
- ~~**`GlobCntr`'s four non-unit jumps**~~ — CLOSED 2026-08-04: the counter wraps at 256, and
  ~900/256 is exactly the four "jumps". Not a hazard; a modulus.
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
- **A FAILED LOOKUP IS NOT ABSENCE.** `/decompile FUN_00185988` answered "Function not found", and
  I reported the PS3 model as resting on a function that does not exist — then spent a long stretch
  redesigning around that. `get_function_by_address` finds it fine. Before contradicting an existing
  finding, confirm the tool actually looked: a second endpoint, or a disassembly at the address.
- **Do not quote a buffer bound as a payload size.** `cellRudpRead(sock, buf, 0x542, ...)` is the
  maximum message, and I offered it as "too big for pads, right size for board state". The real
  payload turned out to be 128 bytes once a second.
- **A diagnostic that reports success is worse than none.** `board 1 re-initialised after reset`
  printed on every round start while board 1 stayed dead, because the routine behind it was
  misidentified. The log line was believed over the crash. Log what you OBSERVED (board 1's
  counter advanced), not what you CALLED.
- **A "deadlock" that was never one cost this the whole seed fix.** The blocker above was recorded
  as impossible to fix by waiting, in a comment that told the next session not to try. The reasoning
  named the wrong sender: the seed rides the host's *announce*, which starts at the host's
  `begin_round`, not at the barrier's release. Before writing DO-NOT-DO-THIS into the code, trace
  the mechanism the claim rests on — a wrong one is a lot harder to dislodge than an open question.
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

### RE-VERIFIED 2026-08-04, after the model was challenged

The whole "both boards everywhere" conclusion was re-examined from scratch, because the real
cabinets genuinely are one board and one monitor each, linked by serial — so the obvious reading is
that two networked machines should be **one board per peer with the link carried over the wire**.
That reading is wrong for this emulator, and here is the evidence, because it deserves to be
settled rather than re-argued every session.

**The link IS a local memcpy. `FUN_00185988` verified, not assumed:**

```c
FUN_0022d494( (flagA & 1 ^ 1) * 0x4000 + 0xA708 + base,
              (flagB & 1)     * 0x4000 + 0x2000 + base, 0x700 );
FUN_0022d494( (flagB & 1 ^ 1) * 0x4000 + 0x2700 + base,
              (flagA & 1)     * 0x4000 + 0xA008 + base, 0x700 );
// then toggle bit7 and bit0 on both comm flag bytes
```

Two `0x700`-byte copies, each into the bank the reader is not on — the exact twin of the PC
module's inlined SSE transfer at `0x180069E48`. **CAUTION: a `/decompile` lookup for this function
returned "Function not found" and I reported the recon as unfounded on that basis. It exists
(`body 00185988 - 00185b33`); `get_function_by_address` finds it. A failed lookup is not absence,
and treating it as absence cost most of a session.**

**Both boards must run in a networked match.** The frame driver calls the link memcpy whenever
linking is enabled, and steps board 1 only when the two-board flag is set — so a match with the
flag clear would be feeding a board that never executes. The flag is an *argument*:
`FUN_00184FD0(0|1)` sets it and then resets, called from several game-mode entry points, so
one-board and two-board are both real configurations.

**The cellRudp layer is session traffic, not the link.** `FUN_00131940` reads up to `0x542` bytes,
`FUN_00124A48` dispatches on a 1-byte type (`< 5`), and `FUN_00131**1B4**` relays each message to up
to 8 peer slots. The one sender traced end to end (`FUN_000E644C` → `FUN_000DCEC0` → `FUN_00132010`)
emits **128 bytes once every 60 frames** — a status word of packed bitfields. A 60 Hz board-to-board
link is not a once-a-second heartbeat. (`0x542` is a buffer bound, not a payload size; I quoted it
as evidence of payload size, which it is not.)

### PER-BOARD RENDERING — how the PS3 shows each player their own cabinet

This is the part YAMP needed and did not have, and it validates the `RENDER_BOARD_SELECT` approach.

```c
int FUN_0018EB90(void) {                       // per-board video buffer
    if (FUN_00184AD0() != 1) return DAT_00461BDC + 8;          // board 0
    else                     return DAT_00461BDC + 0x300040;   // board 1
}
FUN_000CC2B0()  ->  FUN_000CC170( FUN_0018EB90(), width, height );   // the blit
```

`FUN_00184AD0()` is the "which board is selected" getter. So **the framebuffer is derived from the
bank selection, and the display blits whichever board is selected** — there is no separate "which
board do I show" flag anywhere. Selection *is* the mechanism, which is exactly what YAMP's two-byte
patch on the frame step's trailing `XOR ECX,ECX` is emulating. It also predicts the PC measurement
that `output_texid` stays 2 while the CONTENT changes: the texture object is fixed, its source
buffer moves with the board.

The emulator's public API is a C++ vtable of PPC64 `{entry, TOC}` descriptors at **0x004550E0**+:
`… 0x184AD0 get_board, 0x184D68 select_board, 0x184DF8 step_frame, 0x184EB8 reset,
0x184FD0 init(board_count) …`. Calls go through an object pointer, so there are no static xrefs to
the individual entries — do not conclude from an empty xref list that nothing calls them.

**Frame-sync primitives, first class:** `FUN_0019F2C8` reads `GlobCntr`, `FUN_0019F470` writes it,
and `FUN_0019F3D0` zeroes it **on both boards** (saving and restoring the selection around the
pair). YAMP arrived at the same address as its round anchor independently; the reference
implementation treats it as *the* frame-alignment handle.

**`MainMode` / `SubMode` decoded.** The PS3 steps the boards at HALF RATE when
`MainMode == 4 && SubMode == 0x0A` (`FUN_0011DF64` → `FUN_0019F2FC`, gated on a mode-3 test and a
flag bit). The PC module reads the same two guest addresses in `module_main` and raises
`execute_info.status` bit6 when `MainMode == 1 || (MainMode == 4 && SubMode == 0x0E)` — which is
what `K2Host`'s `s_startScreen`, and therefore the whole coin/start dance, actually keys on.
Unexplained: why one neighbouring sub-state of that screen runs at half rate.

### THE ROM SYMBOL TABLE — extract it with NO address filter

Full listing in **`docs/von-rom-symbols.md`** (276 entries). PS3 `EBOOT.elf` @ **0x00441070**,
8-byte `{u32 addr, u32 name_ptr}` big-endian records; the PC module has the same table at RVA
**0x4507E0** with 16-byte `{u64, u64}` records.

An earlier extraction filtered to `addr < 0x200000` believing the table named code, and so dropped
**every RAM global** — which is most of what netplay needs. Guest RAM is 0x5xxxxx and the I/O boards
are 0x1Axxxxx.

| guest | symbol | why it matters |
|---|---|---|
| `0x5024E0` / `E4` / `E8` | `SynchFlag` / `SynchTime` / `GlobCntr` | three consecutive dwords — the ROM's own frame-sync block. The anchor is not an arbitrary counter we found by sweeping RAM; it is the third word of the ROM's sync state. |
| `0x5039F4` / `0x503A00` | `MainMode` / `SubMode` | the state pair behind `status` bit6 |
| `0x503A84` | `RoundnumberVS` | versus round counter; a natural round-start cross-check |
| `0x5770B0` / `B1` | `net_flag` / `link_ID` | the ROM's own reaction to being linked |
| `0x072C10` / `0x072EA0` | `combutton` / `comfire` | **what the link actually carries** — button and fire state, alongside `SendNetRobot` (`0x026CB8`) |
| `0x01A12000` / `0x01A12700` | `CommSend` / `CommData` | comm-board MMIO |
| `0x01A14000` / `0x01A14002` | `CommBoardReset` / `CommFlagReg` | bit0 = bank select |

`combutton` / `comfire` / `SendNetRobot` are the answer to "what do the two boards say to each
other": each cabinet's inputs and robot state. In the emulator both boards run locally, so that
traffic is GENERATED on each machine rather than transmitted — which is why the wire only needs
pads, and why the ROM's link code works unmodified.

**CORRECTION to an earlier note in this file:** `0x503A08` is listed as an unnamed "I am the slave"
flag. The symbol table says `TempTimer+0x4`. ~~Neither has been checked against behaviour; do not
rely on either reading.~~ **SETTLED 2026-08-06: "I am the slave" is right.** `InitNetwork` sets it
on the slave arm only, and the slave's picture visibly differs as a result - see the banner at the
top of this file.

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
