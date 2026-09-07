// The sketch grid scales the user's BASE step by whole decades so the cell
// stays legible at the current zoom, and the same number drives both the drawn
// lines and the snap lattice. Two invariants carry the whole design:
//
//   1. the step is base x 10^n for an integer n, so every drawn line is a
//      snap point and the lattice reads as a 1/10/100 of the unit; and
//   2. the resulting cell is at least kGridMinPx and under ten times that, at
//      every zoom and every unit.
#include "viewport/GridScale.h"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>

using materializr::gridStepForZoom;

namespace {
constexpr float kMinPx = 8.0f;

// Legitimate only if step is base x 10^n for a whole n — positive, zero or
// negative. Checked in log space because the ratio spans 40 decades.
bool isDecadeMultiple(float step, float base) {
    const double n = std::log10(static_cast<double>(step) / base);
    return std::abs(n - std::round(n)) < 1e-4;
}
} // namespace

// The reported symptom in each direction. Same base, same function, opposite
// ends: mm zoomed out gave a lattice too fine to see, feet zoomed in gave one
// cell wider than the viewport.
TEST(GridScale, RescuesBothAnInvisibleAndAnAbsentGrid) {
    // 1 mm base in a view ~12 m across: was 12192 lines, greyed to nothing.
    const float coarsened = gridStepForZoom(1.0f, 15.0f, kMinPx);
    EXPECT_GT(coarsened, 1.0f) << "must step UP to be visible";
    EXPECT_GE(coarsened / 15.0f, kMinPx);

    // 1 ft base in a view ~100 mm across: was one cell every 3200 px.
    const float refined = gridStepForZoom(304.8f, 0.095238f, kMinPx);
    EXPECT_LT(refined, 304.8f) << "must step DOWN to be present at all";
    EXPECT_GE(refined / 0.095238f, kMinPx);
    EXPECT_TRUE(isDecadeMultiple(refined, 304.8f)) << "refined = " << refined;
}

// The two invariants, swept rather than sampled: six bases across four orders
// of magnitude, against zooms spanning six.
TEST(GridScale, CellStaysLegibleAndTheStepStaysADecadeMultiple) {
    for (float base : {0.1f, 0.5f, 1.0f, 10.0f, 25.4f, 304.8f}) {
        for (float mmPerPx = 0.0001f; mmPerPx < 1000.0f; mmPerPx *= 1.7f) {
            const float step = gridStepForZoom(base, mmPerPx, kMinPx);
            const float cellPx = step / mmPerPx;
            EXPECT_GE(cellPx, kMinPx * 0.999f)
                << "cell too small: base=" << base << " mmPerPx=" << mmPerPx;
            EXPECT_LT(cellPx, kMinPx * 10.0f * 1.001f)
                << "cell too large: base=" << base << " mmPerPx=" << mmPerPx
                << " step=" << step;
            EXPECT_TRUE(isDecadeMultiple(step, base))
                << "not a decade multiple: base=" << base
                << " mmPerPx=" << mmPerPx << " step=" << step;
        }
    }
}

// A base already sized for the zoom is left exactly alone — no drift, and no
// cosmetic rescaling of a step the user deliberately chose.
TEST(GridScale, LeavesAWellSizedBaseUntouched) {
    // cell = 1.0 / 0.05 = 20 px, inside [8, 80).
    EXPECT_FLOAT_EQ(1.0f, gridStepForZoom(1.0f, 0.05f, kMinPx));
    // Exactly at the threshold counts as legible: cell = 8 px.
    EXPECT_FLOAT_EQ(8.0f, gridStepForZoom(8.0f, 1.0f, kMinPx));
}

// ceil, not round. A cell 3x under the floor is the illegible case the whole
// function exists to prevent, and round() would happily return it.
TEST(GridScale, PicksTheSmallestDecadeThatClearsTheFloor) {
    // wants 4 mm; one decade up is 10 (cell 20 px), zero decades is 1 (5 px).
    EXPECT_FLOAT_EQ(10.0f, gridStepForZoom(1.0f, 0.5f, kMinPx));
    // wants 80 mm -> two decades, not one (10 mm would be 1 px).
    EXPECT_FLOAT_EQ(100.0f, gridStepForZoom(1.0f, 10.0f, kMinPx));
    // Stepping DOWN obeys the same rule: 0.1 would be 10x smaller than needed.
    EXPECT_FLOAT_EQ(1.0f, gridStepForZoom(1000.0f, 0.05f, kMinPx));
}

// Garbage in is handed straight back rather than coerced into a lattice that
// looks plausible. The step both divides (renderer) and modulos (snapping), so
// a zero or NaN is not a cosmetic problem.
TEST(GridScale, RejectsNonFiniteAndNonPositiveInputs) {
    const float nan = std::nanf("");
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FLOAT_EQ(1.0f, gridStepForZoom(1.0f, nan, kMinPx));
    EXPECT_FLOAT_EQ(1.0f, gridStepForZoom(1.0f, inf, kMinPx));
    EXPECT_FLOAT_EQ(1.0f, gridStepForZoom(1.0f, 0.0f, kMinPx));
    EXPECT_FLOAT_EQ(1.0f, gridStepForZoom(1.0f, -3.0f, kMinPx));
    EXPECT_FLOAT_EQ(1.0f, gridStepForZoom(1.0f, 15.0f, 0.0f));
    EXPECT_TRUE(std::isnan(gridStepForZoom(nan, 15.0f, kMinPx)));
    EXPECT_FLOAT_EQ(0.0f, gridStepForZoom(0.0f, 15.0f, kMinPx));
}

// Extreme zooms must stay finite and positive at both ends — the step is
// divided by AND modulo'd by, so neither an infinity nor a zero may escape.
TEST(GridScale, ExtremeZoomsStayFiniteAndPositive) {
    for (float mmPerPx : {1.0e-30f, 1.0e-10f, 1.0e10f, 1.0e30f, 1.0e37f}) {
        const float step = gridStepForZoom(1.0f, mmPerPx, kMinPx);
        EXPECT_TRUE(std::isfinite(step)) << "mmPerPx=" << mmPerPx;
        EXPECT_GT(step, 0.0f) << "mmPerPx=" << mmPerPx;
    }
    // The one case that cannot be served: the legible step would not fit in a
    // float, so the base is returned rather than an infinity.
    EXPECT_FLOAT_EQ(1.0f, gridStepForZoom(1.0f, 1.0e38f, kMinPx));
}

// The opening view of an EMPTY sketch. Framing a fixed count of DISPLAY units
// keeps the numbers on screen sensible, but 40 of a large unit is enormous: at
// 40 ft the view is twelve metres, so a shape drawn at screen centre lands
// metres from the plane origin and hangs above the ground grid on exit.
TEST(GridScale, OpeningSketchViewStaysHumanScaleInEveryUnit) {
    constexpr float kMin = 20.0f, kCap = 1000.0f;
    struct Case { const char* unit; float unitSpanMm; };
    // 40 display units expressed in millimetres, per unit.
    const Case cases[] = {
        {"mm", 40.0f}, {"cm", 400.0f}, {"m", 40000.0f},
        {"in", 1016.0f}, {"ft", 12192.0f},
    };
    for (const Case& cs : cases) {
        const float span = materializr::openingSketchSpanMm(
            cs.unitSpanMm, /*baseStepMm=*/1.0f, kMin, kCap);
        EXPECT_GE(span, kMin) << cs.unit;
        EXPECT_LE(span, kCap) << cs.unit << ": a first view must stay human scale";
    }

    // Millimetres are untouched — the whole point is that the common case does
    // not move. 40 mm in, 40 mm out.
    EXPECT_FLOAT_EQ(40.0f,
        materializr::openingSketchSpanMm(40.0f, 1.0f, kMin, kCap));

    // Feet: was 12192 mm (twelve metres), now the cap.
    EXPECT_FLOAT_EQ(kCap,
        materializr::openingSketchSpanMm(12192.0f, 1.0f, kMin, kCap));
}

// The grid term has to be INSIDE the bound. A 1 ft base makes baseStep*40 =
// 12192 mm by itself, so bounding only the unit span would have left the
// reported bug exactly where it was.
TEST(GridScale, TheGridTermIsBoundedToo) {
    constexpr float kMin = 20.0f, kCap = 1000.0f;
    // Millimetre unit span, but a one-foot base step.
    EXPECT_FLOAT_EQ(kCap,
        materializr::openingSketchSpanMm(40.0f, /*baseStepMm=*/304.8f, kMin, kCap));
    // A coarse-but-reasonable base still widens the view, below the cap.
    EXPECT_FLOAT_EQ(400.0f,
        materializr::openingSketchSpanMm(40.0f, /*baseStepMm=*/10.0f, kMin, kCap));
}

// The floor still applies: a tiny unit span must not open a sub-millimetre view.
TEST(GridScale, OpeningViewKeepsItsFloor) {
    EXPECT_FLOAT_EQ(20.0f,
        materializr::openingSketchSpanMm(1.0f, 0.1f, 20.0f, 1000.0f));
}
