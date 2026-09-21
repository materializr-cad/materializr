// Trim on a polygon edge must delete the whole polygon (documented behavior,
// SketchTool.cpp:2963-2964), not partial-trim the underlying SketchLine and
// leave the polygon's bookkeeping (and the renderer, which draws polygons
// purely from m_lines) corrupted.

#include <gtest/gtest.h>

#include "modeling/Sketch.h"
#include "modeling/SketchConstraints.h"
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

using materializr::Constraint;
using materializr::ConstraintType;
using materializr::Sketch;
using materializr::SketchPolygon;
using materializr::SketchSolver;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {
constexpr float kStep = 1.0f;
constexpr float kTol = 1e-4f;

// handleTrimTool/computeTrimHover are private; drive them the same way the
// app does, through the public Trim-mode dispatch in onMouseDown/onMouseMove
// (SketchTool.cpp:127-128, 231-232). Trim uses the raw, unsnapped cursor in
// both, so this is exactly equivalent to calling the private methods.
SketchTool makeTrimTool(Sketch& sk, SketchSolver& solver) {
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setSolver(&solver);
    tool.setGridStep(kStep);
    tool.setMode(SketchToolMode::Trim);
    return tool;
}

void clickTrim(SketchTool& tool, glm::vec2 pos) { tool.onMouseDown(pos); }
void hoverTrim(SketchTool& tool, glm::vec2 pos) { tool.onMouseMove(pos); }

const SketchPolygon* findPolygon(const Sketch& sk, int id) {
    for (const auto& p : sk.getPolygons()) if (p.id == id) return &p;
    return nullptr;
}

bool lineExists(const Sketch& sk, int id) {
    for (const auto& l : sk.getLines()) if (l.id == id) return true;
    return false;
}

// True if some line's endpoints match (p1,p2) in either direction, within kTol.
bool lineWithEndpointsExists(const Sketch& sk, glm::vec2 p1, glm::vec2 p2) {
    for (const auto& l : sk.getLines()) {
        const auto* a = sk.getPoint(l.startPointId);
        const auto* b = sk.getPoint(l.endPointId);
        if (!a || !b) continue;
        bool forward = glm::length(a->pos - p1) < kTol && glm::length(b->pos - p2) < kTol;
        bool reverse = glm::length(a->pos - p2) < kTol && glm::length(b->pos - p1) < kTol;
        if (forward || reverse) return true;
    }
    return false;
}
} // namespace

// Clicking Trim on a polygon edge must delete the whole polygon, cascade-
// remove exactly its own owned lines and a dangling constraint on one of
// them, and leave unrelated geometry (a separate standalone line sharing no
// points with the polygon) completely untouched.
TEST(SketchTrimPolygon, ClickOnEdgeDeletesWholePolygonCascadeOnly) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeTrimTool(sk, solver);

    int centerId = sk.addPoint({0.0f, 0.0f});
    int polyId = sk.addPolygon(centerId, /*radius=*/10.0, /*sides=*/6, /*rotationRad=*/0.0);
    ASSERT_GE(polyId, 0);

    const SketchPolygon* poly = findPolygon(sk, polyId);
    ASSERT_NE(poly, nullptr);
    std::vector<int> ownedLineIds = poly->lineIds;
    ASSERT_FALSE(ownedLineIds.empty());

    // A dangling constraint on one of the polygon's own edges must be pruned
    // along with it (Sketch::pruneOrphanPoints already drops any constraint
    // whose entity id is no longer valid; this proves the removeElement
    // cascade actually invalidates that id).
    Constraint horiz;
    horiz.id = 999001;
    horiz.type = ConstraintType::Horizontal;
    horiz.entityA = ownedLineIds[0];
    sk.addRawConstraint(horiz);
    ASSERT_EQ(sk.getConstraints().size(), 1u);

    // Unrelated geometry: a standalone line far away, sharing no points with
    // the polygon at all. Must survive completely unchanged.
    int farP1 = sk.addPoint({100.0f, 0.0f});
    int farP2 = sk.addPoint({110.0f, 0.0f});
    int farLineId = sk.addLine(farP1, farP2);

    const size_t linesBefore = sk.getLines().size();

    // Midpoint of the first edge.
    const auto* a = sk.getPoint(poly->vertexPointIds[0]);
    const auto* b = sk.getPoint(poly->vertexPointIds[1]);
    glm::vec2 mid = (a->pos + b->pos) * 0.5f;

    clickTrim(tool, mid);

    EXPECT_EQ(findPolygon(sk, polyId), nullptr);
    for (int lid : ownedLineIds) {
        EXPECT_FALSE(lineExists(sk, lid)) << "polygon edge line " << lid << " survived FullDelete";
    }
    // Exactly the polygon's own lines disappeared - nothing else.
    EXPECT_EQ(sk.getLines().size(), linesBefore - ownedLineIds.size());

    EXPECT_TRUE(sk.getConstraints().empty())
        << "constraint on a deleted polygon edge was not pruned";

    ASSERT_TRUE(lineExists(sk, farLineId));
    const auto* fp1 = sk.getPoint(farP1);
    const auto* fp2 = sk.getPoint(farP2);
    ASSERT_NE(fp1, nullptr);
    ASSERT_NE(fp2, nullptr);
    EXPECT_NEAR(fp1->pos.x, 100.0f, kTol);
    EXPECT_NEAR(fp2->pos.x, 110.0f, kTol);
}

// A standalone line (not part of any polygon) still partial-trims normally -
// guards against the pick-priority fix over-broadly excluding real lines.
// Asserts the exact surviving geometry, not just "something remains".
TEST(SketchTrimPolygon, StandaloneLineStillPartialTrimsExactly) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeTrimTool(sk, solver);

    glm::vec2 leftEnd{-10.0f, 0.0f};
    glm::vec2 origin{0.0f, 0.0f};
    glm::vec2 rightEnd{10.0f, 0.0f};
    int p1 = sk.addPoint(leftEnd);
    int p2 = sk.addPoint(rightEnd);
    int lineId = sk.addLine(p1, p2);

    // A crossing line at x=0 gives the trim an intersection to split at.
    glm::vec2 crossTop{0.0f, 10.0f}, crossBottom{0.0f, -10.0f};
    int p3 = sk.addPoint(crossBottom);
    int p4 = sk.addPoint(crossTop);
    int crossId = sk.addLine(p3, p4);

    clickTrim(tool, {-5.0f, 0.0f}); // left half of the horizontal line

    EXPECT_FALSE(lineExists(sk, lineId));
    // Exactly the right half must survive: origin -> rightEnd.
    EXPECT_TRUE(lineWithEndpointsExists(sk, origin, rightEnd))
        << "surviving segment is not exactly (0,0)-(10,0)";
    // The left half must NOT survive in any form.
    EXPECT_FALSE(lineWithEndpointsExists(sk, leftEnd, origin))
        << "left half of the trimmed line was not removed";
    // The crossing line is untouched.
    ASSERT_TRUE(lineExists(sk, crossId));
    EXPECT_TRUE(lineWithEndpointsExists(sk, crossBottom, crossTop));
}

// Hover preview on a polygon edge outlines the full closed loop (FullDelete
// preview), not just the one segment under the cursor.
TEST(SketchTrimPolygon, HoverOnEdgePreviewsWholeLoop) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeTrimTool(sk, solver);

    int centerId = sk.addPoint({0.0f, 0.0f});
    int polyId = sk.addPolygon(centerId, 10.0, 6, 0.0);
    const SketchPolygon* poly = findPolygon(sk, polyId);
    ASSERT_NE(poly, nullptr);

    const auto* a = sk.getPoint(poly->vertexPointIds[0]);
    const auto* b = sk.getPoint(poly->vertexPointIds[1]);
    glm::vec2 mid = (a->pos + b->pos) * 0.5f;

    hoverTrim(tool, mid);
    // FullDelete preview densifies every vertex plus a closing repeat of the
    // first (densifyTrimPreview's polygon branch, SketchTool.cpp:3447-3454).
    EXPECT_GT(tool.getTrimHoverPoints().size(), poly->vertexPointIds.size());
}
