#pragma once

#include <cstdint>

namespace m2ftg
{
	// THE EMULATED BOARD'S CLOCK, DRIVEN BY FRAMES INSTEAD OF BY REAL TIME.
	//
	// ---- Why this exists -----------------------------------------------------------------
	//
	// Virtual On's board is paced by the HOST'S WALL CLOCK, not by module_main calls. The module
	// reads QueryPerformanceCounter through one small wrapper (omg 0x1800856B0, 49 call sites) and
	// its task layer advances the board as much real time as has passed. Measured on two machines
	// 2026-08-04:
	//
	//   * a fast host (uncapped ~170 fps) advances the ROM's frame counter on ~35% of calls -
	//     60/170 - and the other 65% execute nothing at all;
	//   * a slower host advances it by TWO OR THREE frames in a single call, steadily.
	//
	// Netplay keys its frame index to module_main calls, so two peers at different frame rates put
	// different amounts of simulation into the same netplay frame. Worse than mis-numbering: a call
	// that runs three board frames applies ONE pad to all three, while the peer applies three
	// different ones, so the simulations genuinely part company. That is exactly what the first
	// working round showed - the peers' state hashes MATCHED at equal ROM frames (0x8B2A0E97 at
	// rom=8 on both) while their netplay frame 0 landed on rom=9 and rom=11 respectively.
	//
	// This is the same class of fault as Virtua Fighter 2's, and the opposite direction: VF2's
	// module_main calls sometimes advanced ZERO frames, which NetSession::EndFrame's stall test
	// already handles. Nothing can reconcile a call that advanced THREE, because the inputs for the
	// two frames it swallowed no longer exist.
	//
	// ---- What it does --------------------------------------------------------------------
	//
	// The wrapper is patched to return a counter YAMP owns, so every clock read in the module is
	// virtual. StepOneFrame then advances that counter in SUB-FRAME steps, calling module_main
	// after each, and stops the moment the ROM's own frame counter moves.
	//
	// The step being smaller than the board's frame period is what makes this correct: at most one
	// frame can come due per call, so a call can never swallow two. And because the loop terminates
	// on the ROM COUNTER rather than on a tick count, two peers reach identical board state per
	// netplay frame even if they need a different NUMBER of calls to get there - which they will,
	// since QueryPerformanceFrequency is a property of the machine.
	//
	// Deliberately NOT the module's own frame period: that constant is expressed in the local
	// machine's QPC ticks and finding it means trusting a decompile. A step of freq/120 is under
	// the period of any board slower than 120 Hz, which is every Model 2 game, and needs nothing
	// to be reverse-engineered to stay true.
	//
	// The board therefore only advances when a netplay frame does. A lockstep stall freezes it
	// exactly, rather than letting it run on and "catch up" the moment the peer's input arrives.
	class VirtualClock
	{
	public:
		// Patches the module's QueryPerformanceCounter wrapper to read this clock instead.
		// `wallClockFn` is the wrapper; null (an unresolved symbol) leaves the module on real time
		// and makes this whole object inert. Must run while the module's .text is writable, i.e.
		// from the host's symbol-resolution pass.
		bool Install(void* wallClockFn);

		bool Installed() const { return m_installed; }

		// One sub-frame tick. Cheap, and the only way the module's time moves.
		void Advance();

		// Ticks per Advance(), for logging. 0 until Install succeeds.
		int64_t Step() const { return m_step; }

		// The virtual clock's current reading - the same value the module now sees.
		int64_t Ticks() const;

		// PACE WALL TIME TO THE BOARD'S TIME. Call once per host frame, after the frame's work.
		//
		// Taking the clock away from the module also took away the thing that decided how FAST the
		// game runs: the board used to advance with real time, and now it advances one frame per
		// host loop iteration, so an uncapped host runs it at the host's frame rate. Measured on a
		// ~170 fps machine: Virtual On ran roughly three times too fast.
		//
		// So the host loop becomes the limiter, and it paces against the VIRTUAL clock rather than
		// a hardcoded refresh rate. Whatever period the module's own pacing uses is exactly the
		// amount of virtual time a frame consumes, so sleeping until wall time catches up
		// reproduces the board's native rate without anyone having to know what that rate is - and
		// it stays true for Motor Raid and Sega Rally 2, which need not share it.
		//
		// Overruns do NOT accumulate: a host that cannot keep up simply runs slow rather than
		// building a debt it then sprints to repay. That matters most after a lockstep stall, where
		// real time passes and virtual time does not.
		void PaceToVirtualTime();

		// HOW MANY BOARD FRAMES THIS HOST FRAME OWES. 1 in the steady state.
		//
		// The policy above - "a host that cannot keep up runs slow" - is right for one player and
		// exactly backwards for a LINKED PAIR. Game speed is then a competitive advantage: a
		// cabinet presenting at 32 Hz runs its board at 32 Hz, so its pilot moves, turns and fires
		// at barely half the rate of the one at 60, and loses for a reason that has nothing to do
		// with the match. Measured on the two-machine harness, where the second box renders into
		// an RDP virtual display capped at 32 Hz.
		//
		// So when a link is live, the emulated rate is held at 60 Hz in WALL time regardless of
		// how often the host can present: a host frame that overran its budget steps the board
		// more than once to make up the difference. This is the ordinary fixed-timestep
		// accumulator, and it is safe here for the reason lockstep made it unsafe before - there
		// is no frame numbering to violate and no peer to stay bit-identical with. A real cabinet
		// whose video output stuttered would still simulate at full speed; this makes ours do the
		// same.
		//
		// CAPPED, deliberately. Without a ceiling a long hitch (a shader compile, a debugger
		// break, a window drag) accrues a debt that the next frame tries to repay in one burst,
		// which freezes the host and looks exactly like a hang. Past the cap the time is dropped:
		// two cabinets briefly disagreeing about how much time passed is a far smaller problem
		// than one of them stopping.
		unsigned int BoardFramesDue(bool linkLive);

	private:
		bool m_installed = false;
		int64_t m_step = 0;
		int64_t m_frequency = 0;
		// Wall-clock reading this frame is allowed to finish at. 0 = the limiter has not started.
		int64_t m_deadline = 0;
		int64_t m_lastPacedTicks = 0;
		// Wall-clock reading the NEXT board frame is due at. Separate from m_deadline: that one is
		// deliberately reset whenever the host falls behind (no debt), and this one deliberately
		// is not (the debt is the whole point). 0 = not started.
		int64_t m_boardDue = 0;
	};

	// The one instance; the patch it writes points at a fixed address, so it cannot be per-host.
	VirtualClock& ModuleClock();
}
