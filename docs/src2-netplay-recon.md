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

### The frame step's whole interrupt schedule

The memory object's four IRQ slots, all now read out of the module:

| slot | body | meaning |
|---|---|---|
| `+0x90` | `state \|= mask` | **raise** |
| `+0x98` | `state &= ~mask` | **clear** |
| `+0xA0` | `return (enable & mask) != 0` | is this interrupt enabled |
| `+0xA8` | `mem+0x33 ^= 1` | field-parity flip |

and one emulated frame (`FUN_18003B0A0`) is:

```
parity flip -> run CPU -> RAISE 2 (VBlank) -> run CPU
            -> if (rom+0x482 & 0x20) up to 0x80x { raise 0x40; run 200; clear 0x40; run }
            -> RAISE 9  (= 0x01|0x08) -> run CPU -> RAISE 4
```

**Nothing in the module ever clears 1, 2, 4 or 8** — only the guest's own ISR acknowledgements do.
That fits the measured `state=0x05` (bits `0x01` and `0x04` still pending, their handlers not
acking) and it fits `0x737987` ticking (bit `0x08` raised in the `9` and acked). It does **not** fit
`0x73798E` sitting at zero while bit `0x02` is absent from the state: that combination needs bit 2
to be both cleared and never handled, and only the guest can clear it.

### The probes were reading the WRONG GUEST RAM - the same mistake, twice

A coherent probe was added (`YAMP_PRE3_SYNCPROBE`, sampled after `WaitForEmulatedFrame`) and its
first run reported all three ISR counters at zero — including the one a racing peek had clearly
shown ticking. The probe was wrong, not the board: it took guest RAM from the canary's bank
descriptor instead of the base the module's own handlers use (`DAT_18062AA98`).

**This is verbatim the failure already written up in [`src2-hle-hooks.md`](src2-hle-hooks.md)**,
where `CM3Mem+0x18` produced "a whole run of confident, wrong readings", and the rule recorded there
— *validate a memory probe against a known-fixed value before believing anything it says* — was not
followed. `BoardGuestRam()` now refuses any base where guest `0x0189EC` does not read `0x4806302D`.

With that fixed, on a HEALTHY stand-alone board all three counters advance one per frame:

```
f=543 irq=6E/000D vblank(73798E)=30 irq08(737987)=88 irq04(7379A8)=89
f=544 irq=6E/000D vblank(73798E)=31 irq08(737987)=89 irq04(7379A8)=8A
```

**So the entire "VBlank never reaches the guest" line of investigation was an artifact of a bad RAM
base.** VBlank is raised, enabled, delivered and serviced. That question is closed.

### The convergence is a RACE and nothing yet explains it

What is left is the original symptom with no explanation attached. Convergence rate, all at
`YAMP_PRE3_LINK=shared`, counting a run as converged when both cabinets draw and the TX slots go
asymmetric (216/40 rather than 34/34):

| condition | converged |
|---|---|
| default hook mask, 0 s launch delay | 0 / 6 |
| default hook mask, 5 s launch delay | 1 / 1 |
| default hook mask + the sync probe running | 1 / 1 |
| HLE hooks 16-21 disabled, 0 s | **1 / 4** |
| hooks 16,17 only / hooks 18-21 only, 0 s | 0 / 1 each |

**No condition is established as causal.** Anything that perturbs timing — a launch delay, an extra
per-frame probe, a different hook mask — converges *sometimes*. A single success was briefly taken
as confirmation that the `0x091xxx` hooks block a "waiting for link" screen; three repeats at the
same mask gave 1/3, which is the same rate as everything else and therefore evidence of nothing.

The hooks remain a REASONABLE suspect on the code, and that much is worth keeping. The region they
patch is a timed on-screen display gated on a counter at `r15+6` against `0x55`/`0x11D`:

```
0917E0..0918DC   hook 17 forces `blt 0x91840` taken, skipping the whole draw loop over the
                 table at 0xE34DC (lfs/fmuls, then bl 0xF574)
09185C/0918A4/0918D0   hooks 18/19/20 delete three `bl 0xF574` calls (ids 0x6D6, a table
                 lookup, 0x6D7)
091660           hook 16 skips `bl 0xA7CC` / `bl 0xBF3C` once the counter passes 0x55
```

That is consistent with a countdown screen whose rendering has been excised — but "consistent with"
is not a measurement, and the run counts above are what a measurement would have to beat.

### The ROM's startup network screen exists, runs, and is NOT hooked out

Asked directly of the guest image, because "the hooks removed the waiting-for-link screen" was the
standing suspicion and it is checkable rather than arguable.

The ROM carries the text:

| guest | string |
|---|---|
| `0xE38AC` | `        NETWORK CHECKING        ` |
| `0xE38E4` | ` NO CARRIER! CHECK NETWORK CABLE` |
| `0xE3908` | `   NETWORK BOARD NOT PRESENT    ` |
| `0xE392C` | ` NETWORK BOARD HAS ANY PROBLEM  ` |
| `0xE3298` | `WARNING` / `TROUBLE OCCURRED` |

and the boot init chain calls the check **unconditionally**, four instructions after the site hook 1
deletes:

```
0189EC  bl 0x7BA18               <- HOOK 1 deletes this (the security-overlay loader)
...
018A18  lbz r3, 0x19E(0x100000)  ; guest 0x10019E
018A1C  bl 0x93DB4               ; FUN_00093DB4 -> FUN_0009212C, "NETWORK CHECKING"
018A20  cmpwi r3, 0 / bgt 0x18AC4
```

**None of SRC2's 26 hook addresses fall inside `FUN_00093DB4`, `FUN_0009212C` or `FUN_0009065C`.**
So the screen is present and the check runs.

**And its argument is the LINK ID byte.** Guest `0x10019E` is the settings working copy at
`0x100180` plus `0x1E`, which is the LINK ID row (see `source/pre3/ArcadeSettings.h`, read off the
board's own service-menu table). That is an independent confirmation of the measured requirement
that LINK ID must not be SINGLE: the ROM feeds that exact byte to its network check.

What hooks 16-20 gut is therefore a DIFFERENT routine that merely lives nearby - a timed display
over 0x34-byte descriptors at `0xE3400`/`0xE3434`/`0xE3468` with a message-id table at `0xE349C`.
Identifying it is still open, and it is no longer the leading explanation of anything.

**What the next experiment has to be: many trials per condition, not one.** Everything above is 1-6
runs deep, which cannot separate a 25% race from a fix. A scripted N=20 sweep across {hook mask} x
{launch delay} is the only thing that will, and it should come before any further code change.

### THE MEASUREMENTS WERE RACED, and the probe that fixes it is now in place

Every sample above — the peeks, the PC probe, `irq=ENABLE/STATE` — is taken **from the host thread
while `m3e_ctrl` is inside its own frame**. That is precisely the data race
[`pre3-netplay.md`](pre3-netplay.md) §3.8 documents, and it is why the frame-4 desync cost a round:
"reading board state immediately after the update stage reads it while the worker is running the
next frame."

So the contradiction above may be an artifact of sampling an interrupt that is raised and acked
*within* one emulated frame. Before drawing any further conclusion the probes should move inside the
update stage's own window, which `Determinism::BoardFrameMarker` / `WaitForEmulatedFrame` already
exist to provide. **That is the next step, and it comes before any more theorising about VBlank:**
a coherent sample of `(IRQ state, 0x73798E, guest PC)` taken at a known emulated frame boundary
would settle in one run what three rounds of racing snapshots have not.

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


---

## 6. WHY NO NETWORK STATUS IS EVER SHOWN

Chased to the end 2026-08-07, because "we never see the link screen" had been an unexamined
assumption behind several wrong turns.

### The text path is a SYSCALL, and it works

`FUN_0009212C` draws `NETWORK CHECKING` through `FUN_0000A7CC` (locate) and `FUN_0000A738`
(print). Both funnel into `FUN_0000624C`, which is a single PowerPC **`sc`** - a BIOS service call,
commands `0x20` = locate and `0x22` = print - and the guest's exception vectors are populated:

```
0x500  b 0x19B0     external interrupt
0x900  b 0x1BB0     decrementer
0xC00  b 0x2FD0     SYSCALL
```

`0x2FD0` is a real handler: saves SRR0/SRR1 and SPRG0-3, dispatches on `r3 <= 0x2FF`, escapes to
`0xFFF00C00` above that. **And it runs** - guest `0x70A004`, where it stores the interrupted stack
pointer, reads back `0x0073EF80`. The PC probe independently shows the board vectoring
(`next=0x900`) and executing boot ROM at `0xFFF03338`. So traps work and the text is being emitted.

### The loss is the TILEMAP WINDOW: pre3 implements it for 32-BIT ACCESS ONLY

`CM3Mem` window entry 13, top byte `0xF1` - Model 3 tilemap RAM plus the video registers:

| access | handler | behaviour |
|---|---|---|
| read8 | `0x1800127C0` | `mov al, 0xFF; ret` — stub |
| **write8** | `_guard_check_icall` | **`ret` — DISCARDED** |
| read16 | `0x1800127D0` | `mov eax, 0xFFFF; ret` — stub |
| **write16** | `_guard_check_icall` | **`ret` — DISCARDED** |
| read32 | `0x180013A50` | real; forwards to the device at `mem+0x370` below `0xF1100000` |
| write32 | `0x180013B00` | real; same, with `bswap` |

The tilemap device itself is present and installed (`CM3Mem::init`'s seventh argument lands at
`mem+0x370`). Only the narrow accessors are missing. **So any text the board composes with byte or
halfword stores is silently thrown away** - and a halfword is the natural width for a tile entry.

That is sufficient to explain no network status, no `WARNING` / `TROUBLE OCCURRED`, and any other
text this board draws, and it is a MODULE-LEVEL GAP rather than anything to do with the link. It
also explains why every diagnosis so far had to be done by reading guest RAM: the screen was never
going to say anything.

### MEASURED, AND IT KILLS THE THEORY ABOVE

`source/pre3/Patch.cpp` implements the four missing accessors and COUNTS them, because "the ROM
uses narrow stores for text" was an inference. The counter is the acceptance test, and it failed:

```
SRC2:  narrow 0 reads / 0 writes
FV2:   narrow 0 reads / 0 writes
```

Neither game touches this window narrowly. **The narrow accessors fix nothing** - they are correct,
harmless and inert, and are kept only because discarding a write is not behaviour worth preserving
and because the counters are the only visibility into this window.

Counting the WIDE path is what found the answer:

```
SRC2:  wide 6083 reads / TILEMAP 232100 writes (first=F10F7000 last=F10FDBF8) / registers 6284
```

**The board writes tilemap RAM 232,100 times in 700 frames**, into `0xF10F7000-0xF10FDBF8` - the
name-table region. So the text IS composed, it DOES reach tilemap RAM, and the 32-bit path that
carries it works.

(The first attempt at this counter recorded only a first/last pair for the whole window and got
`F1180008`/`F118000C`, both video registers, which said nothing about tilemap RAM in between. The
ranges are counted separately now.)

### So the real gap: THE TILEMAP LAYER IS NEVER COMPOSITED

Text reaches the tilemap and nothing draws it. The ~235 draws a frame are Real3D polygons; the 2D
layer that carries `NETWORK CHECKING`, `WARNING`, the service menu and every other string on this
board is written and then never presented.

That is a RENDERING gap, not a memory-map one, and it is not specific to the link - it explains why
every diagnosis in this document had to be done by reading guest RAM. It is also the highest-value
thing left, because a board that can print its own status is worth more to the outstanding
convergence race than any further host-side probe.

Where to start: `module_render_begin` is documented as uploading the 0x120000 scroll buffer
(`Pre3Host.cpp`), so the module has a path for this layer. Whether it draws it, and what the host
owes it, is unread.

### What is NOT yet proven

~~That the ROM's text service uses narrow stores.~~ **Disproven above** - it uses word stores, and
the accessors this section proposed turned out to be inert. The inference was "16-bit is the natural
width for a tile entry", which is exactly the kind of reasoning this document has now had to retract
three times; the counter is the only reason it took one run rather than a day.

## 7. IT PLAYS LINKED OVER RPCN, ON TWO MACHINES (2026-08-08)

Two PCs, a real RPCN room, and the ROM's own network check agreeing on a two-node ring on both
cabinets. This is the milestone §4 was written for.

```
host  192.168.50.250   YAMP.exe -src2 -net-host          -> room 105
guest 192.168.50.209   YAMP.exe -src2 -net-join 105      (PsExec -i, staged on G:\YAMP\Debug)

MASTER  the boot network check ran as MASTER CONTROLLER (LINK ID = 1), settled on
        id=1 nodes=2, net=0xE8 -> agreed on a ring
SLAVE   the boot network check ran as SLAVE (LINK ID = 2), settled on
        id=2 nodes=2, net=0xE8 -> agreed on a ring
```

Both boards then held `state=4` for ~3500 frames with `wire=up`, and - the part that says the link
is real rather than merely up - **both machines' RX arrays carried the same contents**: node 0's
slot 227 non-zero bytes, node 1's 45-47, with matching per-frame churn on each side. The plugin's
own accounting reports `856 raw` per datagram, which is the 8-byte wire header plus the 848 bytes
the guest programmes, RLE-coded to 55-199 bytes on the wire.

### 7a. The role comes from the ROOM, and the board is REBOOTED into it

The blocker was never the wire. `CommBoard::Configure()` runs once, immediately before
`module_start`, because that is where the module latches the node id and peer count out of the
config block and never re-reads either. An RPCN room does not exist at that instant and cannot be
made to - connect, TLS, login, discovery and create/join are seconds of round trips - so "ask the
room what this cabinet is" answered `no room` on every launch and the cabinet silently came up
STAND-ALONE. Every time.

Guessing the role earlier from the launch flags is the wrong fix: a room is what actually decides
it, and a player who joins from the lobby never touched a launch flag. `CommBoard::DriveRoomRole()`
changes it AFTERWARDS instead, and every mechanism it uses is the module's own:

1. write the config globals - `+0x100C` node id, `+0x1010` peer count. The comm board re-latches
   both when its state machine passes back through state 1, which the guest does for itself during
   boot by writing 0 to `0xC0010180`;
2. `ArcadeSettings::Reset()`, re-arming the LINK ID row - the restore is about to wipe the guest RAM
   both copies of it live in, and without this the rebooted board reads back the SINGLE it powered
   on with and its check returns before printing anything;
3. `RestoreResetSnapshot()` - Determinism's power-on snapshot, captured before a single guest frame
   ran, so restoring it makes the guest re-run its entire boot chain including `FUN_00093DB4`.

Virtual On does the equivalent by pulsing the TEST switch, because its ROM re-enters `Net_check`
when the operator menu is left. SRC2's check is called once from the boot chain and nothing
re-enters it, so the equivalent here is bigger and, usefully, more literal: put the board back to
power-on and let it boot again. Measured end to end:

```
[SRC2 linkgate] LINK ID work=0 nvram=0 src=0 | ROM latched mode=0 (SINGLE)     <- booted stand-alone
[SRC2 link] config +0x100C/+0x1010: 0/0 -> 0/2
[SRC2 link] room says this cabinet is MASTER (node 0) (was STAND-ALONE)
[SRC2 link] board restored after 1 frame(s); it is now booting as MASTER (node 0)
[SRC2 linkgate] LINK ID work=1 nvram=1 src=1 | ROM latched mode=1 (MASTER CONTROLLER)
[SRC2 link] state=1 -> 2 -> 3 -> 4, node=0/2, size=848
```

`s_appliedRole` is deliberately a separate variable from `s_nodeId`: the first is what the guest
last BOOTED as, the second what the host currently wants it to be. Collapsing them would make the
role change believe its work was done the instant it asked for it - the same mistake Virtual On's
`ApplyCabinetRole` documents from the other direction.

**A cabinet with no room is untouched.** A plain `-src2` launch emits no `[SRC2 link]` line at all,
keeps `Src2LinkId` at whatever the operator set, and its check runs SINGLE - verified after the
fact, because the `settings.ini`-fakes-a-regression trap has now cost this project three sessions.

### 7b. What the two-machine run corrected in the wire, and what shared memory had hidden

Three defects in the `Mode::Link` path could not exist under `Mode::Shared`, because shared memory
has one array, no framing and no delivery to fail:

* **The plugin de-dupes byte-identical link payloads** (`Plugin.cpp`'s `LinkPush`), which is right
  for Virtual On - it rate-limits itself to one datagram per board frame - and wrong here, where the
  host transmits every host frame and needs every transmission to count as a sign of life. A cabinet
  whose packet has not changed is not a cabinet that has gone away, but with the de-dupe in the way
  it looks like one: nothing arrives, `STALE_FRAMES` expires, the peer's ready bit is cleared, and
  the board waits forever for a peer that is transmitting perfectly. Fixed with a `seq` byte that
  moves every frame - which leaves the three REDUNDANCY copies of one datagram byte-identical, so
  they still collapse to one.
* **The RX slot was indexed by the SENDER's packet size**, where the module reads that array as
  `rx + ourPacketSize * i`. The two agree in every healthy session, so it worked and would have gone
  on working right up until they disagreed, at which point the peer's data lands at an offset the
  board never reads.
* **`STALE_FRAMES` was 6**, which under shared memory could never fire. Over a wire the two cabinets
  do not run at the same rate - the guest here sent ~2.4 packets per one of the host's - so a window
  measured in OUR frames has an unknown length in THEIRS. Now 30.

Liveness is also no longer tied to the peer having programmed a packet size: any valid datagram
refreshes the staleness clock, because "is the peer there" and "has the peer's guest programmed a
size yet" are unrelated facts.

### 7c. A direct-UDP transport was built, proved the wire, and was then removed

Before RPCN was wired, the same `Mode::Link` path was exercised over a plain address pair (a
`link_direct` ABI entry driving `Transport.h`'s `UdpTransport`). Two instances on one machine over
loopback reached `state=4` for 2300+ frames, exchanged 848-byte packets, and both passed the ROM's
network check - `net=E8`, master id 1 / count 2, slave id 2 / count 2. That is what caught all three
defects in §7b and what made the RPCN run work first try.

It is **gone**, deliberately, and the reasoning is Virtual On's: a second transport that nothing
tests is a second set of behaviours to keep true. It had one job - separating "does the game play
linked over a real wire" from "does the RPCN client bring a peer pair up" - and it did it. RPCN is
what this ships on.

(The cross-machine attempt over that transport never linked: both sides sent thousands of datagrams
and neither received any, and a bare UDP listener confirmed zero arrivals on the far end even with
an inbound allow rule in place. RPCN's P2P on port 3658 then worked between the same two machines in
both directions on the first try, so the block was specific to those ports rather than general -
unexplained, and moot.)

### 7d. Still open

* **Nobody has driven a race.** Both cabinets reach and hold the linked running state; what the ROM
  does with two players actually driving is unexercised, and the CPU-car handoff (§5c) in particular.
* **Latency.** Both machines are on one LAN. The ROM's protocol expects a partner on a serial ring.
* **The lobby path.** `DriveRoomRole` handles a room formed at any time, so the F1 -> Netplay page
  should now work as well as the command line does - untested.
* **More than two cabinets.** The plugin's link channel is point-to-point, so the arrays and
  rendezvous groups are sized for `MAX_NODES` because the MODULE's are, not because this transport
  can fill them.
