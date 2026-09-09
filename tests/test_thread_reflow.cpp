// THREADS-LAST IS ENFORCED BY REFLOW, NOT REFUSAL. An op pushed onto a
// thread-modified body must reorder beneath the Thread step (the op runs
// against clean geometry, the thread re-cuts parametrically on the result)
// instead of being declined. Guards History::pushOperation's reflow path -
// the June-2026 "threads-last discipline" refusal is gone.
//
// PHASE 2 (ThreadFollows suite): the thread must FOLLOW its cylinder through
// upstream edits via its minted face ref - resize (new radius), transform
// (new axis), and a sketch-edit cascade (cylinder moved at the source).
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/History.h"
#include "modeling/BooleanOp.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/ResizeCylindricalOp.h"
#include "modeling/Sketch.h"
#include "modeling/ThreadOp.h"
#include "modeling/TopoName.h"
#include "modeling/TransformOp.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <Standard_Version.hxx>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace materializr;

namespace {

double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

bool inAt(const TopoDS_Shape& s, double x, double y, double z) {
    BRepClass3d_SolidClassifier c(s, gp_Pnt(x, y, z), 1e-7);
    return c.State() == TopAbs_IN;
}

// Ring sample: how many of 16 points on the circle (r, z) about (cx, cy)
// are inside the solid. NOTE: bbox is USELESS here - a swept helicoid's
// BSpline control points bulge far outside the real surface (a correct
// r=10 threaded rod bboxes at 16).
int ringHits(const TopoDS_Shape& s, double cx, double cy, double r,
             double z) {
    int n = 0;
    for (int k = 0; k < 16; ++k) {
        const double a = 2.0 * M_PI * k / 16.0;
        if (inAt(s, cx + r * std::cos(a), cy + r * std::sin(a), z)) ++n;
    }
    return n;
}

constexpr double R = 10.0, L = 9.0; // 3 coarse turns - fast

std::unique_ptr<ThreadOp> makeThread(int bodyId) {
    auto t = std::make_unique<ThreadOp>();
    t->setBody(bodyId);
    t->setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t->setRadius(R);
    t->setLength(L);
    t->setPitch(3.0);
    t->setDepth(1.2);
    t->setIsHole(false);
    return t;
}

// Transverse cutter cylinder through the rod at height z.
TopoDS_Shape transverseCutter(double z, double r = 2.0) {
    return BRepPrimAPI_MakeCylinder(
               gp_Ax2(gp_Pnt(-20.0, 0.0, z), gp_Dir(1, 0, 0)), r, 40.0)
        .Shape();
}

} // namespace

// These two drive a Boolean THROUGH a cut thread and require the kernel to
// re-cut the helix on the modified rod. OCCT < 7.9 cannot: every fallback in
// ThreadOp's chain gives out in turn ("compound cut failed" -> "per-turn:
// result invalid after heal" -> "boolean cut FAILED"), so the reflow leaves
// the rod unthreaded and the volume assertions below are unreachable.
// Reproduced on a source-built 7.7.0 (issue #80, reported from Slackware's
// packaged 7.7.0); identical on 7.9.3 -> passes. We build and ship 7.9.3 on
// every platform, so this is a limitation of an older kernel, not a
// regression. Skip loudly rather than fail, so a source build against a
// distro OCCT reports honestly.
#define MZR_REQUIRE_OCCT_79()                                                 \
    do {                                                                      \
        if (OCC_VERSION_MAJOR < 7 ||                                          \
            (OCC_VERSION_MAJOR == 7 && OCC_VERSION_MINOR < 9)) {              \
            GTEST_SKIP() << "needs OCCT >= 7.9 (building against "            \
                         << OCC_VERSION_MAJOR << "." << OCC_VERSION_MINOR     \
                         << "): re-cutting a thread through a Boolean fails " \
                            "in the older kernel. Materializr ships 7.9.3.";  \
        }                                                                     \
    } while (0)

TEST(ThreadReflow, SubtractOnThreadedBodyReflowsBeneathThread) {
    MZR_REQUIRE_OCCT_79();
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    const double vRod = vol(rod);
    int rodId = doc.addBody(rod, "rod");

    ASSERT_TRUE(hist.pushOperation(makeThread(rodId), doc));
    const double vThreaded = vol(doc.getBody(rodId));
    ASSERT_LT(vThreaded, vRod);

    TopoDS_Shape cutter = transverseCutter(L * 0.5);
    const double vPlainHoled = vol(BRepAlgoAPI_Cut(rod, cutter).Shape());
    int cutId = doc.addBody(cutter, "cutter");
    auto cut = std::make_unique<BooleanOp>();
    cut->setTargetBodyId(rodId);
    cut->setToolBodyId(cutId);
    cut->setMode(BooleanMode::Subtract);

    // The old discipline REFUSED this push. It must now reflow and succeed.
    ASSERT_TRUE(hist.pushOperation(std::move(cut), doc));

    // The thread ended up LAST in history.
    ASSERT_GE(hist.currentStep(), 1);
    EXPECT_EQ(hist.getStep(hist.currentStep())->typeId(), "thread");
    EXPECT_EQ(hist.getStep(hist.currentStep() - 1)->typeId(), "boolean");

    // Result: valid, holed AND threaded (less material than the plain holed
    // rod - the re-cut thread grooves came back on the new geometry).
    TopoDS_Shape res = doc.getBody(rodId);
    ASSERT_FALSE(res.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());
    EXPECT_LT(vol(res), vThreaded);
    EXPECT_LT(vol(res), vPlainHoled - 1.0);
    EXPECT_GT(vol(res), 0.5 * vRod);
}

TEST(ThreadReflow, SecondOpStacksAndThreadStaysLast) {
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    int rodId = doc.addBody(rod, "rod");
    ASSERT_TRUE(hist.pushOperation(makeThread(rodId), doc));

    double vPrev = vol(doc.getBody(rodId));
    for (double z : {L * 0.35, L * 0.7}) {
        int cutId = doc.addBody(transverseCutter(z, 1.5), "cutter");
        auto cut = std::make_unique<BooleanOp>();
        cut->setTargetBodyId(rodId);
        cut->setToolBodyId(cutId);
        cut->setMode(BooleanMode::Subtract);
        ASSERT_TRUE(hist.pushOperation(std::move(cut), doc)) << "z=" << z;
        double vNow = vol(doc.getBody(rodId));
        EXPECT_LT(vNow, vPrev) << "z=" << z;
        vPrev = vNow;
    }
    EXPECT_EQ(hist.getStep(hist.currentStep())->typeId(), "thread");
    EXPECT_TRUE(BRepCheck_Analyzer(doc.getBody(rodId)).IsValid());
}

TEST(ThreadReflow, RoundedRecutOnHoledRodKeepsSweptGeometry) {
    // The reflow re-cut of a SMOOTH profile on a modified rod must derive its
    // cutter from the sweep - falling into the rope groove instead produced
    // the deep-scoop "stacked discs" body (2026-07-21). Same-geometry check:
    // (rod ⊖ thread) ⊖ hole and (rod ⊖ hole) ⊖ thread must agree in volume.
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    int rodId = doc.addBody(rod, "rod");

    auto th = makeThread(rodId);
    th->setProfile(ThreadProfile::Rounded);
    TopoDS_Shape cutter = transverseCutter(L * 0.5);
    ThreadOp ref; // sweep on the clean rod = the approved gentle sine
    ref.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    ref.setRadius(R); ref.setLength(L); ref.setPitch(3.0); ref.setDepth(1.2);
    ref.setIsHole(false); ref.setProfile(ThreadProfile::Rounded);
    TopoDS_Shape sweptClean = ref.buildResult(rod);
    ASSERT_FALSE(sweptClean.IsNull());
    const double vExpected =
        vol(BRepAlgoAPI_Cut(sweptClean, cutter).Shape());

    ASSERT_TRUE(hist.pushOperation(std::move(th), doc));
    int cutId = doc.addBody(cutter, "cutter");
    auto cut = std::make_unique<BooleanOp>();
    cut->setTargetBodyId(rodId);
    cut->setToolBodyId(cutId);
    cut->setMode(BooleanMode::Subtract);
    ASSERT_TRUE(hist.pushOperation(std::move(cut), doc));

    TopoDS_Shape res = doc.getBody(rodId);
    ASSERT_FALSE(res.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());
    // Rope scoops remove several times the sine grooves' material - a 1%
    // band on the expected volume rules them out without being brittle.
    EXPECT_NEAR(vol(res), vExpected, 0.01 * vExpected);
}

TEST(ThreadReflow, RoundedRecutOnCoaxialBoreKeepsSweptGeometry) {
    // Steve's tube: rod, Rounded thread, then a coaxial bore through it.
    // The Common formulation INVERTED on this (OCCT filled the bore), the
    // volume gate declined, and the re-cut fell to the rope grooves for a
    // minute. The complement formulation (swept − (rod − body)) must yield
    // the swept sine on the tube.
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    int rodId = doc.addBody(rod, "rod");

    auto th = makeThread(rodId);
    th->setProfile(ThreadProfile::Rounded);
    TopoDS_Shape bore =
        BRepPrimAPI_MakeCylinder(
            gp_Ax2(gp_Pnt(0, 0, -5.0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)),
            3.0, L + 10.0)
            .Shape();
    ThreadOp ref;
    ref.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    ref.setRadius(R); ref.setLength(L); ref.setPitch(3.0); ref.setDepth(1.2);
    ref.setIsHole(false); ref.setProfile(ThreadProfile::Rounded);
    TopoDS_Shape sweptClean = ref.buildResult(rod);
    ASSERT_FALSE(sweptClean.IsNull());
    const double vExpected = vol(BRepAlgoAPI_Cut(sweptClean, bore).Shape());

    ASSERT_TRUE(hist.pushOperation(std::move(th), doc));
    int boreId = doc.addBody(bore, "bore");
    auto cut = std::make_unique<BooleanOp>();
    cut->setTargetBodyId(rodId);
    cut->setToolBodyId(boreId);
    cut->setMode(BooleanMode::Subtract);
    ASSERT_TRUE(hist.pushOperation(std::move(cut), doc));

    TopoDS_Shape res = doc.getBody(rodId);
    ASSERT_FALSE(res.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());
    // The bore must survive (the Common inversion FILLED it)...
    {
        BRepClass3d_SolidClassifier cls(res, gp_Pnt(0, 0, L * 0.5), 1e-7);
        EXPECT_NE(cls.State(), TopAbs_IN) << "bore filled in";
    }
    // ...and the grooves must be the gentle sine, not rope scoops.
    EXPECT_NEAR(vol(res), vExpected, 0.01 * vExpected);
}

TEST(ThreadReflow, ExternalThreadOnUnionedBoltGrafts) {
    // gift box regression: an external (buttress) thread on a cylinder that is
    // part of a body rebuilt by an upstream UNION. The direct helical cut can
    // invert against the rebuilt TShape even though plain cuts are fine and the
    // same cylinder threads standalone - ThreadOp's graft fallback threads a
    // clean segment and splices it at the shoulder. Whichever path runs, the
    // thread must apply: valid single solid with real grooves on the end.
    // (The exact gift-box body that inverts is exercised by
    // wip-tests/probe_giftbox; here we guard external threading on a complex
    // unioned bolt generally.)
    const double rEnd = 5.4, rShank = 6.5, rHead = 12.0;
    const double zEnd0 = 40.0, endLen = 8.0;
    // shank [0,40] + threaded end [40,48] + wide head [-6,0], all unioned.
    TopoDS_Shape shank = BRepPrimAPI_MakeCylinder(rShank, zEnd0).Shape();
    TopoDS_Shape end = BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(0, 0, zEnd0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)),
        rEnd, endLen).Shape();
    TopoDS_Shape head = BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(0, 0, -6.0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)),
        rHead, 6.0).Shape();
    TopoDS_Shape bolt = BRepAlgoAPI_Fuse(
        BRepAlgoAPI_Fuse(shank, end).Shape(), head).Shape();
    ASSERT_TRUE(BRepCheck_Analyzer(bolt).IsValid());

    ThreadOp t;
    t.setAxis(gp_Ax2(gp_Pnt(0, 0, zEnd0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t.setRadius(rEnd);
    t.setLength(endLen);
    t.setPitch(1.5);
    t.setDepth(0.5);
    t.setIsHole(false);
    t.setProfile(ThreadProfile::Buttress);
    TopoDS_Shape res = t.buildResult(bolt);
    ASSERT_FALSE(res.IsNull()) << "external thread must apply on a unioned bolt";
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());
    int ns = 0;
    for (TopExp_Explorer sx(res, TopAbs_SOLID); sx.More(); sx.Next()) ++ns;
    EXPECT_EQ(ns, 1) << "result must be a single solid";
    // Real grooves on the end: mid-groove ring is part crest (solid), part
    // valley (void). A plain cylinder would be fully solid there.
    const double zMid = zEnd0 + endLen * 0.5, rMid = rEnd - 0.25;
    const int hits = ringHits(res, 0.0, 0.0, rMid, zMid);
    EXPECT_GT(hits, 0) << "no crest material - end not threaded";
    EXPECT_LT(hits, 16) << "no groove voids - end not threaded";
    // The threaded end must not have vanished or ballooned.
    EXPECT_GT(vol(res), 0.6 * vol(bolt));
    EXPECT_LT(vol(res), 1.1 * vol(bolt));

    // Force the graft path (the synthetic bolt above may not invert) so the
    // graft itself is covered: it must also produce a valid, single-solid,
    // genuinely-threaded end.
    ThreadOp g;
    g.setAxis(gp_Ax2(gp_Pnt(0, 0, zEnd0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    g.setRadius(rEnd); g.setLength(endLen); g.setPitch(1.5); g.setDepth(0.5);
    g.setIsHole(false); g.setProfile(ThreadProfile::Buttress);
    g.setForceGraft(true);
    TopoDS_Shape gr = g.buildResult(bolt);
    ASSERT_FALSE(gr.IsNull()) << "graft path must produce a threaded end";
    EXPECT_TRUE(BRepCheck_Analyzer(gr).IsValid());
    int gs = 0;
    for (TopExp_Explorer sx(gr, TopAbs_SOLID); sx.More(); sx.Next()) ++gs;
    EXPECT_EQ(gs, 1);
    const int ghits = ringHits(gr, 0.0, 0.0, rMid, zMid);
    EXPECT_GT(ghits, 0);
    EXPECT_LT(ghits, 16);
}

TEST(ThreadReflow, ThreadRunsThroughEndChamfer) {
    // A threaded rod whose end is TAPERED (chamfer/cone) must run the thread
    // THROUGH the taper - grooves continue, crests truncated by the cone (a
    // real bolt lead-in) - instead of stopping at the cylinder/chamfer edge
    // and leaving a smooth bevel.
    const double rCyl = 5.0, zCyl = 20.0, depth = 0.6;
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(rCyl, zCyl).Shape();
    TopoDS_Shape cone = BRepPrimAPI_MakeCone(
        gp_Ax2(gp_Pnt(0, 0, zCyl), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)),
        5.0, 4.0, 4.0).Shape();          // gentle taper r5->r4 over 4mm
    TopoDS_Shape rod = BRepAlgoAPI_Fuse(cyl, cone).Shape();

    ThreadOp t;
    t.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t.setRadius(rCyl);
    t.setLength(zCyl);                   // the CYLINDER span (stops at z=20)
    t.setPitch(1.5);
    t.setDepth(depth);
    t.setIsHole(false);
    t.setProfile(ThreadProfile::Buttress);
    TopoDS_Shape res = t.buildResult(rod);
    ASSERT_FALSE(res.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());

    // ON THE TAPER (z=21, surface r≈4.75): the thread must be present - a ring
    // between the valley (r=4.4) and the taper surface is part crest, part
    // groove. Without run-through this region is a smooth cone (fully solid
    // below the surface, so a ring at r=4.6 would be all-solid).
    const int taper = ringHits(res, 0.0, 0.0, 4.6, 21.0);
    EXPECT_GT(taper, 0) << "no crest on the taper";
    EXPECT_LT(taper, 16) << "no grooves on the taper - thread stopped at the edge";
    // The cylinder body below is still fully threaded.
    const int barrel = ringHits(res, 0.0, 0.0, rCyl - 0.5 * depth, 10.0);
    EXPECT_GT(barrel, 0);
    EXPECT_LT(barrel, 16);
    // Core intact (not hollowed).
    EXPECT_TRUE(inAt(res, 0.0, 0.0, 21.0));
}

TEST(ThreadReflow, UndoRedoAcrossReflowedTimeline) {
    MZR_REQUIRE_OCCT_79();
    Document doc;
    History hist;
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(R, L).Shape();
    const double vRod = vol(rod);
    int rodId = doc.addBody(rod, "rod");
    ASSERT_TRUE(hist.pushOperation(makeThread(rodId), doc));

    int cutId = doc.addBody(transverseCutter(L * 0.5), "cutter");
    auto cut = std::make_unique<BooleanOp>();
    cut->setTargetBodyId(rodId);
    cut->setToolBodyId(cutId);
    cut->setMode(BooleanMode::Subtract);
    ASSERT_TRUE(hist.pushOperation(std::move(cut), doc));
    const double vFinal = vol(doc.getBody(rodId));

    // The reflowed timeline is [boolean, thread]: undoing walks back to the
    // holed-unthreaded rod, then the clean rod; redoing rebuilds the result.
    ASSERT_TRUE(hist.undo(doc)); // pops the thread
    const double vHoled = vol(doc.getBody(rodId));
    EXPECT_GT(vHoled, vFinal);
    EXPECT_LT(vHoled, vRod);
    ASSERT_TRUE(hist.undo(doc)); // pops the boolean
    EXPECT_NEAR(vol(doc.getBody(rodId)), vRod, 1e-3);

    ASSERT_TRUE(hist.redo(doc));
    ASSERT_TRUE(hist.redo(doc));
    EXPECT_NEAR(vol(doc.getBody(rodId)), vFinal, 1e-3);
    EXPECT_TRUE(BRepCheck_Analyzer(doc.getBody(rodId)).IsValid());
}

// ─── Phase 2: ThreadFollows - the thread tracks its cylinder via face ref ────

namespace {

// Rod built through the REAL sketch→extrude pipeline (so topo naming has
// provenance) and threaded with a MINTED face ref - the app's exact path.
struct RefRod {
    int bodyId = -1;
    int sketchId = -1;
    int centerPt = -1;
    int threadStep = -1;
    std::shared_ptr<Sketch> sk;
};

int biggestBody(Document& doc) {
    int best = -1; double bv = -1.0;
    for (int id : doc.getAllBodyIds()) {
        double v = vol(doc.getBody(id));
        if (v > bv) { bv = v; best = id; }
    }
    return best;
}

RefRod buildThreadedRefRod(Document& doc, History& hist, double r, double h,
                           double pitch, double depth) {
    RefRod out;
    out.sk = std::make_shared<Sketch>();
    out.sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0,0,0), gp_Dir(0,0,1),
                                   gp_Dir(1,0,0))));
    out.centerPt = out.sk->addPoint(glm::vec2(0.0f, 0.0f));
    out.sk->addCircle(out.centerPt, r);
    out.sketchId = doc.addSketch(out.sk, "RodSketch");
    auto e = std::make_unique<ExtrudeOp>();
    e->setSketchSource(out.sketchId);
    char params[160];
    std::snprintf(params, sizeof params,
                  "sketch=%d;dist=%.3f;dir=0;mode=0;target=-1;draft=0;"
                  "regions=0.0:0.0", out.sketchId, h);
    e->deserializeParams(params);
    if (!e->rebuildProfileFromSketch(doc)) return out;
    e->setDistance(h);
    e->setMode(ExtrudeMode::NewBody);
    if (!hist.pushOperation(std::move(e), doc)) return out;
    out.bodyId = biggestBody(doc);

    // Mint the cylinder-face ref exactly as beginThread does.
    TopoDS_Face cyl;
    for (TopExp_Explorer fx(doc.getBody(out.bodyId), TopAbs_FACE);
         fx.More(); fx.Next()) {
        TopoDS_Face f = TopoDS::Face(fx.Current());
        if (BRepAdaptor_Surface(f).GetType() == GeomAbs_Cylinder) {
            cyl = f;
            break;
        }
    }
    if (cyl.IsNull()) { out.bodyId = -1; return out; }
    materializr::topo::Context ctx;
    ctx.doc = &doc;
    ctx.shape = doc.getBody(out.bodyId);
    ctx.type = TopAbs_FACE;
    materializr::topo::Ref ref = materializr::topo::mint(cyl, ctx);

    auto t = std::make_unique<ThreadOp>();
    t->setBody(out.bodyId);
    t->setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t->setRadius(r);
    t->setLength(h);
    t->setPitch(pitch);
    t->setDepth(depth);
    t->setIsHole(false);
    t->setTargetFaceRef(ref);
    if (!hist.pushOperation(std::move(t), doc)) { out.bodyId = -1; return out; }
    out.threadStep = hist.currentStep();
    return out;
}

// Reference volume: the same thread swept on a plain cylinder of the given
// radius/height at the origin (position-independent - volume only).
double refThreadedVol(double r, double h, double pitch, double depth) {
    ThreadOp t;
    t.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    t.setRadius(r); t.setLength(h); t.setPitch(pitch); t.setDepth(depth);
    t.setIsHole(false);
    TopoDS_Shape res =
        t.buildResult(BRepPrimAPI_MakeCylinder(r, h).Shape());
    return vol(res);
}

} // namespace

TEST(ThreadFollows, ReloadedOpKeepsItsAxis) {
    // deserializeParams filled the axis COMPONENTS but never rebuilt the
    // gp_Ax2 - every reloaded op's getAxis() reported a default Z-axis, so
    // the sketch-on-cap centre concluded "no thread axis pierces this
    // plane" on ANY loaded project (in-process tests never caught it).
    ThreadOp a;
    a.setBody(7);
    a.setAxis(gp_Ax2(gp_Pnt(1, 2, 3), gp_Dir(0, -1, 0), gp_Dir(1, 0, 0)));
    a.setRadius(8); a.setLength(20); a.setPitch(2); a.setDepth(1.2);
    ThreadOp b;
    ASSERT_TRUE(b.deserializeParams(a.serializeParams()));
    EXPECT_NEAR(b.getAxis().Location().X(), 1.0, 1e-9);
    EXPECT_NEAR(b.getAxis().Location().Y(), 2.0, 1e-9);
    EXPECT_NEAR(b.getAxis().Location().Z(), 3.0, 1e-9);
    EXPECT_NEAR(b.getAxis().Direction().Y(), -1.0, 1e-9);
}

TEST(ThreadFollows, ResizeCylinderRethreadsAtNewRadius) {
    Document doc; History hist;
    RefRod rod = buildThreadedRefRod(doc, hist, 8.0, 20.0, 3.0, 1.2);
    ASSERT_GE(rod.bodyId, 0);

    auto rs = std::make_unique<ResizeCylindricalOp>();
    rs->setBody(rod.bodyId);
    rs->setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    rs->setHeight(20.0);
    rs->setOldRadii(8.0, 8.0);
    rs->setNewRadii(10.0, 10.0);
    rs->setIsHole(false);
    ASSERT_TRUE(hist.pushOperation(std::move(rs), doc))
        << "resize on a threaded rod must reflow, not fail";

    EXPECT_EQ(hist.getStep(hist.currentStep())->typeId(), "thread");
    TopoDS_Shape body = doc.getBody(rod.bodyId);
    ASSERT_FALSE(body.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(body).IsValid());
    // The thread must sit at the NEW surface: just inside r=10 the ring is
    // part crest (solid) and part groove (void), and nothing extends past
    // r=10. A stale r=8 cut leaves the r=9.9 ring FULLY solid (threads
    // buried inside the fatter rod).
    const int near10 = ringHits(body, 0.0, 0.0, 9.9, 10.0);
    EXPECT_GT(near10, 0) << "crest material at the new radius";
    EXPECT_LT(near10, 16) << "groove openings at the new surface - a full "
                             "ring means the thread is buried (stale r=8)";
    EXPECT_EQ(ringHits(body, 0.0, 0.0, 10.15, 10.0), 0)
        << "nothing past the new radius";
    const double vRef = refThreadedVol(10.0, 20.0, 3.0, 1.2);
    EXPECT_NEAR(vol(body), vRef, 0.01 * vRef)
        << "thread must re-cut at r=10 (ref-resolved), not stay at r=8";
}

TEST(ThreadFollows, MovedBodyRethreadsAtNewAxisOnEdit) {
    Document doc; History hist;
    RefRod rod = buildThreadedRefRod(doc, hist, 8.0, 20.0, 3.0, 1.2);
    ASSERT_GE(rod.bodyId, 0);
    const double vBefore = vol(doc.getBody(rod.bodyId));

    auto tr = std::make_unique<TransformOp>();
    tr->setBodyId(rod.bodyId);
    tr->setType(TransformType::Translate);
    tr->setTranslation(25.0, 0.0, 0.0);
    ASSERT_TRUE(hist.pushOperation(std::move(tr), doc));
    EXPECT_NEAR(vol(doc.getBody(rod.bodyId)), vBefore, 1e-3)
        << "rigid move keeps the thread as-is";

    // Edit the thread's pitch - the recompute must re-cut at the MOVED axis
    // (face ref), not at the original origin (stale params = null cut).
    ThreadOp* th = dynamic_cast<ThreadOp*>(
        const_cast<Operation*>(hist.getStep(rod.threadStep)));
    ASSERT_NE(th, nullptr);
    th->setPitch(2.0);
    ASSERT_TRUE(hist.editStep(rod.threadStep, doc, true))
        << "pitch edit after a move must re-cut at the moved axis";

    TopoDS_Shape body = doc.getBody(rod.bodyId);
    ASSERT_FALSE(body.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(body).IsValid());
    Bnd_Box bb; BRepBndLib::Add(body, bb);
    double x1,y1,z1,x2,y2,z2; bb.Get(x1,y1,z1,x2,y2,z2);
    EXPECT_NEAR(0.5 * (x1 + x2), 25.0, 1e-2) << "body stays at the moved spot";
    const double vRef = refThreadedVol(8.0, 20.0, 2.0, 1.2);
    EXPECT_NEAR(vol(body), vRef, 0.01 * vRef);
}

TEST(ThreadFollows, SketchMovedCircleCascadesThreadToNewSpot) {
    Document doc; History hist;
    RefRod rod = buildThreadedRefRod(doc, hist, 8.0, 20.0, 3.0, 1.2);
    ASSERT_GE(rod.bodyId, 0);

    // Move the rod's circle in the source sketch and cascade from the extrude.
    rod.sk->movePoint(rod.centerPt, glm::vec2(15.0f, 0.0f));
    int extrudeIdx = -1;
    for (int i = 0; i <= hist.currentStep(); ++i) {
        Operation* op = const_cast<Operation*>(hist.getStep(i));
        if (auto* e = dynamic_cast<ExtrudeOp*>(op)) {
            if (e->getSketchId() == rod.sketchId &&
                e->rebuildProfileFromSketch(doc)) { extrudeIdx = i; break; }
        }
    }
    ASSERT_GE(extrudeIdx, 0);
    if (auto s2 = doc.getSketch(rod.sketchId))
        doc.setCascadeSketchOverride(rod.sketchId,
                                     std::make_shared<Sketch>(*s2));
    ASSERT_TRUE(hist.editStep(extrudeIdx, doc, true))
        << "sketch-move cascade through the thread must succeed";

    TopoDS_Shape body = doc.getBody(biggestBody(doc));
    ASSERT_FALSE(body.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(body).IsValid());
    Bnd_Box bb; BRepBndLib::Add(body, bb);
    double x1,y1,z1,x2,y2,z2; bb.Get(x1,y1,z1,x2,y2,z2);
    EXPECT_NEAR(0.5 * (x1 + x2), 15.0, 1e-2) << "rod rebuilt at the new spot";
    const double vRef = refThreadedVol(8.0, 20.0, 3.0, 1.2);
    EXPECT_NEAR(vol(body), vRef, 0.01 * vRef)
        << "thread must follow the moved cylinder, not vanish or misplace";
}

TEST(ThreadFollows, InternalRoundedRingSplice) {
    // The nut side of the Rounded pair: internal thread built by the ring
    // splice (bore to major + constructed thread ring + GLUED plain-seam
    // fuse), not the flat-topped rope grooves. Two proportion regimes: the
    // fat/short tube AND Steve's 10x20mm hole (r=5, 20 deep, ~10 turns) -
    // the plain fuse INVERTED on the latter (fused vol -409 vs +1994)
    // until the seam went to glue mode.
    struct Cfg { double rOut, rBore, len, pitch; };
    for (const Cfg& c : {Cfg{16.0, 10.0, 9.0, 3.0},
                         Cfg{10.0, 5.0, 20.0, 2.0}}) {
        SCOPED_TRACE(::testing::Message()
                     << "rOut=" << c.rOut << " rBore=" << c.rBore
                     << " len=" << c.len << " pitch=" << c.pitch);
        TopoDS_Shape tube;
        {
            TopoDS_Shape solid =
                BRepPrimAPI_MakeCylinder(c.rOut, c.len).Shape();
            TopoDS_Shape boreC =
                BRepPrimAPI_MakeCylinder(c.rBore, c.len).Shape();
            tube = BRepAlgoAPI_Cut(solid, boreC).Shape();
        }
        ASSERT_FALSE(tube.IsNull());
        const double minor = c.rBore, depth = 1.2, major = minor + depth;
        const double vBoredMajor =
            vol(BRepAlgoAPI_Cut(
                    BRepPrimAPI_MakeCylinder(c.rOut, c.len).Shape(),
                    BRepPrimAPI_MakeCylinder(major, c.len).Shape())
                    .Shape());
        const double vTube = vol(tube);

        ThreadOp t;
        t.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1),
                         gp_Dir(1, 0, 0)));
        t.setRadius(minor);
        t.setLength(c.len);
        t.setPitch(c.pitch);
        t.setDepth(depth);
        t.setIsHole(true);
        t.setProfile(ThreadProfile::Rounded);
        TopoDS_Shape res = t.buildResult(tube);
        ASSERT_FALSE(res.IsNull());
        EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());
        // Between "bored clean to major" and the full tube (ridges added).
        EXPECT_GT(vol(res), vBoredMajor + 1.0);
        EXPECT_LT(vol(res), vTube - 1.0);
        // Bore open; mid-thread ring part ridge, part groove.
        EXPECT_EQ(ringHits(res, 0.0, 0.0, minor * 0.5, c.len * 0.5), 0)
            << "bore filled";
        const int mid =
            ringHits(res, 0.0, 0.0, 0.5 * (minor + major), c.len * 0.5);
        EXPECT_GT(mid, 0) << "no thread ridge material";
        EXPECT_LT(mid, 16) << "solid ring - thread grooves missing";
    }
}
