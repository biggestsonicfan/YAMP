#include "NetPlugin.h"

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../DebugLog.h"
#include "../YAMPGeneral.h"
#include "../m2ftg/m2ftg.h"

namespace net
{
    namespace
    {
        HMODULE s_module = nullptr;
        const yampnet_api* s_api = nullptr;
        yampnet_session* s_session = nullptr;
        char s_error[256] = {};

        // Netplay gets its OWN log file, appended and CLOSED per line.
        // DebugLogFile holds d3d12_debug.log open exclusively for the life of the process, so it
        // cannot be read while YAMP runs - which is useless for a two-machine test where the whole
        // point is watching both sides live (and reading the host's room id before starting the
        // guest). This one can be tailed over a network share while the game is running.
        void NetLog(const char* fmt, ...)
        {
            char line[512];
            va_list args;
            va_start(args, fmt);
            vsnprintf(line, sizeof(line), fmt, args);
            va_end(args);

            DebugLog("[net] %s", line);   // still goes to the debugger

            // NEXT TO YAMP.exe, not in the current directory.
            //
            // A relative path puts this wherever the running game happens to have set its CWD,
            // and that differs per game: the VF2 host runs from the folder holding YAMP.exe, but
            // the LJ games run from the directory holding their own module DLL - inside the
            // parent game's install. So Sonic the Fighters' log was landing somewhere nobody
            // would think to look, which matters because a failed session tells the player to
            // "see yampnet.log".
            static const std::string logPath = []
            {
                wchar_t exe[MAX_PATH] = {};
                if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
                {
                    return std::string("yampnet.log");   // no worse than before
                }
                std::filesystem::path p(exe);
                p.replace_filename(L"yampnet.log");
                return p.string();
            }();

            if (FILE* f = nullptr; fopen_s(&f, logPath.c_str(), "a") == 0 && f)
            {
                fprintf(f, "[%8lu] %s\n", GetTickCount(), line);
                fclose(f);
            }
        }

        void SetError(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(s_error, sizeof(s_error), fmt, args);
            va_end(args);
            NetLog("%s", s_error);
        }

        void PluginLog(void* /*ctx*/, yampnet_log_level level, const char* msg)
        {
            static const char* kNames[] = { "debug", "info", "warn", "error" };
            const char* name = (level >= 0 && level <= YAMPNET_LOG_ERROR) ? kNames[level] : "?";
            NetLog("[%s] %s", name, msg ? msg : "");
        }

        // Filled from YAMP's own headers. The plugin compares these against the values it was
        // built with and refuses to run on a mismatch - it writes execute_info.pad[] directly,
        // so a shifted layout would be silent memory corruption rather than a visible bug.
        yampnet_layout BuildLayout()
        {
            using m2ftg::m2ftg_execute_info_t;
            using m2ftg::m2ftg_pad_t;

            yampnet_layout l = {};
            l.execute_info_size = static_cast<uint32_t>(sizeof(m2ftg_execute_info_t));
            l.pad_size = static_cast<uint32_t>(sizeof(m2ftg_pad_t));
            l.pad0_offset = static_cast<uint32_t>(offsetof(m2ftg_execute_info_t, pad));
            l.pad1_offset = static_cast<uint32_t>(offsetof(m2ftg_execute_info_t, pad[1]));
            l.pad_buttons_offset = static_cast<uint32_t>(offsetof(m2ftg_pad_t, m_buttons));
            l.pad_port_offset = static_cast<uint32_t>(offsetof(m2ftg_pad_t, m_port));
            return l;
        }

        // yampnet.dll lives next to YAMP.exe. Deliberately NOT searched for on PATH: a netplay
        // plugin picked up from elsewhere on the system is not something we want to load.
        bool BuildPluginPath(wchar_t* out, size_t count)
        {
            wchar_t exePath[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return false;

            wchar_t* slash = wcsrchr(exePath, L'\\');
            if (!slash)
                return false;
            *(slash + 1) = L'\0';

            if (swprintf(out, count, L"%s%s", exePath, YAMPNET_DLL_NAME) < 0)
                return false;
            return true;
        }
    }

    bool Load()
    {
        if (s_session)
            return true;    // already up
        if (s_module)
            return false;   // already tried and failed; don't thrash LoadLibrary

        wchar_t path[MAX_PATH * 2] = {};
        if (!BuildPluginPath(path, _countof(path)))
        {
            SetError("could not resolve the YAMP.exe directory");
            return false;
        }

        // Absent DLL is the EXPECTED case in a release without netplay - log it quietly and
        // leave no error text, so the settings UI reports "unavailable" rather than "broken".
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        {
            NetLog("no netplay plugin present; netplay disabled");
            return false;
        }

        s_module = LoadLibraryW(path);
        if (!s_module)
        {
            SetError("LoadLibrary failed (error %lu)", GetLastError());
            return false;
        }

        auto getApi = reinterpret_cast<YampNet_GetApiFn>(
            GetProcAddress(s_module, YAMPNET_GETAPI_SYMBOL));
        if (!getApi)
        {
            SetError("plugin exports no %hs", YAMPNET_GETAPI_SYMBOL);
            return false;
        }

        s_api = getApi(YAMPNET_ABI_VERSION);
        if (!s_api)
        {
            SetError("plugin does not support ABI %u", YAMPNET_ABI_VERSION);
            return false;
        }
        // Belt and braces: a plugin could hand back a table built against another version.
        if (s_api->abi_version != YAMPNET_ABI_VERSION)
        {
            SetError("plugin ABI %u, YAMP expects %u", s_api->abi_version, YAMPNET_ABI_VERSION);
            s_api = nullptr;
            return false;
        }

        yampnet_host host = {};
        host.abi_version = YAMPNET_ABI_VERSION;
        host.layout = BuildLayout();
        host.log = &PluginLog;
        host.log_ctx = nullptr;

        yampnet_result err = YAMPNET_OK;
        s_session = s_api->create(&host, &err);
        if (!s_session)
        {
            SetError(err == YAMPNET_ERR_LAYOUT
                         ? "plugin was built against different m2ftg struct layouts (rebuild it)"
                         : "plugin create() failed (%d)",
                     static_cast<int>(err));
            s_api = nullptr;
            return false;
        }

        NetLog("netplay plugin loaded (ABI %u)", YAMPNET_ABI_VERSION);
        s_error[0] = '\0';
        return true;
    }

    void Unload()
    {
        if (s_api && s_session)
        {
            // Leave the room and log out DELIBERATELY before tearing the session down. The server
            // does clean up on its own when the connection drops (Client::drop ->
            // clean_user_state removes the user from their rooms and deletes the room once it is
            // empty), but YAMP exits via TerminateProcess, so "the socket eventually closed" is a
            // poor thing to rely on - this makes the room disappear at once, and on purpose.
            s_api->leave_room(s_session);
            s_api->disconnect(s_session);
            s_api->destroy(s_session);
        }
        s_session = nullptr;
        s_api = nullptr;

        if (s_module)
        {
            FreeLibrary(s_module);
            s_module = nullptr;
        }
    }

    namespace
    {
        SessionConfig s_cfg;
        bool s_startRequested = false;
        bool s_parsed = false;
        bool s_connectSent = false;
        bool s_roomSent = false;
        // Set by EndSession(): stops DriveSession re-establishing a command-line session after
        // the player has deliberately ended it. Declared here because DriveSession is below.
        bool s_sessionEnded = false;

        // Copies the whitespace-delimited token following `flag` out of the command line.
        bool ArgStr(const wchar_t* cmd, const wchar_t* flag, char* out, size_t cap)
        {
            const wchar_t* at = wcsstr(cmd, flag);
            if (!at)
                return false;
            at += wcslen(flag);
            while (*at == L' ') ++at;
            size_t n = 0;
            while (*at && *at != L' ' && n + 1 < cap)
                out[n++] = static_cast<char>(*at++);   // ASCII: hosts, npids and hex only
            out[n] = '\0';
            return n != 0;
        }
    }

    void ParseCommandLine()
    {
        if (s_parsed)
            return;
        s_parsed = true;

        // The ACCOUNT comes from the settings file, the ACTION from the command line.
        //
        // Credentials used to have to be repeated on the command line, which meant the two-machine
        // harness carried a password in a .cmd file and the settings page's copy was ignored - two
        // sources of truth for the same account. Now the settings are the source and each -net-*
        // switch merely overrides one field, so `YAMP.exe -stf -net-host` is a complete command.
        // Main.cpp loads the settings before calling this, so they are already available.
        if (const YAMPSettings* s = gGeneral.GetSettings())
        {
            strncpy_s(s_cfg.server, s->m_netServer.c_str(), _TRUNCATE);
            strncpy_s(s_cfg.npid, s->m_netNpid.c_str(), _TRUNCATE);
            strncpy_s(s_cfg.password, s->m_netToken.c_str(), _TRUNCATE);
            strncpy_s(s_cfg.fingerprint, s->m_netCertFingerprint.c_str(), _TRUNCATE);
            if (!s->m_netComId.empty())
                strncpy_s(s_cfg.com_id, s->m_netComId.c_str(), _TRUNCATE);
        }

        const wchar_t* cmd = GetCommandLineW();
        ArgStr(cmd, L"-net-server", s_cfg.server, sizeof(s_cfg.server));
        ArgStr(cmd, L"-net-user", s_cfg.npid, sizeof(s_cfg.npid));
        ArgStr(cmd, L"-net-pass", s_cfg.password, sizeof(s_cfg.password));
        ArgStr(cmd, L"-net-fp", s_cfg.fingerprint, sizeof(s_cfg.fingerprint));
        ArgStr(cmd, L"-net-comid", s_cfg.com_id, sizeof(s_cfg.com_id));

        s_cfg.host = wcsstr(cmd, L"-net-host") != nullptr;
        if (const wchar_t* j = wcsstr(cmd, L"-net-join"))
            s_cfg.room_id = _wcstoui64(j + wcslen(L"-net-join"), nullptr, 10);

        // An ACTION is what arms the command-line path. Settings alone must NOT arm it, or every
        // configured machine would auto-host on boot and the lobby would never get a turn.
        s_cfg.enabled = s_cfg.server[0] && s_cfg.npid[0] && s_cfg.password[0]
                     && (s_cfg.host || s_cfg.room_id);
        NetLog("cfg server=%hs user=%hs comid=%hs %hs%llu enabled=%d\n",
                 s_cfg.server, s_cfg.npid, s_cfg.com_id,
                 s_cfg.host ? "host" : "join ", s_cfg.room_id, (int)s_cfg.enabled);
    }

    const SessionConfig& Config() { return s_cfg; }

    // s_cfg.enabled is set only by ParseCommandLine, so it doubles as "this session came from the
    // command line" - the UI path leaves it false and drives the flag instead.
    bool ShouldStartRound() { return s_cfg.enabled || s_startRequested; }
    void RequestStartRound() { s_startRequested = true; }
    void ClearStartRequest() { s_startRequested = false; }

    void Logf(const char* fmt, ...)
    {
        char line[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        NetLog("%s", line);
    }

    bool DriveSession()
    {
        if (!IsAvailable() || !s_cfg.enabled || s_sessionEnded)
            return false;

        const yampnet_state st = s_api->get_state(s_session);
        if (st == YAMPNET_STATE_FAILED)
            return false;

        // 1. Log in (which also runs server/world discovery inside the plugin).
        if (!s_connectSent && st == YAMPNET_STATE_IDLE)
        {
            yampnet_rpcn_config rc = {};
            rc.server = s_cfg.server;
            rc.port = 0;                     // plugin default (31313)
            rc.npid = s_cfg.npid;
            rc.token = s_cfg.password;
            rc.communication_id = s_cfg.com_id;
            rc.cert_fingerprint = s_cfg.fingerprint[0] ? s_cfg.fingerprint : nullptr;

            if (s_api->connect(s_session, &rc) != YAMPNET_OK)
            {
                NetLog("connect failed: %s\n", s_api->get_error(s_session));
                return false;
            }
            s_connectSent = true;
            NetLog("connecting to %hs as %hs\n", s_cfg.server, s_cfg.npid);
        }

        // 2. Once online, take a room. Host creates; guest joins the id it was given.
        if (!s_roomSent && st == YAMPNET_STATE_ONLINE)
        {
            yampnet_result r;
            if (s_cfg.host)
            {
                yampnet_room_config rcfg = {};
                rcfg.max_players = 2;
                // The harness host publishes its own dip switches too, so a -net-host / -net-join
                // pair is played under the same cabinet settings the lobby would have produced.
                if (const YAMPSettings* set = gGeneral.GetSettings();
                    set != nullptr && set->m_m2RealDamage)
                {
                    rcfg.game_flags |= YAMPNET_ROOM_FLAG_REAL_DAMAGE;
                }
                r = s_api->create_room(s_session, &rcfg);
            }
            else
            {
                r = s_api->join_room(s_session, s_cfg.room_id, nullptr);
            }

            if (r != YAMPNET_OK)
            {
                NetLog("%hs failed: %s\n", s_cfg.host ? "create_room" : "join_room",
                         s_api->get_error(s_session));
                return false;
            }
            s_roomSent = true;
            NetLog("%hs requested\n", s_cfg.host ? "create_room" : "join_room");
        }

        return true;
    }

    // ---- Lobby ------------------------------------------------------------------------------

    namespace
    {
        char s_actionError[256] = {};
        char s_peerLostReason[128] = {};
        bool s_peerLost = false;
        // Set by the UI path only, so GetStatus can tell "nothing has been started" from "idle
        // because the user disconnected" without asking the plugin.
        bool s_uiStarted = false;

        void SetActionError(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(s_actionError, sizeof(s_actionError), fmt, args);
            va_end(args);
            NetLog("ui: %s", s_actionError);
        }

        void CopyArg(char* dst, size_t cap, const char* src)
        {
            if (src == nullptr)
            {
                dst[0] = '\0';
                return;
            }
            strncpy_s(dst, cap, src, _TRUNCATE);
        }

        // Both entry points into the session share it, so a command-line session must not be
        // steered from the lobby half way through.
        bool UiMayAct()
        {
            if (!IsAvailable())
            {
                SetActionError("the netplay plugin is not loaded");
                return false;
            }
            if (s_cfg.enabled)
            {
                SetActionError("this session was started from the command line");
                return false;
            }
            return true;
        }
    }

    const char* LastActionError() { return s_actionError; }

    bool SessionInProgress()
    {
        if (!IsAvailable())
            return false;
        switch (s_api->get_state(s_session))
        {
        case YAMPNET_STATE_IN_ROOM:
        case YAMPNET_STATE_SYNCING:
        case YAMPNET_STATE_IN_MATCH:
            return true;
        default:
            return false;
        }
    }

    void NotePeerLost(const char* reason)
    {
        if (s_peerLost)
            return;   // keep the FIRST reason; later ones are consequences
        s_peerLost = true;
        strncpy_s(s_peerLostReason, reason ? reason : "connection lost", _TRUNCATE);
        NetLog("peer lost: %hs", s_peerLostReason);
    }

    void ClearPeerLost()
    {
        s_peerLost = false;
        s_peerLostReason[0] = 0;
    }

    void EndSession()
    {
        if (IsAvailable())
        {
            s_api->leave_room(s_session);
            s_api->disconnect(s_session);
        }
        s_sessionEnded = true;
        s_uiStarted = false;
        s_connectSent = false;
        s_roomSent = false;
        s_startRequested = false;
        s_cfg.host = false;
        s_cfg.room_id = 0;
        ClearPeerLost();
        NetLog("session ended by the player");
    }

    Status GetStatus()
    {
        Status st;
        if (!IsAvailable())
        {
            st.text = "The netplay plugin (yampnet.dll) is not loaded.";
            return st;
        }

        st.state = s_api->get_state(s_session);
        st.room_id = s_api->get_room_id(s_session);
        st.local_player = s_api->get_local_player(s_session);
        st.stall_count = s_api->get_stall_count(s_session);
        st.started = s_cfg.enabled || s_uiStarted;
        st.hosting = s_cfg.host;
        st.peer_lost = s_peerLost;
        st.peer_lost_reason = s_peerLostReason;
        st.error = s_api->get_error(s_session);
        st.real_damage =
            (s_api->get_room_flags(s_session) & YAMPNET_ROOM_FLAG_REAL_DAMAGE) != 0;
        st.vf2_version20 =
            (s_api->get_room_flags(s_session) & YAMPNET_ROOM_FLAG_VF2_VERSION20) != 0;

        uint32_t dFrame = 0, dLocal = 0, dRemote = 0;
        if (s_api->get_desync(s_session, &dFrame, &dLocal, &dRemote) != 0)
        {
            st.desynced = true;
            st.desync_frame = dFrame;
            st.desync_local = dLocal;
            st.desync_remote = dRemote;
        }

        switch (st.state)
        {
        case YAMPNET_STATE_CONNECTING: st.text = "Connecting to the netplay server..."; break;
        case YAMPNET_STATE_ONLINE:     st.text = "Logged in. Host a room, or join one by ID."; break;
        // The transport learns a peer exists only when its first datagram arrives, and a guest
        // sends nothing until it starts a round - so "in a room" genuinely cannot mean "someone
        // else is here". Saying so is better than inventing a player count the protocol has not
        // got.
        case YAMPNET_STATE_IN_ROOM:    st.text = "In a room. Both players press Start match."; break;
        case YAMPNET_STATE_SYNCING:    st.text = "Waiting for the other player to start..."; break;
        case YAMPNET_STATE_IN_MATCH:   st.text = "Match running."; break;
        case YAMPNET_STATE_FAILED:     st.text = "Netplay failed - see yampnet.log."; break;
        default:                       st.text = st.started ? "Disconnected." : "Not connected."; break;
        }
        return st;
    }

    bool Connect(const char* server, const char* npid, const char* token,
                 const char* fingerprint, const char* comId)
    {
        if (!UiMayAct())
            return false;
        if (server == nullptr || *server == '\0' || npid == nullptr || *npid == '\0'
            || token == nullptr || *token == '\0')
        {
            SetActionError("server, account and password are all required");
            return false;
        }

        // Copied into the session config rather than used in place: the caller's strings are
        // ImGui edit buffers that it is free to reuse, and the overlay reads these later.
        CopyArg(s_cfg.server, sizeof(s_cfg.server), server);
        CopyArg(s_cfg.npid, sizeof(s_cfg.npid), npid);
        CopyArg(s_cfg.password, sizeof(s_cfg.password), token);
        CopyArg(s_cfg.fingerprint, sizeof(s_cfg.fingerprint), fingerprint);
        if (comId != nullptr && *comId != '\0')
            CopyArg(s_cfg.com_id, sizeof(s_cfg.com_id), comId);

        yampnet_rpcn_config rc = {};
        rc.server = s_cfg.server;
        rc.port = 0;                     // plugin default (31313)
        rc.npid = s_cfg.npid;
        rc.token = s_cfg.password;
        rc.communication_id = s_cfg.com_id;
        rc.cert_fingerprint = s_cfg.fingerprint[0] ? s_cfg.fingerprint : nullptr;

        if (s_api->connect(s_session, &rc) != YAMPNET_OK)
        {
            SetActionError("%s", s_api->get_error(s_session));
            return false;
        }

        s_uiStarted = true;
        s_connectSent = true;            // keeps DriveSession out of the way for good measure
        s_actionError[0] = '\0';
        NetLog("ui: connecting to %hs as %hs", s_cfg.server, s_cfg.npid);
        return true;
    }

    void Disconnect()
    {
        if (!IsAvailable() || s_cfg.enabled)
            return;

        s_api->disconnect(s_session);
        s_uiStarted = false;
        s_connectSent = false;
        s_roomSent = false;
        s_startRequested = false;
        s_cfg.host = false;
        s_cfg.room_id = 0;
        NetLog("ui: disconnected");
    }

    bool HostRoom(const char* password, bool realDamage, bool vf2Version20)
    {
        if (!UiMayAct())
            return false;

        yampnet_room_config rcfg = {};
        rcfg.max_players = 2;
        rcfg.is_private = (password != nullptr && *password != '\0') ? 1u : 0u;
        rcfg.password = rcfg.is_private ? password : nullptr;
        rcfg.forced_seed = 0;            // the plugin generates and distributes the match seed
        // Published once, at creation. The room - not either player's settings file - is what the
        // match is played under from here on.
        rcfg.game_flags = (realDamage ? YAMPNET_ROOM_FLAG_REAL_DAMAGE : 0u)
                        | (vf2Version20 ? YAMPNET_ROOM_FLAG_VF2_VERSION20 : 0u);

        if (s_api->create_room(s_session, &rcfg) != YAMPNET_OK)
        {
            SetActionError("%s", s_api->get_error(s_session));
            return false;
        }
        s_cfg.host = true;
        s_cfg.room_id = 0;               // filled in by the server's reply; read via GetStatus
        s_actionError[0] = '\0';
        NetLog("ui: hosting requested");
        return true;
    }

    bool JoinRoom(unsigned long long roomId, const char* password)
    {
        if (!UiMayAct())
            return false;
        if (roomId == 0)
        {
            SetActionError("enter the room ID the host is showing");
            return false;
        }

        if (s_api->join_room(s_session, roomId,
                             (password != nullptr && *password != '\0') ? password : nullptr)
            != YAMPNET_OK)
        {
            SetActionError("%s", s_api->get_error(s_session));
            return false;
        }
        s_cfg.host = false;
        s_cfg.room_id = roomId;
        s_actionError[0] = '\0';
        NetLog("ui: join %llu requested", roomId);
        return true;
    }

    bool RefreshRooms()
    {
        if (!UiMayAct())
            return false;
        if (s_api->search_rooms(s_session) != YAMPNET_OK)
        {
            SetActionError("could not search for rooms: %s", s_api->get_error(s_session));
            return false;
        }
        s_actionError[0] = '\0';
        return true;
    }

    unsigned int GetRooms(RoomRow* out, unsigned int max_out)
    {
        if (!IsAvailable() || out == nullptr || max_out == 0)
            return 0;

        yampnet_room_info rooms[32];
        const uint32_t n = s_api->get_rooms(s_session, rooms,
                                            max_out < 32u ? max_out : 32u);
        for (uint32_t i = 0; i < n; ++i)
        {
            out[i] = {};
            out[i].room_id = rooms[i].room_id;
            strncpy_s(out[i].owner, rooms[i].name, _TRUNCATE);
            out[i].players = rooms[i].player_count;
            out[i].max_players = rooms[i].max_players;
            out[i].has_password = rooms[i].has_password != 0;
            out[i].real_damage =
                (rooms[i].game_flags & YAMPNET_ROOM_FLAG_REAL_DAMAGE) != 0;
            out[i].vf2_version20 =
                (rooms[i].game_flags & YAMPNET_ROOM_FLAG_VF2_VERSION20) != 0;
        }
        return n;
    }

    bool EffectiveRealDamage(bool localSetting)
    {
        if (!SessionInProgress())
        {
            return localSetting;
        }
        // Read from the plugin every time rather than cached: for a guest the room flags only
        // become known when the join reply lands, and this is called from the emulator's frame
        // loop, which is running long before that.
        return (s_api->get_room_flags(s_session) & YAMPNET_ROOM_FLAG_REAL_DAMAGE) != 0;
    }

    bool EffectiveVf2Version20(bool localSetting)
    {
        if (!SessionInProgress())
        {
            return localSetting;
        }
        return (s_api->get_room_flags(s_session) & YAMPNET_ROOM_FLAG_VF2_VERSION20) != 0;
    }

    void LeaveRoom()
    {
        if (!IsAvailable() || s_cfg.enabled)
            return;

        s_api->leave_room(s_session);
        s_startRequested = false;
        s_cfg.host = false;
        s_cfg.room_id = 0;
        NetLog("ui: left the room");
    }

    bool IsAvailable() { return s_session != nullptr && s_api != nullptr; }
    const yampnet_api* Api() { return s_api; }
    yampnet_session* Session() { return s_session; }
    const char* LoadError() { return s_error; }
}
