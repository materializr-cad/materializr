// The Reference tool copies geometry that lies on the sketch plane (body edges,
// a cross-section, another sketch's lines and circles - the snap references
// gathered on entry) into the sketch as real elements.

#include <gtest/gtest.h>

#include "modeling/PlaneReferences.h"
#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using namespace materializr;

namespace {

struct Rig {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool;
    Rig() {
        sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
        // The top face of a 40 x 30 box sits on this plane.
        addBodyPlaneReferences(sk, {BRepPrimAPI_MakeBox(40, 30, 10).Shape()});
        tool.setSketch(&sk);
        tool.setSolver(&solver);
        tool.setGridStep(1.0f);
        tool.setMode(SketchToolMode::Reference);
    }
    void click(glm::vec2 p, bool ctrl = false) { tool.onMouseDown(p, ctrl); tool.onMouseUp(p); }
};

} // namespace

TEST(SketchReferenceTool, ClickCopiesJustTheEdgeUnderTheCursor) {
    Rig r;
    r.click({20, 0});                       // middle of the bottom edge
    ASSERT_EQ(r.sk.getLines().size(), 1u);
    const auto& l = r.sk.getLines()[0];
    const auto* a = r.sk.getPoint(l.startPointId);
    const auto* b = r.sk.getPoint(l.endPointId);
    ASSERT_TRUE(a && b);
    EXPECT_NEAR(std::abs(a->pos.x - b->pos.x), 40.0f, 1e-3f);
    EXPECT_NEAR(a->pos.y, 0.0f, 1e-3f);
}

TEST(SketchReferenceTool, CtrlClickTakesTheWholeConnectedOutline) {
    Rig r;
    r.click({20, 0}, /*ctrl=*/true);
    EXPECT_EQ(r.sk.getLines().size(), 4u);
    EXPECT_EQ(r.sk.getPoints().size(), 4u) << "corners are shared, not duplicated";
    EXPECT_FALSE(r.sk.buildRegions().empty()) << "the copied outline closes";
}

TEST(SketchReferenceTool, CopyingTheSameEdgeTwiceDoesNotDuplicateIt) {
    Rig r;
    r.click({20, 0});
    r.click({20, 0});
    EXPECT_EQ(r.sk.getLines().size(), 1u);
}

TEST(SketchReferenceTool, ClickingAwayFromAnyReferenceDoesNothing) {
    Rig r;
    r.click({20, 15});                      // middle of the face, no edge near
    EXPECT_TRUE(r.sk.getLines().empty());
    EXPECT_TRUE(r.sk.getPoints().empty());
}

TEST(SketchReferenceTool, HoverHighlightsWhatAClickWouldTake) {
    Rig r;
    r.tool.onMouseMove({20, 0.1f});
    ASSERT_EQ(r.tool.getReferenceHover().size(), 1u);
    r.tool.onMouseMove({20, 15});
    EXPECT_TRUE(r.tool.getReferenceHover().empty());
}

TEST(SketchReferenceTool, ACircleReferenceBecomesARealCircle) {
    Sketch other;
    other.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    other.addCircle(other.addPoint({10, 10}), 4.0);

    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    addSketchPlaneReferences(sk, {&other});
    SketchSolver solver;
    SketchTool tool;
    tool.setSketch(&sk); tool.setSolver(&solver); tool.setGridStep(1.0f);
    tool.setMode(SketchToolMode::Reference);

    tool.onMouseDown({14, 10}, false);      // on the rim
    tool.onMouseUp({14, 10});
    ASSERT_EQ(sk.getCircles().size(), 1u);
    EXPECT_NEAR(sk.getCircles()[0].radius, 4.0, 1e-4);
}
