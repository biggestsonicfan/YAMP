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

        // THE VERTICAL AXIS POINTS DOWN. m_y1 is POSITIVE when the player pushes DOWN - that is
        // the sl convention the whole host uses, and it is not a guess: source/input/Input.h says
        // so on PadState ("y+ = down (sl convention)"), the XInput reader negates sThumbLY to get
        // there, and csl_pad::set_state writes -1.0f for UP and +1.0f for DOWN.
        //
        // This file had it backwards in both directions, and the round trip turned a vertical
        // press into nothing at all rather than into its opposite: pressing UP set BUTTON_UP in
        // m_now AND, through the sign error here, BUTTON_L_DOWN as well - so the receiver saw up
        // and down held together, cancelled them to m_y1 = 0.0f, and handed the board a pad with
        // both directions asserted. Horizontal was unaffected, which is why it survived being
        // played: x is negative-left on both sides.
        constexpr float kUp = -1.0f;
        constexpr float kDown = 1.0f;

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
        if (pad.m_y1 <= kUp * kStickDeadzone) input |= Bit(pxd::sl::BUTTON_L_UP);
        if (pad.m_y1 >= kDown * kStickDeadzone) input |= Bit(pxd::sl::BUTTON_L_DOWN);

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

        // Same convention csl_pad::set_state fills a local pad with, or the module is handed one
        // pad that means "up" and one that means "down" depending on which machine produced it.
        out.m_x1 = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
        out.m_y1 = (up ? kUp : 0.0f) + (down ? kDown : 0.0f);
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
