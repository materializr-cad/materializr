// Lines drawn on top of one another are equally near any click, so Select
// always landed on whichever was created first. Among equally-near lines a
// construction line now wins (it is the one most often moved or deleted), and
// clicking the same spot again walks on to the next one.

#include <gtest/gtest.h>

#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <glm/glm.hpp>

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
    Sketch sk;
    SketchSolver solver;
    SketchTool tool;
    int normalId = -1, constructionId = -1;

    Rig() {
        // A normal line drawn first, a construction line laid over its middle.
        int a = sk.addPoint({0, 0}), b = sk.addPoint({20, 0});
        int c = sk.addPoint({5, 0}), d = sk.addPoint({15, 0});
        normalId = sk.addLine(a, b);
        constructionId = sk.addLine(c, d);
        sk.setElementConstruction(constructionId, true);
        tool.setSketch(&sk);
        tool.setSolver(&solver);
        tool.setGridStep(1.0f);
        tool.setMode(SketchToolMode::Select);
    }

    int click(glm::vec2 p) {
        tool.onMouseDown(p);
        tool.onMouseUp(p);
        const auto& sel = tool.getSelectedLines();
        return sel.size() == 1 ? *sel.begin() : -1;
    }
};

} // namespace

TEST(SketchPickOverlap, ConstructionLineWinsOverANormalLineUnderIt) {
    Rig r;
    EXPECT_EQ(r.click({10, 0}), r.constructionId);
}

TEST(SketchPickOverlap, RepeatClickAtTheSameSpotWalksToTheNextLine) {
    Rig r;
    EXPECT_EQ(r.click({10, 0}), r.constructionId);
    EXPECT_EQ(r.click({10, 0}), r.normalId);
    EXPECT_EQ(r.click({10, 0}), r.constructionId) << "and wraps round";
}

TEST(SketchPickOverlap, ClickingElsewhereStartsOverWithTheConstructionLine) {
    Rig r;
    EXPECT_EQ(r.click({10, 0}), r.constructionId);
    EXPECT_EQ(r.click({10, 0}), r.normalId);
    EXPECT_EQ(r.click({12, 0}), r.constructionId);
}

TEST(SketchPickOverlap, WhereOnlyOneLineIsNearThatLineIsPicked) {
    Rig r;
    EXPECT_EQ(r.click({2, 0}), r.normalId) << "the construction line starts at x=5";
    EXPECT_EQ(r.click({2, 0}), r.normalId) << "nothing to cycle to";
}
