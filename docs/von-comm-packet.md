# Virtual On — the comm-board packet, fully decoded (2026-08-09)

The complete field map of the 0x700-byte linked-cabinet packet, reverse engineered from
`von_prog.bin` (IDA, i960, port 7331) with the `omg` DLL (Ghidra, port 5678) as the module-side
reference. Method: scan the image for address constants into the two windows (i960 absolute
stores have no data xrefs), then disassemble every writer/reader against the ROM's own symbol
table (`docs/von-rom-symbols.md`).

## The exchange (i960 vsync path, 0x1440–0x15AC)

```
cSend = 0x5032F0 (0x700 bytes)   cRecn = 0x5024F0 (2 slots x 0x700; slot 1 never filled)
comm-RAM SEND window = guest 0x1A12000, RECV window = guest 0x1A12700
staging = 0x501CE0 (so "0x502236" in old notes = staging+0x556, not a global)

per board frame, when net_flag==1 and the ring is up:
  seq            = 1823 * ([cSend+2] + 3)         ; u16, 0x14B4..0x14F8
  [cSend+2]      = seq
  [cSend+0x556]  = seq ^ 0xAE5E
  memcpy(0x1A12000, cSend, 0x700)                 ; out
  memcpy(0x501CE0, 0x1A12700, 0x700)              ; in, staged
  accept iff staged[+0x556] == staged[+2] ^ 0xAE5E
  memcpy(cRecn, staged, 0x700)                    ; only a validated packet lands
no net_flag: [cRecn+4] = 0xFE (and slot 1's +4 too, 0x502BF4)
ring down / rejected: [cRecn+4] = 0xFF
```

The stamp is a self-consistency check, not an ordering contract — but several fields below make
ordered, per-board-frame delivery matter anyway (see "protocol properties").

## Field map

All multi-byte fields little-endian. "Writer/reader" cites the i960 address that settled it.

| offset | size | field | evidence |
|---|---|---|---|
| +0x000 | 2 | unused (cleared by `Net_check`'s 0x700 memset, 0xC58EC/0xC5910) | |
| +0x002 | u16 | **seq stamp**, `1823*(prev+3)` | 0x14B4/0x14F8 |
| +0x004 | u16 | **sender's mode/state** — the entire versus handshake | every mode handler; receive sentinels 0xFE/0xFF |
| +0x006 | 2 | unused | |
| +0x008 | u16 | **stage id**. Roller publishes `table_0x19430[rand()&7]` with state 0x24 (0xCDB30–0xCDB64); adopter takes it at 0x19770 → 0x503A84 → `FieldNo` 0x5770F0 (InitGame 0x198D8). `SelectV` presets 0xFFFF = "not chosen" (0x18EF8). Solo: current stage number (0x503A80, 0x19810/0x19984) | |
| +0x00A | u16 | solo-challenge opponent char (low16 of stage-table entry 0x504CA0) | InitGame solo tail 0x19B28 |
| +0x00C | u16 | **own character id** (0x503A98) | published 0x19688 (`InitWaitChallenger`); adopted 0x1976C → 0x503A9C (peer char); challenge path reads it 0xCD5C8 |
| +0x00E | u16 | **game clock** (0x503A14) — master runs it, slave adopts | master 0x1A568; slave 0x1A4F0; `Game_00` presets 0xFFFF (0x1A10C) |
| +0x010 | u16 | secondary clock / overtime counter (0x503A20) | master 0x1A570; slave 0x1A4FC |
| +0x014 | u32 | time limit (0x503A18; 0xF423F = infinite/freeplay) | InitGame 0x19954 (versus) / 0x19B08 (solo) |
| +0x018 | u16 | round-count setting (0x503A78, from backup 0x1D0001E/0x1D0001F) | 0x1994C / 0x19B10 |
| +0x01A | u16 | round trio A (0x503A74) | zeroed+published InitGame 0x19874; updated `Game_20` 0x1B348 |
| +0x01C | u16 | round trio B (0x503A6C) | 0x1987C; 0x1B338 |
| +0x01E | u16 | round trio C (0x503A70) | 0x19884; 0x1B340 |
| +0x020 | u16 | match counter (0x503A8C) | `Game_00` 0x1A04C |
| +0x022 | u16 | **sound-cue byte** ("BB" channel): `sendBB` pops one queued byte per frame to the local sound port 0x1C00008 AND mirrors it here; when its own queue is empty it EDGE-DETECTS `cRecn+0x22` against 0x504C78 and plays the peer's byte — synced announcer on both cabinets | `sendBB` 0x18538; `InitBB` presets 0xFF |
| +0x028..+0x114 | ~0xEC | **robot state marshal** — the local robot object copied field-by-field (id/flags +0x28, position words +0x2C..+0x40, orientation/anim halfwords +0x44..+0x54, matrices/joint quads +0x60..+0xB0, health/status +0xB8..+0x114) | loose-code marshaller 0x26980 (copy at 0x26E08); `initRobot` 0x27550 presets; remote-mirror readers around 0x27140/0x273AC |
| +0x514 | s8 | **P1 robot id** (0x504DAC) | writer 0x27418; `comfire` 0x72EE0 adopts on the non-master (slot from comm-flag bit0 — the `Advertize` scan's memo, see below) |
| +0x515 | s8 | **P2 robot id** (0x504DB0) | 0x27410 / 0x72EF4 |
| +0x516 | u16 | spawn: obj+0x108 | 0x27424 |
| +0x518/+0x51A/+0x51C | u16 ×3 | spawn: obj+0x1D0/+0x1D2/+0x1D8 | 0x27430.. |
| +0x520/+0x524/+0x528 | u32 ×3 | spawn position xyz (obj+8/+0xC/+0x10) | 0x27454.. |
| +0x52C | f32 | spawn heading, `float(sext16(obj+0x2E))` | 0x27488 |
| +0x530..+0x540 | u16 ×9 | **one-shot event block** (`SendNetRobot`, obj+0x3E..+0x50). In versus the sender CLEARS the source after copying (obj+0x46=0xFFFF, +0x4A/+0x4C=0, 0x26D3C..0x26D48); receiver applies only when `cRecn+0x53A != 0` (0x26D7C). A truly lost packet loses the event — the ROM has no retransmit | send 0x26CB8; recv 0x26D60 |
| +0x542..+0x555 | 0x14 | **operator-settings mirror**: `memcpy(cSend+0x542, backup 0x1D00016, 0x14)` every `GameMain` frame (0x19254–0x19268). The SLAVE adopts the master's settings: +0x546→backup 0x1D0001A (versus time), +0x54B→0x1D0001F (versus rounds), +0x54C/+0x551/+0x550 → 0x503AA0/A4/A8 (0x19294–0x192CC). Settings authority = master | |
| +0x556 | u16 | **check** = seq ^ 0xAE5E | 0x1500; accept test 0x1524 |
| +0x558..+0x6FF | | unused | |

## Protocol properties the transport must honour

1. **State 0x24 (stage roll) lives for ONE board frame** — the roller publishes stage+0x24 and
   moves on; `WaitChallenger` (0x196C0) makes a one-shot decision on the peer's state byte.
   Requires ordered delivery and at most one packet consumed per board frame.
2. **+0x530 events are one-shot** — cleared at the sender after a single transmission. Requires
   the receive path not to discard packets (queue, don't overwrite), and makes redundancy cheap
   insurance.
3. **+0x22 sound cues are edge-detected** — a dropped or coalesced packet loses an announcer cue
   (cosmetic, but the same ordering argument).
4. **Master authority**: clock (+0xE/+0x10), time limit (+0x14), rounds (+0x18), operator
   settings (+0x542 block) all flow master→slave. The slave publishes the same offsets but the
   master never reads them for adoption.
5. **The `Advertize` scan (0x2BADC) memos the hit slot index into comm-flag reg 0x1A14002**, and
   `comfire` (0x72EE0) reads it back with `&1` to pick the cRecn slot for char adoption. Under
   the module the transfer rewrites that register every frame (bank/bit7 semantics, observed
   0x81/0x00). Char adoption demonstrably works on the verified two-machine runs, but if char
   select ever misbehaves in linked play, THIS register's bit0 is the first suspect.

## Verdict vs YAMP's implementation (2026-08-09)

**No functional difference — nothing to reimplement.** YAMP carries the entire 0x700 window
verbatim (`kPacketLink`: RLE + raw fallback, 3× redundancy, ordered 8-deep queue, one take per
board frame, resident re-lay via `StageCommPayload`), so every field above rides the wire by
construction, stage id included. Every offset the code interprets was re-verified against the
ROM this pass and is exact: `PKT_SEQ`+2 (and the 1823*(n+3) formula), `PKT_STATE`+4 (u16; 0xFE
no-net / 0xFF no-packet sentinels), `PKT_STAGE`+8 (0xFFFF preset during select), `PKT_CHECK`
+0x556 ^ 0xAE5E, `SYM_STAGE_SEL` 0x503A84, `SYM_FIELDNO` 0x5770F0. The delivery discipline the
code already enforces (ordered, one-per-board-frame, resident) is exactly what properties 1–3
require; properties 1–3 are the reverse-engineered justification for it.
