// SVG export -> re-import round trip. Steve's report: exporting a sketch and
// re-importing it "was drawing lines on top of each other and not recognizing
// a closed loop proper" - the old exporter wrote every line as its own
// two-point <path>, so a drawn loop arrived at import as disjoint fragments
// (duplicate corner points, no closure, no regions). The exporter now walks
// connected chains by shared sketch-point id and emits ONE closed path per
// loop, true <circle> elements and true arc (A) commands. These tests pin the
// contract: loops come back closed, with shared corners, no duplicates, and
// circles come back as native circles.

#include "modeling/Sketch.h"
#include "modeling/SvgImport.h"
#include "io/SvgExport.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <memory>
#include "test_tmp_path.h"

using materializr::Sketch;
using materializr::SvgExport;
using materializr::SvgImport;
using materializr::SvgPaths;

namespace {

int countNonFromTextPoints(const Sketch& sk) {
    int n = 0;
    for (const auto& p : sk.getPoints())
        if (!p.fromText) ++n;
    return n;
}

} // namespace

TEST(SvgRoundTrip, ClosedLineLoopStaysOneClosedLoop) {
    // An M-ish non-convex octagon - the logo case: 8 lines sharing corners.
    Sketch sk;
    const float V[8][2] = {{0, 0}, {20, 0}, {20, 14}, {14, 14},
                           {14, 6}, {6, 6}, {6, 14}, {0, 14}};
    int pid[8];
    for (int i = 0; i < 8; ++i) pid[i] = sk.addPoint({V[i][0], V[i][1]});
    for (int i = 0; i < 8; ++i) sk.addLine(pid[i], pid[(i + 1) % 8]);

    const std::string path = mzrtest::tmpPath("mtz_svg_rt_loop.svg");
    auto res = SvgExport::exportSketch(path, sk);
    ASSERT_TRUE(res.success) << res.errorMessage;
    EXPECT_EQ(res.curveCount, 1) << "one connected loop must export as ONE path";

    SvgPaths svg;
    ASSERT_TRUE(SvgImport::load(path, svg));
    std::remove(path.c_str());
    ASSERT_EQ(svg.loops.size(), 1u) << "re-import must see a single loop";
    EXPECT_TRUE(svg.closed[0]) << "the loop must arrive CLOSED";

    // Place at 1:1 (widthMm == the artwork's own width) and check recovery:
    // 8 straight runs -> 8 lines between 8 SHARED corner points, no dupes.
    Sketch out;
    ASSERT_GT(SvgImport::place(&out, svg, {0, 0}, svg.size().x, 0.0f), 0);
    EXPECT_EQ(static_cast<int>(out.getLines().size()), 8)
        << "no duplicated / fragmented segments";
    EXPECT_EQ(countNonFromTextPoints(out), 8)
        << "corners must be shared single points";
    EXPECT_TRUE(out.getSplines().empty());
    EXPECT_TRUE(out.getCircles().empty());
}

TEST(SvgRoundTrip, CircleComesBackAsNativeCircle) {
    Sketch sk;
    int c = sk.addPoint({10.0f, 10.0f});
    sk.addCircle(c, 7.0);

    const std::string path = mzrtest::tmpPath("mtz_svg_rt_circle.svg");
    auto res = SvgExport::exportSketch(path, sk);
    ASSERT_TRUE(res.success) << res.errorMessage;

    SvgPaths svg;
    ASSERT_TRUE(SvgImport::load(path, svg));
    std::remove(path.c_str());
    Sketch out;
    ASSERT_GT(SvgImport::place(&out, svg, {0, 0}, svg.size().x, 0.0f), 0);
    ASSERT_EQ(out.getCircles().size(), 1u)
        << "a circle must round-trip as a native circle, not a polygon";
    EXPECT_NEAR(out.getCircles()[0].radius, 7.0, 0.05);
    EXPECT_TRUE(out.getLines().empty());
}

TEST(SvgRoundTrip, LineArcProfileStaysClosed) {
    // A rectangle whose top-right corner is a quarter-round: 4 lines + 1 arc,
    // all sharing endpoints - the mixed-element closure case.
    Sketch sk;
    int p0 = sk.addPoint({0, 0});
    int p1 = sk.addPoint({20, 0});
    int p2 = sk.addPoint({20, 10});   // arc start (right side, below corner)
    int p3 = sk.addPoint({15, 15});   // arc end (top side, left of corner)
    int p4 = sk.addPoint({0, 15});
    int ctr = sk.addPoint({15, 10});  // arc centre
    sk.addLine(p0, p1);
    sk.addLine(p1, p2);
    sk.addArc(ctr, p2, p3, 5.0);
    sk.addLine(p3, p4);
    sk.addLine(p4, p0);

    const std::string path = mzrtest::tmpPath("mtz_svg_rt_arc.svg");
    auto res = SvgExport::exportSketch(path, sk);
    ASSERT_TRUE(res.success) << res.errorMessage;
    EXPECT_EQ(res.curveCount, 1) << "lines + arc sharing endpoints = ONE path";

    SvgPaths svg;
    ASSERT_TRUE(SvgImport::load(path, svg));
    std::remove(path.c_str());
    ASSERT_EQ(svg.loops.size(), 1u);
    EXPECT_TRUE(svg.closed[0]) << "mixed line/arc loop must arrive closed";

    Sketch out;
    ASSERT_GT(SvgImport::place(&out, svg, {0, 0}, svg.size().x, 0.0f), 0);
    // Recovery splits at the 4 sharp corners: straight runs stay lines, the
    // rounded corner comes back as a smooth run. What matters: nothing
    // exploded into hundreds of fragments and the loop is region-capable.
    EXPECT_LE(static_cast<int>(out.getLines().size()), 6);
    // 3 sharp corners: the arc is TANGENT to both neighbours, so its joints
    // are correctly smooth, not corners.
    EXPECT_GE(countNonFromTextPoints(out), 3);
}

TEST(SvgRoundTrip, SplineSurvives) {
    Sketch sk;
    std::vector<int> ids;
    ids.push_back(sk.addPoint({0, 0}));
    ids.push_back(sk.addPoint({5, 8}));
    ids.push_back(sk.addPoint({12, -3}));
    ids.push_back(sk.addPoint({20, 4}));
    sk.addSpline(ids);

    const std::string path = mzrtest::tmpPath("mtz_svg_rt_spline.svg");
    auto res = SvgExport::exportSketch(path, sk);
    ASSERT_TRUE(res.success) << res.errorMessage;

    SvgPaths svg;
    ASSERT_TRUE(SvgImport::load(path, svg));
    std::remove(path.c_str());
    ASSERT_EQ(svg.loops.size(), 1u);

    Sketch out;
    ASSERT_GT(SvgImport::place(&out, svg, {0, 0}, svg.size().x, 0.0f), 0);
    EXPECT_GE(out.getSplines().size(), 1u)
        << "a smooth open run must come back as a spline, not chord soup";
    EXPECT_LE(out.getLines().size(), 2u);
}
