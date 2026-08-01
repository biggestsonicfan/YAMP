#include "RpcnClient.h"

#include "Protobuf.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace yampnet
{
    namespace
    {
        void PutU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
        void PutU32(uint8_t* p, uint32_t v)
        {
            p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
            p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
        }
        void PutU64(uint8_t* p, uint64_t v)
        {
            for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (i * 8));
        }
        uint16_t GetU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
        uint32_t GetU32(const uint8_t* p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16)
                 | (uint32_t(p[3]) << 24);
        }
        uint64_t GetU64(const uint8_t* p)
        {
            uint64_t v = 0;
            for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
            return v;
        }
    }

    void RpcnClient::Fail(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        vsnprintf(m_error, sizeof(m_error), fmt, args);
        va_end(args);
    }

    bool RpcnClient::Connect(const char* host, uint16_t port, const CertFingerprint& pinned)
    {
        m_error[0] = '\0';
        m_in_used = 0;
        m_next_packet_id = 1;

        m_signaling_addr = 0;
        m_user_id = 0;

        if (!m_tls.Connect(host, port ? port : kRpcnDefaultPort, pinned))
            return false;

        // Resolve the host for the UDP signaling helper, which sits on the same machine but on its
        // own port. This MUST happen after the TLS connect: TlsClient owns the WSAStartup, so
        // calling getaddrinfo first fails with WSANOTINITIALISED for the FIRST client in a process
        // (later ones only worked because an earlier client was holding winsock open) - which
        // silently left m_signaling_addr at 0 and made every signaling ping a no-op.
        // Not fatal if it fails: only signaling suffers, the TLS session is fine.
        {
            addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(host, nullptr, &hints, &res) == 0 && res)
            {
                m_signaling_addr =
                    reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
                freeaddrinfo(res);
            }
        }
        return true;
    }

    void RpcnClient::Disconnect()
    {
        m_tls.Close();
        if (m_udp != static_cast<uintptr_t>(~0ull))
        {
            closesocket(static_cast<SOCKET>(m_udp));
            m_udp = static_cast<uintptr_t>(~0ull);
        }
        m_in_used = 0;
    }

    uint64_t RpcnClient::Request(RpcnCommand cmd, const void* payload, uint32_t payload_size)
    {
        if (!m_tls.IsConnected())
        {
            Fail("not connected");
            return 0;
        }

        const uint32_t total = kRpcnHeaderSize + payload_size;
        uint8_t header[kRpcnHeaderSize];
        header[0] = 0;                                   // PacketType::Request
        PutU16(header + 1, static_cast<uint16_t>(cmd));
        PutU32(header + 3, total);                       // size INCLUDES the header
        const uint64_t id = m_next_packet_id++;
        PutU64(header + 7, id);

        if (!m_tls.SendAll(header, kRpcnHeaderSize))
            return 0;
        if (payload_size && !m_tls.SendAll(payload, payload_size))
            return 0;
        return id;
    }

    namespace
    {
        // Payloads are just NUL-terminated strings back to back.
        struct StringPacker
        {
            uint8_t buf[1024];
            uint32_t n = 0;
            bool ok = true;

            void Put(const char* s)
            {
                const uint32_t len = static_cast<uint32_t>(s ? strlen(s) : 0);
                if (!ok || n + len + 1 > sizeof(buf)) { ok = false; return; }
                if (len) memcpy(buf + n, s, len);
                n += len;
                buf[n++] = 0;
            }
        };
    }

    uint64_t RpcnClient::Login(const char* npid, const char* password, const char* token)
    {
        if (!npid || !password)
        {
            Fail("login requires an npid and password");
            return 0;
        }

        // Three NUL-terminated strings: login, password, token. The token may be empty (the
        // server reads it with get_string(true)) but its terminator must still be present.
        StringPacker p;
        p.Put(npid);
        p.Put(password);
        p.Put(token ? token : "");
        if (!p.ok)
        {
            Fail("login credentials too long");
            return 0;
        }
        return Request(RpcnCommand::Login, p.buf, p.n);
    }

    uint64_t RpcnClient::CreateAccount(const char* npid, const char* password,
                                       const char* online_name, const char* avatar_url,
                                       const char* email)
    {
        // Every field is read with get_string(false), so an empty one is Malformed rather than
        // defaulted. Catch it here where the error can actually say which field.
        struct { const char* v; const char* name; } fields[] = {
            { npid, "npid" }, { password, "password" }, { online_name, "online_name" },
            { avatar_url, "avatar_url" }, { email, "email" },
        };
        for (const auto& f : fields)
        {
            if (!f.v || !*f.v)
            {
                Fail("Create: %s must not be empty", f.name);
                return 0;
            }
        }

        auto valid_username = [](const char* s) {
            const size_t len = strlen(s);
            if (len < 3 || len > 16)
                return false;
            for (const char* p = s; *p; ++p)
            {
                const bool alnum = (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z')
                                || (*p >= 'a' && *p <= 'z');
                if (!alnum && *p != '-' && *p != '_')
                    return false;
            }
            return true;
        };
        if (!valid_username(npid))
        {
            Fail("Create: npid must be 3-16 chars of [A-Za-z0-9_-]");
            return 0;
        }
        if (!valid_username(online_name))
        {
            Fail("Create: online_name must be 3-16 chars of [A-Za-z0-9_-]");
            return 0;
        }

        StringPacker p;
        p.Put(npid);
        p.Put(password);
        p.Put(online_name);
        p.Put(avatar_url);
        p.Put(email);
        if (!p.ok)
        {
            Fail("Create: fields too long");
            return 0;
        }
        return Request(RpcnCommand::Create, p.buf, p.n);
    }

    namespace
    {
        // Room payload = [12-byte ComId][u32 LE protobuf size][protobuf]. Returns total bytes, or
        // 0 if the ComId is unusable.
        uint32_t FrameRoomPayload(uint8_t* out, uint32_t cap, const char* com_id,
                                  const uint8_t* pb, uint32_t pb_size)
        {
            const uint32_t total = 12 + 4 + pb_size;
            if (!com_id || cap < total)
                return 0;

            // Exactly 12 bytes, NUL-padded. The server rejects it unless the first 9 are ASCII
            // uppercase or digits, so a lowercase id fails as Malformed rather than NotFound.
            memset(out, 0, 12);
            const uint32_t len = static_cast<uint32_t>(strlen(com_id));
            memcpy(out, com_id, len < 12 ? len : 12);
            for (int i = 0; i < 9; ++i)
            {
                const char c = static_cast<char>(out[i]);
                const bool valid = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                if (!valid)
                    return 0;
            }

            PutU32(out + 12, pb_size);
            if (pb_size) memcpy(out + 16, pb, pb_size);
            return total;
        }
    }

    uint64_t RpcnClient::GetServerList(const char* com_id)
    {
        uint8_t payload[16];
        const uint32_t n = FrameRoomPayload(payload, sizeof(payload), com_id, nullptr, 0);
        if (!n)
        {
            Fail("GetServerList: bad ComId '%s'", com_id ? com_id : "(null)");
            return 0;
        }
        // No protobuf on this one - send the ComId alone, not the length prefix.
        return Request(RpcnCommand::GetServerList, payload, 12);
    }

    uint64_t RpcnClient::GetWorldList(const char* com_id, uint16_t server_id)
    {
        uint8_t payload[16];
        if (!FrameRoomPayload(payload, sizeof(payload), com_id, nullptr, 0))
        {
            Fail("GetWorldList: bad ComId");
            return 0;
        }
        PutU16(payload + 12, server_id);
        return Request(RpcnCommand::GetWorldList, payload, 14);
    }

    uint32_t RpcnClient::ParseServerList(const uint8_t* p, uint32_t size, uint16_t* out,
                                         uint32_t max)
    {
        if (size < 2) return 0;
        uint32_t count = GetU16(p);
        if (2 + count * 2 > size) count = (size - 2) / 2;
        if (count > max) count = max;
        for (uint32_t i = 0; i < count; ++i)
            out[i] = GetU16(p + 2 + i * 2);
        return count;
    }

    uint32_t RpcnClient::ParseWorldList(const uint8_t* p, uint32_t size, uint32_t* out,
                                        uint32_t max)
    {
        // NOTE the asymmetry with ParseServerList: the world count is a u32 while the server count
        // is a u16 (cmd_server.rs). Reading it as u16 shifts every entry by two bytes and yields
        // plausible-but-wrong world ids rather than an obvious failure.
        if (size < 4) return 0;
        uint32_t count = GetU32(p);
        if (4 + count * 4 > size) count = (size - 4) / 4;
        if (count > max) count = max;
        for (uint32_t i = 0; i < count; ++i)
            out[i] = GetU32(p + 4 + i * 4);
        return count;
    }

    uint64_t RpcnClient::CreateRoom(const char* com_id, uint32_t world_id, uint32_t max_slot,
                                const char* password)
    {
        uint8_t pb[256];
        PbWriter w(pb, sizeof(pb));
        w.Varint(1, world_id);                 // worldId
        w.Varint(3, max_slot);                 // maxSlot
        // teamId is NOT optional despite proto3: room_manager.rs does
        // `pb.team_id.get_verified()?` and Option<Uint8>::get_verified() returns Malformed when the
        // field is absent ("Protobuf Uint8 is none!"). It is a uint8 WRAPPER submessage, so it has
        // to be written as { uint32 value = 1 }, not a bare varint.
        if (password && *password)
        {
            // TWO RULES, BOTH ENFORCED SILENTLY BY THE SERVER - get either wrong and the room ends
            // up with NO password while still looking protected to us.
            //
            // 1. The password is a FIXED 8 BYTES (SceNpMatching2SessionPassword). room_manager.rs
            //    takes it only `if pb.room_password.len() == 8` and otherwise just logs
            //    "Invalid password length" and leaves the room open. So pad/truncate to 8.
            // 2. passwordSlotMask marks WHICH SLOTS the password guards, and it defaults to 0 -
            //    meaning every slot is public. A joiner sending no password takes `req_slot(false)`
            //    and walks straight into a "private" room. Marking all maxSlot slots private (the
            //    mask is MSB-first: slot i = bit 63-i) is what actually gates the room, and it is
            //    also how the browser can tell a locked room from an open one, since the search
            //    reply exposes privateSlotNum but no password flag.
            char fixed[8] = {};
            const size_t given = strlen(password);
            memcpy(fixed, password, given < sizeof(fixed) ? given : sizeof(fixed));
            w.Bytes(9, fixed, sizeof(fixed));                                // roomPassword

            uint64_t slot_mask = 0;
            for (uint32_t i = 0; i < max_slot && i < 64; ++i)
                slot_mask |= (1ull << (63 - i));
            w.Varint(11, slot_mask);                                         // passwordSlotMask
        }
        w.Wrapped(16, 0);                      // teamId
        if (!w.Ok())
        {
            Fail("CreateRoom: protobuf overflow");
            return 0;
        }

        uint8_t payload[512];
        const uint32_t n = FrameRoomPayload(payload, sizeof(payload), com_id, pb, w.Size());
        if (!n)
        {
            Fail("CreateRoom: bad ComId '%s'", com_id ? com_id : "(null)");
            return 0;
        }
        return Request(RpcnCommand::CreateRoom, payload, n);
    }

    uint64_t RpcnClient::JoinRoom(const char* com_id, uint64_t room_id, const char* password)
    {
        uint8_t pb[256];
        PbWriter w(pb, sizeof(pb));
        w.Varint(1, room_id);                  // roomId
        if (password && *password)
        {
            // Same fixed 8 bytes as CreateRoom - the server compares the raw arrays, so a password
            // padded differently here would simply never match the one the room was made with.
            char fixed[8] = {};
            const size_t given = strlen(password);
            memcpy(fixed, password, given < sizeof(fixed) ? given : sizeof(fixed));
            w.Bytes(2, fixed, sizeof(fixed));                                // roomPassword
        }
        w.Wrapped(6, 0);                       // teamId - mandatory here too (room_manager.rs:357)
        if (!w.Ok())
        {
            Fail("JoinRoom: protobuf overflow");
            return 0;
        }

        uint8_t payload[512];
        const uint32_t n = FrameRoomPayload(payload, sizeof(payload), com_id, pb, w.Size());
        if (!n)
        {
            Fail("JoinRoom: bad ComId");
            return 0;
        }
        return Request(RpcnCommand::JoinRoom, payload, n);
    }

    uint64_t RpcnClient::SearchRoom(const char* com_id, uint32_t world_id)
    {
        uint8_t pb[256];
        PbWriter w(pb, sizeof(pb));
        // option bit 0 = include the owner's npId in each result. Without it the server's
        // to_RoomDataExternal leaves `owner` unset (it gates on `search_option & 0x7`), and every
        // room in the browser comes back nameless - which is exactly what happened first time.
        // Bits 1 and 2 would add onlineName / avatarUrl; the npid is all we display.
        w.Varint(1, 1);                        // option
        w.Varint(2, world_id);                 // worldId
        w.Varint(5, 32);                       // rangeFilter_max - ask for a usable page
        if (!w.Ok())
        {
            Fail("SearchRoom: protobuf overflow");
            return 0;
        }

        uint8_t payload[512];
        const uint32_t n = FrameRoomPayload(payload, sizeof(payload), com_id, pb, w.Size());
        if (!n)
        {
            Fail("SearchRoom: bad ComId");
            return 0;
        }
        return Request(RpcnCommand::SearchRoom, payload, n);
    }

    uint64_t RpcnClient::RequestSignalingInfos(const char* npid)
    {
        if (!npid || !*npid)
        {
            Fail("RequestSignalingInfos needs an npid");
            return 0;
        }
        StringPacker p;
        p.Put(npid);
        if (!p.ok)
        {
            Fail("npid too long");
            return 0;
        }
        return Request(RpcnCommand::RequestSignalingInfos, p.buf, p.n);
    }

    namespace
    {
        // Every room reply goes through Client::add_data_packet, i.e. [u32 LE length][protobuf].
        // Strip that before handing bytes to the protobuf reader, or the length prefix gets
        // parsed as a tag and the whole message silently reads as empty.
        bool StripDataPacket(const uint8_t*& p, uint32_t& size)
        {
            if (size < 4)
                return false;
            const uint32_t len = GetU32(p);
            if (len == 0 || 4 + len > size)
                return false;
            p += 4;
            size = len;
            return true;
        }
    }

    uint32_t RpcnClient::ParseRoomList(const uint8_t* payload, uint32_t size,
                                       RoomListing* out, uint32_t max_out)
    {
        if (!out || max_out == 0 || !StripDataPacket(payload, size))
            return 0;

        // SearchRoomResponse { startIndex = 1, total = 2, repeated RoomDataExternal rooms = 3 }
        uint32_t count = 0;
        PbReader top(payload, size);
        while (count < max_out && top.Next())
        {
            if (top.Field() != 3 || top.WireType() != 2)
                continue;

            RoomListing row = {};
            // RoomDataExternal: roomId = 6 (bare varint), maxSlot = 8, curMemberNum = 10 and
            // privateSlotNum = 4 (all uint16 WRAPPER submessages - see the proto note: uint8/uint16
            // are messages { uint32 value = 1 }, not bare varints), owner = 12 (UserInfo).
            PbReader room = top.Sub();
            while (room.Next())
            {
                switch (room.Field())
                {
                case 4:  row.has_password = room.AsWrapped() != 0; break;
                case 6:  row.room_id = room.AsVarint(); break;
                case 8:  row.max_slots = static_cast<uint16_t>(room.AsWrapped()); break;
                case 10: row.cur_members = static_cast<uint16_t>(room.AsWrapped()); break;
                case 12:
                {
                    // UserInfo { npId = 1, onlineName = 2, avatarUrl = 3 }
                    PbReader owner = room.Sub();
                    while (owner.Next())
                    {
                        if (owner.Field() == 1 && owner.WireType() == 2)
                        {
                            const uint32_t n = owner.AsBytesLen() < sizeof(row.owner) - 1
                                                   ? owner.AsBytesLen()
                                                   : sizeof(row.owner) - 1;
                            memcpy(row.owner, owner.AsBytes(), n);
                            row.owner[n] = '\0';
                            break;
                        }
                    }
                    break;
                }
                default: break;
                }
            }

            if (row.room_id != 0)
                out[count++] = row;
        }
        return count;
    }

    uint64_t RpcnClient::ParseRoomId(const uint8_t* payload, uint32_t size)
    {
        if (!StripDataPacket(payload, size))
            return 0;

        // CreateRoomResponse{ internal=1 } and JoinRoomResponse{ room_data=1 } both wrap a
        // RoomDataInternal at field 1, whose roomId is field 4.
        PbReader top(payload, size);
        while (top.Next())
        {
            if (top.Field() != 1 || top.WireType() != kWireLen)
                continue;
            PbReader room = top.Sub();
            while (room.Next())
            {
                if (room.Field() == 4 && room.WireType() == kWireVarint)
                    return room.AsVarint();
            }
        }
        return 0;
    }

    uint32_t RpcnClient::ParseRoomMembers(const uint8_t* payload, uint32_t size,
                                          char out[][20], uint32_t max_out)
    {
        uint32_t count = 0;
        if (!StripDataPacket(payload, size))
            return 0;

        PbReader top(payload, size);
        while (top.Next())
        {
            if (top.Field() != 1 || top.WireType() != kWireLen)
                continue;

            PbReader room = top.Sub();
            while (room.Next())
            {
                if (room.Field() != 7 || room.WireType() != kWireLen)
                    continue;   // memberList

                PbReader member = room.Sub();
                while (member.Next())
                {
                    if (member.Field() != 1 || member.WireType() != kWireLen)
                        continue;   // userInfo

                    PbReader ui = member.Sub();
                    while (ui.Next())
                    {
                        if (ui.Field() != 1 || ui.WireType() != kWireLen)
                            continue;   // npId
                        if (count >= max_out)
                            return count;
                        uint32_t n = ui.AsBytesLen();
                        if (n > 19) n = 19;
                        memcpy(out[count], ui.AsBytes(), n);
                        out[count][n] = '\0';
                        ++count;
                    }
                }
            }
        }
        return count;
    }

    bool RpcnClient::ParseSignalingAddr(const uint8_t* payload, uint32_t size,
                                        uint32_t* out_ipv4_be, uint16_t* out_port)
    {
        // Reply is add_data_packet: [u32 LE length][SignalingAddr protobuf].
        if (size < 4)
            return false;
        const uint32_t len = GetU32(payload);
        if (len == 0 || 4 + len > size)
            return false;

        PbReader r(payload + 4, len);
        bool got_ip = false;
        while (r.Next())
        {
            if (r.Field() == 1 && r.WireType() == kWireLen && r.AsBytesLen() >= 4)
            {
                if (out_ipv4_be) memcpy(out_ipv4_be, r.AsBytes(), 4);
                got_ip = true;
            }
            else if (r.Field() == 2 && r.WireType() == kWireLen)
            {
                if (out_port) *out_port = static_cast<uint16_t>(r.AsWrapped());
            }
        }
        return got_ip;
    }

    bool RpcnClient::Poll(RpcnPacket* out)
    {
        if (!out || !m_tls.IsConnected())
            return false;

        for (;;)
        {
            // Do we already hold a complete packet?
            if (m_in_used >= kRpcnHeaderSize)
            {
                const uint32_t size = GetU32(m_in + 3);
                if (size < kRpcnHeaderSize || size > sizeof(m_in))
                {
                    Fail("malformed packet size %u", size);
                    Disconnect();
                    return false;
                }
                if (m_in_used >= size)
                {
                    out->type = m_in[0];
                    out->command = GetU16(m_in + 1);
                    out->packet_id = GetU64(m_in + 7);

                    // Replies carry an error byte before their payload; notifications do not.
                    if (out->type == 1 && size > kRpcnHeaderSize)
                    {
                        out->error = static_cast<RpcnError>(m_in[kRpcnHeaderSize]);
                        out->payload = m_in + kRpcnHeaderSize + 1;
                        out->payload_size = size - kRpcnHeaderSize - 1;
                    }
                    else
                    {
                        out->error = RpcnError::NoError;
                        out->payload = m_in + kRpcnHeaderSize;
                        out->payload_size = size - kRpcnHeaderSize;
                    }

                    // Snoop our own Login reply for the user_id. Doing it here means callers
                    // never have to remember to parse it, and signaling just works after login.
                    if (out->type == 1
                        && out->command == static_cast<uint16_t>(RpcnCommand::Login)
                        && out->error == RpcnError::NoError)
                    {
                        // online_name  avatar_url  then i64 user_id.
                        uint32_t at = 0;
                        int skipped = 0;
                        while (at < out->payload_size && skipped < 2)
                        {
                            if (out->payload[at] == 0) ++skipped;
                            ++at;
                        }
                        if (skipped == 2 && at + 8 <= out->payload_size)
                            m_user_id = static_cast<int64_t>(GetU64(out->payload + at));
                    }

                    // Slide the remainder down. The caller's payload pointer stays valid only
                    // until the next Poll(), which is documented.
                    memmove(m_in, m_in + size, m_in_used - size);
                    m_in_used -= size;
                    return true;
                }
            }

            if (m_in_used == sizeof(m_in))
            {
                Fail("inbound buffer full");
                Disconnect();
                return false;
            }

            const int got = m_tls.Recv(m_in + m_in_used, sizeof(m_in) - m_in_used);
            if (got < 0)
            {
                Fail("%s", m_tls.LastError());
                Disconnect();
                return false;
            }
            if (got == 0)
                return false;   // nothing more right now
            m_in_used += static_cast<uint32_t>(got);
        }
    }

    namespace
    {
        // The LAN address the server should record. A socket bound to INADDR_ANY reports 0.0.0.0
        // from getsockname, which is useless to a peer - so ask the routing table instead by
        // "connecting" a throwaway UDP socket to the server and reading back the chosen source.
        // No packets are sent by connect() on UDP.
        uint32_t LocalIpv4Towards(uint32_t dest_addr)
        {
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET)
                return 0;

            sockaddr_in dst = {};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(53);
            dst.sin_addr.s_addr = dest_addr ? dest_addr : htonl(0x08080808);

            uint32_t out = 0;
            if (connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0)
            {
                sockaddr_in self = {};
                int len = sizeof(self);
                if (getsockname(s, reinterpret_cast<sockaddr*>(&self), &len) == 0)
                    out = self.sin_addr.s_addr;
            }
            closesocket(s);
            return out;
        }
    }

    bool RpcnClient::OpenSignaling(uint16_t local_port)
    {
        if (m_udp != static_cast<uintptr_t>(~0ull))
            return true;

        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
        {
            Fail("signaling socket() failed (%d)", WSAGetLastError());
            return false;
        }

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        // Bind the P2P port: the same socket carries game traffic later, so the address the server
        // observes here is the one peers will actually use.
        sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = htons(local_port);
        if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            Fail("could not bind UDP %u (%d)", local_port, WSAGetLastError());
            closesocket(s);
            return false;
        }

        m_udp = static_cast<uintptr_t>(s);
        return true;
    }

    bool RpcnClient::SendSignalingPing(int64_t user_id)
    {
        if (m_udp == static_cast<uintptr_t>(~0ull))
            return false;
        if (user_id == 0)
            user_id = m_user_id;
        if (user_id == 0 || m_signaling_addr == 0)
            return false;

        // Exactly 13 bytes or the server rejects it: [0]=1, [1..9]=user_id LE, [9..13]=local IPv4.
        uint8_t pkt[13] = {};
        pkt[0] = 1;
        for (int i = 0; i < 8; ++i)
            pkt[1 + i] = uint8_t(static_cast<uint64_t>(user_id) >> (i * 8));

        // Our LAN address, so the server can hand peers a local address when we share a public IP.
        // Must be the real interface address - see LocalIpv4Towards.
        const uint32_t local_ip = LocalIpv4Towards(m_signaling_addr);
        memcpy(pkt + 9, &local_ip, 4);

        sockaddr_in dst = {};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(kRpcnSignalingPort);
        // Filled by the caller-configured server address; resolved lazily here would need the
        // hostname, so the transport layer sets this up. For now target loopback-safe default.
        dst.sin_addr.s_addr = m_signaling_addr;

        const int sent = sendto(static_cast<SOCKET>(m_udp), reinterpret_cast<const char*>(pkt),
                                sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        return sent == sizeof(pkt);
    }

    bool RpcnClient::SendTo(uint32_t ipv4_be, uint16_t port, const void* data, uint32_t len)
    {
        if (m_udp == static_cast<uintptr_t>(~0ull) || !ipv4_be || !port)
            return false;

        sockaddr_in dst = {};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        dst.sin_addr.s_addr = ipv4_be;

        const int sent = sendto(static_cast<SOCKET>(m_udp), static_cast<const char*>(data),
                                static_cast<int>(len), 0,
                                reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        return sent == static_cast<int>(len);
    }

    int RpcnClient::RecvFrom(void* buf, uint32_t cap, uint32_t* out_ipv4_be, uint16_t* out_port)
    {
        if (m_udp == static_cast<uintptr_t>(~0ull))
            return 0;

        sockaddr_in from = {};
        int from_len = sizeof(from);
        const int got = recvfrom(static_cast<SOCKET>(m_udp), static_cast<char*>(buf),
                                 static_cast<int>(cap), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
        if (got <= 0)
            return 0;

        if (out_ipv4_be) *out_ipv4_be = from.sin_addr.s_addr;
        if (out_port) *out_port = ntohs(from.sin_port);
        return got;
    }

    bool RpcnClient::ParseSignalingReply(const void* buf, uint32_t len,
                                         uint32_t* out_ipv4_be, uint16_t* out_port)
    {
        const uint8_t* p = static_cast<const uint8_t*>(buf);
        if (len < 9 || GetU16(p) != 0 || p[2] != 0)
            return false;
        if (out_ipv4_be) memcpy(out_ipv4_be, p + 3, 4);
        // BIG-endian port here, unlike the rest of the protocol.
        if (out_port) *out_port = static_cast<uint16_t>((p[7] << 8) | p[8]);
        return true;
    }

    bool RpcnClient::PollSignaling(uint32_t* out_ipv4_be, uint16_t* out_port)
    {
        if (m_udp == static_cast<uintptr_t>(~0ull))
            return false;

        uint8_t buf[64];
        sockaddr_in from = {};
        int from_len = sizeof(from);
        const int got = recvfrom(static_cast<SOCKET>(m_udp), reinterpret_cast<char*>(buf),
                                 sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (got < 9)
            return false;

        // Reply: [0..2] VPort=0, [2] subset=0, [3..7] public IPv4, [7..9] public port.
        // NOTE the port is BIG-endian here while the rest of the protocol is little-endian.
        if (GetU16(buf) != 0 || buf[2] != 0)
            return false;

        if (out_ipv4_be)
            memcpy(out_ipv4_be, buf + 3, 4);
        if (out_port)
            *out_port = uint16_t((buf[7] << 8) | buf[8]);
        return true;
    }
}
