// Copyright 2020-2026 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

// OHOS input state machine, extracted from platform_window_ohos.cpp so it can
// be unit-tested without dragging in the EGL / XComponent / NativeWindow
// dependencies the rest of the file needs. Header-only by design — the
// production code and the host test both include it; nothing else has to be
// wired into a wscript.
//
// What this file owns:
//   1. Per-finger touch state with a 1-deep pending-phase queue. Needed
//      because the OHOS x86_64 emulator can complete a sub-frame tap
//      (DOWN+UP within one engine frame); without the queue, GetTouchData
//      would only ever observe the final phase and the input layer would
//      never see a 0→1→0 value transition.
//   2. Synthetic MOUSE_BUTTON_LEFT state driven by the primary finger so
//      Defold's mouse_trigger MOUSE_BUTTON_LEFT → "advance" mapping fires
//      on tap. Same pending-queue pattern handles sub-frame press/release.
//   3. Synthetic KEY_ESC state used to forward the OHOS system back-gesture
//      into the engine's key_trigger KEY_ESC → "menu" mapping.

#ifndef DM_PLATFORM_WINDOW_OHOS_INPUT_H
#define DM_PLATFORM_WINDOW_OHOS_INPUT_H

#include <stdint.h>
#include <string.h>
#include "window.h" // WindowTouchData

namespace dmPlatform
{
    static const uint32_t OHOS_MAX_TOUCH_POINTS = 10;

    // dmHID::Phase: BEGAN=0, MOVED=1, STATIONARY=2, ENDED=3, CANCELLED=4.
    // We keep the numeric constants here to avoid pulling in the dmHID
    // header from the platform layer.
    enum
    {
        OHOS_INPUT_PHASE_BEGAN     = 0,
        OHOS_INPUT_PHASE_MOVED     = 1,
        OHOS_INPUT_PHASE_ENDED     = 3,
        OHOS_INPUT_PHASE_CANCELLED = 4,
    };

    struct OhosInputState
    {
        // Touch slots. m_TouchPending[i] is -1 if no queued phase, else the
        // phase value to advance slot i to on the next ReadTouches().
        WindowTouchData m_TouchData[OHOS_MAX_TOUCH_POINTS];
        int32_t         m_TouchPending[OHOS_MAX_TOUCH_POINTS];
        uint32_t        m_TouchCount;

        // Mouse emulation from the primary finger.
        int32_t  m_MouseX;
        int32_t  m_MouseY;
        uint8_t  m_MouseButtonLeft;
        uint8_t  m_MouseButtonLeftDirty;
        int32_t  m_MouseButtonPending;
        int32_t  m_PrimaryTouchId;

        // Synthetic KEY_ESC state.
        uint8_t  m_KeyEscDown;
        uint8_t  m_KeyEscDirty;
        int32_t  m_KeyEscPending;
    };

    static inline void OhosInputInit(OhosInputState* s)
    {
        memset(s, 0, sizeof(*s));
        for (uint32_t i = 0; i < OHOS_MAX_TOUCH_POINTS; ++i)
            s->m_TouchPending[i] = -1;
        s->m_MouseButtonPending = -1;
        s->m_PrimaryTouchId     = -1;
        s->m_KeyEscPending      = -1;
    }

    static inline void OhosInputPushTouch(OhosInputState* s, int32_t id, int32_t phase, int32_t x, int32_t y)
    {
        bool is_down = (phase == OHOS_INPUT_PHASE_BEGAN);
        bool is_up   = (phase == OHOS_INPUT_PHASE_ENDED || phase == OHOS_INPUT_PHASE_CANCELLED);

        // Find existing slot for this finger or allocate a new one.
        int slot = -1;
        for (uint32_t i = 0; i < s->m_TouchCount; ++i)
        {
            if (s->m_TouchData[i].m_Id == id) { slot = (int)i; break; }
        }
        if (slot < 0)
        {
            if (s->m_TouchCount >= OHOS_MAX_TOUCH_POINTS) return;
            slot = (int)s->m_TouchCount++;
            WindowTouchData& nt = s->m_TouchData[slot];
            nt.m_Id    = id;
            nt.m_Phase = phase;
            nt.m_X     = x;
            nt.m_Y     = y;
            s->m_TouchPending[slot] = -1;
        }
        else
        {
            WindowTouchData& t = s->m_TouchData[slot];
            t.m_X = x;
            t.m_Y = y;
            if (t.m_Phase == OHOS_INPUT_PHASE_BEGAN && is_up)
            {
                // BEGAN unread; queue the UP so ReadTouches surfaces both
                // phases across two consecutive frames.
                s->m_TouchPending[slot] = phase;
            }
            else
            {
                t.m_Phase = phase;
            }
        }

        // Mouse emulation: primary = first finger down while no other primary.
        if (is_down && s->m_PrimaryTouchId < 0)
        {
            s->m_PrimaryTouchId = id;
        }
        if (s->m_PrimaryTouchId == id)
        {
            s->m_MouseX = x;
            s->m_MouseY = y;
            if (is_down)
            {
                s->m_MouseButtonLeft      = 1;
                s->m_MouseButtonLeftDirty = 1;
                s->m_MouseButtonPending   = -1;
            }
            else if (is_up)
            {
                if (s->m_MouseButtonLeftDirty && s->m_MouseButtonLeft == 1)
                {
                    s->m_MouseButtonPending = 0;
                }
                else
                {
                    s->m_MouseButtonLeft      = 0;
                    s->m_MouseButtonLeftDirty = 1;
                    s->m_MouseButtonPending   = -1;
                }
                s->m_PrimaryTouchId = -1;
            }
        }
    }

    static inline void OhosInputPushKey(OhosInputState* s, int32_t code, int32_t pressed)
    {
        if (code != 1 /* PLATFORM_KEY_ESC */) return;
        if (pressed)
        {
            s->m_KeyEscDown    = 1;
            s->m_KeyEscDirty   = 1;
            s->m_KeyEscPending = -1;
        }
        else
        {
            if (s->m_KeyEscDirty && s->m_KeyEscDown == 1)
            {
                s->m_KeyEscPending = 0;
            }
            else
            {
                s->m_KeyEscDown    = 0;
                s->m_KeyEscDirty   = 1;
                s->m_KeyEscPending = -1;
            }
        }
    }

    static inline uint32_t OhosInputReadTouches(OhosInputState* s, WindowTouchData* out, uint32_t out_count)
    {
        if (!out) return 0;
        uint32_t n = s->m_TouchCount < out_count ? s->m_TouchCount : out_count;
        for (uint32_t i = 0; i < n; ++i)
        {
            out[i] = s->m_TouchData[i];
        }

        // Advance phases:
        //   pending set   -> apply it
        //   else BEGAN    -> demote to MOVED
        //   else ENDED/CC -> drop the slot
        uint32_t write = 0;
        for (uint32_t i = 0; i < s->m_TouchCount; ++i)
        {
            WindowTouchData& t = s->m_TouchData[i];
            int32_t&         p = s->m_TouchPending[i];
            if (t.m_Phase == OHOS_INPUT_PHASE_ENDED || t.m_Phase == OHOS_INPUT_PHASE_CANCELLED)
                continue;
            if (p >= 0)
            {
                t.m_Phase = p;
                p = -1;
            }
            else if (t.m_Phase == OHOS_INPUT_PHASE_BEGAN)
            {
                t.m_Phase = OHOS_INPUT_PHASE_MOVED;
            }
            if (write != i)
            {
                s->m_TouchData[write]    = t;
                s->m_TouchPending[write] = p;
            }
            ++write;
        }
        s->m_TouchCount = write;
        return n;
    }

    static inline int32_t OhosInputReadMouseButton(OhosInputState* s, int32_t button)
    {
        if (button != 0) return 0; // only LEFT
        int32_t value = (int32_t)s->m_MouseButtonLeft;
        s->m_MouseButtonLeftDirty = 0;
        if (s->m_MouseButtonPending >= 0)
        {
            s->m_MouseButtonLeft       = (uint8_t)s->m_MouseButtonPending;
            s->m_MouseButtonPending    = -1;
            s->m_MouseButtonLeftDirty  = 1;
        }
        return value;
    }

    static inline int32_t OhosInputReadKey(OhosInputState* s, int32_t code)
    {
        if (code != 1 /* PLATFORM_KEY_ESC */) return 0;
        int32_t value = (int32_t)s->m_KeyEscDown;
        s->m_KeyEscDirty = 0;
        if (s->m_KeyEscPending >= 0)
        {
            s->m_KeyEscDown    = (uint8_t)s->m_KeyEscPending;
            s->m_KeyEscPending = -1;
            s->m_KeyEscDirty   = 1;
        }
        return value;
    }
}

#endif // DM_PLATFORM_WINDOW_OHOS_INPUT_H
