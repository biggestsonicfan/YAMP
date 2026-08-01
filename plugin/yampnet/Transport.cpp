#include "Transport.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>

namespace yampnet
{
    namespace
    {
        // Winsock needs process-wide startup. Refcounted so opening/closing transports repeatedly
        // (host, then join, then host again) does not tear it down under a live socket.
        int g_wsa_refs = 0;

        bool WsaAcquire()
        {
            if (g_wsa_refs++ == 0)
            {
                WSADATA data = {};
                if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                {
                    g_wsa_refs = 0;
                    return false;
                }
            }
            return true;
        }

        void WsaRelease()
        {
            if (g_wsa_refs > 0 && --g_wsa_refs == 0)
                WSACleanup();
        }
    }

    UdpTransport::UdpTransport() : m_socket(static_cast<uintptr_t>(INVALID_SOCKET)) {}

    UdpTransport::~UdpTransport() { Close(); }

    bool UdpTransport::Open(bool host, const char* address, uint16_t port)
    {
        Close();

        if (!WsaAcquire())
        {
            snprintf(m_error, sizeof(m_error), "WSAStartup failed");
            return false;
        }

        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
        {
            snprintf(m_error, sizeof(m_error), "socket() failed (%d)", WSAGetLastError());
            WsaRelease();
            return false;
        }

        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);

        sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        // The guest binds an ephemeral port; the host binds the well-known one so the guest can
        // find it. The host learns the guest's address from its first datagram.
        local.sin_port = host ? htons(port) : 0;

        if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
        {
            snprintf(m_error, sizeof(m_error), "bind(%u) failed (%d)", port, WSAGetLastError());
            closesocket(s);
            WsaRelease();
            return false;
        }

        m_is_host = host;
        m_peer_known = false;
        std::memset(m_peer_addr, 0, sizeof(m_peer_addr));

        if (!host)
        {
            sockaddr_in peer = {};
            peer.sin_family = AF_INET;
            peer.sin_port = htons(port);
            if (inet_pton(AF_INET, address ? address : "127.0.0.1", &peer.sin_addr) != 1)
            {
                snprintf(m_error, sizeof(m_error), "bad peer address '%s'",
                         address ? address : "(null)");
                closesocket(s);
                WsaRelease();
                return false;
            }
            static_assert(sizeof(sockaddr_in) <= sizeof(m_peer_addr), "peer addr storage too small");
            std::memcpy(m_peer_addr, &peer, sizeof(peer));
            m_peer_known = true;   // guest can transmit immediately; host waits to be found
        }

        m_socket = static_cast<uintptr_t>(s);
        m_error[0] = '\0';
        return true;
    }

    void UdpTransport::Close()
    {
        if (m_socket != static_cast<uintptr_t>(INVALID_SOCKET))
        {
            closesocket(static_cast<SOCKET>(m_socket));
            m_socket = static_cast<uintptr_t>(INVALID_SOCKET);
            WsaRelease();
        }
        m_peer_known = false;
    }

    bool UdpTransport::Send(uint32_t /*peer*/, const void* data, uint32_t len)
    {
        if (m_socket == static_cast<uintptr_t>(INVALID_SOCKET) || !m_peer_known)
            return false;

        sockaddr_in peer = {};
        std::memcpy(&peer, m_peer_addr, sizeof(peer));

        const int sent = sendto(static_cast<SOCKET>(m_socket),
                                static_cast<const char*>(data), static_cast<int>(len), 0,
                                reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
        if (sent == SOCKET_ERROR)
        {
            const int err = WSAGetLastError();
            // WOULDBLOCK just means the send buffer is momentarily full - a dropped datagram,
            // which the redundancy is there to absorb. Not an error worth reporting.
            if (err != WSAEWOULDBLOCK)
                snprintf(m_error, sizeof(m_error), "sendto failed (%d)", err);
            return false;
        }
        return true;
    }

    int UdpTransport::Recv(void* buf, uint32_t cap, uint32_t* out_peer)
    {
        if (m_socket == static_cast<uintptr_t>(INVALID_SOCKET))
            return 0;

        sockaddr_in from = {};
        int from_len = sizeof(from);
        const int got = recvfrom(static_cast<SOCKET>(m_socket), static_cast<char*>(buf),
                                 static_cast<int>(cap), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
        if (got == SOCKET_ERROR)
            return 0;   // WOULDBLOCK / ICMP port-unreachable; nothing pending

        // The host adopts the first sender it hears from as its peer.
        if (m_is_host && !m_peer_known)
        {
            std::memcpy(m_peer_addr, &from, sizeof(from));
            m_peer_known = true;
        }

        if (out_peer)
            *out_peer = 0;
        return got;
    }
}
