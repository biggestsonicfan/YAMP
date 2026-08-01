#include "Protobuf.h"

#include <cstring>

namespace yampnet
{
    // ---------------------------------------------------------------------------------------
    // PbWriter
    // ---------------------------------------------------------------------------------------

    void PbWriter::Put(uint8_t b)
    {
        if (!m_ok || m_used >= m_cap) { m_ok = false; return; }
        m_buf[m_used++] = b;
    }

    void PbWriter::RawVarint(uint64_t v)
    {
        do
        {
            uint8_t byte = static_cast<uint8_t>(v & 0x7F);
            v >>= 7;
            if (v) byte |= 0x80;
            Put(byte);
        } while (v && m_ok);
    }

    void PbWriter::Varint(uint32_t field, uint64_t value)
    {
        RawVarint((static_cast<uint64_t>(field) << 3) | kWireVarint);
        RawVarint(value);
    }

    void PbWriter::Bytes(uint32_t field, const void* data, uint32_t len)
    {
        RawVarint((static_cast<uint64_t>(field) << 3) | kWireLen);
        RawVarint(len);
        if (!m_ok) return;
        if (m_used + len > m_cap) { m_ok = false; return; }
        if (len) memcpy(m_buf + m_used, data, len);
        m_used += len;
    }

    void PbWriter::String(uint32_t field, const char* s)
    {
        Bytes(field, s, static_cast<uint32_t>(s ? strlen(s) : 0));
    }

    void PbWriter::Wrapped(uint32_t field, uint32_t value)
    {
        const uint32_t token = BeginSub(field);
        Varint(1, value);
        EndSub(token);
    }

    uint32_t PbWriter::BeginSub(uint32_t field)
    {
        RawVarint((static_cast<uint64_t>(field) << 3) | kWireLen);
        // Reserve a single length byte. Submessages here are far below 128 bytes; if one ever
        // grows past that, EndSub shifts the payload to make room rather than corrupting it.
        Put(0);
        return m_used;   // token = offset of the first payload byte
    }

    void PbWriter::EndSub(uint32_t token)
    {
        if (!m_ok) return;
        const uint32_t len = m_used - token;

        if (len < 128)
        {
            m_buf[token - 1] = static_cast<uint8_t>(len);
            return;
        }

        // Length needs more than one byte: work out how many, shift the payload up, and rewrite.
        uint32_t extra = 0;
        for (uint64_t v = len >> 7; v; v >>= 7) ++extra;

        if (m_used + extra > m_cap) { m_ok = false; return; }
        memmove(m_buf + token + extra, m_buf + token, len);

        uint32_t at = token - 1;
        uint64_t v = len;
        do
        {
            uint8_t byte = static_cast<uint8_t>(v & 0x7F);
            v >>= 7;
            if (v) byte |= 0x80;
            m_buf[at++] = byte;
        } while (v);

        m_used += extra;
    }

    // ---------------------------------------------------------------------------------------
    // PbReader
    // ---------------------------------------------------------------------------------------

    bool PbReader::Next()
    {
        if (!m_ok || m_pos >= m_len)
            return false;

        auto read_varint = [&](uint64_t& out) -> bool {
            out = 0;
            uint32_t shift = 0;
            while (m_pos < m_len)
            {
                const uint8_t b = m_data[m_pos++];
                out |= static_cast<uint64_t>(b & 0x7F) << shift;
                if (!(b & 0x80))
                    return true;
                shift += 7;
                if (shift > 63)
                    return false;
            }
            return false;
        };

        uint64_t tag = 0;
        if (!read_varint(tag)) { m_ok = false; return false; }

        m_field = static_cast<uint32_t>(tag >> 3);
        m_wire = static_cast<uint32_t>(tag & 7);
        m_bytes = nullptr;
        m_bytes_len = 0;
        m_varint = 0;

        switch (m_wire)
        {
        case kWireVarint:
            if (!read_varint(m_varint)) { m_ok = false; return false; }
            return true;

        case kWireLen:
        {
            uint64_t len = 0;
            if (!read_varint(len) || m_pos + len > m_len) { m_ok = false; return false; }
            m_bytes = m_data + m_pos;
            m_bytes_len = static_cast<uint32_t>(len);
            m_pos += static_cast<uint32_t>(len);
            return true;
        }

        case 5:   // fixed32
            if (m_pos + 4 > m_len) { m_ok = false; return false; }
            m_pos += 4;
            return true;

        case 1:   // fixed64
            if (m_pos + 8 > m_len) { m_ok = false; return false; }
            m_pos += 8;
            return true;

        default:
            m_ok = false;
            return false;
        }
    }

    uint32_t PbReader::AsWrapped() const
    {
        if (m_wire != kWireLen || !m_bytes)
            return 0;
        PbReader sub(m_bytes, m_bytes_len);
        while (sub.Next())
        {
            if (sub.Field() == 1 && sub.WireType() == kWireVarint)
                return static_cast<uint32_t>(sub.AsVarint());
        }
        return 0;
    }
}
