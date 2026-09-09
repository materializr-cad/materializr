// Rung 1: the working EdgeAnchor, migrated behind the topo registry as the
// "sketchedge" scheme. These prove the migration preserves what already works -
// edges mint through the registry, resolveSet claims DISTINCT edges, and the
// set survives a sketch dimension edit (best-first picks the generative
// scheme over the now-stale ordinal fallback) - with no change to EdgeAnchor
// or FilletOp.

#include "modeling/TopoName.h"
#include "core/Document.h"
#include "modeling/Sketch.h"
#include "modeling/ExtrudeOp.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Curve.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <cmath>
#include <memory>

using materializr::Sketch;
using namespace materializr;

namespace {

std::shared_ptr<Sketch> makeRect(double w, double h, int pid[4]) {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    pid[0] = sk->addPoint({0.0f, 0.0f});
    pid[1] = sk->addPoint({(float)w, 0.0f});
    pid[2] = sk->addPoint({(float)w, (float)h});
    pid[3] = sk->addPoint({0.0f, (float)h});
    sk->addLine(pid[0], pid[1]);
    sk->addLine(pid[1], pid[2]);
    sk->addLine(pid[2], pid[3]);
    sk->addLine(pid[3], pid[0]);
    return sk;
}

TopoDS_Edge verticalEdgeAt(const TopoDS_Shape& body, double x, double y) {
    for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.GetType() != GeomAbs_Line) continue;
        if (std::abs(c.Line().Direction().Dot(gp_Dir(0, 0, 1))) < 0.999) continue;
        gp_Pnt m = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
        if (std::hypot(m.X() - x, m.Y() - y) < 1e-6) return TopoDS::Edge(ex.Current());
    }
    return {};
}

// XY of a vertical edge's midpoint (to check it followed the resize).
bool verticalXY(const TopoDS_Edge& e, double& x, double& y) {
    if (e.IsNull()) return false;
    BRepAdaptor_Curve c(e);
    gp_Pnt m = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
    x = m.X(); y = m.Y();
    return true;
}

int extrudeBox(Document& doc, std::shared_ptr<Sketch> sk) {
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    EXPECT_TRUE(ext.rebuildProfileFromSketch(doc));
    EXPECT_TRUE(ext.execute(doc));
    return doc.getAllBodyIds().front();
}

} // namespace

// A single edge mints a sketchedge name (EdgeAnchor blob) PLUS the ordinal
// fallback, and both resolve back to it on the unchanged body.
TEST(TopoEdge, MintsSketchEdgeAndOrdinal) {
    Document doc;
    int pid[4];
    int body = extrudeBox(doc, makeRect(20.0, 10.0, pid));

    topo::Context ctx; ctx.doc = &doc; ctx.shape = doc.getBody(body);
    ctx.type = TopAbs_EDGE;

    TopoDS_Edge corner = verticalEdgeAt(doc.getBody(body), 20.0, 10.0);
    ASSERT_FALSE(corner.IsNull());

    topo::Ref ref = topo::mint(corner, ctx);
    ASSERT_GE(ref.names.size(), 2u);
    EXPECT_EQ(ref.names.front().scheme, "sketchedge");           // most robust first
    EXPECT_EQ(ref.names.front().payload.rfind("v2", 0), 0u);     // EdgeAnchor blob format
    EXPECT_EQ(ref.names.back().scheme, "ordinal");

    TopoDS_Shape out;
    ASSERT_TRUE(topo::resolve(ref, ctx, out));
    EXPECT_TRUE(out.IsSame(corner));
}

// resolveSet claims DISTINCT edges for a whole set on the unchanged body.
TEST(TopoEdge, ResolveSetDistinctOnSameBody) {
    Document doc;
    int pid[4];
    int body = extrudeBox(doc, makeRect(20.0, 10.0, pid));
    topo::Context ctx; ctx.doc = &doc; ctx.shape = doc.getBody(body);
    ctx.type = TopAbs_EDGE;

    TopoDS_Edge c1 = verticalEdgeAt(doc.getBody(body), 20.0, 10.0);
    TopoDS_Edge c2 = verticalEdgeAt(doc.getBody(body), 0.0, 0.0);
    ASSERT_FALSE(c1.IsNull()); ASSERT_FALSE(c2.IsNull());

    std::vector<topo::Ref> refs{ topo::mint(c1, ctx), topo::mint(c2, ctx) };
    std::vector<TopoDS_Shape> out;
    ASSERT_TRUE(topo::resolveSet(refs, ctx, out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_FALSE(out[0].IsSame(out[1]));            // distinct
    EXPECT_TRUE(out[0].IsSame(c1));
    EXPECT_TRUE(out[1].IsSame(c2));
}

// THE migration proof: mint refs on the ORIGINAL body, WIDEN the sketch,
// re-extrude, then resolveSet the SAME refs against the NEW body. The ordinal
// fallback is now stale (edges renumbered), so best-first must pick the
// sketchedge scheme, which re-finds the moved corners - exactly EdgeAnchor's
// survives-resize behaviour, now flowing through the registry.
TEST(TopoEdge, ResolveSetFollowsSketchResize) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    // Hold the ExtrudeOp so re-executing UPDATES the same body (as the cascade
    // does) instead of a fresh op creating a second one.
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = doc.getAllBodyIds().front();

    topo::Context ctx0; ctx0.doc = &doc; ctx0.shape = doc.getBody(body);
    ctx0.type = TopAbs_EDGE;
    // The two right-hand corners that will move: (20,0) and (20,10).
    std::vector<topo::Ref> refs{
        topo::mint(verticalEdgeAt(doc.getBody(body), 20.0, 0.0),  ctx0),
        topo::mint(verticalEdgeAt(doc.getBody(body), 20.0, 10.0), ctx0),
    };
    ASSERT_FALSE(refs[0].empty()); ASSERT_FALSE(refs[1].empty());

    // Widen 20 -> 40 and re-derive the base with the SAME op.
    sk->movePoint(pid[1], {40.0f, 0.0f});
    sk->movePoint(pid[2], {40.0f, 10.0f});
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));

    Bnd_Box bb; BRepBndLib::Add(doc.getBody(body), bb);
    Standard_Real x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
    ASSERT_NEAR(x1 - x0, 40.0, 1e-6) << "base widened to 40";

    topo::Context ctx1; ctx1.doc = &doc; ctx1.shape = doc.getBody(body);
    ctx1.type = TopAbs_EDGE;
    std::vector<TopoDS_Shape> out;
    ASSERT_TRUE(topo::resolveSet(refs, ctx1, out))
        << "sketchedge must re-find the moved corners after the resize";
    ASSERT_EQ(out.size(), 2u);
    EXPECT_FALSE(out[0].IsSame(out[1]));

    // They must land on the NEW corner positions (x=40), not the old x=20.
    double x, y;
    ASSERT_TRUE(verticalXY(TopoDS::Edge(out[0]), x, y));
    EXPECT_NEAR(x, 40.0, 1e-6); EXPECT_NEAR(y, 0.0, 1e-6);
    ASSERT_TRUE(verticalXY(TopoDS::Edge(out[1]), x, y));
    EXPECT_NEAR(x, 40.0, 1e-6); EXPECT_NEAR(y, 10.0, 1e-6);
}
