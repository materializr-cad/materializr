// Two short chamfer edges on the SAME line, far apart, built in one op.
//
// Tester 2026-11-17 (1.8.0b6_3_1): chamfering the four 1 mm wall-base edges of
// a channel one pair at a time gave a clean result; selecting all four in one
// op produced a band joining the two bevels. The native blend was fine - the
// "partial taper" coverage check rejected it. It projects generated-face
// vertices onto each edge's line, and the far edge's faces sat inside its
// perpendicular filter, so their vertices projected 16+ mm past the end of a
// 1 mm edge and opened a fake gap. The fallback then built a corner-fill ramp.
// Fixed by clamping the projection to the edge's own extent.

#include "modeling/ChamferOp.h"
#include "core/Document.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <cmath>
#include <string>

using namespace materializr;

namespace {

int faceCount(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
}

double volume(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

// 30 x 10 x 2 slab with two 1 x 1 x 3 pillars standing on it at the same y.
TopoDS_Shape slabWithPillars() {
    TopoDS_Shape slab = BRepPrimAPI_MakeBox(30.0, 10.0, 2.0).Shape();
    TopoDS_Shape p1 = BRepPrimAPI_MakeBox(gp_Pnt(2, 4, 2), 1.0, 1.0, 3.0).Shape();
    TopoDS_Shape p2 = BRepPrimAPI_MakeBox(gp_Pnt(25, 4, 2), 1.0, 1.0, 3.0).Shape();
    return BRepAlgoAPI_Fuse(BRepAlgoAPI_Fuse(slab, p1).Shape(), p2).Shape();
}

// The concave pillar-base edge on the y == 4 side: 1 mm, along X, at z == 2.
std::vector<TopoDS_Edge> baseEdges(const TopoDS_Shape& body) {
    std::vector<TopoDS_Edge> out;
    TopTools_IndexedMapOfShape unique;   // an explorer visits an edge once per face
    TopExp::MapShapes(body, TopAbs_EDGE, unique);
    for (int i = 1; i <= unique.Extent(); ++i) {
        TopoDS_Edge e = TopoDS::Edge(unique(i));
        BRepAdaptor_Curve c(e);
        if (c.GetType() != GeomAbs_Line) continue;
        const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
        if (std::abs(a.Y() - 4.0) > 1e-6 || std::abs(b.Y() - 4.0) > 1e-6) continue;
        if (std::abs(a.Z() - 2.0) > 1e-6 || std::abs(b.Z() - 2.0) > 1e-6) continue;
        if (std::abs(std::abs(b.X() - a.X()) - 1.0) > 1e-6) continue;
        out.push_back(e);
    }
    return out;
}

} // namespace

TEST(ChamferCollinearEdges, TwoFarApartEdgesBuildNativelyInOneOp) {
    Document doc;
    const int id = doc.addBody(slabWithPillars(), "b");
    const TopoDS_Shape before = doc.getBody(id);
    const auto edges = baseEdges(before);
    ASSERT_EQ(edges.size(), 2u);

    ChamferOp op;
    op.setBody(id);
    op.setEdges(edges);
    op.setDistance(0.7);
    testing::internal::CaptureStderr();
    const bool ok = op.execute(doc);
    const std::string log = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(ok);
    // On this shape nothing else can build, so a wrongly benched native blend
    // is kept anyway and the geometry checks below still pass: the log is what
    // proves the coverage check did not misfire.
    EXPECT_EQ(log.find("partial taper"), std::string::npos) << log;

    const TopoDS_Shape after = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(after).IsValid());
    // One bevel face per edge and nothing else: the fallback ramp added a
    // whole extra band of faces between the two bevels.
    EXPECT_EQ(faceCount(after), faceCount(before) + 2);
    // Each concave bevel adds a 0.7 x 0.7 triangle prism, 1 mm long.
    EXPECT_NEAR(volume(after) - volume(before), 2 * 0.5 * 0.7 * 0.7 * 1.0, 1e-6);
}

TEST(ChamferCollinearEdges, SameResultAsOneEdgeAtATime) {
    Document one;
    const int a = one.addBody(slabWithPillars(), "b");
    Document both;
    const int b = both.addBody(slabWithPillars(), "b");
    ChamferOp opBoth;
    opBoth.setBody(b);
    opBoth.setEdges(baseEdges(both.getBody(b)));
    opBoth.setDistance(0.7);
    ASSERT_TRUE(opBoth.execute(both));

    // First pillar only, then the second on the result.
    auto first = baseEdges(one.getBody(a));
    ASSERT_EQ(first.size(), 2u);
    ChamferOp op1;
    op1.setBody(a);
    op1.setEdges({first[0]});
    op1.setDistance(0.7);
    ASSERT_TRUE(op1.execute(one));
    auto rest = baseEdges(one.getBody(a));
    ASSERT_EQ(rest.size(), 1u);
    ChamferOp op2;
    op2.setBody(a);
    op2.setEdges(rest);
    op2.setDistance(0.7);
    ASSERT_TRUE(op2.execute(one));

    EXPECT_EQ(faceCount(both.getBody(b)), faceCount(one.getBody(a)));
    EXPECT_NEAR(volume(both.getBody(b)), volume(one.getBody(a)), 1e-6);
}
