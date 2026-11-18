// A chamfer size that cannot fit must be REFUSED, not drawn as junk.
//
// Tester 2026-11-17 (beta 6): chamfer a short edge and it behaves up to ~0.7 mm,
// goes strange from 0.8 to 1 mm, then the preview simply vanishes past 1 mm; two
// edges break from 0.7 mm on. Cause: when native ChFi3d can't build (two bevels
// collide, or the setback is as wide as the face beside the edge) the cut/fill
// FALLBACKS still "succeeded" and returned valid-looking junk - 14 to 18 extra
// faces, with the volume going DOWN as the size went up.
//
// The fallbacks now have to move at least as much material as the largest
// smaller bevel that does build natively on the same edges.
//
// Fixture: tests/data/chamfer_overlap_pre.brep is just the solid from the
// tester's project (a thin-walled channel, 1 mm walls) - geometry only, no
// project data. The two edges are the 1 mm edges meeting at one corner.

#include "modeling/ChamferOp.h"
#include "core/Document.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Curve.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <cmath>
#include <vector>

#ifndef MZR_TEST_DATA_DIR
#error "MZR_TEST_DATA_DIR must point at tests/data"
#endif

using namespace materializr;

namespace {

TopoDS_Shape channel() {
    TopoDS_Shape s;
    BRep_Builder b;
    const std::string path = std::string(MZR_TEST_DATA_DIR) + "/chamfer_overlap_pre.brep";
    EXPECT_TRUE(BRepTools::Read(s, path.c_str(), b)) << path;
    return s;
}

double volume(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

int faceCount(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
}

bool near(const gp_Pnt& p, double x, double y, double z) {
    return std::abs(p.X() - x) < 1e-6 && std::abs(p.Y() - y) < 1e-6 && std::abs(p.Z() - z) < 1e-6;
}

// The edge between two points, either direction.
TopoDS_Edge edgeBetween(const TopoDS_Shape& body, gp_Pnt a, gp_Pnt b) {
    TopTools_IndexedMapOfShape unique;
    TopExp::MapShapes(body, TopAbs_EDGE, unique);
    for (int i = 1; i <= unique.Extent(); ++i) {
        TopoDS_Edge e = TopoDS::Edge(unique(i));
        BRepAdaptor_Curve c(e);
        if (c.GetType() != GeomAbs_Line) continue;
        const gp_Pnt p = c.Value(c.FirstParameter()), q = c.Value(c.LastParameter());
        if ((near(p, a.X(), a.Y(), a.Z()) && near(q, b.X(), b.Y(), b.Z())) ||
            (near(q, a.X(), a.Y(), a.Z()) && near(p, b.X(), b.Y(), b.Z())))
            return e;
    }
    return {};
}

struct Result { bool ok; int dFaces; double dVol; bool valid; };

// pick: 0 = the vertical edge, 1 = the horizontal one, both = {0, 1}.
Result runFresh(const std::vector<int>& pick, double d) {
    Document doc;
    doc.addBody(channel(), "b");
    const TopoDS_Shape pre = doc.getBody(1);
    const TopoDS_Edge e0 = edgeBetween(pre, gp_Pnt(-9, 10, 18), gp_Pnt(-9, 10, 19));
    const TopoDS_Edge e1 = edgeBetween(pre, gp_Pnt(-9, 10, 19), gp_Pnt(-8, 10, 19));
    EXPECT_FALSE(e0.IsNull());
    EXPECT_FALSE(e1.IsNull());
    std::vector<TopoDS_Edge> edges;
    for (int i : pick) edges.push_back(i == 0 ? e0 : e1);

    ChamferOp op;
    op.setBody(1);
    op.setEdges(edges);
    op.setDistance(d);
    Result r{op.execute(doc), 0, 0.0, false};
    if (r.ok) {
        const TopoDS_Shape after = doc.getBody(1);
        r.dFaces = faceCount(after) - faceCount(pre);
        r.dVol = volume(after) - volume(pre);
        r.valid = BRepCheck_Analyzer(after).IsValid();
    }
    return r;
}

} // namespace

TEST(ChamferOverlap, FixtureLoads) {
    const TopoDS_Shape s = channel();
    ASSERT_FALSE(s.IsNull());
    EXPECT_EQ(faceCount(s), 14);
}

TEST(ChamferOverlap, TwoEdgesBuildCleanlyWhileTheyFit) {
    for (double d : {0.2, 0.5, 0.7}) {
        const Result r = runFresh({0, 1}, d);
        ASSERT_TRUE(r.ok) << "d=" << d;
        EXPECT_TRUE(r.valid);
        EXPECT_EQ(r.dFaces, 2) << "one bevel face per edge, d=" << d;
        EXPECT_GT(r.dVol, 0.0);   // a concave bevel adds material
    }
}

TEST(ChamferOverlap, TwoEdgesAreRefusedWhenTheyNoLongerFit) {
    // Beta 6 returned +14..+18 faces here, with LESS volume than at 0.7 mm.
    for (double d : {0.75, 0.8, 0.9, 1.0, 1.2, 5.0, 20.0}) {
        const Result r = runFresh({0, 1}, d);
        EXPECT_FALSE(r.ok) << "d=" << d << " built faces+" << r.dFaces << " vol+" << r.dVol;
    }
}

TEST(ChamferOverlap, OneEdgeGrowsSmoothly) {
    double last = 0.0;
    for (double d : {0.3, 0.5, 0.7, 0.8, 0.9}) {
        const Result r = runFresh({0}, d);
        ASSERT_TRUE(r.ok) << "d=" << d;
        EXPECT_TRUE(r.valid);
        EXPECT_EQ(r.dFaces, 1) << "d=" << d;
        EXPECT_GT(r.dVol, last) << "a bigger bevel must move more material (d=" << d << ")";
        last = r.dVol;
    }
    // Exactly as wide as the face beside the edge: if it builds at all it must
    // not be the junk the fallback used to return (volume below the 0.9 mm one).
    const Result edge = runFresh({0}, 1.0);
    if (edge.ok) {
        EXPECT_TRUE(edge.valid);
        EXPECT_GE(edge.dVol, last);
    }
}

TEST(ChamferOverlap, OneEdgeIsRefusedPastTheFaceBesideIt) {
    for (double d : {1.05, 1.2, 2.0, 20.0}) EXPECT_FALSE(runFresh({0}, d).ok) << "d=" << d;
}
