#include "Lockstep.h"

#include <cstring>

namespace yampnet
{
    // ---------------------------------------------------------------------------------------
    // InputRing
    // ---------------------------------------------------------------------------------------

    void InputRing::Clear()
    {
        std::memset(m_slots, 0, sizeof(m_slots));
        std::memset(m_valid, 0, sizeof(m_valid));
        m_newest = kInvalidFrame;
    }

    bool InputRing::Insert(uint32_t frame, uint32_t input)
    {
        const uint32_t idx = frame & kRingMask;
        Slot& slot = m_slots[idx];

        // Newest-wins. An occupied slot holding a frame >= ours is either this same frame arriving
        // again (duplicate/redundancy) or a NEWER frame that has already lapped the ring - in both
        // cases the stored value is the one to keep, so the write is simply dropped. This is what
        // makes redundant re-sends free and reordering harmless.
        if (m_valid[idx] && slot.frame >= frame)
            return false;

        slot.frame = frame;
        slot.input = input;
        m_valid[idx] = true;

        if (m_newest == kInvalidFrame || frame > m_newest)
            m_newest = frame;
        return true;
    }

    bool InputRing::Has(uint32_t frame) const
    {
        const uint32_t idx = frame & kRingMask;
        return m_valid[idx] && m_slots[idx].frame == frame;
    }

    uint32_t InputRing::Get(uint32_t frame) const
    {
        const uint32_t idx = frame & kRingMask;
        return (m_valid[idx] && m_slots[idx].frame == frame) ? m_slots[idx].input : 0u;
    }

    // ---------------------------------------------------------------------------------------
    // Lockstep
    // ---------------------------------------------------------------------------------------

    void Lockstep::Configure(uint32_t local_player, uint32_t player_count, uint32_t frame_delay)
    {
        m_local_player = (local_player < kMaxPlayers) ? local_player : 0;
        m_player_count = (player_count == 0 || player_count > kMaxPlayers) ? kMaxPlayers
                                                                          : player_count;
        m_frame_delay = frame_delay;
        m_generation = 0;
        m_announce_mask = 0;
        m_last_local_frame = kInvalidFrame;
        m_stalls = 0;
        for (auto& ring : m_rings)
            ring.Clear();
    }

    void Lockstep::BeginRound(uint32_t generation)
    {
        // Wipe the rings. The PS3 barrier does exactly this (zeroing the per-round frame/input
        // block) once every peer has been heard: it guarantees a round can never observe an input
        // belonging to the previous one, which is what the generation field also defends against
        // on the wire.
        m_generation = generation & 0x1Fu;
        m_announce_mask = 1u << m_local_player;   // we have announced by definition
        m_last_local_frame = kInvalidFrame;
        for (auto& ring : m_rings)
            ring.Clear();
    }

    void Lockstep::OnPeerAnnounce(uint32_t player, uint32_t generation)
    {
        if (player >= m_player_count)
            return;
        if ((generation & 0x1Fu) != m_generation)
            return;   // an announcement for a different round; not ours to act on
        m_announce_mask |= (1u << player);
    }

    bool Lockstep::BarrierReleased() const
    {
        const uint32_t expected = (m_player_count >= 32) ? 0xFFFFFFFFu
                                                        : ((1u << m_player_count) - 1u);
        return (m_announce_mask & expected) == expected;
    }

    void Lockstep::SubmitLocal(uint32_t frame, uint32_t input, InputRecord* out)
    {
        InputRing& mine = m_rings[m_local_player];
        mine.Insert(frame, input);
        if (m_last_local_frame == kInvalidFrame || frame > m_last_local_frame)
            m_last_local_frame = frame;

        if (!out)
            return;

        std::memset(out, 0, sizeof(*out));
        out->frame = frame;
        out->packed = PackHeader(m_local_player, m_generation);

        // inputs[0] is this frame, inputs[i] is frame-i. Frames we do not have (start of a round,
        // where frame-i underflows past the beginning) stay 0, which is a neutral pad.
        for (uint32_t i = 0; i < kRedundancy; ++i)
        {
            if (i > frame)
                break;
            const uint32_t f = frame - i;
            if (mine.Has(f))
                out->inputs[i] = mine.Get(f);
        }
    }

    void Lockstep::OnRecord(const InputRecord& rec)
    {
        const uint32_t player = UnpackPlayer(rec.packed);
        const uint32_t gen = UnpackGeneration(rec.packed);

        // Wrong round, or a player index this session does not have. Both are expected around a
        // round boundary (in-flight packets from the round that just ended), so drop quietly.
        if (player >= m_player_count || gen != m_generation)
            return;

        // Never accept a remote record for our own slot - our inputs are authoritative locally,
        // and honouring an echo of them would let the network rewrite local history.
        if (player == m_local_player)
            return;

        InputRing& ring = m_rings[player];
        for (uint32_t i = 0; i < kRedundancy; ++i)
        {
            if (i > rec.frame)
                break;
            ring.Insert(rec.frame - i, rec.inputs[i]);
        }
    }

    bool Lockstep::Ready(uint32_t frame) const
    {
        for (uint32_t p = 0; p < m_player_count; ++p)
        {
            if (!m_rings[p].Has(frame))
                return false;
        }
        return true;
    }

    uint32_t Lockstep::InputFor(uint32_t player, uint32_t frame) const
    {
        if (player >= m_player_count)
            return 0;
        return m_rings[player].Get(frame);
    }
}
