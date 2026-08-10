#include "K2Host.h"
#include "../../ModuleLoad.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ImportSymbols.h"

#include "../../criware/Cri.h"
#include "../../pxd/LJ/sl.h"            // pxd::sl — the head of the context, and the primitives
#include "../../pxd/K2/sl.h"            // pxd::K2 — THIS generation's context layout (0xF3C0)
#include "../../pxd/LJ/sl_internal.h"   // handle_internal_buffer_t (the 8-byte queue node)
#include "../m2ftg.h"                 // m2ftg_config_t (0x100C) — unchanged in this generation
#include "../Determinism.h"           // RomFrameCounterAddress / ReadEmulatedRam32
#include "../VirtualClock.h"          // the module's timebase, driven by frames not real time
#include "../Debug/HleHooks.h"              // the HLE trap table, disabled by repointing its handlers
#include "../CommBoard/CommBoard.h"   // the Model 2 comm board's DPRAM model, shared with MrLink
#include "../../cabinet/Cabinet.h"    // the shared cabinet front panel (pause/coin/assign/switches)
#include "../SystemSwitches.h"        // cabinet TEST / SERVICE on the emulated I/O board
#include "../../net/NetPlugin.h"      // net::Logf — the diagnostic sink both peers share
#include "../ModuleArgs.h"
#include "../DisplayModes.h"
#include "../../input/Input.h"
#include "../HostUI.h"
#include "../../DebugLog.h"
#include "../../YAMPGeneral.h"
#include "../../GameVerify.h"
#include "../../Utils/MemoryMgr.h"       // VP::Patch — protection-aware writes into module .text
#include "../../Utils/ScopedUnprotect.hpp"
#include "../../Utils/Trampoline.h"
#include "../../wil/resource.h"

#include "VonBoard.h"

namespace m2ftg
{
	namespace K2
	{
		using namespace pxd;

		// ---- Comm-board RESET semantics ------------------------------------------------------
		//
		// The one piece of comm-board firmware the module does not model, and the cause of the
		// hard hang on leaving the debug/test menu with the link running.
		//
		// The ROM's link check (`Net_check`, i960 0xC5870 - decoded in docs/von-netplay-recon.md)
		// drives the comm board through its two registers exactly as the hardware manual would
		// have it: zero both registers (hold the board in reset), wait a second, write
		// CommBoardReset = 1 (release it), then poll CommFlagReg until bit7 - data ready - reads
		// 0, which on real hardware is the firmware acknowledging its boot. Only then does it
		// wait for bit7 to SET (the first transfer), write its link_ID into comm RAM, and poll
		// the firmware's status bytes (byte0 = ring up, byte3 = node count == 2).
		//
		// The module's link transfer sets bit7 on BOTH flag registers every frame regardless of
		// the reset registers - the retail Kiwami 2 cabinet is standalone and nothing ever ran
		// this path - so the "wait for bit7 to clear" poll can never terminate once the link is
		// enabled. And that poll is a tight four-instruction i960 loop with no frame yield, so
		// the CPU step never returns, module_main never returns, and the whole host spins.
		//
		// Cold boot escapes by ORDERING alone: Net_check runs before the module enables the link,
		// so no transfer interferes and the check completes (which is why net_flag reads 1 in a
		// healthy linked run). The debug menu's exit re-runs the check with the link live and
		// hung the machine - MEASURED 2026-08-04 on a live process: both boards frozen, GlobCntr
		// static, board 0 spinning at Net_check+0x64 with reset=01/flag=81. Clearing bit7 by
		// hand un-froze it, "THIS IS MASTER SITE" / "SLAVE SITE" printed, and the pair
		// re-linked (net_flag -> 1 on both boards, MainMode -> 1).
		//
		// So YAMP models the missing semantics at the link transfer, the module's own once-per-
		// frame comm-board tick: a board whose reset register the ROM has dropped to 0 stops
		// driving its flag register until the ROM releases it. Armed only after the register has
		// been SEEN at 1 (the ROM's own release during its boot-time check), so a cold boot - or
		// any run where the check never re-runs - behaves exactly as before this existed.
		void (*g_origLinkTransfer)() = nullptr;
		// Defined with the rest of the link, driven from the shim below - it hands the peer's packet
		// to the module's OWN transfer as board 1's outgoing packet, so it has to be in place BEFORE
		// that transfer runs.
		static void StageCommPayload();
		namespace CommReset
		{
			// Register RVAs per comm block: reset at +0x8000, flag at +0x8001 (see OmgRva).
			constexpr size_t RESET[2] = { OmgRva::COMM_P1_FLAG - 1, OmgRva::COMM_P2_FLAG - 1 };
			constexpr size_t FLAG[2]  = { OmgRva::COMM_P1_FLAG,     OmgRva::COMM_P2_FLAG };
			static bool s_seenOn[2];
			static bool s_armed[2];
			// How many times the module's OWN link transfer ran. It is called from the frame driver
			// only while linking is enabled, so "does it run at all on a one-board cabinet" is a
			// question with two very different answers - and if it does, it is a writer into the
			// same CommData window YAMP fills from the wire.
			static uint32_t s_transfers;
		}

		void LinkTransferWithResetSemantics()
		{
			// Reassert the two-board gate HERE, not just once per host frame. This shim runs
			// inside the frame driver before either board's CPU steps, and every consumer of the
			// gate - the board-1 branch of the driver, and hook 7's MASTER/SLAVE assignment that
			// InitNetwork reads - executes after it in the same call. A per-host-frame reassert
			// is not enough on its own: StepOneBoardFrame can run many module_main calls per
			// host frame while the counter holds (all of boot), and the module's own bring-up
			// zeroes the gate mid-burst - if the ROM's InitNetwork then runs before the next
			// host frame, it reads gate=0 and assigns NOLINK, and the machine comes up
			// standalone. Measured live 2026-08-04.
			if (g_twoBoardGate != nullptr && WantTwoBoardMode())
			{
				*g_twoBoardGate = 1;
			}

			// PRE: sample the reset registers before the transfer touches the flags. The arm /
			// disarm transitions are logged - they happen a handful of times per link check, and
			// a silent device model is how the original bug survived a whole session.
			for (int b = 0; b < 2; b++)
			{
				const bool released = (*(g_dllBase + CommReset::RESET[b]) & 1) != 0;
				if (released) CommReset::s_seenOn[b] = true;
				const bool arm = CommReset::s_seenOn[b] && !released;
				if (arm != CommReset::s_armed[b])
				{
					CommReset::s_armed[b] = arm;
					net::Logf("comm board %d %s", b,
						arm ? "held in RESET by the ROM - flag register pinned low"
						    : "released from reset");
				}
			}

			// The peer's packet goes in BEFORE the module's transfer, which is what then delivers it.
			StageCommPayload();

			CommReset::s_transfers++;
			g_origLinkTransfer();

			// POST: a comm board held in reset drives nothing, so undo what this frame's
			// transfer did to its registers. The ROM's release (writing 1) happens inside the
			// following CPU step - after this ran - which is exactly what lets its "bit7 must
			// read 0 after release" poll pass on the first read instead of spinning forever.
			for (int b = 0; b < 2; b++)
			{
				if (CommReset::s_armed[b]) *(g_dllBase + CommReset::FLAG[b]) = 0;
			}
		}

		// Non-zero byte count, so "did anything ever land here" is one number.

		// ---- HOLD THE RING DOWN until a linked cabinet is actually there ----------------------
		//
		// WHY THIS IS NOT AN HLE HOOK, which is the question that led here. The ROM's link check
		// (`Net_check`, i960 0xC5870) runs from `BlackOut+0x13C`, BEFORE the mainloop and therefore
		// before the warning screen - and NOTHING hooks it. Every hook in that address range is
		// Inert. The check nevertheless succeeds on a machine with one board and no peer, and the
		// comm board says why:
		//
		//     [comm] B0 reset=01 flag=00 bank0=01000102 bank1=01010102
		//                                       ^^  ^^^^
		//                                    byte0=1  byte2=1 byte3=2
		//
		// Those are the FIRMWARE status bytes - ring up, node id, node count. The ROM only ever
		// READS them; on hardware the comm board's own firmware writes them once the serial ring is
		// healthy. The module has no firmware, and what it leaves there is a fake healthy TWO-NODE
		// ring. So `Net_check` polls "is the ring up?" (yes), "are there two nodes?" (yes) and
		// declares success against a wire that does not exist.
		//
		// WHAT THE MODULE'S "FIRMWARE" ACTUALLY IS, found with a hardware write watchpoint on the
		// byte (there are no xrefs - the block is reached through a pointer). `FUN_18006A790` is the
		// comm board's REGISTER-WRITE handler, and it responds to the ROM releasing the board from
		// reset by faking a firmware boot:
		//
		//     6A7D1  cmp   dword [r8+0x8004], 0    ; "already booted" latch
		//     6A7DF  test  byte  [r8+0x8000], 1    ; CommBoardReset bit0 - released?
		//     6A7ED  movzx ecx, byte [0x6911B4]    ; the BOARD INDEX
		//     6A7FF  inc   cl                      ; node id = board index + 1
		//     6A818  mov   byte [rax+r8], 1        ; byte0 = 1  RING UP      ] and again for
		//     6A81D  mov   byte [rax+r8+2], cl     ; byte2 = node id         ] the other bank
		//     6A822  mov   byte [rax+r8+3], 2      ; byte3 = 2  NODE COUNT   ]
		//
		// Ring-up and node count are IMMEDIATES: it declares a healthy two-node ring with nothing
		// attached. That is the whole reason the boot-time link check has always succeeded.
		//
		// TWO CONSEQUENCES SHAPE THIS FUNCTION.
		//
		// 1. It writes the status ONCE, at release, and never again - the latch at +0x8004 sees to
		//    that. Real firmware reports continuously, because a ring can go DOWN. So this runs
		//    every call rather than seeding once, and that is measured, not defensive: writing the
		//    zero only on the first call leaves `bank0=01000102` by the first sample and the board
		//    boots straight through, because the ROM resets and releases the comm board on every
		//    `Net_check` and every `BlackOut` re-runs `Net_check`.
		//
		// 2. NODE ID COMES FROM THE BOARD INDEX, which is wrong for us. That was fine for the
		//    module's own two-local-boards arrangement, but one board per machine means the index
		//    is 0 on BOTH peers, so both would claim node id 1 - and step 12 of the check copies
		//    byte 2 into guest 0x5770D1. Node id here comes from the CABINET ROLE instead. It is a
		//    different value from `link_ID` (0x5770B1, which the cabinet-role backup byte drives)
		//    and the two have to agree.
		//
		// The ROM's reaction to a ring that is down is exactly what a pair of cabinets needs: it
		// sits in the check's ring-up poll showing "Checking Network Now", which YIELDS THROUGH
		// `synch` every frame - so module_main keeps returning and the host stays responsive. Do
		// NOT express "no link" by withholding flag bit7 instead: that poll is four instructions
		// with no frame yield and hangs the process (it is what the reset-semantics shim exists to
		// avoid), which is also why this deliberately leaves the flag register alone.
		//
		// Both banks, because bit0 of the flag register decides which one the i960 is reading and
		// it flips under us. Board 0 only: board 1, when it exists, is the local second cabinet and
		// its ring is the module's own business.

		struct CommLink
		{
			bool peerUp;        // are the peer's datagrams arriving right now?
			uint8_t nodes;      // how many cabinets are on the ring, including this one
		};

		// THE LINK RUNS OVER THE RPCN SESSION'S P2P SOCKET, and that is the only transport.
		//
		// A direct address:port harness lived here while the comm-board firmware model was being
		// brought up, because RPCN cannot do loopback - it hardcodes 3658 in the address it hands a
		// peer - so two instances on one machine were the only way to exercise the link at all. It
		// did that job and is gone: RPCN is the protocol this ships on, and a second transport that
		// nothing tests is a second set of behaviours to keep true.
		static bool LinkAvailable()
		{
			return net::LinkReady();
		}

		// Peer-packet freshness - the shared DPRAM residency model (CommBoard::LinkHealth),
		// aged once per HOST frame in LogLinkState.
		static CommBoard::LinkHealth s_linkHealth;

		// "Is a linked partner exchanging with us RIGHT NOW?" Cheap and side-effect free, unlike
		// ObserveLink, which also opens the harness transport - so this is what callers outside
		// the firmware path (the frame pacer) ask.
		bool LinkIsLive()
		{
			if (s_cabinetRole == 0)
			{
				return false;
			}
			return LinkAvailable() && s_linkHealth.Fresh();
		}

		// THE ONE FUNCTION THE TRANSPORT HAS TO REPLACE. Everything else below is the firmware's
		// side of the wire and does not care where the answer comes from.
		static CommLink ObserveLink()
		{
			const bool up = LinkIsLive();
			return CommLink{ up, static_cast<uint8_t>(up ? 2 : 1) };
		}

		// The ROM's own acceptance test, applied to a packet off the wire. OBSERVABILITY, not a
		// filter: an unstamped packet is passed to the comm RAM exactly like a stamped one, because
		// it is not corruption. It is what a cabinet sends before its `net_flag` goes up - the ROM
		// does not stamp anything until the vsync path at 0x14A0 runs, so a zeroed CommSend fails
		// this (0x0000 against 0xAE5E) throughout both cabinets' link checks. That traffic is what
		// brings the RING up; only its content is meaningless, and the peer's ROM discards it for
		// itself one frame later.
		//
		// What the count is FOR: it separates "datagrams are arriving" from "the peer's ROM is past
		// its link check and stamping real packets", and that distinction is the one the last
		// two-machine run could not make.
		static bool LinkPacketStamped(const uint8_t* packet)
		{
			uint16_t seq = 0, check = 0;
			memcpy(&seq, packet + OmgRva::PKT_SEQ, sizeof(seq));
			memcpy(&check, packet + OmgRva::PKT_CHECK, sizeof(check));
			return check == static_cast<uint16_t>(seq ^ OmgRva::PKT_CHECK_XOR);
		}

		static uint32_t s_pktStamped = 0;    // packets off the wire the ROM will accept
		static uint32_t s_pktUnstamped = 0;  // ...and ones it will not (pre-link-up traffic)

		// The newest packet off the wire, held resident. NOT consumed on delivery: the module's own
		// link transfer overwrites this cabinet's CommData every frame (with board 1's send buffer,
		// which on a one-board machine is 0x700 zero bytes), so a packet written once and left
		// alone survives exactly until the next `module_main`. Keeping the last one and re-laying it
		// after every transfer is what a real DPRAM does between arrivals, and it is what makes a
		// dropped datagram cost nothing.
		static uint8_t s_peerPacket[OmgRva::COMM_PAYLOAD];

		// Counts datagrams actually put on the wire, so the send-rate change is measurable rather
		// than asserted. Reset per host frame by LogLinkState.
		static uint32_t s_linkSends = 0;
		static bool s_sentThisHostFrame = false;
		static void LinkSendMark() { ++s_linkSends; s_sentThisHostFrame = true; }

		// The SEND half: our CommSend onto the wire. Runs before module_main, which is where the
		// bank it reads comes from - MEASURED, not assumed. Both banks of CommSend hold a valid
		// stamped packet at any time, and the one matching the counter the ROM stamped into cSend
		// is bank `flag & 1`:
		//
		//   flag=01  send0=113D/BF63  send1=DD60/733E  tx=DD60      <- bank 1
		//   flag=00  send0=FAE0/54BE  send1=F060/5E3E  tx=FAE0      <- bank 0
		//
		// which is simply the ROM's vsync path doing its send and receive memcpys back to back
		// through the same guest window, so both land in whichever bank is current.
		static void ExchangeCommPayload()
		{
			if (g_dllBase == nullptr)
			{
				return;
			}
			uint8_t* const block = g_dllBase + OmgRva::COMM_P1;
			const size_t front = CommBoard::CurrentBank(block);

			// ONLY TRANSMIT ONCE OUR OWN COMM BOARD HAS BEEN RELEASED FROM RESET.
			//
			// Without this, "the peer is up" means no more than "the other YAMP process is sending
			// datagrams", which is true from its first emulated frame - long before its ROM reaches
			// `Net_check`. Observed: the instant the slave booted, the MASTER's ring came up, its
			// check completed and it walked off into the warning screen while the slave was still
			// booting. The two cabinets end up at completely different points in their boot
			// sequences, and every flap of that signal re-runs somebody's check.
			//
			// `CommBoardReset` bit0 is the ROM's own "I have released the board" - step 5 of the
			// check, and the same edge the module's firmware stub keys its fake boot off. Gating
			// transmission on it makes peer liveness mean what it means on hardware: the other
			// cabinet's CPU has brought its comm board up and is participating in the ring.
			if (!CommBoard::ReleasedFromReset(block))
			{
				return;
			}
			const uint8_t* const outgoing = block + 0x2000 + front;

			// ONE PACKET OUT AND ONE PACKET IN PER BOARD FRAME, NOT PER module_main CALL.
			//
			// `StepOneBoardFrame` calls this before every call it makes, which is one to three per
			// board frame in the steady state and up to sixteen during boot - so the same packet
			// went out two or three times over. The ROM stamps a fresh counter at +2 exactly once
			// per board frame (i960 0x14F8), which makes "is this a packet the peer has not seen"
			// free to answer and needs no state of our own beyond the last value sent.
			//
			// The heartbeat is NOT optional. Before `net_flag` goes up the ROM stamps nothing, so
			// the counter is frozen at zero and a pure change-detector would transmit nothing at
			// all - and it is exactly that traffic which brings the peer's ring up in the first
			// place. So: act on a new stamp, or once per host frame when there is no stamp yet.
			//
			// THE TAKE IS RATE-LIMITED FOR A SHARPER REASON THAN THE SEND. The plugin queues
			// received packets in order so no one-frame state is discarded on the way in - but
			// `StageCommPayload` lays down whatever `s_peerPacket` holds, and the ROM reads the
			// comm window ONCE per board frame. Taking a packet per module_main call therefore
			// overwrote s_peerPacket two or three times between reads and showed the ROM only the
			// last of them, which is the same discard the queue exists to prevent, just moved
			// downstream. Measured: with the queue in and the take unthrottled, the peer's 0x24
			// still never reached the ROM and the cabinets still loaded different stages.
			//
			// One in, one out, per board frame - matching the rate the peer produces them.
			uint16_t seq = 0;
			memcpy(&seq, outgoing + OmgRva::PKT_SEQ, sizeof(seq));
			static uint16_t s_lastSentSeq = 0;
			const bool stampIsNew = (seq != s_lastSentSeq);
			if (!stampIsNew && s_sentThisHostFrame)
			{
				return;
			}
			s_lastSentSeq = seq;
			LinkSendMark();

			// Nothing arrived: keep the packet we already have. Delivery is StageCommPayload's job,
			// and it happens just before the module's transfer rather than here - see there.
			net::LinkSend(outgoing, OmgRva::COMM_PAYLOAD);
			if (net::LinkTake(s_peerPacket, OmgRva::COMM_PAYLOAD) != OmgRva::COMM_PAYLOAD)
			{
				return;
			}
			s_linkHealth.NotePacket();
			(LinkPacketStamped(s_peerPacket) ? s_pktStamped : s_pktUnstamped)++;
		}

		// THE RECEIVE HALF — and it is the MODULE that performs the delivery, not this function.
		//
		// The remote cabinet is board 1. Its comm block exists, its send buffer is real memory, and
		// the module's own per-frame transfer (`FUN_18006A310`) already copies that buffer into
		// board 0's RECV[0] slot - into the bank board 0 is not reading, then flipping the selector
		// so it is - and raises bit7 (data ready), which is what `Net_check` step 9 waits for. The
		// only reason board 1's packet is normally garbage is that its i960 never runs to write one.
		// So YAMP writes it, and the entire delivery protocol comes from the module.
		//
		// Called from the link-transfer shim IMMEDIATELY BEFORE that transfer and before either
		// board's CPU steps. Position is still the whole point, just inverted: the transfer READS
		// this buffer, so the packet has to be resident when it runs.
		//
		// THE BANK IS THE MODULE'S ANSWER, NOT A GUESS. The transfer sources board 1's packet from
		// bank `P2flag & 1`, and this reads that same byte one C statement earlier with no emulated
		// instruction in between - so the single correct bank is knowable rather than something to
		// hedge against. This is what replaced writing BOTH banks of board 0's RECV[0] after the
		// fact: that hedge existed only because YAMP was writing the transfer's OUTPUT, where the
		// selector had already moved on and YAMP's attempts to drive bit0 were overwritten.
		//
		// RESIDENCY IS FREE NOW. Nothing in the module ever writes a send buffer (measured: the only
		// references to 0x1807CC738 are the three reads inside the transfer), so an un-refreshed
		// packet simply stays and keeps being delivered - which is what a DPRAM does between
		// arrivals, and it means a dropped datagram still costs nothing.
		static void StageCommPayload()
		{
			// In `-von-2board` the second board really is running and really is producing packets,
			// so its send buffer belongs to its i960 and must not be written over. That mode is the
			// local two-cabinet experiment; it has no wire.
			if (!s_linkHealth.HavePacket() || g_dllBase == nullptr || s_cabinetRole == 0
				|| WantTwoBoardMode())
			{
				return;
			}
			const size_t bank = CommBoard::CurrentBank(g_dllBase + OmgRva::COMM_P2);
			memcpy(g_dllBase + OmgRva::COMM_P2_SEND + bank, s_peerPacket, OmgRva::COMM_PAYLOAD);
		}

		void DriveCommFirmware()
		{
			// A LIVE ROOM IMPLIES THE HOLD. "Wait for the other cabinet at boot" is a local
			// preference for a standalone board; joining a room is an explicit statement that
			// there IS another cabinet, and a player who joined one and left the box unticked
			// would get a session where nothing links and nothing says why.
			const YAMPSettings* settings = gGeneral.GetSettings();
			if (g_dllBase == nullptr || settings == nullptr
				|| (!settings->m_vonHoldLink && !LinkAvailable()))
			{
				return;
			}
			// A standalone cabinet never runs the check (link_ID 3 exits it early), so there is no
			// ring to report on and nothing here should touch the module's own state. The APPLIED
			// role, not the setting - see s_cabinetRole.
			const uint32_t role = s_cabinetRole;
			if (role == 0)
			{
				return;
			}

			const CommLink link = ObserveLink();
			// Truthful rather than convenient. While the ring is down the ROM is parked at the
			// ring-up poll and never reads the status bytes, but if a future change raised byte 0
			// without a peer the ROM would take its own "Illegal Nodes: %d" path - reset the comm
			// board and restart the check. That is correct behaviour for a mis-reported ring, not
			// a bug to paper over.
			CommBoard::WriteFirmwareStatus(g_dllBase + OmgRva::COMM_P1, link.peerUp,
				static_cast<uint8_t>(role) /* MASTER -> 1, SLAVE -> 2 */, link.nodes,
				"comm firmware", "Checking Network Now");

			// After the status, because the exchange is what raises "data ready" and the ROM reads
			// the two together.
			ExchangeCommPayload();

			// AND FINISH THE MODULE'S OWN BRING-UP, which a ring that never comes up would starve.
			//
			// `FUN_180072FB0` sits in state 0x12 until DLL flag 0x6910DF goes non-zero, and the only
			// thing that sets it is hook 0's handler - which the emulated ROM reaches at
			// `BlackOut+0x190`, i.e. AFTER `Net_check` RETURNS. Parking the board inside the link
			// check therefore parks the module one state short of finished, for as long as the hold
			// lasts, and the board is drawing frames nobody composites: measured `brought-up=0` and
			// a black window across a whole run while `GlobCntr` advanced normally.
			//
			// So say it ourselves. This is not a shortcut past anything - it is the same write the
			// hook makes, at the only moment it can be made, because the ROM is deliberately not
			// going to get there until a peer answers. The hook's other job (clearing 0x2000 bytes
			// of text RAM) is precisely what must NOT happen here: that clear is what wipes the site
			// screen the moment the check completes.
			// Only while the ring is DOWN: once a peer answers, the ROM completes the check and
			// reaches BlackOut+0x190 on its own, and hook 0 does the write for real.
			if (!link.peerUp && g_dllBase[0x6910DF] == 0)
			{
				g_dllBase[0x6910DF] = 1;
				net::Logf("bring-up flag forced - the ROM is parked in Net_check waiting for the "
					"ring and cannot set it itself");
			}
		}

		// PUMP THE RPCN SESSION. Once per host frame, and Virtual On is the only game that has to
		// do this for itself. (Declaration order: the definition follows the link helpers it uses.)
		// do this for itself.
		//
		// Every other title reaches poll() through `NetSession::Drive`, which also runs the whole
		// lockstep round flow - barrier, seed, frame numbering, state checks. Virtual On uses NONE
		// of that: the ROM's own link protocol does the synchronising, so K2Host never calls
		// NetSession at all. That is the right architecture and it left one thing dangling - the
		// session's connect, its RPCN signalling, its transport update and its socket drain all
		// hang off poll(), so without this the room would never form and link_ready would never
		// go true, on the lobby path as well as the command-line one.
		void DriveNetSession()
		{
			if (!net::IsAvailable())
			{
				return;
			}
			net::Api()->poll(net::Session());
			net::DriveSession();   // -net-host / -net-join; idempotent, inert for a lobby session
		}

		// Reported to the netplay overlay - see K2Host.h. Reads the ROM's own answers rather than
		// YAMP's intentions: node id/count are what `Net_check` copied out of the ring, and
		// `checkDone` is net_flag, so an overlay saying "linked" is saying what the CABINET
		// thinks, not what the transport hopes.
		bool GetLinkedCabinet(LinkedCabinet& out)
		{
			if (s_cabinetRole == 0 || g_dllBase == nullptr || !m2ftg::IsBoardBooted())
			{
				return false;
			}
			out.role = s_cabinetRole;
			out.ringUp = LinkIsLive();
			out.nodeId = *I960At(g_dllBase, 0, OmgRva::SYM_NODE_ID);
			out.nodes = *I960At(g_dllBase, 0, OmgRva::SYM_TOTAL_NODES);
			out.checkDone = *I960At(g_dllBase, 0, OmgRva::SYM_NET_FLAG) == 1;
			return true;
		}

		// ---- THE ROM-LEVEL PROOF, in yampnet.log -----------------------------------------------
		//
		// Called once per HOST frame. Two jobs, and the first one is not cosmetic: this is what
		// ages the link's peer timeout, and it has to happen here rather than in the exchange
		// because a host frame is one to sixteen `module_main` calls.
		//
		// The second is the diagnostic this work has been missing. Everything the link produced so
		// far was TRANSPORT-level - `ring UP, node id 1 of 2` says our own firmware model answered,
		// not that the ROM believed it - and the `[comm]`/`net=`/`id=` probe goes through
		// DebugLogFile, which needs a debugger attached to read. So a two-machine run could show
		// traffic flowing and leave "did the cabinet actually link?" to somebody watching a screen.
		// These fields are the ROM's own verdict, in the log both peers already write:
		//
		//   net=1                  Net_check completed - "Network Check Success" (step 12)
		//   id=1 node=1/2          link_ID, and the node id/count the check copied out of the ring
		//   rx=<state>             cRecn+4: 0xFE no net_flag, 0xFF ring down or packet REJECTED,
		//                          anything else came out of a peer packet the ROM ACCEPTED
		//   tx/rx seq              the stamped counters - both moving means both ROMs are past
		//                          their checks and exchanging real state
		//
		// Logged on change of the discrete fields plus a slow heartbeat, so a whole run is a few
		// dozen lines and the transition that matters is never lost between samples.
		void LogLinkState(int frame)
		{
			// The no-stamp heartbeat is once per HOST frame; this is where that frame turns over.
			s_sentThisHostFrame = false;

			// Age the peer timeout here, in HOST frames - a host frame is one to sixteen
			// module_main calls, and a timeout counted down there would be of unknown,
			// machine-dependent length.
			s_linkHealth.Age();

			// s_cabinetRole is the game gate as well as the role gate: it is only ever non-zero
			// after ApplyCabinetRole verified hook 6's InitNetwork injector, which exists in `omg`
			// alone. The RVAs below are Virtual On work-RAM addresses and mean nothing in VF2.
			const YAMPSettings* settings = gGeneral.GetSettings();
			if (g_dllBase == nullptr || settings == nullptr || !settings->m_vonLinkLog
				|| s_cabinetRole == 0 || !m2ftg::IsBoardBooted())
			{
				return;
			}
			const auto u8 = [](uint32_t a) { return *I960At(g_dllBase, 0, a); };
			const auto u16 = [](uint32_t a) {
				uint16_t v = 0; memcpy(&v, I960At(g_dllBase, 0, a), sizeof(v)); return v;
			};

			const uint8_t netFlag = u8(OmgRva::SYM_NET_FLAG);
			const uint8_t linkId  = u8(OmgRva::SYM_LINK_ID);
			const uint8_t nodeId  = u8(OmgRva::SYM_NODE_ID);
			const uint8_t nodes   = u8(OmgRva::SYM_TOTAL_NODES);
			const uint8_t rxState = u8(OmgRva::SYM_CRECN + OmgRva::PKT_STATE);
			const uint16_t txSeq  = u16(OmgRva::SYM_CSEND + OmgRva::PKT_SEQ);
			const uint16_t rxSeq  = u16(OmgRva::SYM_CRECN + OmgRva::PKT_SEQ);
			const auto u32 = [](uint32_t a) {
				return *reinterpret_cast<uint32_t*>(I960At(g_dllBase, 0, a));
			};
			const uint32_t mode   = u32(OmgRva::SYM_MAINMODE);

			// THE FIELDS THAT NAME A STAGE DESYNC'S CAUSE.
			//
			// `rx=` alone cannot: it is the PEER's state, and the two ways the cabinets can end up
			// on different stages differ only in what THIS cabinet did. Either both rolled (both
			// had VersusMode != 0 at i960 0xCDB0C, each published its own stage and adopted the
			// other's), or the adopter never saw the single-frame 0x24 that carries the roll. So:
			//
			//   tx=      this cabinet's own published state (cSend+4) - 0x24 means "I have the
			//            stage", and BOTH cabinets showing 0x24 having each rolled is the first
			//            case, caught red-handed
			//   stage=   the value in the payload, out (cSend+8) and in (cRecn+8)
			//   sel=     0x503A84, what this cabinet will actually load
			//   vs=      VersusMode (0x503A7C), the flag that decides roll-vs-adopt
			//   field=   FieldNo (0x5770F0), the stage the match is running
			//
			// Compare `sel=` across the two logs and the disagreement is visible directly; compare
			// `tx=`/`vs=` at the frame it appears and the cause is visible too.
			const uint8_t txState  = u8(OmgRva::SYM_CSEND + OmgRva::PKT_STATE);
			const uint16_t txStage = u16(OmgRva::SYM_CSEND + OmgRva::PKT_STAGE);
			const uint16_t rxStage = u16(OmgRva::SYM_CRECN + OmgRva::PKT_STAGE);
			const uint32_t stageSel = u32(OmgRva::SYM_STAGE_SEL);
			const uint32_t versus   = u32(OmgRva::SYM_VERSUSMODE);
			const uint32_t fieldNo  = u32(OmgRva::SYM_FIELDNO);

			// The discrete state, packed so one comparison decides whether anything moved. The
			// sequence counters are deliberately OUT of it: they change every frame by design, and
			// including them would turn "log on change" into "log every frame".
			const uint32_t key = netFlag | (linkId << 8) | (nodeId << 16) | (nodes << 20)
				| (rxState << 24);
			// The stage handshake gets its own key rather than being squeezed into the one above,
			// because every field in it has to be able to trigger a line ON ITS OWN - the whole
			// point is to catch the one frame a value changes.
			const uint64_t stageKey = static_cast<uint64_t>(txState)
				| (static_cast<uint64_t>(versus) << 8)
				| (static_cast<uint64_t>(stageSel) << 24)
				| (static_cast<uint64_t>(fieldNo) << 40);
			static uint32_t s_lastKey = 0xFFFFFFFF;
			static uint32_t s_lastMode = 0xFFFFFFFF;
			static uint64_t s_lastStageKey = 0xFFFFFFFFFFFFFFFFull;
			const bool changed = key != s_lastKey || mode != s_lastMode
				|| stageKey != s_lastStageKey;
			s_lastStageKey = stageKey;
			if (!changed && (frame % 120) != 0)
			{
				return;
			}
			s_lastKey = key;
			s_lastMode = mode;


			// WHERE THE PEER'S PACKET IS AT THE END OF A HOST FRAME. Four points along the path it
			// takes, so a failure names its own cause instead of leaving a choice of suspects:
			//
			//   b1snd        board 1's send buffer - what YAMP STAGED for the module to deliver
			//   data0/data1  the emulated comm RAM's receive window, BOTH banks - what the MODULE's
			//                own transfer put there out of b1snd
			//   stage        guest 0x501CE0, what the ROM's own memcpy picked up out of it
			//   cRecn        only written if the ROM accepted it
			//
			// The first two split the delivery in half, which is the point of logging b1snd at all:
			// a valid pair in b1snd and zeros in data0/data1 means the module's transfer did not run
			// or sourced the other bank, and an empty b1snd means nothing was staged.
			//
			// Each is the packet's <seq>/<check> pair, and the pair is self-validating: a live
			// packet has check == seq ^ 0xAE5E, so a glance says whether what is sitting there is a
			// real packet, a stale one, or zeros.
			const auto pair = [](const uint8_t* p, char* out, size_t n) {
				uint16_t seq = 0, check = 0;
				memcpy(&seq, p + OmgRva::PKT_SEQ, sizeof(seq));
				memcpy(&check, p + OmgRva::PKT_CHECK, sizeof(check));
				snprintf(out, n, "%04X/%04X%s", seq, check,
					check == static_cast<uint16_t>(seq ^ OmgRva::PKT_CHECK_XOR) ? "*" : "");
			};
			char data0[16], data1[16], stage[16], send0[16], send1[16], b1snd[16];
			pair(g_dllBase + OmgRva::COMM_P1 + OmgRva::COMM_RECV0, data0, sizeof(data0));
			pair(g_dllBase + OmgRva::COMM_P1 + OmgRva::COMM_BANK + OmgRva::COMM_RECV0,
				data1, sizeof(data1));
			pair(I960At(g_dllBase, 0, 0x501CE0), stage, sizeof(stage));
			// Whichever bank the module will source board 1's packet from next transfer - the same
			// selector StageCommPayload writes through, so the two cannot drift apart in the log.
			pair(g_dllBase + OmgRva::COMM_P2_SEND
				+ ((*(g_dllBase + OmgRva::COMM_P2_FLAG) & 1) ? OmgRva::COMM_BANK : 0),
				b1snd, sizeof(b1snd));
			// And the OUTGOING window, both banks. `tx` above is what the ROM stamped into cSend;
			// these are where that landed once its own memcpy put it in comm RAM, which is what
			// decides whether the bank YAMP transmits from is the one the ROM just wrote.
			pair(g_dllBase + OmgRva::COMM_P1_SEND, send0, sizeof(send0));
			pair(g_dllBase + OmgRva::COMM_P1_SEND + OmgRva::COMM_BANK, send1, sizeof(send1));
			const char* verdict =
				netFlag != 1                        ? "link check has NOT completed" :
				rxState == OmgRva::PKT_STATE_NONET  ? "checked, but the ROM sees no net_flag" :
				rxState == OmgRva::PKT_STATE_NOPKT  ? "checked - no packet accepted this frame" :
				                                      "LINKED - accepting the peer's packets";
			net::Logf("[von] f=%d %s | net=%u id=%u node=%u/%u main=%u "
				"tx=%02X rx=%02X vs=%u sel=%u field=%u stage tx=%04X rx=%04X | "
				"seq tx=%04X rx=%04X | stamped=%u unstamped=%u | "
				"flag=%02X send0=%s send1=%s b1snd=%s data0=%s data1=%s stg=%s xfer=%u",
				frame, verdict,
				netFlag, linkId, nodeId, nodes, mode,
				txState, rxState, versus, stageSel, fieldNo, txStage, rxStage,
				txSeq, rxSeq, s_pktStamped, s_pktUnstamped,
				*(g_dllBase + OmgRva::COMM_P1_FLAG), send0, send1, b1snd, data0, data1, stage,
				CommReset::s_transfers);
		}

		// ADVANCE THE BOARD BY EXACTLY ONE EMULATED FRAME.
		//
		// One module_main call is NOT one emulated frame - the board is paced by the module's
		// clock, so a fast host gets calls that advance nothing and a slow one gets calls that
		// advance two or three frames at once. See VirtualClock.h; the short version is that
		// netplay cannot number frames it does not control, and three board frames sharing one pad
		// is a real divergence rather than a bookkeeping one.
		//
		// With the clock virtual, YAMP owns the pacing: tick it forward in sub-frame steps and stop
		// the moment the ROM's own counter moves. Every peer therefore runs exactly one emulated
		// frame per netplay frame no matter how fast it renders, and a lockstep stall (which simply
		// does not call this) freezes the board rather than letting it accumulate time to catch up
		// on later.
		//
		// Returns module_main's result from the call that produced the frame.
	}
}
