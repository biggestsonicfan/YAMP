#include "RpcnTransport.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace yampnet
{
    namespace
    {
        // The server drops unauthenticated clients after 10 s and forgets a peer's signaling info
        // if it stops hearing from them, so the keepalive has to keep running for the whole
        // session, not just at login.
        constexpr uint64_t kKeepaliveIntervalMs = 2000;

        // Hole punching while we have not heard back, then a slower heartbeat purely to hold the
        // mapping open. Players can sit in a room for minutes before starting a round, and a NAT
        // will drop an idle UDP mapping in well under that.
        constexpr uint64_t kPunchIntervalMs = 250;
        constexpr uint64_t kHolePunchIdleMs = 1000;

        constexpr uint64_t kSignalingRetryMs = 2000;

        // Not a game packet: shorter than a PacketHeader, so the lockstep layer discards it even
        // if one is delivered. Its only job is to make our NAT create a mapping towards the peer.
        constexpr uint8_t kPunchPacket[4] = { 'Y', 'N', 'P', '!' };
    }

    void RpcnTransport::Fail(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        vsnprintf(m_error, sizeof(m_error), fmt, args);
        va_end(args);
        m_stage = Stage::Failed;
        Note("failed: %s", m_error);
    }

    void RpcnTransport::Note(const char* fmt, ...)
    {
        if (!m_log)
            return;
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        m_log(m_log_ctx, buf);
    }

    const char* RpcnTransport::PeerAddrText() const
    {
        static char text[32];
        if (!m_peer_ip || !m_peer_port)
            return "unknown";
        const uint8_t* o = reinterpret_cast<const uint8_t*>(&m_peer_ip);   // network order
        snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", o[0], o[1], o[2], o[3], m_peer_port);
        return text;
    }

    void RpcnTransport::SetPeer(uint32_t ip, uint16_t port, const char* source)
    {
        if (!ip || !port)
            return;
        const bool changed = (ip != m_peer_ip || port != m_peer_port);
        m_peer_ip = ip;
        m_peer_port = port;
        m_signaling_retry_ms = 0;
        if (m_stage == Stage::Hosting || m_stage == Stage::Joining)
            m_stage = Stage::Linked;
        if (changed)
        {
            // Punch immediately rather than waiting out the interval - this is the moment the
            // hole has to be opened, and the peer may already be sending.
            m_last_punch_ms = 0;
            Note("peer %s at %s (via %s)", m_peer_npid[0] ? m_peer_npid : "?", PeerAddrText(),
                 source);
        }
    }

    bool RpcnTransport::Start(const Config& cfg)
    {
        Stop();

        // Before the first Fail() can happen. Stop() deliberately leaves these alone so a
        // reconnect keeps logging.
        m_log = cfg.log;
        m_log_ctx = cfg.log_ctx;

        if (!cfg.server || !cfg.npid || !cfg.password || !cfg.com_id)
        {
            Fail("RpcnTransport: server, npid, password and com_id are all required");
            return false;
        }

        strncpy_s(m_com_id, cfg.com_id, _TRUNCATE);
        strncpy_s(m_npid, cfg.npid, _TRUNCATE);

        CertFingerprint pin;
        if (cfg.fingerprint_hex && *cfg.fingerprint_hex && !pin.FromHex(cfg.fingerprint_hex))
        {
            Fail("bad certificate fingerprint");
            return false;
        }

        if (!m_client.Connect(cfg.server, cfg.port, pin))
        {
            Fail("connect failed: %s", m_client.LastError());
            return false;
        }

        // The signaling socket must exist before login completes so the keepalive can start as
        // soon as we have a user_id.
        if (!m_client.OpenSignaling(cfg.local_p2p_port))
        {
            Fail("could not open P2P socket: %s", m_client.LastError());
            return false;
        }

        if (!m_client.Login(cfg.npid, cfg.password, ""))
        {
            Fail("login could not be sent: %s", m_client.LastError());
            return false;
        }

        m_stage = Stage::LoggingIn;
        return true;
    }

    void RpcnTransport::Stop()
    {
        m_client.Disconnect();
        m_stage = Stage::Idle;
        m_room_id = 0;
        m_room_flags = 0;
        m_is_host = false;
        m_peer_ip = 0;
        m_peer_port = 0;
        m_peer_heard = false;
        m_peer_npid[0] = '\0';
        m_pending_serverlist = m_pending_worldlist = m_pending_room = m_pending_signaling = 0;
        m_pending_search = 0;
        m_signaling_retry_ms = 0;
        m_last_punch_ms = 0;
        m_room_count = 0;
        m_error[0] = '\0';
    }

    void RpcnTransport::PumpKeepalive()
    {
        if (m_client.UserId() == 0)
            return;
        const uint64_t now = GetTickCount64();
        if (now - m_last_keepalive_ms < kKeepaliveIntervalMs)
            return;
        m_last_keepalive_ms = now;
        m_client.SendSignalingPing();
    }

    void RpcnTransport::PumpPunch()
    {
        if (!PeerKnown())
            return;
        const uint64_t now = GetTickCount64();
        const uint64_t interval = m_peer_heard ? kHolePunchIdleMs : kPunchIntervalMs;
        if (m_last_punch_ms != 0 && now - m_last_punch_ms < interval)
            return;
        m_last_punch_ms = now;
        m_client.SendTo(m_peer_ip, m_peer_port, kPunchPacket, sizeof(kPunchPacket));
    }

    void RpcnTransport::PumpSignalingRetry()
    {
        // Only when we know WHO the peer is but not WHERE. The usual cause is a peer that has not
        // yet been seen by the server's UDP helper, which resolves itself within a keepalive or
        // two - so this quietly retries instead of failing the session.
        if (m_signaling_retry_ms == 0 || PeerKnown() || m_pending_signaling != 0 || !m_peer_npid[0])
            return;
        if (GetTickCount64() < m_signaling_retry_ms)
            return;
        m_signaling_retry_ms = 0;
        m_pending_signaling = m_client.RequestSignalingInfos(m_peer_npid);
    }

    void RpcnTransport::OnNotification(const RpcnPacket& pkt)
    {
        switch (static_cast<RpcnNotification>(pkt.command))
        {
        case RpcnNotification::UserJoinedRoom:
        {
            // The host's cue that it has a peer at all. Everything it needs to start punching is
            // in here, provided the room was created with signaling on.
            char npid[20] = {};
            uint32_t ip = 0;
            uint16_t port = 0;
            bool has_addr = false;
            if (!RpcnClient::ParseJoinedNotification(pkt.payload, pkt.payload_size, npid,
                                                     sizeof(npid), &ip, &port, &has_addr))
                break;
            if (npid[0] && strcmp(npid, m_npid) == 0)
                break;   // our own join echoed back
            if (npid[0])
                strncpy_s(m_peer_npid, npid, _TRUNCATE);

            Note("%s joined the room", m_peer_npid[0] ? m_peer_npid : "a peer");

            if (has_addr && ip && port)
            {
                SetPeer(ip, port, "join notification");
            }
            else if (m_peer_npid[0] && !PeerKnown() && m_pending_signaling == 0)
            {
                // No address in the notification: an older room, or one the server decided needed
                // no signaling. Ask directly - which also makes the server tell the PEER about us.
                m_pending_signaling = m_client.RequestSignalingInfos(m_peer_npid);
            }
            break;
        }

        case RpcnNotification::UserLeftRoom:
        case RpcnNotification::RoomDestroyed:
        {
            if (!PeerKnown() && !m_peer_npid[0])
                break;
            Note("peer left the room");
            m_peer_ip = 0;
            m_peer_port = 0;
            m_peer_heard = false;
            m_peer_npid[0] = '\0';
            m_signaling_retry_ms = 0;
            if (m_stage == Stage::Linked)
                m_stage = m_is_host ? Stage::Hosting : Stage::Joining;
            break;
        }

        case RpcnNotification::SignalingHelper:
        {
            // Pushed to the TARGET of a RequestSignalingInfos, carrying the caller's address. The
            // server sends it for exactly one reason: so the side that was asked about also starts
            // transmitting. Honouring it is what unblocks a room with no signaling of its own.
            char npid[20] = {};
            uint32_t ip = 0;
            uint16_t port = 0;
            if (!RpcnClient::ParseSignalingHelper(pkt.payload, pkt.payload_size, npid,
                                                  sizeof(npid), &ip, &port))
                break;
            if (npid[0] && strcmp(npid, m_npid) == 0)
                break;
            if (m_peer_npid[0] && npid[0] && strcmp(npid, m_peer_npid) != 0)
                break;   // somebody else entirely - not the peer we are in a room with
            if (npid[0])
                strncpy_s(m_peer_npid, npid, _TRUNCATE);
            SetPeer(ip, port, "signaling helper");
            break;
        }

        default:
            break;
        }
    }

    bool RpcnTransport::PumpReplies()
    {
        RpcnPacket pkt;
        while (m_client.Poll(&pkt))
        {
            if (pkt.type == 2)
            {
                OnNotification(pkt);
                continue;
            }
            if (pkt.type != 1)
                continue;   // ServerInfo greeting - nothing to do with it

            const auto cmd = static_cast<RpcnCommand>(pkt.command);

            if (cmd == RpcnCommand::Login)
            {
                if (pkt.error != RpcnError::NoError)
                {
                    Fail("login rejected (ErrorType=%u)", static_cast<unsigned>(pkt.error));
                    return false;
                }
                // Discovery next. With CreateMissing on, this registers the title if new.
                m_pending_serverlist = m_client.GetServerList(m_com_id);
                continue;
            }

            if (pkt.packet_id == m_pending_serverlist)
            {
                m_pending_serverlist = 0;
                uint16_t servers[8];
                const uint32_t n = RpcnClient::ParseServerList(pkt.payload, pkt.payload_size,
                                                              servers, 8);
                if (pkt.error != RpcnError::NoError || !n)
                {
                    Fail("GetServerList failed (ErrorType=%u)", static_cast<unsigned>(pkt.error));
                    return false;
                }
                m_server_id = servers[0];
                m_pending_worldlist = m_client.GetWorldList(m_com_id, m_server_id);
                continue;
            }

            if (pkt.packet_id == m_pending_worldlist)
            {
                m_pending_worldlist = 0;
                uint32_t worlds[8];
                const uint32_t n = RpcnClient::ParseWorldList(pkt.payload, pkt.payload_size,
                                                             worlds, 8);
                if (pkt.error != RpcnError::NoError || !n)
                {
                    Fail("GetWorldList failed (ErrorType=%u)", static_cast<unsigned>(pkt.error));
                    return false;
                }
                m_world_id = worlds[0];
                m_stage = Stage::Online;
                continue;
            }

            if (pkt.packet_id == m_pending_search)
            {
                m_pending_search = 0;
                m_room_count = 0;
                if (pkt.error == RpcnError::NoError)
                {
                    m_room_count = RpcnClient::ParseRoomList(pkt.payload, pkt.payload_size,
                                                             m_rooms, kMaxRooms);
                }
                // A failed or empty search is not a session error - an empty server is the normal
                // state - so it never calls Fail(); the list simply comes back with nothing in it.
                continue;
            }

            if (pkt.packet_id == m_pending_room)
            {
                m_pending_room = 0;
                if (pkt.error != RpcnError::NoError)
                {
                    Fail("room command failed (ErrorType=%u)", static_cast<unsigned>(pkt.error));
                    return false;
                }

                m_room_id = RpcnClient::ParseRoomId(pkt.payload, pkt.payload_size);
                if (!m_room_id)
                {
                    Fail("room reply carried no roomId");
                    return false;
                }

                // Read back rather than assumed, on BOTH sides. For a guest this is the only place
                // the host's cabinet settings arrive; for a host it is the server confirming what
                // it actually stored (it clears the FULL bit it owns), so the two peers end up
                // reading the same word from the same source.
                m_room_flags = RpcnClient::ParseRoomFlagAttr(pkt.payload, pkt.payload_size);

                if (m_is_host)
                {
                    // Wait for a UserJoinedRoom notification, which is what tells us both that a
                    // guest exists and where it is.
                    m_stage = Stage::Hosting;
                }
                else
                {
                    // Find the member that is not us - that is the host we must reach.
                    char members[8][20];
                    const uint32_t n = RpcnClient::ParseRoomMembers(pkt.payload, pkt.payload_size,
                                                                    members, 8);
                    m_peer_npid[0] = '\0';
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        if (strcmp(members[i], m_npid) != 0)
                        {
                            strncpy_s(m_peer_npid, members[i], _TRUNCATE);
                            break;
                        }
                    }
                    if (!m_peer_npid[0])
                    {
                        Fail("joined a room with no other member");
                        return false;
                    }

                    m_stage = Stage::Joining;

                    // The host's address is already in this reply when the room has signaling on,
                    // so the common case needs no extra round trip. Fall back to asking when it is
                    // absent - an older host's room, or one the server chose not to signal.
                    uint32_t ip = 0;
                    uint16_t port = 0;
                    if (RpcnClient::ParseJoinSignalingAddr(pkt.payload, pkt.payload_size, &ip,
                                                           &port)
                        && ip && port)
                    {
                        SetPeer(ip, port, "join reply");
                    }
                    else
                    {
                        m_pending_signaling = m_client.RequestSignalingInfos(m_peer_npid);
                    }
                }
                continue;
            }

            if (pkt.packet_id == m_pending_signaling)
            {
                m_pending_signaling = 0;
                uint32_t ip = 0;
                uint16_t port = 0;
                if (pkt.error != RpcnError::NoError
                    || !RpcnClient::ParseSignalingAddr(pkt.payload, pkt.payload_size, &ip, &port)
                    || !ip || !port)
                {
                    // NOT fatal, and it used to be. A peer that has not yet been seen by the UDP
                    // helper answers NotFound, which is a timing accident rather than a broken
                    // session - it fixes itself within a keepalive or two.
                    Note("no address for '%s' yet (ErrorType=%u); retrying", m_peer_npid,
                         static_cast<unsigned>(pkt.error));
                    m_signaling_retry_ms = GetTickCount64() + kSignalingRetryMs;
                    continue;
                }
                SetPeer(ip, port, "signaling lookup");
                continue;
            }
        }
        return true;
    }

    void RpcnTransport::Update()
    {
        if (m_stage == Stage::Idle || m_stage == Stage::Failed)
            return;

        if (!m_client.IsConnected())
        {
            Fail("disconnected: %s", m_client.LastError());
            return;
        }

        PumpReplies();
        PumpKeepalive();
        PumpSignalingRetry();
        PumpPunch();
    }

    bool RpcnTransport::Host(uint32_t max_slot, const char* password, uint32_t flag_attr)
    {
        if (m_stage != Stage::Online)
        {
            Fail("Host() before discovery finished");
            return false;
        }
        m_is_host = true;
        m_room_flags = 0;   // adopted from the server's reply, like the room id
        m_peer_ip = 0;
        m_peer_port = 0;
        m_peer_heard = false;
        m_peer_npid[0] = '\0';
        m_pending_room = m_client.CreateRoom(m_com_id, m_world_id, max_slot, password, flag_attr);
        return m_pending_room != 0;
    }

    bool RpcnTransport::Join(uint64_t room_id, const char* password)
    {
        if (m_stage != Stage::Online)
        {
            Fail("Join() before discovery finished");
            return false;
        }
        m_is_host = false;
        m_room_flags = 0;
        m_peer_ip = 0;
        m_peer_port = 0;
        m_peer_heard = false;
        m_peer_npid[0] = '\0';
        m_pending_room = m_client.JoinRoom(m_com_id, room_id, password);
        return m_pending_room != 0;
    }

    bool RpcnTransport::Search()
    {
        if (m_stage != Stage::Online)
        {
            Fail("Search requires being logged in and idle");
            return false;
        }
        if (m_pending_search != 0)
            return true;   // one in flight already; its reply will refresh the list

        m_pending_search = m_client.SearchRoom(m_com_id, m_world_id);
        return m_pending_search != 0;
    }

    bool RpcnTransport::Send(uint32_t /*peer*/, const void* data, uint32_t len)
    {
        if (!m_peer_ip || !m_peer_port)
            return false;
        return m_client.SendTo(m_peer_ip, m_peer_port, data, len);
    }

    int RpcnTransport::Recv(void* buf, uint32_t cap, uint32_t* out_peer)
    {
        for (;;)
        {
            uint32_t ip = 0;
            uint16_t port = 0;
            const int got = m_client.RecvFrom(buf, cap, &ip, &port);
            if (got <= 0)
                return 0;

            // Signaling replies share this socket; route them by SOURCE rather than by content,
            // since a signaling reply's leading bytes can look exactly like a game packet header.
            if (m_client.IsSignalingSource(ip, port))
                continue;

            if (!m_peer_heard)
            {
                // First contact. Take the source we actually hear from in preference to the one we
                // were told about: a peer behind a symmetric NAT reaches us from a different port
                // than the server observed, and that address is the only one that can work. Only
                // the port may differ though - a datagram from an unrelated IP is not our peer.
                const bool plausible = !m_peer_ip || ip == m_peer_ip;
                if (!plausible)
                    continue;

                m_peer_heard = true;
                const bool moved = (port != m_peer_port || ip != m_peer_ip);
                m_peer_ip = ip;
                m_peer_port = port;
                if (m_stage == Stage::Hosting || m_stage == Stage::Joining)
                    m_stage = Stage::Linked;
                Note(moved ? "peer reached us from %s (not the advertised port); using that"
                           : "peer link established with %s", PeerAddrText());
            }
            else if (ip != m_peer_ip || port != m_peer_port)
            {
                continue;   // stray datagram from somewhere else - ignore
            }

            // A punch is not game traffic; it exists only to open the NAT. Swallow it here so the
            // lockstep layer never sees a runt packet.
            if (got == static_cast<int>(sizeof(kPunchPacket))
                && memcmp(buf, kPunchPacket, sizeof(kPunchPacket)) == 0)
                continue;

            if (out_peer)
                *out_peer = 0;
            return got;
        }
    }
}
