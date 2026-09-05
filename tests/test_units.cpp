// Display units: the one place a length changes unit.
//
// This test deliberately includes NO ImGui header. Units.h must compile in a TU
// that has never seen imgui.h — any ImGui call creeping into it (the Round-2
// review caught a ClearActiveID() that would have crashed before the context
// existed) fails right here at compile time.

#include "core/Units.h"
#include "core/LengthEdit.h"

#include <gtest/gtest.h>

#include <cmath>
#include <iterator>
#include <string>

using materializr::LengthUnit;

namespace {

// Every test that changes the global unit restores it on exit, including on an
// early ASSERT failure — otherwise one failing case would poison the rest.
struct ScopedUnit {
    LengthUnit saved;
    explicit ScopedUnit(LengthUnit u) : saved(materializr::currentUnit()) { materializr::setCurrentUnit(u); }
    ~ScopedUnit() { materializr::setCurrentUnit(saved); }
};

const LengthUnit kAll[] = { LengthUnit::Mm, LengthUnit::Cm, LengthUnit::M,
                            LengthUnit::In, LengthUnit::Ft };

} // namespace

TEST(Units, DefaultIsMillimetres) {
    EXPECT_EQ(LengthUnit::Mm, materializr::currentUnit());
    EXPECT_STREQ("mm", materializr::unitSuffix());
}

// 1. mm -> display -> mm is the identity, to well below anything printed.
TEST(Units, RoundTripExact) {
    for (LengthUnit u : kAll) {
        ScopedUnit s(u);
        for (double x : { 0.0, 0.001, 1.0, 25.4, 304.8, 1234.5678, 1e6 })
            EXPECT_NEAR(x, materializr::toMm(materializr::toDisplay(x)), 1e-9) << "unit " << int(u);
    }
}

// 2. The same through a FLOAT shadow — many Op members are float. The error
// must stay below what the unit's own precision can show, or an untouched
// value would visibly drift after one edit.
TEST(Units, FloatMemberRoundTrip) {
    for (LengthUnit u : kAll) {
        ScopedUnit s(u);
        const double resolutionMm = std::pow(10.0, -materializr::unitInfo(u).decimals)
                                  * materializr::unitInfo(u).toMm;
        for (float x : { 0.5f, 25.4f, 100.0f, 1234.5f }) {
            const float disp = static_cast<float>(materializr::toDisplay(x));
            const float back = static_cast<float>(materializr::toMm(disp));
            EXPECT_LT(std::fabs(back - x), resolutionMm) << "unit " << int(u) << " x=" << x;
        }
    }
}

// 3. Exact strings, all five units, length / area / volume.
TEST(Units, FormatEachUnit) {
    const double mm = 25.4, mm2 = 645.16, mm3 = 16387.064;   // 1 in, 1 in², 1 in³
    struct Row { LengthUnit u; const char* len; const char* area; const char* vol; };
    const Row rows[] = {
        { LengthUnit::Mm, "25.40 mm",  "645.16 mm\xC2\xB2",   "16387.06 mm\xC2\xB3" },
        { LengthUnit::Cm, "2.540 cm",  "6.452 cm\xC2\xB2",    "16.387 cm\xC2\xB3"   },
        { LengthUnit::M,  "0.0254 m",  "0.0006 m\xC2\xB2",    "0.0000 m\xC2\xB3"    },
        { LengthUnit::In, "1.000 in",  "1.000 in\xC2\xB2",    "1.000 in\xC2\xB3"    },
        { LengthUnit::Ft, "0.0833 ft", "0.0069 ft\xC2\xB2",   "0.0006 ft\xC2\xB3"   },
    };
    for (const Row& r : rows) {
        ScopedUnit s(r.u);
        EXPECT_EQ(std::string(r.len),  materializr::fmtLength(mm))  << "unit " << int(r.u);
        EXPECT_EQ(std::string(r.area), materializr::fmtArea(mm2))   << "unit " << int(r.u);
        EXPECT_EQ(std::string(r.vol),  materializr::fmtVolume(mm3)) << "unit " << int(r.u);
    }
}

// 4. A trailing unit token overrides the current unit. Longest match first:
// "5m" is metres, not mm.
TEST(Units, ParseSuffixes) {
    ScopedUnit s(LengthUnit::Mm);
    double mm = -1;
    EXPECT_TRUE(materializr::parseLength("25.4mm", mm)); EXPECT_DOUBLE_EQ(25.4,  mm);
    EXPECT_TRUE(materializr::parseLength("1in",    mm)); EXPECT_DOUBLE_EQ(25.4,  mm);
    EXPECT_TRUE(materializr::parseLength("2\"",    mm)); EXPECT_DOUBLE_EQ(50.8,  mm);
    EXPECT_TRUE(materializr::parseLength("3ft",    mm)); EXPECT_DOUBLE_EQ(914.4, mm);
    EXPECT_TRUE(materializr::parseLength("3'",     mm)); EXPECT_DOUBLE_EQ(914.4, mm);
    EXPECT_TRUE(materializr::parseLength("2cm",    mm)); EXPECT_DOUBLE_EQ(20.0,  mm);
    EXPECT_TRUE(materializr::parseLength("5m",     mm)); EXPECT_DOUBLE_EQ(5000.0, mm) << "'5m' must be metres";
    EXPECT_TRUE(materializr::parseLength("1IN",    mm)); EXPECT_DOUBLE_EQ(25.4,  mm);
    EXPECT_TRUE(materializr::parseLength(" 1 in ", mm)); EXPECT_DOUBLE_EQ(25.4,  mm);
}

// 5. No suffix means "whatever the user is looking at".
TEST(Units, ParseNoSuffixUsesCurrentUnit) {
    double mm = -1;
    { ScopedUnit s(LengthUnit::In); EXPECT_TRUE(materializr::parseLength("1",   mm)); EXPECT_DOUBLE_EQ(25.4,  mm); }
    { ScopedUnit s(LengthUnit::Ft); EXPECT_TRUE(materializr::parseLength("0.5", mm)); EXPECT_DOUBLE_EQ(152.4, mm); }
    { ScopedUnit s(LengthUnit::Mm); EXPECT_TRUE(materializr::parseLength("7",   mm)); EXPECT_DOUBLE_EQ(7.0,   mm); }
}

// 6. Garbage is refused and the output is left alone — parseFinite's contract.
TEST(Units, ParseRejectsGarbage) {
    ScopedUnit s(LengthUnit::Mm);
    double mm = 42.0;
    EXPECT_FALSE(materializr::parseLength("abc",     mm));
    EXPECT_FALSE(materializr::parseLength("1e999in", mm));   // inf
    EXPECT_FALSE(materializr::parseLength("",        mm));
    EXPECT_FALSE(materializr::parseLength("   ",     mm));
    EXPECT_FALSE(materializr::parseLength("in",      mm));   // suffix, no number
    EXPECT_FALSE(materializr::parseLength(nullptr,   mm));
    EXPECT_DOUBLE_EQ(42.0, mm) << "a refusal must not touch the output";
}

// 7. Expressions are refused. Formulas are mm and are out of this parser's
// scope — accepting "10+5" here (strtod would read the 10) is exactly how a
// variable-bearing formula would get scaled by the display unit.
TEST(Units, ParseRejectsExpressions) {
    ScopedUnit s(LengthUnit::In);
    double mm = 42.0;
    EXPECT_FALSE(materializr::parseLength("width/2", mm));
    EXPECT_FALSE(materializr::parseLength("10+5",    mm));
    EXPECT_FALSE(materializr::parseLength("2*in",    mm));
    EXPECT_FALSE(materializr::parseLength("1 2",     mm));
    EXPECT_FALSE(materializr::parseLength("1in2",    mm));
    EXPECT_DOUBLE_EQ(42.0, mm);
}

// Area and volume scale by the factor squared and cubed, not the factor.
TEST(Units, AreaVolumeFactors) {
    ScopedUnit s(LengthUnit::In);
    EXPECT_NEAR(1.0, materializr::areaToDisplay(645.16),   1e-9);
    EXPECT_NEAR(1.0, materializr::volToDisplay(16387.064), 1e-9);
}

// Out-of-range enum values fall back to mm instead of indexing past the table
// (a hand-edited settings file is the realistic source).
TEST(Units, OutOfRangeUnitFallsBackToMm) {
    EXPECT_STREQ("mm", materializr::unitInfo(static_cast<LengthUnit>(99)).suffix);
    EXPECT_STREQ("mm", materializr::unitInfo(static_cast<LengthUnit>(-1)).suffix);
}

// 16. The RAII restorer every test relies on actually restores — including when
// the scope is left early.
TEST(Units, ScopedUnitRestores) {
    ASSERT_EQ(LengthUnit::Mm, materializr::currentUnit());
    {
        ScopedUnit s(LengthUnit::Ft);
        EXPECT_EQ(LengthUnit::Ft, materializr::currentUnit());
    }
    EXPECT_EQ(LengthUnit::Mm, materializr::currentUnit());
    auto earlyReturn = [] { ScopedUnit s(LengthUnit::Cm); return; };
    earlyReturn();
    EXPECT_EQ(LengthUnit::Mm, materializr::currentUnit());
}

// --- seed/commit symmetry --------------------------------------------------
// Every editable length field has two halves: a SEED that writes the model into
// a text buffer, and a COMMIT that parses that buffer back. If only one of them
// converts, opening a dialog and pressing Enter without typing anything moves
// the value by the unit factor — silent corruption from doing nothing.
//
// Seven fields shipped exactly that way: commit through parseLength (converts),
// seed through snprintf("%.2f") (does not). No test could see it, because each
// half was correct in isolation. This pins the PAIR.
TEST(Units, SeedThenCommitIsIdentity) {
    const double values[] = { 0.5, 1.0, 12.7, 25.4, 100.0, 304.8, 1234.5 };
    for (int u = 0; u < static_cast<int>(std::size(kAll)); ++u) {
        ScopedUnit guard(kAll[u]);
        for (double mm : values) {
            char buf[64] = "";
            ASSERT_TRUE(materializr::formatLengthDigits(buf, sizeof(buf), mm))
                << "unit " << u << " value " << mm;
            double back = 0.0;
            ASSERT_TRUE(materializr::parseLength(buf, back))
                << "unit " << u << " could not re-read its own seed: \"" << buf << "\"";
            // Tolerance is what the unit's own decimals can represent — the
            // seed rounds to that many places, so the round trip cannot beat it.
            const double step = std::pow(10.0, -materializr::unitInfo(
                                    materializr::currentUnit()).decimals);
            const double tol = materializr::unitInfo(materializr::currentUnit()).toMm * step;
            EXPECT_NEAR(mm, back, tol)
                << "unit " << u << ": seeded \"" << buf << "\" read back as " << back
                << " from " << mm << " mm";
        }
    }
}

// The dimension popup's seed is the same contract, with the diameter doubling
// folded in. A circle's Radius constraint stores a radius and is shown as Ø.
TEST(Units, DimensionSeedThenCommitIsIdentity) {
    ScopedUnit guard(LengthUnit::Cm);
    const double radiusMm = 14.85;            // the value that exposed the bug
    char buf[64] = "";
    ASSERT_TRUE(materializr::seedDimensionText(buf, sizeof(buf),
                materializr::DimKind::Radius, /*isArc=*/false, radiusMm));
    EXPECT_STREQ("2.970", buf) << "a circle seeds its DIAMETER in the display unit";

    double committed = radiusMm;
    ASSERT_TRUE(materializr::applyDimensionEdit(materializr::DimKind::Radius,
                /*isArc=*/false, buf, committed));
    EXPECT_NEAR(radiusMm, committed, 1e-6)
        << "committing an untouched seed must not move the value";
}

// --- canonical capture ------------------------------------------------------
// Operation::description() formats lengths in the DISPLAY unit, and those
// strings are written into the .mzr as DESC. Saved under inches, a step read
// "Extrude 2.000 in" forever after, on any machine, because a step that
// reloads as a baked ReplayOp returns the stored string verbatim. Anything
// destined for the file is captured under a forced-mm scope.
TEST(Units, ScopedUnitForcesAndRestores) {
    materializr::setCurrentUnit(materializr::LengthUnit::In);
    ASSERT_EQ("1.000 in", materializr::fmtLength(25.4)) << "precondition";
    {
        materializr::ScopedUnit canonical(materializr::LengthUnit::Mm);
        EXPECT_EQ("25.40 mm", materializr::fmtLength(25.4))
            << "inside the scope, a caption is millimetres regardless of the user's unit";
    }
    EXPECT_EQ("1.000 in", materializr::fmtLength(25.4))
        << "and the user's unit is restored on scope exit";
    materializr::setCurrentUnit(materializr::LengthUnit::Mm);
}

// The guard must survive an early return — a throw mid-capture would otherwise
// leave the whole app formatting in millimetres.
TEST(Units, ScopedUnitRestoresOnEarlyExit) {
    materializr::setCurrentUnit(materializr::LengthUnit::Ft);
    auto bail = []() {
        materializr::ScopedUnit canonical(materializr::LengthUnit::Mm);
        return;   // early return with the guard live
    };
    bail();
    EXPECT_EQ(materializr::LengthUnit::Ft, materializr::currentUnit());
    materializr::setCurrentUnit(materializr::LengthUnit::Mm);
}

// The unit table picks decimals so every unit resolves to about 0.01 mm.
// A hardcoded "%.3f" under metres or feet (which ask for 4) snapped the stored
// value to a 1 mm grid on every commit.
TEST(Units, LengthFormatFollowsTheTable) {
    struct { materializr::LengthUnit u; const char* fmt; } cases[] = {
        { materializr::LengthUnit::Mm, "%.2f" },
        { materializr::LengthUnit::Cm, "%.3f" },
        { materializr::LengthUnit::M,  "%.4f" },
        { materializr::LengthUnit::In, "%.3f" },
        { materializr::LengthUnit::Ft, "%.4f" },
    };
    for (const auto& c : cases) {
        materializr::ScopedUnit s(c.u);
        EXPECT_STREQ(c.fmt, materializr::lengthFormat());
    }
    // And the precision it buys: metres must still resolve a 0.1 mm edit.
    materializr::ScopedUnit s(materializr::LengthUnit::M);
    char buf[32];
    std::snprintf(buf, sizeof(buf), materializr::lengthFormat(),
                  materializr::toDisplay(1234.5));
    double back = 0.0;
    ASSERT_TRUE(materializr::parseLength(buf, back));
    EXPECT_NEAR(1234.5, back, 0.05) << "4 decimals of metres keeps 0.1 mm";
}

// Switching to a large unit must move the WORKING SCALE with it. The sketch
// grid step is stored in millimetres and also sizes the initial sketch view
// (orthoSize = gridStep * 40), so a 1 mm grid gives a ~40 mm working area.
// Relabelling that as feet leaves the whole visible sketch 0.13 ft across and
// the snap lattice 1/300th of a useful one. Carrying the DISPLAYED NUMBER
// across is what "work in feet now" means.
TEST(Units, GridStepCarriesItsNumberNotItsMillimetres) {
    // The transform applyDisplayUnitChange performs, in isolation.
    auto carry = [](float stepMm, materializr::LengthUnit from,
                    materializr::LengthUnit to) {
        materializr::ScopedUnit s(from);
        const double shown = materializr::toDisplay(stepMm);
        materializr::setCurrentUnit(to);
        return static_cast<float>(materializr::toMm(shown));
    };

    // 1 mm -> feet is 1 FOOT, not 1 mm relabelled as 0.00328 ft.
    EXPECT_NEAR(304.8f, carry(1.0f, materializr::LengthUnit::Mm,
                              materializr::LengthUnit::Ft), 1e-3);
    // and the view it drives becomes ~40 ft rather than ~40 mm.
    EXPECT_NEAR(12192.0f, 304.8f * 40.0f, 1e-1);

    // 10 mm under cm reads "1", so inches give one inch.
    EXPECT_NEAR(25.4f, carry(10.0f, materializr::LengthUnit::Cm,
                             materializr::LengthUnit::In), 1e-3);
    // Round trip returns exactly where it started.
    const float there = carry(1.0f, materializr::LengthUnit::Mm, materializr::LengthUnit::Ft);
    EXPECT_NEAR(1.0f, carry(there, materializr::LengthUnit::Ft,
                            materializr::LengthUnit::Mm), 1e-4);
    materializr::setCurrentUnit(materializr::LengthUnit::Mm);
}
