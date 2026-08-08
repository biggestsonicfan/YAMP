#include "CommBoard.h"

#include "../YAMPGeneral.h"
#include "../DebugLog.h"
#include "../net/NetPlugin.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
			// TaskM3E, the emulator object — see Determinism.h. Its +0x90 is the ROM object, the
			// same global module_main independently reads for the coin latch.
			inline constexpr uintptr_t RVA_MACHINE = 0x1895D0;
			inline constexpr size_t MACHINE_ROM = 0x90;
			inline constexpr size_t MACHINE_PHASE = 0x88;
			inline constexpr int MACHINE_RUNNING = 0x10;

			// The CXComm subobject inside M3ERomSrc2, from the ROM factory's
			// `plVar5[0xb1] = CXComm::vftable` (DLL 0x180038BCF). Spikeout's sits at 0x5E0, which
			// is why this is not a shared constant: it is per game, and only one of the two games
			// that have one is reachable from a stock install.
			inline constexpr size_t ROM_COMM = 0x588;

			// The module's own CXComm vtable. Used ONLY as a self-check — see ReadBoard.
			inline constexpr uintptr_t RVA_CXCOMM_VTABLE = 0x10C858;

			// Fields inside CXComm, all read off its seven methods and FUN_180035BA0.
			inline constexpr size_t COMM_NODE_ID = 0x20008;
			inline constexpr size_t COMM_NODE_COUNT = 0x2000C;
			inline constexpr size_t COMM_BANK = 0x20010;
			inline constexpr size_t COMM_COMMAND = 0x20018;
			inline constexpr size_t COMM_STATUS = 0x2001C;
			inline constexpr size_t COMM_PACKET_SIZE = 0x20024;
			inline constexpr size_t COMM_SEQUENCE = 0x20026;
			inline constexpr size_t COMM_STATE = 0x20028;

			static const uint8_t* ModuleBase()
			{
				return reinterpret_cast<const uint8_t*>(
					GetModuleHandleW(L"pre3-pxd-w64-d3d12_retail.dll"));
			}

			// The machine's bring-up phase, or -1 when the module is not loaded. 0x10 is running;
			// a link that stalls the BOOT BARRIER parks it at 0xF. Telling those two apart is the
			// difference between "the host never delivered a rendezvous bit" and "the comm offsets
			// are wrong" - a distinction the first probe run needed and did not have.
			static int Phase()
			{
				const uint8_t* base = ModuleBase();
				if (base == nullptr) return -1;
				return *reinterpret_cast<const int*>(base + RVA_MACHINE + MACHINE_PHASE);
			}

			// The CXComm, or null when the machine is not up, the game is not SRC2, or the object
			// at rom+0x588 does not carry the vtable it should.
			static const uint8_t* Comm()
			{
				if (gGeneral.GetGameId() != YAMPGeneral::GameId::SRC2) return nullptr;

				const uint8_t* base = ModuleBase();
				if (base == nullptr) return nullptr;

				const uint8_t* machine = base + RVA_MACHINE;
				if (*reinterpret_cast<const int*>(machine + MACHINE_PHASE) != MACHINE_RUNNING)
				{
					return nullptr;
				}
				const uint8_t* rom = *reinterpret_cast<const uint8_t* const*>(machine + MACHINE_ROM);
				if (rom == nullptr) return nullptr;

				const uint8_t* comm = rom + ROM_COMM;
				// THE SELF-CHECK. A wrong offset here reads plausible small integers out of
				// whatever else lives in a 0x20610-byte object, and "state 0, size 0" is exactly
				// what a board that has not linked looks like — so the failure would be invisible
				// and would be believed. The vtable pointer is the known-fixed value.
				if (*reinterpret_cast<const uint8_t* const*>(comm) != base + RVA_CXCOMM_VTABLE)
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

		// ---- the rendezvous word ---------------------------------------------------------
		//
		// Four bit groups in one u64, and they are DIRECTIONAL - see CommBoard.h. The low two are
		// what the module waits on and the host must drive; the high two are what the module
		// reports about itself.
		inline constexpr unsigned BIT_READY = 0x00;    // host->module: node i's packet is in RX
		inline constexpr unsigned BIT_LINKED = 0x10;   // host->module: node i has booted
		inline constexpr unsigned BIT_ACK = 0x20;      // module->host: the guest's 0xF000 answer
		inline constexpr unsigned BIT_BOOTED = 0x30;   // module->host: this node has booted

		static uint64_t NodeMask(unsigned group, uint8_t node)
		{
			return 1ull << ((group + node) & 63);
		}

		// How long a peer's packet stays "fresh" enough to hold its ready bit up.
		//
		// A DELIBERATE POLICY RATHER THAN A READING OF THE MODULE, and the first thing to revisit
		// with two machines. Nothing in the module ever CLEARS a ready bit, so holding them all
		// set makes the board free-run and holding them strictly per-frame makes it lockstep; the
		// truth is whatever Gaiden's own host does, which cannot be read from here. This picks the
		// middle: a peer's bit stays up while its packets keep arriving and drops when they stop,
		// so jitter is absorbed and a dead peer stalls the board rather than being simulated as a
		// silent one. Six frames is ~100 ms at 60 Hz.
		inline constexpr uint32_t STALE_FRAMES = 6;

		static uint32_t s_frame = 0;
		static uint32_t s_lastRx[MAX_NODES] = {};
		static bool s_everRx[MAX_NODES] = {};

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
#pragma pack(push, 1)
		struct WireHeader
		{
			uint8_t magic;      // 'C', so a stale ABI's datagram is dropped rather than decoded
			uint8_t node;
			uint8_t flags;      // bit0 ready, bit1 ack, bit2 booted
			uint8_t reserved;
			uint16_t size;
		};
#pragma pack(pop)
		static_assert(sizeof(WireHeader) == 6);

		inline constexpr uint8_t WIRE_MAGIC = 'C';
		inline constexpr uint8_t FLAG_READY = 0x01;
		inline constexpr uint8_t FLAG_ACK = 0x02;
		inline constexpr uint8_t FLAG_BOOTED = 0x04;

		static std::vector<uint8_t> s_wire;

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

		if (!ParseEnvironment())
		{
			return;
		}

		EnsureBuffers();
		DebugLogFile("[%s link] mode=%s node=%u/%u (config +0x100C/+0x1010)\n",
			gGeneral.GetGameTag(), s_mode == Mode::Probe ? "PROBE (no peer)" : "LINK",
			s_nodeId, s_nodeCount);
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
		EnsureBuffers();
		workPointers[0] = s_tx.data();
		workPointers[1] = s_rx.data();
		workPointers[2] = &s_rendezvous;
	}

	void CommBoard::Update()
	{
		if (s_mode == Mode::Off) return;
		EnsureBuffers();
		s_frame++;

		BoardView view{};
		const bool haveBoard = ReadBoard(view);
		const size_t packet = haveBoard ? view.packetSize : 0;

		// Our own ready bit is unconditional: the board fills its TX slot inside its own frame, so
		// from the host's point of view this node always has a packet to offer. What the peers see
		// is a different question and is answered by the wire below.
		s_rendezvous |= NodeMask(BIT_READY, s_nodeId);

		// Promote our own boot REPORT into the group the module reads back. This is the one place
		// the two directions meet for a single cabinet, and it is why the groups are separate at
		// all: the module says "I am up" in 0x30 and asks "is everyone up?" of 0x10, so even the
		// local node's answer passes through the host.
		if ((s_rendezvous & NodeMask(BIT_BOOTED, s_nodeId)) != 0)
		{
			s_rendezvous |= NodeMask(BIT_LINKED, s_nodeId);
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
				s_rendezvous |= NodeMask(BIT_READY, node) | NodeMask(BIT_LINKED, node);
			}
			LogState();
			return;
		}

		// ---- Link -----------------------------------------------------------------------
		if (!net::LinkReady())
		{
			// No peer yet. Leave the peers' bits clear: the board then waits, which is the correct
			// behaviour and is the ROM's own.
			LogState();
			return;
		}

		// A PACKET SIZE OF ZERO IS NOT A REASON NOT TO SEND, and this is a deadlock if it is:
		// the guest cannot program a size until its board is running, the board cannot run until
		// the boot barrier releases, and the barrier cannot release until each peer has heard that
		// the other has booted - which is carried in this datagram's flags. So the header always
		// goes out; the payload is whatever there is, which early on is nothing.
		const size_t payload = (packet > 0 && packet <= MAX_PACKET) ? packet : 0;
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

		// SEND LAST FRAME'S TX SLOT. The board wrote it during the previous update stage, which is
		// why this runs before the module's frame rather than after: one packet per board frame,
		// taken at a point where the slot is not being written underneath us.
		{
			auto* header = reinterpret_cast<WireHeader*>(s_wire.data());
			header->magic = WIRE_MAGIC;
			header->node = s_nodeId;
			header->reserved = 0;
			header->size = static_cast<uint16_t>(payload);
			header->flags = static_cast<uint8_t>(FLAG_READY
				| ((s_rendezvous & NodeMask(BIT_ACK, s_nodeId)) != 0 ? FLAG_ACK : 0)
				| ((s_rendezvous & NodeMask(BIT_BOOTED, s_nodeId)) != 0 ? FLAG_BOOTED : 0));
			if (payload > 0)
			{
				memcpy(s_wire.data() + sizeof(WireHeader),
					s_tx.data() + static_cast<size_t>(s_nodeId) * payload, payload);
			}
			net::LinkSend(s_wire.data(), static_cast<unsigned int>(sizeof(WireHeader) + payload));
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

			// size 0 is legal and carries only the peer's flags - see the deadlock note above.
			if (header->size > 0)
			{
				memcpy(s_rx.data() + static_cast<size_t>(header->node) * header->size,
					s_wire.data() + sizeof(WireHeader), header->size);
				s_lastRx[header->node] = s_frame;
				s_everRx[header->node] = true;
			}

			// The peer's own report, expanded into the groups the MODULE READS - 0x00 and 0x10.
			// Deliberately not into 0x20/0x30: those are the module's to write about itself, and a
			// host that scribbled another node's report into them would be inventing a claim the
			// module makes only about the board it is running.
			const uint64_t mask = NodeMask(BIT_READY, header->node)
				| NodeMask(BIT_LINKED, header->node);
			s_rendezvous &= ~mask;
			s_rendezvous |= ((header->flags & FLAG_READY) != 0 ? NodeMask(BIT_READY, header->node) : 0)
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
				s_rendezvous &= ~NodeMask(BIT_READY, node);
			}
		}

		LogState();
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
				const int phase = Board::Phase();
				DebugLogFile("[%s link] f=%u no CXComm: machine phase=0x%X%s rendezvous=%016llX\n",
					gGeneral.GetGameTag(), s_frame, phase,
					phase == Board::MACHINE_RUNNING
						? " (RUNNING, so it is the rom+0x588 self-check that failed)"
						: " (still in bring-up - a stall here is the boot barrier)",
					static_cast<unsigned long long>(s_rendezvous));
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
		char slot[64] = "-";
		if (view.packetSize > 0 && static_cast<size_t>(view.packetSize) <= MAX_PACKET)
		{
			const uint8_t* mine = s_tx.data() + static_cast<size_t>(s_nodeId) * view.packetSize;
			size_t nonZero = 0;
			for (size_t i = 0; i < view.packetSize; ++i) nonZero += (mine[i] != 0) ? 1 : 0;
			sprintf_s(slot, "%zu/%u nz %02X%02X%02X%02X%02X%02X%02X%02X",
				nonZero, view.packetSize, mine[0], mine[1], mine[2], mine[3],
				mine[4], mine[5], mine[6], mine[7]);
		}

		DebugLogFile("[%s link] f=%u state=%u node=%d/%d size=%u seq=%u bank=%u cmd=%04X "
			"status=%04X rendezvous=%016llX tx=%s\n",
			gGeneral.GetGameTag(), s_frame, view.state, view.nodeId, view.nodeCount,
			view.packetSize, view.sequence, view.bank, view.command, view.status,
			static_cast<unsigned long long>(s_rendezvous), slot);
	}
}
