// ShellOp topo::Ref wiring, tested through THE REAL IN-APP FLOW: a
// History::editStep replay with a SketchEditOp in the chain (what
// cascadeFromSketchEdit runs). The replay rolls the LIVE sketch back to its
// stale pre-edit state mid-replay while the final state is pinned as the
// cascade override - the exact hazard that broke the first attempt in-app.
//
// The body is L-SHAPED with TWO +X walls (A big at x=20, B small at x=18) and
// the edit moves A far away (x=40): the geometric normal+point rebind then
// prefers B (2mm from the stale anchor point) over the true A' (20mm away) -
// so a simple box can't mask the sketch-anchored path here. Only a correct
// topo resolution against the OVERRIDE sketch opens the right wall.

#include "modeling/TopoName.h"
#include "modeling/ShellOp.h"
#include "core/Document.h"
#include "core/History.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
#include "modeling/ExtrudeOp.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
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

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

// L-profile with two +X-facing walls: A (x=20, y 0..8, area 8x10=80 when
// extruded 10) and B (x=18, y 8..10, area 2x10=20).
//   p0(0,0) p1(20,0) p2(20,8) p3(18,8) p4(18,10) p5(0,10)
std::shared_ptr<Sketch> makeL(int pid[6]) {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    pid[0] = sk->addPoint({0.0f, 0.0f});
    pid[1] = sk->addPoint({20.0f, 0.0f});
    pid[2] = sk->addPoint({20.0f, 8.0f});
    pid[3] = sk->addPoint({18.0f, 8.0f});
    pid[4] = sk->addPoint({18.0f, 10.0f});
    pid[5] = sk->addPoint({0.0f, 10.0f});
    for (int i = 0; i < 6; ++i) sk->addLine(pid[i], pid[(i + 1) % 6]);
    return sk;
}

// Sum of the areas of planar faces with an (outward) +X normal lying at x≈xp.
double plusXAreaAt(const TopoDS_Shape& body, double xp) {
    double sum = 0.0;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) continue;
        gp_Dir n = s.Plane().Axis().Direction();
        if (std::abs(std::abs(n.X()) - 1.0) > 1e-6) continue;   // not an X wall
        GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
        if (std::abs(g.CentreOfMass().X() - xp) > 0.05) continue;
        sum += g.Mass();
    }
    return sum;
}

// The +X wall (largest-area planar X-normal face) nearest x=xp.
TopoDS_Face wallAt(const TopoDS_Shape& body, double xp, double yLo, double yHi) {
    TopoDS_Face best; double bestA = -1.0;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) continue;
        gp_Dir n = s.Plane().Axis().Direction();
        if (std::abs(std::abs(n.X()) - 1.0) > 1e-6) continue;
        GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
        gp_Pnt c = g.CentreOfMass();
        if (std::abs(c.X() - xp) > 0.05) continue;
        if (c.Y() < yLo || c.Y() > yHi) continue;
        if (g.Mass() > bestA) { bestA = g.Mass(); best = f; }
    }
    return best;
}

} // namespace

// The direct fix test: topo::resolveSet must read the CASCADE OVERRIDE sketch,
// not the (rolled-back, stale) live one. Fails if the override plumbing in
// TopoName::sketchRefs is removed - verified by reverting the fix.
TEST(TopoShell, ResolveReadsCascadeOverrideNotStaleSketch) {
    Document doc;
    int pid[6];
    auto sk = makeL(pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = doc.getAllBodyIds().front();

    // Name wall A (x=20) while everything is live.
    TopoDS_Face wallA = wallAt(doc.getBody(body), 20.0, 0.0, 8.0);
    ASSERT_FALSE(wallA.IsNull());
    topo::Context ctx; ctx.doc = &doc; ctx.shape = doc.getBody(body);
    ctx.type = TopAbs_FACE;
    topo::Ref ref = topo::mint(wallA, ctx);
    ASSERT_FALSE(ref.empty());

    // Rebuild the body 40-wide (as the cascade's extrude does, from the FINAL
    // sketch), pin the final sketch as the override, then roll the LIVE sketch
    // back to its stale 20-wide state - the mid-replay condition.
    sk->movePoint(pid[1], {40.0f, 0.0f});
    sk->movePoint(pid[2], {40.0f, 8.0f});
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    doc.setCascadeSketchOverride(sid, std::make_shared<Sketch>(*sk));
    sk->movePoint(pid[1], {20.0f, 0.0f});
    sk->movePoint(pid[2], {20.0f, 8.0f});

    // Resolve the name against the rebuilt body: must land the MOVED wall A'
    // at x=40 (via the override), not fail or land B (x=18) off the stale one.
    topo::Context rc; rc.doc = &doc; rc.shape = doc.getBody(body);
    rc.type = TopAbs_FACE;
    rc.crossRebuild = true;   // as an op resolving after an upstream rebuild
    std::vector<TopoDS_Shape> out;
    ASSERT_TRUE(topo::resolveSet({ref}, rc, out) && out.size() == 1)
        << "resolution must use the cascade override, not the stale live sketch";
    GProp_GProps g; BRepGProp::SurfaceProperties(TopoDS::Face(out[0]), g);
    EXPECT_NEAR(g.CentreOfMass().X(), 40.0, 1e-6)
        << "must resolve to the MOVED wall A' (x=40), not B (x=18)";
    doc.clearCascadeSketchOverrides();
    // Restore the live sketch (hygiene).
    sk->movePoint(pid[1], {40.0f, 0.0f});
    sk->movePoint(pid[2], {40.0f, 8.0f});
}

// THE IN-APP FLOW, end to end: extrude -> shell(open wall A) -> SketchEditOp,
// then the cascade replay (editStep, transactional, override pinned). The
// widened body must still be hollow with wall A' OPEN (rim only at x=40) and
// wall B INTACT (full 20mm^2 face at x=18) - the geometric fallback alone
// would open B instead.
TEST(TopoShell, OpenedWallFollowsResizeThroughHistoryReplay) {
    Document doc;
    History hist;
    int pid[6];
    auto sk = makeL(pid);
    int sid = doc.addSketch(sk);

    auto ext = std::make_unique<ExtrudeOp>();
    ExtrudeOp* extP = ext.get();
    extP->setSketchSource(sid);
    extP->setDistance(10.0);
    ASSERT_TRUE(extP->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extP->execute(doc));
    hist.pushExecuted(std::move(ext));
    int body = doc.getAllBodyIds().front();
    const double solidVol = volumeOf(doc.getBody(body));

    auto sh = std::make_unique<ShellOp>();
    sh->setBody(body);
    sh->setThickness(1.0);
    TopoDS_Face wallA = wallAt(doc.getBody(body), 20.0, 0.0, 8.0);
    ASSERT_FALSE(wallA.IsNull());
    sh->addFaceToRemove(wallA);
    ASSERT_TRUE(sh->execute(doc));
    hist.pushExecuted(std::move(sh));
    ASSERT_LT(volumeOf(doc.getBody(body)), solidVol * 0.9) << "initial shell hollows";

    // The user's edit, recorded the way the app records it.
    auto before = std::make_shared<Sketch>(*sk);
    sk->movePoint(pid[1], {40.0f, 0.0f});
    sk->movePoint(pid[2], {40.0f, 8.0f});
    auto after = std::make_shared<Sketch>(*sk);
    hist.pushExecuted(std::make_unique<materializr::SketchEditOp>(sk, before, after));

    // cascadeFromSketchEdit equivalent.
    ASSERT_TRUE(extP->rebuildProfileFromSketch(doc));
    doc.setCascadeSketchOverride(sid, std::make_shared<Sketch>(*sk));
    bool ok = hist.editStep(0, doc, /*transactional=*/true);
    doc.clearCascadeSketchOverrides();
    ASSERT_TRUE(ok) << "the shell must follow the resize through the replay";

    const TopoDS_Shape& shelled = doc.getBody(body);
    EXPECT_TRUE(BRepCheck_Analyzer(shelled).IsValid());
    const double wideSolid = (40.0 * 8.0 + 18.0 * 2.0) * 10.0; // 3560
    EXPECT_LT(volumeOf(shelled), wideSolid * 0.5)
        << "widened body must still be hollow (opening not silently dropped)";
    // Wall B (x=18, area 20) must be INTACT - opening it instead of A is the
    // geometric fallback's wrong answer.
    EXPECT_GT(plusXAreaAt(shelled, 18.0), 15.0)
        << "wall B must remain closed (full face at x=18)";
    // Wall A' (x=40, would be area 80 closed) must be OPEN - only a thin rim
    // band (perimeter x thickness ~ 33) may remain at x=40.
    EXPECT_LT(plusXAreaAt(shelled, 40.0), 50.0)
        << "wall A' must be the opened one (rim only at x=40)";
}
