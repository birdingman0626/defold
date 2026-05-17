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

// Host-runnable unit tests for the OHOS input state machines extracted into
// platform_window_ohos_input.h. Covers:
//
//   * Per-finger touch slot allocation, BEGAN -> MOVED demotion, ENDED slot
//     drop, and the 1-deep pending-phase queue that surfaces sub-frame
//     DOWN+UP taps as two consecutive frames.
//   * Primary-finger mouse-emulation: position tracking on MOVE, left-button
//     state, sub-frame press/release queueing, primary handoff when the
//     first finger lifts before a second comes down.
//   * Synthetic KEY_ESC pending queue mirrors the mouse-button pattern.

#include <stdint.h>
#define JC_TEST_IMPLEMENTATION
#include <jc_test/jc_test.h>

#include "../platform_window_ohos_input.h"

using namespace dmPlatform;

// ---------- touch ----------

TEST(OhosInput, TouchBeganPromotesToMoved)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, /*id*/ 0, OHOS_INPUT_PHASE_BEGAN, 100, 200);

    WindowTouchData out[OHOS_MAX_TOUCH_POINTS];
    ASSERT_EQ(1u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(OHOS_INPUT_PHASE_BEGAN, out[0].m_Phase);
    ASSERT_EQ(0, out[0].m_Id);
    ASSERT_EQ(100, out[0].m_X);
    ASSERT_EQ(200, out[0].m_Y);

    // Next poll: same slot now reports MOVED, no new events queued.
    ASSERT_EQ(1u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(OHOS_INPUT_PHASE_MOVED, out[0].m_Phase);
}

TEST(OhosInput, TouchEndedDropsSlotAfterReport)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 10, 20);
    WindowTouchData out[OHOS_MAX_TOUCH_POINTS];
    OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS);

    // Finger moves a bit, then lifts.
    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_MOVED, 15, 25);
    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_ENDED, 15, 25);

    ASSERT_EQ(1u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(OHOS_INPUT_PHASE_ENDED, out[0].m_Phase);

    // Slot dropped after report.
    ASSERT_EQ(0u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
}

TEST(OhosInput, SubFrameTapQueuesEndedBehindBegan)
{
    // The case that motivated the whole pending-phase queue: DOWN and
    // UP for the same finger arrive between two polls. We need the
    // engine to see BEGAN on poll N and ENDED on poll N+1, even though
    // both events were pushed back-to-back.
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 50, 60);
    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_ENDED, 50, 60);

    WindowTouchData out[OHOS_MAX_TOUCH_POINTS];
    ASSERT_EQ(1u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(OHOS_INPUT_PHASE_BEGAN, out[0].m_Phase);

    ASSERT_EQ(1u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(OHOS_INPUT_PHASE_ENDED, out[0].m_Phase);

    ASSERT_EQ(0u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
}

TEST(OhosInput, TouchUpdatesPositionWithoutDuplicatingSlot)
{
    // Same finger id across DOWN/MOVE/MOVE/UP must occupy ONE slot so
    // gesture classifiers don't read it as a multi-finger gesture.
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 7, OHOS_INPUT_PHASE_BEGAN, 0, 0);
    OhosInputPushTouch(&s, 7, OHOS_INPUT_PHASE_MOVED, 1, 1);
    OhosInputPushTouch(&s, 7, OHOS_INPUT_PHASE_MOVED, 2, 2);
    ASSERT_EQ(1u, s.m_TouchCount);

    WindowTouchData out[OHOS_MAX_TOUCH_POINTS];
    ASSERT_EQ(1u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(7, out[0].m_Id);
    ASSERT_EQ(2, out[0].m_X);
    ASSERT_EQ(2, out[0].m_Y);
}

TEST(OhosInput, TwoFingersOccupyDistinctSlots)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 10, 10);
    OhosInputPushTouch(&s, 1, OHOS_INPUT_PHASE_BEGAN, 20, 20);
    ASSERT_EQ(2u, s.m_TouchCount);

    WindowTouchData out[OHOS_MAX_TOUCH_POINTS];
    ASSERT_EQ(2u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));

    // Lift the first finger; the second stays.
    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_ENDED, 10, 10);
    ASSERT_EQ(2u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(1u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(1, out[0].m_Id);
}

TEST(OhosInput, TouchCancelledDropsLikeEnded)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN,     5, 5);
    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_CANCELLED, 5, 5);

    WindowTouchData out[OHOS_MAX_TOUCH_POINTS];
    ASSERT_EQ(1u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(OHOS_INPUT_PHASE_BEGAN, out[0].m_Phase);
    ASSERT_EQ(1u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
    ASSERT_EQ(OHOS_INPUT_PHASE_CANCELLED, out[0].m_Phase);
    ASSERT_EQ(0u, OhosInputReadTouches(&s, out, OHOS_MAX_TOUCH_POINTS));
}

TEST(OhosInput, TouchMaxPointsCapsAllocation)
{
    OhosInputState s;
    OhosInputInit(&s);
    for (uint32_t i = 0; i < OHOS_MAX_TOUCH_POINTS + 5; ++i)
    {
        OhosInputPushTouch(&s, (int32_t)i, OHOS_INPUT_PHASE_BEGAN, 0, 0);
    }
    ASSERT_EQ(OHOS_MAX_TOUCH_POINTS, s.m_TouchCount);
}

// ---------- mouse emulation ----------

TEST(OhosInput, MouseLeftPressOnPrimaryDown)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 100, 200);
    ASSERT_EQ(0, s.m_PrimaryTouchId == -1 ? -1 : 0);  // primary is id 0
    ASSERT_EQ(1, OhosInputReadMouseButton(&s, 0));
    ASSERT_EQ(100, s.m_MouseX);
    ASSERT_EQ(200, s.m_MouseY);
}

TEST(OhosInput, MouseLeftReleaseObservableAfterUp)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 1, 1);
    // First poll observes the press.
    ASSERT_EQ(1, OhosInputReadMouseButton(&s, 0));

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_ENDED, 1, 1);
    // Press was already observed (dirty cleared), so UP applies
    // immediately rather than being queued.
    ASSERT_EQ(0, OhosInputReadMouseButton(&s, 0));
}

TEST(OhosInput, MouseLeftSubFrameTapQueuesRelease)
{
    // DOWN then UP arrive between two polls. Engine must see 0→1→0
    // across two consecutive frames, not just 0→0.
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 7, 8);
    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_ENDED, 7, 8);

    ASSERT_EQ(1, OhosInputReadMouseButton(&s, 0));
    ASSERT_EQ(0, OhosInputReadMouseButton(&s, 0));
}

TEST(OhosInput, MousePositionTracksPrimaryDuringMove)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 10, 10);
    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_MOVED, 50, 60);
    ASSERT_EQ(50, s.m_MouseX);
    ASSERT_EQ(60, s.m_MouseY);
}

TEST(OhosInput, SecondaryFingerDoesNotMovePrimaryMouse)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 100, 100);
    OhosInputPushTouch(&s, 1, OHOS_INPUT_PHASE_BEGAN, 500, 500);

    // Mouse stays where the primary finger is.
    ASSERT_EQ(100, s.m_MouseX);
    ASSERT_EQ(100, s.m_MouseY);
}

TEST(OhosInput, PrimaryHandoffAfterFirstLift)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 10, 10);
    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_ENDED, 10, 10);
    OhosInputReadMouseButton(&s, 0); // flush press
    OhosInputReadMouseButton(&s, 0); // flush release

    // After the first lift, the next BEGAN claims primary.
    OhosInputPushTouch(&s, 1, OHOS_INPUT_PHASE_BEGAN, 99, 88);
    ASSERT_EQ(1, s.m_PrimaryTouchId);
    ASSERT_EQ(99, s.m_MouseX);
    ASSERT_EQ(88, s.m_MouseY);
    ASSERT_EQ(1, OhosInputReadMouseButton(&s, 0));
}

TEST(OhosInput, MouseButtonOtherThanLeftAlwaysZero)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushTouch(&s, 0, OHOS_INPUT_PHASE_BEGAN, 0, 0);
    // Only button 0 (LEFT) reflects touch state.
    ASSERT_EQ(0, OhosInputReadMouseButton(&s, 1));
    ASSERT_EQ(0, OhosInputReadMouseButton(&s, 2));
}

// ---------- ESC key (back-gesture bridge) ----------

TEST(OhosInput, EscPressObservedOnce)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushKey(&s, 1, 1);
    ASSERT_EQ(1, OhosInputReadKey(&s, 1));

    OhosInputPushKey(&s, 1, 0);
    ASSERT_EQ(0, OhosInputReadKey(&s, 1));
}

TEST(OhosInput, EscSubFrameTapQueuesRelease)
{
    // Same race as the mouse button: ArkTS's onBackPress fires press
    // then release back-to-back in one JS callback; engine must see
    // both transitions across consecutive frames.
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushKey(&s, 1, 1);
    OhosInputPushKey(&s, 1, 0);

    ASSERT_EQ(1, OhosInputReadKey(&s, 1));
    ASSERT_EQ(0, OhosInputReadKey(&s, 1));
}

TEST(OhosInput, EscOnlyKeyAccepted)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushKey(&s, 2, 1);  // some other code
    ASSERT_EQ(0, OhosInputReadKey(&s, 2));
    ASSERT_EQ(0, s.m_KeyEscDown);
}

TEST(OhosInput, EscSteadyPressStaysOneAcrossPolls)
{
    OhosInputState s;
    OhosInputInit(&s);

    OhosInputPushKey(&s, 1, 1);
    ASSERT_EQ(1, OhosInputReadKey(&s, 1));
    ASSERT_EQ(1, OhosInputReadKey(&s, 1));
    ASSERT_EQ(1, OhosInputReadKey(&s, 1));
}

int main(int argc, char **argv)
{
    jc_test_init(&argc, argv);
    return jc_test_run_all();
}
