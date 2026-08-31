// Sketch Offset — geometry contract.
//
// Every refusal case here was MEASURED against OCCT 7.9.3 during spec work, not
// guessed (docs/specs/spec-sketch-offset/occt-offset-findings.md). Several of
// them SUCCEED at the OCCT level while producing meaningless geometry, which is
// exactly why they need tests: a naive implementation ships confident garbage.
//
//   - bow-tie source      -> MakeOffset returns 1 wire, 6 edges, IsValid()==true
//   - pinching neck       -> returns TWO wires from one Perform()
//   - collapse            -> IsDone()==true with an EMPTY shape
//   - circle offset >= r  -> IsDone()==false (a different failure mode)
//   - offset of 0 / 1e-9  -> succeeds, returning geometry on top of the source

#include "modeling/Sketch.h"
#include "modeling/SketchOffset.h"

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <vector>

using namespace materializr;

namespace {

// --- fixture builders -------------------------------------------------------

// Closed polygon through `pts`, as lines. Returns the line ids.
std::set<int> addPolyline(Sketch& sk, const std::vector<glm::vec2>& pts) {
    std::set<int> ids;
    std::vector<int> p;
    for (auto v : pts) p.push_back(sk.addPoint(v));
    for (size_t i = 0; i < p.size(); ++i)
        ids.insert(sk.addLine(p[i], p[(i + 1) % p.size()]));
    return ids;
}

// Same, but every junction gets TWO distinct coincident points — the shape an
// imported loop has. Exercises the weldTol fallback stage of adjacency.
std::set<int> addPolylineUnwelded(Sketch& sk, const std::vector<glm::vec2>& pts) {
    std::set<int> ids;
    for (size_t i = 0; i < pts.size(); ++i) {
        const int a = sk.addPoint(pts[i]);
        const int b = sk.addPoint(pts[(i + 1) % pts.size()]);
        ids.insert(sk.addLine(a, b));
    }
    return ids;
}

std::vector<glm::vec2> rect(float w, float h) {
    return {{0, 0}, {w, 0}, {w, h}, {0, h}};
}

// Two 15-wide lobes joined by a 4-wide neck; an inward offset past 2 splits it.
std::vector<glm::vec2> dumbbell() {
    return {{0,0},{15,0},{15,8},{35,8},{35,0},{50,0},
            {50,20},{35,20},{35,12},{15,12},{15,20},{0,20}};
}

// Graph-closed, but crossing itself.
std::vector<glm::vec2> bowTie() {
    return {{0,0},{40,40},{40,0},{0,40}};
}

OffsetSource srcOfLines(std::set<int> lines) {
    OffsetSource s; s.lineIds = std::move(lines); return s;
}

double totalTurn(const OffsetPlan& p) { return double(p.size()); }

} // namespace

// --- source validation ------------------------------------------------------

TEST(SketchOffsetValidate, ClosedRectangleIsAccepted) {
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    EXPECT_EQ(validateSource(sk, s), OffsetError::None);
}

TEST(SketchOffsetValidate, OpenChainIsRefused) {
    Sketch sk;
    std::vector<int> p;
    for (auto v : {glm::vec2{0,0}, glm::vec2{10,0}, glm::vec2{10,10}})
        p.push_back(sk.addPoint(v));
    OffsetSource s;
    s.lineIds.insert(sk.addLine(p[0], p[1]));
    s.lineIds.insert(sk.addLine(p[1], p[2]));
    EXPECT_EQ(validateSource(sk, s), OffsetError::OpenChain);
}

TEST(SketchOffsetValidate, BranchingIsRefused) {
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    // A spur off one existing corner gives it degree 3.
    const int corner = sk.getLines().front().startPointId;
    const int tip = sk.addPoint({-10, -10});
    s.lineIds.insert(sk.addLine(corner, tip));
    EXPECT_EQ(validateSource(sk, s), OffsetError::Branching);
}

TEST(SketchOffsetValidate, TwoDisjointLoopsAreRefused) {
    Sketch sk;
    auto a = addPolyline(sk, rect(10, 10));
    auto b = addPolyline(sk, {{100,100},{110,100},{110,110},{100,110}});
    OffsetSource s;
    s.lineIds = a;
    s.lineIds.insert(b.begin(), b.end());
    EXPECT_EQ(validateSource(sk, s), OffsetError::Disconnected);
}

TEST(SketchOffsetValidate, BowTieIsRefused) {
    // THE case that motivates result-independent validation: OCCT offsets this
    // happily, returning one wire that passes BRepCheck_Analyzer. Nothing
    // downstream can tell it from good output, so it must be caught up front.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, bowTie()));
    EXPECT_EQ(validateSource(sk, s), OffsetError::SelfIntersecting);
}

TEST(SketchOffsetValidate, ImportedStyleLoopWithUnweldedVerticesIsAccepted) {
    // Geometrically closed, but every junction holds two DISTINCT coincident
    // point ids. Under id-equality adjacency alone this reads as four separate
    // degree-1 chains and is wrongly refused as open.
    Sketch sk;
    auto s = srcOfLines(addPolylineUnwelded(sk, rect(40, 20)));
    EXPECT_EQ(validateSource(sk, s), OffsetError::None);
}

TEST(SketchOffsetValidate, LoneCircleIsAClosedLoop) {
    Sketch sk;
    OffsetSource s;
    s.circleIds.insert(sk.addCircle(sk.addPoint({0, 0}), 10.0));
    EXPECT_EQ(validateSource(sk, s), OffsetError::None);
}

TEST(SketchOffsetValidate, EmptySelectionIsRefused) {
    Sketch sk;
    EXPECT_EQ(validateSource(sk, OffsetSource{}), OffsetError::EmptySelection);
}

TEST(SketchOffsetValidate, ZeroLengthEntityIsRefused) {
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    // Duplicate a corner and bridge the two with a zero-length line. That
    // corner keeps degree 2 in the welded graph, so only the degeneracy
    // check can reject this.
    const int a = sk.addPoint({40, 20});
    const int b = sk.addPoint({40, 20});
    s.lineIds.insert(sk.addLine(a, b));
    const auto err = validateSource(sk, s);
    EXPECT_TRUE(err == OffsetError::DegenerateEntity || err == OffsetError::Branching)
        << "expected a degeneracy or topology refusal, got " << int(err);
}

// --- tolerance --------------------------------------------------------------

TEST(SketchOffsetTolerance, WeldToleranceGrowsWithMagnitude) {
    // The whole reason positional predicates are scale-aware: a fixed 1e-6 mm
    // is below a float ULP past ~10 mm from the origin.
    EXPECT_LT(weldTol({0.0f, 0.0f}), weldTol({1000.0f, 1000.0f}));
    EXPECT_GT(weldTol({10000.0f, 0.0f}), 1e-6);
}

TEST(SketchOffsetTolerance, SubToleranceDistanceIsRefusedBeforeOcct) {
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    OffsetPlan plan;
    // 0 and 1e-9 both SUCCEED in OCCT, returning geometry coincident with the
    // source. They have to be stopped before the call.
    EXPECT_EQ(computeOffsetPlan(sk, s, 0.0, plan), OffsetError::DistanceTooSmall);
    EXPECT_EQ(computeOffsetPlan(sk, s, 1e-9, plan), OffsetError::DistanceTooSmall);
    EXPECT_TRUE(plan.empty());
}

TEST(SketchOffsetTolerance, NonFiniteDistanceIsRefused) {
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    OffsetPlan plan;
    EXPECT_EQ(computeOffsetPlan(sk, s, std::nan(""), plan), OffsetError::DistanceTooSmall);
    EXPECT_EQ(computeOffsetPlan(sk, s, INFINITY, plan), OffsetError::DistanceTooSmall);
    EXPECT_TRUE(plan.empty());
}

// --- offsetting -------------------------------------------------------------

TEST(SketchOffset, OutwardRectangleProducesLinesAndJoinFillets) {
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    OffsetPlan plan;
    ASSERT_EQ(computeOffsetPlan(sk, s, 3.0, plan), OffsetError::None);

    EXPECT_EQ(plan.lines.size(), 4u) << "one parallel line per source side";
    EXPECT_EQ(plan.arcs.size(), 4u) << "a join fillet at each sharp corner";
    EXPECT_TRUE(plan.circles.empty());

    // A join fillet has radius exactly |d| — it has no source arc to derive
    // from, which is why CAP-2 describes two arc populations.
    for (const auto& a : plan.arcs) EXPECT_NEAR(a.radius, 3.0, 1e-6);
}

TEST(SketchOffset, InwardRectangleHasNoJoinFillets) {
    // Inward, the corners are concave: the offset sides simply meet, so no
    // fillet is inserted. A test that demanded 4 arcs both ways would be wrong.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    OffsetPlan plan;
    ASSERT_EQ(computeOffsetPlan(sk, s, -3.0, plan), OffsetError::None);
    EXPECT_EQ(plan.lines.size(), 4u);
    EXPECT_TRUE(plan.arcs.empty());
}

TEST(SketchOffset, LinesLandAtExactlyTheOffsetDistance) {
    // The analytic invariant CAP-2 actually asserts, replacing the "sample
    // every point" criterion that could not have decided anything.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    OffsetPlan plan;
    ASSERT_EQ(computeOffsetPlan(sk, s, -3.0, plan), OffsetError::None);

    // Inward by 3 turns 40x20 into 34x14 — every line sits 3 from its source.
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for (const auto& l : plan.lines) {
        for (auto v : {l.a, l.b}) {
            minX = std::min(minX, double(v.x)); maxX = std::max(maxX, double(v.x));
            minY = std::min(minY, double(v.y)); maxY = std::max(maxY, double(v.y));
        }
    }
    EXPECT_NEAR(minX,  3.0, 1e-6);
    EXPECT_NEAR(maxX, 37.0, 1e-6);
    EXPECT_NEAR(minY,  3.0, 1e-6);
    EXPECT_NEAR(maxY, 17.0, 1e-6);
}

TEST(SketchOffset, CircleOffsetsConcentrically) {
    Sketch sk;
    OffsetSource s;
    s.circleIds.insert(sk.addCircle(sk.addPoint({5, 5}), 10.0));

    OffsetPlan out, in;
    ASSERT_EQ(computeOffsetPlan(sk, s, 3.0, out), OffsetError::None);
    ASSERT_EQ(computeOffsetPlan(sk, s, -3.0, in), OffsetError::None);
    ASSERT_EQ(out.circles.size(), 1u);
    ASSERT_EQ(in.circles.size(), 1u);
    EXPECT_NEAR(out.circles[0].radius, 13.0, 1e-6);
    EXPECT_NEAR(in.circles[0].radius, 7.0, 1e-6);
    EXPECT_NEAR(out.circles[0].center.x, 5.0, 1e-6);
    EXPECT_NEAR(out.circles[0].center.y, 5.0, 1e-6);
}

TEST(SketchOffset, CircleOffsetAtOrBeyondRadiusIsRefused) {
    // IsDone() == false here — a DIFFERENT failure mode from the collapse
    // below, and one an implementation checking only for an empty shape misses.
    Sketch sk;
    OffsetSource s;
    s.circleIds.insert(sk.addCircle(sk.addPoint({0, 0}), 10.0));
    OffsetPlan plan;
    EXPECT_NE(computeOffsetPlan(sk, s, -10.0, plan), OffsetError::None);
    EXPECT_NE(computeOffsetPlan(sk, s, -12.0, plan), OffsetError::None);
    EXPECT_TRUE(plan.empty());
}

TEST(SketchOffset, CollapsingOffsetIsRefusedNotSilentlyEmpty) {
    // IsDone() == true, shape non-null, ZERO edges. SvgImport's guard
    // (!IsDone() || Shape().IsNull()) passes straight through this.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    OffsetPlan plan;
    EXPECT_EQ(computeOffsetPlan(sk, s, -11.0, plan), OffsetError::OffsetCollapsed);
    EXPECT_TRUE(plan.empty());
}

TEST(SketchOffset, SplittingOffsetIsRefused) {
    // One Perform() returning TWO wires. Counting edges cannot detect this —
    // the first spike counted edges and was structurally blind to it.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, dumbbell()));
    OffsetPlan plan;
    EXPECT_EQ(computeOffsetPlan(sk, s, -3.0, plan), OffsetError::OffsetSplit);
    EXPECT_TRUE(plan.empty());
}

TEST(SketchOffset, NonSplittingInwardOffsetOnTheSameShapeSucceeds) {
    // Discriminates the split test above: at -1 the neck survives, so a
    // blanket "dumbbells are refused" implementation would fail here.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, dumbbell()));
    OffsetPlan plan;
    EXPECT_EQ(computeOffsetPlan(sk, s, -1.0, plan), OffsetError::None);
    EXPECT_FALSE(plan.empty());
}

// --- direction --------------------------------------------------------------

TEST(SketchOffsetDirection, SignIsNegativeInsideAndPositiveOutside) {
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    EXPECT_LT(signedDistanceToLoop(sk, s, {20, 10}), 0.0) << "centre is inside";
    EXPECT_GT(signedDistanceToLoop(sk, s, {100, 10}), 0.0) << "far right is outside";
    EXPECT_NEAR(std::abs(signedDistanceToLoop(sk, s, {50, 10})), 10.0, 1e-5);
}

TEST(SketchOffsetDirection, CircleCentreResolvesOutwardRatherThanUndefined) {
    // The nearest point is non-unique at the centre. It must still return
    // deterministically rather than depending on which edge won a tie.
    Sketch sk;
    OffsetSource s;
    s.circleIds.insert(sk.addCircle(sk.addPoint({0, 0}), 10.0));
    const double d1 = signedDistanceToLoop(sk, s, {0, 0});
    const double d2 = signedDistanceToLoop(sk, s, {0, 0});
    EXPECT_EQ(d1, d2) << "must be deterministic";
    EXPECT_NEAR(std::abs(d1), 10.0, 1e-5);
}

// --- applying ---------------------------------------------------------------

TEST(SketchOffsetApply, PlanBecomesRealEditableEntities) {
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    const size_t linesBefore = sk.getLines().size();
    const size_t arcsBefore  = sk.getArcs().size();

    OffsetPlan plan;
    ASSERT_EQ(computeOffsetPlan(sk, s, 3.0, plan), OffsetError::None);
    applyOffsetPlan(sk, plan);

    EXPECT_EQ(sk.getLines().size(), linesBefore + plan.lines.size());
    EXPECT_EQ(sk.getArcs().size(),  arcsBefore  + plan.arcs.size());
}

TEST(SketchOffsetApply, SmallOffsetDoesNotWeldOntoItsOwnSource) {
    // findCoincidentPoint welds within 0.3*snapScale() — a UI snap radius. A
    // valid 0.1 mm offset routed through it would collapse onto the source.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    OffsetPlan plan;
    ASSERT_EQ(computeOffsetPlan(sk, s, 0.1, plan), OffsetError::None);
    applyOffsetPlan(sk, plan);

    // The offset rectangle's corner must be at (-0.1,-0.1)-ish, NOT welded to
    // the source corner at (0,0).
    bool foundDistinct = false;
    for (const auto& p : sk.getPoints()) {
        const double d = std::hypot(double(p.pos.x), double(p.pos.y));
        if (d > 1e-4 && d < 0.5) { foundDistinct = true; break; }
    }
    EXPECT_TRUE(foundDistinct)
        << "offset output welded onto the source — the snap radius leaked in";
}

TEST(SketchOffsetApply, RefusedOffsetLeavesTheSketchUntouched) {
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    const size_t pts = sk.getPoints().size();
    const size_t lns = sk.getLines().size();

    OffsetPlan plan;
    ASSERT_NE(computeOffsetPlan(sk, s, -11.0, plan), OffsetError::None);
    applyOffsetPlan(sk, plan);   // empty plan — must be a no-op

    EXPECT_EQ(sk.getPoints().size(), pts);
    EXPECT_EQ(sk.getLines().size(), lns);
}

TEST(SketchOffsetApply, OffsetOutputSurvivesAndFormsARegion) {
    // CAP-3's real criterion: emitting entity types proves nothing. The ring
    // between source and offset must actually build as a region, or the output
    // is not usable for the extrude that motivates the whole feature.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    OffsetPlan plan;
    ASSERT_EQ(computeOffsetPlan(sk, s, -3.0, plan), OffsetError::None);
    applyOffsetPlan(sk, plan);

    const auto regions = sk.buildRegions();
    EXPECT_GE(regions.size(), 1u) << "offset output did not form a usable region";
}

// --- arc winding and welding ------------------------------------------------
// Both of these exist because mutation testing showed the earlier suite could
// not detect their fixes being removed: reverting the REVERSED-edge endpoint
// swap, and reverting result-to-result welding, both left every test green.

TEST(SketchOffset, JoinFilletsAreMinorArcsNotTheirComplement) {
    // Sketch arcs are start->end CCW, while an OCCT edge carries its own
    // orientation. Ignoring TopAbs_REVERSED keeps the same two endpoints but
    // flips which way round the circle the arc sweeps, turning each 90-degree
    // corner fillet into a 270-degree one. Endpoints alone cannot catch that;
    // the swept angle can.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    OffsetPlan plan;
    ASSERT_EQ(computeOffsetPlan(sk, s, 3.0, plan), OffsetError::None);
    ASSERT_EQ(plan.arcs.size(), 4u);

    for (const auto& a : plan.arcs) {
        const glm::vec2 u = a.start - a.center;
        const glm::vec2 v = a.end   - a.center;
        double sweep = std::atan2(double(v.y), double(v.x)) -
                       std::atan2(double(u.y), double(u.x));
        while (sweep < 0.0)          sweep += 2.0 * M_PI;
        while (sweep >= 2.0 * M_PI)  sweep -= 2.0 * M_PI;
        // A rectangle's outward corner fillet sweeps a quarter turn.
        EXPECT_NEAR(sweep, M_PI / 2.0, 1e-4)
            << "arc swept " << sweep << " rad — endpoints were not orientation-corrected";
    }
}

TEST(SketchOffsetApply, ResultEndpointsAreWeldedToEachOther) {
    // The offset of a rectangle is 8 entities in a closed chain: 4 lines and 4
    // fillets, sharing 8 junctions. Welding result-to-result means 8 endpoint
    // points (+4 arc centres). Without it every entity brings its own pair and
    // the count nearly doubles — and the chain, though it looks right, is not
    // topologically closed, so regions and extrude break downstream.
    Sketch sk;
    auto s = srcOfLines(addPolyline(sk, rect(40, 20)));
    const size_t before = sk.getPoints().size();

    OffsetPlan plan;
    ASSERT_EQ(computeOffsetPlan(sk, s, 3.0, plan), OffsetError::None);
    ASSERT_EQ(plan.lines.size(), 4u);
    ASSERT_EQ(plan.arcs.size(), 4u);
    applyOffsetPlan(sk, plan);

    const size_t added = sk.getPoints().size() - before;
    // 8 shared junctions + 4 arc centres = 12. Unwelded would be 16 + 4 = 20.
    EXPECT_LE(added, 12u) << "result endpoints were not welded to each other";
}

// --- messages ---------------------------------------------------------------

TEST(SketchOffsetMessages, EveryErrorHasItsOwnMessage) {
    // CAP-4/CAP-5 require DISTINCT messages: "offset failed" is useless when
    // the fix differs per cause.
    const OffsetError all[] = {
        OffsetError::EmptySelection, OffsetError::UnsupportedEntity,
        OffsetError::OpenChain, OffsetError::Branching,
        OffsetError::Disconnected, OffsetError::DegenerateEntity,
        OffsetError::SelfIntersecting, OffsetError::DistanceTooSmall,
        OffsetError::OffsetFailed, OffsetError::OffsetCollapsed,
        OffsetError::OffsetSplit, OffsetError::UnsupportedResult,
    };
    std::set<std::string> seen;
    for (auto e : all) {
        const char* m = offsetErrorMessage(e);
        ASSERT_NE(m, nullptr);
        EXPECT_GT(std::string(m).size(), 8u) << "message too terse to act on";
        EXPECT_TRUE(seen.insert(m).second) << "duplicate message: " << m;
    }
    EXPECT_STRNE(offsetErrorMessage(OffsetError::None), nullptr);
}
