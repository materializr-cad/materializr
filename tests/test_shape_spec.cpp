// Line / Circle / Rectangle / Arc take typed values from their panels
// (SketchTool::ShapeSpec), the way Polygon already did. A typed field overrides
// what the cursor says for THAT value only; the rest keeps following the cursor,
// and the preview and the commit read the same numbers.

#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SketchSolver.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>
#include <map>
#include <string>

namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using materializr::Sketch;
using materializr::SketchSolver;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {
struct Rig {
    Sketch sketch;
    SketchSolver solver;
    SketchTool tool;
    explicit Rig(SketchToolMode m) {
        tool.setSketch(&sketch);
        tool.setSolver(&solver);
        tool.setGridStep(1.0f);
        tool.setSnapToGridEnabled(false);
        tool.setInferenceLevel(SketchTool::InferenceLevel::Off);
        tool.setMode(m);
    }
    void click(glm::vec2 p) { tool.onMouseMove(p); tool.onMouseDown(p); }
    void hover(glm::vec2 p) { tool.onMouseMove(p); }
};
glm::vec2 pt(const Sketch& s, int id) { return s.getPoint(id)->pos; }
}

TEST(ShapeSpec, LineTypedLengthKeepsCursorDirection) {
    Rig r(SketchToolMode::Line);
    r.click({0, 0});
    r.hover({3, 4});                       // direction 53.13 deg, cursor length 5
    r.tool.shapeSpec().hasLen = true;
    r.tool.shapeSpec().len = 10.0;
    const glm::vec2 e = r.tool.getPreviewEnd();
    EXPECT_NEAR(e.x, 6.0f, 1e-4f);
    EXPECT_NEAR(e.y, 8.0f, 1e-4f);
    ASSERT_TRUE(r.tool.commitShapeNow());
    ASSERT_EQ(r.sketch.getLines().size(), 1u);
    const auto& l = r.sketch.getLines().front();
    EXPECT_NEAR(glm::length(pt(r.sketch, l.endPointId) - pt(r.sketch, l.startPointId)), 10.0f, 1e-4f);
    // The typed length becomes a Distance dimension, as the old popup did.
    bool dist = false;
    for (const auto& c : r.sketch.getConstraints())
        if (c.type == materializr::ConstraintType::Distance && std::abs(c.value - 10.0) < 1e-9) dist = true;
    EXPECT_TRUE(dist);
}

TEST(ShapeSpec, LineTypedAngleKeepsCursorLength) {
    Rig r(SketchToolMode::Line);
    r.click({0, 0});
    r.hover({3, 4});
    r.tool.shapeSpec().hasAngle = true;
    r.tool.shapeSpec().angleDeg = 90.0;
    const glm::vec2 e = r.tool.getPreviewEnd();
    EXPECT_NEAR(e.x, 0.0f, 1e-4f);
    EXPECT_NEAR(e.y, 5.0f, 1e-4f);
}

TEST(ShapeSpec, FollowCursorHandsEverythingBack) {
    Rig r(SketchToolMode::Line);
    r.click({0, 0});
    r.hover({3, 4});
    r.tool.shapeSpec().hasLen = true; r.tool.shapeSpec().len = 10.0;
    r.tool.clearShapeSpec();
    EXPECT_FALSE(r.tool.shapeSpec().any());
    const glm::vec2 e = r.tool.getPreviewEnd();
    EXPECT_NEAR(e.x, 3.0f, 1e-5f);
    EXPECT_NEAR(e.y, 4.0f, 1e-5f);
}

TEST(ShapeSpec, CircleTypedDiameterAndRadius) {
    Rig r(SketchToolMode::Circle);
    r.click({0, 0});
    r.hover({1, 0});
    r.tool.setCircleDiameter(true);
    r.tool.shapeSpec().hasLen = true;
    r.tool.shapeSpec().len = 14.0;          // a diameter
    ASSERT_TRUE(r.tool.commitShapeNow());
    ASSERT_EQ(r.sketch.getCircles().size(), 1u);
    EXPECT_NEAR(r.sketch.getCircles().front().radius, 7.0, 1e-6);
}

TEST(ShapeSpec, RectangleAxisAlignedTypedSizeKeepsConstraints) {
    Rig r(SketchToolMode::Rectangle);
    r.click({0, 0});
    r.hover({1, 1});                        // up and to the right
    r.tool.shapeSpec().hasW = true; r.tool.shapeSpec().w = 8.0;
    r.tool.shapeSpec().hasH = true; r.tool.shapeSpec().h = 26.0;
    ASSERT_TRUE(r.tool.commitShapeNow());
    ASSERT_EQ(r.sketch.getLines().size(), 4u);
    int hv = 0;
    for (const auto& c : r.sketch.getConstraints())
        if (c.type == materializr::ConstraintType::Horizontal ||
            c.type == materializr::ConstraintType::Vertical) ++hv;
    EXPECT_EQ(hv, 4);                       // angle 0 = the rectangle as it always was
}

TEST(ShapeSpec, RectangleRotatedIsAtThatAngleAndSquare) {
    Rig r(SketchToolMode::Rectangle);
    r.click({0, 0});
    r.hover({1, 1});
    r.tool.shapeSpec().hasW = true; r.tool.shapeSpec().w = 10.0;
    r.tool.shapeSpec().hasH = true; r.tool.shapeSpec().h = 4.0;
    r.tool.shapeSpec().hasAngle = true; r.tool.shapeSpec().angleDeg = 30.0;
    glm::vec2 c[4];
    ASSERT_TRUE(r.tool.rectResolve(r.tool.getCurrentPos(), c));
    EXPECT_NEAR(glm::length(c[1] - c[0]), 10.0f, 1e-4f);
    EXPECT_NEAR(glm::length(c[3] - c[0]), 4.0f, 1e-4f);
    EXPECT_NEAR(glm::dot(c[1] - c[0], c[3] - c[0]), 0.0f, 1e-3f);
    const glm::vec2 u = glm::normalize(c[1] - c[0]);
    EXPECT_NEAR(std::atan2(u.y, u.x) * 180.0f / 3.14159265f, 30.0f, 1e-3f);
    ASSERT_TRUE(r.tool.commitShapeNow());
    ASSERT_EQ(r.sketch.getLines().size(), 4u);
    EXPECT_EQ(r.sketch.getPoints().size(), 4u);
}

TEST(ShapeSpec, RectangleCentreModeIsCentredOnTheClick) {
    Rig r(SketchToolMode::Rectangle);
    r.tool.setRectMode(SketchTool::RectMode::Center);
    r.click({5, 5});
    r.hover({6, 6});
    r.tool.shapeSpec().hasW = true; r.tool.shapeSpec().w = 8.0;
    r.tool.shapeSpec().hasH = true; r.tool.shapeSpec().h = 4.0;
    glm::vec2 c[4];
    ASSERT_TRUE(r.tool.rectResolve(r.tool.getCurrentPos(), c));
    const glm::vec2 centre = 0.25f * (c[0] + c[1] + c[2] + c[3]);
    EXPECT_NEAR(centre.x, 5.0f, 1e-4f);
    EXPECT_NEAR(centre.y, 5.0f, 1e-4f);
}

TEST(ShapeSpec, ArcTypedChordThenSweepIsExact) {
    Rig r(SketchToolMode::Arc);
    r.click({0, 0});
    r.hover({1, 0});
    r.tool.shapeSpec().hasLen = true; r.tool.shapeSpec().len = 10.0;
    r.tool.shapeSpec().hasApex = true; r.tool.shapeSpec().apex = 180.0;   // degrees
    ASSERT_TRUE(r.tool.commitShapeNow());                                  // the chord end
    EXPECT_EQ(r.tool.getClickCount(), 2);
    r.hover({5, 1});                                                       // bow upwards
    ASSERT_TRUE(r.tool.commitShapeNow());
    ASSERT_EQ(r.sketch.getArcs().size(), 1u);
    EXPECT_NEAR(r.sketch.getArcs().front().radius, 5.0, 1e-3);            // a semicircle on a 10 chord
}

TEST(ShapeSpec, ArcTypedRadiusBelowHalfChordClampsToSemicircle) {
    Rig r(SketchToolMode::Arc);
    r.tool.setArcDimMode(SketchTool::ArcDimMode::Radius);
    r.click({0, 0});
    r.click({10, 0});
    r.hover({5, 2});
    r.tool.shapeSpec().hasApex = true; r.tool.shapeSpec().apex = 1.0;     // under half the chord
    const glm::vec2 a = r.tool.getPreviewEnd();
    EXPECT_NEAR(a.x, 5.0f, 1e-3f);
    EXPECT_NEAR(a.y, 5.0f, 1e-3f);                                         // the semicircle's apex
}

// A typed size of 0 is pinned at 0.00 (the panel's 0 button), not "follow the
// cursor". It cannot make a shape: the click / Place is refused, says why, and
// the placement stays live so the value can be fixed.
TEST(ShapeSpec, ZeroTypedSizeRefusesClickAndPlace) {
    {   // Line
        Rig r(SketchToolMode::Line);
        r.click({0, 0});
        r.tool.shapeSpec().hasLen = true;
        r.tool.shapeSpec().len = 0.0;
        r.click({5, 0});
        EXPECT_TRUE(r.sketch.getLines().empty());
        EXPECT_TRUE(r.tool.isPlacing());
        ASSERT_NE(nullptr, r.tool.consumeShapeRejection());
        EXPECT_EQ(nullptr, r.tool.consumeShapeRejection());   // read-and-clear
        EXPECT_FALSE(r.tool.commitShapeNow());
        EXPECT_NE(nullptr, r.tool.consumeShapeRejection());
        r.tool.shapeSpec().len = 4.0;                          // fixing it places the line
        r.click({5, 0});
        ASSERT_EQ(1u, r.sketch.getLines().size());
    }
    {   // Rectangle: either side at 0 refuses
        Rig r(SketchToolMode::Rectangle);
        r.click({0, 0});
        r.tool.shapeSpec().hasW = true;  r.tool.shapeSpec().w = 5.0;
        r.tool.shapeSpec().hasH = true;  r.tool.shapeSpec().h = 0.0;
        r.click({6, 6});
        EXPECT_TRUE(r.sketch.getLines().empty());
        EXPECT_NE(nullptr, r.tool.consumeShapeRejection());
    }
    {   // Circle
        Rig r(SketchToolMode::Circle);
        r.click({0, 0});
        r.tool.shapeSpec().hasLen = true;  r.tool.shapeSpec().len = 0.0;
        EXPECT_FALSE(r.tool.commitShapeNow());
        EXPECT_TRUE(r.sketch.getCircles().empty());
        EXPECT_NE(nullptr, r.tool.consumeShapeRejection());
    }
    {   // Polygon
        Rig r(SketchToolMode::Polygon);
        r.click({0, 0});
        r.tool.polygonSpec().hasSize = true;  r.tool.polygonSpec().size = 0.0;
        EXPECT_FALSE(r.tool.commitPolygonNow());
        EXPECT_TRUE(r.sketch.getPolygons().empty());
        EXPECT_NE(nullptr, r.tool.consumeShapeRejection());
    }
    {   // Nothing typed: no refusal, the cursor rules as before
        Rig r(SketchToolMode::Line);
        r.click({0, 0});
        r.click({3, 0});
        EXPECT_EQ(1u, r.sketch.getLines().size());
        EXPECT_EQ(nullptr, r.tool.consumeShapeRejection());
    }
}

// Switching Snap & Grid off in the middle of a drag: the dragged geometry must
// follow the cursor from then on, not keep landing on the lattice. (Tester list
// 11-14-26 reported the move "staying snapped"; at the tool level it does not.)
TEST(SnapToggle, PointDragFollowsCursorAfterSnapOff) {
    Rig r(SketchToolMode::Select);
    r.sketch.addRectangle({0, 0}, {10, 10});
    const int pid = r.sketch.addPoint({20.0f, 20.0f});
    r.tool.setGridStep(1.0f);
    r.tool.setSnapToGridEnabled(true);
    r.tool.setInferenceLevel(SketchTool::InferenceLevel::Full);
    r.hover({20.0f, 20.0f});
    r.tool.onMouseDown({20.0f, 20.0f});
    r.hover({21.4f, 22.3f});
    r.hover({23.37f, 24.81f});
    EXPECT_NEAR(r.sketch.getPoint(pid)->pos.x, 23.0f, 1e-4);   // snapped while ON
    EXPECT_NEAR(r.sketch.getPoint(pid)->pos.y, 25.0f, 1e-4);
    r.tool.setSnapToGridEnabled(false);
    for (glm::vec2 s : { glm::vec2{27.37f, 28.81f}, glm::vec2{29.62f, 31.23f}, glm::vec2{30.41f, 33.77f} }) {
        r.hover(s);
        EXPECT_NEAR(r.sketch.getPoint(pid)->pos.x, s.x, 1e-3);
        EXPECT_NEAR(r.sketch.getPoint(pid)->pos.y, s.y, 1e-3);
    }
    r.tool.onMouseUp({30.41f, 33.77f});
}

TEST(SnapToggle, LineDragFollowsCursorAfterSnapOff) {
    Rig r(SketchToolMode::Select);
    const int a = r.sketch.addPoint({20.0f, 20.0f}), b = r.sketch.addPoint({26.0f, 20.0f});
    r.sketch.addLine(a, b);
    r.tool.setGridStep(1.0f);
    r.tool.setSnapToGridEnabled(true);
    r.tool.setInferenceLevel(SketchTool::InferenceLevel::Full);
    r.hover({23.0f, 20.0f});
    r.tool.onMouseDown({23.0f, 20.0f});
    r.hover({24.4f, 21.3f});
    r.hover({26.37f, 22.81f});
    EXPECT_NEAR(r.sketch.getPoint(a)->pos.x, 23.0f, 1e-4);     // whole-step delta while ON
    EXPECT_NEAR(r.sketch.getPoint(a)->pos.y, 23.0f, 1e-4);
    r.tool.setSnapToGridEnabled(false);
    for (glm::vec2 s : { glm::vec2{28.37f, 24.81f}, glm::vec2{30.62f, 26.23f}, glm::vec2{31.41f, 28.77f} }) {
        r.hover(s);
        EXPECT_NEAR(r.sketch.getPoint(a)->pos.x, s.x - 3.0f, 1e-3);   // grabbed 3 in from the end
        EXPECT_NEAR(r.sketch.getPoint(a)->pos.y, s.y - 0.0f, 1e-3);
    }
    r.tool.onMouseUp({31.41f, 28.77f});
}

// Moving a point on a busy sketch with Snap to grid off: the only things allowed to
// pull it are a POINT snap (endpoint, midpoint, centre), the axis-aligned symmetry
// suggestion, or alignment with another point's X / Y inside a few screen pixels.
// On-line, extension and rim pulls belong to placing new geometry - in a drag they
// read as the move sticking to the grid. (Before: 57% of positions were pulled at
// Full; now about 15%, each with its guide reported.)
TEST(SnapToggle, SelectDragOnBusySketchOnlyPointAndSymmetrySnaps) {
    for (auto lvl : { SketchTool::InferenceLevel::Reduced, SketchTool::InferenceLevel::Full,
                      SketchTool::InferenceLevel::Max }) {
        Rig r(SketchToolMode::Select);
        unsigned seed = 12345;
        auto rnd = [&]{ seed = seed * 1664525u + 1013904223u; return ((seed >> 8) & 0xffff) / 65535.0f; };
        for (int i = 0; i < 12; ++i) {
            const int a = r.sketch.addPoint({rnd() * 60 - 30, rnd() * 60 - 30});
            const int b = r.sketch.addPoint({rnd() * 60 - 30, rnd() * 60 - 30});
            r.sketch.addLine(a, b);
        }
        for (int i = 0; i < 10; ++i)
            r.sketch.addCircle(r.sketch.addPoint({rnd() * 60 - 30, rnd() * 60 - 30}), 1.0 + rnd() * 6.0);
        const int pid = r.sketch.addPoint({100.0f, 100.0f});
        r.tool.setGridStep(1.0f);
        r.tool.setPixelScale(0.02f);                    // 6 px of alignment band = 0.12 mm
        r.tool.setSnapToGridEnabled(false);
        r.tool.setInferenceLevel(lvl);
        r.hover({100.0f, 100.0f});
        r.tool.onMouseDown({100.0f, 100.0f});
        int pulled = 0;
        for (int i = 0; i < 400; ++i) {
            const glm::vec2 c{rnd() * 70 - 35, rnd() * 70 - 35};
            r.hover(c);
            const glm::vec2 p = r.sketch.getPoint(pid)->pos;
            if (glm::length(p - c) <= 0.02f) continue;
            ++pulled;
            for (const auto& g : r.tool.getActiveInferences()) {
                const bool allowed = g.kind == materializr::InferenceGuide::Endpoint ||
                                     g.kind == materializr::InferenceGuide::Midpoint ||
                                     g.kind == materializr::InferenceGuide::Symmetry ||
                                     g.kind == materializr::InferenceGuide::AxisHFromPoint ||
                                     g.kind == materializr::InferenceGuide::AxisVFromPoint;
                EXPECT_TRUE(allowed) << "level " << (int)lvl << " cursor (" << c.x << "," << c.y
                                     << ") pulled by inference kind " << (int)g.kind;
            }
        }
        EXPECT_LT(pulled, 100) << "level " << (int)lvl << ": most of a free drag was still being pulled";
        r.tool.onMouseUp({0, 0});
    }
}

// A slow drag with grid snap off must follow the cursor from the very first step.
// The dragged point used to snap onto ITSELF (the cursor was snapped before the
// dragged geometry was excluded), swallowing the first ~0.25 mm of every drag -
// "stuck, then a jump", worst when zoomed in.
TEST(SnapToggle, SlowDragHasNoSelfSnapDeadZone) {
    for (float stepMm : { 0.02f, 0.05f, 0.1f, 0.2f }) {
        Rig r(SketchToolMode::Select);
        r.sketch.addRectangle({0, 0}, {10, 10});
        const int pid = r.sketch.addPoint({20.0f, 20.0f});
        r.tool.setGridStep(1.0f);
        r.tool.setSnapToGridEnabled(false);
        r.tool.setInferenceLevel(SketchTool::InferenceLevel::Full);
        r.hover({20.0f, 20.0f});
        r.tool.onMouseDown({20.0f, 20.0f});
        for (int i = 1; i <= 60; ++i) {
            const glm::vec2 c{20.0f + stepMm * i, 20.0f + 0.5f * stepMm * i};
            r.hover(c);
            const glm::vec2 p = r.sketch.getPoint(pid)->pos;
            ASSERT_NEAR(p.x, c.x, 1e-3) << "step " << stepMm << " #" << i;
            ASSERT_NEAR(p.y, c.y, 1e-3) << "step " << stepMm << " #" << i;
        }
        r.tool.onMouseUp({0, 0});
    }
}

// Dragging a line (or any selection) moves it rigidly by the cursor's travel: the
// cursor is not pulled onto unrelated endpoints, nor onto the line's own moving
// midpoint / ends, so a slow drag grabbed a little off the middle still tracks.
TEST(SnapToggle, LineDragIsRigidAndIgnoresNearbyPointsAndItself) {
    Rig r(SketchToolMode::Select);
    const int a = r.sketch.addPoint({20.0f, 20.0f}), b = r.sketch.addPoint({26.0f, 20.0f});
    r.sketch.addLine(a, b);
    r.sketch.addPoint({28.1f, 23.1f});                 // a stray point the cursor will pass close to
    r.tool.setGridStep(1.0f);
    r.tool.setSnapToGridEnabled(false);
    r.tool.setInferenceLevel(SketchTool::InferenceLevel::Max);
    r.hover({23.2f, 20.0f});                           // 0.2 mm off the line's midpoint
    r.tool.onMouseDown({23.2f, 20.0f});
    for (int i = 1; i <= 40; ++i) {                    // slow: 0.05 mm a frame
        const glm::vec2 c{23.2f + 0.05f * i, 20.0f + 0.05f * i};
        r.hover(c);
        ASSERT_NEAR(r.sketch.getPoint(a)->pos.x, 20.0f + 0.05f * i, 1e-3) << "#" << i;
        ASSERT_NEAR(r.sketch.getPoint(a)->pos.y, 20.0f + 0.05f * i, 1e-3) << "#" << i;
    }
    // Carry on past the stray point at (28.1, 23.1): the cursor goes through
    // (28.1, 23.1)+(0.1, 0.1) and the line must still be exactly cursor-(3.2, 0).
    r.hover({28.2f, 23.2f});
    EXPECT_NEAR(r.sketch.getPoint(a)->pos.x, 28.2f - 3.2f, 1e-3);
    EXPECT_NEAR(r.sketch.getPoint(a)->pos.y, 23.2f, 1e-3);
    r.tool.onMouseUp({0, 0});
}

// Moving ONE point can line it up with another point's X or Y (level this vertex
// with that one): within a few screen pixels, with the guide reported so it can be
// drawn. A line / selection drag never does, and neither does a point drag at
// Reduced.
TEST(SnapToggle, SinglePointDragAlignsToAnotherPointsXOrY) {
    auto build = [](Rig& r) {
        const int q = r.sketch.addPoint({30.0f, 5.0f});   // the alignment target
        (void)q;
        r.tool.setGridStep(1.0f);
        r.tool.setPixelScale(0.02f);                       // band = 6 px = 0.12 mm
        r.tool.setSnapToGridEnabled(false);
    };
    {   // single point, Full: 0.07 mm off Q's X -> pulled onto X = 30, the guide says so
        Rig r(SketchToolMode::Select);
        build(r);
        const int pid = r.sketch.addPoint({20.0f, 20.0f});
        r.tool.setInferenceLevel(SketchTool::InferenceLevel::Full);
        r.hover({20.0f, 20.0f});
        r.tool.onMouseDown({20.0f, 20.0f});
        r.hover({25.0f, 21.0f});
        r.hover({30.07f, 22.0f});
        EXPECT_NEAR(r.sketch.getPoint(pid)->pos.x, 30.0f, 1e-3);
        EXPECT_NEAR(r.sketch.getPoint(pid)->pos.y, 22.0f, 1e-3);   // only X aligned
        bool guide = false;
        for (const auto& g : r.tool.getActiveInferences())
            if (g.kind == materializr::InferenceGuide::AxisVFromPoint) guide = true;
        EXPECT_TRUE(guide) << "the alignment must be reported so it is drawn";
        r.hover({30.5f, 22.0f});                          // well outside the band: free
        EXPECT_NEAR(r.sketch.getPoint(pid)->pos.x, 30.5f, 1e-3);
        r.tool.onMouseUp({0, 0});
    }
    {   // single point, Reduced: no standalone alignment
        Rig r(SketchToolMode::Select);
        build(r);
        const int pid = r.sketch.addPoint({20.0f, 20.0f});
        r.tool.setInferenceLevel(SketchTool::InferenceLevel::Reduced);
        r.hover({20.0f, 20.0f});
        r.tool.onMouseDown({20.0f, 20.0f});
        r.hover({25.0f, 21.0f});
        r.hover({30.07f, 22.0f});
        EXPECT_NEAR(r.sketch.getPoint(pid)->pos.x, 30.07f, 1e-3);
        r.tool.onMouseUp({0, 0});
    }
    {   // a line drag passing the same spot: rigid, never aligned
        Rig r(SketchToolMode::Select);
        build(r);
        const int a = r.sketch.addPoint({20.0f, 20.0f}), b = r.sketch.addPoint({24.0f, 20.0f});
        r.sketch.addLine(a, b);
        r.tool.setInferenceLevel(SketchTool::InferenceLevel::Full);
        r.hover({22.0f, 20.0f});
        r.tool.onMouseDown({22.0f, 20.0f});
        r.hover({27.0f, 21.0f});
        r.hover({32.07f, 22.0f});                          // b's X would be 34.07; a's 30.07
        EXPECT_NEAR(r.sketch.getPoint(a)->pos.x, 30.07f, 1e-3);
        r.tool.onMouseUp({0, 0});
    }
}
