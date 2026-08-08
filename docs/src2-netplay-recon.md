# Sega Racing Classic 2 netplay — the linked-cabinet path

Recon 2026-08-07 against `pre3-pxd-w64-d3d12_retail.dll` (the copy in
`build/bin/Win64/Debug/pre3/`), read in Ghidra. Nothing here has been run yet: every claim below is
static analysis, and the section that says which parts are *inference* says so explicitly.

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

### 2.4 `p_work_ptr[2]` is a shared 64-bit rendezvous word

Three disjoint bit ranges, all read/written through `*(u64*)execute_info->p_work_ptr[2]`:

| bits | meaning | set by | waited on by |
|---|---|---|---|
| `0 .. n-1` | node *i* has a packet ready for this frame | (the host) | `FUN_180035BA0` state 3 |
| `nodeId + 0x20` | node *i* answered the guest's `0xF000` command | `FUN_180035970` | the host |
| `nodeId + 0x30` | node *i* has finished booting | `FUN_1800393D0` case 0xE | `FUN_1800393D0` case 0xF |

The boot barrier is the same shape as the frame barrier: the machine build **counts set bits and
refuses to advance until the count equals the node count**. Both are spelled as an early `return`
out of the frame, so a peer that never answers stalls the board — which is the ROM's own waiting,
exactly the property that makes this path not need lockstep.

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

## 5. What is NOT established, in the order it should be settled

1. **Nobody has run SRC2's comm board.** Every fact above is static. The first experiment is a
   single machine with `link_peer_count = 2`, `link_node_id = 0`, the three buffers allocated and a
   log of `comm + 0x20028` (the state), `+0x20024` (the packet size) and `+0x20026` (the sequence)
   per frame. If the state reaches 3 and parks there waiting for node 1's bit, everything in §2 is
   confirmed at once.
2. **Does the guest ever program a packet size?** The board can sit in state 3 forever and look
   healthy. `+0x20024` reading non-zero is the proof that SRC2's ROM is actually driving the link.
3. **What does the game DO with a second cabinet?** SRC2's own menus decide whether two nodes mean
   a two-player race or two independent seats. Unknown; the ROM has not been read for this.
4. **The HLE table.** Three of SRC2's 26 hooks are boot-critical and two of them excise a security
   overlay ([`src2-hle-hooks.md`](src2-hle-hooks.md)). None of the 26 looks link-related, but that
   was read for determinism, not for the comm board — worth a second pass against the guest
   addresses the link code lives at, once §5.1 says where that is.
5. **Analogue inputs.** SRC2 reads the ADC ring that FV2 never touches. On this path they never
   cross the wire — each cabinet drives its own — so this is a local input feature, not a netplay
   one, but it has to exist before a race is playable at all.
6. **The 16 link-interface slots.** Deliberately last. Nothing in the data path needs them, and
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
