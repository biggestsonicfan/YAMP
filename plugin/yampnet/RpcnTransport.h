#pragma once

// ITransport backed by RPCN: login, rooms, peer discovery via signaling, then direct P2P
// datagrams. Drops in behind the same interface as the UDP dev backend, so the lockstep engine
// is unchanged by the swap.
//
// CONNECTION MODEL (and why it is asymmetric):
//   The GUEST resolves the host through RequestSignalingInfos and transmits first.
//   The HOST learns the guest's address from the first datagram it receives.
// That avoids needing to parse room notifications to discover when someone joined, and it is also
// what makes NAT traversal work at all: the guest's outbound packet opens the return path.
//
// Game traffic deliberately shares the signaling socket. That socket's NAT mapping is the one the
// server observed and advertised to peers; a second socket would get a different mapping that no
// peer could reach.

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
        };

        enum class Stage
        {
            Idle,
            LoggingIn,
            Online,          // logged in, discovery done
            Hosting,         // room created, waiting for a peer's first datagram
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

        // --- ITransport ---
        bool Send(uint32_t peer, const void* data, uint32_t len) override;
        int Recv(void* buf, uint32_t cap, uint32_t* out_peer) override;
        bool Ready() const override { return m_stage == Stage::Linked; }
        const char* LastError() const override { return m_error[0] ? m_error : m_client.LastError(); }

    private:
        void Fail(const char* fmt, ...);
        bool PumpReplies();
        void PumpKeepalive();

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
        char m_error[256] = {};
    };
}
