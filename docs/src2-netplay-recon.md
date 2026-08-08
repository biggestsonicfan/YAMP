# Sega Racing Classic 2 netplay — the linked-cabinet path

Recon 2026-08-07 against `pre3-pxd-w64-d3d12_retail.dll` (the copy in
`build/bin/Win64/Debug/pre3/`), read in Ghidra, and then **RUN**: §5a is what a single machine
measured, and it confirms §2 end to end. Anything still inferred says so where it stands.

**The headline: pre3 already contains a complete emulated Model 3 comm board, and the wire format
is not ours to invent — the module defines it.** SRC2 does not need YAMP to fake a link the way
Virtual On's did. It needs YAMP to fill in three pointers, two config bytes, and a datagram channel
between them.

---

## 1. Why this is the Virtual On shape and not the Fighting Vipers 2 shape

| | FV2 (pre3, shipped) | Virtual On (m2ftg) | **SRC2 (pre3, this document)** |
|---|---|---|---|
| cabinets | one board, one screen | two boards, one per machine | **two boards, one per machine** |
| netplay model | input lockstep + savestate reset | the ROM's own link protocol on the wire | **the ROM's own link protocol on the wire** |
| determinism required | yes (canary, clock pin, seed) | no | **no** |
| what crosses the wire | pads | the 0x700 comm-RAM window | **the comm board's packet slots** |
| who synchronises | YAMP's barrier | the ROM | **the ROM** |

So the plan is Virtual On's, and [`docs/von-handoff.md`](von-handoff.md) is the closer reference
than [`docs/pre3-netplay.md`](pre3-netplay.md) — despite SRC2 living in the pre3 tree.

The difference in YAMP's favour: Virtual On needed the comm-board *firmware* faked
(`DriveCommFirmware`) because the module's own was a stub. **pre3's is real**, so there is less to
invent and more to satisfy.

---

## 2. The chain from guest address to host buffer, every link read out of the module

### 2.1 The guest window is live, not stubbed

`CM3Mem`'s address decode is a 17-entry table of `{topByte, read8, write8, read16, write16, read32,
write32}` at DLL **0x1801077C0**, stride 0x38. Entry 9 is top byte **0xC0** — the Model 3 network
board window, the same address MAME's `model3.cpp` maps for the Daytona II family.

Its six handlers (0x180012AE0/B00/B20/B40/B70/BA0) all do the same thing:

```
mem = [0x18052AA80]                  ; the CM3Mem pointer
dev = mem->0x360                     ; the network device
if (dev) tail-call dev->vtable[+0x40 .. +0x68](dev, addr [, value])
else     return 0                    ; and discard writes
```

`mem + 0x360` is filled at machine build, by `CM3Mem`'s init (`FUN_180014180`, vtable slot 0x70,
called from `FUN_1800393D0` right after the 0x378-byte object is constructed). **It is the ROM
object.** So slots 8-13 of the ROM's vtable *are* the network-board accessors.

**This is the opposite of the security board.** There (see
[`src2-hle-hooks.md`](src2-hle-hooks.md)) `M3ERomSrc2` slots 2/3 are `return 0` and a bare `ret` —
a stub that lies. Here:

```
M3ERomSrc2::vftable = 0x18010C3F0        (the factory's `lea rax,[rip+0xD37EE]` at 0x180038BFB)
  [ 8] +0x40  0x18000DF20      read8
  [ 9] +0x48  _guard_check_icall   write8 discarded — the board is 16/32-bit only
  [10] +0x50  0x180037890      read16    <-- real
  [11] +0x58  0x1800378E0      write16   <-- real
  [12] +0x60  0x180037930      read32    -> FUN_180035DF0(rom+0x588)
  [13] +0x68  0x180037940      write32   -> FUN_180035EA0(rom+0x588)
```

and `FUN_180037890` / `FUN_1800378E0` are:

```c
comm = rom + 0x588;
if ((addr >> 16 & 0xFF) == 0)          // 0xC000xxxx - the RAM window
    halfword at comm + 8 + bank*0x10000 + ((u16)addr ^ 2)      // byte-swapped
else if (top == 2 || top == 3)         // 0xC002xxxx / 0xC003xxxx - the registers
    comm->vtable[5 or 6](comm, addr)
```

### 2.2 `rom + 0x588` is a `CXComm`, and it is a real device

The class exists in the module's RTTI (`.?AVCXComm@?A0x091d97c1@@`, type descriptor 0x18018BCA0),
its complete-object locator is at **0x18010FF48** and its vtable at **0x18010C858** with seven
methods. It is **embedded as a subobject**, not allocated: the ROM factory (`FUN_180038720`) writes
the `CXComm` vtable pointer into the ROM object for exactly two of the six games —

* case 3, Spikeout FE, at `rom + 0x5E0`,
* **case 5, SRC2, at `rom + 0x588`**,

which is why both allocate **0x20610** bytes where every other game allocates 0x5E0-0x5F0. The two
64 KB banks are the size difference.

Object layout, read off the seven methods and the state machine:

| offset (from `comm`) | field |
|---|---|
| `+0x00000` | vtable |
| `+0x00008` | **bank 0**, 0x10000 bytes |
| `+0x10008` | **bank 1**, 0x10000 bytes |
| `+0x20008` | **node id** (this cabinet's index) |
| `+0x2000C` | **node count** |
| `+0x20010` | which bank the guest currently sees (0/1) |
| `+0x20014` | status bits (bit 0 set on any command) |
| `+0x20018` | **command register** — bit 15 set = a command is pending |
| `+0x2001C` | status/ready word the guest polls |
| `+0x20020` | register at guest `0xC0020800`, byte-swapped |
| `+0x20022` | register at guest `0xC0020804`, byte-swapped |
| `+0x20024` | **packet size in bytes**, register at guest `0xC0020808` |
| `+0x20026` | frame/sequence counter, mirrored into the far bank at `+0x0E` |
| `+0x20028` | the transfer state machine's state (1..5) |

Commands the guest can write (`FUN_180035970`):

| value | effect |
|---|---|
| `0xA000` | zero the current bank |
| `0xB000` / `0xB001` / `0xB002` | select bank 0 / bank 1 / toggle |
| `0xF000` | **"I am ready"** — clears the ready word and sets bit `nodeId + 0x20` in the host's rendezvous word |

### 2.3 The transfer itself: `FUN_180035BA0`, called once per emulated frame

`M3ERomSrc2`'s per-frame task is `FUN_180037B70` (its own vtable slot 23, `+0xB8`). Its body is:
set the audio volume, run the shared input update (`FUN_1800336D0`), `FUN_180033E20`, then

```c
FUN_180035BA0(rom + 0x588);       // pump the comm board
```

before publishing SRC2's telemetry bytes into `execute_info + 0x168C`. So **one host frame is one
comm-board tick**, with no separate thread of its own (the `m3e_ctrl` worker of
[`pre3-netplay.md`](pre3-netplay.md) §3.8 is what runs all of this).

The state machine:

| state | what happens |
|---|---|
| 1 | `vtable[0]` — reset the registers; adopt **node id from `DAT_18018B05C` and node count from `DAT_18018B060`**; → 2 |
| 2 | `vtable[1]` — service the pending command; when it reports done → 3 |
| 3 | if node count < 2, or my id ≥ the count, → 5 (**stand-alone**). Otherwise `vtable[2]`, then **WAIT until every node's bit is set in the host rendezvous word**; only then `vtable[3]` and → 4 |
| 4 | **THE TRANSFER** (below); bump the sequence counter, flip the bank, stay in 4 |
| 5 | `vtable[4]` — park the registers in the "no link" state |

State 4, with `p = execute_info + 0x1660` (the three `p_work_ptr` slots
[`pre3.h`](../source/pre3/pre3.h) already declares and YAMP leaves null):

```c
size = comm->0x20024;                     // the guest's own packet size
me   = comm->0x20008;                     // my node id

// 1. publish MY packet: the visible bank -> the host's TX array, at my slot
memcpy((u8*)p[0] + size * me,  comm + 0x108 + bank*0x10000,  size);

// 2. ingest EVERY node's packet: the host's RX array -> the OTHER bank
for (i = 0; i < nodeCount; i++)
    memcpy(comm + 0x108 + (bank^1)*0x10000 + size*i,  (u8*)p[1] + size*srcIndex,  size);

// 3. stamp the sequence counter into the far bank and flip
comm->0x20026++;  *(u16*)(comm + (bank^1)*0x10000 + 0x0E) = comm->0x20026;  bank ^= 1;
```

**So `p_work_ptr[0]` is a TX array and `p_work_ptr[1]` is an RX array, both
`nodeCount * packetSize` bytes, indexed by node.** That is the whole wire format, and the module
chose it, not us.

### 2.4 `p_work_ptr[2]` is a shared 64-bit rendezvous word, and its groups are DIRECTIONAL

Four bit ranges, all read/written through `*(u64*)execute_info->p_work_ptr[2]`:

| bits | direction | meaning | who |
|---|---|---|---|
| `0 .. n-1` | host → module | node *i*'s packet is present in the RX array | `FUN_180035BA0` state 3 waits |
| `16 .. 16+n-1` | host → module | node *i* has finished booting | `FUN_1800393D0` case 0xF waits |
| `nodeId + 0x20` | module → host | this node answered the guest's `0xF000` command | `FUN_180035970` sets |
| `nodeId + 0x30` | module → host | this node has finished booting | `FUN_1800393D0` case 0xE sets |

**The 0x10 group is easy to miss and costs a whole run.** The module sets bit `nodeId + 0x30` in
case 0xE and then, one case later, waits on `word & rol64(walkingOne, 16)` — bits 16 upward, not
the 0x30 it just wrote. So the module never signals *itself* through this word: it REPORTS in the
high groups and WAITS on the host in the low ones, and even a single cabinet's own boot answer has
to pass through the host. A first implementation that set 0x30 instead of 0x10 produced a board
that ran 800 frames without a single draw — see §5a.

Both waits are spelled as an early `return` out of the frame, so a peer that never answers stalls
the board — which is the ROM's own waiting, exactly the property that makes this path not need
lockstep.

**Like a Dragon Gaiden clearly implements this as literal shared memory between board instances in
one process.** YAMP has to synthesize it: our own bits we set locally, the peer's bits arrive with
the peer's packet.

### 2.5 Node id and count come from the config, and `link_reserved` is misnamed

`DAT_18018B05C` and `DAT_18018B060` have **no writers in the module** — because they are not
separate globals. `module_start` copies the 0x1028-byte config to 0x18018A050, and

```
0x18018B05C - 0x18018A050 = 0x100C      pre3_config_t::link_reserved   -> NODE ID
0x18018B060 - 0x18018A050 = 0x1010      pre3_config_t::link_peer_count -> NODE COUNT
```

(The same arithmetic checks out for every named field: `+0x00` game at 0x18018A050, `+0x05`
`is_vs_mode` at 0x18018A055, `+0x04` free play at 0x18018A054 — all consistent with their observed
uses.)

**`link_reserved` is the master/slave switch.** Node 0 is the master; the module's own tests are
`nodeId == nodeCount - 1` (the last cabinet drives several link-interface notifications) and
`nodeId == 0` implicitly through the NVRAM initialiser's "takes the link path only when this is
zero", which is the note already in `pre3.h`. It should be renamed `link_node_id`.

---

## 3. The `params + 0x1070` link interface is NOT the data path

[`pre3-netplay.md`](pre3-netplay.md) §6 records sixteen vtable slots and says their semantics are
the first task for SRC2. Two corrections:

1. **The function attributed there as the main consumer, `FUN_1800341C0`, is a five-line
   initialiser.** The heavy user is `FUN_1800341F0`, and it is **not SRC2's** — it belongs to the
   two Daytona USA 2 tasks the ROM factory never constructs (its vtable slots appear in three
   tables, and SRC2's own per-frame task is `FUN_180037B70`).
2. **SRC2's per-frame path never touches the link interface at all.** For SRC2 the interface is
   read by the NVRAM initialiser (`FUN_180026840`, which forces VS mode on when it is present), the
   shared input update (`FUN_1800336D0`, where slot `+0x98` vetoes the coin/start latch), and the
   machine build/frame step (`FUN_1800393D0`, the boot barrier and run-state notifications).

So it is a **session/operator interface** — "is a link configured", "is the peer up", "tell the
cabinet the match is starting" — layered *over* a data path that runs entirely through the work
pointers. Since YAMP writes the implementation, its semantics are ours to define within whatever
the module's call sites demand, and **a first cut can pass null**, exactly as today.

The one behaviour that is lost by passing null is the forced VS mode in the NVRAM initialiser —
which YAMP can set directly through `config.is_vs_mode` instead.

---

## 4. What YAMP has to build

Nothing below exists yet.

### 4.1 The host side of the comm board (`source/pre3/CommBoard.{h,cpp}`)

* Own the three buffers and hand their addresses to the module in
  `execute_info.p_work_ptr[0..2]` — the field `pre3.h` currently documents as "nullable pointers
  the module reads and null-checks; YAMP leaves them null, which is the path the module already
  handles". Sizing needs `packetSize` (`comm + 0x20024`), which the **guest** writes, so the
  buffers must either be allocated at the maximum a 64 KB bank can hold per node, or grown on the
  first non-zero size the board publishes. Allocate for the worst case; it is 128 KB.
* Set `config.link_reserved` (→ `link_node_id`) and `config.link_peer_count = 2`.
* Per frame, **before** `entries.update`: publish the local TX slot to the wire and copy every
  received peer packet into its RX slot; set our own ready bit, and mirror the peer's.

### 4.2 The wire (`plugin/yampnet`, no ABI change needed)

`kPacketLink` (ABI 9, RLE-coded since ABI 10) already carries an opaque payload with redundancy and
an ordered receive queue. SRC2's payload is smaller than Virtual On's 0x700 and just as
zero-heavy, so the existing RLE and `kLinkRedundancy = 3` transfer unchanged.

**Take the Virtual On lesson with it:** `WaitChallenger`'s single-frame `0x24` state cost a day
(see `von-handoff.md` §"THE STAGE HANDSHAKE"). Any state SRC2 signals for one frame — a race start,
a course vote — has the same hazard, so the ordered queue and the one-take-per-board-frame rule are
requirements here too, not optimisations.

### 4.3 The role

Room-assigned, as Virtual On's is: `local_player` 0 = host = **node 0 = MASTER**, 1 = guest =
**node 1 = SLAVE**. Unlike Virtual On there is no soft reset to arrange — the node id is read at
`module_start`, so the role must be settled **before the board boots**, and changing it means
restarting the module.

### 4.4 The driver

`pre3::NetSession` currently runs the FV2 lockstep round. SRC2 needs the other thing: no barrier,
no seed, no canary, no clock pin, no savestate reset — just `poll()` every frame from the moment
the session exists, and the comm exchange. That is `K2Host::DriveNetSession`'s shape, and the trap
it exposed applies here verbatim: **the session must be pumped by whoever hosts the game, or the
room never forms.**

`pre3::NetplaySupported()` excludes SRC2 today and should keep doing so until this lands.

---

## 5a. WHAT THE FIRST PROBE MEASURED — §2 confirmed on one machine

`source/pre3/CommBoard.{h,cpp}` with `YAMP_PRE3_LINK=probe:2`: node 0 of a two-cabinet ring, the
three buffers attached, and the rendezvous bits held for an imaginary peer that never speaks.

```
f=149 state=3 node=0/2 size=848 seq=0   rendezvous=0001000100030003 tx=0/848
f=150 state=4 node=0/2 size=848 seq=0   rendezvous=0001000100030003 tx=0/848
f=270 state=4 node=0/2 size=848 seq=120 rendezvous=0001000100030003 tx=0/848
f=510 state=4 node=0/2 size=848 seq=360 rendezvous=0001000100030003 tx=34/848
f=870 state=4 node=0/2 size=848 seq=720 rendezvous=0001000100030003 tx=34/848
```

Every claim in §2 is in that trace:

* the state machine walks **1 → 2 → 3 → 4** and stays there;
* **`node=0/2`** — the board latched YAMP's `link_node_id` / `link_peer_count` out of the config,
  which is §2.5;
* **`size=848`** — the GUEST programmed the packet size through `0xC0020808`. This is the answer to
  "is the ROM actually driving the link, or ignoring it": it is driving it;
* **`seq` advances exactly one per emulated frame** (120 per 120), so the transfer is running at
  the rate §2.3 predicts;
* **`tx=34/848`** — 34 non-zero bytes in our own TX slot. The board is not merely ticking, it is
  **writing a packet**, which is the whole data path proven on one machine;
* `rendezvous=0001000100030003` decodes as bits 0,1 (ready, ours) + 16,17 (linked, ours) + **32**
  (`0x20+0`, the module answering the guest's `0xF000`) + **48** (`0x30+0`, the module reporting
  its own boot). Both module→host groups appeared on their own, exactly where §2.4 says.

### Three things the probe taught that reading could not

1. **The board's own `LINK ID` must not be SINGLE.** With the service-menu row at its default the
   guest never writes the comm board's enable register and the state machine sits at 0 forever,
   with `size` already programmed — a link that looks half-configured and never starts. Setting
   `[StF] Src2LinkId=1` (MASTER) through `pre3::ArcadeSettings` is what produced the trace above.
   **The board's LINK ID and the module's `link_node_id` are two different switches and both have
   to agree.**
2. **The enable register is `0xC0010180`**, bit 0 (`FUN_180035EA0`): writing 1 sets the state to 1
   and starts the machine, writing 0 resets the state, node id and bank to zero.
3. **The rendezvous groups are directional and the boot barrier reads 0x10, not 0x30** (§2.4).
   Setting the wrong group stalls the machine before its running phase, which presents as a board
   that runs hundreds of frames and never draws — with no other symptom.

### The master draws nothing against a SILENT slave — and that turned out to be correct

With the probe's imaginary peer, `draws` stays 0 and the module raises `execute_info.status` bit
`0x100`. The control isolates it — `LINK ID = MASTER` on its own renders normally at 392-405
draws — so it is the live link, and the reading was "a master whose slave never answers, because
the RX slot holds 848 zero bytes, which is not a cabinet".

**Two instances confirmed it.** See §5b.

## 5b. TWO CABINETS, ONE MACHINE — the link works

`Mode::Shared` (`YAMP_PRE3_LINK=shared:<nodeId>:2`) maps a named section holding the rendezvous
word and the packet array, and hands the module **the same pointer for TX and RX**. That is not a
model of the link, it *is* the link: Gaiden runs its cabinets in one process against shared memory,
so a board writes its own slot and reads every slot including its own, with no copy and no protocol
anywhere. It also settles the open question about whether a cabinet sees its own packet come back —
with one array it necessarily does.

Two YAMP processes, `build/bin/Win64/Debug` and `Debug2`, node 0 and node 1:

```
node 0  f=1088 state=4 node=0/2 size=848 seq=840 rendezvous=0003000300030003 tx=217/848
node 1  f=1007 state=4 node=1/2 size=848 seq=840 rendezvous=0003000300030003 tx= 41/848
```

* `rendezvous=0003000300030003` is **bits 0,1 + 16,17 + 32,33 + 48,49** — both nodes present in
  all four groups. The peer's bits are there because the word is genuinely shared, so this is the
  first time the module→host groups have been seen for a node that is not us.
* **Both cabinets render**: `status=0x40`, the attract/credit screen, draws climbing in step
  (229 → 232 → 235 on both). The blank master is gone the moment a real slave answers.
* The TX slots are **asymmetric and both live** — 217 non-zero bytes from the master against 41
  from the slave, sharing a common tail. The two boards are not merely running, they are saying
  different things to each other.

What this does NOT yet show: a race. Both cabinets sit at the credit screen because nothing has
coined them up, and there is no `-src2-autostart` equivalent of Virtual On's harness.

## 5c. THE PAIR ONLY CONVERGES SOMETIMES — and what the hung board is waiting for

**It is a start-up race, not a setting.** Three trials, changing only the delay between launching
the two instances:

| delay | node 0 | node 1 |
|---|---|---|
| 0 s | `tx=34` draws=0 | `tx=34` draws=0 |
| 1 s | `tx=34` draws=0 | `tx=34` draws=0 |
| **5 s** | **`tx=216` draws=333** | **`tx=40` draws=189** |

`tx=34` is the *stand-alone* signature — the same packet a solo probe produces. Both boards reach
comm state 4 with the sequence advancing and every rendezvous bit set, so the transport is not the
problem in either case.

### The hung board is spinning on a byte nothing writes

`YAMP_PRE3_PC` puts **both** cabinets at guest `0x04C9A0/A8`, `lr=0x04C960`, with an unchanging
register set from ~frame 480 onward. `YAMP_PRE3_DUMP` decodes that to a wait-for-flag:

```
04C990  lis r3,0x73 ; addi r3,r3,0x798E   ; r3 = 0x0073798E
04C998  lbz r0,0(r3)                      ; sample
04C9A0  cmpl r2,r0        <-- spin head
04C9A4  lbz  r2,0(r3)                     ; re-read
04C9A8  beq  0x04C9A0                     ; loop while UNCHANGED
04C9AC  li r0,0 ; stb r0,0(r3)            ; consume it
```

Three sibling entry points (`0x04C948`, `0x04C978`, `0x04C984`) call the same waiter for the flags
at `0x737987`, `0x73798E` and `0x7379A8`. It is the same shape as the spin hook 2 excises at
`0x018B30` ([`src2-hle-hooks.md`](src2-hle-hooks.md)).

`YAMP_PRE3_PEEK` says which flags move. Remembering that guest byte `A` is host byte `A ^ 3`:

| guest byte | over 750 frames |
|---|---|
| `0x73798C` | `0x67 → 0xFD → 0x93 → 0x29 → 0xBF` — **a counter, +150 per 150 frames** |
| **`0x73798E`** | **`0x00`, never once anything else** |

So the VBlank interrupt is alive — the frame counter next door proves it — and **the byte the board
is actually waiting on is never written by anybody.**

### The flag is the VBLANK counter, and only VBlank stops

Daytona USA 2's guest image loaded at matching addresses settles what the flag is. Parity checked
first, against two known-fixed values: `0x04C998` reads back the exact spin loop above, and
`0x0189EC` is `4806302D`, hook 1's instruction.

```c
FUN_00001D80:                      // xref: installed from 0x00001AA4
    ...call the registered handlers...
    DAT_0073798E++;                // <-- THE FLAG THE HUNG BOARD IS WAITING ON
    DAT_00700000++;
    do { *FUN_0000AEC4() = 0x2000000; } while (DAT_FE100018 & 2);   // ack, and 2 is VBLANK
```

So `0x73798E` is the **VBlank counter**, and its two siblings are the other two interrupts:
`FUN_00001E10` increments `0x737987` and acks bit `0x08`; `FUN_00001E78` increments `0x7379A8` and
acks bit `0x04` (the routine already in [`src2-hle-hooks.md`](src2-hle-hooks.md)).

And the ticking neighbour that first looked like a healthy VBlank is nothing of the sort: guest
`0x73798C` is written by `FUN_0004C9EC` — the function whose own wait-for-flag at `0x04CA40` is
where **hook 0**, the idle-loop cut-out, sits. It moves because the host yields there.

**Interrupts are still being delivered to the hung board.** Reading the low byte of the same peek
(guest `0x737987` is host `0x737984`):

| guest byte | ISR | on the HUNG board |
|---|---|---|
| `0x737987` | `FUN_00001E10`, IRQ bit `0x08` | `0x66 → 0xFC → 0x92 → 0x28 → 0xBE` — **+150 per 150 frames** |
| `0x73798E` | `FUN_00001D80`, IRQ bit `0x02` (VBlank) | **`0x00`, never** |

A healthy stand-alone board has `0x73798E = 0x44`, so VBlank does fire there. So this is not a board
with interrupts masked and not a board that has stopped: **it is taking one interrupt and not the
other, and it is blocked on the one it is not taking.**

### CORRECTION: pre3 DOES raise VBlank, and the first scan for it was wrong

The paragraph that used to stand here said pre3 never raises IRQ `0x02` and proposed injecting it.
**That was wrong, and the way it was wrong is worth keeping.** The scan behind it looked only at
calls through the memory object's vtable slot `0x98` — and `0x98` is the CLEAR:

```
CM3Mem vtable 0x18010BC50:  +0x90 = FUN_180012430   state |=  mask    RAISE
                            +0x98 = FUN_180012460   state &= ~mask    CLEAR
```

The frame step uses both, and slot `0x90` is where the interrupts actually come from
(`FUN_18003B0A0`):

```c
mem->vtable[0x90](mem, 2);      // VBLANK, unconditional, once per emulated frame
  ... if (rom->0x482 & 0x20) loop up to 0x80 times:
mem->vtable[0x90](mem, 0x40);   //   scanline: raise
mem->vtable[0x98](mem, 0x40);   //            ...and clear
mem->vtable[0x90](mem, 9);      // and 4 at the end of the frame
```

So VBlank is raised every frame, and the "injection" built on the wrong conclusion was calling
`0x98` — **clearing VBlank once per frame**, the exact opposite of its intent. It has been removed.
The trap is easy to fall into a second time: the SCSI window's `vtable[0x98](0x100)` reads naturally
as "raise the SCSI interrupt" and is in fact an acknowledge-on-read.

### And VBlank is unmasked on the hung board too

The raise only asserts the CPU line when the guest has enabled the bit:

```c
state |= mask;
if ((state & (ENABLE | 0x100)) != 0 && cpu != NULL) cpu->assert_line(1);   // ENABLE = mem+0x34
```

`mem+0x34` is what the guest programs through `0xF0100014`. Measured on a hung pair, both nodes:

```
irq=6E/0005      enable 0x6E (VBlank bit 0x02 IS set), state word 0x0005
```

which also matches the guest's own soft copy at `0x737984`. So the board is **not** missing VBlank
because it is masked, and **not** because the module fails to raise it.

**What is left is narrow and specific:** VBlank is raised, VBlank is enabled, and the ISR that would
increment `0x73798E` still does not run on a linked board. The next thing to check is therefore the
delivery path rather than the source — whether `cpu->assert_line(1)` is reached (the `mem+0x358`
branch above), and whether the guest is spinning with `MSR[EE]` clear, which would make this a wait
for an interrupt the board itself has disabled.

### Where that leaves the comm board's own interrupt

`CXComm` raises nothing: its seven methods and `FUN_180035BA0` only touch its own registers, and the
memory object's IRQ raise/clear — `FUN_180012430` (`or word ptr [mem+0x36]`) and `FUN_180012460`
(`and`), which the SCSI window reaches through vtable `+0x98` with mask `0x100` — is never reached
from the comm path. That remains true and remains a defect, but it is no longer the leading
explanation of THIS hang, because the board is not waiting on a comm interrupt. It is waiting on
VBlank.

**The next question is precise: what gates pre3's VBlank assertion, and why does a linked board stop
getting it?** The callers of `FUN_180012430` and their masks are where to look, and whether bit
`0x02` is conditional on something the linked board no longer does.

### The comm board never interrupts the guest either

Real Model 3 network hardware raises an IRQ when a transfer completes. `CXComm` has no such call:
its seven methods and `FUN_180035BA0` only touch its own registers, and the memory object's
IRQ-raise (`CM3Mem` vtable `+0x98`, which the SCSI window at `0xC1` does use, with mask `0x100`) is
never reached from the comm path.

That also explains the race rather than contradicting it: a pair that converges is one where the
timing led the ROM down a path that does not enter this wait, not one where the flag got set. Which
is testable — the same PC probe on a converged pair should never show `0x04C9A0`.

**The next step is to establish the network board's IRQ mask and raise it from the transfer**, and
the mask is the part that needs reversing rather than guessing: `0x100` belongs to the SCSI, and
MAME's `model3.cpp` IRQ bits are the reference to check it against.

Playing a linked race is downstream of that.

## 5. What is NOT established, in the order it should be settled

1. ~~**Nobody has run SRC2's comm board.**~~ **Done — see §5a**, and §2 is confirmed end to end.
2. ~~**Does the guest ever program a packet size?**~~ **Yes: 848 bytes**, and it fills 34 of them.
3. **What does the game DO with a second cabinet?** Both reach the credit screen together (§5b),
   which is as far as an uncoined pair can get. Whether two nodes mean one race or two independent
   seats is decided by SRC2's own menus and needs a race played; the ROM has not been read for it.
4. **The HLE table.** Three of SRC2's 26 hooks are boot-critical and two of them excise a security
   overlay ([`src2-hle-hooks.md`](src2-hle-hooks.md)). None of the 26 looks link-related, and §5a
   removes the strongest reason to suspect them: the guest reaches the comm board, programs it and
   feeds it a packet with the shipped mask in place. So a hook bisect is no longer the first thing
   to try if two cabinets fail to agree — the RX side is.
5. ~~**Is mirroring our own packet into our own RX slot correct?**~~ **Yes** — `Mode::Shared`
   gives the module one array for both directions, both cabinets run on it, so a board does see
   its own slot. `Mode::Link` mirrors deliberately to match.
6. **The ready-bit policy is YAMP's invention.** Nothing in the module ever CLEARS a ready bit, so
   holding them all set free-runs the board and clearing them per frame makes it lockstep. The
   staleness window in `CommBoard.cpp` is a middle that has never been tested against a peer.
7. **Analogue inputs.** SRC2 reads the ADC ring that FV2 never touches. On this path they never
   cross the wire — each cabinet drives its own — so this is a local input feature, not a netplay
   one, but it has to exist before a race is playable at all.
8. **The 16 link-interface slots.** Deliberately last. Nothing in the data path needs them, and
   defining them before the board has been seen to link would be guessing at an interface whose
   only consumer is code we cannot run yet.

## 6. Files this touches

| file | change |
|---|---|
| `source/pre3/pre3.h` | rename `link_reserved` → `link_node_id`; document `p_work_ptr[0..2]` as TX/RX/rendezvous |
| `source/pre3/CommBoard.{h,cpp}` | **new** — the buffers, the per-frame exchange, the rendezvous word |
| `source/pre3/Gaiden/Pre3Host.cpp` | fill the work pointers and the two config fields; pump the session |
| `source/pre3/NetSession.{h,cpp}` | a non-lockstep path, or a sibling driver, for linked cabinets |
| `plugin/yampnet/` | nothing — `kPacketLink` carries this as-is |
