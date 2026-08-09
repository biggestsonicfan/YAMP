#pragma once

#include <cstddef>
#include <cstdint>

namespace m2ftg
{
	// The Model 2 comm board's DPRAM model, shared by the modules that emulate it: the mr and
	// omg DLLs ship the SAME board emulation - two banks of comm RAM behind a register pair -
	// so everything here is a property of the BOARD, not of a game. Where each game's comm
	// BLOCKS live in its module, what the slots inside a bank mean, and who performs the
	// per-frame transfer stay per-game (MrLink.cpp and K2Host.cpp), because those are
	// properties of the module build and of the ROM's packet - and the two games answer the
	// transfer question in OPPOSITE ways on purpose (see MrLink.h).
	namespace CommBoard
	{
		// A comm block is two banks of comm RAM followed by the register pair the guest reaches
		// through its 0x1A14000 window.
		inline constexpr size_t BANK      = 0x4000;
		inline constexpr size_t REG_RESET = 0x8000;   // bit0: 0 = held in reset, 1 = released
		inline constexpr size_t REG_FLAG  = 0x8001;   // bit0 = bank select, bit7 = data ready
		inline constexpr uint8_t FLAG_BANK  = 0x01;
		inline constexpr uint8_t FLAG_READY = 0x80;

		// Firmware status bytes at the front of each bank. On hardware the board's own firmware
		// fills them once the serial ring is healthy; the modules have no firmware, so the host
		// says it (WriteFirmwareStatus below).
		inline constexpr size_t STATUS_RING       = 0;   // 1 while peer datagrams flow
		inline constexpr size_t STATUS_NODE_ID    = 2;
		inline constexpr size_t STATUS_NODE_COUNT = 3;

		// The bank the flag register currently selects, as a byte offset into the block.
		inline size_t CurrentBank(const uint8_t* block)
		{
			return (block[REG_FLAG] & FLAG_BANK) ? BANK : 0;
		}

		// The ROM's own "I released the board" - reset bit0 (both ROMs' checks: zero the
		// registers, write reset = 1, then poll the flag). While the board is held in reset the
		// firmware is not booted: no status, no exchange, no transmission - which is also what
		// keeps "the peer is up" meaning "the peer's ROM reached its check", not "the other
		// process started" (Virtual On's boot-flap lesson, cited verbatim by Motor Raid).
		inline bool ReleasedFromReset(const uint8_t* block)
		{
			return (block[REG_RESET] & 1) != 0;
		}

		// Peer-packet freshness - the health half of DPRAM residency. Both games hold the
		// newest peer packet resident (a DPRAM keeps its contents between arrivals, and
		// re-laying it costs nothing) and answer "is the ring up" from how recently a datagram
		// actually LANDED: residency alone is not enough, because each ROM's watchdog wants its
		// neighbour's alive counter to ADVANCE - only fresh packets keep the ring healthy, and
		// a quiet peer errors out through the ROM's own accounting, which is the correct
		// linked-cabinet behaviour.
		//
		// Age() once per HOST frame, wherever the game turns its host frame over - a timeout
		// counted in module_main calls would be of unknown, machine-dependent length (one host
		// frame is one to sixteen of them during boot).
		inline constexpr int LINK_TIMEOUT_FRAMES = 30;

		class LinkHealth
		{
		public:
			// A peer datagram landed: the resident packet is fresh and the ring has a live
			// neighbour.
			void NotePacket()
			{
				m_have = true;
				m_sinceRecv = 0;
			}

			// One host frame passed. Saturates just past the timeout, so a cabinet that sat
			// alone for an hour recovers the instant a peer appears.
			void Age()
			{
				if (m_sinceRecv <= LINK_TIMEOUT_FRAMES)
				{
					++m_sinceRecv;
				}
			}

			bool HavePacket() const { return m_have; }
			bool Fresh() const { return m_sinceRecv <= LINK_TIMEOUT_FRAMES; }

		private:
			// Born stale: nothing is fresh until a packet lands.
			int m_sinceRecv = LINK_TIMEOUT_FRAMES + 1;
			bool m_have = false;
		};

		// The firmware's answer, truthfully, into BOTH banks of a comm block. The modules' own
		// boot responses write these bytes the same way and their transfers never touch them,
		// so nothing here fights the module. Logs the ring edge through net::Logf as
		// "<tag>: ring UP/DOWN, node id N of M", naming the ROM's wait screen while it is down
		// - while the ring is down the ROM parks at its ring-up poll on that screen, which is
		// correct behaviour, not a failure to paper over.
		void WriteFirmwareStatus(uint8_t* block, bool ringUp, uint8_t nodeId, uint8_t nodeCount,
			const char* tag, const char* waitScreen);
	}
}
