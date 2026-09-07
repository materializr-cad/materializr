// The transactions behind every length widget, exercised without ImGui.
//
// These are the "dangerous workflows" the adversarial review said a plain
// conversion test could not catch: diameter-halving order, slider bounds,
// drag quantisation, buffer reseeding while a field is active. Each is a pure
// function in core/LengthEdit.h precisely so it can be pinned here.

#include "core/LengthEdit.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

using materializr::DimKind;
using materializr::LengthUnit;

namespace {
struct ScopedUnit {
    LengthUnit saved;
    explicit ScopedUnit(LengthUnit u) : saved(materializr::currentUnit()) { materializr::setCurrentUnit(u); }
    ~ScopedUnit() { materializr::setCurrentUnit(saved); }
};
} // namespace

// 10. The one write-back path. Under inches, a field showing 1.0 stores 25.4 mm.
// Mutation: swap toMm/toDisplay -> 0.03937 stored instead.
TEST(LengthEdit, LengthFieldCommitWritesMm) {
    { ScopedUnit s(LengthUnit::In); EXPECT_DOUBLE_EQ(25.4,  materializr::lengthFieldCommit(1.0)); }
    { ScopedUnit s(LengthUnit::Ft); EXPECT_DOUBLE_EQ(304.8, materializr::lengthFieldCommit(1.0)); }
    { ScopedUnit s(LengthUnit::Mm); EXPECT_DOUBLE_EQ(7.5,   materializr::lengthFieldCommit(7.5)); }
}

// 11. Convert, THEN halve. A circle's Radius constraint is typed as a diameter:
// "2" under inches is a 2 in diameter = 50.8 mm, stored as a 25.4 mm radius.
// Mutation: halve then convert gives the same number here — so the arc case
// and the angle case pin the order: an arc must NOT be halved, an angle must
// NOT be converted.
TEST(LengthEdit, RadiusEditConvertsThenHalves) {
    ScopedUnit s(LengthUnit::In);
    double v = 0.0;
    ASSERT_TRUE(materializr::applyDimensionEdit(DimKind::Radius, /*isArc=*/false, "2", v));
    EXPECT_DOUBLE_EQ(25.4, v) << "circle: 2 in diameter -> 25.4 mm radius";
    ASSERT_TRUE(materializr::applyDimensionEdit(DimKind::Radius, /*isArc=*/true, "2", v));
    EXPECT_DOUBLE_EQ(50.8, v) << "arc: typed AS radius, not halved";
    ASSERT_TRUE(materializr::applyDimensionEdit(DimKind::Length, false, "2", v));
    EXPECT_DOUBLE_EQ(50.8, v);
    // A typed suffix beats the current unit, and still halves afterwards.
    ASSERT_TRUE(materializr::applyDimensionEdit(DimKind::Radius, false, "10mm", v));
    EXPECT_DOUBLE_EQ(5.0, v);
}

TEST(LengthEdit, AngleEditIgnoresTheUnit) {
    ScopedUnit s(LengthUnit::Ft);   // the most aggressive factor
    double v = 0.0;
    ASSERT_TRUE(materializr::applyDimensionEdit(DimKind::Angle, false, "90", v));
    EXPECT_NEAR(M_PI / 2.0, v, 1e-12);
    EXPECT_FALSE(materializr::applyDimensionEdit(DimKind::Angle, false, "90in", v))
        << "an angle carrying a length suffix is garbage, not 90 * 304.8";
}

TEST(LengthEdit, DimensionEditRefusesBadInput) {
    ScopedUnit s(LengthUnit::Mm);
    double v = 42.0;
    EXPECT_FALSE(materializr::applyDimensionEdit(DimKind::Length, false, "abc", v));
    EXPECT_FALSE(materializr::applyDimensionEdit(DimKind::Length, false, "-5",  v)) << "lengths are positive";
    EXPECT_FALSE(materializr::applyDimensionEdit(DimKind::Length, false, "0",   v));
    EXPECT_FALSE(materializr::applyDimensionEdit(DimKind::Radius, false, "",    v));
    EXPECT_DOUBLE_EQ(42.0, v) << "a refusal leaves the value alone";
}

// Seeding mirrors the commit: a circle shows its diameter, an arc its radius,
// an angle its degrees — all in the current unit except the angle.
TEST(LengthEdit, SeedDimensionTextMirrorsApply) {
    ScopedUnit s(LengthUnit::In);
    char b[32];
    ASSERT_TRUE(materializr::seedDimensionText(b, sizeof b, DimKind::Radius, false, 25.4));
    EXPECT_STREQ("2.000", b) << "circle radius 25.4 mm seeds as 2 in DIAMETER";
    ASSERT_TRUE(materializr::seedDimensionText(b, sizeof b, DimKind::Radius, true, 25.4));
    EXPECT_STREQ("1.000", b);
    ASSERT_TRUE(materializr::seedDimensionText(b, sizeof b, DimKind::Length, false, 50.8));
    EXPECT_STREQ("2.000", b);
    ASSERT_TRUE(materializr::seedDimensionText(b, sizeof b, DimKind::Angle, false, M_PI / 4.0));
    EXPECT_STREQ("45.00", b);
    // Round trip: what was seeded, committed unchanged, stores the same value.
    double v = 0.0;
    materializr::seedDimensionText(b, sizeof b, DimKind::Radius, false, 25.4);
    ASSERT_TRUE(materializr::applyDimensionEdit(DimKind::Radius, false, b, v));
    EXPECT_NEAR(25.4, v, 1e-9);
}

// 12. Slider value and bounds move together; the far end writes back exactly.
TEST(LengthEdit, SliderBoundsConvertTogether) {
    ScopedUnit s(LengthUnit::In);
    const auto sh = materializr::sliderShadow(50.8, 1.0, 100.0);
    EXPECT_NEAR(2.0,      sh.value, 1e-12);
    EXPECT_NEAR(1 / 25.4, sh.lo,    1e-12);
    EXPECT_NEAR(100 / 25.4, sh.hi,  1e-12);
    EXPECT_NEAR(100.0, materializr::lengthFieldCommit(sh.hi), 1e-9) << "top of the slider is still 100 mm";
}

// 13. Drags snap in the DISPLAY unit's step, not to 0.1 mm.
TEST(LengthEdit, DragQuantisesInDisplayUnit) {
    { ScopedUnit s(LengthUnit::In); EXPECT_NEAR(25.654, materializr::quantiseDragMm(25.7), 1e-9) << "in dragStep 0.01 in = 0.254 mm"; }
    { ScopedUnit s(LengthUnit::Mm); EXPECT_NEAR(25.7, materializr::quantiseDragMm(25.7), 1e-9) << "mm dragStep 0.1 — the snap upstream always had"; }
    { ScopedUnit s(LengthUnit::Cm); EXPECT_NEAR(25.7, materializr::quantiseDragMm(25.7), 1e-9) << "cm dragStep 0.01 cm = 0.1 mm"; }
}

// 14. The buffer follows the model unless THIS field is being edited.
TEST(LengthEdit, BufferReseedsUnlessActive) {
    ScopedUnit s(LengthUnit::In);
    char b[32] = "half-typ";
    EXPECT_FALSE(materializr::reseedBuffer(b, sizeof b, 25.4, /*active=*/true));
    EXPECT_STREQ("half-typ", b) << "an active edit is never clobbered";
    EXPECT_TRUE(materializr::reseedBuffer(b, sizeof b, 25.4, /*active=*/false));
    EXPECT_STREQ("1.000", b);
    char tiny[3];
    EXPECT_FALSE(materializr::reseedBuffer(tiny, sizeof tiny, 25.4, false)) << "too small: refused, not truncated";
}

// The stepper magnitudes must mean what their labels say. stepperRow added its
// literal 10/1/0.1 to a millimetre member, so beside a field reading "in" the
// button labelled +1 moved the value by 1 mm (0.039 in) and the mm bounds
// shrank the usable range by the unit factor.
TEST(LengthEdit, StepperMagnitudesFollowTheUnit) {
    // Millimetres must be UNCHANGED from the old literal magnitudes.
    { ScopedUnit s(LengthUnit::Mm);
      const double st = materializr::unitInfo(materializr::currentUnit()).step;
      EXPECT_NEAR(10.0, 10.0 * st, 1e-9);
      EXPECT_NEAR(1.0,  1.0  * st, 1e-9);
      EXPECT_NEAR(0.1,  0.1  * st, 1e-9) << "mm keeps exactly the old 10/1/0.1"; }
    // Inches: the middle button means one tenth of an inch, i.e. 2.54 mm.
    { ScopedUnit s(LengthUnit::In);
      const double st = materializr::unitInfo(materializr::currentUnit()).step;
      EXPECT_NEAR(2.54, materializr::toMm(st) , 1e-9)
          << "a +0.1 in button must move the model by 2.54 mm, not 0.1 mm"; }
    // Centimetres land back on the millimetre grid.
    { ScopedUnit s(LengthUnit::Cm);
      const double st = materializr::unitInfo(materializr::currentUnit()).step;
      EXPECT_NEAR(1.0, materializr::toMm(st), 1e-9); }
}
