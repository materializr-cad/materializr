// The sketch grid draws a lattice that may be coarser than the snap step.
// The invariant under test: it is NEVER finer, and it is always a whole-decade
// multiple, so every line drawn is a place the cursor can actually land.
#include "viewport/GridScale.h"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>

using materializr::gridDrawStep;

namespace {
constexpr float kMinPx = 8.0f;
// A drawn step is only legitimate if it is step x 10^n for a whole n >= 0.
bool isDecadeMultiple(float drawn, float step) {
    if (drawn < step * 0.999f) return false;
    const double n = std::log10(static_cast<double>(drawn) / step);
    return std::abs(n - std::round(n)) < 1e-4;
}
} // namespace

// The reported symptom: switch feet -> mm and the grid vanishes. The step
// became 1 mm while the camera still framed roughly 40 ft, so a cell was a
// fraction of a pixel.
TEST(GridScale, ZoomedOutMillimetreGridCoarsensUntilVisible) {
    const float mmPerPx = 15.0f;                  // ~12 m across an 800 px view
    const float drawn = gridDrawStep(1.0f, mmPerPx, kMinPx);
    EXPECT_GE(drawn / mmPerPx, kMinPx) << "a cell must be at least kGridMinPx wide";
    EXPECT_TRUE(isDecadeMultiple(drawn, 1.0f)) << "drawn = " << drawn;
}

// The rule that makes coarsening safe. Swept across four decades of zoom and
// a range of steps rather than checked at the one value that motivated it.
TEST(GridScale, NeverFinerThanTheSnapStepAndAlwaysADecadeMultiple) {
    for (float step : {0.1f, 0.5f, 1.0f, 10.0f, 25.4f, 304.8f}) {
        for (float mmPerPx = 0.001f; mmPerPx < 100.0f; mmPerPx *= 1.7f) {
            const float drawn = gridDrawStep(step, mmPerPx, kMinPx);
            EXPECT_GE(drawn, step * 0.999f)
                << "finer than the snap step: step=" << step << " mmPerPx=" << mmPerPx;
            EXPECT_TRUE(isDecadeMultiple(drawn, step))
                << "not a decade multiple: step=" << step
                << " mmPerPx=" << mmPerPx << " drawn=" << drawn;
        }
    }
}

// Zooming IN must not subdivide. At that zoom the lattice really is that
// coarse, and inventing intermediate lines would invent snap points.
TEST(GridScale, ZoomedInLeavesTheStepAlone) {
    EXPECT_FLOAT_EQ(1.0f, gridDrawStep(1.0f, 0.01f, kMinPx));
    EXPECT_FLOAT_EQ(304.8f, gridDrawStep(304.8f, 0.5f, kMinPx));
    // Exactly at the threshold is already legible — no step-up.
    EXPECT_FLOAT_EQ(8.0f, gridDrawStep(8.0f, 1.0f, kMinPx));
}

// A one-decade step-up is enough whenever the shortfall is under 10x, and it
// does not overshoot to two. This is what pins ceil() as the right rounding:
// floor() would return a still-invisible lattice.
TEST(GridScale, StepsUpByTheSmallestSufficientDecade) {
    // 1 mm step, cell wants 8 * 0.5 = 4 mm -> one decade (10 mm), not two.
    EXPECT_FLOAT_EQ(10.0f, gridDrawStep(1.0f, 0.5f, kMinPx));
    // Wants 80 mm -> two decades (100 mm).
    EXPECT_FLOAT_EQ(100.0f, gridDrawStep(1.0f, 10.0f, kMinPx));
}

// Garbage in is handed straight back rather than coerced into a lattice that
// looks plausible. NaN matters: it reaches a float-to-int conversion downstream.
TEST(GridScale, RejectsNonFiniteAndNonPositiveInputs) {
    const float nan = std::nanf("");
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FLOAT_EQ(1.0f, gridDrawStep(1.0f, nan, kMinPx));
    EXPECT_FLOAT_EQ(1.0f, gridDrawStep(1.0f, inf, kMinPx));
    EXPECT_FLOAT_EQ(1.0f, gridDrawStep(1.0f, 0.0f, kMinPx));
    EXPECT_FLOAT_EQ(1.0f, gridDrawStep(1.0f, -3.0f, kMinPx));
    EXPECT_FLOAT_EQ(1.0f, gridDrawStep(1.0f, 15.0f, 0.0f));
    EXPECT_TRUE(std::isnan(gridDrawStep(nan, 15.0f, kMinPx)));
    EXPECT_FLOAT_EQ(0.0f, gridDrawStep(0.0f, 15.0f, kMinPx));
}

// An absurd zoom-out must not overflow the float or hand the shader a scale
// of zero — the renderer DIVIDES by the step. Three separate overflow points,
// each reached by a different input, because guarding only the arguments
// leaves the two intermediate products unguarded.
TEST(GridScale, ExtremeZoomStaysFiniteAndPositive) {
    // 1. Ordinary huge zoom: the arithmetic all stays in range.
    for (float mmPerPx : {1.0e6f, 1.0e18f, 1.0e30f, 1.0e37f}) {
        const float drawn = gridDrawStep(0.1f, mmPerPx, kMinPx);
        EXPECT_TRUE(std::isfinite(drawn)) << "mmPerPx=" << mmPerPx;
        EXPECT_GT(drawn, 0.0f) << "mmPerPx=" << mmPerPx;
        EXPECT_TRUE(isDecadeMultiple(drawn, 0.1f)) << "mmPerPx=" << mmPerPx;
    }
    // 2. A step so tiny it needs forty decades. minPx * mmPerPx and the ratio
    //    both leave float range here; the computation is done in double so
    //    they do not overflow, and the answer is an ordinary decade multiple.
    const float tiny = gridDrawStep(1.0e-30f, 1.0e9f, kMinPx);
    EXPECT_TRUE(std::isfinite(tiny));
    EXPECT_TRUE(isDecadeMultiple(tiny, 1.0e-30f)) << "tiny = " << tiny;
    EXPECT_GE(tiny / 1.0e9f, kMinPx);
    // 3. The one case that genuinely cannot be served: the visible lattice
    //    would not fit in a float. The renderer divides by the step, so an
    //    infinite one is refused and the snap lattice is drawn unchanged.
    EXPECT_FLOAT_EQ(1.0f, gridDrawStep(1.0f, 1.0e38f, kMinPx));
}
