// Trim on a spline or a polygon edge cuts it between its crossings instead of
// deleting the whole element. A reported bug: someone needed to trim part of a
// spline and the tool could only erase all of it.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/Sketch.h"
#include "modeling/SketchConstraints.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using namespace materializr;

namespace {

SketchTool makeTrimTool(Sketch& sk, SketchSolver& solver) {
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setSolver(&solver);
    tool.setGridStep(1.0f);
    tool.setMode(SketchToolMode::Trim);
    return tool;
}

int addSplineThrough(Sketch& sk, const std::vector<glm::vec2>& pts) {
    std::vector<int> ids;
    for (auto p : pts) ids.push_back(sk.addPoint(p));
    return sk.addSpline(ids);
}

int addSeg(Sketch& sk, glm::vec2 a, glm::vec2 b) {
    return sk.addLine(sk.addPoint(a), sk.addPoint(b));
}

const SketchSpline* findSpline(const Sketch& sk, int id) {
    for (const auto& s : sk.getSplines()) if (s.id == id) return &s;
    return nullptr;
}

glm::vec2 firstPt(const Sketch& sk, const SketchSpline& s) { return sk.getPoint(s.controlPointIds.front())->pos; }
glm::vec2 lastPt(const Sketch& sk, const SketchSpline& s) { return sk.getPoint(s.controlPointIds.back())->pos; }

// A point on spline `s` at fraction f (0..1) of its sampled polyline.
glm::vec2 onCurve(const Sketch& sk, const SketchSpline& s, float f) {
    auto samp = sk.sampleSpline2D(s, 24);
    return samp[static_cast<size_t>(f * static_cast<float>(samp.size() - 1))];
}

// Largest distance from any sample of `piece` to the polyline `orig`.
float maxDeviation(const std::vector<glm::vec2>& piece, const std::vector<glm::vec2>& orig) {
    float worst = 0.0f;
    for (auto q : piece) {
        float best = 1e30f;
        for (size_t i = 0; i + 1 < orig.size(); ++i) {
            glm::vec2 d = orig[i + 1] - orig[i];
            float l2 = glm::dot(d, d);
            float t = l2 > 0 ? std::clamp(glm::dot(q - orig[i], d) / l2, 0.0f, 1.0f) : 0.0f;
            best = std::min(best, glm::length(q - (orig[i] + t * d)));
        }
        worst = std::max(worst, best);
    }
    return worst;
}

double faceArea(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::SurfaceProperties(s, g); return g.Mass();
}
double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

// An S-ish open spline running left to right over x in [0,20].
const std::vector<glm::vec2> kWave = {{0, 0}, {5, 4}, {10, 0}, {15, 4}, {20, 0}};

} // namespace

// Two verticals cross the spline; clicking the stretch between them keeps both
// outer ends, each ending exactly on its line.
TEST(SketchTrimSpline, MiddleStretchRemovedBothEndsSurvive) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    int sid = addSplineThrough(sk, kWave);
    const SketchSpline orig = *findSpline(sk, sid);
    const auto origSamp = sk.sampleSpline2D(orig, 24);
    addSeg(sk, {7, -10}, {7, 10});
    addSeg(sk, {13, -10}, {13, 10});

    tool.onMouseDown(onCurve(sk, orig, 0.5f));

    ASSERT_EQ(sk.getSplines().size(), 2u);
    EXPECT_EQ(findSpline(sk, sid), nullptr) << "original spline should be replaced";
    const SketchSpline* left = nullptr; const SketchSpline* right = nullptr;
    for (const auto& s : sk.getSplines()) {
        if (firstPt(sk, s).x < 1.0f) left = &s; else right = &s;
    }
    ASSERT_NE(left, nullptr); ASSERT_NE(right, nullptr);
    EXPECT_NEAR(lastPt(sk, *left).x, 7.0f, 1e-3f);
    EXPECT_NEAR(firstPt(sk, *right).x, 13.0f, 1e-3f);
    EXPECT_NEAR(lastPt(sk, *right).x, 20.0f, 1e-6f);
    // The cut point lies on the old curve.
    EXPECT_LT(maxDeviation({lastPt(sk, *left), firstPt(sk, *right)}, origSamp), 0.05f);
    // Original control points that survive are kept (same ids), not copies.
    EXPECT_EQ(left->controlPointIds.front(), orig.controlPointIds.front());
    EXPECT_EQ(right->controlPointIds.back(), orig.controlPointIds.back());
    // The survivors stay close to the curve they were cut from.
    float dl = maxDeviation(sk.sampleSpline2D(*left, 24), origSamp);
    float dr = maxDeviation(sk.sampleSpline2D(*right, 24), origSamp);
    std::printf("[trim-spline] survivor deviation: left %.3f right %.3f (curve is 20 x 4)\n", dl, dr);
    EXPECT_LT(dl, 0.6f);
    EXPECT_LT(dr, 0.6f);
    // Cutters untouched.
    EXPECT_EQ(sk.getLines().size(), 2u);
}

// Clicking the overhanging end leaves the middle and the other end as
// two splines (the middle cut at both its crossings).
TEST(SketchTrimSpline, EndStretchRemovedRestSurvives) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    int sid = addSplineThrough(sk, kWave);
    const SketchSpline orig = *findSpline(sk, sid);
    addSeg(sk, {7, -10}, {7, 10});
    addSeg(sk, {13, -10}, {13, 10});

    tool.onMouseDown(onCurve(sk, orig, 0.1f)); // left end, x ~ 2

    ASSERT_EQ(sk.getSplines().size(), 2u);
    float minX = 1e9f, maxX = -1e9f;
    for (const auto& s : sk.getSplines()) {
        minX = std::min({minX, firstPt(sk, s).x, lastPt(sk, s).x});
        maxX = std::max({maxX, firstPt(sk, s).x, lastPt(sk, s).x});
    }
    EXPECT_NEAR(minX, 7.0f, 1e-3f) << "left end should be gone";
    EXPECT_NEAR(maxX, 20.0f, 1e-6f);
}

// A spline nothing crosses is still removed whole.
TEST(SketchTrimSpline, UncrossedSplineIsDeletedWhole) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    int sid = addSplineThrough(sk, kWave);
    addSeg(sk, {50, 0}, {60, 0});
    tool.onMouseDown(onCurve(sk, *findSpline(sk, sid), 0.5f));
    EXPECT_TRUE(sk.getSplines().empty());
    EXPECT_EQ(sk.getLines().size(), 1u);
}

// Another spline works as the cutter.
TEST(SketchTrimSpline, CrossedBySecondSpline) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    int a = addSplineThrough(sk, {{0, 0}, {10, 1}, {20, 0}});
    int b = addSplineThrough(sk, {{10, -8}, {10.5, 0}, {10, 8}});
    const SketchSpline origA = *findSpline(sk, a);
    tool.onMouseDown(onCurve(sk, origA, 0.9f)); // right of the crossing
    ASSERT_EQ(sk.getSplines().size(), 2u);
    ASSERT_NE(findSpline(sk, b), nullptr) << "the cutter must be untouched";
    // The survivor of `a` runs from x=0 to the crossing near x=10.2.
    bool found = false;
    for (const auto& s : sk.getSplines()) {
        if (s.id == b) continue;
        found = true;
        EXPECT_NEAR(firstPt(sk, s).x, 0.0f, 1e-6f);
        EXPECT_GT(lastPt(sk, s).x, 9.5f);
        EXPECT_LT(lastPt(sk, s).x, 11.0f);
    }
    EXPECT_TRUE(found);
}

// The spline's own loop: a curve crossing itself is cut at the crossing, so the
// loop can be clicked away and the tail kept (the reported sketch had one).
TEST(SketchTrimSpline, SelfCrossingLoopCanBeTrimmed) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    // A teardrop: runs right, loops up and back across its own path.
    int sid = addSplineThrough(sk, {{0, 0}, {6, 0}, {10, 3}, {8, 7}, {4, 4}, {6, -3}, {12, -3}});
    const SketchSpline orig = *findSpline(sk, sid);
    auto samp = sk.sampleSpline2D(orig, 24);
    // Confirm the fixture really self-intersects (else the test proves nothing).
    bool crosses = false;
    for (size_t i = 0; i + 1 < samp.size() && !crosses; ++i)
        for (size_t j = i + 2; j + 1 < samp.size(); ++j) {
            glm::vec2 d1 = samp[i + 1] - samp[i], d2 = samp[j + 1] - samp[j];
            float den = d1.x * d2.y - d1.y * d2.x;
            if (std::abs(den) < 1e-9f) continue;
            glm::vec2 o = samp[j] - samp[i];
            float t = (o.x * d2.y - o.y * d2.x) / den, u = (o.x * d1.y - o.y * d1.x) / den;
            if (t >= 0 && t <= 1 && u >= 0 && u <= 1) { crosses = true; break; }
        }
    ASSERT_TRUE(crosses) << "fixture does not self-cross";

    tool.onMouseDown({8.0f, 7.0f}); // top of the loop (a control point, on the curve)
    EXPECT_FALSE(sk.getSplines().empty()) << "loop click erased the whole spline";
    // Whatever remains is shorter than the original and nothing is left at the loop apex.
    for (const auto& s : sk.getSplines()) {
        for (auto q : sk.sampleSpline2D(s, 24))
            EXPECT_GT(glm::length(q - glm::vec2(8.0f, 7.0f)), 0.5f);
    }
}

// A closed spline cut by a line loses the stretch under the cursor and the rest
// becomes one open curve.
TEST(SketchTrimSpline, ClosedSplineBecomesOneOpenCurve) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    std::vector<int> ids;
    for (int i = 0; i < 8; ++i) {
        float a = 6.2831853f * static_cast<float>(i) / 8.0f;
        ids.push_back(sk.addPoint({10.0f * std::cos(a), 10.0f * std::sin(a)}));
    }
    ids.push_back(ids.front());
    int sid = sk.addSpline(ids);
    const SketchSpline orig = *findSpline(sk, sid);
    addSeg(sk, {0, -20}, {0, 20}); // crosses the ring at (0,+-10)

    tool.onMouseDown({10.0f, 0.0f}); // the right-hand half
    ASSERT_EQ(sk.getSplines().size(), 1u);
    const SketchSpline& s = sk.getSplines().front();
    EXPECT_NE(s.controlPointIds.front(), s.controlPointIds.back()) << "should be open now";
    for (auto q : sk.sampleSpline2D(s, 24)) EXPECT_LT(q.x, 0.5f) << "right half should be gone";
    EXPECT_NEAR(std::abs(firstPt(sk, s).y), 10.0f, 0.2f);
    EXPECT_NEAR(std::abs(lastPt(sk, s).y), 10.0f, 0.2f);
}

// Hover outlines exactly the stretch that a click would remove.
TEST(SketchTrimSpline, HoverPreviewIsTheStretchBetweenCrossings) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    int sid = addSplineThrough(sk, kWave);
    addSeg(sk, {7, -10}, {7, 10});
    addSeg(sk, {13, -10}, {13, 10});
    tool.onMouseMove(onCurve(sk, *findSpline(sk, sid), 0.5f));
    const auto& hv = tool.getTrimHoverPoints();
    ASSERT_GT(hv.size(), 4u);
    for (auto q : hv) { EXPECT_GE(q.x, 7.0f - 1e-3f); EXPECT_LE(q.x, 13.0f + 1e-3f); }
    EXPECT_NEAR(hv.front().x, 7.0f, 1e-2f);
    EXPECT_NEAR(hv.back().x, 13.0f, 1e-2f);
}

// A line crossing a spline is now cut at the spline, too.
TEST(SketchTrimSpline, SplineCutsALine) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    addSplineThrough(sk, {{0, 5}, {10, 5.5f}, {20, 5}});
    int lid = addSeg(sk, {10, 0}, {10, 12});
    tool.onMouseDown({10.0f, 1.0f}); // below the spline
    for (const auto& l : sk.getLines()) EXPECT_NE(l.id, lid);
    ASSERT_EQ(sk.getLines().size(), 1u);
    const SketchLine& l = sk.getLines().front();
    EXPECT_NEAR(sk.getPoint(l.startPointId)->pos.y, 5.5f, 0.1f);
    EXPECT_NEAR(sk.getPoint(l.endPointId)->pos.y, 12.0f, 1e-4f);
}

// --- polygons ---------------------------------------------------------------

// A line cuts across a hexagon; clicking the left half of the top edge removes
// just that half. The polygon dissolves into plain lines, everything else stays.
TEST(SketchTrimPolygonEdge, EdgeCutAtCrossingKeepsTheRest) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    int center = sk.addPoint({0, 0});
    int pid = sk.addPolygon(center, 10.0, 6, 0.0);
    ASSERT_GE(pid, 0);
    addSeg(sk, {0, -15}, {0, 15});
    const size_t linesBefore = sk.getLines().size(); // 6 edges + the cutter

    // Top edge runs (5,8.66)-(-5,8.66); click its left half.
    tool.onMouseDown({-2.5f, 8.660254f});

    EXPECT_TRUE(sk.getPolygons().empty()) << "polygon should have been dissolved";
    // The clicked edge was split in two at x=0 and the left half dropped; the
    // right half is a new line, so the count is edges-1 (5) + right half + cutter.
    EXPECT_EQ(sk.getLines().size(), linesBefore);
    bool leftHalf = false, rightHalf = false;
    for (const auto& l : sk.getLines()) {
        auto a = sk.getPoint(l.startPointId)->pos, b = sk.getPoint(l.endPointId)->pos;
        if (std::abs(a.y - 8.660254f) < 1e-3f && std::abs(b.y - 8.660254f) < 1e-3f) {
            float lo = std::min(a.x, b.x), hi = std::max(a.x, b.x);
            if (hi <= 0.01f) leftHalf = true;
            if (lo >= -0.01f) rightHalf = true;
        }
    }
    EXPECT_FALSE(leftHalf);
    EXPECT_TRUE(rightHalf);
}

// --- the point of it all: a region that results from a trim extrudes --------

// A rectangle, and a spline running in from outside, across it and out the far
// side. Trim the two overhangs; the spline then divides the rectangle in two,
// and each half extrudes to a solid whose volume is its area x height.
TEST(SketchTrimSpline, RegionsFromATrimmedSplineExtrude) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    int a = sk->addPoint({0, 0}), b = sk->addPoint({10, 0}), c = sk->addPoint({10, 10}), d = sk->addPoint({0, 10});
    sk->addLine(a, b); sk->addLine(b, c); sk->addLine(c, d); sk->addLine(d, a);
    int sid = addSplineThrough(*sk, {{-4, 4}, {0, 5}, {5, 7}, {10, 5}, {14, 4}});
    const SketchSpline orig = *findSpline(*sk, sid);
    SketchSolver solver;
    SketchTool tool = makeTrimTool(*sk, solver);
    tool.onMouseDown(onCurve(*sk, orig, 0.04f)); // overhang on the left
    // The first click cut at both edges: the overhang goes, the middle and the
    // right overhang remain. The right overhang has nothing left to cut at, so
    // a click takes it whole.
    ASSERT_EQ(sk->getSplines().size(), 2u);
    auto dumpSplines = [&](const char* tag) {
        for (const auto& sp : sk->getSplines()) {
            std::printf("[dbg] %s spline %d:", tag, sp.id);
            for (int pid : sp.controlPointIds) {
                auto q = sk->getPoint(pid)->pos;
                std::printf(" #%d(%.6f,%.6f)", pid, q.x, q.y);
            }
            std::printf("\n");
        }
        std::printf("[dbg] %s points=%zu lines=%zu\n", tag, sk->getPoints().size(), sk->getLines().size());
    };
    dumpSplines("after-first-click");
    for (const auto& sp : sk->getSplines()) {
        if (firstPt(*sk, sp).x > 9.9f) {
            auto click = onCurve(*sk, sp, 0.5f);
            std::printf("[dbg] second click on spline %d at (%.6f,%.6f)\n", sp.id, click.x, click.y);
            tool.onMouseDown(click);
            break;
        }
    }
    dumpSplines("after-second-click");
    ASSERT_EQ(sk->getSplines().size(), 1u);
    const SketchSpline& cut = sk->getSplines().front();
    EXPECT_NEAR(firstPt(*sk, cut).x, 0.0f, 0.05f);
    EXPECT_NEAR(lastPt(*sk, cut).x, 10.0f, 0.05f);

    auto regions = sk->buildRegions();
    ASSERT_EQ(regions.size(), 2u) << "the spline should split the rectangle into two regions";
    double areaSum = 0;
    for (const auto& r : regions) areaSum += faceArea(r.face);
    EXPECT_NEAR(areaSum, 100.0, 0.5);

    const int skId = doc.addSketch(sk, "Sketch 1");
    const double h = 4.0;
    for (const auto& r : regions) {
        ExtrudeOp ex;
        ex.setSketchSource(skId);
        ASSERT_TRUE(ex.rebuildProfileFromSketch(doc));
        ex.setProfile(r.face);
        ex.setDistance(h);
        ex.setMode(ExtrudeMode::NewBody);
        ASSERT_TRUE(ex.execute(doc));
    }
    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 2u);
    double volSum = 0;
    for (int id : ids) {
        double v = vol(doc.getBody(id));
        EXPECT_GT(v, 10.0);
        volSum += v;
    }
    EXPECT_NEAR(volSum, 100.0 * h, 2.0);
}

// Trim re-plans on every mouse move, and a traced outline is a spline with
// hundreds of control points (thousands of samples). Hovering one that also
// meets another long spline and itself must stay interactive.
TEST(SketchTrimSpline, HoverOnALongTracedSplineStaysFast) {
    Sketch sk; SketchSolver solver; SketchTool tool = makeTrimTool(sk, solver);
    std::vector<glm::vec2> a, b;
    for (int i = 0; i < 450; ++i) {
        float x = static_cast<float>(i) * 0.5f;
        a.push_back({x, 10.0f * std::sin(x * 0.2f)});
        b.push_back({x, 10.0f * std::cos(x * 0.2f)});
    }
    int sa = addSplineThrough(sk, a);
    addSplineThrough(sk, b);
    const SketchSpline spl = *findSpline(sk, sa);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) tool.onMouseMove(onCurve(sk, spl, 0.3f + 0.1f * static_cast<float>(i)));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5.0;
    std::printf("[trim-spline] long-spline hover plan: %.1f ms per move\n", ms);
    EXPECT_GT(tool.getTrimHoverPoints().size(), 2u);
    EXPECT_LT(ms, 100.0);
}
