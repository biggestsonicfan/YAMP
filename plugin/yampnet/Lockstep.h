#pragma once

// The lockstep core: input rings, packet encode/decode, the round barrier and frame accounting.
//
// Deliberately free of Windows, sockets and m2ftg types so it can be reasoned about (and unit
// tested) on its own. Transport hands it bytes; PadCodec turns pads into input words. Modelled
// directly on the Sonic the Fighters PS3 netcode:
//
//   * DELAY-BASED LOCKSTEP. Inputs are keyed by ABSOLUTE FRAME NUMBER into a per-player ring.
//     The simulation may advance frame N only once every player's input for N is known. There is
//     no rollback and no state snapshotting - the PS3 had none either.
//   * REDUNDANCY. Every packet re-carries the last kRedundancy frames of that player's input, so
//     a dropped datagram is repaired by the next one instead of stalling the round. This is why
//     the transport can be lossy/unordered and still never desync.
//   * NEWEST-WINS INSERTION. A ring slot is only overwritten when the incoming frame is newer
//     than what is there, which makes ingest idempotent and safe against duplicates and
//     reordering - the property that makes the redundancy free.
//   * GENERATION. A 5-bit-ish round counter fences off inputs belonging to a previous round, so
//     late packets from the round that just ended cannot poison the new one.

#include <stdint.h>
#include <stddef.h>   // offsetof, for kLinkHeaderBytes

namespace yampnet
{
    // 1024 frames of history per player, as on PS3 (~17 s at 60 Hz). Power of two: the ring index
    // is frame & kRingMask.
    inline constexpr uint32_t kRingSize = 1024;
    inline constexpr uint32_t kRingMask = kRingSize - 1;

    // Frames of input re-sent in every packet. The PS3 used 10; that is the loss burst we can
    // absorb without stalling.
    inline constexpr uint32_t kRedundancy = 10;

    // This build is 1v1 (StF). The PS3 relay walked 8 slots; the rings are per-player so raising
    // this is mostly a matter of the barrier mask and the transport.
    inline constexpr uint32_t kMaxPlayers = 2;

    inline constexpr uint32_t kInvalidFrame = 0xFFFFFFFFu;

    // Wire record. Fixed size, little-endian, no padding surprises - all members are 4-byte
    // aligned and the struct is memcpy'd straight into the datagram.
    struct InputRecord
    {
        uint32_t frame;                  // newest frame carried
        uint32_t packed;                 // player << 29 | generation << 24
        uint32_t inputs[kRedundancy];    // [0] = frame, [1] = frame-1, ... [9] = frame-9
    };
    static_assert(sizeof(InputRecord) == 8 + kRedundancy * 4, "InputRecord must stay packed");

    // --- Wire framing ---------------------------------------------------------------------
    // Every datagram starts with a PacketHeader. Two kinds only: per-frame input, and the round
    // announcement that drives the barrier.
    enum PacketType : uint8_t
    {
        kPacketInput = 0,
        kPacketAnnounce = 1,
        // The host's match seed, published while merely IN A ROOM. Carries the same payload as an
        // announce and is deliberately NOT the same packet: an announce feeds the barrier, and a
        // seed heartbeat must never be able to release one. See AnnouncePacket.
        kPacketSeed = 2,
        // A LINKED-CABINET payload: the game's own inter-cabinet link, carried verbatim.
        //
        // Nothing else on this wire looks like it. The lockstep packets carry INPUTS and are
        // interpreted here; this one is opaque bytes that only the emulated hardware understands,
        // and the plugin's entire job is to move it. Virtual On is the first user: two Model 2
        // cabinets exchange a 0x700-byte state snapshot every frame over a serial ring, and
        // running that over the wire is what makes two machines one linked pair.
        //
        // Consequences that make it a separate type rather than a flag on an input packet:
        //   * it flows OUTSIDE a round - the cabinets link during their boot-time network check,
        //     long before anything presses Start, and they stay linked between matches;
        //   * it must never touch the barrier, the input rings or the state-check machinery;
        //   * it is lossy by design - the payload is a complete snapshot, so newest-wins ingest
        //     loses nothing, and the game's own protocol does the synchronising.
        kPacketLink = 3,
    };

    struct PacketHeader
    {
        uint8_t type;
        uint8_t player;
        uint8_t generation;
        uint8_t reserved;
        // Low 32 bits of the room id. Game traffic is plain P2P between two addresses, so a
        // LEFTOVER PROCESS from a previous test - same machines, same port, same player ids, same
        // generation - was indistinguishable from the real peer and could join a session it was
        // never in. Stamping the room makes cross-room traffic self-identifying and free to drop.
        // 0 means "room unknown", which only happens before a room is taken.
        uint32_t session;
    };

    // Every input packet also carries this peer's most recent STATE CHECK: a value that must be
    // identical on both machines for a given frame (YAMP supplies the ROM's own frame_counter,
    // which advances exactly once per emulated frame, so a mismatch is real divergence and never
    // a timing artefact). It rides along rather than getting its own packet because a datagram is
    // already going out every frame, and a check that is lost with its packet costs nothing - the
    // next frame carries the next one.
    inline constexpr uint32_t kNoCheck = 0xFFFFFFFFu;

    struct InputPacket
    {
        PacketHeader header;
        InputRecord record;
        uint32_t check_frame;    // kNoCheck when this peer has not completed a frame yet
        uint32_t check_value;
    };
    static_assert(sizeof(PacketHeader) == 8, "PacketHeader must stay 8 bytes");

    // The largest link payload the channel will carry. Sized for Virtual On's 0x700; a game with
    // a bigger one needs this raised on BOTH peers, which is what the ABI version is for.
    inline constexpr uint32_t kLinkPayloadMax = 0x700;

    // Copies of each link datagram put on the wire. THREE, for the same reason the lockstep path
    // re-carries ten frames of input: the payload's protocol has single-frame states that a lossy
    // wire will otherwise eat. See the note in ApiLinkSend - this is not belt-and-braces, it is a
    // measured fix for a stage desync that a one-shot send reproduces.
    inline constexpr uint32_t kLinkRedundancy = 3;

    // How `payload` is encoded. Self-describing on the wire rather than implied by a version,
    // because the encoder falls back to raw whenever coding would EXPAND the packet - which it can,
    // for an incompressible payload - so both forms occur in a healthy session.
    enum LinkFormat : uint32_t
    {
        kLinkRaw = 0,
        kLinkRle = 1,
    };

    struct LinkPacket
    {
        PacketHeader header;
        uint32_t format;                     // LinkFormat
        uint32_t raw_len;                    // size after decoding
        uint32_t len;                        // bytes of `payload` actually on the wire
        uint8_t payload[kLinkPayloadMax];
    };

    // Everything before `payload`. From offsetof rather than a hand-added sum, so it cannot drift
    // from the struct - and it is the one number both the sender's length and the receiver's
    // bounds check are computed from.
    inline constexpr uint32_t kLinkHeaderBytes =
        static_cast<uint32_t>(offsetof(LinkPacket, payload));

    // ---- Link payload compression -------------------------------------------------------------
    //
    // WHY THIS EXISTS. Virtual On's link payload is 0x700 bytes, which with a header is ~1804 on
    // the wire against a 1500-byte path MTU: every datagram IP-fragments, and on the open internet
    // a lost fragment loses the whole datagram. Measured on a live two-machine match, the payload
    // is ~1600/1792 zero bytes and RLE-codes to 326 bytes on average, 333 at its densest. So a
    // byte-wise RLE takes it under the MTU with room to spare and cuts the rate from ~200 KB/s to
    // ~20 KB/s each way.
    //
    // STATELESS, deliberately, and that is the whole reason it is RLE and not a delta. Only ~5
    // bytes change per frame, so delta-coding would win more - but a delta is meaningless unless
    // the receiver holds the exact baseline it was coded against, which needs acknowledgement and
    // retransmission. That would destroy the property the entire design rests on: every packet is
    // a complete snapshot, so loss, duplication and reordering are all free and newest-wins ingest
    // is correct. A compressed packet still stands alone.
    //
    // Format: one control byte, then data.
    //   C <  0x80 : (C + 1) literal bytes follow          (1..128)
    //   C >= 0x80 : (C - 0x80 + 3) copies of the next byte (3..130)
    //
    // Returns the encoded length, or 0 if encoding would not be smaller than the input (the caller
    // then sends it raw). Never writes more than `cap` bytes.
    inline uint32_t RleEncode(const uint8_t* src, uint32_t len, uint8_t* dst, uint32_t cap)
    {
        uint32_t o = 0, i = 0;
        while (i < len)
        {
            uint32_t run = 1;
            while (i + run < len && src[i + run] == src[i] && run < 130) ++run;
            if (run >= 3)
            {
                if (o + 2 > cap || o + 2 >= len) return 0;
                dst[o++] = static_cast<uint8_t>(0x80 + (run - 3));
                dst[o++] = src[i];
                i += run;
                continue;
            }
            // Literals, up to 128, stopping early where a codeable run begins.
            uint32_t lit = 0;
            while (i + lit < len && lit < 128)
            {
                uint32_t ahead = 1;
                while (i + lit + ahead < len && src[i + lit + ahead] == src[i + lit] && ahead < 3)
                    ++ahead;
                if (ahead >= 3) break;
                ++lit;
            }
            if (lit == 0) lit = 1;
            if (o + 1 + lit > cap || o + 1 + lit >= len) return 0;
            dst[o++] = static_cast<uint8_t>(lit - 1);
            for (uint32_t k = 0; k < lit; ++k) dst[o++] = src[i + k];
            i += lit;
        }
        return o;
    }

    // Returns bytes written, or 0 if the input is malformed or would overrun `cap`. Every bound is
    // checked: this decodes data straight off the wire into a buffer the emulated hardware reads.
    inline uint32_t RleDecode(const uint8_t* src, uint32_t len, uint8_t* dst, uint32_t cap)
    {
        uint32_t o = 0, i = 0;
        while (i < len)
        {
            const uint8_t c = src[i++];
            if (c >= 0x80)
            {
                const uint32_t run = static_cast<uint32_t>(c - 0x80) + 3;
                if (i >= len || o + run > cap) return 0;
                const uint8_t v = src[i++];
                for (uint32_t k = 0; k < run; ++k) dst[o++] = v;
            }
            else
            {
                const uint32_t lit = static_cast<uint32_t>(c) + 1;
                if (i + lit > len || o + lit > cap) return 0;
                for (uint32_t k = 0; k < lit; ++k) dst[o++] = src[i + k];
                i += lit;
            }
        }
        return o;
    }

    // The round announcement doubles as seed distribution: the host's announce carries the
    // authoritative match seed and the guest adopts it. Both peers must re-seed the emulator's
    // `rand` from the same value or the simulations diverge - this is the PS3's shared-match-seed
    // mechanism, carried on the barrier packet rather than in room data.
    //
    // The barrier is NOT early enough on its own. A guest that presses Start before the host has
    // no seed yet - announces only begin when a peer calls BeginRound - so it seeded its emulator
    // with 0 and reset its board from a generator the host did not share. Whichever player pressed
    // first decided whether the round could work. The host therefore also publishes the same
    // payload as kPacketSeed from the moment the room exists, which makes the order irrelevant.
    struct AnnouncePacket
    {
        PacketHeader header;
        uint32_t seed;
    };

    inline uint32_t PackHeader(uint32_t player, uint32_t generation)
    {
        return (player << 29) | ((generation & 0x1Fu) << 24);
    }
    inline uint32_t UnpackPlayer(uint32_t packed) { return packed >> 29; }
    inline uint32_t UnpackGeneration(uint32_t packed) { return (packed >> 24) & 0x1Fu; }

    // A single player's input history.
    class InputRing
    {
    public:
        void Clear();

        // Newest-wins. Returns true if the slot was updated (i.e. this was new information).
        bool Insert(uint32_t frame, uint32_t input);

        // True if the ring currently holds this exact frame. False once the frame has been
        // overwritten by a newer one that aliases to the same slot.
        bool Has(uint32_t frame) const;

        // Input for `frame`, or 0 when absent - always check Has() first.
        uint32_t Get(uint32_t frame) const;

        uint32_t Newest() const { return m_newest; }

    private:
        struct Slot
        {
            uint32_t frame;
            uint32_t input;
        };
        Slot m_slots[kRingSize] = {};
        bool m_valid[kRingSize] = {};
        uint32_t m_newest = kInvalidFrame;
    };

    // The per-session lockstep state machine.
    class Lockstep
    {
    public:
        // Begin a session. Does not start a round - call BeginRound for that.
        void Configure(uint32_t local_player, uint32_t player_count, uint32_t frame_delay);

        // --- Round barrier ---------------------------------------------------------------
        // Start announcing `generation`. Clears all per-round state (rings, frame counters), so
        // stale inputs from the previous round cannot survive into this one.
        void BeginRound(uint32_t generation);
        // Record that `player` announced `generation`. Ignores announcements for other rounds.
        void OnPeerAnnounce(uint32_t player, uint32_t generation);
        // True once every player in the session has announced the current generation.
        bool BarrierReleased() const;
        uint32_t Generation() const { return m_generation; }

        // --- Input ------------------------------------------------------------------------
        // Record this machine's input for `frame` and fill `out` with the packet to transmit.
        // The record carries this frame plus the previous kRedundancy-1 frames from our own ring.
        void SubmitLocal(uint32_t frame, uint32_t input, InputRecord* out);

        // Ingest a received record. Silently drops records from the wrong generation or an
        // out-of-range player - both are normal on a round boundary, not errors.
        void OnRecord(const InputRecord& rec);

        // True when every player's input for `frame` is known and the sim may advance.
        bool Ready(uint32_t frame) const;

        uint32_t InputFor(uint32_t player, uint32_t frame) const;

        // Newest frame we have submitted locally, or kInvalidFrame before the first submit.
        // step() uses this so a frame is transmitted exactly once even when it re-polls while
        // stalled waiting for the peer.
        uint32_t LastLocalFrame() const { return m_last_local_frame; }

        uint32_t FrameDelay() const { return m_frame_delay; }
        uint32_t LocalPlayer() const { return m_local_player; }
        uint32_t PlayerCount() const { return m_player_count; }

        // Diagnostics
        uint32_t StallCount() const { return m_stalls; }
        void NoteStall() { ++m_stalls; }

    private:
        InputRing m_rings[kMaxPlayers];
        uint32_t m_local_player = 0;
        uint32_t m_player_count = kMaxPlayers;
        uint32_t m_frame_delay = 2;
        uint32_t m_generation = 0;
        uint32_t m_announce_mask = 0;    // bit per player that has announced m_generation
        uint32_t m_last_local_frame = kInvalidFrame;
        uint32_t m_stalls = 0;
    };
}
