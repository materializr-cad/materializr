// Sketch Offset tool - geometry core.
//
// Task 1 coverage: the chain walk. Adjacency is by shared point id, the walk
// extends both ways while the shared endpoint has degree exactly 2, and stops
// at a branch, a free end, or a spline.
//
// Spec: docs/superpowers/specs/2026-08-31-sketch-offset-tool-design.md

#include "modeling/Sketch.h"
#include "modeling/SketchOffset.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>

using materializr::OffsetChain;
using materializr::OffsetSeg;
using materializr::Sketch;
using materializr::walkOffsetChain;

namespace {

// segs[i].p1 must meet segs[i+1].p0, and a closed chain must also close.
void expectContiguous(const OffsetChain& ch) {
    ASSERT_FALSE(ch.segs.empty());
    for (size_t i = 0; i + 1 < ch.segs.size(); ++i) {
        glm::vec2 d = ch.segs[i].p1 - ch.segs[i + 1].p0;
        EXPECT_NEAR(glm::length(d), 0.0f, 1e-4f)
            << "seg " << i << " does not meet seg " << (i + 1);
    }
    if (ch.closed) {
        glm::vec2 d = ch.segs.back().p1 - ch.segs.front().p0;
        EXPECT_NEAR(glm::length(d), 0.0f, 1e-4f) << "closed chain does not close";
    }
}

// Axis-aligned square, corners (0,0) (a,0) (a,a) (0,a), as four shared-vertex
// lines. Returns the corner point ids.
std::vector<int> makeSquare(Sketch& sk, float a) {
    std::vector<int> pts{
        sk.addPoint({0.0f, 0.0f}), sk.addPoint({a, 0.0f}),
        sk.addPoint({a, a}),       sk.addPoint({0.0f, a})};
    for (int i = 0; i < 4; ++i) sk.addLine(pts[i], pts[(i + 1) % 4]);
    return pts;
}

} // namespace

TEST(SketchOffsetWalk, ClosedSquareWalksAllFourSides) {
    Sketch sk;
    makeSquare(sk, 10.0f);

    // Click the middle of the bottom edge.
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);

    ASSERT_TRUE(ch.valid());
    EXPECT_TRUE(ch.closed);
    EXPECT_EQ(ch.segs.size(), 4u);
    expectContiguous(ch);
    for (const auto& s : ch.segs) EXPECT_EQ(s.kind, OffsetSeg::Kind::Line);
}

TEST(SketchOffsetWalk, LoneLineIsAnOpenChainOfOne) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    sk.addLine(a, b);

    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);

    ASSERT_TRUE(ch.valid());
    EXPECT_FALSE(ch.closed);
    ASSERT_EQ(ch.segs.size(), 1u);
    EXPECT_NEAR(ch.segs[0].length(), 10.0f, 1e-4f);
}

TEST(SketchOffsetWalk, OpenChainWalksBothWaysFromTheMiddle) {
    Sketch sk;
    // Three collinear segments; pick the MIDDLE one, expect all three.
    int p0 = sk.addPoint({0.0f, 0.0f});
    int p1 = sk.addPoint({10.0f, 0.0f});
    int p2 = sk.addPoint({20.0f, 0.0f});
    int p3 = sk.addPoint({30.0f, 0.0f});
    sk.addLine(p0, p1);
    sk.addLine(p1, p2);
    sk.addLine(p2, p3);

    OffsetChain ch = walkOffsetChain(sk, {15.0f, 0.0f}, 0.5f);

    ASSERT_TRUE(ch.valid());
    EXPECT_FALSE(ch.closed);
    EXPECT_EQ(ch.segs.size(), 3u);
    expectContiguous(ch);
    // Travel order runs end to end, whichever way round.
    float span = glm::length(ch.segs.back().p1 - ch.segs.front().p0);
    EXPECT_NEAR(span, 30.0f, 1e-4f);
}

TEST(SketchOffsetWalk, StopsAtATJunction) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int j = sk.addPoint({10.0f, 0.0f});
    int b = sk.addPoint({20.0f, 0.0f});
    int t = sk.addPoint({10.0f, 10.0f});
    sk.addLine(a, j);
    sk.addLine(j, b);
    sk.addLine(j, t); // the branch

    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);

    ASSERT_TRUE(ch.valid());
    EXPECT_FALSE(ch.closed);
    EXPECT_EQ(ch.segs.size(), 1u) << "degree-3 vertex must stop the walk";
}

TEST(SketchOffsetWalk, WalksThroughASplineJunction) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int j = sk.addPoint({10.0f, 0.0f});
    int c = sk.addPoint({20.0f, 5.0f});
    int d = sk.addPoint({30.0f, 0.0f});
    sk.addLine(a, j);
    sk.addSpline({j, c, d}); // shares the line's far endpoint

    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);

    ASSERT_TRUE(ch.valid());
    ASSERT_EQ(ch.segs.size(), 2u) << "a spline continues the chain";
    expectContiguous(ch);
    EXPECT_EQ(ch.segs[1].kind, OffsetSeg::Kind::Spline);
    EXPECT_GE(ch.segs[1].pts.size(), 3u) << "the curve is carried as samples";
}

TEST(SketchOffsetWalk, PickingASplineWalksIt) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 5.0f});
    int c = sk.addPoint({20.0f, 0.0f});
    sk.addSpline({a, b, c});

    // Click on the curve itself.
    OffsetChain ch = walkOffsetChain(sk, {10.0f, 5.0f}, 1.5f);
    ASSERT_TRUE(ch.valid());
    ASSERT_EQ(ch.segs.size(), 1u);
    EXPECT_EQ(ch.segs[0].kind, OffsetSeg::Kind::Spline);
    EXPECT_FALSE(ch.closed);
    // Travel runs end to end along the curve.
    EXPECT_NEAR(ch.segs[0].p0.x, 0.0f, 1e-3f);
    EXPECT_NEAR(ch.segs[0].p1.x, 20.0f, 1e-3f);
}

TEST(SketchOffsetWalk, SplineTraversedBackwardsReversesItsSamples) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 5.0f});
    int c = sk.addPoint({20.0f, 0.0f});
    int t = sk.addPoint({30.0f, 0.0f});
    sk.addSpline({a, b, c});
    sk.addLine(t, c); // stored t->c, so the walk enters the spline at its END

    OffsetChain ch = walkOffsetChain(sk, {25.0f, 0.0f}, 0.5f);

    ASSERT_EQ(ch.segs.size(), 2u);
    expectContiguous(ch);
    const OffsetSeg& sp = ch.segs[1];
    ASSERT_EQ(sp.kind, OffsetSeg::Kind::Spline);
    EXPECT_NEAR(sp.p0.x, 20.0f, 1e-3f) << "entered at the far end";
    EXPECT_NEAR(sp.p1.x, 0.0f, 1e-3f);
}

TEST(SketchOffsetWalk, CircleIsAClosedChainOfOne) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    sk.addCircle(c, 5.0);

    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);

    ASSERT_TRUE(ch.valid());
    EXPECT_TRUE(ch.closed);
    ASSERT_EQ(ch.segs.size(), 1u);
    EXPECT_EQ(ch.segs[0].kind, OffsetSeg::Kind::Circle);
    EXPECT_NEAR(ch.segs[0].r, 5.0f, 1e-4f);
}

TEST(SketchOffsetWalk, PolygonNeedsNoSpecialCase) {
    // addPolygon emits its edges as real SketchLine elements, so the ordinary
    // line walk picks it up as a closed loop.
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    sk.addPolygon(c, 10.0, 6, 0.0);

    OffsetChain ch = walkOffsetChain(sk, {10.0f, 0.0f}, 0.5f);

    ASSERT_TRUE(ch.valid());
    EXPECT_TRUE(ch.closed);
    EXPECT_EQ(ch.segs.size(), 6u);
    expectContiguous(ch);
}

TEST(SketchOffsetWalk, ArcTraversedBackwardsCarriesNegativeSweep) {
    Sketch sk;
    // Semicircle centred at the origin, r=5, sweeping CCW from (5,0) to
    // (-5,0), plus a line hanging off its END so the walk must enter the arc
    // from that side and traverse it backwards.
    int c  = sk.addPoint({0.0f, 0.0f});
    int s  = sk.addPoint({5.0f, 0.0f});
    int e  = sk.addPoint({-5.0f, 0.0f});
    int t  = sk.addPoint({-5.0f, -10.0f});
    sk.addArc(c, s, e, 5.0);
    // Stored t->e, so the walk starts at t, reaches the arc at its stored END
    // and must traverse it backwards. (The chain's overall travel direction is
    // arbitrary - it falls out of the start element's stored point order. Only
    // self-consistency matters; the offset SIDE comes from the cursor, not from
    // the direction the walk happened to take.)
    sk.addLine(t, e);

    OffsetChain ch = walkOffsetChain(sk, {-5.0f, -5.0f}, 0.5f);

    ASSERT_TRUE(ch.valid());
    ASSERT_EQ(ch.segs.size(), 2u);
    expectContiguous(ch);

    const OffsetSeg* arc = nullptr;
    for (const auto& sg : ch.segs)
        if (sg.kind == OffsetSeg::Kind::Arc) arc = &sg;
    ASSERT_NE(arc, nullptr);
    EXPECT_NEAR(std::abs(arc->sweep), static_cast<float>(M_PI), 1e-3f);
    // Entered at (-5,0), so travel runs clockwise: negative sweep.
    EXPECT_LT(arc->sweep, 0.0f);
    EXPECT_NEAR(arc->p0.x, -5.0f, 1e-4f);
    EXPECT_NEAR(arc->p1.x, 5.0f, 1e-4f);
}

TEST(SketchOffsetWalk, EmptySketchAndMissesReturnNothing) {
    Sketch sk;
    EXPECT_FALSE(walkOffsetChain(sk, {0.0f, 0.0f}, 0.5f).valid());

    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    sk.addLine(a, b);
    EXPECT_FALSE(walkOffsetChain(sk, {5.0f, 50.0f}, 0.5f).valid());
}

// ─── Task 2: offsetting + corner fix-up ─────────────────────────────────────

using materializr::offsetChain;
using materializr::OffsetCorners;
using materializr::OffsetResult;

namespace {

// Measured with the production distanceToChain, not a copy of it: a second
// implementation here silently skipped splines (treating them as radius-zero
// arcs) and reported metres of error against perfectly good geometry.
float distToChain(const OffsetChain& ch, glm::vec2 q) {
    return materializr::distanceToChain(ch, q);
}

// THE INVARIANT: every point of a valid offset lies at distance exactly |d|
// from the source chain. Every offset test asserts it.
void expectOnOffset(const OffsetResult& res, const OffsetChain& src, float d,
                    float tolOverride = 0.0f) {
    ASSERT_TRUE(res.valid);
    const float tol = (tolOverride > 0.0f) ? tolOverride
                                           : std::max(1e-3f, 2e-3f * std::abs(d));
    for (size_t i = 0; i < res.segs.size(); ++i) {
        for (int k = 0; k <= 16; ++k) {
            glm::vec2 q = res.segs[i].at(static_cast<float>(k) / 16.0f);
            EXPECT_NEAR(distToChain(src, q), std::abs(d), tol)
                << "seg " << i << " at t=" << (k / 16.0f);
        }
    }
}

// Signed area of a closed chain's densified outline. Positive = CCW, which is
// the winding for which a positive `d` offsets OUTWARD.
float signedArea(const OffsetChain& ch) {
    std::vector<std::vector<glm::vec2>> polys;
    materializr::densifyChain(ch, polys);
    float a = 0.0f;
    for (const auto& p : polys)
        for (size_t i = 0; i + 1 < p.size(); ++i)
            a += p[i].x * p[i + 1].y - p[i + 1].x * p[i].y;
    return 0.5f * a;
}

float resultArea(const OffsetResult& res) {
    std::vector<std::vector<glm::vec2>> polys;
    materializr::densifySegs(res.segs, polys);
    float a = 0.0f;
    for (const auto& p : polys)
        for (size_t i = 0; i + 1 < p.size(); ++i)
            a += p[i].x * p[i + 1].y - p[i + 1].x * p[i].y;
    return std::abs(0.5f * a);
}

// The signed distance that offsets a closed chain outward, whichever way the
// walk happened to orient it.
float outward(const OffsetChain& ch, float mag) {
    return (signedArea(ch) >= 0.0f) ? mag : -mag;
}

} // namespace

TEST(SketchOffsetGeom, SquareOutwardRoundCorners) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    ASSERT_TRUE(ch.valid());

    const float d = outward(ch, 2.0f);
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);

    ASSERT_TRUE(res.valid);
    EXPECT_EQ(res.segs.size(), 8u) << "4 sides + 4 corner arcs";
    expectOnOffset(res, ch, d);

    // A rounded outward offset is the square grown by d on each side, with the
    // corners replaced by quarter-circles: (a+2d)^2 - (4-pi)d^2.
    const float a = 10.0f, m = 2.0f;
    EXPECT_NEAR(resultArea(res),
                (a + 2 * m) * (a + 2 * m) - (4.0f - static_cast<float>(M_PI)) * m * m,
                0.3f);
}

TEST(SketchOffsetGeom, SquareOutwardSharpCorners) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    const float d = outward(ch, 2.0f);

    OffsetResult res = offsetChain(ch, d, OffsetCorners::Sharp);

    ASSERT_TRUE(res.valid);
    EXPECT_EQ(res.segs.size(), 4u) << "mitered: no corner elements";
    // A mitered square offset is exactly the larger square.
    EXPECT_NEAR(resultArea(res), 14.0f * 14.0f, 0.05f);
}

TEST(SketchOffsetGeom, SquareInwardIsTheSmallerSquare) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    const float d = -outward(ch, 2.0f); // inward

    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);

    ASSERT_TRUE(res.valid);
    // Every inward corner CLOSES, so it is trimmed, not rounded: 4 sides.
    EXPECT_EQ(res.segs.size(), 4u);
    expectOnOffset(res, ch, d);
    EXPECT_NEAR(resultArea(res), 6.0f * 6.0f, 0.05f);
}

TEST(SketchOffsetGeom, OpenLChainRoundsOneSideAndTrimsTheOther) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    int c = sk.addPoint({10.0f, 10.0f});
    sk.addLine(a, b);
    sk.addLine(b, c);
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    ASSERT_EQ(ch.segs.size(), 2u);

    // The corner opens on one side and closes on the other.
    OffsetResult open  = offsetChain(ch,  2.0f, OffsetCorners::Round);
    OffsetResult close = offsetChain(ch, -2.0f, OffsetCorners::Round);

    ASSERT_TRUE(open.valid);
    ASSERT_TRUE(close.valid);
    EXPECT_EQ(open.segs.size(),  3u) << "opening side gains a corner arc";
    EXPECT_EQ(close.segs.size(), 2u) << "closing side is trimmed to the crossing";
    expectOnOffset(open,  ch,  2.0f);
    expectOnOffset(close, ch, -2.0f);
}

TEST(SketchOffsetGeom, CircleGrowsAndShrinksAndRefuses) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    sk.addCircle(c, 5.0);
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);

    OffsetResult grow = offsetChain(ch, 2.0f, OffsetCorners::Round);
    ASSERT_TRUE(grow.valid);
    ASSERT_EQ(grow.segs.size(), 1u);
    EXPECT_NEAR(grow.segs[0].r, 7.0f, 1e-4f);

    OffsetResult shrink = offsetChain(ch, -2.0f, OffsetCorners::Round);
    ASSERT_TRUE(shrink.valid);
    EXPECT_NEAR(shrink.segs[0].r, 3.0f, 1e-4f);

    OffsetResult gone = offsetChain(ch, -5.0f, OffsetCorners::Round);
    EXPECT_FALSE(gone.valid);
    EXPECT_NE(gone.rejectReason, nullptr);
}

TEST(SketchOffsetGeom, TangentJoinInsertsNoCorner) {
    Sketch sk;
    // A line running up the y-axis into a semicircle it meets tangentially:
    // line (0,-10)->(0,0), then an arc centred (5,0) r=5 from (0,0) to (10,0).
    int p0 = sk.addPoint({0.0f, -10.0f});
    int p1 = sk.addPoint({0.0f, 0.0f});
    int ac = sk.addPoint({5.0f, 0.0f});
    int p2 = sk.addPoint({10.0f, 0.0f});
    sk.addLine(p0, p1);
    // Arcs always sweep CCW from start to end, so bulging UP through (5,5)
    // means storing it (10,0)->(0,0). The chain then traverses it backwards,
    // which is exactly what the signed sweep is for.
    sk.addArc(ac, p2, p1, 5.0);

    OffsetChain ch = walkOffsetChain(sk, {0.0f, -5.0f}, 0.5f);
    ASSERT_EQ(ch.segs.size(), 2u);

    for (float d : {1.5f, -1.5f}) {
        OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
        ASSERT_TRUE(res.valid) << "d=" << d;
        EXPECT_EQ(res.segs.size(), 2u)
            << "a tangent join needs no corner element (d=" << d << ")";
        expectOnOffset(res, ch, d);
    }
}

TEST(SketchOffsetGeom, ArcSweepingMoreThanHalfATurnSurvives) {
    Sketch sk;
    // 270-degree arc: CCW from (5,0) round to (0,-5).
    int c = sk.addPoint({0.0f, 0.0f});
    int s = sk.addPoint({5.0f, 0.0f});
    int e = sk.addPoint({0.0f, -5.0f});
    sk.addArc(c, s, e, 5.0);

    OffsetChain ch = walkOffsetChain(sk, {0.0f, 5.0f}, 0.5f);
    ASSERT_EQ(ch.segs.size(), 1u);
    EXPECT_GT(std::abs(ch.segs[0].sweep), static_cast<float>(M_PI));

    OffsetResult res = offsetChain(ch, 2.0f, OffsetCorners::Round);
    ASSERT_TRUE(res.valid);
    ASSERT_EQ(res.segs.size(), 1u);
    EXPECT_NEAR(std::abs(res.segs[0].sweep), std::abs(ch.segs[0].sweep), 1e-4f);
    expectOnOffset(res, ch, 2.0f);
}

TEST(SketchOffsetGeom, SlotOffsetsConcentrically) {
    Sketch sk;
    // Slot: two horizontal lines joined by semicircular end caps, r=5,
    // centres (0,0) and (20,0).
    int cL = sk.addPoint({0.0f, 0.0f});
    int cR = sk.addPoint({20.0f, 0.0f});
    int tl = sk.addPoint({0.0f, 5.0f});
    int tr = sk.addPoint({20.0f, 5.0f});
    int br = sk.addPoint({20.0f, -5.0f});
    int bl = sk.addPoint({0.0f, -5.0f});
    // Caps stored so their CCW sweep bulges OUTWARD (through (25,0) and
    // (-5,0)); the walk traverses both backwards.
    sk.addLine(tl, tr);
    sk.addArc(cR, br, tr, 5.0);
    sk.addLine(br, bl);
    sk.addArc(cL, tl, bl, 5.0);

    OffsetChain ch = walkOffsetChain(sk, {10.0f, 5.0f}, 0.5f);
    ASSERT_TRUE(ch.valid());
    EXPECT_TRUE(ch.closed);
    ASSERT_EQ(ch.segs.size(), 4u);

    const float d = outward(ch, 2.0f);
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    ASSERT_TRUE(res.valid);
    EXPECT_EQ(res.segs.size(), 4u) << "tangent joins throughout: no corners";
    expectOnOffset(res, ch, d);

    // Outward: the cap radii grow to 7.
    for (const auto& s : res.segs)
        if (s.kind == OffsetSeg::Kind::Arc) EXPECT_NEAR(s.r, 7.0f, 1e-3f);
}

// ─── Task 3: pruning + commit ───────────────────────────────────────────────

using materializr::applyOffset;
using materializr::distanceToChain;
using materializr::pruneOffset;

TEST(SketchOffsetPrune, InwardSquareBeyondHalfTheSideIsRefused) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);

    // 6 mm inward on a 10 mm square: the opposite walls have crossed, so every
    // candidate segment sits closer than 6 mm to the far side.
    const float d = -outward(ch, 6.0f);
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    pruneOffset(res, ch, d);

    EXPECT_FALSE(res.valid);
    EXPECT_NE(res.rejectReason, nullptr);
    EXPECT_TRUE(res.segs.empty());
}

TEST(SketchOffsetPrune, ValidOffsetSurvivesPruningUntouched) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    const float d = -outward(ch, 2.0f);

    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    const size_t before = res.segs.size();
    pruneOffset(res, ch, d);

    ASSERT_TRUE(res.valid);
    EXPECT_EQ(res.segs.size(), before) << "a well-formed offset must not be cut";
    EXPECT_TRUE(res.closed);
    expectOnOffset(res, ch, d);
}

TEST(SketchOffsetPrune, NarrowNotchPrunesTheInvalidRunOnly) {
    Sketch sk;
    // A shallow V: two 10 mm legs meeting at the origin with a 3 mm gap
    // between their far ends. Offsetting into the V by more than the notch
    // half-width invalidates the geometry near the vertex but not the legs.
    int a = sk.addPoint({-10.0f, 10.0f});
    int v = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 10.0f});
    sk.addLine(a, v);
    sk.addLine(v, b);
    OffsetChain ch = walkOffsetChain(sk, {-5.0f, 5.0f}, 0.5f);
    ASSERT_EQ(ch.segs.size(), 2u);

    const float d = 3.0f;
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    pruneOffset(res, ch, d);

    if (res.valid) expectOnOffset(res, ch, d);
    // Whatever survives, nothing may sit inside the |d| band.
    for (const auto& s : res.segs)
        for (int k = 0; k <= 8; ++k)
            EXPECT_GE(distanceToChain(ch, s.at(k / 8.0f)), d - 0.05f);
}

TEST(SketchOffsetApply, CommitsConnectedGeometryAndWelds) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    const float d = outward(ch, 2.0f);
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Sharp);
    pruneOffset(res, ch, d);
    ASSERT_TRUE(res.valid);

    const size_t linesBefore = sk.getLines().size();
    const size_t ptsBefore   = sk.getPoints().size();

    std::set<int> pts, els;
    applyOffset(sk, res, nullptr, pts, els);

    EXPECT_EQ(sk.getLines().size(), linesBefore + 4u);
    EXPECT_EQ(els.size(), 4u);
    // Four mitered corners shared between eight segment ends: four new points,
    // not eight.
    EXPECT_EQ(sk.getPoints().size(), ptsBefore + 4u);
    EXPECT_EQ(pts.size(), 4u);

    // The committed square must actually close, so it forms a region.
    auto regions = sk.buildRegions();
    EXPECT_GE(regions.size(), 2u) << "source square + offset square";
}

TEST(SketchOffsetApply, ArcsCommitCounterClockwiseWhicheverWayTheyWereWalked) {
    Sketch sk;
    // Semicircle walked backwards (see the walk test above), so its offset
    // carries a negative sweep and must be stored with its ends swapped.
    int c = sk.addPoint({0.0f, 0.0f});
    int s = sk.addPoint({5.0f, 0.0f});
    int e = sk.addPoint({-5.0f, 0.0f});
    int t = sk.addPoint({-5.0f, -10.0f});
    sk.addArc(c, s, e, 5.0);
    sk.addLine(t, e);

    OffsetChain ch = walkOffsetChain(sk, {-5.0f, -5.0f}, 0.5f);
    ASSERT_EQ(ch.segs.size(), 2u);

    const float d = 1.5f;
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    pruneOffset(res, ch, d);
    ASSERT_TRUE(res.valid);

    const size_t arcsBefore = sk.getArcs().size();
    std::set<int> pts, els;
    applyOffset(sk, res, nullptr, pts, els);
    ASSERT_GT(sk.getArcs().size(), arcsBefore);

    // Every committed arc must read back at the radius it was created with,
    // sweeping CCW from its stored start to its stored end.
    for (size_t i = arcsBefore; i < sk.getArcs().size(); ++i) {
        const auto& ar = sk.getArcs()[i];
        const auto* ac = sk.getPoint(ar.centerPointId);
        const auto* as = sk.getPoint(ar.startPointId);
        const auto* ae = sk.getPoint(ar.endPointId);
        ASSERT_NE(ac, nullptr); ASSERT_NE(as, nullptr); ASSERT_NE(ae, nullptr);
        EXPECT_NEAR(glm::length(as->pos - ac->pos), ar.radius, 1e-3);
        EXPECT_NEAR(glm::length(ae->pos - ac->pos), ar.radius, 1e-3);
    }
}

TEST(SketchOffsetApply, WeldCallbackReusesExistingPoints) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    sk.addLine(a, b);
    // A point already sitting exactly where the offset line will start.
    int existing = sk.addPoint({0.0f, -2.0f});

    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    OffsetResult res = offsetChain(ch, 2.0f, OffsetCorners::Round);
    pruneOffset(res, ch, 2.0f);
    ASSERT_TRUE(res.valid);

    auto weld = [&](glm::vec2 p) {
        for (const auto& q : sk.getPoints())
            if (glm::length(q.pos - p) < 1e-3f) return q.id;
        return -1;
    };
    std::set<int> pts, els;
    const size_t ptsBefore = sk.getPoints().size();
    applyOffset(sk, res, weld, pts, els);

    EXPECT_EQ(sk.getPoints().size(), ptsBefore + 1u) << "one end welded";
    EXPECT_TRUE(pts.count(existing)) << "welded onto the existing point";
}

// ─── Task 4: the tool state machine ─────────────────────────────────────────
//
// Drives SketchTool directly - no GL, no window. Covers the wiring the
// geometry tests cannot: phases, the cursor picking the side, and the fact
// that a click REQUESTS the commit rather than performing it (the app wraps
// commitOffset in recordSketchMutation for one undo step).

#include "modeling/SketchTool.h"

using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {

SketchTool makeToolOn(Sketch& sk) {
    SketchTool t;
    t.setSketch(&sk);
    t.setMode(SketchToolMode::Offset);
    return t;
}

} // namespace

TEST(SketchOffsetTool, HoverHighlightsTheWholeChainBeforeAnyClick) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    SketchTool tool = makeToolOn(sk);

    EXPECT_EQ(tool.getOffsetPhase(), SketchTool::OffsetPhase::Pick);
    tool.onMouseMove({5.0f, 0.0f});

    // All four sides highlight, not just the edge under the cursor.
    const auto& hover = tool.getOffsetChainHover();
    ASSERT_FALSE(hover.empty());
    size_t pts = 0;
    for (const auto& p : hover) pts += p.size();
    EXPECT_GE(pts, 5u) << "the highlight must cover the whole closed loop";
    EXPECT_FALSE(tool.hasOffsetChain()) << "hovering must not capture anything";
}

TEST(SketchOffsetTool, ClickCapturesTheChainAndCursorPicksTheSide) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    SketchTool tool = makeToolOn(sk);

    tool.onMouseMove({5.0f, 0.0f});
    tool.onMouseDown({5.0f, 0.0f});
    ASSERT_TRUE(tool.hasOffsetChain());
    EXPECT_EQ(tool.getOffsetPhase(), SketchTool::OffsetPhase::Distance);

    // Below the bottom edge = outside the square.
    tool.onMouseMove({5.0f, -3.0f});
    const float outside = tool.getOffsetDistance();
    EXPECT_NEAR(std::abs(outside), 3.0f, 0.2f);

    // Inside the square: same magnitude, opposite sign.
    tool.onMouseMove({5.0f, 3.0f});
    const float inside = tool.getOffsetDistance();
    EXPECT_NEAR(std::abs(inside), 3.0f, 0.2f);
    EXPECT_LT(outside * inside, 0.0f) << "the two sides must have opposite signs";
}

TEST(SketchOffsetTool, ClickRequestsTheCommitButDoesNotMutate) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    SketchTool tool = makeToolOn(sk);

    tool.onMouseMove({5.0f, 0.0f});
    tool.onMouseDown({5.0f, 0.0f});
    tool.onMouseMove({5.0f, -3.0f});
    ASSERT_TRUE(tool.offsetReady());

    const size_t linesBefore = sk.getLines().size();
    tool.onMouseDown({5.0f, -3.0f});

    EXPECT_TRUE(tool.offsetReadyToCommit());
    EXPECT_EQ(sk.getLines().size(), linesBefore)
        << "the click must not mutate - the app commits inside recordSketchMutation";

    std::set<int> pts, els;
    tool.commitOffset(pts, els);
    EXPECT_GT(sk.getLines().size(), linesBefore);
    EXPECT_FALSE(tool.offsetReadyToCommit());
    // Back to Pick with the tool still active: offsetting several chains in a
    // row is the normal way to use it.
    EXPECT_EQ(tool.getOffsetPhase(), SketchTool::OffsetPhase::Pick);
    EXPECT_EQ(tool.getMode(), SketchToolMode::Offset);
}

TEST(SketchOffsetTool, EscapeStepsBackOnePhaseThenLeaves) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    SketchTool tool = makeToolOn(sk);

    tool.onMouseMove({5.0f, 0.0f});
    tool.onMouseDown({5.0f, 0.0f});
    ASSERT_EQ(tool.getOffsetPhase(), SketchTool::OffsetPhase::Distance);

    tool.onCancel();
    EXPECT_EQ(tool.getOffsetPhase(), SketchTool::OffsetPhase::Pick);
    EXPECT_FALSE(tool.hasOffsetChain());
    EXPECT_EQ(tool.getMode(), SketchToolMode::Offset) << "first Escape stays in the tool";

    tool.onCancel();
    EXPECT_EQ(tool.getMode(), SketchToolMode::Select) << "second Escape leaves it";
}

TEST(SketchOffsetTool, TypedValueSetsMagnitudeAndKeepsTheSide) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    SketchTool tool = makeToolOn(sk);

    tool.onMouseMove({5.0f, 0.0f});
    tool.onMouseDown({5.0f, 0.0f});
    tool.onMouseMove({5.0f, -1.0f});      // pick the outside
    const float sign = (tool.getOffsetDistance() < 0.0f) ? -1.0f : 1.0f;

    ASSERT_TRUE(tool.applyDimension(4.0f));
    EXPECT_NEAR(tool.getOffsetDistance(), sign * 4.0f, 1e-4f);
    EXPECT_TRUE(tool.offsetReadyToCommit()) << "a typed value asks for the commit";
}

TEST(SketchOffsetTool, SwitchingToolsAbandonsAnInProgressOffset) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    SketchTool tool = makeToolOn(sk);

    tool.onMouseMove({5.0f, 0.0f});
    tool.onMouseDown({5.0f, 0.0f});
    tool.onMouseMove({5.0f, -3.0f});
    ASSERT_TRUE(tool.hasOffsetChain());

    const size_t linesBefore = sk.getLines().size();
    tool.setMode(SketchToolMode::Line);

    EXPECT_FALSE(tool.hasOffsetChain());
    EXPECT_EQ(sk.getLines().size(), linesBefore) << "an abandoned offset creates nothing";
}

TEST(SketchOffsetTool, TheCursorCannotAskForAnImpossibleInwardOffset) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    SketchTool tool = makeToolOn(sk);

    tool.onMouseMove({5.0f, 0.0f});
    tool.onMouseDown({5.0f, 0.0f});

    // 6 mm inside a 10 mm square is only 4 mm from the NEAREST wall, and the
    // distance is measured to the nearest part of the chain - so dragging can
    // never request more than half the width on a convex closed profile. Nice
    // property: the impossible case is unreachable by cursor, only by typing.
    tool.onMouseMove({5.0f, 6.0f});
    EXPECT_NEAR(std::abs(tool.getOffsetDistance()), 4.0f, 0.2f);
    EXPECT_TRUE(tool.offsetReady());
}

TEST(SketchOffsetTool, AnOverlargeTypedOffsetIsRefusedWithAReason) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    SketchTool tool = makeToolOn(sk);

    tool.onMouseMove({5.0f, 0.0f});
    tool.onMouseDown({5.0f, 0.0f});
    tool.onMouseMove({5.0f, 3.0f});   // inside: fixes the sign
    const float inwardSign = (tool.getOffsetDistance() < 0.0f) ? -1.0f : 1.0f;

    // Typed straight in, past the point where the opposite walls cross.
    tool.setOffsetDistance(inwardSign * 6.0f);

    EXPECT_FALSE(tool.offsetReady());
    EXPECT_NE(tool.offsetRejection(), nullptr);
    EXPECT_TRUE(tool.getOffsetPreview().empty()) << "no ghost for geometry we would refuse";

    const size_t linesBefore = sk.getLines().size();
    EXPECT_FALSE(tool.applyDimension(6.0f)) << "a refused offset cannot be committed";
    EXPECT_FALSE(tool.offsetReadyToCommit());
    EXPECT_EQ(sk.getLines().size(), linesBefore);
}

// ─── Splines ────────────────────────────────────────────────────────────────
//
// There is no exact offset of a B-spline: the curve is sampled, offset along
// its normal point by point, and re-fitted to a spline at commit. So these
// cases assert the invariant to a FIT tolerance, not the analytic one - and
// the tolerance is the honest measure of how good the approximation is.

namespace {

// A spline whose control points sit on a circle, so the offset has a value we
// can check independently: every point should land at radius r ± d.
int addCircularSpline(Sketch& sk, glm::vec2 c, float r, int n, float sweepDeg) {
    std::vector<int> ids;
    for (int i = 0; i < n; ++i) {
        float a = glm::radians(sweepDeg) * static_cast<float>(i) / (n - 1);
        ids.push_back(sk.addPoint(c + glm::vec2(std::cos(a), std::sin(a)) * r));
    }
    return sk.addSpline(ids);
}

} // namespace

TEST(SketchOffsetSpline, OffsetOfACurveStaysAtTheRightDistance) {
    Sketch sk;
    addCircularSpline(sk, {0.0f, 0.0f}, 20.0f, 9, 120.0f);
    OffsetChain ch = walkOffsetChain(sk, {20.0f, 0.0f}, 1.0f);
    ASSERT_TRUE(ch.valid());
    ASSERT_EQ(ch.segs.size(), 1u);

    for (float d : {2.0f, -2.0f, 5.0f}) {
        OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
        pruneOffset(res, ch, d);
        ASSERT_TRUE(res.valid) << "d=" << d;
        ASSERT_EQ(res.segs.size(), 1u);
        EXPECT_EQ(res.segs[0].kind, OffsetSeg::Kind::Spline);
        // 1% of the offset distance is comfortably tighter than anything that
        // matters at CAD scale.
        expectOnOffset(res, ch, d, std::max(0.02f, 0.01f * std::abs(d)));
    }
}

TEST(SketchOffsetSpline, OffsetOfACircularArcSplineHasTheExpectedRadius) {
    Sketch sk;
    const glm::vec2 C{0.0f, 0.0f};
    addCircularSpline(sk, C, 20.0f, 11, 150.0f);
    OffsetChain ch = walkOffsetChain(sk, {20.0f, 0.0f}, 1.0f);
    ASSERT_TRUE(ch.valid());

    // Offsetting a curve that lies on a circle must give one that lies on a
    // concentric circle - an independent check on the normal direction.
    const float d = 3.0f;
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    pruneOffset(res, ch, d);
    ASSERT_TRUE(res.valid);

    float rMin = 1e9f, rMax = -1e9f;
    for (const auto& sg : res.segs)
        for (int k = 0; k <= 20; ++k) {
            float rr = glm::length(sg.at(k / 20.0f) - C);
            rMin = std::min(rMin, rr);
            rMax = std::max(rMax, rr);
        }
    // Either 23 (outward) or 17 (inward) - the side depends on the walk's
    // travel direction, which is arbitrary. Both must be uniform.
    const bool outward = (rMin > 20.0f);
    const float want = outward ? 23.0f : 17.0f;
    EXPECT_NEAR(rMin, want, 0.15f);
    EXPECT_NEAR(rMax, want, 0.15f);
}

TEST(SketchOffsetSpline, CommitsASplineNotAPileOfLines) {
    Sketch sk;
    addCircularSpline(sk, {0.0f, 0.0f}, 20.0f, 9, 120.0f);
    OffsetChain ch = walkOffsetChain(sk, {20.0f, 0.0f}, 1.0f);
    const float d = 2.0f;
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    pruneOffset(res, ch, d);
    ASSERT_TRUE(res.valid);

    const size_t splinesBefore = sk.getSplines().size();
    const size_t linesBefore   = sk.getLines().size();
    std::set<int> pts, els;
    applyOffset(sk, res, nullptr, pts, els);

    EXPECT_EQ(sk.getSplines().size(), splinesBefore + 1u) << "one spline out";
    EXPECT_EQ(sk.getLines().size(), linesBefore) << "and no stray line soup";

    const auto& made = sk.getSplines().back();
    // Editable density: an offset of a 9-point spline must not come back as a
    // hundred-point mass of vertex markers. (It did, before the cap.)
    EXPECT_LE(made.controlPointIds.size(), 18u) << "at most double the source";
    EXPECT_GE(made.controlPointIds.size(), 3u);

    // And it must still sit where the preview promised.
    for (glm::vec2 q : sk.sampleSpline2D(made, 12))
        EXPECT_NEAR(distToChain(ch, q), d, 0.1f);
}

TEST(SketchOffsetSpline, AerofoilShapedClosedChainOffsets) {
    Sketch sk;
    // The shape an aerofoil import actually makes: an upper and a lower spline
    // meeting at a shared nose point, closed by a trailing-edge line.
    const int nose = sk.addPoint({0.0f, 0.0f});
    std::vector<int> up{nose}, lo{nose};
    for (int i = 1; i <= 6; ++i) {
        float x = 10.0f * static_cast<float>(i) / 6.0f;
        up.push_back(sk.addPoint({x, 1.6f * std::sqrt(x / 10.0f) * (1.0f - x / 12.0f)}));
        lo.push_back(sk.addPoint({x, -1.0f * std::sqrt(x / 10.0f) * (1.0f - x / 12.0f)}));
    }
    sk.addSpline(up);
    sk.addSpline(lo);
    sk.addLine(up.back(), lo.back()); // trailing edge

    OffsetChain ch = walkOffsetChain(sk, {5.0f, 1.1f}, 1.0f);
    ASSERT_TRUE(ch.valid());
    EXPECT_TRUE(ch.closed) << "nose + trailing edge close the section";
    EXPECT_EQ(ch.segs.size(), 3u);

    const float d = outward(ch, 1.0f);
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    pruneOffset(res, ch, d);
    ASSERT_TRUE(res.valid);
    expectOnOffset(res, ch, d, 0.06f);

    std::set<int> pts, els;
    const size_t splinesBefore = sk.getSplines().size();
    applyOffset(sk, res, nullptr, pts, els);
    EXPECT_EQ(sk.getSplines().size(), splinesBefore + 2u) << "both surfaces stay curves";
}

TEST(SketchOffsetSpline, SharpCornersFallBackToRoundAgainstACurve) {
    Sketch sk;
    // A line meeting a curve at an angle: there is no analytic intersection to
    // miter to, so Sharp must round rather than invent a corner.
    int a = sk.addPoint({-10.0f, 10.0f});
    int j = sk.addPoint({0.0f, 0.0f});
    int m = sk.addPoint({10.0f, 4.0f});
    int e = sk.addPoint({20.0f, 0.0f});
    sk.addLine(a, j);
    sk.addSpline({j, m, e});

    OffsetChain ch = walkOffsetChain(sk, {-5.0f, 5.0f}, 0.5f);
    ASSERT_EQ(ch.segs.size(), 2u);

    for (auto corners : {OffsetCorners::Round, OffsetCorners::Sharp}) {
        for (float d : {1.5f, -1.5f}) {
            OffsetResult res = offsetChain(ch, d, corners);
            pruneOffset(res, ch, d);
            ASSERT_TRUE(res.valid) << "d=" << d;
            expectOnOffset(res, ch, d, 0.05f);
        }
    }
}

TEST(SketchOffsetSpline, OneSmoothCurveOffsetsToOneCurve) {
    Sketch sk;
    addCircularSpline(sk, {0.0f, 0.0f}, 30.0f, 7, 110.0f);
    OffsetChain ch = walkOffsetChain(sk, {30.0f, 0.0f}, 1.5f);
    ASSERT_TRUE(ch.valid());

    // A spline offset is a sampled approximation, so the prune band has to be
    // sized to the representation. With an analytic epsilon this came back as
    // three abutting fragments instead of one curve.
    for (float d : {2.0f, -2.0f, 5.0f, -5.0f}) {
        OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
        pruneOffset(res, ch, d);
        ASSERT_TRUE(res.valid) << "d=" << d;
        EXPECT_EQ(res.segs.size(), 1u)
            << "one smooth source curve must give one offset curve (d=" << d << ")";
    }

    const float d = 3.0f;
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    pruneOffset(res, ch, d);
    std::set<int> pts, els;
    applyOffset(sk, res, nullptr, pts, els);
    EXPECT_EQ(els.size(), 1u) << "and commits as a single element";
}
