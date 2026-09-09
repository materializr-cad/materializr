// experiment/generative-edges - Phase 1: a filleted CORNER edge is anchored to
// the sketch VERTEX it sits over, so it survives a sketch DIMENSION edit that
// relocates the corner (where ordinal/carrier matching fails).
//
// The decisive test: build a box from a rectangle sketch, fillet one vertical
// corner, then WIDEN the sketch. With generative anchoring the fillet re-binds
// to the moved corner and re-executes; without it (control), it fails - which
// is exactly today's "a downstream fillet couldn't follow it" behaviour.

#include "core/Document.h"
#include "core/History.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/FilletOp.h"
#include "modeling/EdgeAnchor.h"

#include <gtest/gtest.h>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <gp_Ax3.hxx>
#include <cmath>
#include <memory>

using materializr::Sketch;

namespace {

// Rectangle sketch on the XY plane with corners (0,0)-(w,h). Returns the sketch
// and its 4 corner point ids (CCW from origin).
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

// The vertical (Z-parallel) edge of `body` sitting over sketch-plane (x,y).
TopoDS_Edge verticalEdgeAt(const TopoDS_Shape& body, double x, double y) {
    for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.GetType() != GeomAbs_Line) continue;
        if (std::abs(c.Line().Direction().Dot(gp_Dir(0, 0, 1))) < 0.999) continue;
        gp_Pnt m = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
        if (std::hypot(m.X() - x, m.Y() - y) < 1e-6)
            return TopoDS::Edge(ex.Current());
    }
    return {};
}

// A straight top/bottom RIM edge whose endpoints project to (x1,y1)-(x2,y2)
// in the XY plane, at height z=h.
TopoDS_Edge rimEdgeAt(const TopoDS_Shape& body, double x1, double y1,
                      double x2, double y2, double h) {
    for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.GetType() != GeomAbs_Line) continue;
        if (std::abs(c.Line().Direction().Dot(gp_Dir(0, 0, 1))) > 0.001) continue; // horizontal
        gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
        if (std::abs(a.Z() - h) > 1e-6 || std::abs(b.Z() - h) > 1e-6) continue;
        bool fwd = (std::hypot(a.X() - x1, a.Y() - y1) < 1e-6 &&
                    std::hypot(b.X() - x2, b.Y() - y2) < 1e-6);
        bool rev = (std::hypot(a.X() - x2, a.Y() - y2) < 1e-6 &&
                    std::hypot(b.X() - x1, b.Y() - y1) < 1e-6);
        if (fwd || rev) return TopoDS::Edge(ex.Current());
    }
    return {};
}

int onlyBodyId(Document& d) { return d.getAllBodyIds().front(); }

double bboxWidthX(Document& d, int id) {
    Bnd_Box b; BRepBndLib::Add(d.getBody(id), b);
    Standard_Real x0, y0, z0, x1, y1, z1; b.Get(x0, y0, z0, x1, y1, z1);
    return x1 - x0;
}

} // namespace

// THE headline test: fillet follows a sketch-dimension edit via its anchor.
TEST(GenerativeEdges, FilletFollowsSketchResize) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);

    ExtrudeOp ext;
    ext.setSketchSource(sid);
    ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = onlyBodyId(doc);

    // Fillet the corner over point pid[2] = (20,10), anchored to the sketch.
    TopoDS_Edge corner = verticalEdgeAt(doc.getBody(body), 20.0, 10.0);
    ASSERT_FALSE(corner.IsNull());
    FilletOp f;
    f.setBody(body);
    f.setEdges({corner});
    f.setRadius(2.0);
    f.setSourceSketch(sid);
    ASSERT_TRUE(f.execute(doc));
    const double vFilleted20 = [&]{ GProp_GProps g; BRepGProp::VolumeProperties(doc.getBody(body), g); return g.Mass(); }();

    // WIDEN the rectangle: move the two right-hand corners x: 20 -> 40.
    sk->movePoint(pid[1], {40.0f, 0.0f});
    sk->movePoint(pid[2], {40.0f, 10.0f});

    // Re-derive the base (as the cascade does: extrude replays first) ...
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    EXPECT_NEAR(bboxWidthX(doc, body), 40.0, 1e-6) << "base should have widened to 40";

    // ... then the fillet must FOLLOW the moved corner to (40,10).
    ASSERT_TRUE(f.execute(doc))
        << "generative anchor should re-find the corner after the resize";

    // Sanity: still a valid filleted solid, wider than before, fillet intact.
    GProp_GProps g; BRepGProp::VolumeProperties(doc.getBody(body), g);
    EXPECT_GT(g.Mass(), vFilleted20) << "wider body must have more volume";
    EXPECT_NEAR(bboxWidthX(doc, body), 40.0, 1e-6);
}

// Phase 2: a RIM edge (top edge from a sketch LINE) follows the resize too.
TEST(GenerativeEdges, FilletFollowsResize_RimEdge) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);   // corners p0(0,0) p1(20,0) p2(20,10) p3(0,10)
    int sid = doc.addSketch(sk);

    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = onlyBodyId(doc);

    // Top rim edge over the bottom sketch line p0(0,0)-p1(20,0), at z=10.
    TopoDS_Edge rim = rimEdgeAt(doc.getBody(body), 0, 0, 20, 0, 10.0);
    ASSERT_FALSE(rim.IsNull());
    FilletOp f;
    f.setBody(body);
    f.setEdges({rim});
    f.setRadius(2.0);
    f.setSourceSketch(sid);
    ASSERT_TRUE(f.execute(doc));

    // Widen: p1,p2 x 20 -> 40. The rim edge p0-p1 grows from 20 to 40 long.
    sk->movePoint(pid[1], {40.0f, 0.0f});
    sk->movePoint(pid[2], {40.0f, 10.0f});
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));

    ASSERT_TRUE(f.execute(doc))
        << "generative RIM anchor should re-find the top edge after the resize";
    EXPECT_NEAR(bboxWidthX(doc, body), 40.0, 1e-6);
}

// Coverage: EVERY edge of an extruded rectangular prism must anchor (4 corners
// + 8 rims, 0 none) - otherwise a whole-body fillet fails the all-or-nothing
// rule (this is exactly the user's 14-edge case).
TEST(GenerativeEdges, WholePrismEveryEdgeAnchors) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = onlyBodyId(doc);

    TopTools_IndexedMapOfShape emap;
    TopExp::MapShapes(doc.getBody(body), TopAbs_EDGE, emap); // unique edges
    std::vector<TopoDS_Edge> all;
    for (int i = 1; i <= emap.Extent(); ++i) all.push_back(TopoDS::Edge(emap(i)));
    ASSERT_EQ(all.size(), 12u); // box: 4 vertical + 4 top + 4 bottom

    auto anchors = EdgeAnchor::compute(all, {{sid, sk.get()}});
    int corners = 0, rims = 0, none = 0;
    for (const auto& a : anchors)
        (a.kind == EdgeAnchor::Anchor::Corner ? corners :
         a.kind == EdgeAnchor::Anchor::Rim    ? rims : none)++;
    EXPECT_EQ(corners, 4);
    EXPECT_EQ(rims, 8);
    EXPECT_EQ(none, 0) << "every prism edge must be attributable to a sketch feature";
}

// Control: a fillet whose anchors were never captured (a stale edge from the
// pre-resize body, as any pre-anchoring op has) cannot follow the resize -
// proving it's the anchoring, not something else, doing the work.
TEST(GenerativeEdges, WithoutAnchorResizeStillFails) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);

    ExtrudeOp ext;
    ext.setSketchSource(sid);
    ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = onlyBodyId(doc);

    TopoDS_Edge corner = verticalEdgeAt(doc.getBody(body), 20.0, 10.0);
    ASSERT_FALSE(corner.IsNull());

    sk->movePoint(pid[1], {40.0f, 0.0f});
    sk->movePoint(pid[2], {40.0f, 10.0f});
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));

    FilletOp f;
    f.setBody(body);
    f.setEdges({corner}); // stale: from the pre-resize body, no anchors captured
    f.setRadius(2.0);
    EXPECT_FALSE(f.execute(doc))
        << "without an anchor the fillet cannot follow the resized corner "
           "(this is the limitation the feature fixes)";
}

// Anchors round-trip through the op's serialize/deserialize (v2 format), and
// the legacy v1 blob form still parses.
TEST(GenerativeEdges, AnchorSerializesAndParses) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = onlyBodyId(doc);

    FilletOp f;
    f.setBody(body);
    f.setEdges({verticalEdgeAt(doc.getBody(body), 20.0, 10.0)});
    f.setRadius(2.0);
    ASSERT_TRUE(f.execute(doc));

    const std::string blob = f.serializeParams();
    EXPECT_NE(blob.find("anchor=v2"), std::string::npos);

    FilletOp f2;
    ASSERT_TRUE(f2.deserializeParams(blob));

    // Field-level round-trip at the EdgeAnchor layer.
    std::vector<EdgeAnchor::Anchor> in = {
        { EdgeAnchor::Anchor::Corner, 2, 7, 5.0, 0.5 },
        { EdgeAnchor::Anchor::Rim,    3, 4, 90.0, 0.25 },
        { EdgeAnchor::Anchor::Arc,    4, 9, -32.5, 0.5 },
        { EdgeAnchor::Anchor::Circle, 4, 11, 0.0, 0.5 },
        { EdgeAnchor::Anchor::None,  -1, -1, 0.0, 0.5 },
    };
    std::vector<EdgeAnchor::Anchor> back;
    ASSERT_TRUE(EdgeAnchor::parse(EdgeAnchor::serialize(in), back));
    ASSERT_EQ(back.size(), in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        EXPECT_EQ(back[i].kind, in[i].kind) << i;
        if (in[i].kind == EdgeAnchor::Anchor::None) continue;
        EXPECT_EQ(back[i].sketchId, in[i].sketchId) << i;
        EXPECT_EQ(back[i].elemId, in[i].elemId) << i;
        EXPECT_NEAR(back[i].h, in[i].h, 1e-6) << i;
        EXPECT_NEAR(back[i].t, in[i].t, 1e-6) << i;
    }

    // Legacy v1 blob ("<sid>~C,<pid>~R,<lid>,<h>") still parses, header sketch
    // id fanned out to every anchor.
    std::vector<EdgeAnchor::Anchor> legacy;
    ASSERT_TRUE(EdgeAnchor::parse("2~C,7~R,4,90.000000~N", legacy));
    ASSERT_EQ(legacy.size(), 3u);
    EXPECT_EQ(legacy[0].kind, EdgeAnchor::Anchor::Corner);
    EXPECT_EQ(legacy[0].sketchId, 2);
    EXPECT_EQ(legacy[0].elemId, 7);
    EXPECT_EQ(legacy[1].kind, EdgeAnchor::Anchor::Rim);
    EXPECT_EQ(legacy[1].sketchId, 2);
    EXPECT_NEAR(legacy[1].h, 90.0, 1e-6);
    EXPECT_EQ(legacy[2].kind, EdgeAnchor::Anchor::None);
}

// THE IN-APP FLOW: the resize goes through History::editStep with a
// SketchEditOp in the chain - exactly what cascadeFromSketchEdit runs. The
// replay rolls the LIVE sketch back through its snapshots, so when the fillet
// re-executes mid-replay the sketch holds the STALE (pre-edit) state while
// the extrude below was rebuilt from the final one. Without the cascade
// sketch override the fillet resolves against old geometry and fails every
// time ("like nothing changed"); with it, the edit lands.
TEST(GenerativeEdges, FilletFollowsResize_ThroughHistoryReplay) {
    Document doc;
    History hist;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);

    auto ext = std::make_unique<ExtrudeOp>();
    ExtrudeOp* extP = ext.get();
    extP->setSketchSource(sid);
    extP->setDistance(10.0);
    ASSERT_TRUE(extP->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extP->execute(doc));
    hist.pushExecuted(std::move(ext));
    int body = onlyBodyId(doc);

    auto fil = std::make_unique<FilletOp>();
    fil->setBody(body);
    fil->setEdges({verticalEdgeAt(doc.getBody(body), 20.0, 10.0)});
    fil->setRadius(2.0);
    ASSERT_TRUE(fil->execute(doc));
    hist.pushExecuted(std::move(fil));

    // The user's edit, recorded the way the app records it: before/after
    // snapshots around the live-sketch mutation, pushed as a history step.
    auto before = std::make_shared<Sketch>(*sk);
    sk->movePoint(pid[1], {40.0f, 0.0f});
    sk->movePoint(pid[2], {40.0f, 10.0f});
    auto after = std::make_shared<Sketch>(*sk);
    hist.pushExecuted(std::make_unique<materializr::SketchEditOp>(sk, before, after));

    // cascadeFromSketchEdit equivalent: re-derive the extrude profile from
    // the edited sketch, then replay the whole chain transactionally.
    ASSERT_TRUE(extP->rebuildProfileFromSketch(doc));

    // Control: WITHOUT the override the replay reads the rolled-back sketch
    // and the fillet cannot re-find the moved corner - the bug this guards.
    EXPECT_FALSE(hist.editStep(0, doc, /*transactional=*/true))
        << "expected the replay to fail without the cascade sketch override "
           "(if this now PASSES, the override plumbing may be removable)";

    // With the edited sketch's final state pinned, the same replay succeeds.
    doc.setCascadeSketchOverride(sid, std::make_shared<Sketch>(*sk));
    bool ok = hist.editStep(0, doc, /*transactional=*/true);
    doc.clearCascadeSketchOverrides();
    ASSERT_TRUE(ok) << "fillet must follow the resize through the history replay";
    EXPECT_NEAR(bboxWidthX(doc, body), 40.0, 1e-6);
}

// Arc coverage: a rounded-corner profile (two lines bridged by an arc, closed
// back to the origin) extrudes to a body whose every edge must anchor -
// including the two circular rim edges from the ARC and the seamless walls.
TEST(GenerativeEdges, ArcProfileEveryEdgeAnchors) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    // Rectangle 20x10 with its (20,10) corner rounded by an R5 arc centred
    // at (15,5): p0(0,0) p1(20,0) ... arc p1a(20,5)->(15,10)p2a ... p3(0,10).
    int p0  = sk->addPoint({0, 0});
    int p1  = sk->addPoint({20, 0});
    int p1a = sk->addPoint({20, 5});
    int ctr = sk->addPoint({15, 5});
    int p2a = sk->addPoint({15, 10});
    int p3  = sk->addPoint({0, 10});
    sk->addLine(p0, p1);
    sk->addLine(p1, p1a);
    sk->addArc(ctr, p1a, p2a, 5.0);
    sk->addLine(p2a, p3);
    sk->addLine(p3, p0);
    int sid = doc.addSketch(sk);

    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = onlyBodyId(doc);

    TopTools_IndexedMapOfShape emap;
    TopExp::MapShapes(doc.getBody(body), TopAbs_EDGE, emap);
    std::vector<TopoDS_Edge> all;
    for (int i = 1; i <= emap.Extent(); ++i) all.push_back(TopoDS::Edge(emap(i)));

    auto anchors = EdgeAnchor::compute(all, {{sid, sk.get()}});
    int none = 0;
    for (size_t i = 0; i < anchors.size(); ++i)
        if (anchors[i].kind == EdgeAnchor::Anchor::None) ++none;
    EXPECT_EQ(none, 0) << "every edge of an arc-cornered prism must anchor";

    // And they all resolve back to distinct edges on the unchanged body.
    std::vector<TopoDS_Edge> out;
    EXPECT_TRUE(EdgeAnchor::resolve(anchors, {{sid, sk.get()}},
                                    doc.getBody(body), out));
    EXPECT_EQ(out.size(), all.size());
}
