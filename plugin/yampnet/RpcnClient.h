#pragma once

// RPCN protocol client - framing, request/reply correlation, login, and the UDP signaling
// keepalive.
//
// Verified against the RPCN server source (RipleyTom/rpcn), not guessed:
//
//   Header, 15 bytes, little-endian:
//     [0]      u8   packet_type   Request=0 Reply=1 Notification=2 ServerInfo=3
//     [1..3]   u16  command
//     [3..7]   u32  packet_size   TOTAL, INCLUDING this header
//     [7..15]  u64  packet_id     echoed back on the reply
//   A Reply carries u8 ErrorType at [15], then its payload.
//   Strings in payloads are NUL-terminated raw bytes.
//   Unauthenticated clients are dropped after 10 s, so log in promptly.
//
// The UDP signaling helper (server port 3657) is what lets peers learn each other's public
// address; game traffic itself is direct P2P on port 3658 and never touches the server.

#include <stdint.h>

#include "TlsClient.h"

namespace yampnet
{
    // Values match the server's CommandType enum exactly (declaration order).
    enum class RpcnCommand : uint16_t
    {
        Login = 0,
        Terminate = 1,
        Create = 2,
        GetServerList = 12,
        GetWorldList = 13,
        CreateRoom = 14,
        JoinRoom = 15,
        LeaveRoom = 16,
        SearchRoom = 17,
        GetRoomDataInternal = 20,
        SetRoomDataInternal = 21,
        SetRoomMemberDataInternal = 23,
        RequestSignalingInfos = 27,
    };

    enum class RpcnError : uint8_t
    {
        NoError = 0,
        Malformed = 1,
        Invalid = 2,
        InvalidInput = 3,
        TooSoon = 4,
    };

    // Server-pushed notifications (packet_type 2). Values are the declaration order of the
    // server's NotificationType enum (notifications.rs); the ones below are the only ones a
    // two-player room ever produces. These are NOT optional extras: UserJoinedRoom is how a HOST
    // learns that a guest exists at all, and it is the only way it can learn an address to punch
    // towards before the guest's own datagrams start arriving.
    enum class RpcnNotification : uint16_t
    {
        UserJoinedRoom = 0,
        UserLeftRoom = 1,
        RoomDestroyed = 2,
        SignalingHelper = 12,
    };

    inline constexpr uint32_t kRpcnHeaderSize = 15;
    inline constexpr uint16_t kRpcnDefaultPort = 31313;
    inline constexpr uint16_t kRpcnSignalingPort = 3657;   // server-side UDP helper
    inline constexpr uint16_t kRpcnP2PPort = 3658;         // peer-to-peer game traffic

    // A decoded inbound packet. `payload` points into the client's buffer and is only valid until
    // the next Poll().
    struct RpcnPacket
    {
        uint8_t type = 0;
        uint16_t command = 0;
        uint64_t packet_id = 0;
        RpcnError error = RpcnError::NoError;
        const uint8_t* payload = nullptr;   // past the error byte for replies
        uint32_t payload_size = 0;
    };

    class RpcnClient
    {
    public:
        bool Connect(const char* host, uint16_t port, const CertFingerprint& pinned);
        void Disconnect();
        bool IsConnected() const { return m_tls.IsConnected(); }

        // Sends Login (npid, password, token). Token may be empty. Returns the packet id, or 0.
        uint64_t Login(const char* npid, const char* password, const char* token);

        // Sends Create to register an account. Server rules, worth knowing before you call:
        //   * npid and online_name must be 3-16 chars of [A-Za-z0-9_-].
        //   * NONE of the five fields may be empty - the server reads them all with
        //     get_string(false), so an empty avatar_url alone is rejected as Malformed.
        //   * email must PARSE as a real address even when validation is disabled; a token is
        //     only needed if the server has EmailUrl set (it is empty by default).
        // Returns the packet id, or 0.
        uint64_t CreateAccount(const char* npid, const char* password, const char* online_name,
                               const char* avatar_url, const char* email);

        // Generic request. `payload` may be null. Returns the packet id used, or 0 on failure.
        uint64_t Request(RpcnCommand cmd, const void* payload, uint32_t payload_size);

        // --- Rooms --------------------------------------------------------------------------
        // Room commands are framed as [12-byte ComId][u32 LE protobuf length][protobuf].
        // The ComId's first 9 bytes must be ASCII uppercase/digits (e.g. "NPWR02113_00"), and the
        // (comId, worldId) pair MUST exist in the server's servers.cfg or the server answers
        // InvalidInput - it looks the pair up in its `world` table and does not invent defaults.
        // The proper discovery order before creating a room. With the server's CreateMissing=true
        // these also REGISTER a previously unknown title, so a fresh comId becomes usable without
        // editing servers.cfg. Payloads carry no protobuf: GetServerList is just the ComId,
        // GetWorldList is the ComId followed by a u16 server id.
        uint64_t GetServerList(const char* com_id);
        uint64_t GetWorldList(const char* com_id, uint16_t server_id);
        // Replies are u16 count followed by that many u16 / u32 entries.
        static uint32_t ParseServerList(const uint8_t* p, uint32_t size, uint16_t* out, uint32_t max);
        static uint32_t ParseWorldList(const uint8_t* p, uint32_t size, uint32_t* out, uint32_t max);

        // `password` may be null/empty for a public room. RPCN answers RoomPasswordMismatch(18)
        // or RoomPasswordMissing(19) on a bad join rather than a generic failure.
        //
        // `flag_attr` is the room's u32 attribute word, which YAMP uses to publish the cabinet
        // settings a match will be played under (see YAMPNET_ROOM_FLAG_* in YampNet.h). The server
        // stores it verbatim apart from SCE_NP_MATCHING2_ROOM_FLAG_ATTR_FULL (0x20000000), which it
        // clears here and sets itself once the room is full - so never rely on that bit.
        //
        // The room is created WITH SIGNALING ENABLED (sigOptParam). That single field is what makes
        // the server exchange peer addresses on its own: without it `need_signaling` is false in
        // room_manager.rs, the join reply carries no signaling_data and the host's UserJoinedRoom
        // notification carries no address - which left the host with nobody to punch towards.
        uint64_t CreateRoom(const char* com_id, uint32_t world_id, uint32_t max_slot,
                            const char* password, uint32_t flag_attr);
        uint64_t JoinRoom(const char* com_id, uint64_t room_id, const char* password);
        uint64_t SearchRoom(const char* com_id, uint32_t world_id);
        // npid of the peer whose address we want. Reply is [u32 len][SignalingAddr protobuf].
        uint64_t RequestSignalingInfos(const char* npid);

        // One row of a SearchRoom reply.
        struct RoomListing
        {
            uint64_t room_id;
            uint16_t cur_members;
            uint16_t max_slots;
            bool     has_password;
            char     owner[20];
            uint32_t flag_attr;      // as published by CreateRoom; see YAMPNET_ROOM_FLAG_*
        };
        // Parses a SearchRoomResponse (repeated RoomDataExternal) into `out`. Returns the count
        // written, capped at max_out.
        static uint32_t ParseRoomList(const uint8_t* payload, uint32_t size,
                                      RoomListing* out, uint32_t max_out);

        // Pulls roomId out of a CreateRoomResponse / JoinRoomResponse payload. Returns 0 if absent.
        static uint64_t ParseRoomId(const uint8_t* payload, uint32_t size);
        // Pulls flagAttr out of the same two replies (RoomDataInternal field 10). Returns 0 if
        // absent - which is also the value of a room created without any flags, and is why the
        // caller must treat "no flags" and "flags we do not understand" identically.
        static uint32_t ParseRoomFlagAttr(const uint8_t* payload, uint32_t size);
        // Collects member npids from a CreateRoomResponse / JoinRoomResponse. Returns the count.
        static uint32_t ParseRoomMembers(const uint8_t* payload, uint32_t size,
                                         char out[][20], uint32_t max_out);
        // Parses a RequestSignalingInfos reply into an address. Returns false if malformed.
        static bool ParseSignalingAddr(const uint8_t* payload, uint32_t size,
                                       uint32_t* out_ipv4_be, uint16_t* out_port);

        // Pulls the peer address out of a JoinRoomResponse's signaling_data (field 2, repeated
        // Matching2SignalingInfo). Present only when the room was created with sigOptParam, which
        // is why CreateRoom always sets it. Returns the FIRST entry - in a two-slot room that is
        // the host - so a guest needs no RequestSignalingInfos round trip at all.
        static bool ParseJoinSignalingAddr(const uint8_t* payload, uint32_t size,
                                           uint32_t* out_ipv4_be, uint16_t* out_port);

        // Parses a UserJoinedRoom notification: the joiner's npid, plus its signaling address when
        // the room asked for signaling. `out_has_addr` distinguishes "no address in this
        // notification" (the caller should ask for one) from "address of 0.0.0.0:0".
        static bool ParseJoinedNotification(const uint8_t* payload, uint32_t size,
                                            char* out_npid, uint32_t npid_cap,
                                            uint32_t* out_ipv4_be, uint16_t* out_port,
                                            bool* out_has_addr);

        // Parses a SignalingHelper notification (MatchingSignalingInfo): the npid and address of a
        // peer that just asked the server for OUR address. The server sends it precisely so both
        // ends punch, so it is the fallback path when a room has no signaling of its own.
        static bool ParseSignalingHelper(const uint8_t* payload, uint32_t size,
                                         char* out_npid, uint32_t npid_cap,
                                         uint32_t* out_ipv4_be, uint16_t* out_port);

        // Reads whatever is available and returns one decoded packet at a time. Returns false when
        // nothing more is pending. Never blocks.
        bool Poll(RpcnPacket* out);

        // Our own user_id, captured automatically from the Login reply (which is
        // online_name\0 avatar_url\0 then an i64). 0 until logged in. The signaling keepalive
        // needs it, so nothing UDP works before Login has been polled.
        int64_t UserId() const { return m_user_id; }

        // --- UDP signaling ------------------------------------------------------------------
        // Opens the local P2P socket (bound to kRpcnP2PPort) used both for signaling keepalives
        // and, later, for game traffic.
        // `local_port` defaults to the standard P2P port. Override it only for testing two peers
        // inside ONE process - a second bind of 3658 fails with WSAEADDRINUSE, and RPCN hardcodes
        // 3658 when it hands out a peer's LOCAL address, so a non-default port is not usable for
        // same-NAT play.
        bool OpenSignaling(uint16_t local_port = kRpcnP2PPort);
        // Sends the 13-byte keepalive so the server records/refreshes our public address. Call
        // every couple of seconds while online. Pass 0 to use the logged-in UserId().
        bool SendSignalingPing(int64_t user_id = 0);
        // Parses a signaling reply if one is pending. Returns true and fills the public address.
        bool PollSignaling(uint32_t* out_ipv4_be, uint16_t* out_port);

        // --- P2P datagrams ------------------------------------------------------------------
        // Game traffic rides the SAME socket as the signaling keepalives, on purpose: that socket
        // is the one the server observed and advertised to peers, so its NAT mapping is the one
        // they can reach. Opening a second socket would get a different mapping and silently fail.
        bool SendTo(uint32_t ipv4_be, uint16_t port, const void* data, uint32_t len);
        // Raw receive. Returns bytes, or 0 when nothing is pending. Fills the sender so callers
        // can tell a signaling reply (from the server) from a peer's game data by source address -
        // safer than sniffing content, whose first bytes can collide.
        int RecvFrom(void* buf, uint32_t cap, uint32_t* out_ipv4_be, uint16_t* out_port);
        // True if this datagram came from the RPCN signaling helper rather than a peer.
        bool IsSignalingSource(uint32_t ipv4_be, uint16_t port) const
        { return ipv4_be == m_signaling_addr && port == kRpcnSignalingPort; }
        // Decodes a signaling reply body (the 9/21-byte form). Returns false if it is not one.
        static bool ParseSignalingReply(const void* buf, uint32_t len,
                                        uint32_t* out_ipv4_be, uint16_t* out_port);
        uint32_t SignalingServerAddr() const { return m_signaling_addr; }

        const CertFingerprint& ServerFingerprint() const { return m_tls.ServerFingerprint(); }
        const char* LastError() const { return m_error[0] ? m_error : m_tls.LastError(); }

    private:
        void Fail(const char* fmt, ...);

        TlsClient m_tls;
        uint64_t m_next_packet_id = 1;

        // Inbound reassembly: TLS gives us a byte stream, packets can straddle records.
        uint8_t m_in[64 * 1024] = {};
        uint32_t m_in_used = 0;

        uintptr_t m_udp = static_cast<uintptr_t>(~0ull);
        // The signaling helper lives on the SAME host as the TLS server but on UDP 3657, so the
        // address is resolved once at Connect() and reused for every keepalive.
        uint32_t m_signaling_addr = 0;   // in_addr, network byte order
        int64_t m_user_id = 0;
        char m_error[256] = {};
    };
}
