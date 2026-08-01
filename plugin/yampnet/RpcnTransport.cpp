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
    }

    void RpcnTransport::Fail(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        vsnprintf(m_error, sizeof(m_error), fmt, args);
        va_end(args);
        m_stage = Stage::Failed;
    }

    bool RpcnTransport::Start(const Config& cfg)
    {
        Stop();

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
        m_peer_npid[0] = '\0';
        m_pending_serverlist = m_pending_worldlist = m_pending_room = m_pending_signaling = 0;
        m_pending_search = 0;
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

    bool RpcnTransport::PumpReplies()
    {
        RpcnPacket pkt;
        while (m_client.Poll(&pkt))
        {
            if (pkt.type != 1)
                continue;   // ServerInfo greeting / notifications - nothing to do with them yet

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
                    // Nothing more to do: the guest will find us and transmit first.
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
                    m_pending_signaling = m_client.RequestSignalingInfos(m_peer_npid);
                    m_stage = Stage::Joining;
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
                    Fail("could not resolve peer '%s' (ErrorType=%u)", m_peer_npid,
                         static_cast<unsigned>(pkt.error));
                    return false;
                }
                m_peer_ip = ip;
                m_peer_port = port;
                m_stage = Stage::Linked;
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

            // First contact from the guest: adopt it as the peer. This is how the host learns an
            // address it could not have looked up, and it is what makes the return path work.
            if (!m_peer_ip)
            {
                m_peer_ip = ip;
                m_peer_port = port;
                if (m_stage == Stage::Hosting)
                    m_stage = Stage::Linked;
            }
            else if (ip != m_peer_ip || port != m_peer_port)
            {
                continue;   // stray datagram from somewhere else - ignore
            }

            if (out_peer)
                *out_peer = 0;
            return got;
        }
    }
}
