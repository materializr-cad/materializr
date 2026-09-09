// A value you TYPE in a sketch beats the snap grid.
//
// Two defects, reported together:
//
//   1. Typed values were re-rounded to the lattice. Worst on circles:
//      handleCircleTool ran snapRadialToGrid on whatever it was handed,
//      including the exact position the typed-value path computed. Typing a
//      7.3mm diameter on a 1mm grid built r=4.0 - and the same path then
//      recorded a Radius CONSTRAINT of 3.65 against it, so the circle
//      disagreed with its own constraint from the moment it existed.
//
//   2. Rectangle-from-centre doubled a typed width. applyDimension halves the
//      typed side in Center mode (m_firstClick is the centre and
//      handleRectangleTool mirrors the corner through it), but the two places
//      that RE-APPLY the locked width - onMouseDown and onMouseMove - did not.
//      Typing the width and then CLICKING for the height spanned the full
//      typed value either side of the centre. Typing BOTH values was correct,
//      which is why it survived: the bug needed one typed side and one clicked.
//
// The grid must still apply to everything the user did NOT type - that is what
// the last two cases guard, so a future "just turn the snapping off" fix can't
// pass this file.
#include <gtest/gtest.h>

#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SketchSolver.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <glm/glm.hpp>
#include <cmath>
#include <string>

// SketchTool's Text / SVG stamp paths live in the app, not materializr_core.
namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using materializr::ConstraintType;
using materializr::Sketch;
using materializr::SketchSolver;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {

constexpr float kStep = 1.0f;

struct Rig {
    Sketch sketch;
    SketchSolver solver;
    SketchTool tool;
    explicit Rig(SketchToolMode mode) {
        tool.setSketch(&sketch);
        tool.setSolver(&solver);
        tool.setGridStep(kStep);
        tool.setSnapToGridEnabled(true);
        tool.setMode(mode);
    }
    // The app streams mouse-moves, so the tool's cursor already tracks the
    // pointer when a press lands. Without priming, the first move computes a
    // delta from the tool's initial position.
    void press(glm::vec2 p) { tool.onMouseMove(p); tool.onMouseDown(p); }
};

void bounds(const Sketch& s, float& w, float& h, float& xmin, float& xmax) {
    xmin = 1e30f; xmax = -1e30f;
    float ymin = 1e30f, ymax = -1e30f;
    for (const auto& p : s.getPoints()) {
        xmin = std::min(xmin, p.pos.x); xmax = std::max(xmax, p.pos.x);
        ymin = std::min(ymin, p.pos.y); ymax = std::max(ymax, p.pos.y);
    }
    w = xmax - xmin;
    h = ymax - ymin;
}

} // namespace

TEST(TypedSketchValues, CircleTypedDiameterBeatsTheGrid) {
    Rig r(SketchToolMode::Circle);
    r.press(glm::vec2(0.0f, 0.0f));
    r.tool.onMouseMove(glm::vec2(5.0f, 0.0f));   // aim
    ASSERT_TRUE(r.tool.applyDimension(7.3f));    // diameter, off the 1mm lattice

    ASSERT_EQ(r.sketch.getCircles().size(), 1u);
    const double radius = r.sketch.getCircles().front().radius;
    EXPECT_NEAR(radius, 3.65, 1e-4);

    // And the constraint the same path records must describe the circle that
    // was actually built - these disagreed before (r=4.0 vs a Radius of 3.65).
    bool sawRadius = false;
    for (const auto& c : r.sketch.getConstraints())
        if (c.type == ConstraintType::Radius) {
            sawRadius = true;
            EXPECT_NEAR(c.value, radius, 1e-4);
        }
    EXPECT_TRUE(sawRadius);
}

TEST(TypedSketchValues, RectFromCentreDoesNotDoubleATypedWidth) {
    Rig r(SketchToolMode::Rectangle);
    r.tool.setRectMode(SketchTool::RectMode::Center);
    r.press(glm::vec2(0.0f, 0.0f));              // centre
    r.tool.onMouseMove(glm::vec2(4.0f, 3.0f));   // pick the quadrant
    ASSERT_TRUE(r.tool.applyDimension(6.0f));    // type the width
    r.tool.onMouseMove(glm::vec2(4.0f, 3.0f));   // a move re-applies the lock
    r.tool.onMouseDown(glm::vec2(4.0f, 3.0f));   // CLICK the height

    float w, h, xmin, xmax;
    bounds(r.sketch, w, h, xmin, xmax);
    EXPECT_NEAR(w, 6.0f, 1e-3);
    // Centred on the first click, not running the full width off one side.
    EXPECT_NEAR(xmin, -3.0f, 1e-3);
    EXPECT_NEAR(xmax,  3.0f, 1e-3);
}

TEST(TypedSketchValues, RectBothSidesTypedBeatTheGrid) {
    for (auto mode : {SketchTool::RectMode::Center, SketchTool::RectMode::Corner}) {
        Rig r(SketchToolMode::Rectangle);
        r.tool.setRectMode(mode);
        r.press(glm::vec2(0.0f, 0.0f));
        r.tool.onMouseMove(glm::vec2(4.0f, 3.0f));
        ASSERT_TRUE(r.tool.applyDimension(6.3f));
        ASSERT_TRUE(r.tool.applyDimension(4.7f));

        float w, h, xmin, xmax;
        bounds(r.sketch, w, h, xmin, xmax);
        EXPECT_NEAR(w, 6.3f, 1e-3);
        EXPECT_NEAR(h, 4.7f, 1e-3);
    }
}

TEST(TypedSketchValues, AClickedCircleStillSnapsToTheGrid) {
    // The fix is scoped to TYPED values. Turning the grid off for clicks too
    // would pass every test above and be wrong.
    Rig r(SketchToolMode::Circle);
    r.press(glm::vec2(0.0f, 0.0f));
    r.tool.onMouseMove(glm::vec2(3.4f, 0.0f));
    r.tool.onMouseDown(glm::vec2(3.4f, 0.0f));

    ASSERT_EQ(r.sketch.getCircles().size(), 1u);
    EXPECT_NEAR(r.sketch.getCircles().front().radius, 3.0, 1e-4);
}

TEST(TypedSketchValues, AClickedRectangleStillSnapsToTheGrid) {
    Rig r(SketchToolMode::Rectangle);
    r.tool.setRectMode(SketchTool::RectMode::Corner);
    r.press(glm::vec2(0.0f, 0.0f));
    r.tool.onMouseMove(glm::vec2(6.4f, 4.6f));
    r.tool.onMouseDown(glm::vec2(6.4f, 4.6f));

    float w, h, xmin, xmax;
    bounds(r.sketch, w, h, xmin, xmax);
    EXPECT_NEAR(w, 6.0f, 1e-3);
    EXPECT_NEAR(h, 5.0f, 1e-3);
}
