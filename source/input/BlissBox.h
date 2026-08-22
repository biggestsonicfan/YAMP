#pragma once

// The Bliss-Box adapter layer — a THIRD controller backend beside XInput and DirectInput, and the
// only one that can tell you what is plugged into it.
//
// WHY THIS EXISTS AT ALL. A Bliss-Box (4-Play / Gamer-Pro / GPA / BlisSTer) presents each of its
// ports as an ordinary HID gamepad, so DirectInput already sees a Sega Saturn Twin Stick attached
// to one — as "Bliss-Box 4-Play PORT.1", a nameless bag of numbered buttons. That is enough to
// bind, and not enough to be RIGHT: the twin stick's twelve inputs would arrive as "Button 3",
// "Button 7", "Axis 2 -", and every one of them would need mapping by hand, per port, with no way
// for YAMP to know a twin stick is what it is looking at.
//
// The adapter's own API answers that. Over a HID FEATURE report it will say which of ~80 console
// controllers is on each port (SATURN_DIGITAL, N64, DC_PAD, ...), which means YAMP can detect a
// Saturn Twin Stick, decode it against the vendor's published Saturn button map, and hand Virtual
// On its twelve cabinet inputs with nothing configured by the player. See TwinStickState.
//
// PROTOCOL, from the vendor's API document (bliss-box.com/api, exported 2026-08-21):
//   * USB VID 0x16D0, PID 0x0D04..0x0D07. Each PORT is its own USB device — a 4-Play carries one
//     firmware chip per port — and the PID is what distinguishes them, "reserved for player ID".
//     RetroArch's input/include/blissbox.h agrees: "first of 4 controllers, each one increments
//     PID by 1".
//   * Get commands are HID GetFeature calls whose report ID selects the command. WHERE THE REPLY
//     STARTS IS NOT FIXED: a numbered HID feature report is supposed to echo its report ID in
//     byte 0 and put the body at byte 1, but the adapter measured here returns the body alone.
//     Assuming the prefix shifts every field one byte - which lands the left lever's axes on top
//     of the button rows and makes half a twin stick silently dead - so the framing is PROBED per
//     device instead. See ProbeDataOffset. (RetroArch allows for both shapes too.)
//       0x11 adapter info: [0]=controller type  [1]=modes  [2]=major  [3]=player ID (minus 3)
//                          [4]=minor
//       0x10 controller payload: [0]=player ID  [1..3]=button rows 1-3 (24 buttons)
//                          [4]=X  [5]=Y  [6]=Z  [7]=X2  [8]=Y2  [9]=Z2  [10]=slider [11]=dial
//                          [12]=HAT
//   * "The Bliss-Box uses V-USB ... a control transfer can and will fail from time to time ... If
//     you get an error, simply try again. Make sure to build in a time out." Hence the retry and
//     the worker thread below — a failed control transfer must never become a dropped frame.
//
// The payload command exists FOR LATENCY: the document notes a control transfer beats the 16 ms
// guaranteed interrupt poll, at the cost of not being guaranteed to land every time. That trade is
// only safe off the game thread, so this backend owns a polling thread and the frame loop reads a
// published snapshot — never the USB stack. See the note on Poll().

#include <cstdint>
#include <string>

namespace Input
{
	namespace BlissBox
	{
		// Bliss-Box controller enum, verbatim from the API document's table. Only the values YAMP
		// reasons about are named; the rest stay numeric because nothing here needs them.
		enum ControllerType : uint8_t
		{
			TYPE_NO_DETECTION = 0,
			TYPE_SATURN_DIGITAL = 3,   // MEASURED: what a Twin Stick reports as - see TwinStickState
			TYPE_SATURN_ANALOG = 8,    // the Saturn 3D pad
			TYPE_SATURN_GUN = 59,
			TYPE_SEARCHING = 255,      // port powered, still identifying whatever is on it
		};

		// The adapter's raw payload, exactly as command 0x10 lays it out (report ID stripped).
		// Kept whole rather than pre-digested so the settings page can show it: every mapping in
		// this file is a claim about these bytes, and a claim you cannot see is a claim you cannot
		// check against the hardware in front of you.
		struct Payload
		{
			uint8_t playerId = 0;
			uint8_t buttons[3] = {};   // rows 1-3; HID button n (1-based) = row (n-1)/8, bit (n-1)%8
			uint8_t x = 0x80, y = 0x80, z = 0x80;
			uint8_t x2 = 0x80, y2 = 0x80, z2 = 0x80;
			uint8_t slider = 0, dial = 0;
			uint8_t hat = 0x0F;
		};

		// A SEGA SATURN TWIN STICK, decoded. Thirteen switches, which is exactly what the levers
		// carry and exactly what a standard Saturn pad carries - and that is not a coincidence.
		//
		// Sega built the Twin Stick (HSS-0151) on the SAME double-74153 encoder as the standard
		// Saturn pad, so the console cannot tell the two apart and neither can the Bliss-Box: the
		// port reports SATURN_DIGITAL either way. There is no twin-stick device ID to look for.
		// What there IS, is a fixed one-to-one correspondence between the levers and the pad's
		// thirteen inputs, which is what makes this decodable at all:
		//
		//   Saturn pad   Twin Stick                    Saturn pad   Twin Stick
		//   Up/Dn/Lf/Rt  Left lever, that direction    Y            Right lever, Up
		//   L            Left lever, TRIGGER (weapon)  B            Right lever, Down
		//   R            Left lever, THUMB (dash)      X            Right lever, Left
		//   Start        Start                         Z            Right lever, Right
		//                                              A            Right lever, TRIGGER (weapon)
		//                                              C            Right lever, THUMB (dash)
		//
		// (Table from the NFG/GameSX controller wiki, controls:twin_stick_button_layout.)
		//
		// The Saturn side then lands on HID buttons per the vendor's own global mapping sheet
		// (docs.google.com/spreadsheets/d/1Bk3j5kaKfV1tOfzCq3GLKFsff027RdmwOuSfBLM3Ims, the
		// "version 3 & 4" native table): A=HID1, B=HID2, X=HID3, Y=HID4, Start=HID6, Z=HID7,
		// C=HID8, L=HID9, R=HID10, and the d-pad on the main X/Y axes - the sheet's note is that
		// the Bliss-Box "will send d-pad only controllers ... u,d,l,r buttons to analog by
		// default", so the axes are the primary source and the HAT is taken as well rather than
		// instead (a firmware with UDLR mode set populates one or the other).
		//
		// TRIGGER vs THUMB is the one thing the wiki names and the sheet does not disambiguate on
		// the wire, so both are carried separately here and the consumer decides. On the cabinet
		// the trigger is the weapon and the thumb button is the dash/turbo.
		struct TwinStickState
		{
			bool leftUp = false, leftDown = false, leftLeft = false, leftRight = false;
			bool rightUp = false, rightDown = false, rightLeft = false, rightRight = false;
			bool leftTrigger = false, leftThumb = false;    // Saturn L, R
			bool rightTrigger = false, rightThumb = false;  // Saturn A, C
			bool start = false;

			bool AnyHeld() const
			{
				return leftUp || leftDown || leftLeft || leftRight
					|| rightUp || rightDown || rightLeft || rightRight
					|| leftTrigger || leftThumb || rightTrigger || rightThumb || start;
			}
		};

		// One adapter port. `present` means the USB device is open; `IsTwinStickCapable` means
		// something that decodes as a Saturn digital pad is plugged into it - which a Twin Stick
		// is, and so is an ordinary Saturn pad, because the hardware genuinely cannot tell them
		// apart. Offering the choice anyway is the honest behaviour: a player holding a Saturn pad
		// gets the same twelve inputs on the buttons the wiki table names.
		struct PortState
		{
			bool present = false;
			int pid = 0;                     // 0x0D04..0x0D07 - the port's identity on the bus
			int playerId = 0;                // adapter info byte 3 minus 3, so 1..4 (0 = unknown)
			uint8_t controllerType = TYPE_NO_DETECTION;
			uint8_t firmwareMajor = 0, firmwareMinor = 0;
			uint8_t modes = 0;
			std::string id;                  // "blissbox:<pid>" - stable, and what settings.ini stores
			std::string name;                // "Bliss-Box port 1 - Saturn (digital)"
			Payload payload;
			TwinStickState stick;

			// Health, shown on the settings page. A Bliss-Box that is working normally still
			// accumulates errors (V-USB drops control transfers by design); a Bliss-Box whose
			// errors climb while updates do not is unplugged or wedged.
			uint64_t updates = 0;
			uint64_t errors = 0;

			// USABLE, not "is definitely a Saturn Twin Stick".
			//
			// A Twin Stick does report SATURN_DIGITAL, as measured on firmware 4.2 - the same
			// value an ordinary Saturn pad reports, since they share an encoder. So this could be
			// a strict equality test. It deliberately is not: the bias here is that decoding an
			// unexpected controller with the Saturn map fails VISIBLY (lamps that do not line up,
			// right there on the settings page) while an over-tight gate fails SILENTLY - no input
			// at all, nothing on screen saying why, and no reason to suspect the gate rather than
			// the wiring. That cost a debugging session already, when a byte-framing bug made this
			// port look like an unlisted controller type and the gate turned a decode problem into
			// a dead stick.
			//
			// NO_DETECTION and SEARCHING are still excluded: those are the adapter saying "nothing
			// here yet", which is not a controller.
			bool IsTwinStickCapable() const
			{
				return present
					&& controllerType != TYPE_NO_DETECTION
					&& controllerType != TYPE_SEARCHING;
			}
		};

		inline constexpr int MAX_PORTS = 4;

		// Human-readable name for a Bliss-Box controller enum value ("Saturn (digital)", "N64",
		// "searching...", "none"). Unknown values format as "type <n>" rather than lying.
		const char* ControllerTypeName(uint8_t type);

		// Starts the polling thread if it is not already running. Cheap and idempotent — call it
		// from anywhere that is about to want twin-stick input. Does NOT block on USB.
		void Start();

		// Stops the thread and closes every handle. Safe to call without a matching Start.
		void Shutdown();

		// Asks the worker to re-enumerate the bus at its next pass (a hot-plugged adapter, or the
		// Controls page's Rescan button). Safe from any thread, never blocks.
		void RequestRescan();

		// THE FRAME-LOOP READ. Copies the worker's latest snapshot for `port` (0..MAX_PORTS-1) and
		// returns false for an index that holds nothing. This is a mutex-guarded struct copy of a
		// few dozen bytes — it does NOT touch the USB stack, which is the whole point of the
		// worker thread. The Bliss-Box is a V-USB device: its control transfers routinely fail and
		// retry, and a frame loop that waited on one would hitch on the adapter's schedule rather
		// than the display's.
		bool GetPort(int port, PortState& out);

		// Number of ports currently open (not the number with a controller in them).
		int PortsOpen();

		// The first port holding something twin-stick capable, or -1. Used for the "Auto" port
		// assignment, so a single stick on any port just works.
		int FindTwinStick(int skipPort = -1);
	}
}
