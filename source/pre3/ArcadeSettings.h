#pragma once

#include <cstdint>

namespace pre3
{
	// The board's GAME ASSIGNMENTS, for the rows the module's own settings injector cannot reach.
	//
	// Sega Racing Classic 2 keeps its arcade settings in two places with an identical layout: a
	// working copy at guest 0x100180 and the NVRAM copy at guest 0x72629C. The module's injector
	// (DLL 0x18002C410, reached from hook 5) writes the NVRAM copy at boot and the board loads it
	// into the working copy. Every address and offset below was read out of the board's OWN
	// service-menu table at guest 0x0E5548 - each row there carries the address of the byte it
	// edits - and then confirmed by reading those bytes out of a running game and comparing them
	// against what the GAME ASSIGNMENTS screen displayed.
	//
	//   offset  row              option list (from the same menu table)
	//   +0x17   coin setting     injector only: 1, or 0x1B for free play
	//   +0x18   COUNTRY          INTERNATIONAL, JAPAN, USA, EXPORT, AUSTRALIA, KOREA
	//   +0x1E   LINK ID          SINGLE, MASTER, SLAVE, LIVE
	//   +0x1F   CAR NUMBER       1 .. 8
	//   +0x20   CABINET TYPE     DELUXE, TWIN, SPECIAL
	//   +0x21   DIFFICULTY       EASY, NORMAL, HARD, HARDEST
	//   +0x22   ADVERTISE SOUND  OFF, ON
	//   +0x23   GAME MODE        NORMAL(SPRINT), GRAND PRIX, 100/200/300/400/500 MILES
	//   +0x26   MOTOR POWER      50%, 60%, 70%, 80%, 90%, 100%
	//   +0x28   RANKING MODE     NORMAL, CAMPAIGN, INTERNET
	//
	// WHY THIS EXISTS AT ALL, rather than more config bytes: the injector writes +0x17, +0x18,
	// +0x1E, +0x1F, +0x21 and +0x22, and it NEVER writes +0x20 - so CABINET TYPE has no host
	// route whatsoever. COUNTRY has one, but the injector's fold can only produce three of the
	// board's six values (config 0 -> JAPAN, 1 -> USA, 2 -> INTERNATIONAL), so EXPORT, AUSTRALIA
	// and KOREA are equally unreachable. Both are written here instead, directly into the board's
	// memory, which is the same thing the module does and lands in the same bytes.
	//
	// DIFFICULTY is deliberately NOT here. The injector can express all four of its values once
	// the host pre-folds the index, so Pre3Host does that and this stays out of the way - the
	// rule being that a setting the module can carry should travel the module's own path.
	namespace ArcadeSettings
	{
		// LINK ID's four values. Netplay will drive this; see SetLink.
		enum class LinkId : uint8_t { Single = 0, Master = 1, Slave = 2, Live = 3 };

		// What to apply. Values are the board's own, i.e. indices into the option lists above,
		// so a UI combo can offer the board's strings verbatim and store the index.
		//
		// The last three rows joined 2026-08-08 for parity with the netplay room metadata: a room
		// publishes and adopts them (SetRoomAssignments), so a machine has to be able to CHOOSE
		// them outside a session too, or the host could only ever publish the board's defaults as
		// edited by hand in the service menu each boot. Like CABINET TYPE they have no injector
		// route at all - the direct write here is their only host path.
		struct Desired
		{
			uint8_t country = 1;       // JAPAN
			uint8_t cabinetType = 1;   // TWIN, which is what the board defaults to
			uint8_t linkId = 0;        // SINGLE
			uint8_t carNumber = 0;     // car 1
			uint8_t gameMode = 0;      // NORMAL(SPRINT)
			uint8_t motorPower = 3;    // 80%, the board's own default
			uint8_t ranking = 0;       // NORMAL
		};

		void SetDesired(const Desired& desired);

		// NETPLAY HANDOFF. A linked SRC2 cabinet is described by two bytes - which role this
		// cabinet plays (MASTER drives the link, SLAVE follows) and which car it is - and both
		// have to agree across the session or the boards disagree about who owns what.
		//
		// Not wired to a session yet, and deliberately so: pre3 netplay is gated on a
		// deterministic clock that SRC2 does not have (see Determinism.cpp, where the pin is an
		// FV2-only whitelist because pinning it stops SRC2 booting). This is the seam that
		// wiring will use - the session sets role and car once at match start, and everything
		// downstream already reads through it.
		void SetLink(LinkId role, uint8_t carNumber);

		// Applies the desired values once the board has settled, and then latches so the game's
		// own service menu owns them for the rest of the session. Call once per frame; it is a
		// no-op for any game that is not SRC2, and before the board's own injector has run.
		void Update();

		// THE BOOT NETWORK-CHECK GATE, and whether our LINK ID ever reaches it.
		//
		// SRC2's boot network screen is `FUN_00093DB4`, called exactly once from the boot main:
		//
		//     00018a14  lis  r3, 0x10
		//     00018a18  lbz  r3, 0x19e(r3)     ; LINK ID - the working copy, 0x100180 + 0x1E
		//     00018a1c  bl   0x93db4
		//
		// and the first thing it does is `if (mode == 0) { nodes = 1; id = 1; return; }` - so a
		// SINGLE cabinet prints NOTHING. Every string it can draw ("THIS IS MASTER CONTROLLER",
		// "NO CARRIER! CHECK NETWORK CABLE", "NETWORK BOARD NOT PRESENT") is below that early
		// return. No text is composed, which is a very different thing from text being composed
		// and lost, and docs/src2-netplay-recon.md §6 spent a commit on the second reading.
		//
		// THE TIMING IS THE QUESTION THIS ANSWERS. The module's own settings injector (HLE hook 5,
		// guest 0x7480, inside FUN_00007434) runs at guest 0x18950 - 0xC8 bytes of boot code
		// before that read, in the SAME emulated frame. Update() above deliberately waits for that
		// injector and therefore writes a HOST frame later, by which point the guest is thousands
		// of instructions past 0x18A1C. If that is right, the UI's LINK ID row can never affect
		// the boot check, only the service menu and everything after it.
		//
		// So this samples the two copies of LINK ID against the ROM's own latched answer and logs
		// a line whenever any of them moves, plus a one-shot verdict the moment the check is seen
		// to have completed. Cheap, unconditional, and self-terminating - it stops sampling once
		// the verdict is out. Call once per frame beside Update().
		void LogLinkGate();

		// Resets the latch. Call when the board restarts.
		void Reset();

		// THE FIVE ROWS A LINKED RACE IS PLAYED UNDER, read live out of the working settings copy
		// so they include everything - the injector's values, this file's writes AND any edit the
		// operator made in the service menu since. Published as room metadata when an SRC2 room is
		// hosted (net::Src2Assignments), which is the whole reason the read exists: the browser
		// lists what a race IS before anyone joins it.
		//
		// Offsets and option counts are from the service menu's own row table at guest 0x0E5548,
		// same provenance as the header comment's table:
		//
		//   +0x20  CABINET TYPE  DELUXE / TWIN / SPECIAL
		//   +0x21  DIFFICULTY    EASY / NORMAL / HARD / HARDEST
		//   +0x23  GAME MODE     NORMAL(SPRINT) / GRAND PRIX / 100..500 MILES
		//   +0x26  MOTOR POWER   50% .. 100%
		//   +0x28  RANKING MODE  NORMAL / CAMPAIGN / INTERNET
		//
		// Values are clamped to those ranges on the way out - a mid-boot read can catch the block
		// before the injector has filled it, and a garbage index published to a room browser would
		// be indistinguishable from a real configuration.
		struct LiveAssignments
		{
			uint8_t cabinetType;   // 0..2
			uint8_t difficulty;    // 0..3
			uint8_t gameMode;      // 0..6
			uint8_t motorPower;    // 0..5
			uint8_t ranking;       // 0..2
		};
		// False when the board (or its validated guest RAM) is not up yet.
		bool ReadLiveAssignments(LiveAssignments& out);

		// THE ROOM'S ASSIGNMENTS, FORCED. A netplay room publishes the host's five rows (see
		// ReadLiveAssignments) and every cabinet in the room must RUN them, not just read about
		// them: GAME MODE decides the race being driven and DIFFICULTY what the AI does, so two
		// cabinets disagreeing are in different races that happen to share a link.
		//
		// Armed by CommBoard::DriveRoomRole at the same moment it reboots the board into its
		// cabinet role, because that reboot is what makes adoption possible at all: the restore
		// wipes guest RAM, ArcadeSettings::Reset() re-arms the latch, and the next Update() runs
		// AFTER the module's own injector (the coin-byte wait) - so the room's values land last
		// and win, exactly as this file's own writes always have. While armed, Update() writes
		// these five rows (and the override's cabinet type instead of the local preference);
		// DIFFICULTY - deliberately absent from Desired because the local value travels the
		// injector's own path - IS written here, since the room's value arrives mid-session where
		// no config field can carry it.
		//
		// Cleared when the room dissolves; the standalone reboot then restores the local values.
		void SetRoomAssignments(const LiveAssignments& assignments);
		void ClearRoomAssignments();
	}
}
