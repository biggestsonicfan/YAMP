#pragma once

// Minimal protobuf wire-format reader/writer - just enough for RPCN's np2_structs.proto.
//
// RPCN's room commands carry protobuf payloads (the server uses prost), unlike Login/Create which
// are plain NUL-terminated strings. Rather than pull in a protobuf runtime for a handful of
// messages, this encodes the wire format directly: it is only varints and length-delimited
// blobs, and vendoring a compiler into a plugin that is meant to stay small and swappable is a
// poor trade.
//
// ONE TRAP WORTH KNOWING: np2_structs.proto defines `uint8` and `uint16` as MESSAGES
// (`message uint16 { uint32 value = 1; }`, kept from the flatbuffers port), not scalars. So a
// field declared `uint16 serverId = 1` is a length-delimited SUBMESSAGE containing a varint,
// not a bare varint. WriteWrapped/ReadWrapped exist for exactly that.

#include <stdint.h>

namespace yampnet
{
    enum : uint32_t
    {
        kWireVarint = 0,
        kWireLen = 2,
    };

    class PbWriter
    {
    public:
        PbWriter(uint8_t* buf, uint32_t cap) : m_buf(buf), m_cap(cap) {}

        void Varint(uint32_t field, uint64_t value);
        void Bytes(uint32_t field, const void* data, uint32_t len);
        void String(uint32_t field, const char* s);

        // Writes a `uint8`/`uint16` wrapper submessage: { uint32 value = 1 }.
        void Wrapped(uint32_t field, uint32_t value);

        // Begins a submessage; returns a token to pass to EndSub. Length is back-patched, so
        // nesting does not need a second pass.
        uint32_t BeginSub(uint32_t field);
        void EndSub(uint32_t token);

        uint32_t Size() const { return m_used; }
        bool Ok() const { return m_ok; }

    private:
        void RawVarint(uint64_t v);
        void Put(uint8_t b);

        uint8_t* m_buf;
        uint32_t m_cap;
        uint32_t m_used = 0;
        bool m_ok = true;
    };

    // Forward-only field reader.
    class PbReader
    {
    public:
        PbReader(const uint8_t* data, uint32_t len) : m_data(data), m_len(len) {}

        // Advances to the next field. Returns false at the end or on malformed input.
        bool Next();

        uint32_t Field() const { return m_field; }
        uint32_t WireType() const { return m_wire; }

        uint64_t AsVarint() const { return m_varint; }
        const uint8_t* AsBytes() const { return m_bytes; }
        uint32_t AsBytesLen() const { return m_bytes_len; }

        // Reads a uint8/uint16 wrapper submessage's inner value (field 1). 0 if absent.
        uint32_t AsWrapped() const;

        // Sub-reader over the current length-delimited field.
        PbReader Sub() const { return PbReader(m_bytes, m_bytes_len); }

        bool Ok() const { return m_ok; }

    private:
        const uint8_t* m_data;
        uint32_t m_len;
        uint32_t m_pos = 0;

        uint32_t m_field = 0;
        uint32_t m_wire = 0;
        uint64_t m_varint = 0;
        const uint8_t* m_bytes = nullptr;
        uint32_t m_bytes_len = 0;
        bool m_ok = true;
    };
}
