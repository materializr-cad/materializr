// The arc tool takes typed dimensions like the line, circle and rectangle do.
//
// Arc placement is three clicks - chord start, chord end, apex - so no single
// number ever specified it and applyDimension() simply refused for Arc. It now
// works in two stages:
//
//   click 2 : the CHORD, the straight-line distance between the arc's ends;
//   click 3 : ONE number for the bow, read as either the swept ANGLE or the
//             RADIUS depending on SketchTool::ArcDimMode.
//
// Which way the arc bows stays with the cursor - that is a direction, not a
// dimension, and no number expresses it.
//
// Both readings describe the same family of arcs, so the tests below check them
// against each other as well as against absolutes: a 90° sweep on a chord of
// length L must produce radius L/√2, and a semicircle must produce radius L/2.

#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SketchSolver.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>
#include <string>

// SketchTool's Text / SVG stamp paths are not part of materializr_core (they
// pull in font rendering); stub them so this links.
namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using materializr::Sketch;
using materializr::SketchArc;
using materializr::SketchPoint;
using materializr::SketchSolver;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {

struct Rig {
    Sketch sketch;
    SketchSolver solver;
    SketchTool tool;
    Rig() {
        tool.setSketch(&sketch);
        tool.setSolver(&solver);
        tool.setGridStep(1.0f);
        tool.setSnapToGridEnabled(false);   // typed values must land exactly
        tool.setInferenceLevel(SketchTool::InferenceLevel::Off);
        tool.setMode(SketchToolMode::Arc);
    }
    void click(glm::vec2 p) { tool.onMouseMove(p); tool.onMouseDown(p); }
    void hover(glm::vec2 p) { tool.onMouseMove(p); }
    const SketchArc* lastArc() const {
        return sketch.getArcs().empty() ? nullptr : &sketch.getArcs().back();
    }
    // Radius the placed arc actually came out with, measured from its centre to
    // its start point rather than trusting the stored value.
    float measuredRadius() const {
        const SketchArc* a = lastArc();
        if (!a) return -1.0f;
        const SketchPoint* c = sketch.getPoint(a->centerPointId);
        const SketchPoint* s = sketch.getPoint(a->startPointId);
        if (!c || !s) return -1.0f;
        return glm::length(s->pos - c->pos);
    }
    // Swept angle start -> end, in degrees. handleArcTool deliberately stores
    // the endpoints in the order whose CCW sweep passes through the apex, so
    // the plain CCW sweep IS the arc's real swept angle - no complement.
    float measuredSweepDeg() const {
        const SketchArc* a = lastArc();
        if (!a) return -1.0f;
        const SketchPoint* c = sketch.getPoint(a->centerPointId);
        const SketchPoint* s = sketch.getPoint(a->startPointId);
        const SketchPoint* e = sketch.getPoint(a->endPointId);
        if (!c || !s || !e) return -1.0f;
        const float sA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        const float eA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        const float TWO_PI = 2.0f * static_cast<float>(M_PI);
        float d = eA - sA;
        while (d < 0.0f)      d += TWO_PI;
        while (d >= TWO_PI)   d -= TWO_PI;
        return d * 180.0f / static_cast<float>(M_PI);
    }
};

constexpr float kL = 20.0f;                      // chord length used throughout
const glm::vec2 kA(0.0f, 0.0f), kB(kL, 0.0f);    // chord along +X

} // namespace

TEST(ArcTypedDimension, ChordLengthIsTypedAtTheSecondClick) {
    Rig r;
    r.click(kA);
    r.hover(glm::vec2(5.0f, 0.0f));              // aim +X; length comes from typing
    ASSERT_TRUE(r.tool.applyDimension(kL));
    EXPECT_EQ(r.tool.getClickCount(), 2) << "typing the chord should advance to the apex";
    EXPECT_NEAR(r.tool.getSecondClick().x, kL, 1e-3f);
    EXPECT_NEAR(r.tool.getSecondClick().y, 0.0f, 1e-3f);
}

// A semicircle: the apex is half the chord off the midpoint, radius L/2.
TEST(ArcTypedDimension, SweepOf180MakesASemicircle) {
    Rig r;
    r.tool.setArcDimMode(SketchTool::ArcDimMode::Sweep);
    r.click(kA);
    r.click(kB);
    r.hover(glm::vec2(kL * 0.5f, 5.0f));         // bow upward
    ASSERT_TRUE(r.tool.applyDimension(180.0f));
    ASSERT_NE(r.lastArc(), nullptr);
    EXPECT_NEAR(r.measuredRadius(), kL * 0.5f, 1e-2f);
    EXPECT_NEAR(r.measuredSweepDeg(), 180.0f, 0.5f);
}

// 90° on a chord of length L means radius L/√2.
TEST(ArcTypedDimension, SweepOf90GivesTheMatchingRadius) {
    Rig r;
    r.tool.setArcDimMode(SketchTool::ArcDimMode::Sweep);
    r.click(kA);
    r.click(kB);
    r.hover(glm::vec2(kL * 0.5f, 5.0f));
    ASSERT_TRUE(r.tool.applyDimension(90.0f));
    ASSERT_NE(r.lastArc(), nullptr);
    EXPECT_NEAR(r.measuredRadius(), kL / std::sqrt(2.0f), 1e-2f);
}

// The same arc asked for the other way round.
TEST(ArcTypedDimension, RadiusModeReproducesTheSweepModeArc) {
    const float wantR = kL / std::sqrt(2.0f);    // the 90° arc above
    Rig r;
    r.tool.setArcDimMode(SketchTool::ArcDimMode::Radius);
    r.click(kA);
    r.click(kB);
    r.hover(glm::vec2(kL * 0.5f, 5.0f));
    ASSERT_TRUE(r.tool.applyDimension(wantR));
    ASSERT_NE(r.lastArc(), nullptr);
    EXPECT_NEAR(r.measuredRadius(), wantR, 1e-2f);
    // Radius mode takes the MINOR arc, so this is the 90° one, not the 270°.
    EXPECT_NEAR(r.measuredSweepDeg(), 90.0f, 0.5f);
}

// A radius under half the chord describes no arc through both endpoints.
TEST(ArcTypedDimension, RadiusBelowHalfTheChordIsRefused) {
    Rig r;
    r.tool.setArcDimMode(SketchTool::ArcDimMode::Radius);
    r.click(kA);
    r.click(kB);
    r.hover(glm::vec2(kL * 0.5f, 5.0f));
    EXPECT_NEAR(r.tool.arcMinRadius(), kL * 0.5f, 1e-3f);
    EXPECT_FALSE(r.tool.applyDimension(kL * 0.5f - 1.0f));
    EXPECT_EQ(r.lastArc(), nullptr) << "refused input must not place an arc";
    // Exactly half the chord IS valid - that is the semicircle.
    EXPECT_TRUE(r.tool.applyDimension(kL * 0.5f));
    ASSERT_NE(r.lastArc(), nullptr);
    EXPECT_NEAR(r.measuredRadius(), kL * 0.5f, 1e-2f);
}

// Which side the arc bows to follows the cursor, not the number.
TEST(ArcTypedDimension, CursorSideChoosesWhichWayItBows) {
    const SketchPoint* cUp = nullptr;
    float upY = 0.0f, downY = 0.0f;
    {
        Rig r;
        r.tool.setArcDimMode(SketchTool::ArcDimMode::Sweep);
        r.click(kA); r.click(kB);
        r.hover(glm::vec2(kL * 0.5f, 5.0f));     // above the chord
        ASSERT_TRUE(r.tool.applyDimension(90.0f));
        ASSERT_NE(r.lastArc(), nullptr);
        cUp = r.sketch.getPoint(r.lastArc()->centerPointId);
        ASSERT_NE(cUp, nullptr);
        upY = cUp->pos.y;
    }
    {
        Rig r;
        r.tool.setArcDimMode(SketchTool::ArcDimMode::Sweep);
        r.click(kA); r.click(kB);
        r.hover(glm::vec2(kL * 0.5f, -5.0f));    // below the chord
        ASSERT_TRUE(r.tool.applyDimension(90.0f));
        ASSERT_NE(r.lastArc(), nullptr);
        const SketchPoint* c = r.sketch.getPoint(r.lastArc()->centerPointId);
        ASSERT_NE(c, nullptr);
        downY = c->pos.y;
    }
    // Bowing up puts the centre BELOW the chord and vice versa.
    EXPECT_LT(upY, 0.0f);
    EXPECT_GT(downY, 0.0f);
}

// A typed sweep is exact and must not be dragged onto the nearest 15° step the
// way a dragged apex is.
TEST(ArcTypedDimension, TypedSweepIsNotPulledOntoA15DegreeStep) {
    Rig r;
    r.tool.setArcDimMode(SketchTool::ArcDimMode::Sweep);
    r.click(kA);
    r.click(kB);
    r.hover(glm::vec2(kL * 0.5f, 5.0f));
    ASSERT_TRUE(r.tool.applyDimension(97.0f));   // 7° off the 90 step
    ASSERT_NE(r.lastArc(), nullptr);
    EXPECT_NEAR(r.measuredSweepDeg(), 97.0f, 0.5f);
}

// Sweep is the default, and the choice sticks across placements so a run of
// same-radius arcs is not a mode toggle each time.
TEST(ArcTypedDimension, ModeDefaultsToSweepAndPersists) {
    Rig r;
    EXPECT_EQ(r.tool.getArcDimMode(), SketchTool::ArcDimMode::Sweep);
    r.tool.setArcDimMode(SketchTool::ArcDimMode::Radius);
    r.click(kA);
    r.click(kB);
    r.hover(glm::vec2(kL * 0.5f, 5.0f));
    ASSERT_TRUE(r.tool.applyDimension(kL));
    // A completed arc, then a fresh one: still Radius.
    EXPECT_EQ(r.tool.getArcDimMode(), SketchTool::ArcDimMode::Radius);
    r.tool.setMode(SketchToolMode::Arc);
    EXPECT_EQ(r.tool.getArcDimMode(), SketchTool::ArcDimMode::Radius);
}

// arcMinRadius only means anything at the apex stage.
TEST(ArcTypedDimension, MinRadiusIsZeroBeforeTheChordExists) {
    Rig r;
    EXPECT_FLOAT_EQ(r.tool.arcMinRadius(), 0.0f);
    r.click(kA);
    EXPECT_FLOAT_EQ(r.tool.arcMinRadius(), 0.0f);
    r.click(kB);
    EXPECT_NEAR(r.tool.arcMinRadius(), kL * 0.5f, 1e-3f);
}
