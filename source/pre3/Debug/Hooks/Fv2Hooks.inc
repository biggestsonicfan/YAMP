// Split out of HleHooks.cpp (2026-08-09), which #includes this inside namespace
// pre3::HleHooks - `Info`, `Kind` and MAX_COUNT come from HleHooks.h.

		// ---- Fighting Vipers 2 -----------------------------------------------------------
		//
		// Table index 3 (M3ERomFv2 + 0x5A8, set by the ROM factory at DLL 0x180038720), which is
		// the table at DLL 0x180108FE0: 36 records.
		//
		// There are no ROM symbols for this board - nothing in the module names a single guest
		// address, and unlike Sonic the Fighters there is no symbolised disassembly to borrow
		// from. So the notes below describe what each HANDLER does, which is knowable exactly,
		// rather than guessing at what the ROM routine was called. Five handler shapes cover the
		// whole table and give the Kind column its meaning:
		//
		//   0x26780  burn the rest of the CPU timeslice, then run the original instruction
		//   0x267E0  call the board object (r3 passed through), then return to the link register
		//   0x26B30  set r4 = 2, then run the original instruction
		//   0x19080  return to the link register immediately - the routine is stubbed out
		//   0x01DC0  a bare `ret` - the instruction is simply deleted
		//
		// Everything from 0x28390 up is a large native routine ending in the link-register
		// return, i.e. a whole ROM function reimplemented in x64. The byte size in each note is
		// that handler's own size, which is the honest measure of how much work it replaces.
		static constexpr Info FV2_HOOKS[] = {
			{ 0x00A218, 0x26780, Kind::Core,    "idle-loop cut-out: yields the rest of the CPU's timeslice, then runs the original instruction" },
			{ 0x009F4C, 0x26780, Kind::Core,    "idle-loop cut-out (second site)" },
			{ 0x009FB4, 0x26780, Kind::Core,    "idle-loop cut-out (third site)" },
			{ 0x0073F8, 0x267E0, Kind::Host,    "routine replaced by a call into the board object with r3, then return" },
			{ 0x007428, 0x267E0, Kind::Host,    "routine replaced by the same board call (second site)" },
			{ 0x007480, 0x267E0, Kind::Host,    "routine replaced by the same board call (third site)" },
			{ 0x00ABF4, 0x26B30, Kind::Patch,   "forces r4 = 2, then runs the original instruction" },
			{ 0x0380E4, 0x19080, Kind::Removed, "routine stubbed out - the handler returns to the link register and does nothing else. WITH HOOK 10 THIS IS THE START-UP WARNING SCREEN: disable both and the board plays it, which is what YAMP ships" },
			{ 0x00B330, 0x28150, Kind::Patch,   "executes the instruction word minus 0x20 instead of the one in the ROM" },
			{ 0x0567B4, 0x01DC0, Kind::Removed, "instruction deleted" },
			{ 0x0567E8, 0x01DC0, Kind::Removed, "instruction deleted. WITH HOOK 7 THIS IS THE START-UP WARNING SCREEN - see hook 7" },
			{ 0x038B8C, 0x28280, Kind::Host,    "fills a guest table at 0x214942 from the halfword at 0x21495E" },
			{ 0x038B84, 0x28180, Kind::Host,    "arcade settings injection: writes the coin setting (free play -> 0x1B) and the difficulty from the YAMP config into the game's own RAM at 0x1205xx" },
			{ 0x02B434, 0x28350, Kind::Patch,   "executes the instruction with its low nibble decremented" },
			{ 0x02BE34, 0x28350, Kind::Patch,   "executes the instruction with its low nibble decremented" },
			{ 0x02C558, 0x28350, Kind::Patch,   "executes the instruction with its low nibble decremented" },
			{ 0x022D60, 0x28390, Kind::Speed,   "native routine, 1276 bytes - heavy double-precision maths over the FPU file" },
			{ 0x022C34, 0x28890, Kind::Speed,   "native routine, 597 bytes - guest memory walk" },
			{ 0x022360, 0x28AF0, Kind::Speed,   "native routine, 339 bytes - square root over guest floats" },
			{ 0x022C70, 0x28C50, Kind::Speed,   "native routine, 1851 bytes - the largest FPU block in the table" },
			{ 0x00266C, 0x29390, Kind::Speed,   "native routine, 343 bytes - FPU only" },
			{ 0x0026A0, 0x294F0, Kind::Speed,   "native routine, 307 bytes - FPU only" },
			{ 0x0026D4, 0x29630, Kind::Speed,   "native routine, 355 bytes - FPU only" },
			{ 0x001FC0, 0x297A0, Kind::Speed,   "native routine, 897 bytes - mixed integer and FPU" },
			{ 0x00201C, 0x29B30, Kind::Speed,   "native routine, 826 bytes - mixed integer and FPU" },
			{ 0x008B04, 0x29E70, Kind::Speed,   "native routine, 617 bytes" },
			{ 0x008BFC, 0x2A0E0, Kind::Speed,   "native routine, 629 bytes" },
			{ 0x002F60, 0x2A360, Kind::Speed,   "native routine, 260 bytes" },
			{ 0x002754, 0x2A470, Kind::Speed,   "native routine, 331 bytes" },
			{ 0x002ECC, 0x2A5C0, Kind::Speed,   "native routine, 405 bytes" },
			{ 0x002E18, 0x2A760, Kind::Speed,   "native routine, 378 bytes" },
			{ 0x002E64, 0x2A8E0, Kind::Speed,   "native routine, 1358 bytes" },
			{ 0x0584A0, 0x2AE30, Kind::Speed,   "native routine, 1976 bytes - the largest handler in the table" },
			{ 0x058914, 0x2B5F0, Kind::Speed,   "native routine, 950 bytes" },
			{ 0x058804, 0x2B9B0, Kind::Speed,   "native routine, 680 bytes" },
			{ 0x058850, 0x2BC60, Kind::Speed,   "native routine, 529 bytes" },
		};
		static_assert(sizeof(FV2_HOOKS) / sizeof(FV2_HOOKS[0]) <= MAX_COUNT);
