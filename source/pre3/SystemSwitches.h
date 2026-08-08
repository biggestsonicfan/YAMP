#pragma once

#include <cstddef>

namespace pre3
{
	// Wires the cabinet's TEST and SERVICE switches into the emulated Model 3 I/O, so the board's
	// own service menu — and with it the arcade INPUT TEST, which is the only authoritative way to
	// check that every button reaches the ROM the way the panel wired it — is reachable.
	//
	// Same intent as m2ftg::InstallSystemSwitches, different mechanism. The m2ftg boards rebuild
	// their I/O byte once per emulated frame, so that host has to intercept the rebuild and pull
	// the two lines low behind it. pre3 instead funnels every read of the board's JAMMA registers
	// through one accessor (M3EInput::read_port, ImportSymbol::IO_READ_PORT), which is not
	// overridden by any of the six games — so the switches can simply be sampled AT READ TIME,
	// which is also what the hardware does and leaves the module's own per-frame rebuild alone.
	//
	// Install once at patch time, with .rdata writable: the redirect goes into the vtables rather
	// than into code. SetSystemSwitches is then the per-frame switch position, and does nothing at
	// all if the install found no accessor.
	//
	// Takes the board vtables ALREADY LOCATED (pre3::FindBoardVtables), rather than finding them
	// itself, because locating them keys on slot 0 - the very slot this redirects. Finding them
	// per-installer would mean the first install hid them from the second, an ordering trap with
	// no symptom but a silently missing feature.
	void InstallSystemSwitches(void* const* vtables, size_t count);
	void SetSystemSwitches(bool test, bool service);

	// THE CABINET'S WHEEL AND PEDALS, per frame, and the reason Sega Racing Classic 2 has never
	// been drivable: the board's 8-channel ADC ring is read through the same accessor and NOTHING
	// has ever filled it, so every channel returns zero - which on a steering axis is full lock,
	// not centre. See the ADC block in the .cpp for the decoded contract and for the one thing in
	// it that is still convention rather than measurement (which channel is which).
	//
	// steer -1..+1 (0 = straight ahead), throttle and brake 0..1. Values outside that are clamped.
	// Rides the same install as the switches, so it is inert on a board with no accessor and needs
	// no separate feature gate.
	//
	// LOCAL INPUT, NOT NETPLAY. Each cabinet reads its own converter and only the resulting car
	// state crosses the link, so this never touches the wire - see docs/src2-netplay-recon.md's
	// analogue note.
	void SetDrivingAxes(float steer, float throttle, float brake);
}
