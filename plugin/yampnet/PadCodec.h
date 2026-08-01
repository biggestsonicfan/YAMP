#pragma once

// Conversion between YAMP's pad struct and the compact input word that goes on the wire.
//
// THE DETERMINISM RULE, and the reason this file exists at all:
//
//   Every byte written into a pad must be a pure function of (player index, input words).
//   Nothing may depend on "am I the local player" or on data only the local machine has.
//
// That is why Decode() is applied to BOTH players - including our own - rather than leaving the
// local pad as the input layer produced it. The local machine has richer information (true analog
// axes, per-button pressure, real controller ids) than it transmits; if it fed that to the module
// while the peer fed a reconstruction, the two simulations would drift apart within a few frames
// even though both "had the same inputs". Round-tripping our own pad through the same lossy
// encode/decode as the remote one keeps the two machines bit-identical.
//
// Fields deliberately NOT derived from local state:
//   m_is_remote  - forced false on both machines. If the module ever branched on it, a true/false
//                  split between peers would be an instant desync.
//   m_user_id    - set to the player index, not the real controller/user id.
//   m_port       - player index.

#include <stdint.h>

#include "pxd/LJ/sl.h"

namespace yampnet
{
    // Bits of lj_pad_t::m_now that are transmitted. Covers the face/shoulder/start buttons
    // (0x0-0xB), the d-pad (0xC-0xF) and the digitised stick directions (0x10-0x17), which is
    // everything an m2ftg module can observe. Analog axes are re-derived from these bits rather
    // than sent, so both machines compute them identically.
    inline constexpr uint32_t kInputMask = 0x00FFFFFFu;

    uint32_t EncodePad(const pxd::lj_pad_t& pad);

    // Per-player history the reconstruction needs (held-frame counters and the previous word).
    // Cleared at the start of every round so a round always begins from a known state.
    struct PadHistory
    {
        uint32_t prev_input = 0;
        int32_t button_frames[32] = {};

        void Clear();
    };

    // Rebuild a full pad from an input word. `player` becomes m_port/m_user_id.
    void DecodePad(uint32_t input, uint32_t player, PadHistory& hist, pxd::lj_pad_t& out);
}
