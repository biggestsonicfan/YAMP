#include "PadCodec.h"

#include <cstring>

namespace yampnet
{
    namespace
    {
        // Stick deflection past which a digitised direction bit is asserted. Applied on the
        // SENDING side only: once digitised, the value on the wire is what both machines use, so
        // the threshold itself can change without breaking cross-version determinism mid-round.
        constexpr float kStickDeadzone = 0.5f;

        inline uint32_t Bit(pxd::sl::BUTTON b) { return 1u << static_cast<uint32_t>(b); }
    }

    uint32_t EncodePad(const pxd::lj_pad_t& pad)
    {
        uint32_t input = pad.m_now & kInputMask;

        // Fold the analog stick into the digital stick-direction bits so a stick player and a
        // d-pad player produce the same wire representation - and so the receiver never has to
        // reconstruct a float it was not sent.
        if (pad.m_x1 <= -kStickDeadzone) input |= Bit(pxd::sl::BUTTON_L_LEFT);
        if (pad.m_x1 >= kStickDeadzone) input |= Bit(pxd::sl::BUTTON_L_RIGHT);
        if (pad.m_y1 >= kStickDeadzone) input |= Bit(pxd::sl::BUTTON_L_UP);
        if (pad.m_y1 <= -kStickDeadzone) input |= Bit(pxd::sl::BUTTON_L_DOWN);

        return input & kInputMask;
    }

    void PadHistory::Clear()
    {
        prev_input = 0;
        std::memset(button_frames, 0, sizeof(button_frames));
    }

    void DecodePad(uint32_t input, uint32_t player, PadHistory& hist, pxd::lj_pad_t& out)
    {
        input &= kInputMask;
        const uint32_t prev = hist.prev_input;

        // Preserve the module-owned tail: the host only ever fills the prefix up to m_port
        // (kLjPadCopyBytes), and the bytes past it belong to the module across frames.
        std::memset(&out, 0, pxd::kLjPadCopyBytes);

        out.m_now = input;
        out.m_prev = prev;
        out.m_push = input & ~prev;      // pressed this frame
        out.m_pull = ~input & prev;      // released this frame

        // Re-derive the axes from the digitised bits. Both machines run this identically, so the
        // floats are guaranteed to match bit-for-bit (values are exact in binary floating point).
        const bool left = (input & (Bit(pxd::sl::BUTTON_LEFT) | Bit(pxd::sl::BUTTON_L_LEFT))) != 0;
        const bool right = (input & (Bit(pxd::sl::BUTTON_RIGHT) | Bit(pxd::sl::BUTTON_L_RIGHT))) != 0;
        const bool up = (input & (Bit(pxd::sl::BUTTON_UP) | Bit(pxd::sl::BUTTON_L_UP))) != 0;
        const bool down = (input & (Bit(pxd::sl::BUTTON_DOWN) | Bit(pxd::sl::BUTTON_L_DOWN))) != 0;

        out.m_x1 = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
        out.m_y1 = (up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f);
        out.m_x2 = 0.0f;
        out.m_y2 = 0.0f;

        for (uint32_t i = 0; i < 32; ++i)
        {
            const bool held = (input & (1u << i)) != 0;
            const bool was_held = (prev & (1u << i)) != 0;

            // Pressure bytes are binary here: the wire format carries no analog trigger value, so
            // inventing one would differ between the machine that has the real trigger and the
            // one that does not.
            out.m_buttons[i] = held ? 0xFF : 0x00;
            out.m_prev_buttons[i] = was_held ? 0xFF : 0x00;

            hist.button_frames[i] = held ? (hist.button_frames[i] + 1) : 0;
            out.m_button_frame[i] = hist.button_frames[i];
        }

        // Identical on every machine on purpose - see the header. m_is_remote in particular must
        // NOT reflect who is local.
        out.m_port = player;
        out.m_user_id = static_cast<int>(player);
        out.m_is_connected = true;
        out.m_is_remote = false;

        hist.prev_input = input;
    }
}
