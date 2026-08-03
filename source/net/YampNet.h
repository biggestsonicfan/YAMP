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

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bump on ANY change to the structs or the function table below. The loader refuses a plugin
// whose version does not match exactly - a stale DLL is rejected at load rather than being
// allowed to scribble through a shifted struct.
#define YAMPNET_ABI_VERSION 6u

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
// The plugin writes m2ftg's execute_info.pad[] directly, so it is coupled to those struct
// layouts. That coupling is the price of keeping pad conversion out of YAMP; this handshake is
// what stops it becoming a memory-corruption bug. YAMP fills every field from offsetof()/sizeof()
// on its own headers, and the plugin compares against the values it was built with. A mismatch
// fails the load loudly instead of silently writing at the wrong offsets.
typedef struct yampnet_layout
{
    uint32_t execute_info_size;      // sizeof(m2ftg_execute_info_t)      expect 0x1760
    uint32_t pad_size;               // sizeof(m2ftg_pad_t)               expect 0x190
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
    // The shared match seed. Valid once state is SYNCING or IN_MATCH. YAMP must feed this to the
    // ROM `rand` HLE hook before the round or the peers diverge.
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
} yampnet_api;

// The single exported symbol. Returns NULL if the plugin cannot satisfy `requested_abi`.
typedef const yampnet_api* (*YampNet_GetApiFn)(uint32_t requested_abi);
#define YAMPNET_GETAPI_SYMBOL "YampNet_GetApi"

#ifdef __cplusplus
}  // extern "C"
#endif
