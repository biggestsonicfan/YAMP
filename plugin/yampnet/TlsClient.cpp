#include "TlsClient.h"
#include "Winsock.h"

#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <sspi.h>
#include <wincrypt.h>
#include <bcrypt.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace yampnet
{
    namespace
    {
        constexpr uint32_t kIoBuffer = 32 * 1024;

        // The plugin-wide Winsock refcount lives in Winsock.h now (was a private copy here).

        int HexVal(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }
    }

    bool CertFingerprint::FromHex(const char* text)
    {
        is_set = false;
        if (!text)
            return false;

        uint32_t n = 0;
        int hi = -1;
        for (const char* p = text; *p; ++p)
        {
            if (*p == ':' || *p == ' ' || *p == '-')
                continue;
            const int v = HexVal(*p);
            if (v < 0)
                return false;
            if (hi < 0) { hi = v; }
            else
            {
                if (n >= sizeof(bytes)) return false;
                bytes[n++] = static_cast<uint8_t>((hi << 4) | v);
                hi = -1;
            }
        }
        if (hi >= 0 || n != sizeof(bytes))
            return false;
        is_set = true;
        return true;
    }

    void CertFingerprint::ToHex(char* out, uint32_t cap) const
    {
        if (!out || cap < 2 * sizeof(bytes) + 1)
        {
            if (out && cap) out[0] = '\0';
            return;
        }
        static const char* kHex = "0123456789ABCDEF";
        for (uint32_t i = 0; i < sizeof(bytes); ++i)
        {
            out[i * 2] = kHex[bytes[i] >> 4];
            out[i * 2 + 1] = kHex[bytes[i] & 0xF];
        }
        out[sizeof(bytes) * 2] = '\0';
    }

    TlsClient::TlsClient() : m_socket(static_cast<uintptr_t>(INVALID_SOCKET)) {}

    TlsClient::~TlsClient() { Close(); }

    void TlsClient::Fail(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        vsnprintf(m_error, sizeof(m_error), fmt, args);
        va_end(args);
    }

    void TlsClient::Close()
    {
        if (m_have_ctx && m_ctx)
        {
            DeleteSecurityContext(static_cast<CtxtHandle*>(m_ctx));
            m_have_ctx = false;
        }
        if (m_ctx) { free(m_ctx); m_ctx = nullptr; }
        if (m_cred)
        {
            FreeCredentialsHandle(static_cast<CredHandle*>(m_cred));
            free(m_cred);
            m_cred = nullptr;
        }
        if (m_socket != static_cast<uintptr_t>(INVALID_SOCKET))
        {
            closesocket(static_cast<SOCKET>(m_socket));
            m_socket = static_cast<uintptr_t>(INVALID_SOCKET);
        }
        if (m_wsa_held) { WsaRelease(); m_wsa_held = false; }
        free(m_enc); m_enc = nullptr; m_enc_used = 0; m_enc_cap = 0;
        free(m_plain); m_plain = nullptr; m_plain_used = m_plain_off = m_plain_cap = 0;
        m_connected = false;
        m_peer_closed = false;
    }

    bool TlsClient::Connect(const char* host, uint16_t port, const CertFingerprint& pinned)
    {
        Close();

        if (!host || !*host)
        {
            Fail("no host given");
            return false;
        }

        if (!WsaAcquire())
        {
            Fail("WSAStartup failed");
            return false;
        }
        m_wsa_held = true;

        // --- TCP ---------------------------------------------------------------------------
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%u", port);

        addrinfo hints = {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* result = nullptr;
        if (getaddrinfo(host, port_str, &hints, &result) != 0 || !result)
        {
            Fail("could not resolve %s", host);
            return false;
        }

        SOCKET s = INVALID_SOCKET;
        for (addrinfo* ai = result; ai; ai = ai->ai_next)
        {
            s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == INVALID_SOCKET)
                continue;
            if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
                break;
            closesocket(s);
            s = INVALID_SOCKET;
        }
        freeaddrinfo(result);

        if (s == INVALID_SOCKET)
        {
            Fail("could not connect to %s:%u", host, port);
            return false;
        }
        m_socket = static_cast<uintptr_t>(s);

        // --- Credentials -------------------------------------------------------------------
        // MANUAL_CRED_VALIDATION: RPCN's self-signed cert has no SAN and CN="RPCN", so Schannel's
        // own validation could never pass. We verify by fingerprint instead (VerifyPin).
        SCHANNEL_CRED sc = {};
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS
                   | SCH_CRED_NO_SERVERNAME_CHECK;

        m_cred = calloc(1, sizeof(CredHandle));
        m_ctx = calloc(1, sizeof(CtxtHandle));
        if (!m_cred || !m_ctx)
        {
            Fail("out of memory");
            Close();
            return false;
        }

        TimeStamp expiry = {};
        SECURITY_STATUS ss = AcquireCredentialsHandleA(
            nullptr, const_cast<char*>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND, nullptr, &sc,
            nullptr, nullptr, static_cast<CredHandle*>(m_cred), &expiry);
        if (ss != SEC_E_OK)
        {
            Fail("AcquireCredentialsHandle failed (0x%08lX)", static_cast<unsigned long>(ss));
            Close();
            return false;
        }

        m_enc_cap = kIoBuffer;
        m_enc = static_cast<uint8_t*>(malloc(m_enc_cap));
        if (!m_enc) { Fail("out of memory"); Close(); return false; }

        if (!Handshake(host))
        {
            Close();
            return false;
        }
        if (!VerifyCertificate(host, pinned))
        {
            Close();
            return false;
        }

        m_connected = true;
        return true;
    }

    bool TlsClient::Handshake(const char* host)
    {
        SOCKET s = static_cast<SOCKET>(m_socket);
        auto* cred = static_cast<CredHandle*>(m_cred);
        auto* ctx = static_cast<CtxtHandle*>(m_ctx);

        const DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
                        | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

        SECURITY_STATUS ss = SEC_I_CONTINUE_NEEDED;
        bool first = true;
        m_enc_used = 0;

        while (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE)
        {
            // Schannel asks for more bytes until it has a complete flight.
            if (!first && (ss == SEC_E_INCOMPLETE_MESSAGE || m_enc_used == 0))
            {
                if (m_enc_used == m_enc_cap)
                {
                    Fail("handshake buffer overflow");
                    return false;
                }
                const int got = recv(s, reinterpret_cast<char*>(m_enc + m_enc_used),
                                     static_cast<int>(m_enc_cap - m_enc_used), 0);
                if (got <= 0)
                {
                    Fail("connection closed during handshake");
                    return false;
                }
                m_enc_used += static_cast<uint32_t>(got);
            }

            SecBuffer in[2] = {};
            in[0].BufferType = SECBUFFER_TOKEN;
            in[0].pvBuffer = m_enc;
            in[0].cbBuffer = m_enc_used;
            in[1].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc in_desc = { SECBUFFER_VERSION, 2, in };

            SecBuffer out[1] = {};
            out[0].BufferType = SECBUFFER_TOKEN;
            SecBufferDesc out_desc = { SECBUFFER_VERSION, 1, out };

            DWORD attrs = 0;
            TimeStamp expiry = {};
            ss = InitializeSecurityContextA(
                cred,
                first ? nullptr : ctx,
                first ? const_cast<char*>(host) : nullptr,
                req, 0, 0,
                first ? nullptr : &in_desc,
                0, ctx, &out_desc, &attrs, &expiry);

            if (first)
            {
                first = false;
                m_have_ctx = true;
            }

            if (out[0].pvBuffer && out[0].cbBuffer)
            {
                const char* p = static_cast<const char*>(out[0].pvBuffer);
                uint32_t left = out[0].cbBuffer;
                while (left)
                {
                    const int sent = send(s, p, static_cast<int>(left), 0);
                    if (sent <= 0)
                    {
                        FreeContextBuffer(out[0].pvBuffer);
                        Fail("send failed during handshake");
                        return false;
                    }
                    p += sent;
                    left -= static_cast<uint32_t>(sent);
                }
                FreeContextBuffer(out[0].pvBuffer);
            }

            if (ss == SEC_E_INCOMPLETE_MESSAGE)
                continue;   // need more bytes; keep what we have

            if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED)
            {
                // Anything Schannel did not consume is the start of the next flight (or of
                // application data) and must be preserved.
                if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer)
                {
                    memmove(m_enc, m_enc + (m_enc_used - in[1].cbBuffer), in[1].cbBuffer);
                    m_enc_used = in[1].cbBuffer;
                }
                else
                {
                    m_enc_used = 0;
                }
                if (ss == SEC_E_OK)
                    break;
            }
            else
            {
                Fail("TLS handshake failed (0x%08lX)", static_cast<unsigned long>(ss));
                return false;
            }
        }

        SecPkgContext_StreamSizes sizes = {};
        if (QueryContextAttributes(ctx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK)
        {
            Fail("could not query TLS stream sizes");
            return false;
        }
        m_header_size = sizes.cbHeader;
        m_trailer_size = sizes.cbTrailer;
        m_max_message = sizes.cbMaximumMessage;

        m_plain_cap = m_max_message + m_header_size + m_trailer_size + kIoBuffer;
        m_plain = static_cast<uint8_t*>(malloc(m_plain_cap));
        if (!m_plain)
        {
            Fail("out of memory");
            return false;
        }
        return true;
    }

    namespace
    {
        // Schannel's policy errors are numbers users cannot act on. These are the ones a game
        // client actually meets, in the words of what the operator has to go and fix.
        const char* ExplainPolicyError(DWORD err)
        {
            switch (err)
            {
            case CERT_E_EXPIRED:
            case CERT_E_VALIDITYPERIODNESTING:
                return "the server certificate has expired";
            case CERT_E_UNTRUSTEDROOT:
            case CERT_E_UNTRUSTEDTESTROOT:
                return "the server certificate is not signed by a trusted authority "
                       "(a self-signed server needs its fingerprint pinned instead)";
            case CERT_E_CN_NO_MATCH:
                return "the server certificate is not valid for this host name "
                       "(connect by the name on the certificate, or pin the fingerprint)";
            case CERT_E_CHAINING:
                return "incomplete certificate chain - the server is not sending its intermediate "
                       "certificate";
            case CERT_E_WRONG_USAGE:
                return "the server certificate is not valid for server authentication";
            case CERT_E_REVOKED:
                return "the server certificate has been revoked";
            case TRUST_E_CERT_SIGNATURE:
                return "the server certificate's signature is invalid";
            default:
                return "the server certificate could not be validated";
            }
        }
    }

    bool TlsClient::VerifyChain(const void* cert_context, const char* host)
    {
        auto cert = static_cast<PCCERT_CONTEXT>(const_cast<void*>(cert_context));

        CERT_CHAIN_PARA chain_para = {};
        chain_para.cbSize = sizeof(chain_para);
        LPSTR usage[] = { const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH) };
        chain_para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        chain_para.RequestedUsage.Usage.cUsageIdentifier = 1;
        chain_para.RequestedUsage.Usage.rgpszUsageIdentifier = usage;

        // hAdditionalStore = the certificates the server itself sent: that is where the
        // intermediate lives, and without it the chain stops at the leaf and every publicly
        // issued certificate looks untrusted.
        //
        // No revocation flags on purpose. Checking would mean an OCSP/CRL fetch on the connect
        // path, which fails whenever that responder is slow or unreachable - a game client that
        // cannot join a match because someone's OCSP responder is down is worse than one that
        // does not notice a revoked certificate.
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (!CertGetCertificateChain(nullptr, cert, nullptr, cert->hCertStore, &chain_para,
                                     0, nullptr, &chain))
        {
            Fail("could not build a certificate chain (error %lu)", GetLastError());
            return false;
        }

        wchar_t whost[256];
        if (MultiByteToWideChar(CP_UTF8, 0, host, -1, whost,
                                static_cast<int>(_countof(whost))) == 0)
        {
            CertFreeCertificateChain(chain);
            Fail("host name is not usable for certificate validation");
            return false;
        }

        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_para = {};
        ssl_para.cbSize = sizeof(ssl_para);
        ssl_para.dwAuthType = AUTHTYPE_SERVER;
        ssl_para.pwszServerName = whost;

        CERT_CHAIN_POLICY_PARA policy_para = {};
        policy_para.cbSize = sizeof(policy_para);
        policy_para.pvExtraPolicyPara = &ssl_para;

        CERT_CHAIN_POLICY_STATUS status = {};
        status.cbSize = sizeof(status);

        const BOOL checked = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                                                              &policy_para, &status);
        CertFreeCertificateChain(chain);

        if (!checked)
        {
            Fail("certificate policy check failed (error %lu)", GetLastError());
            return false;
        }
        if (status.dwError != 0)
        {
            // The fingerprint goes in the message because this is also how a self-signed server
            // is adopted: the connection is refused, but the value to pin is right there.
            char got[80];
            m_server_fp.ToHex(got, sizeof(got));
            Fail("%s; server presented SHA-256 %s",
                 ExplainPolicyError(status.dwError), got);
            return false;
        }
        return true;
    }

    bool TlsClient::VerifyCertificate(const char* host, const CertFingerprint& pinned)
    {
        auto* ctx = static_cast<CtxtHandle*>(m_ctx);

        PCCERT_CONTEXT cert = nullptr;
        if (QueryContextAttributes(ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK
            || !cert)
        {
            Fail("server presented no certificate");
            return false;
        }

        DWORD hash_len = sizeof(m_server_fp.bytes);
        const BOOL hashed = CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr,
                                                  cert->pbCertEncoded, cert->cbCertEncoded,
                                                  m_server_fp.bytes, &hash_len);
        if (!hashed || hash_len != sizeof(m_server_fp.bytes))
        {
            CertFreeCertificateContext(cert);
            Fail("could not hash server certificate");
            return false;
        }
        m_server_fp.is_set = true;

        // TWO MODES, and which one applies is decided by whether a fingerprint was configured.
        //
        // PINNED: exact match, no chain check. A pinned self-signed certificate cannot chain to
        // anything, and for a single known server the pin is the stronger statement anyway.
        //
        // UNPINNED: full chain + host name validation, as any HTTPS client would do. This used to
        // be trust-on-first-use, which was fine while every server was self-signed and pinned, and
        // is wrong the moment a real certificate is in play: a publicly issued certificate is
        // REISSUED on every renewal, so its fingerprint changes every couple of months. Pinning
        // one would break every client at each renewal - the correct answer there is to validate
        // it properly and leave the pin empty.
        bool ok;
        if (pinned.is_set)
        {
            ok = memcmp(pinned.bytes, m_server_fp.bytes, sizeof(pinned.bytes)) == 0;
            if (!ok)
            {
                char got[80];
                m_server_fp.ToHex(got, sizeof(got));
                Fail("certificate fingerprint mismatch (server presented %s)", got);
            }
        }
        else
        {
            ok = VerifyChain(cert, host);
        }

        CertFreeCertificateContext(cert);
        return ok;
    }

    bool TlsClient::SendAll(const void* data, uint32_t len)
    {
        if (!m_connected)
            return false;

        auto* ctx = static_cast<CtxtHandle*>(m_ctx);
        SOCKET s = static_cast<SOCKET>(m_socket);
        const uint8_t* src = static_cast<const uint8_t*>(data);

        while (len)
        {
            const uint32_t chunk = (len < m_max_message) ? len : m_max_message;

            const uint32_t total = m_header_size + chunk + m_trailer_size;
            uint8_t* rec = static_cast<uint8_t*>(malloc(total));
            if (!rec) { Fail("out of memory"); return false; }
            memcpy(rec + m_header_size, src, chunk);

            SecBuffer bufs[4] = {};
            bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
            bufs[0].pvBuffer = rec;
            bufs[0].cbBuffer = m_header_size;
            bufs[1].BufferType = SECBUFFER_DATA;
            bufs[1].pvBuffer = rec + m_header_size;
            bufs[1].cbBuffer = chunk;
            bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
            bufs[2].pvBuffer = rec + m_header_size + chunk;
            bufs[2].cbBuffer = m_trailer_size;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };

            const SECURITY_STATUS ss = EncryptMessage(ctx, 0, &desc, 0);
            if (ss != SEC_E_OK)
            {
                free(rec);
                Fail("EncryptMessage failed (0x%08lX)", static_cast<unsigned long>(ss));
                return false;
            }

            const uint32_t out_len = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
            const char* p = reinterpret_cast<const char*>(rec);
            uint32_t left = out_len;
            while (left)
            {
                const int sent = send(s, p, static_cast<int>(left), 0);
                if (sent <= 0)
                {
                    free(rec);
                    Fail("send failed");
                    return false;
                }
                p += sent;
                left -= static_cast<uint32_t>(sent);
            }
            free(rec);

            src += chunk;
            len -= chunk;
        }
        return true;
    }

    // Decrypts whatever complete records are sitting in m_enc into m_plain.
    bool TlsClient::DecryptPending()
    {
        auto* ctx = static_cast<CtxtHandle*>(m_ctx);

        while (m_enc_used)
        {
            SecBuffer bufs[4] = {};
            bufs[0].BufferType = SECBUFFER_DATA;
            bufs[0].pvBuffer = m_enc;
            bufs[0].cbBuffer = m_enc_used;
            bufs[1].BufferType = SECBUFFER_EMPTY;
            bufs[2].BufferType = SECBUFFER_EMPTY;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };

            const SECURITY_STATUS ss = DecryptMessage(ctx, &desc, 0, nullptr);
            if (ss == SEC_E_INCOMPLETE_MESSAGE)
                return true;    // wait for more bytes
            if (ss == SEC_I_CONTEXT_EXPIRED)
            {
                // Graceful TLS shutdown. Not an error yet: deliver what we already hold.
                m_peer_closed = true;
                m_enc_used = 0;
                return true;
            }
            if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE)
            {
                Fail("DecryptMessage failed (0x%08lX)", static_cast<unsigned long>(ss));
                return false;
            }

            for (int i = 0; i < 4; ++i)
            {
                if (bufs[i].BufferType != SECBUFFER_DATA || !bufs[i].cbBuffer)
                    continue;
                // Compact consumed plaintext before appending so a long-lived connection does not
                // creep forward through the buffer.
                if (m_plain_off && m_plain_off == m_plain_used)
                    m_plain_off = m_plain_used = 0;
                if (m_plain_used + bufs[i].cbBuffer > m_plain_cap)
                {
                    Fail("plaintext buffer overflow");
                    return false;
                }
                memcpy(m_plain + m_plain_used, bufs[i].pvBuffer, bufs[i].cbBuffer);
                m_plain_used += bufs[i].cbBuffer;
            }

            uint32_t extra = 0;
            for (int i = 0; i < 4; ++i)
            {
                if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer)
                    extra = bufs[i].cbBuffer;
            }
            if (extra)
            {
                memmove(m_enc, m_enc + (m_enc_used - extra), extra);
                m_enc_used = extra;
            }
            else
            {
                m_enc_used = 0;
            }
        }
        return true;
    }

    int TlsClient::Recv(void* buf, uint32_t cap)
    {
        if (!m_connected)
            return -1;

        // Hand back buffered plaintext first.
        if (m_plain_off < m_plain_used)
        {
            const uint32_t avail = m_plain_used - m_plain_off;
            const uint32_t take = (avail < cap) ? avail : cap;
            memcpy(buf, m_plain + m_plain_off, take);
            m_plain_off += take;
            if (m_plain_off == m_plain_used)
                m_plain_off = m_plain_used = 0;
            return static_cast<int>(take);
        }

        // Buffer is drained; now a pending close becomes the answer.
        if (m_peer_closed)
        {
            Fail("connection closed by server");
            return -1;
        }

        SOCKET s = static_cast<SOCKET>(m_socket);

        // Non-blocking peek at the socket: this is called from the per-frame poll, so it must
        // never block the game loop.
        u_long pending = 0;
        if (ioctlsocket(s, FIONREAD, &pending) != 0)
        {
            Fail("ioctlsocket failed");
            return -1;
        }
        if (pending == 0)
            return 0;

        if (m_enc_used == m_enc_cap)
        {
            Fail("encrypted buffer full");
            return -1;
        }
        const int got = recv(s, reinterpret_cast<char*>(m_enc + m_enc_used),
                             static_cast<int>(m_enc_cap - m_enc_used), 0);
        if (got == 0)
        {
            // TCP FIN. Decrypt anything still buffered before declaring the connection over.
            m_peer_closed = true;
            if (!DecryptPending())
                return -1;
            return (m_plain_off < m_plain_used) ? Recv(buf, cap) : -1;
        }
        if (got < 0)
        {
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                return 0;
            Fail("recv failed (%d)", WSAGetLastError());
            return -1;
        }
        m_enc_used += static_cast<uint32_t>(got);

        if (!DecryptPending())
            return -1;

        if (m_plain_off < m_plain_used)
            return Recv(buf, cap);
        return 0;
    }
}
