// THE TOPOLOGICAL-NAMING CONTRACT. One named case per (reference kind ×
// upstream-edit kind). "Topo naming works" MEANS this suite is green - a
// verbal "it's done" doesn't count. When a new breakage is found in the wild,
// it becomes a new case here FIRST, then gets fixed.
//
// Every case drives the REAL replay path (History::editStep / the sketch-edit
// cascade with the override pinned), not op internals - the gaps that bit us
// repeatedly lived between the ops and the replay machinery, not inside
// either.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/History.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/FilletOp.h"
#include "modeling/TaperOp.h"
#include "modeling/ProjectSketchOp.h"
#include "modeling/TransformOp.h"
#include "modeling/FaceLineage.h"

#include <BRepAdaptor_Curve.hxx>
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
#include <vector>

using materializr::Sketch;
using namespace materializr;

namespace {

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

std::shared_ptr<Sketch> makeRect(double x0, double y0, double x1, double y1,
                                 int pid[4]) {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1),
                               gp_Dir(1, 0, 0))));
    pid[0] = sk->addPoint({(float)x0, (float)y0});
    pid[1] = sk->addPoint({(float)x1, (float)y0});
    pid[2] = sk->addPoint({(float)x1, (float)y1});
    pid[3] = sk->addPoint({(float)x0, (float)y1});
    for (int i = 0; i < 4; ++i) sk->addLine(pid[i], pid[(i + 1) % 4]);
    return sk;
}

// Straight edge whose two endpoints both satisfy pred.
template <class Pred>
TopoDS_Edge lineEdgeWhere(const TopoDS_Shape& body, Pred pred) {
    for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.GetType() != GeomAbs_Line) continue;
        gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
        if (pred(a) && pred(b)) return TopoDS::Edge(ex.Current());
    }
    return {};
}

// Planar faces whose normal is diagonal in the Y-Z plane (|ny|,|nz| both
// significant, |nx| tiny) - chamfer bevels of a Y-edge. Sorted large→small.
std::vector<TopoDS_Face> yzBevels(const TopoDS_Shape& body) {
    std::vector<std::pair<double, TopoDS_Face>> got;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) continue;
        gp_Dir n = s.Plane().Axis().Direction();
        if (std::abs(n.X()) > 0.1) continue;
        if (std::abs(n.Y()) < 0.2 || std::abs(n.Z()) < 0.2) continue;
        GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
        got.push_back({g.Mass(), f});
    }
    std::sort(got.begin(), got.end(),
              [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<TopoDS_Face> out;
    for (auto& [a, f] : got) out.push_back(f);
    return out;
}

gp_Pnt centroid(const TopoDS_Face& f) {
    GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
    return g.CentreOfMass();
}

// Push an extrude of `sk` and return the new body id.
int pushExtrude(Document& doc, History& hist, int sid, double dist,
                ExtrudeOp** out = nullptr) {
    auto ext = std::make_unique<ExtrudeOp>();
    if (out) *out = ext.get();
    ext->setSketchSource(sid);
    ext->setDistance(dist);
    EXPECT_TRUE(ext->rebuildProfileFromSketch(doc));
    EXPECT_TRUE(ext->execute(doc));
    hist.pushExecuted(std::move(ext));
    return doc.getAllBodyIds().back();
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// CASE 1 - edge ref held by a downstream chamfer, where the edge is OWNED BY
// an upstream chamfer's bevel (op-generated geometry, no sketch feature under
// it). Resizing the upstream chamfer moves the edge; the downstream chamfer
// must follow it (gen scheme against the republished ledger).
TEST(TopoContract, BevelEdgeChamfer_FollowsUpstreamChamferResize) {
    Document doc;
    History hist;
    int pid[4];
    auto sk = makeRect(0, 0, 20, 20, pid);
    int sid = doc.addSketch(sk);
    int body = pushExtrude(doc, hist, sid, 10.0);

    // Chamfer A: top-front edge (y=0, z=10), d=2.
    auto chA = std::make_unique<ChamferOp>();
    ChamferOp* chAp = chA.get();
    chA->setBody(body);
    chA->setEdges({lineEdgeWhere(doc.getBody(body), [](const gp_Pnt& p) {
        return std::abs(p.Y()) < 1e-7 && std::abs(p.Z() - 10.0) < 1e-7;
    })});
    chA->setDistance(2.0);
    ASSERT_TRUE(chA->execute(doc));
    hist.pushExecuted(std::move(chA));

    // Chamfer B on the bevel's LOWER boundary edge (y=0, z=8) - an edge that
    // exists only because A does.
    TopoDS_Edge lower = lineEdgeWhere(doc.getBody(body), [](const gp_Pnt& p) {
        return std::abs(p.Y()) < 1e-7 && std::abs(p.Z() - 8.0) < 1e-6;
    });
    ASSERT_FALSE(lower.IsNull());
    auto chB = std::make_unique<ChamferOp>();
    chB->setBody(body);
    chB->setEdges({lower});
    chB->setDistance(0.5);
    ASSERT_TRUE(chB->execute(doc));
    hist.pushExecuted(std::move(chB));

    // THE EDIT: A grows 2 → 4 (the app: set params on the step, editStep).
    chAp->setDistance(4.0);
    ASSERT_TRUE(hist.editStep(1, doc, /*transactional=*/true))
        << "downstream chamfer must re-resolve its bevel-owned edge";

    const TopoDS_Shape& out = doc.getBody(body);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    // B's small bevel must sit on the MOVED lower boundary (z≈6), not the old
    // z≈8 (stale) - and must exist at all (not silently dropped).
    auto bevels = yzBevels(out);
    ASSERT_GE(bevels.size(), 2u) << "both bevels must exist after the edit";
    gp_Pnt cB = centroid(bevels.back());   // smallest = B's
    EXPECT_NEAR(cB.Z(), 6.0, 0.6)
        << "B must follow A's resize down to z~6 (got z=" << cB.Z() << ")";
}

// ───────────────────────────────────────────────────────────────────────────
// CASE 2 - the light-cover scenario (#56): a hole punched THROUGH a bevel,
// then the hole's top rim chamfered. Resizing the big bevel shortens the rim
// fragments. The rim chamfer must re-resolve onto the shorter fragments - or
// at minimum degrade gracefully (model intact, tail suspended) rather than
// corrupt.
TEST(TopoContract, HoleRimChamfer_FollowsBevelResize) {
    Document doc;
    History hist;
    int pid[4];
    auto sk = makeRect(0, 0, 40, 20, pid);
    int sid = doc.addSketch(sk);
    int body = pushExtrude(doc, hist, sid, 10.0);

    // Big bevel: chamfer the top-front long edge (y=0, z=10), d=6.
    auto chA = std::make_unique<ChamferOp>();
    ChamferOp* chAp = chA.get();
    chA->setBody(body);
    chA->setEdges({lineEdgeWhere(doc.getBody(body), [](const gp_Pnt& p) {
        return std::abs(p.Y()) < 1e-7 && std::abs(p.Z() - 10.0) < 1e-7;
    })});
    chA->setDistance(6.0);
    ASSERT_TRUE(chA->execute(doc));
    hist.pushExecuted(std::move(chA));

    // SQUARE hole crossing the bevel's top boundary (y=6): rect x[8,14],
    // y[3,9], cut all the way through (symmetric so plane placement is
    // irrelevant). Straight rim edges - the light cover's actual geometry.
    int hpid[4];
    auto skHole = makeRect(8, 3, 14, 9, hpid);
    int hsid = doc.addSketch(skHole);
    auto cut = std::make_unique<ExtrudeOp>();
    cut->setSketchSource(hsid);
    cut->setMode(ExtrudeMode::Subtract);
    cut->setTargetBody(body);
    cut->setDirection(ExtrudeDirection::Symmetric);
    cut->setDistance(30.0);
    ASSERT_TRUE(cut->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(cut->execute(doc));
    hist.pushExecuted(std::move(cut));

    // Rim chamfer: the hole's TOP-FACE rim fragments - the straight edges at
    // z=10 shared with the flat top (two partial side edges y in [6,9] and
    // the back edge y=9). These are exactly the edges the bevel resize will
    // shorten.
    std::vector<TopoDS_Edge> rim;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_EDGE); ex.More();
         ex.Next()) {
        BRepAdaptor_Curve cc(TopoDS::Edge(ex.Current()));
        if (cc.GetType() != GeomAbs_Line) continue;
        gp_Pnt a = cc.Value(cc.FirstParameter()), b = cc.Value(cc.LastParameter());
        if (std::abs(a.Z() - 10.0) > 1e-6 || std::abs(b.Z() - 10.0) > 1e-6)
            continue;
        // On the hole outline: x in {8,14} (side edges) or y=9 (back edge),
        // interior to the top face (exclude the box's own outline).
        gp_Pnt m((a.XYZ() + b.XYZ()) * 0.5);
        const bool side = (std::abs(m.X() - 8.0) < 1e-6 ||
                           std::abs(m.X() - 14.0) < 1e-6) && m.Y() > 6.0 - 1e-6;
        const bool back = std::abs(m.Y() - 9.0) < 1e-6 && m.X() > 8.0 &&
                          m.X() < 14.0;
        if (!side && !back) continue;
        // TopExp_Explorer re-visits an edge once per owning face - dedup.
        bool dup = false;
        for (const auto& e : rim)
            if (e.IsSame(ex.Current())) { dup = true; break; }
        if (!dup) rim.push_back(TopoDS::Edge(ex.Current()));
    }
    ASSERT_EQ(rim.size(), 3u) << "hole must have 3 top-face rim fragments";
    auto chB = std::make_unique<ChamferOp>();
    chB->setBody(body);
    chB->setEdges(rim);
    chB->setDistance(0.4);
    ASSERT_TRUE(chB->execute(doc));
    hist.pushExecuted(std::move(chB));
    const double volBefore = volumeOf(doc.getBody(body));

    // THE EDIT: bevel grows 6 → 8. Rim fragments on the top face shrink.
    chAp->setDistance(8.0);
    const bool ok = hist.editStep(1, doc, /*transactional=*/true);

    // HARD (graceful-degradation floor, must always hold): the body is valid
    // and the bevel resize itself landed - no corruption, no half-replay.
    const TopoDS_Shape& out = doc.getBody(body);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    ASSERT_FALSE(yzBevels(out).empty());
    EXPECT_NEAR(centroid(yzBevels(out).front()).Y(), 4.0, 1.0)
        << "the resized bevel (d=8, centroid y~4) must be present";
    // TARGET (the actual contract): the rim chamfer re-resolves onto the
    // shortened fragments and the whole chain replays.
    EXPECT_TRUE(ok) << "rim chamfer must survive the bevel resize";
    if (ok) EXPECT_LT(volumeOf(out), volBefore)
        << "d=8 bevel removes more material than d=6";
}

// ───────────────────────────────────────────────────────────────────────────
// CASE 3 - face refs held by TaperOp (draft angle). Today TaperOp feeds its
// STORED face handles straight to BRepOffsetAPI_DraftAngle, so any upstream
// rebuild strands it. Contract: the tapered wall follows a sketch resize
// through the real cascade replay.
TEST(TopoContract, TaperFace_FollowsSketchResizeThroughReplay) {
    Document doc;
    History hist;
    int pid[4];
    auto sk = makeRect(0, 0, 20, 10, pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp* extP = nullptr;
    int body = pushExtrude(doc, hist, sid, 10.0, &extP);

    // Taper the +X wall (x=20) by 10°, neutral at the bottom.
    TopoDS_Face wall;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_FACE); ex.More();
         ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) continue;
        if (std::abs(s.Plane().Axis().Direction().X()) < 0.999) continue;
        if (std::abs(centroid(f).X() - 20.0) > 1e-6) continue;
        wall = f;
        break;
    }
    ASSERT_FALSE(wall.IsNull());
    auto tp = std::make_unique<TaperOp>();
    tp->setBody(body);
    tp->addFace(wall);
    tp->setDirection(0, 0, 1);
    tp->setNeutralPoint(20, 5, 0);
    tp->setAngleDeg(10.0);
    ASSERT_TRUE(tp->execute(doc));
    hist.pushExecuted(std::move(tp));

    // Sketch edit: widen 20 → 30 (the wall moves), recorded the app's way.
    auto before = std::make_shared<Sketch>(*sk);
    sk->movePoint(pid[1], {30.0f, 0.0f});
    sk->movePoint(pid[2], {30.0f, 10.0f});
    auto after = std::make_shared<Sketch>(*sk);
    hist.pushExecuted(std::make_unique<SketchEditOp>(sk, before, after));
    ASSERT_TRUE(extP->rebuildProfileFromSketch(doc));
    doc.setCascadeSketchOverride(sid, std::make_shared<Sketch>(*sk));
    bool ok = hist.editStep(0, doc, /*transactional=*/true);
    doc.clearCascadeSketchOverrides();

    ASSERT_TRUE(ok) << "taper must re-resolve its wall after the resize";
    // The moved wall must be TILTED ~10° off X (a draft survived), i.e. some
    // planar face with nx≈cos(10°) and |nz|≈sin(10°).
    bool tilted = false;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_FACE); ex.More();
         ex.Next()) {
        BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
        if (s.GetType() != GeomAbs_Plane) continue;
        gp_Dir n = s.Plane().Axis().Direction();
        if (std::abs(std::abs(n.X()) - std::cos(10.0 * M_PI / 180.0)) < 0.02 &&
            std::abs(std::abs(n.Z()) - std::sin(10.0 * M_PI / 180.0)) < 0.02) {
            tilted = true;
            break;
        }
    }
    EXPECT_TRUE(tilted) << "the widened body must still carry the 10° draft";
}

// ───────────────────────────────────────────────────────────────────────────
// CASE 4 - ProjectSketchOp's TARGET FACE (its sketch side already re-derives;
// the face it stamps into is a raw stored handle today). Contract: the
// engraving follows the top face across a sketch resize.
TEST(TopoContract, ProjectSketchTargetFace_FollowsSketchResize) {
    Document doc;
    History hist;
    int pid[4];
    auto sk = makeRect(0, 0, 20, 20, pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp* extP = nullptr;
    int body = pushExtrude(doc, hist, sid, 10.0, &extP);
    const double solidVol = volumeOf(doc.getBody(body));

    // Circle to engrave, sketched above the top face.
    auto skC = std::make_shared<Sketch>();
    skC->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 20), gp_Dir(0, 0, 1),
                                gp_Dir(1, 0, 0))));
    int c = skC->addPoint({10.0f, 10.0f});
    skC->addCircle(c, 2.0);
    int csid = doc.addSketch(skC);

    // Target: the top face (z=10).
    TopoDS_Face top;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_FACE); ex.More();
         ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) continue;
        if (std::abs(s.Plane().Axis().Direction().Z()) < 0.999) continue;
        if (std::abs(centroid(f).Z() - 10.0) > 1e-6) continue;
        top = f;
        break;
    }
    ASSERT_FALSE(top.IsNull());
    auto pr = std::make_unique<ProjectSketchOp>();
    pr->setBody(body);
    pr->setTargetFace(top);
    pr->setSketchId(csid);
    pr->setDepth(1.0);
    pr->setMode(ProjectSketchOp::Mode::Engrave);
    ASSERT_TRUE(pr->execute(doc));
    hist.pushExecuted(std::move(pr));
    ASSERT_LT(volumeOf(doc.getBody(body)), solidVol - 5.0)
        << "initial engrave must remove material";

    // Sketch edit widens the base 20 → 30 AND the extrude grows 10 → 15, so
    // the top face MOVES to z=15. A stale target-face handle would stamp at
    // the old z=10 plane (buried inside the solid) - only a true re-resolve
    // puts the pocket floor at z=14.
    auto before = std::make_shared<Sketch>(*sk);
    sk->movePoint(pid[1], {30.0f, 0.0f});
    sk->movePoint(pid[2], {30.0f, 20.0f});
    auto after = std::make_shared<Sketch>(*sk);
    hist.pushExecuted(std::make_unique<SketchEditOp>(sk, before, after));
    ASSERT_TRUE(extP->rebuildProfileFromSketch(doc));
    extP->setDistance(15.0);
    doc.setCascadeSketchOverride(sid, std::make_shared<Sketch>(*sk));
    bool ok = hist.editStep(0, doc, /*transactional=*/true);
    doc.clearCascadeSketchOverrides();

    ASSERT_TRUE(ok) << "projection must re-resolve its target face";
    // The engraving's floor must be a small planar face at z≈14 (depth 1
    // below the MOVED top at z=15).
    bool floorAt14 = false;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_FACE); ex.More();
         ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) continue;
        if (std::abs(s.Plane().Axis().Direction().Z()) < 0.999) continue;
        if (std::abs(centroid(f).Z() - 14.0) > 1e-4) continue;
        floorAt14 = true;
        break;
    }
    EXPECT_TRUE(floorAt14)
        << "pocket floor must sit 1mm under the MOVED top (z=14) - a stale "
           "target handle stamps at the old z=10 plane instead";
}

// ───────────────────────────────────────────────────────────────────────────
// CASE 5 - face-lineage COVERAGE after a fillet. ChamferOp complete()s the
// FaceIdMap so every downstream face has an ancestry id; FilletOp historically
// didn't, leaving holes that only bite two ops later. Contract: after any
// fillet, EVERY face of the result carries at least one lineage id.
TEST(TopoContract, FilletFaceIdCoverage_Complete) {
    Document doc;
    History hist;
    int pid[4];
    auto sk = makeRect(0, 0, 20, 20, pid);
    int sid = doc.addSketch(sk);
    int body = pushExtrude(doc, hist, sid, 10.0);

    auto fl = std::make_unique<FilletOp>();
    fl->setBody(body);
    fl->setEdges({lineEdgeWhere(doc.getBody(body), [](const gp_Pnt& p) {
        return std::abs(p.Y()) < 1e-7 && std::abs(p.Z() - 10.0) < 1e-7;
    })});
    fl->setRadius(2.0);
    ASSERT_TRUE(fl->execute(doc));
    hist.pushExecuted(std::move(fl));

    const topo::FaceIdMap* m = doc.bodyFaceIds(body);
    ASSERT_NE(m, nullptr) << "fillet must publish a FaceIdMap";
    int uncovered = 0, total = 0;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_FACE); ex.More();
         ex.Next()) {
        ++total;
        const std::vector<int>* ids = topo::idsFor(*m, ex.Current());
        if (!ids || ids->empty()) ++uncovered;
    }
    EXPECT_EQ(uncovered, 0)
        << uncovered << "/" << total
        << " faces carry no lineage id after a fillet (chamfer covers all)";
}

// ───────────────────────────────────────────────────────────────────────────
// CASE 6 - editing a step BETWEEN the lineage producer and the consumer. The
// boolean's ledger is wiped by the transform's updateBody and the boolean is
// NOT replayed (edit starts after it) - the consumer must still re-resolve.
// Chamfer has a FaceIdMap lineage tier (and Transform carries the map), so it
// should survive; Fillet lacks that tier - this is the parity gap.
TEST(TopoContract, ChamferOnSeam_SurvivesEditOfInterveningTransform) {
    Document doc;
    History hist;
    int pidA[4], pidB[4];
    auto skA = makeRect(0, 0, 20, 20, pidA);
    int sidA = doc.addSketch(skA);
    int body = pushExtrude(doc, hist, sidA, 10.0);

    // Union an offset second box: seam corner at (20, 10) in plan.
    auto skB = makeRect(10, -10, 30, 10, pidB);
    int sidB = doc.addSketch(skB);
    auto ext2 = std::make_unique<ExtrudeOp>();
    ext2->setSketchSource(sidB);
    ext2->setMode(ExtrudeMode::Union);
    ext2->setTargetBody(body);
    ext2->setDistance(10.0);
    ASSERT_TRUE(ext2->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext2->execute(doc));
    hist.pushExecuted(std::move(ext2));

    // Transform: slide the whole body +5 in X.
    auto tr = std::make_unique<TransformOp>();
    TransformOp* trP = tr.get();
    tr->setBodyId(body);
    tr->setType(TransformType::Translate);
    tr->setTranslation(5.0, 0.0, 0.0);
    ASSERT_TRUE(tr->execute(doc));
    hist.pushExecuted(std::move(tr));

    // Chamfer the seam's vertical corner edge - now at (25, 10). This edge
    // exists only because of the union (no sketch vertex under it).
    TopoDS_Edge seam = lineEdgeWhere(doc.getBody(body), [](const gp_Pnt& p) {
        return std::abs(p.X() - 25.0) < 1e-6 && std::abs(p.Y() - 10.0) < 1e-6;
    });
    ASSERT_FALSE(seam.IsNull()) << "union must create the seam corner edge";
    auto ch = std::make_unique<ChamferOp>();
    ch->setBody(body);
    ch->setEdges({seam});
    ch->setDistance(1.5);
    ASSERT_TRUE(ch->execute(doc));
    hist.pushExecuted(std::move(ch));

    // THE EDIT: change the transform (5 → 10). Replay covers only the
    // transform and the chamfer - the union (lineage producer) does NOT
    // re-execute, so no ledger exists when the chamfer re-resolves.
    trP->setTranslation(10.0, 0.0, 0.0);
    ASSERT_TRUE(hist.editStep(2, doc, /*transactional=*/true))
        << "seam chamfer must re-resolve across the moved body (lineage tier)";

    // The bevel must sit at the MOVED seam (30, 10) - a vertical planar face
    // with a diagonal X-Y normal whose centroid is near that corner.
    bool found = false;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_FACE); ex.More();
         ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) continue;
        gp_Dir n = s.Plane().Axis().Direction();
        if (std::abs(n.Z()) > 0.1) continue;
        if (std::abs(n.X()) < 0.2 || std::abs(n.Y()) < 0.2) continue;
        gp_Pnt cf = centroid(f);
        if (std::hypot(cf.X() - 30.0, cf.Y() - 10.0) < 2.5) { found = true; break; }
    }
    EXPECT_TRUE(found) << "bevel must land on the seam at its NEW position";
}

// ───────────────────────────────────────────────────────────────────────────
// CASE 7 - DISTINCT-CLAIM in the lineage edge tier. Fragment edges of one
// span share the SAME adjacent-face-id pair; without per-edge claiming the
// resolver handed every pair the first matching edge, collapsing a
// 3-fragment selection to 1 and starving the rebuild - which made edits
// ORDER-DEPENDENT ("17 works fresh, fails after any successful edit").
// Contract: two consecutive successful edits on a fragmented-edge chamfer.
TEST(TopoContract, FragmentedEdgeChamfer_SurvivesConsecutiveEdits) {
    Document doc;
    History hist;
    int pid[4];
    auto sk = makeRect(0, 0, 40, 20, pid);
    int sid = doc.addSketch(sk);
    int body = pushExtrude(doc, hist, sid, 10.0);

    // Two notches crossing the top-front edge (y=0, z=10) - fragments it
    // into three pieces.
    for (double x0 : {10.0, 26.0}) {
        int npid[4];
        auto nk = makeRect(x0, -2, x0 + 4, 2, npid);
        int nsid = doc.addSketch(nk);
        auto cut = std::make_unique<ExtrudeOp>();
        cut->setSketchSource(nsid);
        cut->setMode(ExtrudeMode::Subtract);
        cut->setTargetBody(body);
        cut->setDirection(ExtrudeDirection::Symmetric);
        cut->setDistance(30.0);
        ASSERT_TRUE(cut->rebuildProfileFromSketch(doc));
        ASSERT_TRUE(cut->execute(doc));
        hist.pushExecuted(std::move(cut));
    }

    // Chamfer all three fragments of the front-top edge.
    std::vector<TopoDS_Edge> frags;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_EDGE); ex.More();
         ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.GetType() != GeomAbs_Line) continue;
        gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
        if (std::abs(a.Y()) > 1e-7 || std::abs(b.Y()) > 1e-7) continue;
        if (std::abs(a.Z() - 10.0) > 1e-7 || std::abs(b.Z() - 10.0) > 1e-7)
            continue;
        bool dup = false;
        for (const auto& e : frags)
            if (e.IsSame(ex.Current())) { dup = true; break; }
        if (!dup) frags.push_back(TopoDS::Edge(ex.Current()));
    }
    ASSERT_EQ(frags.size(), 3u) << "notches must fragment the edge into 3";
    auto ch = std::make_unique<ChamferOp>();
    ChamferOp* chP = ch.get();
    ch->setBody(body);
    ch->setEdges(frags);
    ch->setDistance(2.0);
    ASSERT_TRUE(ch->execute(doc));
    hist.pushExecuted(std::move(ch));
    const int chIdx = hist.stepCount() - 1;
    const double vol2 = volumeOf(doc.getBody(body));

    // Two consecutive edits. The FIRST recaptures the (identical) face-id
    // pairs; the SECOND resolves through them - pre-fix it collapsed to one
    // fragment and failed / built a partial bevel.
    chP->setDistance(3.0);
    ASSERT_TRUE(hist.editStep(chIdx, doc, /*transactional=*/true))
        << "first re-edit must succeed";
    chP->setDistance(4.0);
    ASSERT_TRUE(hist.editStep(chIdx, doc, /*transactional=*/true))
        << "second re-edit must succeed (order-independence)";
    const TopoDS_Shape& out = doc.getBody(body);
    EXPECT_TRUE(BRepCheck_Analyzer(out).IsValid());
    // d=4 removes strictly more than d=2 did - all three fragments beveled,
    // not a collapsed single-fragment partial.
    EXPECT_LT(volumeOf(out), vol2 - 1.0)
        << "full-width bevel must be present at d=4";
}

// Same chain, consumer is a FILLET (no lineage tier today) - parity contract.
TEST(TopoContract, FilletOnSeam_SurvivesEditOfInterveningTransform) {
    Document doc;
    History hist;
    int pidA[4], pidB[4];
    auto skA = makeRect(0, 0, 20, 20, pidA);
    int sidA = doc.addSketch(skA);
    int body = pushExtrude(doc, hist, sidA, 10.0);

    auto skB = makeRect(10, -10, 30, 10, pidB);
    int sidB = doc.addSketch(skB);
    auto ext2 = std::make_unique<ExtrudeOp>();
    ext2->setSketchSource(sidB);
    ext2->setMode(ExtrudeMode::Union);
    ext2->setTargetBody(body);
    ext2->setDistance(10.0);
    ASSERT_TRUE(ext2->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext2->execute(doc));
    hist.pushExecuted(std::move(ext2));

    auto tr = std::make_unique<TransformOp>();
    TransformOp* trP = tr.get();
    tr->setBodyId(body);
    tr->setType(TransformType::Translate);
    tr->setTranslation(5.0, 0.0, 0.0);
    ASSERT_TRUE(tr->execute(doc));
    hist.pushExecuted(std::move(tr));

    TopoDS_Edge seam = lineEdgeWhere(doc.getBody(body), [](const gp_Pnt& p) {
        return std::abs(p.X() - 25.0) < 1e-6 && std::abs(p.Y() - 10.0) < 1e-6;
    });
    ASSERT_FALSE(seam.IsNull());
    auto fl = std::make_unique<FilletOp>();
    fl->setBody(body);
    fl->setEdges({seam});
    fl->setRadius(1.5);
    ASSERT_TRUE(fl->execute(doc));
    hist.pushExecuted(std::move(fl));

    trP->setTranslation(10.0, 0.0, 0.0);
    ASSERT_TRUE(hist.editStep(2, doc, /*transactional=*/true))
        << "seam fillet must re-resolve across the moved body";

    // The blend cylinder must sit at the MOVED seam (30, 10).
    bool found = false;
    std::string seen;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_FACE); ex.More();
         ex.Next()) {
        BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
        if (s.GetType() != GeomAbs_Cylinder) continue;
        gp_Pnt loc = s.Cylinder().Axis().Location();
        seen += " (" + std::to_string(loc.X()) + "," +
                std::to_string(loc.Y()) + ")";
        // Concave corner: the blend axis sits r on the OPEN side of the seam
        // in both X and Y - just require it near the corner (r*sqrt2 ~ 2.1),
        // orientation-agnostic.
        if (std::hypot(loc.X() - 30.0, loc.Y() - 10.0) < 3.0) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "blend must land on the seam at its NEW position; "
                          "cylinder axes seen:" << seen;
}
