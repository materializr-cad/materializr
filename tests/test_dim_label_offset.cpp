// Dimension label placement offsets.
//
// Reported symptom: "I can't move these stupid dimension tags to a better
// location, each click just opens the edit." Labels stored an offset only at
// creation and nothing ever rewrote it, so a press went straight to the value
// editor. Dragging now rewrites Constraint::labelOffX/Y through dimLabelOffset.
//
// The trap this pins down: (0,0) is ALSO the sentinel for "never placed", which
// the renderer reads as "use the automatic position". A tag dropped exactly on
// its anchor must not silently snap back to auto placement.

#include "modeling/SketchConstraints.h"

#include <gtest/gtest.h>

#include <cmath>

// A press that has not moved is NOT a drag, and the caller must therefore store
// no offset for it. This is the guard against a click silently converting an
// automatically positioned label into a fixed one: the stored offset doubles as
// the "user placed this" flag, so writing the auto offset on a plain click
// gives the label a leader line and freezes it out of automatic placement.
TEST(DimDragThreshold, AStationaryPressIsNotADrag) {
    EXPECT_FALSE(materializr::dimDragExceedsThreshold(0.0f, 0.0f));
}

// A shaky hand is still a click. Anything inside the threshold circle, in any
// direction, must stay a click so tapping a tag reliably edits it.
TEST(DimDragThreshold, SmallJitterStaysAClick) {
    EXPECT_FALSE(materializr::dimDragExceedsThreshold(1.0f, 0.0f));
    EXPECT_FALSE(materializr::dimDragExceedsThreshold(0.0f, -2.0f));
    EXPECT_FALSE(materializr::dimDragExceedsThreshold(-2.0f, 2.0f));
    // Exactly on the boundary is still a click — the test is strictly greater.
    EXPECT_FALSE(materializr::dimDragExceedsThreshold(
        materializr::kDimDragThresholdPx, 0.0f));
}

// Deliberate movement past the threshold is a drag, in any direction.
TEST(DimDragThreshold, DeliberateMovementIsADrag) {
    EXPECT_TRUE(materializr::dimDragExceedsThreshold(4.0f, 0.0f));
    EXPECT_TRUE(materializr::dimDragExceedsThreshold(0.0f, -10.0f));
    EXPECT_TRUE(materializr::dimDragExceedsThreshold(-3.0f, -3.0f));
}

TEST(DimLabelOffset, IsRelativeToTheAnchor) {
    double x = 0, y = 0;
    // Anchor at (10,5), dropped at (13,9) -> the tag sits +3,+4 from its
    // anchor, so it keeps that relationship when the solver later moves the
    // geometry underneath it.
    materializr::dimLabelOffset(13.0, 9.0, 10.0, 5.0, x, y);
    EXPECT_DOUBLE_EQ(3.0, x);
    EXPECT_DOUBLE_EQ(4.0, y);
}

TEST(DimLabelOffset, NegativeOffsetsSurvive) {
    double x = 0, y = 0;
    materializr::dimLabelOffset(2.0, 1.0, 10.0, 5.0, x, y);
    EXPECT_DOUBLE_EQ(-8.0, x);
    EXPECT_DOUBLE_EQ(-4.0, y);
}

// Dropping a label exactly on its anchor produces (0,0) — the "never placed"
// sentinel. Storing that verbatim would make the label jump to its automatic
// position, reading as "the drag was ignored".
TEST(DimLabelOffset, ExactAnchorDropDoesNotBecomeTheUnplacedSentinel) {
    double x = 0, y = 0;
    materializr::dimLabelOffset(7.5, -2.25, 7.5, -2.25, x, y);
    EXPECT_FALSE(x == 0.0 && y == 0.0)
        << "offset collapsed to the unplaced sentinel; the label would revert "
           "to automatic placement";
    // The nudge must be far below anything visible — a tenth of a micron.
    EXPECT_LT(std::abs(x), 1e-3);
    EXPECT_LT(std::abs(y), 1e-3);
}

// One axis landing on zero is ordinary and must be preserved exactly: a label
// dragged straight up from its anchor has no horizontal offset.
TEST(DimLabelOffset, SingleZeroAxisIsLeftAlone) {
    double x = 0, y = 0;
    materializr::dimLabelOffset(10.0, 12.0, 10.0, 5.0, x, y);
    EXPECT_DOUBLE_EQ(0.0, x);
    EXPECT_DOUBLE_EQ(7.0, y);
}
