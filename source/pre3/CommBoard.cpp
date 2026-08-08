#include "CommBoard.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"
#include "../net/NetPlugin.h"
#include "ArcadeSettings.h"
#include "Determinism.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace pre3
{
	namespace CommBoard
	{
		// ---- reaching the board ----------------------------------------------------------
		//
		// Same justification as Determinism.cpp's Machine namespace, and the same two anchors:
		// the module is ASLR'd so everything is an RVA, and GameVerify has already pinned this
		// exact build by SHA-256, so these are DATA offsets with no code to pattern-match.
		namespace Board
		{
			// THE MACHINE IS NOT RE-DERIVED HERE. Determinism owns the RVA that finds it and
			// publishes the ROM object through BoardRomObject(); a second copy of that address
			// would be a second thing to get wrong when a module build moves, and the two copies
			// would not disagree loudly - each would read a plausible number out of a different
			// structure. What IS here is everything below the ROM object, which is layout rather
			// than address and has no code to anchor a pattern on.
			inline constexpr int MACHINE_RUNNING = 0x10;

			// The CXComm subobject inside M3ERomSrc2, from the ROM factory's
			// `plVar5[0xb1] = CXComm::vftable` (DLL 0x180038BCF). Spikeout's sits at 0x5E0, which
			// is why this is not a shared constant: it is per game, and only one of the two games
			// that have one is reachable from a stock install.
			inline constexpr size_t ROM_COMM = 0x588;

			// The module's own CXComm vtable. Used ONLY as a self-check — see ReadBoard.
			inline constexpr uintptr_t RVA_CXCOMM_VTABLE = 0x10C858;

			// THE CONFIG GLOBAL, which module_start fills by copying 0x1028 bytes out of the params
			// block and which nothing in the module ever writes again. Same justification as the
			// vtable RVA above: GameVerify has pinned this build by SHA-256, and a data global has
			// no code around it to pattern-match on.
			//
			// It is the LIVE copy - the one FUN_180035BA0 latches its node id from in state 1, and
			// the one the settings injector folds into the board's LINK ID row at boot. Writing it
			// is therefore how a role change reaches BOTH of the two switches that have to agree,
			// without either being written directly.
			inline constexpr uintptr_t RVA_CONFIG = 0x18A050;
			inline constexpr size_t CONFIG_LINK_NODE_ID = 0x100C;
			inline constexpr size_t CONFIG_LINK_PEER_COUNT = 0x1010;

			// Fields inside CXComm, all read off its seven methods and FUN_180035BA0.
			inline constexpr size_t COMM_NODE_ID = 0x20008;
			inline constexpr size_t COMM_NODE_COUNT = 0x2000C;
			inline constexpr size_t COMM_BANK = 0x20010;
			inline constexpr size_t COMM_COMMAND = 0x20018;
			inline constexpr size_t COMM_STATUS = 0x2001C;
			inline constexpr size_t COMM_PACKET_SIZE = 0x20024;
			inline constexpr size_t COMM_SEQUENCE = 0x20026;
			inline constexpr size_t COMM_STATE = 0x20028;

			// Only the CXComm vtable self-check needs the module's own base; everything else
			// arrives as a pointer from Determinism.
			static const uint8_t* ModuleBase()
			{
				return reinterpret_cast<const uint8_t*>(
					GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
			}

			// The CXComm, or null when the machine is not up, the game is not SRC2, or the object
			// at rom+0x588 does not carry the vtable it should.
			static const uint8_t* Comm()
			{
				if (gGeneral.GetGameId() != YAMPGeneral::GameId::SRC2) return nullptr;

				const uint8_t* base = ModuleBase();
				if (base == nullptr) return nullptr;

				const auto* rom = static_cast<const uint8_t*>(BoardRomObject());
				if (rom == nullptr) return nullptr;

				const uint8_t* comm = rom + ROM_COMM;
				// THE SELF-CHECK. A wrong offset here reads plausible small integers out of
				// whatever else lives in a 0x20610-byte object, and "state 0, size 0" is exactly
				// what a board that has not linked looks like — so the failure would be invisible
				// and would be believed. The vtable pointer is the known-fixed value.
				//
				// Compared against the RESOLVED vtable when the import found one, so the check is
				// no longer only as good as the constant it validates against; the RVA is the
				// fallback and is right for the build GameVerify pins.
				const void* expected = BoardCommVtable();
				if (expected == nullptr) expected = base + RVA_CXCOMM_VTABLE;
				if (*reinterpret_cast<const void* const*>(comm) != expected)
				{
					return nullptr;
				}
				return comm;
			}
		}

		// ---- the buffers the module memcpys through --------------------------------------
		//
		// SIZED FOR THE WORST CASE ON PURPOSE. The module writes `tx + packetSize * nodeId` and
		// reads `rx + packetSize * i` with a packetSize the GUEST programs, so a buffer sized for
		// an expected value is a heap overflow that waits for a ROM to ask for a bigger packet.
		// MAX_NODES * MAX_PACKET is 256 KB each, allocated once, which is not worth being clever
		// about.
		static std::vector<uint8_t> s_tx;
		static std::vector<uint8_t> s_rx;
		static uint64_t s_rendezvous = 0;

		static Mode s_mode = Mode::Off;
		static uint8_t s_nodeId = 0;
		static uint8_t s_nodeCount = 0;
		static bool s_configured = false;

		// WHAT THE BOARD WAS ACTUALLY BOOTED AS - one source of truth, and deliberately not the
		// same variable as s_mode/s_nodeId.
		//
		// The two describe different instants. s_nodeId is what the HOST wants this cabinet to be
		// and changes the moment a room forms; this is what the guest last BOOTED as, and it only
		// moves when the board has been through the restore and come back. Collapsing them would
		// make DriveRoomRole think its work was done the instant it asked for it, which is the
		// same mistake Virtual On's ApplyCabinetRole documents from the other direction - there,
		// reading the SETTING instead of the applied role reported a standalone cabinet while the
		// ROM had already been told it was a slave.
		enum class Role : uint8_t { Unknown, Standalone, Master, Slave };
		static Role s_appliedRole = Role::Unknown;
		static bool s_roleResetPending = false;
		static uint32_t s_roleResetFrame = 0;

		// ---- the rendezvous word ---------------------------------------------------------
		//
		// Four bit groups in one u64, and they are DIRECTIONAL - see CommBoard.h. The low two are
		// what the module waits on and the host must drive; the high two are what the module
		// reports about itself.
		inline constexpr unsigned BIT_READY = 0x00;    // host->module: node i's packet is in RX
		inline constexpr unsigned BIT_LINKED = 0x10;   // host->module: node i has booted
		inline constexpr unsigned BIT_ACK = 0x20;      // module->host: the guest's 0xF000 answer
		inline constexpr unsigned BIT_BOOTED = 0x30;   // module->host: this node has booted

		// The word the module actually reads: the shared section's when there is one, this
		// process's own otherwise. Everything below writes through here so the bit bookkeeping is
		// written once rather than once per transport.
		static uint64_t& Rendezvous();

		static uint64_t NodeMask(unsigned group, uint8_t node)
		{
			return 1ull << ((group + node) & 63);
		}

		// How long a peer's packet stays "fresh" enough to hold its ready bit up.
		//
		// A DELIBERATE POLICY RATHER THAN A READING OF THE MODULE. Nothing in the module ever
		// CLEARS a ready bit, so holding them all set makes the board free-run and holding them
		// strictly per-frame makes it lockstep; the truth is whatever Gaiden's own host does, which
		// cannot be read from here. This picks the middle: a peer's bit stays up while its packets
		// keep arriving and drops when they stop, so jitter is absorbed and a dead peer stalls the
		// board rather than being simulated as a silent one.
		//
		// SIX FRAMES WAS FOR SHARED MEMORY, WHERE IT COULD NEVER FIRE. Over a wire the two cabinets
		// do not run at the same rate - a debug build with the tilemap probes on is nowhere near a
		// release build's frame time - so a window measured in OUR frames is a window whose length
		// in THEIR frames is unknown. Thirty is half a second at 60 Hz, which is far longer than any
		// jitter this link can produce and far shorter than a player would take to notice a peer
		// that has actually gone. The exact number is not load-bearing; being well clear of normal
		// scheduling noise is.
		inline constexpr uint32_t STALE_FRAMES = 30;

		static uint32_t s_frame = 0;
		static uint32_t s_lastRx[MAX_NODES] = {};
		static bool s_everRx[MAX_NODES] = {};

		// Wire counters, for the log only. THREE numbers rather than one because they fail
		// separately and the whole diagnostic value is in which of them is stuck:
		// nothing sent = this cabinet is not producing; sent but nothing received = the wire or the
		// far end; received but the board sequence frozen = the peer's process is alive and its
		// BOARD is not transferring, which no other field here distinguishes.
		static uint32_t s_txPackets = 0;
		static uint32_t s_rxPackets[MAX_NODES] = {};
		static uint16_t s_peerBoardSeq[MAX_NODES] = {};

		// ---- the wire ---------------------------------------------------------------------
		//
		// kPacketLink carries an opaque datagram, so this header is the only framing there is.
		// Unlike Virtual On's payload the length is not a constant — the guest chooses it — so it
		// has to travel with the packet, and the node id has to as well: a receiver cannot assume
		// "the other one" once a ring can hold more than two cabinets.
		//
		// `flags` is the sender's own contribution to the rendezvous word, extracted from its
		// three bit positions and re-expanded into the peer's positions on arrival. Carrying it
		// rather than inferring it is what lets the BOOT barrier work at all: the module sets its
		// own bit 0x30 and waits for everyone's, and no other channel exists to learn that a peer
		// has finished booting.
		//
		// `seq` EXISTS TO DEFEAT A DEDUPE, and without it this link stalls on any static screen.
		//
		// The plugin's link channel drops an arriving payload that is byte-identical to the last
		// one it enqueued (Plugin.cpp's LinkPush), which is right for the sender it was written
		// for - Virtual On rate-limits itself to one datagram per board frame - and wrong for this
		// one, which transmits every host frame and needs every transmission to count as a sign of
		// life. A cabinet whose packet has not changed is not a cabinet that has gone away, but with
		// the dedupe in the way it looks like one: nothing arrives, STALE_FRAMES expires, this host
		// clears the peer's ready bit and the board waits forever for a peer that is transmitting
		// perfectly. A counter that moves every frame makes each frame's datagram distinct while
		// leaving the three REDUNDANCY COPIES of one datagram byte-identical - so they still collapse
		// to one, which is exactly the split that is wanted.
		//
		// `boardSeq` is the emulated board's own transfer counter, carried for the log only. It is
		// the one field that separates "the peer's process is alive and sending" from "the peer's
		// BOARD is running transfers", and those two look identical from every other field here.
#pragma pack(push, 1)
		struct WireHeader
		{
			uint8_t magic;      // 'C', so a stale ABI's datagram is dropped rather than decoded
			uint8_t node;
			uint8_t flags;      // bit0 ready, bit1 ack, bit2 booted
			uint8_t seq;        // low 8 bits of the sender's host frame counter - see above
			uint16_t size;
			uint16_t boardSeq;  // CXComm's own sequence, diagnostic only
		};
#pragma pack(pop)
		static_assert(sizeof(WireHeader) == 8);

		// The most this datagram may be. The plugin's channel carries kLinkPayloadMax = 0x700
		// bytes, and anything longer is refused by link_send with no error the caller can see - so
		// the limit is restated here, where it can be reported against the size the GUEST chose.
		// SRC2 programs 848, which with the header is 856 and fits with room to spare.
		inline constexpr size_t MAX_WIRE = 0x700;

		inline constexpr uint8_t WIRE_MAGIC = 'C';
		inline constexpr uint8_t FLAG_READY = 0x01;
		inline constexpr uint8_t FLAG_ACK = 0x02;
		inline constexpr uint8_t FLAG_BOOTED = 0x04;

		static std::vector<uint8_t> s_wire;

		// ---- the shared section (Mode::Shared) --------------------------------------------
		//
		// One named mapping holding the rendezvous word and the packet array, mapped by every
		// instance on this machine. The module gets the SAME pointer for TX and RX, which is what
		// makes this the real link rather than a model of it: a board writes its own slot and reads
		// every slot including its own, exactly as Gaiden's single-process cabinets do.
		//
		// It also answers, for free, the question CommBoard could not settle by reading - whether a
		// cabinet is supposed to see its own packet come back. With one array it necessarily does.
		struct SharedRing
		{
			uint64_t rendezvous;
			uint8_t packets[static_cast<size_t>(MAX_NODES) * MAX_PACKET];
		};

		static HANDLE s_section = nullptr;
		static SharedRing* s_ring = nullptr;

		// Local\ rather than Global\: the session namespace needs no privilege, and two instances
		// under one login is the whole use case.
		static bool MapSharedRing()
		{
			if (s_ring != nullptr) return true;

			s_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
				static_cast<DWORD>(sizeof(SharedRing) >> 32),
				static_cast<DWORD>(sizeof(SharedRing) & 0xFFFFFFFF),
				L"Local\YAMP_PRE3_COMM");
			if (s_section == nullptr) return false;

			// A fresh section is zero-filled by the kernel, so the first instance needs no init and
			// the second must NOT clear what the first has already published.
			s_ring = static_cast<SharedRing*>(
				MapViewOfFile(s_section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRing)));
			return s_ring != nullptr;
		}

		// ---- the emulated interrupt controller, for diagnosis ----------------------------
		//
		// PRE3 DOES RAISE VBLANK. The frame step asserts it unconditionally once per emulated frame
		// (DLL 0x18003B0A0: `mem->vtable[0x90](mem, 2)`), alongside 0x40 in its scanline loop and 9
		// and 4 at the end. An earlier version of this file claimed the opposite and shipped an
		// "injection" to make up for it. That claim came from scanning only vtable slot 0x98, and
		// 0x98 is the CLEAR:
		//
		//     CM3Mem vtable 0x18010BC50:  +0x90 = FUN_180012430   state |=  mask    RAISE
		//                                 +0x98 = FUN_180012460   state &= ~mask    CLEAR
		//
		// so the injection was CLEARING VBlank every frame. Recorded because the mistake is easy to
		// repeat: the SCSI window's `vtable[0x98](0x100)` reads naturally as "raise the SCSI IRQ"
		// and is in fact an acknowledge-on-read.
		//
		// What the raise does with the mask is the next question, and it is why these two bytes are
		// logged rather than reasoned about:
		//
		//     state |= mask;
		//     if ((state & (ENABLE | 0x100)) != 0 && cpu != NULL) cpu->assert_line(1);
		//
		// ENABLE is mem+0x34, which the GUEST programs through 0xF0100014 - so a raise the guest has
		// not enabled sets the state bit and delivers nothing. mem+0x36 is the state word whose LOW
		// BYTE the guest polls at 0xF0100018 and dispatches its ISRs from.
		// The probe's storage. 8192 frames is over two minutes at 60 Hz, and it wraps rather than
		// stopping so a long run keeps its most recent window.
		struct Sample
		{
			uint32_t frame;
			uint32_t nodes;
			uint16_t irqState;
			uint16_t sequence;
			uint8_t irqEnable;
			uint8_t vblank;
			uint8_t irq08;
			uint8_t irq04;
			uint8_t netFlags;
			uint8_t commState;
		};
		inline constexpr size_t TRACE_CAPACITY = 8192;
		static Sample s_trace[TRACE_CAPACITY];
		static size_t s_traceCount = 0;

		inline constexpr size_t MEM_IRQ_ENABLE = 0x34;
		inline constexpr size_t MEM_IRQ_STATE = 0x36;

		static bool ReadIrq(uint8_t& enable, uint16_t& state)
		{
			const auto* mem = static_cast<const uint8_t*>(BoardMemObject());
			if (mem == nullptr) return false;
			enable = *(mem + MEM_IRQ_ENABLE);
			state = *reinterpret_cast<const uint16_t*>(mem + MEM_IRQ_STATE);
			return true;
		}

		static uint64_t& Rendezvous()
		{
			return (s_mode == Mode::Shared && s_ring != nullptr) ? s_ring->rendezvous : s_rendezvous;
		}

		// ---- configuration ----------------------------------------------------------------

		// YAMP_PRE3_LINK=probe[:count] | <nodeId>[:<count>]
		//
		// Env-gated, matching YAMP_PRE3_DIAG / YAMP_PRE3_SECURITY / YAMP_PRE3_DUMP, because the
		// value has to be read BEFORE module_start — the module latches the node id out of the
		// config there and never looks again — and because the probe is a harness rather than a
		// feature anyone would want a checkbox for.
		static bool ParseEnvironment()
		{
			char value[32];
			size_t length = 0;
			if (getenv_s(&length, value, sizeof(value), "YAMP_PRE3_LINK") != 0 || length <= 1)
			{
				return false;
			}

			const char* colon = strchr(value, ':');
			const int count = (colon != nullptr) ? atoi(colon + 1) : 2;
			s_nodeCount = static_cast<uint8_t>((count >= 2 && count <= MAX_NODES) ? count : 2);

			if (value[0] == 'p' || value[0] == 'P')
			{
				s_mode = Mode::Probe;
				s_nodeId = 0;
				return true;
			}

			// shared:<nodeId>[:count] - note the node id is the FIRST colon field here, so the count
			// moves to the second and the plain forms keep theirs where they were.
			if (value[0] == 's' || value[0] == 'S')
			{
				s_mode = Mode::Shared;
				s_nodeId = (colon != nullptr) ? static_cast<uint8_t>(atoi(colon + 1)) : 0;
				const char* second = (colon != nullptr) ? strchr(colon + 1, ':') : nullptr;
				const int sharedCount = (second != nullptr) ? atoi(second + 1) : 2;
				s_nodeCount = static_cast<uint8_t>(
					(sharedCount >= 2 && sharedCount <= MAX_NODES) ? sharedCount : 2);
				if (s_nodeId >= s_nodeCount) s_nodeId = 0;
				return true;
			}

			const int id = atoi(value);
			s_mode = Mode::Link;
			s_nodeId = static_cast<uint8_t>((id >= 0 && id < s_nodeCount) ? id : 0);
			return true;
		}

		static void EnsureBuffers()
		{
			if (s_tx.empty())
			{
				s_tx.assign(static_cast<size_t>(MAX_NODES) * MAX_PACKET, 0);
				s_rx.assign(static_cast<size_t>(MAX_NODES) * MAX_PACKET, 0);
				s_wire.assign(sizeof(WireHeader) + MAX_PACKET, 0);
			}
		}
	}

	// -------------------------------------------------------------------------------------

	void CommBoard::Configure()
	{
		if (s_configured) return;
		s_configured = true;

		// SRC2 ONLY, and not by preference. Fighting Vipers 2 has no CXComm to talk through, and
		// a non-zero peer count would still change its board: the NVRAM initialiser forces VS mode
		// on and the input path stops latching coin/start. See docs/pre3-netplay.md §6.
		if (gGeneral.GetGameId() != YAMPGeneral::GameId::SRC2)
		{
			return;
		}

		// THE ROOM DECIDES THE ROLE, and the default is a STAND-ALONE CABINET.
		//
		// A cabinet nobody has linked is SINGLE: no peer count, no node id, LINK ID left at the
		// operator's setting. Hosting a room makes this machine the MASTER (node 0) and joining
		// one makes it the SLAVE (node 1), which is the same rule Virtual On uses - `local_player`
		// 0 = host, 1 = guest - and the same rule two real cabinets are wired under.
		//
		// A ROOM FORMED LATER IS NOT A LOST CAUSE - see DriveRoomRole, which is the pre3 answer to
		// Virtual On's SoftResetIntoRole. This function still runs exactly once, immediately before
		// module_start, because that is where the module latches the node id and peer count out of
		// the config block; what DriveRoomRole adds is the ability to CHANGE those and put the
		// board back through its own power-on boot so the new values are read. So this is the
		// launch-time answer, not the only one.
		const net::Status netStatus = net::GetStatus();
		const bool inRoom = netStatus.local_player >= 0;

		if (inRoom)
		{
			s_mode = Mode::Link;
			s_nodeId = static_cast<uint8_t>(netStatus.local_player != 0 ? 1 : 0);
			s_nodeCount = 2;
		}
		else if (!ParseEnvironment())
		{
			// No room and no harness override: a single cabinet, exactly as before any of this.
			// DriveRoomRole takes over from here if one appears.
			s_appliedRole = Role::Standalone;
			return;
		}

		EnsureBuffers();
		// Every mode named, because a ternary that only knew about Probe printed "LINK" for a
		// Shared session and sent a diagnosis chasing the wrong transport for a whole run.
		const char* modeName = "?";
		switch (s_mode)
		{
		case Mode::Probe:  modeName = "PROBE (no peer)"; break;
		case Mode::Shared: modeName = "SHARED (local section)"; break;
		case Mode::Link:   modeName = "LINK (netplay)"; break;
		default: break;
		}
		s_appliedRole = (s_mode == Mode::Off) ? Role::Standalone
			: (s_nodeId == 0 ? Role::Master : Role::Slave);
		DebugLogFile("[%s link] mode=%s node=%u/%u (config +0x100C/+0x1010) from %s\n",
			gGeneral.GetGameTag(), modeName, s_nodeId, s_nodeCount,
			inRoom ? "a live room" : "YAMP_PRE3_LINK");
	}

	// ---- The live role change ------------------------------------------------------------
	//
	// See CommBoard.h for why this exists at all. What follows is the mechanics.

	namespace CommBoard
	{
	namespace
	{
		const char* RoleName(Role role)
		{
			switch (role)
			{
			case Role::Standalone: return "STAND-ALONE";
			case Role::Master:     return "MASTER (node 0)";
			case Role::Slave:      return "SLAVE (node 1)";
			default:               return "unknown";
			}
		}

		// What the ROOM says this cabinet is, right now. The single place the mapping lives, so the
		// launch-time path and the live path cannot drift apart: `local_player` 0 = host = master,
		// anything else = guest = slave, and no room at all = a cabinet nobody has linked.
		Role RoleFromRoom()
		{
			const net::Status status = net::GetStatus();
			if (status.local_player < 0) return Role::Standalone;
			return status.local_player == 0 ? Role::Master : Role::Slave;
		}

		// Write the module's own config globals. Returns false without writing if the module is not
		// where it should be, so a caller can decline to reset a board it cannot reconfigure -
		// rebooting into the SAME role would be a visible glitch that achieved nothing.
		bool WriteConfigRole(uint8_t nodeId, uint8_t nodeCount)
		{
			auto* base = reinterpret_cast<uint8_t*>(
				GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
			if (base == nullptr) return false;

			auto* nodeIdField = reinterpret_cast<int32_t*>(
				base + Board::RVA_CONFIG + Board::CONFIG_LINK_NODE_ID);
			auto* nodeCountField = reinterpret_cast<int32_t*>(
				base + Board::RVA_CONFIG + Board::CONFIG_LINK_PEER_COUNT);

			// SANITY-CHECK BEFORE WRITING, because a wrong RVA here is silent: these are two small
			// integers in a 0x1028-byte block of other small integers, so a misplaced write lands on
			// some other setting and the only symptom is the board behaving oddly in a way nobody
			// connects to netplay. What is known about them is their RANGE - the module's own tests
			// are `< 2` on the count and an index into a 4-slot ring on the id - so anything outside
			// it means this is not the field it is supposed to be.
			const int32_t wasId = *nodeIdField;
			const int32_t wasCount = *nodeCountField;
			if (wasId < 0 || wasId >= MAX_NODES || wasCount < 0 || wasCount > MAX_NODES)
			{
				DebugLogFile("[%s link] role change REFUSED: the config at +0x100C/+0x1010 reads "
					"%d/%d, which is not a node id and count. Not writing.\n",
					gGeneral.GetGameTag(), wasId, wasCount);
				return false;
			}

			*nodeIdField = static_cast<int32_t>(nodeId);
			*nodeCountField = static_cast<int32_t>(nodeCount);
			DebugLogFile("[%s link] config +0x100C/+0x1010: %d/%d -> %u/%u\n",
				gGeneral.GetGameTag(), wasId, wasCount, nodeId, nodeCount);
			return true;
		}

		// Everything this file remembers about a life of the board, dropped. The rendezvous word
		// especially: its bits describe cabinets that no longer exist, and carrying a stale BOOTED
		// bit into a fresh boot would release the module's barrier before the peer was there.
		void ForgetLinkState()
		{
			s_rendezvous = 0;
			s_frame = 0;
			s_txPackets = 0;
			for (uint8_t node = 0; node < MAX_NODES; ++node)
			{
				s_lastRx[node] = 0;
				s_everRx[node] = false;
				s_rxPackets[node] = 0;
				s_peerBoardSeq[node] = 0;
			}
			if (!s_tx.empty())
			{
				std::fill(s_tx.begin(), s_tx.end(), uint8_t{ 0 });
				std::fill(s_rx.begin(), s_rx.end(), uint8_t{ 0 });
			}
		}
	}
	}

	bool CommBoard::RoleResetPending() { return s_roleResetPending; }

	void CommBoard::DriveRoomRole()
	{
		if (gGeneral.GetGameId() != YAMPGeneral::GameId::SRC2) return;

		// A HARNESS SESSION OWNS THE ROLE. YAMP_PRE3_LINK's Probe and Shared modes have no room
		// behind them and never will, so letting "no room" drag them back to stand-alone would
		// make the two single-machine experiments impossible to run. Link is left alone for the
		// same reason: it was asked for explicitly.
		if (s_mode != Mode::Off && s_appliedRole != Role::Standalone
			&& net::GetStatus().local_player < 0)
		{
			return;
		}

		// Waiting for the board to come back. The restore is asynchronous by one frame and the
		// guest then re-runs its whole boot chain, so this stays true for a while - which is the
		// point of publishing it.
		if (s_roleResetPending)
		{
			if (BoardRestorePending()) return;
			s_roleResetPending = false;
			DebugLogFile("[%s link] board restored after %u frame(s); it is now booting as %s\n",
				gGeneral.GetGameTag(), s_frame - s_roleResetFrame, RoleName(s_appliedRole));
			return;
		}

		const Role wanted = RoleFromRoom();
		if (wanted == s_appliedRole) return;

		// The board has to BE somewhere before it can be sent back to the start. Both of these are
		// ordinary early frames rather than faults, so they are silent.
		if (!IsBoardBooted() || BoardRestorePending()) return;

		if (!ResetSnapshotTaken())
		{
			// NOT AN ERROR ON THE FRAME IT FIRST HAPPENS. Pre3Host asks for the snapshot on the
			// first frame the machine reports itself running and the module takes it during that
			// frame's update stage, so on that one frame the answer here is legitimately "not yet" -
			// and a room formed during boot lands exactly there. Reporting it immediately printed
			// "cannot change role" a frame before the role changed perfectly well, which is a line
			// that would send someone looking for a bug that had already fixed itself.
			//
			// So retry quietly and only complain if it PERSISTS, which would mean the snapshot
			// genuinely never happened and the role can never change.
			static uint32_t waiting = 0;
			if (++waiting == 300)
			{
				DebugLogFile("[%s link] cannot change role to %s: 300 frames on and no power-on "
					"snapshot exists, so there is nothing to reboot the board into.\n",
					gGeneral.GetGameTag(), RoleName(wanted));
			}
			return;
		}

		const uint8_t nodeId = (wanted == Role::Slave) ? 1 : 0;
		const uint8_t nodeCount = (wanted == Role::Standalone) ? 0 : 2;
		if (!WriteConfigRole(nodeId, nodeCount)) return;

		// The host's own view moves with the config, and BEFORE the restore is asked for: Attach()
		// runs every frame and the module must find the work pointers already correct on the frame
		// its boot chain first reaches for them.
		s_mode = (wanted == Role::Standalone) ? Mode::Off : Mode::Link;
		s_nodeId = nodeId;
		s_nodeCount = nodeCount;
		if (s_mode != Mode::Off) EnsureBuffers();
		ForgetLinkState();

		// RE-ARM THE SETTINGS LATCH. ArcadeSettings writes the board's LINK ID row once and then
		// latches, leaving the row to the game's own service menu; the restore is about to wipe the
		// guest RAM both copies live in, so without this the rebooted board would read back the
		// SINGLE it powered on with and its network check would return before printing anything.
		ArcadeSettings::Reset();

		DebugLogFile("[%s link] room says this cabinet is %s (was %s) - putting the board back to "
			"power-on so it boots as one\n",
			gGeneral.GetGameTag(), RoleName(wanted), RoleName(s_appliedRole));

		if (!RestoreResetSnapshot())
		{
			DebugLogFile("[%s link] the restore request was refused; the board keeps running as "
				"%s\n", gGeneral.GetGameTag(), RoleName(s_appliedRole));
			return;
		}

		s_appliedRole = wanted;
		s_roleResetPending = true;
		s_roleResetFrame = s_frame;
	}

	bool CommBoard::LinkedCabinetSupported()
	{
		// The title, and nothing else. Sega Racing Classic 2 is the only game in this module with a
		// CXComm to talk through; Fighting Vipers 2 is a lockstep game and answering yes for it
		// would offer the wrong UI for the right feature.
		return gGeneral.GetGameId() == YAMPGeneral::GameId::SRC2;
	}

	bool CommBoard::GetLinkedCabinet(CabinetStatus& out)
	{
		out = {};
		if (!LinkedCabinetSupported() || s_mode == Mode::Off) return false;

		// The board's own LINK ID convention, which is what the player sees on the service screen:
		// 1 MASTER, 2 SLAVE. Node 0 is the master, so the two differ by one and always have.
		out.role = (s_nodeId == 0) ? 1u : 2u;

		BoardView view{};
		const bool haveBoard = ReadBoard(view);

		// RING UP MEANS BOTH HALVES. `net::LinkReady()` alone is the peer's ADDRESS being known,
		// which is true from the moment a room forms and stays true after a cabinet has gone quiet
		// - the same trap Virtual On documents, where it had to age its own liveness timeout on
		// top. Here the second half is the board's own transfer state reaching 4, its running
		// phase, so this cannot claim a ring the emulated hardware is not actually driving.
		out.ringUp = net::LinkReady() && haveBoard && view.state >= 4;

		// THE ROM'S OWN VERDICT, read out of guest RAM rather than inferred. FUN_00093DB4 writes
		// `status & 0x1F` (this cabinet's id, 1-based) and `(status >> 5) & 0x1F` (the node count)
		// into the two low bytes of the word at guest 0x100628, and gates on two bits of
		// 0x7379B9 - 0x20 "board present" and 0x40 "board has no problem". A cabinet that has not
		// finished its check reads zeroes here, which is exactly what the overlay needs to say
		// "still checking" rather than "linked".
		const auto* const ram = static_cast<const uint8_t*>(BoardGuestRam());
		if (ram != nullptr)
		{
			__try
			{
				const uint32_t nodes = *reinterpret_cast<const uint32_t*>(ram + 0x100628);
				const uint32_t flagWord = *reinterpret_cast<const uint32_t*>(ram + 0x7379B8);
				const uint8_t netFlags = static_cast<uint8_t>(flagWord >> 16);   // guest 0x7379B9
				out.nodeId = static_cast<uint8_t>(nodes & 0x1F);
				out.nodes = static_cast<uint8_t>((nodes >> 8) & 0x1F);
				out.checkDone = (netFlags & 0x20) != 0 && (netFlags & 0x40) != 0
					&& out.nodes >= 2;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				out.checkDone = false;
			}
		}
		return true;
	}

	CommBoard::Mode CommBoard::CurrentMode() { return s_mode; }
	uint8_t CommBoard::NodeId() { return s_nodeId; }
	uint8_t CommBoard::NodeCount() { return s_nodeCount; }

	int32_t CommBoard::ConfigNodeId()
	{
		return s_mode == Mode::Off ? 0 : static_cast<int32_t>(s_nodeId);
	}

	int32_t CommBoard::ConfigNodeCount()
	{
		// Zero, not one, when there is no link: the module's own tests are `< 2`, and the config
		// field YAMP has always passed is zero.
		return s_mode == Mode::Off ? 0 : static_cast<int32_t>(s_nodeCount);
	}

	void CommBoard::Attach(void* workPointers[3])
	{
		if (s_mode == Mode::Off) return;

		if (s_mode == Mode::Shared)
		{
			if (!MapSharedRing()) return;
			// THE SAME ARRAY FOR BOTH DIRECTIONS. See SharedRing - this is what makes the mode the
			// real link rather than a model of one.
			workPointers[0] = s_ring->packets;
			workPointers[1] = s_ring->packets;
			workPointers[2] = &s_ring->rendezvous;
			return;
		}

		EnsureBuffers();
		workPointers[0] = s_tx.data();
		workPointers[1] = s_rx.data();
		workPointers[2] = &s_rendezvous;
	}

	namespace
	{
		// A DESYNC LOCATOR, and the reason it hashes blocks rather than a single value: "are the
		// two boards in the same state" and "WHERE did they stop being in the same state" are the
		// same question asked at different resolutions, and only the second one is actionable.
		//
		// Keyed on the GUEST's own frame counter (0x00737978, the master frame counter the module
		// also mirrors to rom+0x205B8), never on the host frame - the two cabinets boot hundreds
		// of host frames apart, so a host-frame comparison would report a desync on every run.
		// Diff two logs by matching gframe.
		//
		// 32 blocks of 0x40000 over the 8 MB work RAM. Under SEH because the upper reaches have
		// never been probed and a diagnostic must not be the thing that takes the board down.
		constexpr uint32_t DIGEST_BLOCKS = 32;
		constexpr uint32_t DIGEST_BLOCK_SIZE = 0x40000;
		constexpr uint32_t DIGEST_INTERVAL = 120;
		constexpr uint32_t ADDR_GUEST_FRAME = 0x737978;

		static uint32_t HashBlock(const uint8_t* p, size_t bytes)
		{
			// FNV-1a over 32-bit words. Words rather than bytes purely for speed: 8 MB every two
			// seconds is already the most expensive thing in this file.
			uint32_t hash = 2166136261u;
			const auto* words = reinterpret_cast<const uint32_t*>(p);
			for (size_t i = 0; i < bytes / 4; ++i)
			{
				hash = (hash ^ words[i]) * 16777619u;
			}
			return hash;
		}

		// ---- THE CAR PROBE: are the AI cars the same on both cabinets? -----------------------
		//
		// The question this exists to answer cannot be answered by the block digest below. That one
		// hashes 32 blocks of 0x40000 over the whole 8 MB and will report a difference for a dozen
		// legitimate reasons - each cabinet has its own car, its own inputs, its own timers - so
		// "the machines differ" says nothing about whether the AI agrees.
		//
		// WHAT MAKES A SHARPER TEST POSSIBLE is the car struct's layout, read out of the ROM's own
		// physics integrator at guest 0x0008DBCC. It takes the car object in r4 and integrates it:
		//
		//     0008dc70  lfs   f29, 0x38(r31)     position  X Y Z   at +0x38 +0x3c +0x40
		//     0008dc58  lfs   f23, 0x50(r31)     velocity  X Y Z   at +0x50 +0x54 +0x58
		//     0008dc98  lfs   f12, 0x1e4(r15)    the motion-integration scalar (hook 13's byte)
		//     0008dc9c  fmuls f13, f23, f12      velocity * scalar
		//     0008dca8  fadds f29, f29, f13      position += that
		//     0008dce0  stfs  f29, 0x38(r31)     written back
		//
		// and its only two callers pass the PLAYER's car, which `FUN_00025540` names outright:
		//
		//     0002557c  addi  r31, r15, 0xbcc    r15 = 0x105000, so the player's car is 0x105BCC
		//
		// So a car is recognisable by its own contents: three finite floats at +0x38 that move
		// smoothly, and three more at +0x50 that are their rate of change. That is what this scans
		// for, rather than needing the AI array's address - which is reached through a function
		// pointer table and was not worth chasing statically when the running game can be asked.
		//
		// PER-BLOCK HASHES AT 0x100 GRANULARITY over a window around the game context, keyed on the
		// GUEST frame exactly as LogSyncDigest is. Diff two cabinets' logs by matching gframe: a
		// block that agrees is state the link is keeping in step, one that differs is state each
		// cabinet owns. The player's own car MUST differ - it is the control that proves the probe
		// can see a difference at all.
		constexpr uint32_t CAR_PLAYER = 0x105BCC;   // proved above
		constexpr size_t CAR_POS = 0x38;
		constexpr size_t CAR_VEL = 0x50;

		// ITS OWN FILE, NOT THE DEBUG LOG, and that is not tidiness - the first race's data came
		// back damaged without it. `DebugLogFile` caps a line at ~1.1 KB and several subsystems
		// write it from different threads, so a 256-block sample was truncated to 185 blocks AND
		// had a `[draw]` line spliced into the middle of it with no newline. Both failures are
		// silent: the parse simply stops early at the first non-hex token and reports a smaller
		// window than was asked for.
		//
		// Opened and CLOSED per sample, the same discipline `yampnet.log` uses, so the file can be
		// read over a share while the game runs rather than only after it exits.
		FILE* OpenCarLog()
		{
			static std::string path = []
			{
				wchar_t exe[MAX_PATH] = {};
				if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return std::string("src2_car.log");
				std::filesystem::path p(exe);
				p.replace_filename(L"src2_car.log");
				return p.string();
			}();
			FILE* f = nullptr;
			return fopen_s(&f, path.c_str(), "a") == 0 ? f : nullptr;
		}

		struct CarProbeConfig
		{
			bool enabled = false;
			// THE DYNAMIC REGION, chosen from the first race's coarse digest rather than guessed.
			// Of the 32 blocks of 0x40000 over the 8 MB work RAM, only ten ever moved during a
			// race, and four of those are 0x100000-0x1FFFFF - which holds the game context at
			// 0x105000, the player's car at 0x105BCC and the object at 0x10D000 the per-frame
			// update carries alongside it. Two of the four (0x140000-0x1BFFFF) came back PARTIALLY
			// agreeing between the cabinets, which is exactly what a region holding both shared and
			// per-cabinet state looks like at 256 KB resolution - and the reason to re-sample it at
			// 0x100.
			uint32_t base = 0x100000;
			uint32_t length = 0x100000;
			uint32_t interval = 30;      // guest frames between samples
		};

		// YAMP_SRC2_CARPROBE=1, or =<hexBase>:<hexLen>:<interval> to move the window once the
		// coarse digest has said where to look.
		const CarProbeConfig& CarProbe()
		{
			static const CarProbeConfig cfg = []
			{
				CarProbeConfig c;
				char value[64];
				size_t length = 0;
				if (getenv_s(&length, value, sizeof(value), "YAMP_SRC2_CARPROBE") != 0
					|| length <= 1 || value[0] == '0')
				{
					return c;
				}
				c.enabled = true;
				// The bare "1" form keeps the defaults; anything with a colon is a full window.
				if (strchr(value, ':') != nullptr)
				{
					c.base = static_cast<uint32_t>(strtoul(value, nullptr, 16));
					const char* first = strchr(value, ':');
					c.length = static_cast<uint32_t>(strtoul(first + 1, nullptr, 16));
					const char* second = strchr(first + 1, ':');
					if (second != nullptr) c.interval = static_cast<uint32_t>(atoi(second + 1));
				}
				// The ceiling is the line buffer's, not the RAM's: 0x400000 is 16384 blocks and a
				// ~80 KB sample line. Its own file makes that affordable where the shared debug log
				// did not - see OpenCarLog.
				if (c.length == 0 || c.length > 0x400000) c.length = 0x100000;
				if (c.interval == 0) c.interval = 30;
				return c;
			}();
			return cfg;
		}

		// A guest float. 4-byte reads through the module's RAM pointer are already in host order -
		// the same property LogSyncDigest relies on to read the frame counter - so no swizzle here.
		// The BYTE accessors elsewhere in YAMP need `^ 3`; these do not.
		float GuestFloat(const uint8_t* ram, uint32_t guestAddress)
		{
			float out = 0.0f;
			memcpy(&out, ram + guestAddress, sizeof(out));
			return out;
		}

		void LogCarProbe(unsigned nodeId)
		{
			const CarProbeConfig& cfg = CarProbe();
			if (!cfg.enabled) return;

			const auto* const ram = static_cast<const uint8_t*>(BoardGuestRam());
			if (ram == nullptr) return;

			// THE BUFFER IS BUILT OUTSIDE THE `__try`, and it has to be: a scope that needs object
			// unwinding cannot contain one (C2712). So the only thing inside the guard is C.
			const uint32_t blockCount = cfg.length / 0x100;
			static std::vector<char> buffer;
			if (buffer.size() < static_cast<size_t>(blockCount) * 5 + 256)
			{
				buffer.assign(static_cast<size_t>(blockCount) * 5 + 256, '\0');
			}
			char* const out = buffer.data();
			const int cap = static_cast<int>(buffer.size());

			// Under SEH for the same reason LogSyncDigest is: a diagnostic must not be the thing
			// that takes the board down, and this window has never been probed.
			__try
			{
				const uint32_t guestFrame = *reinterpret_cast<const uint32_t*>(ram + ADDR_GUEST_FRAME);
				static uint32_t lastBucket = ~0u;
				const uint32_t bucket = guestFrame / cfg.interval;
				if (bucket == lastBucket) return;
				lastBucket = bucket;

				// THE CONTROL, and it is the first thing on the line on purpose. Each cabinet
				// drives its own car, so these three floats MUST differ between the two logs. If
				// they ever match, the probe is reading something that is not the player's car and
				// nothing else on the line can be believed.
				const float px = GuestFloat(ram, CAR_PLAYER + CAR_POS + 0);
				const float py = GuestFloat(ram, CAR_PLAYER + CAR_POS + 4);
				const float pz = GuestFloat(ram, CAR_PLAYER + CAR_POS + 8);
				const float vx = GuestFloat(ram, CAR_PLAYER + CAR_VEL + 0);
				const float vz = GuestFloat(ram, CAR_PLAYER + CAR_VEL + 8);

				int at = _snprintf_s(out, cap, _TRUNCATE,
					"[%s car] gframe=%u node=%u player pos=%.2f,%.2f,%.2f vel=%.3f,%.3f win=%06X+%X ",
					gGeneral.GetGameTag(), guestFrame, nodeId, px, py, pz, vx, vz,
					cfg.base, cfg.length);

				// 16 bits per block rather than 32: the diff only has to say SAME or DIFFERENT, and
				// halving the width halves a log that a two-minute race fills 240 samples of.
				for (uint32_t block = 0; block < blockCount && at > 0 && at < cap - 8; ++block)
				{
					const uint32_t hash = HashBlock(
						ram + cfg.base + static_cast<size_t>(block) * 0x100, 0x100);
					at += _snprintf_s(out + at, static_cast<size_t>(cap - at), _TRUNCATE, "%04X ",
						static_cast<unsigned>((hash ^ (hash >> 16)) & 0xFFFF));
				}
				if (FILE* f = OpenCarLog())
				{
					fprintf(f, "%s\n", out);
					fclose(f);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return;
			}
		}

		void LogSyncDigest(uint32_t /*hostFrame*/, unsigned nodeId)
		{
			const auto* const ram = static_cast<const uint8_t*>(BoardGuestRam());
			if (ram == nullptr) return;

			char line[DIGEST_BLOCKS * 9 + 8];
			int at = 0;
			uint32_t guestFrame = 0;
			__try
			{
				// SAMPLED ON THE GUEST'S CLOCK, NOT THE HOST'S, and this is the whole difference
				// between a usable digest and a useless one. Triggering every 120 HOST frames put
				// the two cabinets 29 guest frames apart, so every block holding anything live
				// differed - twelve of thirty-two, identically, every sample - and that says
				// nothing at all about whether the boards agree. Bucketing the guest counter makes
				// both machines hash the same emulated instant, so a mismatch is a real one.
				guestFrame = *reinterpret_cast<const uint32_t*>(ram + ADDR_GUEST_FRAME);
				static uint32_t lastBucket = ~0u;
				const uint32_t bucket = guestFrame / DIGEST_INTERVAL;
				if (bucket == lastBucket) return;
				lastBucket = bucket;

				for (uint32_t block = 0; block < DIGEST_BLOCKS; ++block)
				{
					const uint32_t hash = HashBlock(
						ram + static_cast<size_t>(block) * DIGEST_BLOCK_SIZE, DIGEST_BLOCK_SIZE);
					at += _snprintf_s(line + at, sizeof(line) - at, _TRUNCATE, "%08X ", hash);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return;
			}

			DebugLogFile("[%s sync] gframe=%u node=%u %s\n",
				gGeneral.GetGameTag(), guestFrame, nodeId, line);
		}
	}

	void CommBoard::Update()
	{
		// BEFORE the no-link early return, deliberately. The probe's whole value is the DIFFERENCE
		// between a linked pair and two stand-alone cabinets given the same drive: what agrees only
		// when linked is what the link is synchronising. Gating it on a link would make the control
		// half of that experiment impossible to run with the same binary.
		LogCarProbe(s_nodeId);

		if (s_mode == Mode::Off) return;
		EnsureBuffers();
		s_frame++;
		LogSyncDigest(s_frame, s_nodeId);

		BoardView view{};
		const bool haveBoard = ReadBoard(view);
		const size_t packet = haveBoard ? view.packetSize : 0;

		// Our own ready bit is unconditional: the board fills its TX slot inside its own frame, so
		// from the host's point of view this node always has a packet to offer. What the peers see
		// is a different question and is answered by the wire below.
		Rendezvous() |= NodeMask(BIT_READY, s_nodeId);

		// Promote our own boot REPORT into the group the module reads back. This is the one place
		// the two directions meet for a single cabinet, and it is why the groups are separate at
		// all: the module says "I am up" in 0x30 and asks "is everyone up?" of 0x10, so even the
		// local node's answer passes through the host.
		if ((Rendezvous() & NodeMask(BIT_BOOTED, s_nodeId)) != 0)
		{
			Rendezvous() |= NodeMask(BIT_LINKED, s_nodeId);
		}

		if (s_mode == Mode::Probe)
		{
			// Stand in for every other cabinet: ready, booted, and silent.
			//
			// BOTH GROUPS, and the LINKED one is not optional: without it the machine's own boot
			// barrier (FUN_1800393D0 case 0xF) never releases and the board never reaches its
			// running phase at all. Measured, on the first probe run, which set the module's
			// REPORT group (0x30) instead of the group the module reads (0x10) and produced a
			// board that ran 800 frames without a single draw.
			for (uint8_t node = 0; node < s_nodeCount; ++node)
			{
				Rendezvous() |= NodeMask(BIT_READY, node) | NodeMask(BIT_LINKED, node);
			}
			LogState();
			return;
		}

		if (s_mode == Mode::Shared)
		{
			// Nothing to send and nothing to ingest: the other instance writes into the same array
			// and the same word. All this cabinet owes the ring is its own two bits, and the peer's
			// appear because they ARE the peer's - no staleness window, no framing, no policy.
			if (s_ring != nullptr) LogState();
			return;
		}

		// ---- Link -----------------------------------------------------------------------
		if (!net::LinkReady())
		{
			// No peer yet. Leave the peers' bits clear: the board then waits, which is the correct
			// behaviour and is the ROM's own.
			//
			// SAID OUT LOUD, on a heartbeat, because "the plugin has no peer address" and "the peer
			// address is known and nothing comes back" are different faults with the same symptom -
			// a cabinet sitting on its network check - and only the first of them is visible here.
			// A link armed with -net-link is never in this state for long: the address is configured
			// rather than discovered, so still being here means the plugin did not load or
			// link_direct was never called.
			static uint32_t lastSaid = 0;
			if (s_frame - lastSaid >= 300)
			{
				lastSaid = s_frame;
				DebugLogFile("[%s link] f=%u no peer: the netplay channel reports no address to "
					"send to. The board will wait here.\n", gGeneral.GetGameTag(), s_frame);
			}
			LogState();
			return;
		}

		// A PACKET SIZE OF ZERO IS NOT A REASON NOT TO SEND, and this is a deadlock if it is:
		// the guest cannot program a size until its board is running, the board cannot run until
		// the boot barrier releases, and the barrier cannot release until each peer has heard that
		// the other has booted - which is carried in this datagram's flags. So the header always
		// goes out; the payload is whatever there is, which early on is nothing.
		// TWO CEILINGS, AND THEY FAIL DIFFERENTLY, which is why they are reported separately.
		//
		// MAX_PACKET is this file's own buffer: a size past it is memory the module is already
		// corrupting and YAMP can only report. MAX_WIRE is the plugin channel's, and a size past
		// THAT is merely undeliverable - link_send refuses it silently, so without this the symptom
		// would be a link that comes up, exchanges nothing, and blames the network.
		size_t payload = (packet > 0 && packet <= MAX_PACKET) ? packet : 0;
		if (packet > MAX_PACKET)
		{
			static bool said = false;
			if (!said)
			{
				said = true;
				// The module memcpys this length into the TX array whatever the host thinks, so a
				// size past the buffer is a corruption YAMP cannot prevent - only report.
				DebugLogFile("[%s link] guest programmed packet size %zu, past the %zu-byte bank. "
					"Not forwarding it; the module is still writing that much into the TX array.\n",
					gGeneral.GetGameTag(), packet, MAX_PACKET);
			}
		}
		else if (sizeof(WireHeader) + payload > MAX_WIRE)
		{
			static bool said = false;
			if (!said)
			{
				said = true;
				DebugLogFile("[%s link] guest programmed packet size %zu; with the %zu-byte header "
					"that is past the %zu-byte netplay channel, which would drop it silently. "
					"Sending flags only - this link cannot carry a packet this large.\n",
					gGeneral.GetGameTag(), packet, sizeof(WireHeader), MAX_WIRE);
			}
			payload = 0;
		}

		// SEND LAST FRAME'S TX SLOT. The board wrote it during the previous update stage, which is
		// why this runs before the module's frame rather than after: one packet per board frame,
		// taken at a point where the slot is not being written underneath us.
		{
			auto* header = reinterpret_cast<WireHeader*>(s_wire.data());
			header->magic = WIRE_MAGIC;
			header->node = s_nodeId;
			header->seq = static_cast<uint8_t>(s_frame);
			header->size = static_cast<uint16_t>(payload);
			header->boardSeq = haveBoard ? view.sequence : 0;
			header->flags = static_cast<uint8_t>(FLAG_READY
				| ((Rendezvous() & NodeMask(BIT_ACK, s_nodeId)) != 0 ? FLAG_ACK : 0)
				| ((Rendezvous() & NodeMask(BIT_BOOTED, s_nodeId)) != 0 ? FLAG_BOOTED : 0));
			if (payload > 0)
			{
				memcpy(s_wire.data() + sizeof(WireHeader),
					s_tx.data() + static_cast<size_t>(s_nodeId) * payload, payload);
			}
			net::LinkSend(s_wire.data(), static_cast<unsigned int>(sizeof(WireHeader) + payload));
			s_txPackets++;
		}

		// INGEST. LinkTake is newest-wins, so this drains what the plugin has and keeps the last
		// packet from each node.
		for (;;)
		{
			const unsigned int got = net::LinkTake(s_wire.data(),
				static_cast<unsigned int>(s_wire.size()));
			if (got < sizeof(WireHeader)) break;

			const auto* header = reinterpret_cast<const WireHeader*>(s_wire.data());
			if (header->magic != WIRE_MAGIC || header->node >= s_nodeCount
				|| header->node == s_nodeId
				|| header->size > MAX_PACKET
				|| got < sizeof(WireHeader) + header->size)
			{
				continue;   // a stale or corrupt datagram is dropped, never decoded
			}

			// ANY VALID DATAGRAM IS A SIGN OF LIFE, sized or not.
			//
			// This used to be inside the `size > 0` branch below, which tied "is the peer there" to
			// "has the peer's guest programmed a packet size yet" - two unrelated facts. A cabinet
			// still running its self-test sends header-only packets for hundreds of frames, and on
			// the old rule those did not refresh the staleness clock at all. It happened not to
			// matter only because the clock was never armed until the first sized packet arrived;
			// the moment either side legitimately went back to sending flags only, the peer's ready
			// bit would drop underneath a link that was working.
			s_lastRx[header->node] = s_frame;
			s_everRx[header->node] = true;

			// size 0 is legal and carries only the peer's flags - see the deadlock note above.
			if (header->size > 0)
			{
				// INDEXED BY *OUR* PACKET SIZE, NOT THE SENDER'S, because the module reads this
				// array as `rx + ourPacketSize * i`. The two agree in every healthy session - both
				// guests are the same ROM programming the same register - so using the sender's
				// worked and would have gone on working right up until they disagreed, at which
				// point the peer's data would land at an offset the board never reads and the
				// symptom would be a silent peer on a link with traffic on it. Copy no more than
				// one slot holds.
				const size_t slot = (payload > 0) ? payload : header->size;
				const size_t bytes = (header->size < slot) ? header->size : slot;
				memcpy(s_rx.data() + static_cast<size_t>(header->node) * slot,
					s_wire.data() + sizeof(WireHeader), bytes);
				s_rxPackets[header->node]++;
				s_peerBoardSeq[header->node] = header->boardSeq;
			}

			// The peer's own report, expanded into the groups the MODULE READS - 0x00 and 0x10.
			// Deliberately not into 0x20/0x30: those are the module's to write about itself, and a
			// host that scribbled another node's report into them would be inventing a claim the
			// module makes only about the board it is running.
			const uint64_t mask = NodeMask(BIT_READY, header->node)
				| NodeMask(BIT_LINKED, header->node);
			Rendezvous() &= ~mask;
			Rendezvous() |= ((header->flags & FLAG_READY) != 0 ? NodeMask(BIT_READY, header->node) : 0)
				| ((header->flags & FLAG_BOOTED) != 0 ? NodeMask(BIT_LINKED, header->node) : 0);
		}

		// MIRROR OUR OWN PACKET BACK INTO OUR OWN RX SLOT. The board ingests every node including
		// itself - the loop walks backwards from nodeId-1 and wraps, so it covers all nodeCount
		// slots - and a real token ring does hand a cabinet's packet back to it after a lap. This
		// is the faithful reading, but it IS a reading rather than a measurement: if it is wrong
		// the game sees itself twice, which is visible on screen and is listed as an open question
		// in docs/src2-netplay-recon.md.
		if (payload > 0)
		{
			memcpy(s_rx.data() + static_cast<size_t>(s_nodeId) * payload,
				s_tx.data() + static_cast<size_t>(s_nodeId) * payload, payload);
		}

		// Drop a peer whose packets have stopped, so the board waits rather than racing ahead on
		// a snapshot that is no longer arriving.
		for (uint8_t node = 0; node < s_nodeCount; ++node)
		{
			if (node == s_nodeId) continue;
			if (s_everRx[node] && s_frame - s_lastRx[node] > STALE_FRAMES)
			{
				Rendezvous() &= ~NodeMask(BIT_READY, node);
			}
		}

		LogState();
	}

	bool CommBoard::TraceEnabled()
	{
		static const bool wanted = []
		{
			char value[8];
			size_t length = 0;
			return getenv_s(&length, value, sizeof(value), "YAMP_PRE3_SYNCPROBE") == 0
				&& length != 0 && value[0] != '0';
		}();
		return wanted;
	}

	void CommBoard::SampleAtFrameBoundary()
	{
		if (!TraceEnabled()) return;

		const auto* ram = static_cast<const uint8_t*>(BoardGuestRam());
		if (ram == nullptr) return;

		uint8_t irqEnable = 0;
		uint16_t irqState = 0;
		if (!ReadIrq(irqEnable, irqState)) return;

		// The three ISR counters, so "which interrupts are being SERVICED" is answered directly
		// rather than inferred from the state word. A guest WORD at A is the host dword at ram+A
		// and the byte at the lowest guest address is that dword's TOP byte:
		//
		//     0x73798E  VBlank      (FUN_00001D80, IRQ bit 0x02)   <- what the linked board waits on
		//     0x737987  IRQ 0x08    (FUN_00001E10)
		//     0x7379A8  IRQ 0x04    (FUN_00001E78)
		const uint32_t w98C = *reinterpret_cast<const uint32_t*>(ram + 0x73798C);
		const uint32_t w984 = *reinterpret_cast<const uint32_t*>(ram + 0x737984);
		const uint32_t w9A8 = *reinterpret_cast<const uint32_t*>(ram + 0x7379A8);
		// THE GAME'S OWN NETWORK VERDICT, which beats anything the host can infer.
		//
		// FUN_00093DB4 is SRC2's network check, called unconditionally from the boot chain at
		// guest 0x18A1C, and it picks between four outcomes from two bits and one status word:
		//
		//     DAT_007379B9 & 0x20 == 0   ->  "NETWORK BOARD NOT PRESENT"
		//     DAT_007379B9 & 0x40 == 0   ->  "NETWORK BOARD HAS ANY PROBLEM"
		//     status & 0x800             ->  "NO CARRIER! CHECK NETWORK CABLE"
		//     otherwise                  ->  low 5 bits = node id, bits 5-9 = node count
		//
		// and its countdown loop calls FUN_0004C948 - the wait-for-flag helper our hung boards
		// spin inside. So these bytes say which of the game's own four diagnoses applies while
		// the screen that would print it is stuck mid-frame and never paints.
		const uint32_t w9B8 = *reinterpret_cast<const uint32_t*>(ram + 0x7379B8);
		const uint8_t netFlags = static_cast<uint8_t>(w9B8 >> 16);   // guest 0x7379B9
		const uint32_t w628 = *reinterpret_cast<const uint32_t*>(ram + 0x100628);

		const uint8_t vblank = static_cast<uint8_t>(w98C >> 8);    // guest 0x73798E
		const uint8_t irq08 = static_cast<uint8_t>(w984);          // guest 0x737987
		const uint8_t irq04 = static_cast<uint8_t>(w9A8 >> 24);    // guest 0x7379A8

		// RECORDED, NOT LOGGED. A DebugLogFile call per frame was expensive enough to change the
		// outcome it was measuring, so this writes one 20-byte struct and returns; DumpTrace() does
		// the I/O once, at teardown.
		BoardView view{};
		ReadBoard(view);

		Sample& out = s_trace[s_traceCount % TRACE_CAPACITY];
		out.frame = static_cast<uint32_t>(s_traceCount);
		out.irqEnable = irqEnable;
		out.irqState = irqState;
		out.vblank = vblank;
		out.irq08 = irq08;
		out.irq04 = irq04;
		out.netFlags = netFlags;
		out.nodes = w628;
		out.commState = view.state;
		out.sequence = view.sequence;
		s_traceCount++;
	}

	void CommBoard::DumpTrace()
	{
		if (s_traceCount == 0) return;

		const size_t total = s_traceCount;
		const size_t have = (total < TRACE_CAPACITY) ? total : TRACE_CAPACITY;
		const size_t first = total - have;

		// Did the VBlank counter EVER move? That is the whole question the hung board poses, and
		// it is one pass over the buffer rather than a judgement call over a log.
		unsigned vblankMoves = 0;
		for (size_t i = first + 1; i < total; ++i)
		{
			if (s_trace[i % TRACE_CAPACITY].vblank != s_trace[(i - 1) % TRACE_CAPACITY].vblank)
			{
				vblankMoves++;
			}
		}

		const Sample& last = s_trace[(total - 1) % TRACE_CAPACITY];
		DebugLogFile("[%s trace] %zu samples; VBlank counter moved %u times%s\n",
			gGeneral.GetGameTag(), have, vblankMoves,
			vblankMoves == 0 ? "  <== NEVER - the board is blocked on it" : "");

		// The game's own verdict, decoded rather than left as hex. These four are the outcomes
		// FUN_00093DB4 chooses between, and exactly one of them applies.
		const char* verdict =
			(last.netFlags & 0x20) == 0 ? "NETWORK BOARD NOT PRESENT" :
			(last.netFlags & 0x40) == 0 ? "NETWORK BOARD HAS ANY PROBLEM" :
			"gates OK (present, no problem)";
		DebugLogFile("[%s trace] final: net(7379B9)=%02X -> %s | nodes(100628)=%08X "
			"(id+1=%u count=%u) | irq=%02X/%04X vblank=%02X irq08=%02X irq04=%02X "
			"commstate=%u seq=%u\n",
			// FUN_00093DB4 writes `status & 0x1F` (node id + 1) and `(status >> 5) & 0x1F` (node
			// count) into the two low bytes of the word at guest 0x100628. A host dword read of a
			// guest word preserves the guest's value, so they come out as byte 0 and byte 1 - not
			// as the >>8/>>16 an earlier version of this line used, which printed "id+1=2 count=0"
			// for a word that plainly reads 0x201.
			gGeneral.GetGameTag(), last.netFlags, verdict, last.nodes,
			last.nodes & 0x1F, (last.nodes >> 8) & 0x1F,
			last.irqEnable, last.irqState, last.vblank, last.irq08, last.irq04,
			last.commState, last.sequence);

		// A thinned timeline: every 60th sample plus every change of the fields that matter.
		// Bounded so a wrapped 8192-sample buffer cannot produce thousands of lines.
		unsigned printed = 0;
		for (size_t i = first; i < total && printed < 40; ++i)
		{
			const Sample& c = s_trace[i % TRACE_CAPACITY];
			const bool changed = (i == first) || [&]
			{
				const Sample& p = s_trace[(i - 1) % TRACE_CAPACITY];
				return c.netFlags != p.netFlags || c.nodes != p.nodes
					|| c.commState != p.commState || c.irqState != p.irqState;
			}();
			if (!changed && (i - first) % 60 != 0) continue;
			printed++;
			DebugLogFile("[%s trace]   f=%u irq=%02X/%04X vb=%02X i08=%02X i04=%02X net=%02X "
				"nodes=%08X st=%u seq=%u\n",
				gGeneral.GetGameTag(), c.frame, c.irqEnable, c.irqState, c.vblank, c.irq08,
				c.irq04, c.netFlags, c.nodes, c.commState, c.sequence);
		}
	}

	bool CommBoard::ReadBoard(BoardView& out)
	{
		const uint8_t* comm = Board::Comm();
		if (comm == nullptr) return false;

		out.nodeId = *reinterpret_cast<const int32_t*>(comm + Board::COMM_NODE_ID);
		out.nodeCount = *reinterpret_cast<const int32_t*>(comm + Board::COMM_NODE_COUNT);
		out.bank = static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(comm + Board::COMM_BANK));
		out.command = *reinterpret_cast<const uint32_t*>(comm + Board::COMM_COMMAND);
		out.status = *reinterpret_cast<const uint32_t*>(comm + Board::COMM_STATUS);
		out.packetSize = *reinterpret_cast<const uint16_t*>(comm + Board::COMM_PACKET_SIZE);
		out.sequence = *reinterpret_cast<const uint16_t*>(comm + Board::COMM_SEQUENCE);
		out.state = static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(comm + Board::COMM_STATE));
		return true;
	}

	void CommBoard::LogState()
	{
		BoardView view{};
		if (!ReadBoard(view))
		{
			// On a heartbeat rather than once, and WITH THE MACHINE PHASE. A board held at the
			// boot barrier (phase 0xF) and a board whose comm offsets are wrong (phase 0x10, no
			// CXComm) are the same silence from the outside; the phase is the only thing that
			// separates them, and the first probe run had to work that out after the fact.
			static uint32_t lastSaid = 0;
			if (s_frame > 300 && s_frame - lastSaid >= 300)
			{
				lastSaid = s_frame;
				const int phase = BoardPhase();
				DebugLogFile("[%s link] f=%u no CXComm: machine phase=0x%X%s rendezvous=%016llX\n",
					gGeneral.GetGameTag(), s_frame, phase,
					phase == Board::MACHINE_RUNNING
						? " (RUNNING, so it is the rom+0x588 self-check that failed)"
						: " (still in bring-up - a stall here is the boot barrier)",
					static_cast<unsigned long long>(Rendezvous()));
			}
			return;
		}

		static uint32_t lastKey = UINT32_MAX;
		static uint32_t lastLogged = 0;
		const uint32_t key = (static_cast<uint32_t>(view.state) << 24)
			| (static_cast<uint32_t>(view.packetSize) << 8)
			| static_cast<uint32_t>(view.nodeCount & 0xFF);
		if (key == lastKey && s_frame - lastLogged < 120) return;
		lastKey = key;
		lastLogged = s_frame;

		// WHAT IS ACTUALLY IN THE TX SLOT, which is the difference between "the state machine
		// ticks" and "there is a packet to send". A board that reaches state 4 against a silent
		// peer looks identical either way from the state fields alone.
		// EVERY NODE'S SLOT, and how much of it MOVED since the last sample.
		//
		// One slot's byte count answers "is there a packet", which was the question while nothing
		// linked. It cannot answer the one a running race poses: the CPU cars are simulated by one
		// cabinet and carried to the other, so "the master is not sending car data" and "the slave
		// is not applying it" look identical from the master's own slot alone. Churn is the
		// discriminator - a slot that is large but static is a stale snapshot, a slot that is
		// small and busy is carrying inputs only.
		//
		// The guest programs 848 bytes and boot filled 34 of them, so the interesting number is
		// whether a race pushes that up into the hundreds.
		char slot[256] = "-";
		if (view.packetSize > 0 && static_cast<size_t>(view.packetSize) <= MAX_PACKET)
		{
			// WHICH ARRAY IS WORTH LOOKING AT DEPENDS ON THE TRANSPORT, and getting it wrong makes
			// this read as a dead link. Shared memory has ONE array, so TX and RX are the same
			// bytes and every node's slot is live in it. A real wire has two, and only THIS node's
			// slot is ever written in TX - so printing TX over a link would show the peer's slot
			// permanently empty however well the link was working. RX is where a peer's packet
			// actually lands, and it holds this cabinet's own mirrored slot too, so it carries
			// everything the TX view had.
			const uint8_t* base = (s_mode == Mode::Shared && s_ring != nullptr)
				? s_ring->packets
				: (s_mode == Mode::Link ? s_rx.data() : s_tx.data());

			static std::vector<uint8_t> previous;
			const size_t span = static_cast<size_t>(view.nodeCount ? view.nodeCount : 1)
				* view.packetSize;
			const bool haveHistory = previous.size() == span;

			int at = 0;
			for (int node = 0; node < view.nodeCount && node < MAX_NODES; ++node)
			{
				const size_t offset = static_cast<size_t>(node) * view.packetSize;
				const uint8_t* p = base + offset;
				size_t nonZero = 0;
				size_t changed = 0;
				for (size_t i = 0; i < view.packetSize; ++i)
				{
					if (p[i] != 0) nonZero++;
					if (haveHistory && p[i] != previous[offset + i]) changed++;
				}
				at += _snprintf_s(slot + at, sizeof(slot) - at, _TRUNCATE,
					"%s[%d]%zu nz/%zu moved%s", at != 0 ? " " : "", node, nonZero, changed,
					node == s_nodeId ? "*" : "");
			}
			previous.assign(base, base + span);
		}

		uint8_t irqEnable = 0;
		uint16_t irqState = 0;
		ReadIrq(irqEnable, irqState);

		// THE WIRE'S OWN COUNTERS, and only on the transports that have one. Printed next to the
		// board's fields rather than in a line of their own so one log line answers the whole
		// question: the board's state machine, what this cabinet put on the wire, what came back,
		// and whether the peer's BOARD (not merely its process) is transferring.
		char wire[96] = "";
		if (s_mode == Mode::Link)
		{
			int at = _snprintf_s(wire, sizeof(wire), _TRUNCATE, " wire=%s tx=%u rx=",
				net::LinkReady() ? "up" : "NO PEER", s_txPackets);
			for (uint8_t node = 0; node < s_nodeCount; ++node)
			{
				if (node == s_nodeId) continue;
				at += _snprintf_s(wire + at, sizeof(wire) - at, _TRUNCATE, "[%u]%u/bseq%u",
					node, s_rxPackets[node], s_peerBoardSeq[node]);
			}
		}

		DebugLogFile("[%s link] f=%u state=%u node=%d/%d size=%u seq=%u bank=%u cmd=%04X "
			"status=%04X rendezvous=%016llX irq=%02X/%04X%s tx=%s\n",
			gGeneral.GetGameTag(), s_frame, view.state, view.nodeId, view.nodeCount,
			view.packetSize, view.sequence, view.bank, view.command, view.status,
			static_cast<unsigned long long>(Rendezvous()), irqEnable, irqState, wire, slot);
	}
}
