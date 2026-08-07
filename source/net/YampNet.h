// YampNet - the ABI between YAMP and the optional netplay plugin (yampnet.dll).
//
// This header is the ONLY thing the two sides share. It is deliberately plain C: no STL, no
// exceptions, no C++ classes across the boundary, so the DLL can be rebuilt (or shipped by
// someone else, or omitted entirely) without recompiling YAMP.
//
// WHY A PLUGIN AT ALL: netplay is expected to churn for a long time after the rest of YAMP is
// stable, and it must be possible to ship a YAMP release with no netcode in it. YAMP therefore
// treats the DLL as strictly optional - if yampnet.dll is missing or refuses to load, netplay
// simply does not exist and no other feature notices (see source/net/NetPlugin.cpp).
//
// DESIGN NOTES (from the Sonic the Fighters PS3 port, NPUB30927, which this mirrors):
//   * The PS3 netcode is DELAY-BASED LOCKSTEP, not rollback. Inputs are keyed by absolute frame
//     number into a 1024-entry per-player ring; the sim consumes frame N once it has every
//     player's input for N. There is no state save/restore anywhere, so none is modelled here.
//   * Every packet redundantly re-carries the last 10 frames of that player's input, so a
//     dropped datagram never stalls the session. yampnet_step is the API expression of this:
//     it can answer WAIT, and YAMP must not advance the emulator on WAIT.
//   * A round begins with a start BARRIER: every peer announces, and only when all have been
//     heard does the per-round frame state reset. Hence YampNet_BeginRound + a generation
//     counter that fences off inputs belonging to a previous round.
//   * DETERMINISM: the emulated ROM's `rand` is an HLE hook fed by a HOST RNG (Mersenne Twister
//     on PS3), which is seeded from hardware entropy and therefore differs per machine. The PS3
//     fixes this by RE-SEEDING from a single shared match seed at round start. That is what
//     YampNet_GetMatchSeed exists for - YAMP must apply it to the rand hook before the round, or
//     the two peers WILL diverge. The seed is delivered by the plugin because only the plugin
//     knows which peer is authoritative; applying it stays in YAMP because it touches m2ftg
//     internals (Patch.cpp) that the plugin has no business reaching into.
//
//     The Model 3 module (pre3) has NO rand hook at all - its board is frame-deterministic by
//     construction - and its single host-varying input is the real-time clock. It therefore
//     applies the same match seed to a different thing: pre3::SetDeterministicClock pins the
//     board's RTC to it. Same contract, same call, different destination; see
//     docs/pre3-netplay.md.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bump on ANY change to the structs or the function table below. The loader refuses a plugin
// whose version does not match exactly - a stale DLL is rejected at load rather than being
// allowed to scribble through a shifted struct.
//
// ABI 8 changes no struct: it strengthens WHEN get_match_seed is promised to be valid (from
// IN_ROOM, not SYNCING). It is a version bump anyway because YAMP now WAITS for that value before
// preparing a round - against an ABI 7 plugin, which publishes nothing until a round begins, a
// guest would sit at Start with no explanation. A rejected DLL says so; a silent wait does not.
// ABI 9 adds the LINKED-CABINET channel (link_ready / link_send / link_take): a raw datagram path
// for a game's own inter-cabinet link, carried over the same P2P socket the lockstep traffic uses
// but touching none of it. Appended to the end of the table and adding no struct, so the bump is
// only about the three new entries existing - but that is exactly what a stale plugin would not
// have, and calling through a null tail pointer is not a failure mode worth allowing.
// ABI 10 changes no function signature: it changes the linked-cabinet channel's WIRE format,
// which now carries an RLE-coded payload (the raw 0x700 fragmented against a 1500-byte MTU). The
// function table is untouched, so this bump exists purely to stop a YAMP.exe and a yampnet.dll
// from different builds pairing up - they would agree on every call and disagree on the bytes.
//
// NOTE what a version cannot do here: it guards YAMP.exe against ITS plugin, not one machine
// against another. Two peers on different builds still have to be updated together. That is
// survivable rather than silent - a mismatched packet decodes to the wrong length, is dropped,
// and the link simply never comes up.
#define YAMPNET_ABI_VERSION 10u

// Looked up next to YAMP.exe. Absent = netplay disabled, which is the normal state of a release
// build until the netcode is ready.
#define YAMPNET_DLL_NAME L"yampnet.dll"

typedef struct yampnet_session yampnet_session;  // opaque

// ---------------------------------------------------------------------------------------------
// Results / state
// ---------------------------------------------------------------------------------------------

typedef enum yampnet_result
{
    YAMPNET_OK = 0,
    YAMPNET_ERR_ABI,             // plugin/host ABI mismatch
    YAMPNET_ERR_LAYOUT,          // m2ftg struct layout handshake failed - see yampnet_layout
    YAMPNET_ERR_STATE,           // call made in the wrong session state
    YAMPNET_ERR_ARG,
    YAMPNET_ERR_NETWORK,
    YAMPNET_ERR_AUTH,            // RPCN login rejected
    YAMPNET_ERR_ROOM,            // create/join/search failed
    YAMPNET_ERR_INTERNAL,
} yampnet_result;

typedef enum yampnet_state
{
    YAMPNET_STATE_IDLE = 0,
    YAMPNET_STATE_CONNECTING,    // TCP/TLS to the RPCN server
    YAMPNET_STATE_ONLINE,        // authenticated, no room
    YAMPNET_STATE_IN_ROOM,       // room joined, peers connecting (P2P signaling)
    YAMPNET_STATE_SYNCING,       // start barrier: waiting for all peers to announce
    YAMPNET_STATE_IN_MATCH,      // stepping frames
    YAMPNET_STATE_FAILED,
} yampnet_state;

// Returned by step(). YAMP MUST NOT advance the emulator unless this is READY.
typedef enum yampnet_step
{
    YAMPNET_STEP_READY = 0,      // all pads written into execute_info; advance one frame
    YAMPNET_STEP_WAIT,           // remote input for this frame not in yet; re-poll, do not advance
    YAMPNET_STEP_TIMEOUT,        // stalled past the configured budget; caller should abort the match
    YAMPNET_STEP_DISCONNECTED,
} yampnet_step;

// ---------------------------------------------------------------------------------------------
// Layout handshake
// ---------------------------------------------------------------------------------------------
//
// The plugin writes execute_info.pad[] directly, so it is coupled to those struct layouts. That
// coupling is the price of keeping pad conversion out of YAMP; this handshake is what stops it
// becoming a memory-corruption bug. YAMP fills every field from offsetof()/sizeof() on its own
// headers, and the plugin compares against the values it was built with. A mismatch fails the
// load loudly instead of silently writing at the wrong offsets.
//
// TWO STRUCT FAMILIES, ONE HANDSHAKE (since ABI 7). YAMP now hosts two arcade emulators whose
// execute_info blocks differ in SIZE - m2ftg's is 0x1760, the Model 3 module's (pre3, Fighting
// Vipers 2 / Sega Rally 2) is 0x1780 with four pad slots and a save-data region - but whose PAD
// GEOMETRY is byte-for-byte identical, because both are the same pxd generation and use the same
// pxd::lj_pad_t at the same two offsets. Since the pads are the only thing the plugin touches,
// the five pad fields below are what get compared EXACTLY, and the size is a BOUND rather than an
// identity: the session is created once, before YAMP knows which game will run, so an equality
// test against one of the two families would refuse the other for no reason.
//
// The host declares the SMALLEST execute_info it may ever hand step(); the plugin's only interest
// is that the pads fit inside it. source/net/NetPlugin.cpp static_asserts the two families'
// agreement, so if a future module moves a pad the build stops rather than the handshake.
typedef struct yampnet_layout
{
    // The smallest execute_info YAMP may pass to step(). NOT an identity - see above.
    uint32_t execute_info_size;      // min(m2ftg 0x1760, pre3 0x1780)   expect 0x1760
    uint32_t pad_size;               // sizeof(pxd::lj_pad_t)             expect 0x190
    uint32_t pad0_offset;            // offsetof(execute_info, pad[0])    expect 0x20
    uint32_t pad1_offset;            // offsetof(execute_info, pad[1])    expect 0x1B0
    uint32_t pad_buttons_offset;     // offsetof(pad, m_buttons)          expect 0xA0
    uint32_t pad_port_offset;        // offsetof(pad, m_port)             expect 0xE0
} yampnet_layout;

typedef enum yampnet_log_level
{
    YAMPNET_LOG_DEBUG = 0,
    YAMPNET_LOG_INFO,
    YAMPNET_LOG_WARN,
    YAMPNET_LOG_ERROR,
} yampnet_log_level;

// Everything YAMP hands the plugin at creation. Passed by pointer and NOT retained: the plugin
// must copy anything it needs (the log callback pointer may be stored).
typedef struct yampnet_host
{
    uint32_t abi_version;            // = YAMPNET_ABI_VERSION
    yampnet_layout layout;
    void* log_ctx;
    void (*log)(void* ctx, yampnet_log_level level, const char* msg);
} yampnet_host;

// ---------------------------------------------------------------------------------------------
// RPCN / room configuration
// ---------------------------------------------------------------------------------------------

typedef struct yampnet_rpcn_config
{
    const char* server;              // RPCN host, e.g. "np.rpcs3.net"
    uint16_t port;
    const char* npid;                // account id
    const char* token;               // auth token / password
    const char* communication_id;    // title comm id used for room scoping
    // SHA-256 of the server certificate as 64 hex chars, or null/empty.
    //
    // EMPTY is the right setting for a server with a real certificate on a real domain: the
    // plugin then validates the chain and host name the way any HTTPS client does.
    //
    // PIN it only for a self-signed server - RPCN's own `--cert-gen` produces a cert with
    // CN="RPCN" and no SAN, which ordinary validation can never accept. Never pin a publicly
    // issued certificate: it is reissued at every renewal (~60 days for Let's Encrypt) and the
    // pin would then reject the server it is meant to protect.
    //
    // Connecting unpinned to a self-signed server fails with the fingerprint named in the error,
    // which is how you obtain the value to pin.
    const char* cert_fingerprint;
} yampnet_rpcn_config;

// ---------------------------------------------------------------------------------------------
// Room game flags
// ---------------------------------------------------------------------------------------------
//
// Cabinet settings that BOTH peers must agree on or the two emulators compute different results
// from identical inputs. They are properties of the ROOM, not of the machine: the host's values
// are published when the room is created and a guest adopts them, exactly as it adopts the match
// seed. A player's own dip-switch settings are ignored for the duration.
//
// ON THE WIRE these ride in RPCN's room `flagAttr` (a u32 attached to the room), and NOT in a
// searchable int attribute, for one reason: flagAttr is the only room field carried by all three
// replies YAMP reads. CreateRoom and JoinRoom return RoomDataInternal (flagAttr = field 10) and
// SearchRoom returns RoomDataExternal (flagAttr = field 14), so the host, the guest and the room
// browser all learn the same value with no extra round trip. Searchable int attrs appear only in
// the search reply, and only when the request lists their ids - a guest joining by ID would never
// see them. The server stores flagAttr verbatim except for SCE_NP_MATCHING2_ROOM_FLAG_ATTR_FULL
// (0x20000000), which it owns and sets when the room fills; every bit below is YAMP's own, well
// clear of the SCE flags (all of which live in the top nibbles), so a stock RPCN server and a
// stock RPCS3 client are unaffected.
#define YAMPNET_ROOM_FLAG_REAL_DAMAGE 0x00000001u  // StF GAME ASSIGNMENTS -> DAMAGE = REAL
#define YAMPNET_ROOM_FLAG_VF2_VERSION20 0x00000002u // VF2 running as version 2.0 (else 2.1)
// m2ftg_config_t.is_vs_mode (config +0x0A). Applies to ALL THREE games and is the strongest case
// of the three flags: it does not merely tune the simulation, it selects a different one. Set, the
// module force-credits both coin counters, skips the attract boot, writes SRAM cabinet mode 3
// instead of 2, and picks the stage from the SECOND host RNG stream (StF 0-8, FV %9, VF2 %11)
// rather than the ROM's fixed sequence. Two peers disagreeing about it are not playing the same
// game, and the one that has it on is drawing from a generator the other never touches.
#define YAMPNET_ROOM_FLAG_VS_MODE 0x00000004u      // m2ftg_config_t.is_vs_mode, config +0x0A
// pre3 (Model 3) only: WHICH STATE A ROUND STARTS FROM. Unlike every flag above this is not a
// cabinet setting at all - it selects between two different reset mechanisms, both of which are
// the module's own:
//
//   clear  the board's PRISTINE POST-BOOT state, captured by YAMP itself through the module's
//          save-state request (machine+0x120 bit 0) before a single guest frame has run. A round
//          therefore starts at power-on and plays forward through the boot, the attract demo and
//          the credit screen - which is what makes the AI observable, and is the closest analogue
//          to what the m2ftg path's ResetBoard does.
//   set    image/fv2/vs_start.bin, the versus start state the module ships and restores for
//          itself in VS mode (machine+0x120 bit 4). Straight into a match, no boot to sit through.
//
// A room property rather than a preference because the two peers must restore the SAME state:
// the whole point of the round-start reset is that both boards begin from identical bytes.
#define YAMPNET_ROOM_FLAG_PRE3_VS_START 0x00000008u
// Adding a bit here needs no ABI bump and no plugin rebuild: the plugin carries game_flags
// verbatim between the room and its peers and never interprets it.

typedef struct yampnet_room_config
{
    uint16_t max_players;            // the PS3 relay walks 8 peer slots; 2 for a StF match
    uint8_t  is_private;
    const char* password;            // NULL when public
    // Word 0 of the PS3 match-settings block is the RNG seed. Pass 0 to let the plugin generate
    // one on the host and distribute it; a non-zero value forces it (useful for replay/debug).
    uint32_t forced_seed;
    // YAMPNET_ROOM_FLAG_* for the room being created. The host's cabinet settings become the
    // room's, and every peer plays under them.
    uint32_t game_flags;
} yampnet_room_config;

typedef struct yampnet_room_info
{
    uint64_t room_id;
    // The owner's account name. RPCN rooms have no title of their own, so this is what identifies
    // a room to a human - the search reply carries the owner's UserInfo for exactly this purpose.
    char     name[64];
    uint16_t player_count;
    uint16_t max_players;
    uint32_t ping_ms;                // 0 = unknown; RPCN's room list carries no timing
    // True when the room is password-gated. Derived from privateSlotNum in the search reply: RPCN
    // has no "has password" flag, but a room created with a password marks its slots private, and
    // a joiner without the password can only take a PUBLIC slot - so private slots ARE the lock.
    uint8_t  has_password;
    // YAMPNET_ROOM_FLAG_* the host created this room with, so the browser can show what the match
    // would actually be played under before anyone joins.
    uint32_t game_flags;
} yampnet_room_info;

// Tunables. Frame delay is the lockstep input delay in frames; the PS3 packet carries 10 frames
// of redundancy, which bounds how much loss can be absorbed without a stall.
typedef struct yampnet_match_config
{
    uint8_t  frame_delay;            // typical 2-4
    uint8_t  input_redundancy;       // frames of input re-sent per packet; PS3 used 10
    uint32_t stall_timeout_ms;       // step() returns TIMEOUT past this
    // --- ABI 7 ---
    // How submit_state_check's values are to be COMPARED, which depends on what they are and only
    // YAMP knows that.
    //
    //   0  COUNTER. The plugin baselines the difference between the peers on the first comparable
    //      frame and then watches for that difference to CHANGE. Right for Sonic the Fighters and
    //      Fighting Vipers, which submit the ROM's own frame_counter: the two boards need not
    //      re-enter a round at the same absolute count, and a constant offset is harmless because
    //      both still advance one per frame.
    //
    //   1  EXACT. The values must be EQUAL. Required for anything that submits a HASH - Virtua
    //      Fighter 2 and the Model 3 boards both do - because the difference between two hashes is
    //      meaningless: it is not constant when the peers agree, so baselining it would either
    //      report a desync on the frame after the baseline or, if the baseline itself were taken
    //      from a disagreeing pair, treat a genuine divergence as the expected offset.
    uint8_t  state_check_exact;
} yampnet_match_config;

// ---------------------------------------------------------------------------------------------
// Function table
// ---------------------------------------------------------------------------------------------
//
// One export returns this table, so the loader does exactly one GetProcAddress and every later
// addition is a version bump rather than a new symbol to resolve.
typedef struct yampnet_api
{
    uint32_t abi_version;

    yampnet_session* (*create)(const yampnet_host* host, yampnet_result* out_err);
    void (*destroy)(yampnet_session* s);

    // Pump sockets/protocol. Call once per frame regardless of state; cheap when idle.
    yampnet_result (*poll)(yampnet_session* s);
    yampnet_state (*get_state)(yampnet_session* s);
    // Human-readable reason for the last failure; valid until the next call. Never NULL.
    const char* (*get_error)(yampnet_session* s);

    // --- RPCN session (all asynchronous: kick off, then watch get_state()) ---
    yampnet_result (*connect)(yampnet_session* s, const yampnet_rpcn_config* cfg);
    yampnet_result (*disconnect)(yampnet_session* s);

    // --- Rooms ---
    yampnet_result (*create_room)(yampnet_session* s, const yampnet_room_config* cfg);
    // Asks the server for the rooms in this title's world. Asynchronous: call it, keep polling,
    // then read the result with get_rooms. Calling it again while one is in flight is harmless.
    yampnet_result (*search_rooms)(yampnet_session* s);
    // Copies up to max_out entries discovered by the last search_rooms; returns the count.
    uint32_t (*get_rooms)(yampnet_session* s, yampnet_room_info* out, uint32_t max_out);
    yampnet_result (*join_room)(yampnet_session* s, uint64_t room_id, const char* password);
    yampnet_result (*leave_room)(yampnet_session* s);

    // --- Match ---
    // Which execute_info.pad[] index is this machine. -1 until IN_ROOM.
    int32_t (*get_local_player)(yampnet_session* s);
    // The shared match seed. YAMP must feed this to the ROM `rand` HLE hook before the round or
    // the peers diverge.
    //
    // ZERO MEANS "NOT KNOWN YET" and is never a legitimate seed - the host clamps its own away
    // from 0 precisely so this test is unambiguous. A host has the value from room creation; a
    // guest adopts it, and since ABI 8 the host publishes it on a heartbeat from IN_ROOM rather
    // than only on the round announce, so a guest holds it before it can reach Start.
    //
    // Callers that need the seed BEFORE the barrier - m2ftg does, because its board reset makes
    // the ROM re-run an initialisation that draws from the generator - must therefore wait for a
    // non-zero value instead of using what they read. That wait cannot deadlock: the host's
    // heartbeat depends on nothing the guest does.
    uint32_t (*get_match_seed)(yampnet_session* s);

    // Start barrier. Announces this peer for `generation` and resets per-round frame state once
    // every peer has been heard; state becomes IN_MATCH when the barrier releases.
    yampnet_result (*begin_round)(yampnet_session* s, uint32_t generation,
                                  const yampnet_match_config* cfg);
    yampnet_result (*end_round)(yampnet_session* s);

    // THE HOT PATH. Called once per emulated frame with the frame counter and YAMP's live
    // execute_info. The plugin reads the LOCAL player's pad out of it, encodes and sends it, and
    // - once every player's input for `frame` is known - writes ALL players' pads back into
    // execute_info.pad[] and returns READY. On WAIT nothing is written and the caller must
    // re-poll rather than advance. execute_info must be the same object across the whole match
    // (the module keeps state in it).
    yampnet_step (*step)(yampnet_session* s, uint32_t frame, void* execute_info);

    // Diagnostics for the UI/overlay.
    uint32_t (*get_ping_ms)(yampnet_session* s);
    uint32_t (*get_stall_count)(yampnet_session* s);

    // --- ABI 2 ---
    // The room this session is in, or 0 when there is none. The LOBBY needs this: the host has to
    // read its own room id off the screen and pass it to the other player, and there is no room
    // browser to find it with (search_rooms is not implemented on this transport). Before ABI 2
    // the id existed only as a line in yampnet.log, which is fine for a command-line harness and
    // useless for a UI.
    uint64_t (*get_room_id)(yampnet_session* s);

    // --- ABI 3: desync detection ---
    //
    // Lockstep guarantees identical INPUTS; it cannot guarantee the two emulators agree on what
    // those inputs produced. Everything that has gone wrong so far (an unseeded host RNG, a
    // wall-clock texture budget, a board that was never reset) showed up as "the AI is doing
    // different things on the two screens" with no way to say WHEN they parted company.
    //
    // YAMP calls submit_state_check once per executed frame with a value that MUST match on both
    // machines. WHAT that value is, is YAMP's business and varies per game: Sonic the Fighters
    // and Fighting Vipers submit the ROM's frame_counter (emulated 0x500020), which advances
    // exactly once per emulated frame in those titles; Virtua Fighter 2's does not - an
    // interrupt bumps it and can land either side of a module_main boundary - so it submits a
    // hash of work RAM instead. Either way it is one uint32, so the cost here is the same.
    // The plugin carries the most recent one on every input packet and compares it against the
    // peer's for the SAME frame.
    void (*submit_state_check)(yampnet_session* s, uint32_t frame, uint32_t value);

    // Non-zero once the peers have been seen to disagree, filling the frame and both values.
    // LATCHED: only the FIRST disagreement is reported, because everything after it is a
    // consequence rather than a cause. Out parameters may be null.
    int32_t (*get_desync)(yampnet_session* s, uint32_t* out_frame,
                          uint32_t* out_local, uint32_t* out_remote);

    // --- ABI 6: room game flags ---
    //
    // YAMPNET_ROOM_FLAG_* for the room this session is in, or 0 when there is none. For a host
    // these are the values it created the room with; for a guest they are the HOST'S, read out of
    // the join reply. Either way this - not the local setting - is what YAMP must apply to the
    // emulator while the session lasts, which is the whole point of publishing them: a cabinet
    // setting that differs between the peers makes identical inputs produce different games.
    //
    // Deliberately sourced from the room rather than remembered from create_room, so a host that
    // changes its own dip switches mid-session cannot drift away from the room it is hosting.
    uint32_t (*get_room_flags)(yampnet_session* s);

    // ---- Linked-cabinet channel --------------------------------------------------------------
    //
    // A raw datagram path for a game whose HARDWARE has a link, rather than for YAMP's lockstep.
    // Virtual On is the first: two Model 2 cabinets shuttle a 0x700-byte state snapshot over a
    // serial ring every frame, and putting that on the wire is the entire netplay mechanism -
    // the ROM's own protocol then does the synchronising, the waiting and the error handling.
    // There is no lockstep, no barrier, no seed and no determinism requirement on this path.
    //
    // It shares the P2P socket with the lockstep traffic and touches nothing else: not the input
    // rings, not the barrier, not the state-check machinery. Usable from IN_ROOM onwards, because
    // cabinets link during their boot-time network check - long before anyone presses Start - and
    // stay linked between matches.

    // 1 once the peer's address is known and payloads can flow. This is what "is another cabinet
    // out there" should be answered from; the round state is a different question entirely.
    int32_t (*link_ready)(yampnet_session* s);

    // Fire-and-forget. Returns the bytes sent, or 0 if there is nowhere to send yet. Loss is
    // expected and harmless for a payload that is a complete snapshot every frame.
    int32_t (*link_send)(yampnet_session* s, const void* data, uint32_t len);

    // Takes the NEWEST payload received since the last call; returns its length, or 0 when
    // nothing new has arrived. Newest-wins, deliberately: an older datagram carries nothing a
    // newer one does not. 0 means "keep what you have" - never hand the hardware a gap.
    int32_t (*link_take)(yampnet_session* s, void* out, uint32_t cap);
} yampnet_api;

// The single exported symbol. Returns NULL if the plugin cannot satisfy `requested_abi`.
typedef const yampnet_api* (*YampNet_GetApiFn)(uint32_t requested_abi);
#define YAMPNET_GETAPI_SYMBOL "YampNet_GetApi"

#ifdef __cplusplus
}  // extern "C"
#endif
