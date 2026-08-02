#pragma once

// ITransport backed by RPCN: login, rooms, peer discovery via signaling, then direct P2P
// datagrams. Drops in behind the same interface as the UDP dev backend, so the lockstep engine
// is unchanged by the swap.
//
// CONNECTION MODEL - SYMMETRIC, AND IT HAS TO BE:
//   BOTH ends learn the other's address from the server, and BOTH start sending immediately.
//   The guest gets the host's address in its JoinRoom reply (signaling_data); the host gets the
//   guest's in the UserJoinedRoom notification the server pushes when someone joins. Both are only
//   populated because CreateRoom asks for signaling (sigOptParam) - see RpcnClient::CreateRoom.
//
// The earlier model had the guest transmit first and the host stay silent until it heard
// something, on the theory that the guest's packet "opens the return path". IT DOES NOT. It opens
// the path through the GUEST's NAT. The host's NAT has no mapping for the guest at all, so it
// drops that first packet on the floor, and a silent host never creates one - two peers on
// different networks sit at the barrier forever, which is exactly what happened. Hole punching
// only works if both sides transmit, so PumpPunch() starts as soon as an address is known and
// keeps a small datagram flowing whether or not anything has been received.
//
// Game traffic deliberately shares the signaling socket. That socket's NAT mapping is the one the
// server observed and advertised to peers; a second socket would get a different mapping that no
// peer could reach.
//
// WHAT THIS STILL CANNOT DO: if either side is behind a symmetric NAT, the mapping it opens
// towards the peer differs from the one the server advertised, and no amount of punching helps -
// RPCN has no relay to fall back on. Recv() absorbs the milder case (a guest whose port differs
// from the advertised one) by re-pointing at the source it actually hears from.

#include <stdint.h>

#include "RpcnClient.h"
#include "Transport.h"

namespace yampnet
{
    class RpcnTransport final : public ITransport
    {
    public:
        struct Config
        {
            const char* server = nullptr;
            uint16_t port = kRpcnDefaultPort;
            const char* fingerprint_hex = nullptr;   // null/empty = validate chain + host name
            const char* npid = nullptr;
            const char* password = nullptr;
            const char* com_id = nullptr;            // e.g. "NPWR02113_00"
            uint16_t local_p2p_port = kRpcnP2PPort;  // override for tests only

            // Optional progress log. Connection problems here are almost always somebody's NAT or
            // firewall rather than a bug, and none of that is diagnosable from "waiting for peer"
            // alone - so the transport reports what it learned and from where.
            void* log_ctx = nullptr;
            void (*log)(void* ctx, const char* msg) = nullptr;
        };

        enum class Stage
        {
            Idle,
            LoggingIn,
            Online,          // logged in, discovery done
            Hosting,         // room created, nobody has joined yet
            Joining,         // room joined, resolving the host
            Linked,          // peer address known - datagrams can flow
            Failed,
        };

        bool Start(const Config& cfg);
        void Stop();

        // Pump TLS, keepalives and pending requests. Call every frame.
        void Update();

        Stage GetStage() const { return m_stage; }
        uint64_t RoomId() const { return m_room_id; }
        bool IsHost() const { return m_is_host; }

        // Create a room and wait for someone to join, or join an existing one. `flag_attr` is the
        // room's published attribute word (YAMPNET_ROOM_FLAG_*); a joiner learns the host's from
        // the reply, so it is only an input on the hosting side.
        bool Host(uint32_t max_slot = 2, const char* password = nullptr, uint32_t flag_attr = 0);
        bool Join(uint64_t room_id, const char* password = nullptr);

        // The attribute word of the room we are in - our own when hosting, the host's when we
        // joined - or 0 when there is no room. Filled from the create/join reply rather than
        // remembered from the request, so it is always the value the SERVER holds.
        uint32_t RoomFlags() const { return m_room_flags; }

        // Ask the server for the rooms in our world. The reply is asynchronous: RoomCount()/Room()
        // report the result of the LAST completed search, and SearchPending() is true until it
        // lands. Only valid once Online.
        bool Search();
        bool SearchPending() const { return m_pending_search != 0; }
        uint32_t RoomCount() const { return m_room_count; }
        const RpcnClient::RoomListing& Room(uint32_t i) const { return m_rooms[i]; }

        // --- Peer state, for diagnostics ------------------------------------------------------
        // "Known" means the server told us where the peer is; "heard" means a datagram actually
        // made it back. The gap between the two is the NAT/firewall failure, and it is the only
        // thing worth reporting to a player stuck on a waiting screen.
        bool PeerKnown() const { return m_peer_ip != 0 && m_peer_port != 0; }
        bool PeerHeard() const { return m_peer_heard; }
        // "a.b.c.d:port", or "unknown" before the server has told us. Points at a static buffer.
        const char* PeerAddrText() const;
        const char* PeerNpid() const { return m_peer_npid; }

        // --- ITransport ---
        bool Send(uint32_t peer, const void* data, uint32_t len) override;
        int Recv(void* buf, uint32_t cap, uint32_t* out_peer) override;
        bool Ready() const override { return m_stage == Stage::Linked; }
        const char* LastError() const override { return m_error[0] ? m_error : m_client.LastError(); }

    private:
        void Fail(const char* fmt, ...);
        void Note(const char* fmt, ...);
        bool PumpReplies();
        void PumpKeepalive();
        void PumpPunch();
        void PumpSignalingRetry();
        void OnNotification(const RpcnPacket& pkt);
        // Adopt a peer address, whatever told us about it. `source` names that for the log.
        void SetPeer(uint32_t ip, uint16_t port, const char* source);

        RpcnClient m_client;
        Stage m_stage = Stage::Idle;

        char m_com_id[16] = {};
        char m_npid[20] = {};
        char m_peer_npid[20] = {};

        uint16_t m_server_id = 0;
        uint32_t m_world_id = 0;
        uint64_t m_room_id = 0;
        uint32_t m_room_flags = 0;
        bool m_is_host = false;

        uint32_t m_peer_ip = 0;      // network byte order
        uint16_t m_peer_port = 0;
        bool m_peer_heard = false;   // a datagram has actually arrived from the peer

        // Outstanding request ids we care about, so replies can be routed without blocking.
        uint64_t m_pending_serverlist = 0;
        uint64_t m_pending_worldlist = 0;
        uint64_t m_pending_room = 0;
        uint64_t m_pending_search = 0;
        uint64_t m_pending_signaling = 0;

        // Result of the last completed search. 32 is far more rooms than a private server for one
        // arcade game will ever hold, and the request asks for a page of 32 anyway.
        static constexpr uint32_t kMaxRooms = 32;
        RpcnClient::RoomListing m_rooms[kMaxRooms] = {};
        uint32_t m_room_count = 0;

        uint64_t m_last_keepalive_ms = 0;
        uint64_t m_last_punch_ms = 0;
        // A signaling lookup is retried rather than fatal: a guest can easily join before the host
        // has been registered by the UDP helper, and that used to kill the whole session.
        uint64_t m_signaling_retry_ms = 0;

        void* m_log_ctx = nullptr;
        void (*m_log)(void* ctx, const char* msg) = nullptr;

        char m_error[256] = {};
    };
}
