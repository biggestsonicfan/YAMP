# Motor Raid — the comm-board protocol, fully decoded (2026-08-09)

The complete map of Motor Raid's linked-cabinet network: the DPRAM window, the 0x1C0-byte
per-node packet, the firmware contract, and the module's own comm-board emulation. Reverse
engineered from the game program (`rom_code.bin`, IDA i960, port 7331 — the ROM symbol table
embedded in the DLL at 0x18017A700, 3987 entries, names everything) with
`mr-pxd-w64-d3d12_retail.dll` (Ghidra, port 5678) as the module-side reference. The ROM's own
source paths (`_am1_users2_yasuda_mb_src_network` etc.) survive in the symbol names.

Motor Raid's network is a RING of up to 8 cabinets plus an optional LIVE MONITOR — richer than
Virtual On's 2-node link — but the module's firmware stub hardcodes a 2-node ring (see §6).

## 1. The guest window and registers

One 16 KB dual-banked DPRAM window plus a register pair — the same architecture as Virtual On's
comm board, different layout inside the window:

```
0x1A10000-0x1A13FFF   comm RAM (handlers mask addr & 0x3FFF; bank = flag reg bit0)
  +0x0000  firmware status:  +0 ring-up (1 = up)     +1 <- ROM writes its link_ID
                             +2 node id (1-based)     +3 total node count
           +0x12 u16 read by the ROM, >>3 stored to +0x14 (unused under the module)
  +0x2000  OWN SEND slot     (0x1C0 bytes - the packet, §4)
  +0x21C0  RX slot 0         (0x1C0)  packet from the node 1 hop downstream
  +0x2380  RX slot 1         (0x1C0)  ... 2 hops downstream
  +0x21C0 + n*0x1C0          RX slot n, up to total_nodes slots
0x1A14000  comm-board reset register (byte; ROM writes 0 = hold, 1 = release)
0x1A14002  comm-board flag register  (byte; bit0 = bank select, bit7 = data ready)
```

`_clearPacket` (0x4A210) proves the slot size: 0x70 word-stores (0x1C0 bytes) at 0x1A12000,
then `total_nodes` more slots of 0x1C0 from 0x1A121C0.

**Ring-relative slot order** (`_checkNetwork` 0x4A890, the two derived indices):

```
nOwnRx    = total_nodes - 1                       ; own echo is always the LAST slot
nMasterRx = (total_nodes - node_ID) % total_nodes ; where the master's packet lands
```

so RX slot k carries the packet of node `((node_ID + k) % total_nodes) + 1` — each hop moves
one slot. Two nodes: the peer is always slot 0, your echo slot 1. This is the same
ring-relative arrangement SRC2's Model 3 comm board uses.

## 2. Roles

`_GAMEASSIGN+0xC` (guest 0x518EFC), from eeprom mirror byte `_EEPData+0x1E` (0x51900E) & 0xF
(`_InitGameSet` 0x13BB0; values >8 are forced to 0 unless 0xF):

| assign | link_ID | meaning |
|---|---|---|
| 0 | 0 | stand-alone (no link check at all — `_exitCommunicationInit`) |
| 1 | 1 | **THIS IS MASTER CONTROLLER** |
| 2-8 | 2 | **THIS IS SLAVE MACHINE** |
| 0xF (>8) | 3 | **THIS IS LIVE MONITOR** (spectator cabinet — `_LiveBikePosSet` renders the field from RX slots) |

The eeprom is a serial part bit-banged through system port 0x1C00000 (`_eepRead` 0x1F500);
`_EEPData` (0x518FF0, 0x28 u16 words) is its RAM mirror, `_eepromReadAll` fills it at boot,
`_InitGameSet` derives `_GAMEASSIGN` from bytes 0x519006..0x519010 inside it.

## 3. The boot check (`_InitCommunication` 0x4B150)

A SubMode-indexed state machine (16-entry table at 0x4B110): `_startComCheck`(0) →
`_checkBoard`(1) → `_checkNetwork`(2) → `_waitCommunication`(3) → `_Wait`(4), with error
states 5-12 (`_messageAndExit`, `_errorStop`, `_commKillError`...).

1. **startComCheck**: sets the sizes the rest of the code uses — `_SizeNetStruct`=0x28,
   `_SizePacket`=0x1C0, `_SizePacket0`=0x188, `_SizeNetOcBuffer`=0x20. Prints "CHECKING
   NETWORK NOW / Ver.5.00". Holds both registers at 0, `_MasterCmdTx`=1, `_NetStatus`=4,
   derives `link_ID` from the assign (§2).
2. **checkBoard**: reads 0x1A14000 — **bit0 SET here means "Nothing Communication Board!"**
   (error). Writes flag = 1 if master else 0, then reset = 1 (release), then polls flag
   **bit7 CLEAR** (a set bit means the firmware is still booting).
3. **checkNetwork**: polls flag **bit7 SET** (data ready), writes its `link_ID` to comm+1,
   then requires comm+0 == 1 (ring up); adopts comm+2 → `_node_ID` (also packet byte +0) and
   comm+3 → `_total_nodes`. total_nodes<2 on a linked cabinet ⇒ registers back to 0 and
   SubMode back to 1 (re-run). Derives `nOwnRx`/`nMasterRx` (§1), prints "Node ID/Total
   Nodes/Cabinet ID", `_LinkMode` = link_ID.
4. **waitCommunication**: publishes `_NetStatus` = 1 and waits until EVERY RX slot's +0x12
   low nibble reads 1 — the all-cabinets-present barrier. Then a SLAVE adopts the master's
   game assignment from `_MasterParaRx+2/+3` (bits: assign&7, +8 flag, race settings) into
   `_GAMEASSIGN`/backup, writes it to eeprom (`_eepromUpdate`) and re-runs `_InitGameSet` —
   **settings authority belongs to the master**, exactly like VON's +0x542 operator block.

`_ResetCommBoard` (0x4A470) = both registers ← 0. `_CheckNetworkBoard` (0x4B180) is the
service-menu probe: resets both, reads 0x1A14000, bit0 set ⇒ `_NetworkBoard`=0 (absent).

## 4. The packet — 0x1C0 bytes per node per frame

### 4a. The 0x28-byte header (`_SendPacket`, guest 0x518F40)

Staged in RAM and copied to the SEND slot every frame by `_SendNetData` (0x4B210: three
16/16/8-byte quad copies). All multi-byte fields little-endian. Writers cited by i960 address.

| offset | size | field | evidence |
|---|---|---|---|
| +0x00 | u8 | **own node id** (from firmware comm+2) | `_checkNetwork` 0x4A984 |
| +0x01 | u8 | bits0-1 = cabinet assign code (&3); **bits2-3 = link-alive counter** (§5); bits4-5 = `_MasterCmdTx` & 3 (§7) | 0x4A9B8 / `_ChkNetworkLink` / `_SendNetData` 0x4B23C |
| +0x04 | u32×2 | **`_MasterParaTx`** — the master-parameter channel: game assignment + race settings broadcast (`_GameAss` 0x4A490 packs assign&7, +8 flag, settings bits); also the race ID pair (`_CalcRaceID` 0x4C394/0x4C3DC writes both words) | `_SendNetData` 0x4B274 |
| +0x08..+0x0B | u8×4 | race-ID bytes (read side: `_NetGetRaceID` reads slot +8/+9/+A) | 0x4C698.. |
| +0x0C | u16 | **race rest time** (countdown) | `_initGameParameter` 0x266E4, `_restTimeDec` 0x269E4 |
| +0x0E | u16 | **frame heartbeat** — incremented once per frame by `_mainLoop` 0x3CB64; peers age timeouts off it (`_NetCounter` reads slot +0xE) | |
| +0x10 | u16 | course-select-end marker | `_SendCourseSelectEnd` 0x4B4BC |
| +0x12 | u8 | low nibble = **`_NetStatus`** (1 = present/waiting, 4 = in check — the §3 barrier reads this); bits5-8 (u16) = cabinet number | `_SendNetData` 0x4B26C; `_checkNetwork` 0x4A97C |
| +0x13 | u8 | race/entry flags; **bit5 is auto-cleared after every send** (one-shot pulse, set by `_NetGetRaceID`/`_NetWorkGameInit`) | `_SendNetData` 0x4B2E8 |
| +0x14 | u8/u16 | **entry status** — entry/start handshake bits (`_EntryModeInit`, `_entryTaskRun`, `_liveEntryTaskRun`, `_entryWaitTaskRun`, `_NetStartCheck`, `_scWaitOther`); live monitor (assign 3) forces 0xF patterns here | 0x22710.. |
| +0x15 | u8 | entry-wait / course-select / race-start bits (`_GentlemenInit` 0x2E7D8 = "gentlemen, start your engines", `_OtherCabinetChkWait`, `_NetWorkGameOut`) | |
| +0x16 | u8/u16 | checkpoint / lap-extend request (`_checkChkPointNormal`, `_NetExtendChk`) | 0x27200.. |
| +0x17 | u8 | game-over flag (`_timeOutGameOver`, `_NetAllGameOverChk`, `_NetGameOverChk`) | 0x26B1C.. |
| +0x18 | u8/u16/u32 | attract-demo sync (`_AdvtiseMode` 0x45C8, `_ChkDemoSync` — linked attract loops run in step); ranks all (u32, `_CalcPlayerRankAll`) | |
| +0x19 | u8 | goal-in flags (`_GameCtrlConst`, `_goalInChampionShip`) | |
| +0x1A/+0x1B | u8/u8 | per-player rank data (`_CalcPlayerRankAll`, `_GetPlayerRankAll` reads slot +0x18..+0x1B) | |
| +0x1C | u16 | cleared by `_NetWorkGameInit` | 0x4BBB4 |
| +0x20 | u32 | rest-time copy | `_restTimeDec` 0x2696C |
| +0x24 | u32 | race-goal state (`_RaceGoalCheck`; readers `_CalcPlayerRank`/`_ChkAllPlayerGoal` at slot +0x24) | 0x26E68 |

The receive path (`_GetNetData` 0x4B300) copies the MASTER's slot (+1 cmd bits4-5 →
`_MasterCmdRx`, +4 u64 → `_MasterParaRx`) every frame; everything else is read in place from
the RX slots by the functions named above.

### 4b. The bike marshal — 0x20-byte records at +0x28

Written straight into the SEND slot (not via `_SendPacket`) by `_NetGameDataTransRun1`
(0x21D5B0, RAM2BASE overlay; the ROM stores the overlay image at file offset +0x150000, which
is why the writers first appeared as "loose code at 0xCD388"). One record per LOCALLY-OWNED
bike, slot index from `_ShipCalcChkBuf[i]` bits 1-4; remote bikes are read back by
`_GetNetPlayerStatus` (0x21D350) from `RXslot + 0x28 + idx*0x20`, owner node from
`_ShipCalcChkBuf[i]` bits 5+ — **one authority per bike, ring-distributed**, the same
ownership model as SRC2's CPU cars. (0x28 + 12*0x20 = 0x1A8 ≤ 0x1C0: up to 12 bikes.)

| rec offset | size | field (`jet` = the bike object from `_JetList`) |
|---|---|---|
| +0x00 | u32 | bitfield: low byte = state flags (bits from jet+0x178/+0x184/+0x44); bits6-9 = jet+0x178 bits5-8; **bits14-27 = jet+0x188 & 0x3FFF** (course-position/checkpoint value) |
| +0x01 | u8 | bits2-5 = jet+0x185 bits3-6 |
| +0x03 | u8 | bits4-7 = jet+0x45 bits1-4 |
| +0x04 | u8 | steering/lean flags (jet+0x178 bit0, jet+0x179 bits1-3 repacked) |
| +0x08 | u32 | jet+0x354 |
| +0x0C | f32 | **pos.x + 2·vel.x** (jet+0x48 + 2*jet+0x88) |
| +0x10 | f32 | **pos.y + 2·vel.y** |
| +0x14 | f32 | **pos.z + 2·vel.z** |
| +0x18 | u32 | lo16 = jet+0x54 (heading); hi16 = jet+0x58 + jet+0xE8 (angle + delta) |
| +0x1C | u32 | lo16 = jet+0x5C + jet+0xEC; byte2 = jet+0x17C; byte3 = jet+0x180 |

**The extrapolation is the hardware's own latency compensation**: the sender transmits
position PLUS TWICE the per-frame velocity, and the receiver adopts it as position — a
built-in dead-reckon that absorbs the ring's delivery latency. The receiver also keeps the
previous pair (stl to jet+0x7C/+0x84) for its own smoothing.

## 5. The link watchdog (`_ChkNetworkLink` 0x4CE40, per frame)

Packet byte +1 bits 2-3 hold a **mod-3 alive counter** (0→1→2→0; the value 3 is reserved as
the ERROR TOKEN). Each cabinet increments its own counter every frame and checks that its
DOWNSTREAM neighbour's counter (RX slot 0, byte +1) advanced by exactly one step. Accounting:
mismatch ⇒ `_NetworkErrorCount` += 8, match ⇒ −5 (only above 30); count > 480 ⇒ the link is
declared dead: MainMode=1/SubMode=9 (network-error screen) and the cabinet broadcasts counter
= 3, which every peer adopts and propagates — the error token circles the ring so all
cabinets fail together. The counter is stamped into BOTH `_SendPacket+1` and the live window
byte 0x1A12001 every call.

Implication for a transport: the peer's packet must keep FLOWING (a resident re-laid packet
alone fails the watchdog — the counter inside it must advance), and delivery should be
ordered; a burst of stale packets replays old counter values and charges +8 per step.

## 6. The module's comm board (mr-pxd-w64-d3d12_retail.dll)

The DLL ships the same comm-board emulation as Virtual On's `omg` module, at different RVAs:

```
0x741E48  pointer to the CURRENT board's comm block (bank switch re-points it)
0x741E50  board 0's block; board 1 = +0x8008        (block: 2 banks x 0x4000)
  +0x8000 reset register   +0x8001 flag register    +0x8004 firmware-boot latch
0x741E38  board index      0x741CB6  two-board gate  0x741CB7  LINK gate
0x521A0   FUN_1800521a0  bank switch (also re-points i960 ctx 0x624250+board*0x1B8,
          work RAM: board0 0x500000-window at RVA 0xFB6350, board1 at 0xCB6310)
0x52400   FUN_180052400  per-board reset: memset(block,0,0x8000), regs preset 0x7EFE,
          latch cleared; second board only if the two-board gate is up
0x522E0   FUN_1800522e0  module soft reset (zeroes gates THEN calls 0x52400 - the same
          gate-zeroed-before-reset trap VON's RESET had)
0x52BA0-0x52FD0  eleven access handlers (byte/word/dword/qword/12B/16B read+write), all
          ORPHAN RUNS invisible to function sweeps; addr & 0x3FFF into bank flag&1;
          0x1A14000/2 map to +0x8000/+0x8001
0x5C030   FUN_18005c030  the LINKED-mode frame driver: four 0x1C0 copies per frame
          (board0.SEND -> board1.RX[0] + board0.RX[1](echo); board1.SEND -> board0.RX[0]
          + board1.RX[1]), each into the bank the destination is NOT reading, then toggles
          bit7 AND bit0 on both flag registers, then steps the i960(s)
```

**The firmware boot response** lives in the byte-write handler (0x52DB0): on any write to
0x1A14000/2 with the latch clear and reset released, it latches, clears flag bits 7+0, and
writes `comm[0]=1, comm[2]=board_index+1, comm[3]=2` into both banks — ring-up, node id,
**and a hardcoded 2-node count** (the PS3-era local two-cabinet link).

**`FUN_18005c030` IS the retail frame driver** — a first reading called it "linked-mode
only" because its whole body is gated on 0x741CB7, but that flag means "machine running",
not "linked cabinets": the boot state machine raises it when the machine app starts
(`FUN_18005bf20`), and the measured proof is its io refresh (`FUN_180054c00`, its only
caller) rebuilding io[9] on every `module_main`. So the transfer, the flag discipline and
the firmware boot response all run every frame in retail, exactly like Virtual On's module
— and a link transport uses VON's architecture verbatim: **stage the peer's packet into
board 1's SEND buffer** (bank = board 1's flag bit0, read just before `module_main`) and
the module's own transfer delivers it to board 0's RX slot 0, correctly banked, with the
echo and data-ready flags handled by the module. YAMP adds only the firmware status bytes
(ring-up / role-correct node id / count, both banks of board 0 — overriding the boot
response's hardcoded board+1 / 2 nodes) and the wire itself. Do NOT drive the flag register
from the host: the module toggles it per frame, and a second toggle cancels the data-ready
protocol.

**Cabinet TEST/SERVICE** ride the refresh the same way as every other m2ftg module: the ROM
reads io[9] bits 2/3 (active low) through port 0x1C00002 (`io[8]&1 ? io[10] : io[9]`;
io[0xA] is the DIP byte, forced 0xFF), and the refresh rebuilds io[9] each frame with those
bits released — so the host must clear them AFTER the refresh, at its two call sites inside
the frame driver (DLL +0x5C386 / +0x5C3A3, io-state pointer +0x75A0A8). Analogue controls:
the module's own default assignment template maps the pad block's `m_x1` to the handlebar
(ch2, with the ROM's mirror LUT), `m_buttons[5]` (RB pressure) to accel (ch0) and
`m_buttons[4]` (LB pressure) to brake (ch1) — measured with the `-mr-iotest` sweep.

## 7. Master channels and authority

- `_MasterCmdTx/Rx` (2 bits, packet +1 bits4-5): the master's command strobe (1 = assignment
  broadcast during the check; game phases use it for start/sync commands).
- `_MasterParaTx/Rx` (8 bytes, packet +4): the master's parameter payload — game assignment
  and race configuration during the check barrier (§3.4), race ID during entry.
- Master-authoritative: game assignment/settings (adopted + written to eeprom by slaves),
  race ID, rest time. Per-bike state is per-OWNER authoritative (§4b), rank/goal fields are
  published by everyone and cross-read.

## 8. Protocol properties a transport must honour

1. **Per-frame flow, not residency**: the §5 watchdog requires the neighbour's alive counter
   to ADVANCE. Holding the last packet resident satisfies the DPRAM model but still fails
   the link after ~2 s of no fresh packets (480/8 = 60 mismatch charges... conservatively,
   error declared after sustained loss). Delivery must be ordered; replayed stale counters
   charge the error accumulator.
2. **The +0x12 status barrier and +0x14/+0x15 entry handshakes are edge-driven** one-frame
   states, like VON's 0x24 stage roll: both cabinets must run their boards at the same rate
   (60 Hz wall) or single-frame states can be overwritten before the peer's ROM reads them —
   the exact VON stage-desync failure mode.
3. **The check needs the ring DOWN until a real peer answers** (comm[0] truthful): raising
   ring-up without a peer walks the master into the game alone (VON's lesson, and MR's
   checkNetwork re-runs the check on a 1-node answer regardless).
4. **node id / total count must come from the transport's role**, not the module's hardcoded
   board_index+1/2 — on a one-board-per-machine link both machines are board 0, so the
   firmware bytes must be overridden per machine (master=1, slave=2, count=2).
5. **The role must exist before the check runs**: link_ID comes from the eeprom-derived
   `_GAMEASSIGN+0xC` read during boot. A role decided by an RPCN room (which forms after
   boot) needs the guest bytes pinned (0x518EFC and its eeprom-mirror source 0x51900E) and
   the board REBOOTED into the role — SRC2's `DriveRoomRole` model, with the module's own
   soft reset (0x522E0; sample-and-restore the gates it zeroes, per the VON RESET fix).
