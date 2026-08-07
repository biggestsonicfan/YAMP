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

    struct LinkPacket
    {
        PacketHeader header;
        uint32_t len;                        // bytes of `payload` that are live
        uint8_t payload[kLinkPayloadMax];
    };

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
