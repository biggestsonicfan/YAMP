#pragma once

// Minimal TLS client over TCP, built on Schannel (SSPI).
//
// WHY SCHANNEL: it is what Windows already ships, so the plugin needs no vendored crypto library
// and links only secur32/crypt32. That matters for a DLL that is meant to be dropped next to
// YAMP.exe and updated on its own.
//
// TWO WAYS TO TRUST A SERVER, chosen by whether a fingerprint was configured:
//
//   PINNED (fingerprint given). RPCN's own `--cert-gen` produces a SELF-SIGNED certificate with
//   CN="RPCN" and no subjectAltName (see rpcn/src/main.rs generate_certificate). Ordinary
//   validation cannot succeed against that however the server is reached - there is nothing to
//   match and no chain to build. So the certificate's SHA-256 is compared against the configured
//   value instead. For a single known server that is a stronger guarantee than trusting the whole
//   public CA set.
//
//   VALIDATED (no fingerprint). Full chain + host name validation against the system trust store,
//   as any HTTPS client does. This is the mode for a server with a real certificate on a real
//   domain, and it is why pinning must NOT be used there: a publicly issued certificate is
//   reissued at every renewal (~60 days for Let's Encrypt) and its fingerprint changes with it, so
//   a pin would break every client a couple of months later.
//
// Schannel's own validation stays OFF in both cases (SCH_CRED_MANUAL_CRED_VALIDATION): doing the
// check here is what lets a failure say which of the two modes was in force and what to fix.

#include <stdint.h>

namespace yampnet
{
    // 32-byte SHA-256 certificate fingerprint.
    struct CertFingerprint
    {
        uint8_t bytes[32] = {};
        bool is_set = false;

        // Parses 64 hex chars, optionally colon/space separated. Returns false if malformed.
        bool FromHex(const char* text);
        // Writes 64 hex chars + NUL into `out` (needs >= 65 bytes).
        void ToHex(char* out, uint32_t cap) const;
    };

    class TlsClient
    {
    public:
        TlsClient();
        ~TlsClient();

        TlsClient(const TlsClient&) = delete;
        TlsClient& operator=(const TlsClient&) = delete;

        // Connects and completes the TLS handshake.
        //   pinned  - if set, the server cert's SHA-256 must match exactly or the connection is
        //             refused. If not set, the certificate must instead pass ordinary chain and
        //             host name validation. Either way the fingerprint the server presented is
        //             recorded (see ServerFingerprint) and named in the error text, so a
        //             self-signed server can still be adopted: try once, read the fingerprint out
        //             of the refusal, pin it.
        // Returns false on any failure; LastError() explains.
        bool Connect(const char* host, uint16_t port, const CertFingerprint& pinned);

        void Close();
        bool IsConnected() const { return m_connected; }

        // Blocking send of exactly `len` bytes. Returns false on error.
        bool SendAll(const void* data, uint32_t len);

        // Reads up to `cap` bytes. Returns bytes read, 0 if nothing is available yet
        // (non-fatal), or -1 on error/disconnect.
        int Recv(void* buf, uint32_t cap);

        // Fingerprint of the certificate the server presented on the last handshake. Useful for
        // first-time setup: connect once unpinned, print this, then pin it.
        const CertFingerprint& ServerFingerprint() const { return m_server_fp; }

        const char* LastError() const { return m_error; }

    private:
        bool Handshake(const char* host);
        // Pin comparison or chain validation, per the two modes above.
        bool VerifyCertificate(const char* host, const CertFingerprint& pinned);
        // PCCERT_CONTEXT, kept as void* so the header does not drag in wincrypt.h.
        bool VerifyChain(const void* cert_context, const char* host);
        bool DecryptPending();
        void Fail(const char* fmt, ...);

        uintptr_t m_socket;
        void* m_cred = nullptr;      // CredHandle*
        void* m_ctx = nullptr;       // CtxtHandle*
        bool m_connected = false;
        bool m_have_ctx = false;
        bool m_wsa_held = false;
        // Peer sent close_notify (or the TCP socket closed). Any plaintext already decrypted must
        // still be delivered BEFORE reporting the close - the server replies and then hangs up in
        // the same breath on some errors, and dropping that reply loses the reason it hung up.
        bool m_peer_closed = false;

        // Schannel stream sizes, learned after the handshake.
        uint32_t m_header_size = 0;
        uint32_t m_trailer_size = 0;
        uint32_t m_max_message = 0;

        // Encrypted bytes read from the socket but not yet decrypted, and plaintext decrypted but
        // not yet handed to the caller. TLS is record-oriented; callers are not.
        uint8_t* m_enc = nullptr;
        uint32_t m_enc_used = 0;
        uint32_t m_enc_cap = 0;
        uint8_t* m_plain = nullptr;
        uint32_t m_plain_used = 0;
        uint32_t m_plain_off = 0;
        uint32_t m_plain_cap = 0;

        CertFingerprint m_server_fp;
        char m_error[256] = {};
    };
}
