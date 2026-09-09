// Tail: FilletOp publishes a GenerationLedger (input edge -> blend face), so
// the "gen" strategy can name a BLEND face - an op-GENERATED face no geometric
// (sketch) scheme can name. The blend's name references the filleted edge,
// which is itself sketch-anchored (EdgeAnchor corner), so the blend-face name
// survives a dimension edit that MOVES the corner. This is the general kernel
// working on real op-produced geometry.

#include "modeling/TopoName.h"
#include "modeling/FilletOp.h"
#include "core/Document.h"
#include "modeling/Sketch.h"
#include "modeling/ExtrudeOp.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
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

// The vertical-axis cylindrical blend face (a filleted vertical corner).
TopoDS_Face blendCyl(const TopoDS_Shape& body) {
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Cylinder) continue;
        if (std::abs(s.Cylinder().Axis().Direction().Dot(gp_Dir(0, 0, 1))) > 0.99)
            return f;
    }
    return {};
}

double centroidX(const TopoDS_Face& f) {
    GProp_GProps g; BRepGProp::SurfaceProperties(f, g); return g.CentreOfMass().X();
}

} // namespace

TEST(TopoFilletGen, BlendFaceNameFollowsResize) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = doc.getAllBodyIds().front();

    // Fillet the corner over point (20,10), anchored to the sketch.
    FilletOp fil;
    fil.setBody(body);
    fil.setEdges({ verticalEdgeAt(doc.getBody(body), 20.0, 10.0) });
    fil.setRadius(2.0);
    fil.setSourceSketch(sid);
    ASSERT_TRUE(fil.execute(doc));
    ASSERT_GT(fil.generationLedger().generated.Extent(), 0)
        << "fillet must publish an input-edge -> blend-face map";

    // Name the blend face by its lineage.
    TopoDS_Face blend1 = blendCyl(doc.getBody(body));
    ASSERT_FALSE(blend1.IsNull());
    topo::Context ctx1;
    ctx1.doc = &doc; ctx1.shape = doc.getBody(body); ctx1.type = TopAbs_FACE;
    ctx1.gen = &fil.generationLedger();
    topo::Ref ref = topo::mint(blend1, ctx1);
    ASSERT_FALSE(ref.empty());
    EXPECT_EQ(ref.names.front().scheme, "gen")
        << "a blend face is nameable only by generation lineage";
    EXPECT_LT(centroidX(blend1), 20.0) << "blend sits at the x=20 corner";

    // WIDEN 20 -> 40 and re-derive: base widens, fillet re-binds to the moved
    // corner (EdgeAnchor), producing a fresh generation ledger.
    sk->movePoint(pid[1], {40.0f, 0.0f});
    sk->movePoint(pid[2], {40.0f, 10.0f});
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    ASSERT_TRUE(fil.execute(doc)) << "fillet follows the moved corner";

    // Resolve the ORIGINAL blend-face name against the NEW ledger: it must land
    // the blend face at the MOVED corner (x ~ 40), via the edge lineage.
    topo::Context ctx2;
    ctx2.doc = &doc; ctx2.shape = doc.getBody(body); ctx2.type = TopAbs_FACE;
    ctx2.gen = &fil.generationLedger();
    TopoDS_Shape out;
    ASSERT_TRUE(topo::resolve(ref, ctx2, out))
        << "the blend-face lineage name must resolve on the resized body";
    ASSERT_EQ(out.ShapeType(), TopAbs_FACE);
    EXPECT_GT(centroidX(TopoDS::Face(out)), 30.0)
        << "resolved blend must be at the MOVED corner (x~40), not the old x=20";
    EXPECT_TRUE(out.IsSame(blendCyl(doc.getBody(body))));
}
