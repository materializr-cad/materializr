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
using materializr::signedDistanceToChain;
using materializr::distanceToChain;

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

// Regression (pot2bug.mzr): hand-drawn fillets are a hair off true tangent.
// That used to read as a real corner, so a round-join arc of radius |d| and
// ~3e-4 rad sweep was inserted at every line/fillet junction. Committed, its
// two ends weld to one point - a degenerate arc that reads as a full circle.
TEST(SketchOffsetGeom, NearlyTangentFilletsInsertNoJoinCircles) {
    Sketch sk;
    // The outline from pot2bug.mzr, coordinates verbatim: a 110 mm frame with
    // R3 fillets whose centres/ends are ~1e-3 off true tangent.
    int p10 = sk.addPoint({105.0f, 108.0f}), p11 = sk.addPoint({108.0f, 105.0f});
    int c12 = sk.addPoint({105.001f, 105.001f});
    int p14 = sk.addPoint({1.0f, 108.0f}),   p15 = sk.addPoint({-2.0f, 105.0f});
    int c16 = sk.addPoint({0.999764f, 105.001f});
    int p18 = sk.addPoint({-2.0f, 1.0f}),    p19 = sk.addPoint({1.0f, -2.0f});
    int c20 = sk.addPoint({1.0f, 1.0f});
    int p22 = sk.addPoint({108.0f, 1.0f}),   p23 = sk.addPoint({105.0f, -2.0f});
    int c24 = sk.addPoint({105.001f, 0.999767f});
    sk.addLine(p10, p14); sk.addLine(p15, p18);
    sk.addLine(p22, p11); sk.addLine(p19, p23);
    sk.addArc(c12, p11, p10, 2.99948);
    sk.addArc(c16, p14, p15, 2.99946);
    sk.addArc(c20, p18, p19, 3.0);
    sk.addArc(c24, p23, p22, 2.99922);

    OffsetChain ch = walkOffsetChain(sk, {50.0f, 108.0f}, 0.5f);
    ASSERT_TRUE(ch.valid());
    ASSERT_EQ(ch.segs.size(), 8u);

    for (float d : {2.0f, -2.0f}) {
        OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
        ASSERT_TRUE(res.valid) << "d=" << d;
        // 4 lines + 4 fillet arcs, nothing else.
        EXPECT_EQ(res.segs.size(), 8u) << "d=" << d;
        for (const auto& s : res.segs) {
            if (s.kind != OffsetSeg::Kind::Arc) continue;
            EXPECT_GT(std::abs(s.sweep), 1.0f)
                << "sliver join arc, r=" << s.r << " d=" << d;
        }
        expectOnOffset(res, ch, d, 5e-3f);
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

// An offset of a construction line used to come out as a normal line. The copy
// now keeps its source's type, and turning "draw as construction" on flips it.
namespace {
int countConstructionLines(const Sketch& sk, size_t from) {
    int n = 0;
    for (size_t i = from; i < sk.getLines().size(); ++i)
        if (sk.getLines()[i].isConstruction) ++n;
    return n;
}

// Offsets the square drawn with `markConstruction` of its four edges set as
// construction; returns how many of the four new edges are construction.
int offsetSquare(int markConstruction, bool invert) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    for (int i = 0; i < markConstruction && i < static_cast<int>(sk.getLines().size()); ++i)
        sk.setElementConstruction(sk.getLines()[i].id, true);
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    const float d = outward(ch, 2.0f);
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Sharp);
    pruneOffset(res, ch, d);
    const size_t before = sk.getLines().size();
    std::set<int> pts, els;
    applyOffset(sk, res, nullptr, pts, els, invert);
    return countConstructionLines(sk, before);
}
} // namespace

TEST(SketchOffsetConstruction, NormalChainOffsetsToNormalLines) {
    EXPECT_EQ(offsetSquare(0, false), 0);
}

TEST(SketchOffsetConstruction, ConstructionEdgesStayConstructionInTheCopy) {
    EXPECT_EQ(offsetSquare(4, false), 4);
    EXPECT_EQ(offsetSquare(2, false), 2) << "a mixed chain keeps its mix";
}

TEST(SketchOffsetConstruction, DrawAsConstructionFlipsEveryEdge) {
    EXPECT_EQ(offsetSquare(0, true), 4) << "normal -> construction";
    EXPECT_EQ(offsetSquare(4, true), 0) << "construction -> normal";
    EXPECT_EQ(offsetSquare(1, true), 3) << "a mixed chain swaps wholesale";
}

// A closed contour with something else attached at one corner is still ONE
// contour. The walk used to stop at that corner (three elements meet there),
// so the chain came out open and the corner was left unjoined on an outward
// offset.
TEST(SketchOffsetWalk, LoopWithAStemAtOneCornerIsStillClosed) {
    Sketch sk;
    auto pts = makeSquare(sk, 10.0f);
    int tip = sk.addPoint({-6.0f, -6.0f});
    sk.addLine(pts[0], tip);                      // stem off the (0,0) corner

    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    ASSERT_TRUE(ch.valid());
    EXPECT_TRUE(ch.closed);
    EXPECT_EQ(ch.segs.size(), 4u) << "the four sides; the stem is its own chain";
    expectContiguous(ch);

    const float d = outward(ch, 2.0f);
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Sharp);
    pruneOffset(res, ch, d);
    ASSERT_TRUE(res.valid);
    EXPECT_TRUE(res.closed);
    EXPECT_EQ(res.segs.size(), 4u) << "all four mitred corners, including the one with the stem";
}

TEST(SketchOffsetWalk, OnlyTheSelectedSegmentIsPickedWhenRestricted) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    const int bottom = sk.getLines()[0].id;

    // Unrestricted: the whole square.
    OffsetChain all = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f);
    ASSERT_EQ(all.segs.size(), 4u);

    // Restricted to the bottom edge: just that line, an open chain of one.
    std::set<int> only{bottom};
    OffsetChain one = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f, &only);
    ASSERT_TRUE(one.valid());
    EXPECT_FALSE(one.closed);
    ASSERT_EQ(one.segs.size(), 1u);
    EXPECT_EQ(one.segs[0].sourceId, bottom);

    // Clicking a side that is not selected picks nothing.
    EXPECT_FALSE(walkOffsetChain(sk, {10.0f, 5.0f}, 0.5f, &only).valid());
}

TEST(SketchOffsetWalk, ARestrictedRunStopsWhereTheSelectionStops) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    std::set<int> two{sk.getLines()[0].id, sk.getLines()[1].id};   // bottom + right
    OffsetChain ch = walkOffsetChain(sk, {5.0f, 0.0f}, 0.5f, &two);
    ASSERT_TRUE(ch.valid());
    EXPECT_FALSE(ch.closed);
    EXPECT_EQ(ch.segs.size(), 2u);
    expectContiguous(ch);
}

// Select first, then choose Offset: the selection IS the pick. It used to ask
// for the very same elements to be clicked again - and for separate (unconnected)
// ones a click then offset only the one clicked (1.8.0 beta report).
TEST(SketchOffsetTool, PreselectedSeparateLinesGoStraightToDistanceAndAllOffset) {
    Sketch sk;
    const int a0 = sk.addPoint({0.0f, 0.0f}),  a1 = sk.addPoint({10.0f, 0.0f});
    const int b0 = sk.addPoint({0.0f, 20.0f}), b1 = sk.addPoint({10.0f, 20.0f});
    const int la = sk.addLine(a0, a1), lb = sk.addLine(b0, b1);
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setMode(SketchToolMode::Select);
    tool.setSelection({}, {la, lb});
    tool.setMode(SketchToolMode::Offset);

    EXPECT_EQ(tool.getOffsetPhase(), SketchTool::OffsetPhase::Distance)
        << "a pre-selection must not need re-selecting";
    ASSERT_TRUE(tool.hasOffsetChain());

    tool.setOffsetDistance(3.0f);
    ASSERT_TRUE(tool.offsetReady());
    const size_t before = sk.getLines().size();
    std::set<int> pts, els;
    tool.commitOffset(pts, els);
    EXPECT_EQ(sk.getLines().size(), before + 2u) << "both separate lines must be offset";
}

TEST(SketchOffsetTool, PreselectedConnectedLinesFormOneChain) {
    Sketch sk;
    makeSquare(sk, 10.0f);
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setMode(SketchToolMode::Select);
    tool.setSelection({}, {sk.getLines()[0].id, sk.getLines()[1].id});
    tool.setMode(SketchToolMode::Offset);
    EXPECT_EQ(tool.getOffsetPhase(), SketchTool::OffsetPhase::Distance);
    tool.setOffsetDistance(2.0f);
    EXPECT_TRUE(tool.offsetReady());
}

// 11-3-26: three separate lines, selected, then Offset - only the first two were
// offset. Every separate chain follows the first.
TEST(SketchOffsetTool, ThreeSeparateLinesAllOffset) {
    Sketch sk;
    int p[6];
    p[0] = sk.addPoint({0.0f, 0.0f});  p[1] = sk.addPoint({20.0f, 0.0f});
    p[2] = sk.addPoint({0.0f, 10.0f}); p[3] = sk.addPoint({20.0f, 10.0f});
    p[4] = sk.addPoint({0.0f, 20.0f}); p[5] = sk.addPoint({20.0f, 20.0f});
    const int l1 = sk.addLine(p[0], p[1]), l2 = sk.addLine(p[2], p[3]), l3 = sk.addLine(p[4], p[5]);
    for (float mmPerPx : {0.0f, 0.04f}) {      // no scale yet, and a real zoom
        Sketch s2 = sk;
        SketchTool tool;
        tool.setSketch(&s2);
        tool.setPixelScale(mmPerPx);
        tool.setMode(SketchToolMode::Select);
        tool.setSelection({}, {l1, l2, l3});
        tool.setMode(SketchToolMode::Offset);
        ASSERT_TRUE(tool.hasOffsetChain());
        tool.setOffsetDistance(3.0f);
        ASSERT_TRUE(tool.offsetReady());
        const size_t before = s2.getLines().size();
        std::set<int> pts, els;
        tool.commitOffset(pts, els);
        EXPECT_EQ(s2.getLines().size(), before + 3u)
            << "all three lines must be offset (mm/px " << mmPerPx << ")";
    }
}


// #182: offsetting a whole regular polygon with mitred corners gives a polygon
// (same sides, same centre, larger radius), not just a ring of plain lines.
TEST(SketchOffsetApply, SharpOffsetOfAPolygonIsAPolygon) {
    Sketch sk;
    const int c = sk.addPoint({0, 0});
    sk.addPolygon(c, 10.0, 6);
    OffsetChain ch = walkOffsetChain(sk, sk.getPoint(sk.getPolygons()[0].vertexPointIds[0])->pos, 0.5f);
    ASSERT_TRUE(ch.valid());
    const float d = outward(ch, 3.0f);
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Sharp);
    pruneOffset(res, ch, d);
    ASSERT_TRUE(res.valid);
    std::set<int> pts, els;
    applyOffset(sk, res, nullptr, pts, els);
    ASSERT_EQ(sk.getPolygons().size(), 2u);
    const auto& np = sk.getPolygons()[1];
    EXPECT_EQ(np.sides, 6);
    EXPECT_EQ(np.centerPointId, c) << "same centre";
    // The apothem grows by 3, so the circumradius grows by 3 / cos(30 deg).
    EXPECT_NEAR(np.radius, 10.0 + 3.0 / std::cos(3.14159265 / 6.0), 1e-2);
}

TEST(SketchOffsetApply, RoundOffsetOfAPolygonStaysPlainGeometry) {
    Sketch sk;
    const int c = sk.addPoint({0, 0});
    sk.addPolygon(c, 10.0, 6);
    OffsetChain ch = walkOffsetChain(sk, sk.getPoint(sk.getPolygons()[0].vertexPointIds[0])->pos, 0.5f);
    ASSERT_TRUE(ch.valid());
    const float d = outward(ch, 3.0f);
    OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
    pruneOffset(res, ch, d);
    ASSERT_TRUE(res.valid);
    std::set<int> pts, els;
    applyOffset(sk, res, nullptr, pts, els);
    EXPECT_EQ(sk.getPolygons().size(), 1u) << "arcs at the corners: not a polygon";
}


// ---------------------------------------------------------------------------
// #186: several separate chains offset together.
// ---------------------------------------------------------------------------

namespace {
size_t runsIn(const SketchTool& t) { return t.getOffsetPreview().size(); }
}

// The preview showed only the LAST chain (densifySegs replaces its output) while
// the commit made them all.
TEST(SketchOffsetMulti, PreviewShowsEveryChainNotJustTheLast) {
    Sketch sk;
    const int a0 = sk.addPoint({0.0f, 0.0f}),  a1 = sk.addPoint({10.0f, 0.0f});
    const int b0 = sk.addPoint({0.0f, 20.0f}), b1 = sk.addPoint({10.0f, 20.0f});
    const int la = sk.addLine(a0, a1), lb = sk.addLine(b0, b1);
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setMode(SketchToolMode::Select);
    tool.setSelection({}, {la, lb});
    tool.setMode(SketchToolMode::Offset);
    tool.setOffsetDistance(3.0f);
    ASSERT_TRUE(tool.offsetReady());
    EXPECT_EQ(runsIn(tool), 2u) << "one offset polyline per chain";
}

TEST(SketchOffsetMulti, LineAndCircleBothPreview) {
    Sketch sk;
    const int a0 = sk.addPoint({13.0f, -6.0f}), a1 = sk.addPoint({13.0f, 9.0f});
    const int c = sk.addPoint({-9.0f, 2.0f});
    const int la = sk.addLine(a0, a1);
    const int ci = sk.addCircle(c, 7.0);
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setMode(SketchToolMode::Select);
    tool.setSelectionFull({}, {la}, {ci}, {}, {});
    tool.setMode(SketchToolMode::Offset);
    ASSERT_TRUE(tool.hasOffsetChain());
    tool.setOffsetDistance(3.0f);
    ASSERT_TRUE(tool.offsetReady());
    EXPECT_EQ(runsIn(tool), 2u) << "the line's offset AND the circle's";
    const size_t roundBefore = sk.getCircles().size() + sk.getArcs().size();
    const size_t linesBefore = sk.getLines().size();
    std::set<int> pts, els;
    tool.commitOffset(pts, els);
    EXPECT_EQ(sk.getCircles().size() + sk.getArcs().size(), roundBefore + 1)
        << "the circle's offset (a circle or a full arc)";
    EXPECT_EQ(sk.getLines().size(), linesBefore + 1) << "and the commit makes both too";
}

// Two parallel lines drawn in OPPOSITE directions used to move apart when offset
// together (each went to the right of its own travel); one drawn the same way
// moved together. They follow the first line's side in space now.
TEST(SketchOffsetMulti, ParallelLinesDrawnOppositeWaysOffsetTheSameSide) {
    for (bool reversed : {false, true}) {
        Sketch sk;
        const int a0 = sk.addPoint({0.0f, 0.0f}),  a1 = sk.addPoint({10.0f, 0.0f});
        const int b0 = sk.addPoint({0.0f, 20.0f}), b1 = sk.addPoint({10.0f, 20.0f});
        const int la = sk.addLine(a0, a1);
        const int lb = reversed ? sk.addLine(b1, b0) : sk.addLine(b0, b1);
        SketchTool tool;
        tool.setSketch(&sk);
        tool.setMode(SketchToolMode::Select);
        tool.setSelection({}, {la, lb});
        tool.setMode(SketchToolMode::Offset);
        tool.setOffsetDistance(3.0f);
        ASSERT_TRUE(tool.offsetReady());
        std::set<int> pts, els;
        tool.commitOffset(pts, els);
        ASSERT_EQ(els.size(), 2u);
        std::vector<float> shift;
        for (int id : els)
            for (const auto& l : sk.getLines())
                if (l.id == id) {
                    const float y = sk.getPoint(l.startPointId)->pos.y;
                    shift.push_back(y > 10.0f ? y - 20.0f : y);      // how far it moved off its source
                }
        ASSERT_EQ(shift.size(), 2u);
        EXPECT_NEAR(std::abs(shift[0]), 3.0f, 1e-3);
        EXPECT_NEAR(std::abs(shift[1]), 3.0f, 1e-3);
        EXPECT_GT(shift[0] * shift[1], 0.0f)
            << "both lines move the same way (reversed=" << reversed << ")";
    }
}


// #186 example 3: a spline with tight bends left gaps in its offset that moved
// around as the distance changed. Loops cut out of the inside of a bend are
// rejoined where they cross, and a bend sampled coarsely gets a round join rather
// than a chord that cut inside the offset distance - so the offset is one
// unbroken curve at every distance.
TEST(SketchOffsetSpline, TightlyBentSplineOffsetsWithoutGaps) {
    Sketch sk;
    const glm::vec2 ctrl[] = {{20, 0}, {19, -8}, {18, 6}, {14.2487f, -8}, {10.4974f, 6},
                              {4, -8}, {-2.49742f, 6}, {-12, -8}, {-21.5026f, 6}};
    std::vector<int> ids;
    for (const auto& c : ctrl) ids.push_back(sk.addPoint(c));
    sk.addSpline(ids);
    const auto samples = sk.sampleSpline2D(sk.getSplines()[0], 6);
    OffsetChain ch = walkOffsetChain(sk, samples[samples.size() / 2], 0.5f);
    ASSERT_TRUE(ch.valid());
    for (float d : {-6.f, -4.f, -2.f, -1.f, 1.f, 2.f, 4.f, 6.f}) {
        OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
        pruneOffset(res, ch, d);
        ASSERT_TRUE(res.valid) << "d=" << d;
        std::vector<std::vector<glm::vec2>> runs;
        densifySegs(res.segs, runs);
        EXPECT_EQ(runs.size(), 1u) << "one unbroken offset curve at d=" << d;
        // And it never comes closer to the source than the distance asked for.
        for (const auto& run : runs)
            for (const auto& q : run)
                EXPECT_GE(distanceToChain(ch, q), std::abs(d) - 0.1f) << "d=" << d;
    }
}

// #182 (Iron47, b861): a sharp peak between two valleys. Where two offset arcs cross
// under the peak the valid stretch joining them is a few samples long; pruning
// stepped over it and left a gap that came and went with the distance.
TEST(SketchOffsetSpline, SharpPeakBetweenValleysLeavesNoGap) {
    Sketch sk;
    const glm::vec2 ctrl[] = {{37, 0}, {35, 0}, {34, 2}, {32.9282f, -2}, {32, 1}, {29.6906f, -3},
                              {27.3812f, 1}, {26, -0.3812f}, {24.0479f, 3}, {22.9761f, -1},
                              {21.9043f, 3}, {19, -4}, {17.6603f, 1}, {17, -5}, {14, 2},
                              {11, -1}, {10, 3}, {6.5359f, -3}, {5.19615f, 2}, {1.73205f, -4},
                              {-1.1547f, 1}, {-3, -4}, {-6, 1}, {-13, -8}, {-18.7735f, 2}, {-27, -5}};
    std::vector<int> ids;
    for (const auto& c : ctrl) ids.push_back(sk.addPoint(c));
    sk.addSpline(ids);
    const auto samples = sk.sampleSpline2D(sk.getSplines()[0], 6);
    OffsetChain ch = walkOffsetChain(sk, samples[samples.size() / 2], 0.5f);
    ASSERT_TRUE(ch.valid());
    for (float d = -7.f; d <= 7.f; d += 0.25f) {
        if (std::abs(d) < 0.01f) continue;
        OffsetResult res = offsetChain(ch, d, OffsetCorners::Round);
        pruneOffset(res, ch, d);
        ASSERT_TRUE(res.valid) << "d=" << d;
        std::vector<std::vector<glm::vec2>> runs;
        densifySegs(res.segs, runs);
        EXPECT_EQ(runs.size(), 1u) << "one unbroken offset curve at d=" << d;
    }
}

// #182 (Iron47, b861): offsetting a triangle INWARD by more than it can take turns it
// inside out; the flipped edges sit outside the triangle, farther than |d| from it,
// and used to survive as three stray corner pieces. The offset has to lie on the
// side the sign asks for, so there is no result at all.
TEST(SketchOffsetClosed, InwardOffsetPastTheInradiusGivesNothing) {
    Sketch sk;
    const int a = sk.addPoint({-9, 15.5885f}), b = sk.addPoint({-9, -15.5885f}),
              c = sk.addPoint({18, 0});
    sk.addLine(a, b); sk.addLine(b, c); sk.addLine(c, a);
    OffsetChain ch = walkOffsetChain(sk, {-9, 0}, 0.5f);
    ASSERT_TRUE(ch.valid());
    ASSERT_TRUE(ch.closed);
    // Inradius is 9 mm. Which sign is "inward" depends on travel direction, so try
    // both: the one that goes inside must be refused beyond 9, the other grows.
    int refused = 0, grew = 0;
    for (float d : {-42.07f, 42.07f}) {
        for (OffsetCorners oc : {OffsetCorners::Sharp, OffsetCorners::Round}) {
            OffsetResult res = offsetChain(ch, d, oc);
            pruneOffset(res, ch, d);
            if (!res.valid) { ++refused; continue; }
            ++grew;
            std::vector<std::vector<glm::vec2>> runs;
            densifySegs(res.segs, runs);
            EXPECT_EQ(runs.size(), 1u) << "outward offset is one closed loop, d=" << d;
        }
    }
    EXPECT_EQ(refused, 2);
    EXPECT_EQ(grew, 2);
}

// #182 (Iron47, b862): around a triangle the Offset preview jumped to an inward offset
// (and said the distance was too big) when the cursor was in part of the wedge beyond a
// corner. A 60-degree corner has a 120-degree outside wedge, wider than one edge's
// normal covers. Every cursor position outside the triangle reads as outside, on the
// same side, at the distance to the nearest corner or edge.
TEST(SketchOffsetClosed, CursorBeyondATriangleCornerIsAlwaysOutside) {
    Sketch sk;
    const int a = sk.addPoint({-7, 12.124f}), b = sk.addPoint({-7, -12.124f}), c = sk.addPoint({14, 0});
    sk.addLine(a, b); sk.addLine(b, c); sk.addLine(c, a);
    OffsetChain ch = walkOffsetChain(sk, {-7, 0}, 0.5f);
    ASSERT_TRUE(ch.valid());
    const float outside = signedDistanceToChain(ch, {100, 100}) > 0 ? 1.0f : -1.0f;
    for (glm::vec2 corner : {glm::vec2(-7, 12.124f), glm::vec2(-7, -12.124f), glm::vec2(14, 0)}) {
        for (int deg = 0; deg < 360; deg += 5) {
            const float r = 3.0f, ang = static_cast<float>(deg) * 3.14159265f / 180.0f;
            const glm::vec2 q = corner + glm::vec2(std::cos(ang), std::sin(ang)) * r;
            const float d = signedDistanceToChain(ch, q);
            EXPECT_NEAR(std::abs(d), distanceToChain(ch, q), 1e-3f);
            // The directions that land inside the triangle itself are not the point here.
            const bool insideTri = q.x > -7.0f && q.x < 14.0f &&
                                   std::abs(q.y) < (14.0f - q.x) / 1.7320508f;
            if (!insideTri && std::abs(d) > 1e-3f)   // a point ON an edge has no side
                EXPECT_GT(d * outside, 0.0f) << "corner (" << corner.x << "," << corner.y
                                              << ") at " << deg << " deg read as inside";
        }
    }
}

// #182 (Iron47, b862): where a spline bends about as tightly as the offset distance, the
// offset grazes a swallowtail and the stretch it loses is bridged by no crossing of the
// pieces either side: a gap of a few tenths of a millimetre at one distance (+2.5 here).
TEST(SketchOffsetSpline, GrazedSwallowtailLeavesNoGap) {
    Sketch sk;
    const glm::vec2 ctrl[] = {
        {55, -14}, {53, -22.5167f}, {52, -15}, {51, -22.5167f}, {48, -14}, {46, -22}, {46, -15},
        {43.8564f, -23}, {39.9808f, -15}, {39, -25}, {34, -19}, {27, -28}, {15, -18.6667f},
        {13, -31}, {9, -19}, {5, -29}, {5, -22}, {1, -26}, {-1, -21}, {-6.81056f, -26},
        {-8.6862f, -19}, {-15, -26}, {-13.3923f, -20}, {-22, -22.3064f}, {-14, -16.1658f},
        {-26, -16.1658f}, {-21, -13.2791f}, {-31, -12}, {-23, -6}, {-27, -2}};
    std::vector<int> ids;
    for (const auto& c : ctrl) ids.push_back(sk.addPoint(c));
    sk.addSpline(ids);
    const auto samples = sk.sampleSpline2D(sk.getSplines()[0], 6);
    OffsetChain ch = walkOffsetChain(sk, samples[samples.size() / 2], 0.5f);
    ASSERT_TRUE(ch.valid());
    for (float d = -6.f; d <= 6.f; d += 0.1f) {
        if (std::abs(d) < 0.01f) continue;
        for (OffsetCorners oc : {OffsetCorners::Round, OffsetCorners::Sharp}) {
            OffsetResult res = offsetChain(ch, d, oc);
            pruneOffset(res, ch, d);
            ASSERT_TRUE(res.valid) << "d=" << d;
            std::vector<std::vector<glm::vec2>> runs;
            densifySegs(res.segs, runs);
            EXPECT_EQ(runs.size(), 1u) << "one unbroken offset curve at d=" << d;
        }
    }
}
