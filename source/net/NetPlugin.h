#pragma once

// YAMP-side loader for the optional netplay plugin (yampnet.dll).
//
// Netplay is a plugin precisely so a YAMP release can ship WITHOUT it: if the DLL is absent,
// stale, or refuses to initialise, IsAvailable() stays false, Api() stays null, and every
// netplay entry point in the UI hides itself. Nothing else in YAMP is allowed to hard-depend on
// this header beyond checking IsAvailable().

#include "YampNet.h"

namespace net
{
    // Loads yampnet.dll from the YAMP.exe directory and negotiates the ABI. Safe to call more
    // than once (subsequent calls are no-ops). Never throws; failures are logged and leave the
    // plugin unavailable. Returns true only if a session was created successfully.
    bool Load();

    // Unloads and destroys the session. Called on shutdown; safe when never loaded.
    void Unload();

    // False whenever netplay is unavailable for ANY reason (no DLL, ABI mismatch, layout
    // mismatch, create() failed). This is the only thing the rest of YAMP should test.
    bool IsAvailable();

    // Null unless IsAvailable(). Valid until Unload().
    const yampnet_api* Api();
    yampnet_session* Session();

    // Why the plugin is unavailable, for the settings UI. Empty when it loaded fine.
    const char* LoadError();

    // ---- Session configuration ---------------------------------------------------------------
    // The account is read from the settings file (YAMPSettings::m_net*, filled in by the Netplay
    // page); the command line supplies the ACTION and can override any single field:
    //   -net-host            create a room and wait for a peer
    //   -net-join <roomId>   join an existing room
    //   -net-server <host>   override the configured RPCN server
    //   -net-user <npid>     override the account
    //   -net-pass <secret>
    //   -net-fp <64 hex>     server cert SHA-256; omit for a normally-validated certificate
    //   -net-comid <id>      title comm id, defaults to NPWR02113_00
    // One of -net-host / -net-join is what arms this path; a configured account on its own leaves
    // netplay to the lobby.
    struct SessionConfig
    {
        char server[64] = {};
        char npid[24] = {};
        char password[64] = {};
        char fingerprint[72] = {};
        char com_id[16] = "NPWR02113_00";
        unsigned long long room_id = 0;
        bool host = false;
        bool enabled = false;      // true once -net-server was supplied
    };

    // Fills the config from GetCommandLineW(). Safe to call before Load().
    void ParseCommandLine();
    const SessionConfig& Config();

    // Drives connect -> discovery -> host/join. Call every frame; cheap and idempotent once the
    // room is up. Returns false if netplay is disabled or has failed.
    bool DriveSession();

    // Whether the round may begin now that a room is up.
    //
    // The COMMAND-LINE path auto-starts: it is the two-machine regression harness and has to stay
    // hands-free. A UI-driven session instead sits in the lobby until the host presses Start,
    // which calls RequestStartRound(). Keeping both means adding the lobby cannot silently break
    // the only test that proves the netcode end to end.
    bool ShouldStartRound();
    void RequestStartRound();
    // Clears the request when a round ends, so the next match needs a fresh Start press.
    void ClearStartRequest();

    // WAS THIS PROCESS LAUNCHED FOR NETPLAY? Fixed for the lifetime of the process, deliberately.
    //
    // A linked-cabinet game (Virtual On, and later Motor Raid) runs its second Model 2 board only
    // when the answer is yes: an idle second cabinet is not neutral, it is a peer the ROM waits
    // for, and the operator menu deadlocks against it. Going the other way - bringing a board up
    // mid-session - is the transition that was avoided by running two boards always, and it is
    // still not something to do to a live board. So the answer never changes while the game runs;
    // switching modes means restarting it.
    //
    // Answered from the launch state only (the -net-* harness flags, or netplay being enabled for
    // this launch), NOT from whether a session happens to be up right now.
    bool WantsNetplayBoards();

    // ---- Lobby (YAMP Settings -> Netplay) ----------------------------------------------------
    //
    // The command-line path above drives itself end to end; these are the same steps under
    // explicit control, one click at a time. Both paths share one session, so only one may be in
    // use at a time - the UI refuses to act while Config().enabled is set.

    // Everything the lobby renders, snapshotted so the UI never touches the plugin ABI directly.
    struct Status
    {
        yampnet_state state = YAMPNET_STATE_IDLE;
        unsigned long long room_id = 0;
        int local_player = -1;         // -1 until a room is taken; 0 = host, 1 = guest
        unsigned int stall_count = 0;
        bool started = false;          // a connect has been issued (either path)
        bool hosting = false;
        // Set when the other player vanished mid-match. The UI turns this into a dialog whose OK
        // button calls EndSession(); it stays latched until then, because a message that clears
        // itself is a message the player never sees.
        bool peer_lost = false;
        const char* peer_lost_reason = "";
        // Set once the two emulators have been caught disagreeing (see submit_state_check).
        bool desynced = false;
        unsigned int desync_frame = 0;
        unsigned int desync_local = 0;
        unsigned int desync_remote = 0;
        const char* text = "";         // one line describing `state`; never null
        const char* error = "";        // the plugin's last failure text; "" when none
        // The cabinet settings this room is played under - the host's, adopted by every peer.
        // Meaningless unless there is a room; the lobby only shows them from IN_ROOM onwards.
        bool real_damage = false;
        // VF2 only: the room is played as version 2.0 when set, 2.1 when clear. The two
        // versions are different games mechanically, so a mismatch desyncs.
        bool vf2_version20 = false;
        // All three games: the room plays in VS mode (m2ftg_config_t.is_vs_mode). Selects a
        // different simulation, not a variant of one - see YAMPNET_ROOM_FLAG_VS_MODE.
        bool vs_mode = false;
        // pre3 only: the room starts its rounds from the board's shipped VERSUS START state when
        // set, and from its power-on state when clear. Not a cabinet setting - it selects which
        // of the module's two reset mechanisms the round uses. See YAMPNET_ROOM_FLAG_PRE3_VS_START.
        bool pre3_vs_start = false;
    };
    Status GetStatus();

    // Each returns false and fills LastActionError() if the plugin rejected the call. Progress is
    // asynchronous: watch GetStatus().state, exactly as DriveSession does.
    bool Connect(const char* server, const char* npid, const char* token,
                 const char* fingerprint, const char* comId);
    void Disconnect();
    // `realDamage` is this machine's DAMAGE dip switch, published as the room's. Everyone who
    // joins plays under it - see EffectiveRealDamage.
    bool HostRoom(const char* password, bool realDamage, bool vf2Version20, bool vsMode,
                  bool pre3VsStart);
    bool JoinRoom(unsigned long long roomId, const char* password);

    // One row of the room browser, flattened so the UI never sees the plugin's types.
    struct RoomRow
    {
        unsigned long long room_id = 0;
        char owner[64] = {};
        unsigned int players = 0;
        unsigned int max_players = 0;
        bool has_password = false;
        // The host's DAMAGE setting, shown in the browser so the room can be judged before
        // joining it: it is not a preference the joiner gets to keep, it is how that match plays.
        bool real_damage = false;
        // The host's VF2 version, shown for the same reason: it is how that match plays.
        bool vf2_version20 = false;
        // The host's VS-mode switch, shown for the same reason.
        bool vs_mode = false;
        // The host's pre3 round-start choice, shown for the same reason: it decides whether a
        // round begins at power-on or already in a match.
        bool pre3_vs_start = false;
    };
    // Kicks off a search (asynchronous - the list refreshes when the reply lands).
    bool RefreshRooms();
    // Copies the most recent results; returns how many were written.
    unsigned int GetRooms(RoomRow* out, unsigned int max_out);
    void LeaveRoom();
    const char* LastActionError();

    // True while this machine is in a netplay room - lobby, start barrier or live match.
    //
    // ANY feature that can touch the emulated board, the frame pacing or the HLE hook table must
    // be inert while this holds. The two peers only stay in step because they run identical code
    // over identical inputs, so a local board reset (the DLL's DEBUG MENU has one), a locally
    // paused emulator, or a hook toggled on one side is an instant desync that looks like a
    // network fault. There is no way to make these safe per-feature: the answer is to switch them
    // off for the duration.
    bool SessionInProgress();

    // The DAMAGE setting the emulator must actually run under this frame: the ROOM'S while a
    // session is up, and `localSetting` otherwise.
    //
    // This is the one function that decides it, so no caller can accidentally apply a local
    // preference to a networked match. Cabinet settings are not per-player: they change what the
    // ROM computes from a hit, so two peers disagreeing on one produce different games from
    // identical inputs and the desync canary fires within seconds. The room's value is
    // authoritative for the host too - it changes nothing for a host whose switch has not moved,
    // and it stops a mid-session switch flip from silently splitting the pair.
    bool EffectiveRealDamage(bool localSetting);
    // The same rule for VF2's version flag, and for the same reason: 2.0 and 2.1 are different
    // games mechanically, so two peers on different versions diverge from identical inputs.
    bool EffectiveVf2Version20(bool localSetting);
    // The same rule for VS mode, which needs it more than either: it decides whether the module
    // credits both players and draws the stage from the second RNG stream at all. A peer with it
    // off never touches that generator, so a mismatch is not a difference of degree.
    bool EffectiveVsMode(bool localSetting);
    // The same rule for the pre3 round-start state. Not a cabinet setting but subject to the same
    // logic and for a stronger reason than any of them: the two peers must restore the SAME state,
    // or the round starts from two different boards and every later frame is meaningless.
    bool EffectivePre3VsStart(bool localSetting);

    // Called by the host loop when a round ends because the other player went away. Latches the
    // reason for the dialog; harmless to call repeatedly.
    void NotePeerLost(const char* reason);
    void ClearPeerLost();

    // Full teardown: leaves RPCN and stops BOTH drivers, so neither the lobby nor the command-line
    // path re-establishes the session behind the player's back. This is what the dialog's OK does;
    // unlike Disconnect() it deliberately applies to a command-line session as well, because by
    // then the match is over either way.
    void EndSession();

    // ---- Linked-cabinet link (Virtual On, and later Motor Raid / Sega Rally 2) ----------------
    //
    // The game's OWN inter-cabinet link, carried over the RPCN session's P2P socket. Nothing on
    // this path is lockstep: no barrier, no seed, no frame numbering and no determinism
    // requirement. The payload is the emulated comm board's 0x700-byte window, and the ROM's own
    // protocol does the synchronising - which is why it needs to flow from the moment a room
    // exists, not from the moment a round starts. See yampnet_api::link_ready.
    //
    // These are thin passthroughs, and each answers false / 0 when there is no plugin, no session
    // or no peer yet, so a caller can use them unconditionally.

    // Is a peer reachable right now? The honest input to "is the ring up".
    bool LinkReady();
    // Ship this frame's outgoing payload. Fire-and-forget; loss is expected and harmless.
    void LinkSend(const void* data, unsigned int len);
    // Take the newest payload since the last call. Returns 0 when nothing new arrived, which
    // means "keep the one you already have" rather than "there is a gap".
    unsigned int LinkTake(void* out, unsigned int cap);

    // Appends one printf-style line to yampnet.log next to the CWD. Opened and CLOSED per line,
    // so it can be read live over a share while YAMP runs - unlike d3d12_debug.log, which
    // DebugLogFile holds open exclusively for the whole process.
    void Logf(const char* fmt, ...);
}
