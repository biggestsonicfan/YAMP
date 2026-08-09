#pragma once

#include <cstddef>
#include <cstdint>

namespace pre3
{
	// The HOST HALF of the Model 3 network board — the linked-cabinet path for Sega Racing
	// Classic 2. Full derivation in docs/src2-netplay-recon.md; the short version is here because
	// the shape of this file is dictated by it.
	//
	// THE MODULE ALREADY EMULATES THE BOARD, which is the whole reason this file is small. That is
	// the opposite of Virtual On, where the module's comm-board firmware was a stub and
	// K2Host had to drive one (`DriveCommFirmware`). `M3ERomSrc2` EMBEDS a `CXComm` subobject at
	// rom+0x588 with two 64 KB banks — the reason SRC2 and Spikeout allocate 0x20610 bytes where
	// every other game in this module allocates 0x5E0 — and the guest's 0xC0xxxxxx window reaches
	// it live:
	//
	//     guest 0xC0xxxxxx -> CM3Mem window entry 9 (the table at DLL 0x1801077C0)
	//                      -> the device pointer at mem+0x360, which CM3Mem::init
	//                         (DLL 0x180014180) fills with the ROM OBJECT
	//                      -> M3ERomSrc2 vtable slots 10-13 (DLL 0x180037890/78E0/7930/7940)
	//                      -> rom+0x588, the CXComm banks, byte-swapped and addressed ^ 2
	//
	// Worth stating because the SAME GAME contains the opposite arrangement: its security board
	// (slots 2/3) is a stub that returns 0 and discards writes, and that stub is why two of SRC2's
	// HLE hooks exist at all (see SecurityBoard.h and docs/src2-hle-hooks.md). The comm board is
	// not a stub. It is the real thing, waiting for a host.
	//
	// SO THE WIRE FORMAT IS THE MODULE'S, NOT OURS. `FUN_180035BA0` runs once per emulated frame
	// from SRC2's own per-frame task (DLL 0x180037B70) and moves bytes through three pointers the
	// host puts in `execute_info.p_work_ptr[0..2]` — the three nullable slots pre3.h describes and
	// that YAMP has always left null. Those three nulls, and nothing else, are why the board has
	// never linked:
	//
	//   p_work_ptr[0]  TX ARRAY. The board copies its outgoing packet INTO it, at slot [nodeId].
	//   p_work_ptr[1]  RX ARRAY. The board copies every node's packet OUT of it, indexed by node
	//                  - INCLUDING THIS NODE'S OWN. The state-4 ingest walks the sources as
	//                  (me - 1 - i) mod count for i = 0..count-1, so p1[me] is read every frame
	//                  and lands in the LAST of the count+1 window slots the guest programs
	//                  (FUN_00012da4: register 0xC0020804 = size * (count + 1)) - the ring
	//                  handing a cabinet its own packet back after a lap, which the master's
	//                  comm service uses as its ring-lap timing reference. The host must keep
	//                  that slot fed; CommBoard::Update mirrors it.
	//   p_work_ptr[2]  points at a u64 RENDEZVOUS WORD, shared between all cabinets.
	//
	// Both arrays are `nodeCount * packetSize`, and packetSize is a register the GUEST programs
	// (guest 0xC0020808 -> comm+0x20024). The host never chooses it, which is why the buffers here
	// are sized for the largest a bank can hold rather than for an expected value: the module
	// memcpys into them with a length YAMP does not control, so anything smaller is a buffer
	// overflow waiting for a ROM to ask for it.
	//
	// THE RENDEZVOUS WORD IS FOUR FIELDS IN ONE, all read and written as `*(u64*)p_work_ptr[2]`,
	// and THE GROUPS ARE DIRECTIONAL - the low two are the host's to set and the high two are the
	// module's to report. Getting that backwards is not a subtle failure: it stalls the board
	// before it draws anything, which is what the first probe run did.
	//
	//   HOST -> MODULE
	//   bits 0 .. n-1      node i's packet is present in the RX array. FUN_180035BA0 state 3 counts
	//                      these and will not transfer until every node is set.
	//   bits 16 .. 16+n-1  node i has finished booting. The machine's boot barrier
	//                      (FUN_1800393D0 case 0xF) counts these and will not reach its running
	//                      phase until every node is set. Its test is written as
	//                      `word & rol64(walkingOne, 16)`, which is why it reads as bit 16 and not
	//                      as the 0x30 the module writes one case earlier.
	//
	//   MODULE -> HOST
	//   bit nodeId+0x20    this node answered the guest's 0xF000 "ready" command (FUN_180035970).
	//   bit nodeId+0x30    this node has finished booting (FUN_1800393D0 case 0xE).
	//
	// So the module never signals itself through this word: it REPORTS in the high groups and WAITS
	// on the host in the low ones. A host that merely echoed 0x30 into 0x10 would work for one
	// cabinet; the point of the indirection is that the host is what knows about the others.
	//
	// Both waits are spelled as an early return out of the frame, so a peer that never answers
	// stalls the board. That is the ROM doing its own synchronising, and it is exactly why this
	// path needs no lockstep, no barrier, no match seed and no determinism contract — the same
	// property that made Virtual On's link work after four failed lockstep attempts.
	//
	// LIKE A DRAGON GAIDEN CLEARLY IMPLEMENTS THIS AS LITERAL SHARED MEMORY between board
	// instances in one process. Over a network there is no shared memory, so this file synthesizes
	// it: our own bits are set locally, the peer's arrive with the peer's packet.
	namespace CommBoard
	{
		// How many cabinets the ring may hold. Two is what a SRC2 twin is; four is the most the
		// module's own per-node loops and execute_info's four pad slots can describe.
		inline constexpr uint8_t MAX_NODES = 4;

		// The largest packet a bank can carry: 0x10000 minus the 0x108 the transfer skips at the
		// front. Rounded UP to the bank size here, deliberately — see the note above about who
		// chooses the length.
		inline constexpr size_t MAX_PACKET = 0x10000;

		enum class Mode : uint8_t
		{
			// No link. config.link_peer_count stays 0 and the board runs as a single cabinet,
			// which is every game and every session that has not asked for otherwise.
			Off = 0,
			// A link with NO PEER: the rendezvous bits are held set so the board's state machine
			// runs its transfer every frame against an RX array that stays zero.
			//
			// This is the FIRST EXPERIMENT of docs/src2-netplay-recon.md §5, and it is worth its
			// own mode rather than being a degenerate case of Link. Everything the recon claims
			// about the comm board is observable from one machine with it: the state machine
			// reaching 4, a non-zero packet size (which is the proof the ROM is actually driving
			// the link rather than ignoring it), and the sequence counter advancing.
			Probe = 1,
			// TWO PROCESSES ON ONE MACHINE SHARING THE REAL THING - a named shared-memory section
			// holding the rendezvous word and the packet array, mapped by both instances.
			//
			// This is not a simulation of the link, it IS the link: Like a Dragon Gaiden runs its
			// cabinets in one process against shared memory, and this hands the module exactly
			// that. p_work_ptr[0] and [1] point at the SAME array, so a board writes its own slot
			// and reads every slot including its own, with no copy and no protocol anywhere.
			//
			// Worth having as its own mode rather than testing over loopback, for two reasons.
			// RPCN cannot loopback at all (it hardcodes port 3658 in the address it hands a peer),
			// which is why Virtual On needed a direct-UDP harness. And more importantly this
			// separates the two questions that a network test would confound: "does SRC2 play
			// linked" and "is YAMP's wire protocol right". Only the first is worth answering first.
			Shared = 2,
			// A real peer over the netplay plugin's linked-cabinet channel (kPacketLink, the one
			// Virtual On already uses). Our TX slot goes on the wire, the peer's packet lands in
			// its RX slot, and the rendezvous bits follow the packets.
			//
			// WHICH TRANSPORT CARRIES IT IS NOT THIS FILE'S BUSINESS, and that separation is the
			// point. `-net-link` arms the channel over a plain address pair - no server, no room,
			// no NAT traversal - and an RPCN room arms the same channel over the same calls. So the
			// wire protocol below is proven once, against the transport with the fewest ways to
			// fail, and swapping in RPCN afterwards changes nothing here. Everything that goes
			// wrong with RPCN (login, room scoping, signaling, punching) presents from the game as
			// a link that never came up, which is precisely the confusion worth avoiding.
			//
			// TWO CABINETS. The plugin's channel is point-to-point, so a ring of three or more is
			// not expressible on it - the arrays and the rendezvous groups here are written for
			// MAX_NODES because the MODULE's are, not because this transport can fill them.
			Link = 3,
		};

		// Read once, before module_start, because the module latches the node id and count out of
		// the config at that point and never looks again. That is also why the cabinet's ROLE
		// cannot be changed on a running board here the way Virtual On's can: there is no
		// equivalent of SoftResetIntoRole, and changing it means restarting the module.
		//
		// THE DEFAULT IS A STAND-ALONE CABINET. No room and no override means no peer count, no
		// node id, and the operator's own LINK ID setting left alone - which is what every
		// single-player session is and what the board boots as.
		//
		// Sources, in priority order:
		//   1. A NETPLAY SESSION, which is either a room or a direct link and this file cannot
		//      tell them apart - deliberately. Both publish a LOCAL PLAYER ID, and that id is the
		//      whole of what the cabinet's role is read from: 0 makes this machine the MASTER,
		//      anything else the SLAVE, the same rule Virtual On uses and the same way two real
		//      cabinets are wired. `-net-link-node 0/1` therefore needs no case of its own here.
		//      A live link also takes over the board's LINK ID row (see YAMPUserInterface),
		//      because the service-menu row and the module's node id describe one fact and must
		//      not disagree.
		//   2. YAMP_PRE3_LINK, the harness, matching the YAMP_PRE3_* family the diagnostics use:
		//        probe[:count]              one cabinet, an imaginary peer that never speaks
		//        shared:<nodeId>[:count]    two processes on this machine, real shared memory
		//        <nodeId>[:count]           a peer over the netplay plugin
		//
		// THE ROOM MUST EXIST BEFORE THE BOARD BOOTS. This is read once, immediately before
		// module_start, because that is where the module latches both fields. A room formed later
		// cannot change the role without restarting the module.
		//
		// Sega Racing Classic 2 only. Fighting Vipers 2 is not a linked-cabinet game and handing
		// it a peer count would drag in behaviour it does not want — the NVRAM initialiser forces
		// VS mode on, and the input path changes the coin/start latch — for a board with no comm
		// object to talk through.
		void Configure();

		// THE ROLE CHANGE ON A RUNNING BOARD - pre3's answer to Virtual On's SoftResetIntoRole, and
		// the thing that lets a room decide the cabinet rather than the command line.
		//
		// Configure() above runs once, before module_start, and by then an RPCN room does not exist:
		// connect, TLS, login, discovery and create/join are seconds of round trips, and the board
		// has to boot long before any of them land. So "ask the room what this cabinet is" answers
		// `no room` on every launch, and the cabinet comes up STAND-ALONE every time. The fix is not
		// to guess the role earlier from the launch flags - a room is what actually decides it, and
		// a player who joins from the lobby never touched a launch flag. The fix is to be able to
		// CHANGE it afterwards.
		//
		// VIRTUAL ON DOES THIS BY PULSING THE TEST SWITCH: the ROM re-runs `Net_check` when the
		// operator menu is left, and that is where it re-reads its cabinet byte. SRC2's boot network
		// check (`FUN_00093DB4`, guest 0x18A1C) is not reachable that way - it is called once from
		// the boot chain and nothing re-enters it - so the equivalent here is bigger and, usefully,
		// more literal: PUT THE BOARD BACK TO POWER-ON AND LET IT BOOT AGAIN.
		//
		// Everything that needs is already the module's own:
		//   * the config globals hold the node id and peer count, and the comm board re-latches
		//     them when its state machine passes back through state 1 - which the guest does for
		//     itself during boot, by writing 0 to 0xC0010180;
		//   * Determinism's power-on snapshot is the pristine pre-boot machine, captured before a
		//     single guest frame ran, and restoring it makes the guest re-run its whole boot chain
		//     including the network check;
		//   * ArcadeSettings::Reset re-arms the LINK ID row so the service-menu byte is written
		//     again after the restore has wiped guest RAM.
		//
		// So the host writes two config words, re-arms the settings latch and asks for the restore.
		// The module does the rest, and the cabinet comes up as a MASTER or a SLAVE having genuinely
		// booted as one - which is what the ROM's own check needs in order to print its verdict.
		//
		// Call once per frame. A no-op when the room's answer already matches what the board was
		// booted as, which is every frame but the one after a room forms or dissolves.
		void DriveRoomRole();

		// True while a role change is waiting for the board to come back. The host must not treat
		// these frames as ordinary ones - see Pre3Host.
		bool RoleResetPending();

		// IS THIS GAME A LINKED-CABINET GAME AT ALL? Answered from the title, not from whether a
		// session happens to be up, because the UI needs it before any session exists - it decides
		// whether the Netplay page is offered.
		//
		// DELIBERATELY NOT `pre3::NetplaySupported()`, which is the LOCKSTEP question and answers
		// FV2 only: that path is gated on a pinnable deterministic clock, which SRC2 does not have
		// and does not need. Conflating the two is exactly why a linked SRC2 cabinet showed no
		// netplay UI at all - no page, no overlay, no room id - while its link was working
		// perfectly. Virtual On had the same gap on the m2ftg side and the same answer.
		bool LinkedCabinetSupported();

		// WHAT THE OVERLAY RENDERS, and the shape is `m2ftg::K2::LinkedCabinet`'s on purpose: the
		// two games answer the same questions about the same kind of link, and the UI should not
		// have to care which emulator is running.
		//
		// Every field is the cabinet's OWN answer rather than the host's intent - `checkDone` in
		// particular is the ROM's verdict from its boot network check, not "YAMP thinks the link is
		// up". A host that reported its own hopes would say the ring was fine while the board sat
		// in its check.
		struct CabinetStatus
		{
			uint32_t role;      // 1 = MASTER, 2 = SLAVE, matching the board's own LINK ID row
			bool ringUp;        // the peer's packets are arriving AND the board is transferring
			uint8_t nodeId;     // as the ROM read it out of the ring (1-based), guest 0x10062B
			uint8_t nodes;      // guest 0x10062A
			bool checkDone;     // FUN_00093DB4 passed both gates and agreed on a ring
		};
		bool GetLinkedCabinet(CabinetStatus& out);

		Mode CurrentMode();
		uint8_t NodeId();
		uint8_t NodeCount();

		// What Pre3Host writes into the config block. Zero/zero when the mode is Off, which is the
		// state the module treats as "no link" and is what YAMP has always passed.
		//
		// NOTE THE FIELD NAMES: config +0x100C is the NODE ID and +0x1010 the node count. Neither
		// has a writer inside the module because both are just bytes module_start copies out of
		// the params block, which is why the recon had to find them by arithmetic against the
		// config global (DLL 0x18018A050 + 0x100C = DAT_18018B05C, the value FUN_180035BA0 latches
		// as its node id).
		int32_t ConfigNodeId();
		int32_t ConfigNodeCount();

		// Point the module's three work pointers at this file's buffers. Idempotent and cheap, so
		// the host calls it every frame rather than tracking whether it has run — the module reads
		// the pointers during its own frame and a persistent execute_info makes any other timing
		// rule a trap for a future edit.
		//
		// A no-op when the mode is Off, which leaves the three slots exactly as they were: null,
		// the path the module already handles.
		void Attach(void* workPointers[3]);

		// THE LINKED-CABINET FRAME PACER - Virtual On's VirtualClock policy (m2ftg/VirtualClock.h),
		// ported minus the half pre3 does not need. VON had to patch the module's QPC wrapper
		// because its board advances with WALL time; pre3's board is frame-deterministic by
		// construction - a fixed CPU budget of cpu_clock/60 per update stage, nothing in the frame
		// path sampling elapsed time - so "pace the board" reduces to "call the update stage at
		// 60 Hz", and both of VON's remaining pieces transfer:
		//
		//   * the LIMITER: while a link is live the host loop must hold 60 Hz even if the user's
		//     FPS cap is off and the monitor is faster - game speed is a competitive advantage on
		//     a linked pair, and a cabinet running its board at 144 Hz lives 2.4x further into the
		//     race than its peer (the exact on-screen divergence §10 of the recon leaves open).
		//     LinkPacingActive() is the host loop's gate for forcing the cap.
		//   * the ACCUMULATOR: a host frame that overran its budget steps the board more than once
		//     to stay at 60 Hz in wall time - the ordinary fixed-timestep loop, capped at 4 with
		//     the debt dropped past the cap (a long hitch repaid in one burst reads as a hang).
		//     BoardFramesDue() answers how many board frames this host frame owes; call it exactly
		//     once per host frame. 1 in the steady state, and always 1 outside a live link - solo
		//     play keeps the old policy, where a slow host runs slow rather than stuttering.
		//
		// Safe here for the reason it was safe for VON and unsafe under lockstep: there is no
		// frame numbering to violate and no peer to stay bit-identical with - the ROM's own
		// protocol synchronises, and this only fixes the RATE. Deliberately NOT coupled to the
		// peer (no ping term, no adaptive skew): pacing one cabinet to the other is a feedback
		// loop and a competitive lever, where pinning both to the crystal's rate - which is what
		// twin hardware does - needs no cooperation at all.
		bool LinkPacingActive();
		unsigned int BoardFramesDue();

		// One emulated frame's worth of host work, called BEFORE the module's update stage.
		//
		// Publishes the TX slot the board filled during the PREVIOUS frame, ingests whatever has
		// arrived for the peers, and maintains the rendezvous bits. The ordering matters and is
		// not arbitrary: the board writes TX inside its frame, so the freshest packet available to
		// send is always last frame's, and reading it before this frame's update is what makes
		// "one packet per board frame" true. Virtual On learned that rule the expensive way — see
		// the ordered-delivery half of docs/von-handoff.md.
		void Update();

		// The board's own view of the link, read back out of the CXComm subobject: its state
		// machine, packet size, sequence counter and bank. False when there is no board to read or
		// the object does not look like a CXComm.
		//
		// SELF-CHECKING, because a wrong offset here would produce plausible numbers rather than
		// an obvious failure: the vtable pointer at rom+0x588 is compared against the module's own
		// CXComm vtable before any field is believed. That is the lesson already written next to
		// RAM_PTR in HleHooks.cpp — validate a memory probe against a known-fixed value before
		// believing anything it says.
		struct BoardView
		{
			uint8_t state;        // comm+0x20028, the FUN_180035BA0 state machine (1..5)
			uint8_t bank;         // comm+0x20010, which of the two banks the guest sees
			uint16_t packetSize;  // comm+0x20024, programmed by the GUEST
			uint16_t sequence;    // comm+0x20026, bumped once per completed transfer
			int32_t nodeId;       // comm+0x20008, latched from the config in state 1
			int32_t nodeCount;    // comm+0x2000C
			uint32_t command;     // comm+0x20018
			uint32_t status;      // comm+0x2001C
		};
		bool ReadBoard(BoardView& out);

		// THE COHERENT PROBE, and the reason it is a separate entry point from LogState.
		//
		// Everything else in this file samples the board from the HOST thread while `m3e_ctrl` is
		// inside its own frame - the data race docs/pre3-netplay.md §3.8 documents, and the one
		// that cost a netplay round once already. For most of what LogState prints that is
		// tolerable: the comm state machine and the packet size change slowly.
		//
		// It is NOT tolerable for interrupts. The frame step raises VBlank and the guest's ISR
		// acknowledges it WITHIN one emulated frame, so a racing sample can miss the entire life
		// of the bit and report "VBlank never fires" about a board where it fires every frame.
		// That is exactly the contradiction the recon is stuck on.
		//
		// Call this AFTER the module's update stage and AFTER WaitForEmulatedFrame, i.e. at a
		// point where the worker is parked and the frame just simulated is the one being read.
		// Gated on YAMP_PRE3_SYNCPROBE so it costs nothing when nobody is looking.
		// Whether the frame-boundary probe was asked for. The host tests this BEFORE waiting on the
		// emulator, because that wait is what costs: performing it unconditionally put the host loop
		// in lockstep with the worker and took a stand-alone board from ~395 draws a frame to 8.
		bool TraceEnabled();

		void SampleAtFrameBoundary();

		// Write the collected samples out. Called once at teardown, because SampleAtFrameBoundary
		// deliberately does NO I/O: logging per frame was expensive enough to change the outcome it
		// was measuring - two runs with the log enabled converged where three without it hung - and
		// a probe that flips the race it is observing is worse than no probe.
		void DumpTrace();

		// One line per change plus a heartbeat, into the debug log. This is the whole instrument
		// for the first experiment, and it is what says which of "the ROM never drives the link"
		// and "YAMP never delivers a packet" is happening — the two failures look identical from
		// the outside.
		void LogState();
	}
}
