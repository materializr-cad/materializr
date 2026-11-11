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
