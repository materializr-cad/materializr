// A stepper nudge must never move the value the wrong way.
//
// Steve, 2026-08-03: "on extrude, if i click the + or 1 numbers when something
// is past 50mm, it just goes to 50mm". The steppers clamp to [minV, maxV], and
// the extrude row is ±50 - so a distance typed as 80 in the text field beside
// them collapsed to 50 the moment you pressed +1. The clamp reversed the nudge
// and threw away what was typed.
//
// The bounds are there to keep the BUTTONS sane, not to police the value: the
// text field accepts anything. So a bound only stops a nudge that would leave
// the range; it never drags a value that is already outside back in.
//
// This covers steppedValue directly - the button row itself needs an ImGui
// context, but the rule doesn't.

#include "ui/StepperRow.h"

#include <gtest/gtest.h>

using materializr::steppedValue;

// The reported case, with the extrude row's actual bounds.
TEST(StepperNudge, PlusFromAboveMaxDoesNotCollapse) {
    EXPECT_FLOAT_EQ(steppedValue(80.0f, 1.0f, -50.0f, 50.0f), 81.0f);
    EXPECT_FLOAT_EQ(steppedValue(80.0f, 10.0f, -50.0f, 50.0f), 90.0f);
    EXPECT_FLOAT_EQ(steppedValue(80.0f, 0.1f, -50.0f, 50.0f), 80.1f);
}

// Nudging back toward the range from outside it works normally.
TEST(StepperNudge, MinusFromAboveMaxMovesDown) {
    EXPECT_FLOAT_EQ(steppedValue(80.0f, -1.0f, -50.0f, 50.0f), 79.0f);
    // ...and does not stop early at the bound it is heading toward.
    EXPECT_FLOAT_EQ(steppedValue(55.0f, -10.0f, -50.0f, 50.0f), 45.0f);
}

// The mirror image, below the minimum.
TEST(StepperNudge, MinusFromBelowMinDoesNotCollapse) {
    EXPECT_FLOAT_EQ(steppedValue(-80.0f, -1.0f, -50.0f, 50.0f), -81.0f);
    EXPECT_FLOAT_EQ(steppedValue(-80.0f, 1.0f, -50.0f, 50.0f), -79.0f);
}

// From INSIDE the range the bound still holds - that is what it is for.
TEST(StepperNudge, ClampStillAppliesFromInside) {
    EXPECT_FLOAT_EQ(steppedValue(45.0f, 10.0f, -50.0f, 50.0f), 50.0f);
    EXPECT_FLOAT_EQ(steppedValue(-45.0f, -10.0f, -50.0f, 50.0f), -50.0f);
    EXPECT_FLOAT_EQ(steppedValue(49.9f, 1.0f, -50.0f, 50.0f), 50.0f);
}

// Exactly on the bound: pressing further out stops, coming back works.
TEST(StepperNudge, OnTheBound) {
    EXPECT_FLOAT_EQ(steppedValue(50.0f, 1.0f, -50.0f, 50.0f), 50.0f);
    EXPECT_FLOAT_EQ(steppedValue(50.0f, -1.0f, -50.0f, 50.0f), 49.0f);
}

// Positive-only rows (fillet, chamfer, depth) behave the same way.
TEST(StepperNudge, PositiveOnlyRange) {
    EXPECT_FLOAT_EQ(steppedValue(0.5f, -1.0f, 0.1f, 20.0f), 0.1f);   // clamped at min
    EXPECT_FLOAT_EQ(steppedValue(30.0f, 1.0f, 0.1f, 20.0f), 31.0f);  // already above max
    EXPECT_FLOAT_EQ(steppedValue(30.0f, -1.0f, 0.1f, 20.0f), 29.0f); // heading back
}
