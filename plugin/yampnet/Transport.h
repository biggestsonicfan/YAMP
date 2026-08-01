#pragma once

// Datagram transport behind an interface, so the lockstep engine never knows what is carrying
// its packets.
//
// RPCN is the intended production backend (rooms + signaling for NAT traversal, then P2P
// datagrams). UdpTransport below is a DEVELOPMENT backend: a direct address:port link with no
// matchmaking, which exists so the lockstep engine can be exercised over loopback or a LAN before
// the RPCN client is written. Keeping both behind ITransport means the engine and the ABI do not
// change when RPCN lands - only which object gets constructed.
//
// Requirements on any backend: datagrams may be lost, duplicated or reordered. The engine's
// newest-wins ingest and per-packet redundancy already assume exactly that, so no backend needs
// to provide reliability or ordering.

#include <stdint.h>

namespace yampnet
{
    class ITransport
    {
    public:
        virtual ~ITransport() = default;

        // Fire-and-forget. Returns false only on a hard socket failure, not on drop.
        virtual bool Send(uint32_t peer, const void* data, uint32_t len) = 0;

        // Non-blocking. Returns bytes received (>0), or 0 when nothing is pending.
        virtual int Recv(void* buf, uint32_t cap, uint32_t* out_peer) = 0;

        // True once the link is usable.
        virtual bool Ready() const = 0;

        virtual const char* LastError() const = 0;
    };

    // Direct UDP link for development/testing. Two endpoints: the "host" binds and learns the
    // peer's address from its first packet; the "guest" is given the host address up front.
    class UdpTransport final : public ITransport
    {
    public:
        UdpTransport();
        ~UdpTransport() override;

        // host == true binds `port` and waits to learn the peer. host == false binds an ephemeral
        // port and targets address:port.
        bool Open(bool host, const char* address, uint16_t port);
        void Close();

        bool Send(uint32_t peer, const void* data, uint32_t len) override;
        int Recv(void* buf, uint32_t cap, uint32_t* out_peer) override;
        bool Ready() const override { return m_peer_known; }
        const char* LastError() const override { return m_error; }

    private:
        uintptr_t m_socket;         // SOCKET, kept opaque to avoid dragging winsock into headers
        bool m_peer_known = false;
        bool m_is_host = false;
        uint8_t m_peer_addr[32] = {};   // sockaddr_in, stored opaquely
        char m_error[128] = {};
    };
}
