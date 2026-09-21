// Select+Delete on a polygon's edge, vertex, or center must cascade-delete
// the whole polygon (Sketch::removeElement/removeElements), same as the Trim
// tool's already-shipped FullDelete behavior for a polygon edge
// (test_sketch_trim_polygon.cpp) - not leave SketchPolygon::lineIds/
// vertexPointIds pointing at ids that were quietly erased out from under it.
// Also covers the two Line-tool chain-backtracking regressions the fix
// exposed (dropLineChainTail deleting an unrelated polygon by its center, or
// picking a pre-existing polygon edge instead of a just-drawn duplicate).

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

using materializr::Sketch;
using materializr::SketchPolygon;
using materializr::SketchSolver;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {
constexpr float kStep = 1.0f;
constexpr float kTol = 1e-4f;

// handleSelectTool snaps the click position first (unlike Trim, which uses
// the raw cursor), so every polygon here uses sides=4, rotation=0, radius=10:
// vertices land at (+-10,0),(0,+-10), edge midpoints at (+-5,+-5), center at
// (0,0) - all already on the 1.0 lattice, so snap() can't move a click off
// target regardless of tolerance/threshold tuning elsewhere.
constexpr double kRadius = 10.0;
constexpr int kSides = 4;

SketchTool makeSelectTool(Sketch& sk, SketchSolver& solver) {
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setSolver(&solver);
    tool.setGridStep(kStep);
    tool.setMode(SketchToolMode::Select);
    return tool;
}

SketchTool makeLineTool(Sketch& sk, SketchSolver& solver) {
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setSolver(&solver);
    tool.setGridStep(kStep);
    tool.setMode(SketchToolMode::Line);
    return tool;
}

const SketchPolygon* findPolygon(const Sketch& sk, int id) {
    for (const auto& p : sk.getPolygons()) if (p.id == id) return &p;
    return nullptr;
}

bool lineExists(const Sketch& sk, int id) {
    for (const auto& l : sk.getLines()) if (l.id == id) return true;
    return false;
}

bool pointExists(const Sketch& sk, int id) { return sk.getPoint(id) != nullptr; }

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

// Combined selection -> deletion, mirroring exactly what the revised
// Application::deleteSelectedSketchElements does.
void selectAndDelete(Sketch& sk, SketchTool& tool) {
    std::vector<int> ids(tool.getSelectedPoints().begin(), tool.getSelectedPoints().end());
    const auto& lns = tool.getSelectedLines();
    ids.insert(ids.end(), lns.begin(), lns.end());
    sk.removeElements(ids);
    sk.pruneOrphanPoints();
}
} // namespace

TEST(SketchSelectDeletePolygon, SelectClickOnEdgeThenDeleteCascadesWholePolygon) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeSelectTool(sk, solver);

    int centerId = sk.addPoint({0.0f, 0.0f});
    int polyId = sk.addPolygon(centerId, kRadius, kSides, 0.0);
    const SketchPolygon* poly = findPolygon(sk, polyId);
    ASSERT_NE(poly, nullptr);
    std::vector<int> ownedLineIds = poly->lineIds;
    std::vector<int> ownedVertexIds = poly->vertexPointIds;

    int farP1 = sk.addPoint({100.0f, 0.0f});
    int farP2 = sk.addPoint({110.0f, 0.0f});
    int farLineId = sk.addLine(farP1, farP2);

    tool.onMouseDown({5.0f, 5.0f}); // edge midpoint, V0(10,0)-V1(0,10)
    ASSERT_EQ(tool.getSelectedLines().size(), 1u);
    EXPECT_NE(std::find(ownedLineIds.begin(), ownedLineIds.end(),
                        *tool.getSelectedLines().begin()),
              ownedLineIds.end());

    selectAndDelete(sk, tool);

    EXPECT_EQ(findPolygon(sk, polyId), nullptr);
    for (int lid : ownedLineIds) EXPECT_FALSE(lineExists(sk, lid));
    for (int vid : ownedVertexIds) EXPECT_FALSE(pointExists(sk, vid));
    EXPECT_FALSE(pointExists(sk, centerId));

    ASSERT_TRUE(lineExists(sk, farLineId));
    ASSERT_TRUE(pointExists(sk, farP1));
    ASSERT_TRUE(pointExists(sk, farP2));
}

TEST(SketchSelectDeletePolygon, SelectClickOnVertexThenDeleteCascadesWholePolygon) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeSelectTool(sk, solver);

    int centerId = sk.addPoint({0.0f, 0.0f});
    int polyId = sk.addPolygon(centerId, kRadius, kSides, 0.0);
    const SketchPolygon* poly = findPolygon(sk, polyId);
    ASSERT_NE(poly, nullptr);
    std::vector<int> ownedLineIds = poly->lineIds;
    std::vector<int> ownedVertexIds = poly->vertexPointIds;

    int farP1 = sk.addPoint({100.0f, 0.0f});
    int farP2 = sk.addPoint({110.0f, 0.0f});
    int farLineId = sk.addLine(farP1, farP2);

    tool.onMouseDown({10.0f, 0.0f}); // vertex V0
    ASSERT_EQ(tool.getSelectedPoints().size(), 1u);
    EXPECT_EQ(*tool.getSelectedPoints().begin(), ownedVertexIds[0]);

    selectAndDelete(sk, tool);

    EXPECT_EQ(findPolygon(sk, polyId), nullptr);
    for (int lid : ownedLineIds) EXPECT_FALSE(lineExists(sk, lid));
    for (int vid : ownedVertexIds) EXPECT_FALSE(pointExists(sk, vid));
    EXPECT_FALSE(pointExists(sk, centerId));

    ASSERT_TRUE(lineExists(sk, farLineId));
    ASSERT_TRUE(pointExists(sk, farP1));
    ASSERT_TRUE(pointExists(sk, farP2));
}

TEST(SketchSelectDeletePolygon, SelectClickOnCenterThenDeleteCascadesWholePolygon) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeSelectTool(sk, solver);

    int centerId = sk.addPoint({0.0f, 0.0f});
    int polyId = sk.addPolygon(centerId, kRadius, kSides, 0.0);
    const SketchPolygon* poly = findPolygon(sk, polyId);
    ASSERT_NE(poly, nullptr);
    std::vector<int> ownedLineIds = poly->lineIds;
    std::vector<int> ownedVertexIds = poly->vertexPointIds;

    int farP1 = sk.addPoint({100.0f, 0.0f});
    int farP2 = sk.addPoint({110.0f, 0.0f});
    int farLineId = sk.addLine(farP1, farP2);

    tool.onMouseDown({0.0f, 0.0f}); // center
    ASSERT_EQ(tool.getSelectedPoints().size(), 1u);
    EXPECT_EQ(*tool.getSelectedPoints().begin(), centerId);

    selectAndDelete(sk, tool);

    EXPECT_EQ(findPolygon(sk, polyId), nullptr);
    for (int lid : ownedLineIds) EXPECT_FALSE(lineExists(sk, lid));
    for (int vid : ownedVertexIds) EXPECT_FALSE(pointExists(sk, vid));
    EXPECT_FALSE(pointExists(sk, centerId));

    ASSERT_TRUE(lineExists(sk, farLineId));
    ASSERT_TRUE(pointExists(sk, farP1));
    ASSERT_TRUE(pointExists(sk, farP2));
}

// Guards against the new ownership scan over-triggering on plain geometry.
TEST(SketchSelectDeletePolygon, StandaloneGeometryStillDeletesNormally) {
    Sketch sk;

    int p1 = sk.addPoint({0.0f, 0.0f});
    int p2 = sk.addPoint({10.0f, 0.0f});
    int lineId = sk.addLine(p1, p2);
    int isolatedPt = sk.addPoint({50.0f, 50.0f});

    sk.removeElements({lineId, isolatedPt});
    sk.pruneOrphanPoints();

    EXPECT_FALSE(lineExists(sk, lineId));
    EXPECT_FALSE(pointExists(sk, isolatedPt));
    EXPECT_FALSE(pointExists(sk, p1)); // orphaned once the line is gone
    EXPECT_FALSE(pointExists(sk, p2));
}

// The polygon's center can be an existing, pre-shared point
// (handlePolygonTool welds onto one via findCoincidentPoint) - the cascade
// must not delete a point still needed by other, unselected geometry.
TEST(SketchSelectDeletePolygon, SharedCenterPointSurvivesPolygonCascade) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeSelectTool(sk, solver);

    int sharedCenter = sk.addPoint({0.0f, 0.0f});
    int lineFarEnd = sk.addPoint({60.0f, 0.0f});
    int standaloneLineId = sk.addLine(sharedCenter, lineFarEnd);

    int polyId = sk.addPolygon(sharedCenter, kRadius, kSides, 0.0);
    const SketchPolygon* poly = findPolygon(sk, polyId);
    ASSERT_NE(poly, nullptr);
    std::vector<int> ownedLineIds = poly->lineIds;

    tool.onMouseDown({5.0f, 5.0f}); // an edge
    ASSERT_EQ(tool.getSelectedLines().size(), 1u);

    selectAndDelete(sk, tool);

    EXPECT_EQ(findPolygon(sk, polyId), nullptr);
    for (int lid : ownedLineIds) EXPECT_FALSE(lineExists(sk, lid));

    ASSERT_TRUE(pointExists(sk, sharedCenter));
    ASSERT_TRUE(lineExists(sk, standaloneLineId));
    EXPECT_TRUE(lineWithEndpointsExists(sk, {0.0f, 0.0f}, {60.0f, 0.0f}));
}

// Round 2's finding: a bare points-before-lines reorder isn't enough when the
// SAME polygon has more than one independently-shared vertex in the batch -
// the first cascade removes the polygon record, so a later id from the same
// polygon must still resolve correctly. Proves Sketch::removeElements'
// whole-batch-up-front resolution, not just test 5's single-shared-point case.
TEST(SketchSelectDeletePolygon, RemoveElementsResolvesWholeBatchBeforeMutating) {
    Sketch sk;

    int centerId = sk.addPoint({0.0f, 0.0f});
    int polyId = sk.addPolygon(centerId, kRadius, kSides, 0.0);
    const SketchPolygon* poly = findPolygon(sk, polyId);
    ASSERT_NE(poly, nullptr);
    std::vector<int> vertexIds = poly->vertexPointIds; // V0,V1,V2,V3
    std::vector<int> ownedLineIds = poly->lineIds;
    int edgeV2V3 = ownedLineIds[2];

    int lineAEnd = sk.addPoint({20.0f, 0.0f});
    int lineAId = sk.addLine(vertexIds[0], lineAEnd); // shares V0
    int lineBEnd = sk.addPoint({0.0f, 20.0f});
    int lineBId = sk.addLine(vertexIds[1], lineBEnd); // shares V1

    std::vector<int> ids = {edgeV2V3, vertexIds[0], vertexIds[1]};
    sk.removeElements(ids);
    sk.pruneOrphanPoints();

    EXPECT_EQ(findPolygon(sk, polyId), nullptr);
    for (int lid : ownedLineIds) EXPECT_FALSE(lineExists(sk, lid));

    // Independently-shared vertices survive; the un-shared ones are pruned.
    ASSERT_TRUE(pointExists(sk, vertexIds[0]));
    ASSERT_TRUE(pointExists(sk, vertexIds[1]));
    EXPECT_FALSE(pointExists(sk, vertexIds[2]));
    EXPECT_FALSE(pointExists(sk, vertexIds[3]));
    EXPECT_FALSE(pointExists(sk, centerId));

    ASSERT_TRUE(lineExists(sk, lineAId));
    ASSERT_TRUE(lineExists(sk, lineBId));
}

// dropLineChainTail Gap A: a polygon's center is never a line endpoint by
// construction, so the tail point's "still referenced by a line" check alone
// can't see it - backtracking a chain that welded onto a pre-existing
// polygon's center must not delete that polygon.
TEST(SketchSelectDeletePolygon, BacktrackOntoPolygonCenterDoesNotDeletePolygon) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeLineTool(sk, solver);

    int centerId = sk.addPoint({0.0f, 0.0f});
    int polyId = sk.addPolygon(centerId, kRadius, kSides, 0.0);
    const SketchPolygon* poly = findPolygon(sk, polyId);
    ASSERT_NE(poly, nullptr);
    std::vector<int> ownedLineIds = poly->lineIds;
    std::vector<int> ownedVertexIds = poly->vertexPointIds;

    tool.onMouseDown({20.0f, 0.0f}); // chain start (new point)
    tool.onMouseDown({0.0f, 0.0f});  // welds onto the polygon's center
    ASSERT_TRUE(lineWithEndpointsExists(sk, {20.0f, 0.0f}, {0.0f, 0.0f}));

    ASSERT_TRUE(tool.dropLineChainTail());

    EXPECT_FALSE(lineWithEndpointsExists(sk, {20.0f, 0.0f}, {0.0f, 0.0f}));

    ASSERT_NE(findPolygon(sk, polyId), nullptr);
    for (int lid : ownedLineIds) EXPECT_TRUE(lineExists(sk, lid));
    for (int vid : ownedVertexIds) EXPECT_TRUE(pointExists(sk, vid));
    EXPECT_TRUE(pointExists(sk, centerId));
}

// Direct unit test of removeElement's cycle guard: a hand-corrupted polygon
// whose own lineIds contains its own id (as could reach a live sketch via
// ProjectIO/CombineSketchesOp, neither of which validates ids) must not
// stack-overflow, and must clean up the same as an uncorrupted polygon would.
TEST(SketchSelectDeletePolygon, RemoveElementToleratesSelfReferencingPolygonData) {
    Sketch sk;

    int centerId = sk.addPoint({0.0f, 0.0f});
    int v0 = sk.addPoint({10.0f, 0.0f});
    int v1 = sk.addPoint({0.0f, 10.0f});
    int v2 = sk.addPoint({-10.0f, 0.0f});
    int l0 = sk.addLine(v0, v1);
    int l1 = sk.addLine(v1, v2);
    int l2 = sk.addLine(v2, v0);

    SketchPolygon corrupt;
    corrupt.id = sk.getNextId();
    corrupt.centerPointId = centerId;
    corrupt.radius = kRadius;
    corrupt.sides = 3;
    corrupt.vertexPointIds = {v0, v1, v2};
    corrupt.lineIds = {l0, l1, l2, corrupt.id}; // self-reference
    sk.addRawPolygon(corrupt);
    sk.setNextId(corrupt.id + 1);

    sk.removeElement(corrupt.id); // must terminate, not stack-overflow

    EXPECT_EQ(findPolygon(sk, corrupt.id), nullptr);
    EXPECT_FALSE(lineExists(sk, l0));
    EXPECT_FALSE(lineExists(sk, l1));
    EXPECT_FALSE(lineExists(sk, l2));
}

// Two polygons can share one center point (the second addPolygon call reuses
// the first's centerPointId, mirroring handlePolygonTool's weld-onto-existing
// behavior). Deleting the shared point must take out EVERY owning polygon,
// not just the first found, while the point itself survives for other,
// unrelated geometry still using it.
TEST(SketchSelectDeletePolygon, SharedCenterAcrossTwoPolygonsDeletesBothLeavesLineIntact) {
    Sketch sk;

    int sharedCenter = sk.addPoint({0.0f, 0.0f});
    int lineFarEnd = sk.addPoint({80.0f, 0.0f});
    int standaloneLineId = sk.addLine(sharedCenter, lineFarEnd);

    int poly1Id = sk.addPolygon(sharedCenter, kRadius, kSides, 0.0);
    int poly2Id = sk.addPolygon(sharedCenter, kRadius, kSides, 0.0);
    const SketchPolygon* poly1 = findPolygon(sk, poly1Id);
    const SketchPolygon* poly2 = findPolygon(sk, poly2Id);
    ASSERT_NE(poly1, nullptr);
    ASSERT_NE(poly2, nullptr);
    std::vector<int> poly1Lines = poly1->lineIds;
    std::vector<int> poly2Lines = poly2->lineIds;

    sk.removeElements({sharedCenter});
    sk.pruneOrphanPoints();

    EXPECT_EQ(findPolygon(sk, poly1Id), nullptr);
    EXPECT_EQ(findPolygon(sk, poly2Id), nullptr);
    for (int lid : poly1Lines) EXPECT_FALSE(lineExists(sk, lid));
    for (int lid : poly2Lines) EXPECT_FALSE(lineExists(sk, lid));

    ASSERT_TRUE(pointExists(sk, sharedCenter));
    ASSERT_TRUE(lineExists(sk, standaloneLineId));
}

// dropLineChainTail Gap B: if the user's new segment traces exactly over an
// existing polygon edge (both ends welded onto that edge's existing
// vertices), backtracking must remove only the just-drawn duplicate - not
// re-derive "the" segment by an endpoint-pair scan that picks the OLDER
// (pre-existing polygon) line instead.
TEST(SketchSelectDeletePolygon, BacktrackOverExistingPolygonEdgeRemovesOnlyTheNewDuplicate) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeLineTool(sk, solver);

    int centerId = sk.addPoint({0.0f, 0.0f});
    int polyId = sk.addPolygon(centerId, kRadius, kSides, 0.0);
    const SketchPolygon* poly = findPolygon(sk, polyId);
    ASSERT_NE(poly, nullptr);
    std::vector<int> ownedLineIds = poly->lineIds;
    std::vector<int> ownedVertexIds = poly->vertexPointIds;
    int originalEdgeV0V1 = ownedLineIds[0];
    const size_t linesBefore = sk.getLines().size();

    tool.onMouseDown({10.0f, 0.0f}); // welds onto V0
    tool.onMouseDown({0.0f, 10.0f}); // welds onto V1 - duplicate of the existing edge
    EXPECT_EQ(sk.getLines().size(), linesBefore + 1);

    ASSERT_TRUE(tool.dropLineChainTail());

    EXPECT_EQ(sk.getLines().size(), linesBefore);
    ASSERT_NE(findPolygon(sk, polyId), nullptr);
    EXPECT_TRUE(lineExists(sk, originalEdgeV0V1));
    for (int lid : ownedLineIds) EXPECT_TRUE(lineExists(sk, lid));
    for (int vid : ownedVertexIds) EXPECT_TRUE(pointExists(sk, vid));
    EXPECT_TRUE(pointExists(sk, centerId));
}

// The parallel m_lineChain / m_lineChainSegmentIds vectors piece 3's fix
// relies on must pop the same, correct segment across repeated backtracks,
// and must fully clear together on cancel so a fresh chain never reaches
// back into a previous chain's already-gone segment ids.
TEST(SketchSelectDeletePolygon, LineChainSegmentIdsStaySyncedAcrossRepeatedBacktrackAndCancelRestart) {
    Sketch sk;
    SketchSolver solver;
    SketchTool tool = makeLineTool(sk, solver);

    tool.onMouseDown({0.0f, 0.0f});
    tool.onMouseDown({10.0f, 0.0f});
    tool.onMouseDown({20.0f, 0.0f});
    tool.onMouseDown({30.0f, 0.0f});
    tool.onMouseDown({40.0f, 0.0f});
    ASSERT_EQ(tool.lineSegmentCount(), 4);

    ASSERT_TRUE(tool.dropLineChainTail());
    EXPECT_EQ(tool.lineSegmentCount(), 3);
    EXPECT_FALSE(lineWithEndpointsExists(sk, {30.0f, 0.0f}, {40.0f, 0.0f}));
    EXPECT_TRUE(lineWithEndpointsExists(sk, {20.0f, 0.0f}, {30.0f, 0.0f}));

    ASSERT_TRUE(tool.dropLineChainTail());
    EXPECT_EQ(tool.lineSegmentCount(), 2);
    EXPECT_FALSE(lineWithEndpointsExists(sk, {20.0f, 0.0f}, {30.0f, 0.0f}));
    EXPECT_TRUE(lineWithEndpointsExists(sk, {10.0f, 0.0f}, {20.0f, 0.0f}));

    tool.onCancel(); // clears m_lineChain and m_lineChainSegmentIds together
    const size_t linesAfterCancel = sk.getLines().size();

    tool.onMouseDown({100.0f, 100.0f});
    tool.onMouseDown({110.0f, 100.0f});
    ASSERT_EQ(tool.lineSegmentCount(), 1);
    EXPECT_EQ(sk.getLines().size(), linesAfterCancel + 1);

    ASSERT_TRUE(tool.dropLineChainTail());

    // Only the fresh chain's own segment is gone - a desynced
    // m_lineChainSegmentIds would instead remove a stale id left from before
    // the cancel, corrupting the surviving pre-cancel geometry.
    EXPECT_EQ(sk.getLines().size(), linesAfterCancel);
    EXPECT_FALSE(lineWithEndpointsExists(sk, {100.0f, 100.0f}, {110.0f, 100.0f}));
    EXPECT_TRUE(lineWithEndpointsExists(sk, {10.0f, 0.0f}, {20.0f, 0.0f}));
    EXPECT_TRUE(lineWithEndpointsExists(sk, {0.0f, 0.0f}, {10.0f, 0.0f}));
}
