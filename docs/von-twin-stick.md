# Virtual On: the cabinet's control set, and driving it from a real Saturn Twin Stick

Reference for the two halves of `source/input/BlissBox.*` + `source/m2ftg/K2/VonTwinStick.*`.
Everything in the module half was read out of `omg-pxd-w64-gog_retail.dll` (Ghidra, 2026-08-21);
everything in the adapter half comes from the vendor's own published API and mapping sheet, with
the two items still needing a stick in a port called out at the end.

## 1. The module's cabinet inputs

`execute_info+0x15E4` selects one of five 0xC0-byte entries in the table at `DLL+0x12ABB0`.
`FUN_180004BE0` copies the chosen entry into globals each frame; `FUN_180004CC0` walks its 24
`{mask -> code}` pairs and sets **one bit per matched code in a 110-bit set**, so any number of
cabinet inputs can be held at once. `FUN_180081140` is what produces the mask, from `csl_pad`.

### The codes

| code | cabinet input | code | cabinet input |
|------|---------------|------|---------------|
| 0x03 | left lever up | 0x0B | right lever up |
| 0x04 | left lever down | 0x0C | right lever down |
| 0x05 | left lever left | 0x0D | right lever left |
| 0x06 | left lever right | 0x0E | right lever right |
| 0x07 | left weapon trigger | 0x09 | right weapon trigger |
| 0x08 | left dash (thumb) | 0x0A | right dash (thumb) |
| 0x02 | start (reached via sl `BUTTON_START`) | 0x6D | the other start-shaped code, via sl `BUTTON_BACK` |
| 0x6E | filler — "this input is unmapped in this entry" | 0x6F | expands to {0x07, 0x09} (both triggers = centre weapon) |

The direction labels are not guesses. The beginner entries reach the same levers through composed
gesture codes, and `FUN_180004CC0`'s composer spells each one out:

```
0x70 -> {0x03, 0x0B}   both levers up      walk forward
0x71 -> {0x04, 0x0C}   both levers down    walk back
0x72 -> {0x05, 0x0D}   both levers left    strafe left
0x73 -> {0x06, 0x0E}   both levers right   strafe right
0x74 -> {0x04, 0x0B}   left back  + right forward    turn
0x75 -> {0x03, 0x0C}   left forward + right back     turn the other way
0x76 -> {0x05, 0x0E}   left LEFT  + right RIGHT  = both outward = JUMP
0x77 -> {0x06, 0x0D}   left RIGHT + right LEFT   = both inward  = CROUCH
```

Jump and crouch only come out as jump and crouch under this labelling, and it independently
reproduces what the beginner scheme was already observed to do in-game (K2Host's note: Punch read
as crouch, Guard as jump — entry 0 maps them to 0x77 and 0x76 respectively).

## 2. csl_pad -> module mask (`FUN_180081140`)

**The reader permutes, in two groups, because the module enumerates directions U, R, D, L where sl
enumerates them U, D, L, R.**

```
sl BUTTON_UP    (b12) -> module b12      sl BUTTON_A (b0) -> module b2
sl BUTTON_DOWN  (b13) -> module b14      sl BUTTON_B (b1) -> module b1
sl BUTTON_LEFT  (b14) -> module b15      sl BUTTON_X (b2) -> module b3
sl BUTTON_RIGHT (b15) -> module b13      sl BUTTON_Y (b3) -> module b0
sl BUTTON_START (b8)  -> module b9       sl BUTTON_BACK (b9) -> module b8
sl LB/RB/LT/RT  (b4-b7) pass through unchanged
```

The module's bits 16-23 are **not read from `m_now` at all**. They are derived from the four analog
floats against a threshold of `0x4651/0x7FFF` ~ 0.55, in the same U, R, D, L order:

```
m_y1 <= -0.55 -> b16    m_x1 >= +0.55 -> b17    m_y1 >= +0.55 -> b18    m_x1 <= -0.55 -> b19
m_y2 <= -0.55 -> b20    m_x2 >= +0.55 -> b21    m_y2 >= +0.55 -> b22    m_x2 <= -0.55 -> b23
```

**So the right lever is unreachable from button bits — it lives only behind `m_x2`/`m_y2`.** A fill
that sets `sl::BUTTON_R_UP` and friends moves nothing whatsoever.

## 3. The five entries

| entry | what it is |
|-------|-----------|
| 0, 1 | beginner: face buttons and d-pad fire the composed gestures above |
| 2 | partial — most masks are 0x6E filler |
| **3** | **full discrete set**: d-pad *and* left stick -> left lever, face buttons *and* right stick -> right lever, shoulders -> triggers/dashes |
| 4 | partial |

Entry 4 is also force-selected by `FUN_180004CC0` when the device record's type field is 2,
independently of the byte — a d-pad + right-stick arrangement with the face buttons unmapped.

The twin-stick override pins the byte to **3** while a stick is live. Note it is one byte **per
cabinet, not per player**: a stick on either side puts both players on entry 3.

## 4. The adapter half

Bliss-Box, VID `0x16D0`, PID `0x0D04 + port`. Each port is its own USB device. Get commands are HID
`GetFeature` calls whose report ID is the command.

**Where the reply starts is not fixed, and must be probed.** A numbered HID feature report is
supposed to echo its report ID in byte 0 and put the body at byte 1; the adapter measured here
(firmware 4.2) returns the body alone. Assuming the prefix shifts every field one byte, which puts
the left lever's two axes exactly where the button rows are read and leaves half the stick dead
while the other half appears to work — the symptom was physical up registering as the board's left.

`ProbeDataOffset` settles it per device off the adapter-info report's PLAYER ID field, which the
vendor documents as 4..7 ("4 = player 1"). Only one of the two framings can put a valid value there.
On this bench the probe reads `03 78 04 04 02` and picks offset 0, giving controller type **3
(`SATURN_DIGITAL`)**, modes `0x78`, firmware 4.2, player 1. Read with the wrong offset the same
bytes say "type 120", which is in nobody's enum — a good reminder that an unlisted device ID is
often a framing bug wearing a hat.

* `0x11` adapter info — `[0]` controller type, `[1]` modes, `[2]` major, `[3]` player ID (minus 3),
  `[4]` minor
* `0x10` payload — `[0]` player ID, `[1..3]` button rows 1-3, `[4..9]` X/Y/Z/X2/Y2/Z2,
  `[10]` slider, `[11]` dial, `[12]` HAT

Sega built the Twin Stick (HSS-0151) on the **same double-74153 encoder as the standard Saturn
pad**, so neither the Saturn nor the adapter can tell them apart — the port reports `SATURN_DIGITAL`
(3) either way, and there is no twin-stick device ID to look for. What makes it decodable is the
fixed correspondence between the levers and the pad's thirteen inputs
(NFG/GameSX wiki, `controls:twin_stick_button_layout`), composed with the vendor's Saturn HID
numbering (their global mapping sheet, "version 3 & 4" table):

| Twin Stick | Saturn | HID | | Twin Stick | Saturn | HID |
|---|---|---|---|---|---|---|
| left lever U/D/L/R | d-pad | main X/Y axes | | right lever up | Y | 4 |
| left trigger (weapon) | L | 9 | | right lever down | B | 2 |
| left thumb (dash) | R | 10 | | right lever left | X | 3 |
| Start | Start | 6 | | right lever right | Z | 7 |
| | | | | right trigger (weapon) | A | 1 |
| | | | | right thumb (dash) | C | 8 |

## 5. Verification status

**CONFIRMED on hardware** (Twin Stick on a Bliss-Box, firmware 4.2, read off Virtual On's own
input test plus a `-blissbox-dump` capture):

* **Button-row bit order.** HID button *n* = row `(n-1)/8`, bit `(n-1)%8`. Over a full session the
  union of bits seen was row1 `0xEF`, row2 `0x03`, row3 `0x00` — i.e. HID `{1,2,3,4,6,7,8,9,10}`,
  exactly the vendor sheet's Saturn set, with HID5 absent because Saturn has no button there. Nine
  buttons plus the four axis directions is thirteen distinct switches, the Twin Stick's exact
  count, and nothing landed on an unexpected bit.
* **Left lever on the main X/Y axes**, not the button rows and not the HAT (which rests at `0xFF`).
* **Vertical sense**: `y` low = up. Established by the pre-fix run, where physical up drove the
  byte the code then mis-read as `x` and produced module code `0x05` — the board's LEFT. Re-read
  through the corrected framing, that same measurement says y-low is up.
* **Controller type 3, alt map 0** — so the default HID numbering above is the one in force.

**Still inferred**, and only settleable in play rather than in an input test:

0. **Horizontal sense.** `x` low is taken as LEFT. Conventional, and the only direction no
   measurement has tied to a physical push.
1. **Trigger vs dash.** Codes `{0x07, 0x09}` are taken as the two weapon triggers because gesture
   code `0x6F` expands to exactly that pair and "both triggers" is the cabinet's centre-weapon
   input; `{0x08, 0x0A}` are then the dashes. If in-game they turn out swapped, exchange
   `BUTTON_LB`/`BUTTON_LT` and `BUTTON_RB`/`BUTTON_RT` in `VonTwinStick.cpp`.

The **HAT byte is deliberately not decoded** — see the comment on `DecodeDirections`. The bench
adapter (firmware 4.0, modes 0x04, Analog-to-D-pad off) reports `hat 0x00` at rest, and 0x00 is UP
in the ordinary 0-based HID encoding, so trusting it risked a permanently stuck lever. The axes
carry the same four switches unambiguously.

## 6. Netplay caveat

`plugin/yampnet/PadCodec.cpp` quantises only `m_x1`/`m_y1` (into the `L_*` bits) and **zeroes
`m_x2`/`m_y2`**, so the right lever would not survive a pad-transmitting session. This does not
affect Virtual On today: its host calls `Cabinet::RoutePads(pads, false, -1)` and its linked-cabinet
play carries the other cabinet's state through the comm-board DPRAM, not through the pad codec. It
would matter if Virtual On were ever moved onto the transmitted-pad path.
