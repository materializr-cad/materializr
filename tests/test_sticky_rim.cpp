// A point placed on a circle or arc STAYS on it - stickily, not rigidly.
//
// Rim snapping itself has always worked (SketchTool's curve-perimeter snap,
// which even lands the point where the rim crosses a grid line). What was
// missing was memory: nothing recorded that a point had been placed on a rim,
// so the first drag threw the relationship away - and with grid snap on, the
// old multi-point drag rounded EVERY point to the lattice, which moved a line's
// ends 0.6mm off their circles and rotated the line 30 -> 33.7 degrees on a
// drag of essentially zero length.
//
// So SketchPoint carries onCurveId, and dragging consults it. Deliberately NOT
// a solver Constraint: constraints here are opt-in and binding, and Steve asked
// for something you can walk away from - "we can break it and it will be fine
// if we simply take the point off the circle with a move, but moving the line
// about the circle keeps it constrained on the circle". Hence two break bands:
// generous when the whole selection moves, tight when the point is dragged on
// its own.
#include <gtest/gtest.h>

#include "io/ProjectIO.h"
#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <cmath>
#include <sstream>
#include <string>

namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using materializr::ProjectIO;
using materializr::Sketch;
using materializr::SketchPoint;
using materializr::SketchSolver;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {

constexpr float kStep = 1.0f;
constexpr float kR    = 3.4f;              // deliberately not a grid multiple
const glm::vec2 kC(0.0f, 0.0f);

// A circle with a line drawn onto its rim.
struct Rig {
    Sketch sketch;
    SketchSolver solver;
    SketchTool tool;
    int circleId = -1;
    int endPt = -1;

    Rig() {
        tool.setSketch(&sketch);
        tool.setSolver(&solver);
        tool.setGridStep(kStep);
        tool.setSnapToGridEnabled(true);
        const int c0 = sketch.addPoint(kC);
        circleId = sketch.addCircle(c0, kR);

        const float a = 40.0f * static_cast<float>(M_PI) / 180.0f;
        const glm::vec2 aim((kR + 0.15f) * std::cos(a), (kR + 0.15f) * std::sin(a));
        tool.setMode(SketchToolMode::Line);
        tool.onMouseMove(glm::vec2(14.0f, 12.0f));
        tool.onMouseDown(glm::vec2(14.0f, 12.0f));
        tool.onMouseMove(aim);
        tool.onMouseDown(aim);
        if (!sketch.getLines().empty()) endPt = sketch.getLines().back().endPointId;
    }

    void dragLineBy(glm::vec2 by) {
        const auto& l = sketch.getLines().back();
        const SketchPoint* a = sketch.getPoint(l.startPointId);
        const SketchPoint* b = sketch.getPoint(l.endPointId);
        const glm::vec2 mid = 0.5f * (a->pos + b->pos);
        tool.setMode(SketchToolMode::Select);
        tool.onMouseMove(mid);
        tool.onMouseDown(mid);
        tool.onMouseMove(mid + by);
        tool.onMouseUp(mid + by);
    }

    const SketchPoint* end() const { return sketch.getPoint(endPt); }
    float offRim() const {
        const SketchPoint* p = end();
        return p ? std::abs(glm::length(p->pos - kC) - kR) : 1e9f;
    }
};

} // namespace

TEST(StickyRim, DrawingOntoARimRecordsIt) {
    Rig r;
    ASSERT_NE(r.endPt, -1);
    const SketchPoint* p = r.end();
    ASSERT_NE(p, nullptr);
    EXPECT_NEAR(r.offRim(), 0.0f, 1e-3f) << "the point should land ON the rim";
    EXPECT_EQ(p->onCurveId, r.circleId) << "and remember which rim";
}

TEST(StickyRim, MovingTheLineSlidesTheEndAlongTheRim) {
    Rig r;
    const glm::vec2 before = r.end()->pos;
    r.dragLineBy(glm::vec2(2.0f, 0.0f));

    EXPECT_NEAR(r.offRim(), 0.0f, 1e-3f) << "still on the rim";
    EXPECT_EQ(r.end()->onCurveId, r.circleId) << "still attached";
    EXPECT_GT(glm::length(r.end()->pos - before), 0.1f) << "but it did move";
}

TEST(StickyRim, DraggingItClearLetsGo) {
    Rig r;
    r.dragLineBy(glm::vec2(9.0f, 7.0f));
    EXPECT_EQ(r.end()->onCurveId, -1) << "hauled away: the attachment releases";
    EXPECT_GT(r.offRim(), 1.0f);
}

TEST(StickyRim, ATwitchMovesNothing) {
    // The delta is quantised, not each point. Before, a sub-grid drag rounded
    // every point to the lattice - so pressing on a line and twitching moved it,
    // rotated it, and threw away both rim attachments.
    Rig r;
    const glm::vec2 before = r.end()->pos;
    r.dragLineBy(glm::vec2(0.1f, 0.0f));
    EXPECT_NEAR(glm::length(r.end()->pos - before), 0.0f, 1e-4f);
    EXPECT_EQ(r.end()->onCurveId, r.circleId);
}

TEST(StickyRim, ARimLandingPublishesAGuide) {
    // Rim snapping predates the guide overlay and drew NOTHING, so landing on a
    // circle looked the same as landing nowhere - no marker, no label, no way to
    // tell it had happened. It now publishes an OnCircle guide, which the
    // viewport draws with the same diamond marker as On Line.
    Sketch sketch; SketchSolver solver; SketchTool tool;
    tool.setSketch(&sketch); tool.setSolver(&solver);
    tool.setGridStep(kStep); tool.setSnapToGridEnabled(true);
    const int c0 = sketch.addPoint(kC);
    const int circleId = sketch.addCircle(c0, kR);

    tool.setMode(SketchToolMode::Line);
    tool.onMouseMove(glm::vec2(14.0f, 12.0f));
    tool.onMouseDown(glm::vec2(14.0f, 12.0f));

    const float a = 40.0f * static_cast<float>(M_PI) / 180.0f;
    tool.onMouseMove(glm::vec2((kR + 0.15f) * std::cos(a),
                               (kR + 0.15f) * std::sin(a)));

    bool sawRim = false;
    for (const auto& g : tool.getActiveInferences())
        if (g.kind == materializr::InferenceGuide::OnCircle) {
            sawRim = true;
            EXPECT_EQ(g.refId, circleId) << "the guide should name the rim";
        }
    EXPECT_TRUE(sawRim) << "hovering a rim should announce itself";
}

TEST(StickyRim, HoveringNowhereNearARimAnnouncesNothing) {
    Sketch sketch; SketchSolver solver; SketchTool tool;
    tool.setSketch(&sketch); tool.setSolver(&solver);
    tool.setGridStep(kStep); tool.setSnapToGridEnabled(true);
    const int c0 = sketch.addPoint(kC);
    sketch.addCircle(c0, kR);

    tool.setMode(SketchToolMode::Line);
    tool.onMouseMove(glm::vec2(14.0f, 12.0f));
    tool.onMouseDown(glm::vec2(14.0f, 12.0f));
    tool.onMouseMove(glm::vec2(11.0f, 9.0f));      // far from the rim

    for (const auto& g : tool.getActiveInferences())
        EXPECT_NE(g.kind, materializr::InferenceGuide::OnCircle);
}

TEST(StickyRim, TheAttachmentSurvivesSaveAndLoad) {
    Rig r;
    ASSERT_EQ(r.end()->onCurveId, r.circleId);

    std::ostringstream out;
    ProjectIO::writeSketchBody(out, r.sketch);
    out << "SKETCH_END\n";

    Sketch reloaded;
    std::istringstream in(out.str());
    ProjectIO::parseSketchBody(in, reloaded, "SKETCH_END");

    const SketchPoint* p = reloaded.getPoint(r.endPt);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->onCurveId, r.circleId) << "a reloaded sketch keeps the rim";
    EXPECT_NEAR(std::abs(glm::length(p->pos - kC) - kR), 0.0f, 1e-3f);
}

TEST(StickyRim, AFreePointStaysFreeThroughSaveAndLoad) {
    // The default must round-trip too - an old file has no such token at all,
    // and a point that was never on a rim must not come back attached to id 0.
    Sketch sketch;
    const int a = sketch.addPoint(glm::vec2(5.0f, 5.0f));

    std::ostringstream out;
    ProjectIO::writeSketchBody(out, sketch);
    out << "SKETCH_END\n";

    Sketch reloaded;
    std::istringstream in(out.str());
    ProjectIO::parseSketchBody(in, reloaded, "SKETCH_END");

    const SketchPoint* p = reloaded.getPoint(a);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->onCurveId, -1);
}
