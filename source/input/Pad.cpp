// The pad-fill policy for the shared pxd sl pad type — used by EVERY game YAMP hosts: the four
// m2ftg boards (StF/FV/MR/VF2) and all three VF5FS builds, which share the same P/K/G scheme.
//
// pxd::csl_pad is declared in source/pxd/LJ/sl.h and is engine layout, but WHAT fills it is host
// policy: which physical key/button means Punch depends entirely on the game being hosted. So the
// definition lives here, next to Input (the bindings it reads), rather than in the pxd platform
// layer that every host compiles.

#include "../pxd/LJ/sl.h"

#include "Input.h"
#include "../YAMPGeneral.h"

#include <algorithm>

namespace pxd
{
        // Binding-driven pad state: each player's actions (Input, set up on the YAMP
        // Controls page) are routed to the sl button bits whose meaning is fixed by the
        // assign table StF.cpp hands the module (Input::MODULE_ASSIGN): A=P, B=K, Y=G,
        // X=P+G, LT=P+K+G, LB=P+K, RT=K+G. Escape stays reserved for the host pause menu
        // (StF.cpp GameLoop), matching Lost Judgment. StF.cpp calls Input::PollPads()
        // once per frame before these.
        void csl_pad::set_state(unsigned int index)
        {
            m_prev = std::exchange(m_now, 0);
            m_x1 = m_y1 = 0.0f;

            const unsigned int player = index & 1;
            auto setButton = [this](sl::BUTTON button) {
                m_now |= (1u << button);
                m_buttons[button] = 0xFF;
                };
            auto actionDown = [player](Input::Action action) {
                return Input::ActionDown(player, action);
                };

            if (actionDown(Input::Action_Punch)) setButton(sl::BUTTON_A);
            if (actionDown(Input::Action_Kick)) setButton(sl::BUTTON_B);
            if (actionDown(Input::Action_Guard)) setButton(sl::BUTTON_Y);
            if (actionDown(Input::Action_PG)) setButton(sl::BUTTON_X);
            if (actionDown(Input::Action_PKG)) setButton(sl::BUTTON_LT);
            if (actionDown(Input::Action_PK)) setButton(sl::BUTTON_LB);
            if (actionDown(Input::Action_KG)) setButton(sl::BUTTON_RT);
            if (actionDown(Input::Action_Start)) setButton(sl::BUTTON_START);
            if (actionDown(Input::Action_Back)) setButton(sl::BUTTON_BACK);
            // Action_Coin is not a pad button - StF.cpp turns it into the coin status bit.

            // Movement: bound digital inputs get the dpad bits + full analog deflection;
            // the player's left stick always steers as well (the cabinet lever).
            const bool up = actionDown(Input::Action_Up);
            const bool down = actionDown(Input::Action_Down);
            const bool left = actionDown(Input::Action_Left);
            const bool right = actionDown(Input::Action_Right);
            if (left && !right)
            {
                setButton(sl::BUTTON_LEFT);
                m_x1 = -1.0f;
            }
            else if (right && !left)
            {
                setButton(sl::BUTTON_RIGHT);
                m_x1 = 1.0f;
            }
            if (up && !down)
            {
                setButton(sl::BUTTON_UP);
                m_y1 = -1.0f;
            }
            else if (down && !up)
            {
                setButton(sl::BUTTON_DOWN);
                m_y1 = 1.0f;
            }

            const Input::PadState& pad = Input::GetPadState(gGeneral.GetSettings()->m_m2PadIndex[player]);
            if (m_x1 == 0.0f)
            {
                m_x1 = pad.x;
            }
            if (m_y1 == 0.0f)
            {
                m_y1 = pad.y;
            }

            m_push = ~m_prev & m_now;
            m_pull = m_prev & ~m_now;
        }
}
