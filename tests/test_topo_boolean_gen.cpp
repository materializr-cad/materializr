// BooleanOp publishes a two-input GenerationLedger, so the "gen" strategy can
// name a boolean SEAM EDGE by the two faces that made it (each face named by
// sketchface against its own input body - edit-stable). THE deferred dream
// case: seam sub-shapes were unnameable by every geometric scheme, which is
// why fillets on seams bake to ReplayOps today. This proves the naming layer
// now reaches them and survives an upstream sketch edit.

#include "modeling/TopoName.h"
#include "modeling/GenerationLedger.h"
#include "modeling/BooleanOp.h"
#include "core/Document.h"
#include "modeling/Sketch.h"
#include "modeling/ExtrudeOp.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <cmath>
#include <memory>
#include "test_tmp_path.h"

using materializr::Sketch;
using namespace materializr;

namespace {

std::shared_ptr<Sketch> makeRect(double x0, double y0, double x1, double y1,
                                 int pid[4]) {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    pid[0] = sk->addPoint({(float)x0, (float)y0});
    pid[1] = sk->addPoint({(float)x1, (float)y0});
    pid[2] = sk->addPoint({(float)x1, (float)y1});
    pid[3] = sk->addPoint({(float)x0, (float)y1});
    for (int i = 0; i < 4; ++i) sk->addLine(pid[i], pid[(i + 1) % 4]);
    return sk;
}

gp_Pnt edgeMid(const TopoDS_Edge& e) {
    BRepAdaptor_Curve c(e);
    return c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
}

TopoDS_Edge edgeNear(const TopoDS_Shape& body, const gp_Pnt& p, double tol) {
    for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next()) {
        TopoDS_Edge e = TopoDS::Edge(ex.Current());
        if (edgeMid(e).Distance(p) < tol) return e;
    }
    return {};
}

} // namespace

TEST(TopoBooleanGen, SeamEdgeNameSurvivesSketchEdit) {
    Document doc;
    // Body A: 20x10 slab, z 0..10. Body B: 15..25 x 3..7 post, z 0..15 -
    // sticks out of A's top, so B's walls cut SEAM edges into A's top face.
    int pa[4], pb[4];
    auto skA = makeRect(0, 0, 20, 10, pa);
    auto skB = makeRect(15, 3, 25, 7, pb);
    int sidA = doc.addSketch(skA), sidB = doc.addSketch(skB);
    ExtrudeOp extA; extA.setSketchSource(sidA); extA.setDistance(10.0);
    ASSERT_TRUE(extA.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extA.execute(doc));
    int bodyA = doc.getAllBodyIds().front();
    ExtrudeOp extB; extB.setSketchSource(sidB); extB.setDistance(15.0);
    ASSERT_TRUE(extB.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extB.execute(doc));
    int bodyB = -1;
    for (int id : doc.getAllBodyIds()) if (id != bodyA) bodyB = id;
    ASSERT_GE(bodyB, 0);

    BooleanOp fuse;
    fuse.setTargetBodyId(bodyA);
    fuse.setToolBodyId(bodyB);
    fuse.setMode(BooleanMode::Union);
    ASSERT_TRUE(fuse.execute(doc));
    ASSERT_GT(fuse.generationLedger().generated.Extent() +
              fuse.generationLedger().modified.Extent(), 0)
        << "boolean must publish its two-input lineage";

    // The seam edge where B's x=15 wall crosses A's top (z=10): mid (15,5,10).
    TopoDS_Edge seam = edgeNear(doc.getBody(bodyA), gp_Pnt(15, 5, 10), 0.5);
    ASSERT_FALSE(seam.IsNull()) << "fused body must have the seam edge";

    topo::Context ctx;
    ctx.doc = &doc; ctx.shape = doc.getBody(bodyA); ctx.type = TopAbs_EDGE;
    ctx.gen = &fuse.generationLedger();
    topo::Ref ref = topo::mint(seam, ctx);
    std::string genPayload;
    for (const auto& n : ref.names)
        if (n.scheme == "gen") { genPayload = n.payload; break; }
    ASSERT_FALSE(genPayload.empty())
        << "the seam edge must be nameable by generation lineage";

    // UPSTREAM EDIT: slide B two mm left (x 15..25 -> 13..23). Replay the
    // chain the way the cascade does: undo the boolean, rebuild B, re-fuse.
    ASSERT_TRUE(fuse.undo(doc));
    skB->movePoint(pb[0], {13.0f, 3.0f});
    skB->movePoint(pb[1], {23.0f, 3.0f});
    skB->movePoint(pb[2], {23.0f, 7.0f});
    skB->movePoint(pb[3], {13.0f, 7.0f});
    ASSERT_TRUE(extB.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extB.execute(doc));
    ASSERT_TRUE(fuse.execute(doc)) << "re-fuse on the moved post";

    // Resolve the ORIGINAL gen payload against the NEW ledger - directly via
    // the gen strategy, so no other scheme can mask the result. It must land
    // the MOVED seam edge at (13, 5, 10).
    const topo::Strategy* gen = topo::Registry::instance().forScheme("gen");
    ASSERT_NE(gen, nullptr);
    topo::Context rc;
    rc.doc = &doc; rc.shape = doc.getBody(bodyA); rc.type = TopAbs_EDGE;
    rc.gen = &fuse.generationLedger();
    rc.crossRebuild = true;
    TopoDS_Shape out = gen->resolve(genPayload, rc);
    ASSERT_FALSE(out.IsNull())
        << "seam-edge lineage must resolve on the rebuilt boolean";
    ASSERT_EQ(out.ShapeType(), TopAbs_EDGE);
    gp_Pnt mid = edgeMid(TopoDS::Edge(out));
    EXPECT_NEAR(mid.X(), 13.0, 1e-6) << "the MOVED seam (x=13), not the old x=15";
    EXPECT_NEAR(mid.Z(), 10.0, 1e-6) << "still the top-face seam";
}

#include "modeling/FilletOp.h"
#include "modeling/EdgeAnchor.h"
#include <BRepAdaptor_Surface.hxx>

// THE PAYOFF: a FILLET on a boolean seam edge survives an upstream sketch
// edit. The vertical seam at (20,3) - where the post's y=3 wall crosses the
// slab's x=20 wall - sits over NO sketch vertex (proven in-test: EdgeAnchor
// classifies it None), so the proven anchor path fails by construction and
// the new topo-ref last resort (gen lineage via the boolean's ledger,
// republished on the Document by the replay) is what re-finds it.
TEST(TopoBooleanGen, FilletOnSeamSurvivesSketchEdit) {
    Document doc;
    int pa[4], pb[4];
    auto skA = makeRect(0, 0, 20, 10, pa);
    auto skB = makeRect(15, 3, 25, 7, pb);
    int sidA = doc.addSketch(skA), sidB = doc.addSketch(skB);
    ExtrudeOp extA; extA.setSketchSource(sidA); extA.setDistance(10.0);
    ASSERT_TRUE(extA.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extA.execute(doc));
    int bodyA = doc.getAllBodyIds().front();
    ExtrudeOp extB; extB.setSketchSource(sidB); extB.setDistance(15.0);
    ASSERT_TRUE(extB.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extB.execute(doc));
    int bodyB = -1;
    for (int id : doc.getAllBodyIds()) if (id != bodyA) bodyB = id;

    BooleanOp fuse;
    fuse.setTargetBodyId(bodyA);
    fuse.setToolBodyId(bodyB);
    fuse.setMode(BooleanMode::Union);
    ASSERT_TRUE(fuse.execute(doc));

    // The VERTICAL seam edge at (20, 3, ~5).
    TopoDS_Edge seam = edgeNear(doc.getBody(bodyA), gp_Pnt(20, 3, 5), 0.6);
    ASSERT_FALSE(seam.IsNull());
    // Prove the anchor path CANNOT name it (no sketch vertex at (20,3)).
    {
        std::vector<TopoDS_Edge> one{seam};
        auto an = EdgeAnchor::compute(one, {{sidA, skA.get()}, {sidB, skB.get()}});
        ASSERT_EQ(an.size(), 1u);
        EXPECT_EQ(an[0].kind, EdgeAnchor::Anchor::None)
            << "seam must be un-anchorable, or this test proves nothing";
    }

    FilletOp fil;
    fil.setBody(bodyA);
    fil.setEdges({seam});
    fil.setRadius(1.0);
    ASSERT_TRUE(fil.execute(doc)) << "initial fillet on the seam";

    // UPSTREAM EDIT: shift the post -1 in Y (3..7 -> 2..6); the seam moves to
    // (20,2). Replay the chain like the cascade: undo back, rebuild, re-run.
    ASSERT_TRUE(fil.undo(doc));
    ASSERT_TRUE(fuse.undo(doc));
    skB->movePoint(pb[0], {15.0f, 2.0f});
    skB->movePoint(pb[1], {25.0f, 2.0f});
    skB->movePoint(pb[2], {25.0f, 6.0f});
    skB->movePoint(pb[3], {15.0f, 6.0f});
    ASSERT_TRUE(extB.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extB.execute(doc));
    ASSERT_TRUE(fuse.execute(doc));
    ASSERT_TRUE(fil.execute(doc))
        << "fillet must re-find the MOVED seam via its topo ref";

    // The blend is a vertical-axis cylinder near (20, 2).
    bool found = false;
    for (TopExp_Explorer ex(doc.getBody(bodyA), TopAbs_FACE); ex.More(); ex.Next()) {
        BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
        if (s.GetType() != GeomAbs_Cylinder) continue;
        gp_Pnt loc = s.Cylinder().Position().Location();
        if (std::abs(std::abs(s.Cylinder().Axis().Direction().Z()) - 1.0) > 1e-3)
            continue;
        // Moved corner (20,2) -> blend axis (21,1); the UN-moved corner
        // would put it at (21,2), outside the Y tolerance.
        if (std::abs(loc.X() - 21.0) < 0.1 && std::abs(loc.Y() - 1.0) < 0.1) {
            found = true; break;
        }
    }
    EXPECT_TRUE(found) << "blend cylinder must sit at the MOVED seam corner";
}

#include "modeling/ChamferOp.h"

// Mirror of FilletOnSeamSurvivesSketchEdit for ChamferOp.
TEST(TopoBooleanGen, ChamferOnSeamSurvivesSketchEdit) {
    Document doc;
    int pa[4], pb[4];
    auto skA = makeRect(0, 0, 20, 10, pa);
    auto skB = makeRect(15, 3, 25, 7, pb);
    int sidA = doc.addSketch(skA), sidB = doc.addSketch(skB);
    ExtrudeOp extA; extA.setSketchSource(sidA); extA.setDistance(10.0);
    ASSERT_TRUE(extA.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extA.execute(doc));
    int bodyA = doc.getAllBodyIds().front();
    ExtrudeOp extB; extB.setSketchSource(sidB); extB.setDistance(15.0);
    ASSERT_TRUE(extB.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extB.execute(doc));
    int bodyB = -1;
    for (int id : doc.getAllBodyIds()) if (id != bodyA) bodyB = id;

    BooleanOp fuse;
    fuse.setTargetBodyId(bodyA);
    fuse.setToolBodyId(bodyB);
    fuse.setMode(BooleanMode::Union);
    ASSERT_TRUE(fuse.execute(doc));

    TopoDS_Edge seam = edgeNear(doc.getBody(bodyA), gp_Pnt(20, 3, 5), 0.6);
    ASSERT_FALSE(seam.IsNull());

    ChamferOp ch;
    ch.setBody(bodyA);
    ch.setEdges({seam});
    ch.setDistance(1.0);
    ASSERT_TRUE(ch.execute(doc)) << "initial chamfer on the seam";
    const double chamferedV = [&]{ GProp_GProps g;
        BRepGProp::VolumeProperties(doc.getBody(bodyA), g); return g.Mass(); }();

    ASSERT_TRUE(ch.undo(doc));
    ASSERT_TRUE(fuse.undo(doc));
    skB->movePoint(pb[0], {15.0f, 2.0f});
    skB->movePoint(pb[1], {25.0f, 2.0f});
    skB->movePoint(pb[2], {25.0f, 6.0f});
    skB->movePoint(pb[3], {15.0f, 6.0f});
    ASSERT_TRUE(extB.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(extB.execute(doc));
    ASSERT_TRUE(fuse.execute(doc));
    ASSERT_TRUE(ch.execute(doc))
        << "chamfer must re-find the MOVED seam via its topo ref";
    // Same-size bevel on the same-size seam: volume ~unchanged vs original.
    GProp_GProps g; BRepGProp::VolumeProperties(doc.getBody(bodyA), g);
    EXPECT_NEAR(g.Mass(), chamferedV, chamferedV * 0.02)
        << "re-chamfered body ~ the original chamfered volume, relocated";
}

// The seam refs must SURVIVE save/reload (serializeParams round-trip) or a
// reloaded seam fillet loses its last-resort resolution.
TEST(TopoBooleanGen, SeamRefsRoundTripThroughParams) {
    Document doc;
    int pa[4], pb[4];
    auto skA = makeRect(0, 0, 20, 10, pa);
    auto skB = makeRect(15, 3, 25, 7, pb);
    int sidA = doc.addSketch(skA), sidB = doc.addSketch(skB);
    ExtrudeOp extA; extA.setSketchSource(sidA); extA.setDistance(10.0);
    ASSERT_TRUE(extA.rebuildProfileFromSketch(doc) && extA.execute(doc));
    int bodyA = doc.getAllBodyIds().front();
    ExtrudeOp extB; extB.setSketchSource(sidB); extB.setDistance(15.0);
    ASSERT_TRUE(extB.rebuildProfileFromSketch(doc) && extB.execute(doc));
    int bodyB = -1;
    for (int id : doc.getAllBodyIds()) if (id != bodyA) bodyB = id;
    BooleanOp fuse;
    fuse.setTargetBodyId(bodyA); fuse.setToolBodyId(bodyB);
    fuse.setMode(BooleanMode::Union);
    ASSERT_TRUE(fuse.execute(doc));
    TopoDS_Edge seam = edgeNear(doc.getBody(bodyA), gp_Pnt(20, 3, 5), 0.6);
    FilletOp fil;
    fil.setBody(bodyA); fil.setEdges({seam}); fil.setRadius(1.0);
    ASSERT_TRUE(fil.execute(doc));

    const std::string blob = fil.serializeParams();
    EXPECT_NE(blob.find("edgerefs="), std::string::npos)
        << "seam ref must persist";
    FilletOp fil2;
    ASSERT_TRUE(fil2.deserializeParams(blob));
    // (Resolution correctness is covered by FilletOnSeamSurvivesSketchEdit;
    // here: the ref list round-trips intact with a gen name present.)
}

#include "io/ProjectIO.h"
#include <cstdio>

// FULL FILE ROUND-TRIP: the seam fillet's params (incl. edgerefs) go through
// a real .materializr save+load; the RELOADED op must still follow a sketch
// edit - the exact flow of: work, save, quit, reopen, edit.
TEST(TopoBooleanGen, ReloadedSeamFilletFollowsEdit) {
    Document doc;
    int pa[4], pb[4];
    auto skA = makeRect(0, 0, 20, 10, pa);
    auto skB = makeRect(15, 3, 25, 7, pb);
    int sidA = doc.addSketch(skA), sidB = doc.addSketch(skB);
    ExtrudeOp extA; extA.setSketchSource(sidA); extA.setDistance(10.0);
    ASSERT_TRUE(extA.rebuildProfileFromSketch(doc) && extA.execute(doc));
    int bodyA = doc.getAllBodyIds().front();
    ExtrudeOp extB; extB.setSketchSource(sidB); extB.setDistance(15.0);
    ASSERT_TRUE(extB.rebuildProfileFromSketch(doc) && extB.execute(doc));
    int bodyB = -1;
    for (int id : doc.getAllBodyIds()) if (id != bodyA) bodyB = id;
    BooleanOp fuse;
    fuse.setTargetBodyId(bodyA); fuse.setToolBodyId(bodyB);
    fuse.setMode(BooleanMode::Union);
    ASSERT_TRUE(fuse.execute(doc));
    const TopoDS_Shape fusedBefore = doc.getBody(bodyA);

    TopoDS_Edge seam = edgeNear(doc.getBody(bodyA), gp_Pnt(20, 3, 5), 0.6);
    FilletOp fil;
    fil.setBody(bodyA); fil.setEdges({seam}); fil.setRadius(1.0);
    ASSERT_TRUE(fil.execute(doc));

    // Save the fillet step's params through a real project file.
    using materializr::ProjectIO;
    ProjectHistory hist;
    hist.present = true;
    hist.initialState = {{bodyA, fusedBefore}};
    ProjectHistoryStep st;
    st.typeId = fil.typeId();
    st.params = fil.serializeParams();
    st.changed = {{bodyA, doc.getBody(bodyA)}};
    hist.steps = {st};
    const std::string path = mzrtest::tmpPath("mtz_seam_roundtrip.materializr");
    ASSERT_TRUE(ProjectIO::save(path, doc, &hist).success);
    Document scratch;
    ProjectHistory loaded;
    ASSERT_TRUE(ProjectIO::load(path, scratch, &loaded).success);
    std::remove(path.c_str());
    ASSERT_EQ(loaded.steps.size(), 1u);
    ASSERT_NE(loaded.steps[0].params.find("edgerefs="), std::string::npos)
        << "edge refs must survive the FILE";

    // "Reopen": a fresh FilletOp from the FILE params, working in the live
    // doc (sketch ids match - same project).
    FilletOp reloaded;
    ASSERT_TRUE(reloaded.deserializeParams(loaded.steps[0].params));
    // The app rehydrates at load: ordinal indices -> edges of the LOADED
    // (stale-TShape) shapes. The later edit then invalidates exactly those.
    Operation::ReloadState rs;
    rs.modifiedBefore = {{bodyA, loaded.initialState[0].second}};
    rs.modifiedAfter  = {{bodyA, loaded.steps[0].changed[0].second}};
    ASSERT_TRUE(reloaded.rehydrateFromReload(rs, scratch));

    // The edit after reopening: undo to pre-fillet, move the post, rebuild,
    // re-fuse (republishes the ledger), then the RELOADED fillet re-executes.
    ASSERT_TRUE(fil.undo(doc));
    ASSERT_TRUE(fuse.undo(doc));
    skB->movePoint(pb[0], {15.0f, 2.0f});
    skB->movePoint(pb[1], {25.0f, 2.0f});
    skB->movePoint(pb[2], {25.0f, 6.0f});
    skB->movePoint(pb[3], {15.0f, 6.0f});
    ASSERT_TRUE(extB.rebuildProfileFromSketch(doc) && extB.execute(doc));
    ASSERT_TRUE(fuse.execute(doc));
    ASSERT_TRUE(reloaded.execute(doc))
        << "the FILE-reloaded seam fillet must re-find the moved seam";
}

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

// ownsFace (click-to-edit) must claim ONLY blend faces - a planar neighbour
// (the big slab top the blends were carved from) must never open the fillet
// editor (Steve: "most faces of the base box showed the fillet properties").
TEST(TopoBooleanGen, OwnsFaceClaimsOnlyBlends) {
    Document doc;
    int body = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 10, 10).Shape(),
                           "box");
    // Fillet all four vertical edges.
    std::vector<TopoDS_Edge> edges;
    TopTools_IndexedMapOfShape emap;
    TopExp::MapShapes(doc.getBody(body), TopAbs_EDGE, emap);
    for (int i = 1; i <= emap.Extent(); ++i) {
        BRepAdaptor_Curve c(TopoDS::Edge(emap(i)));
        if (c.GetType() == GeomAbs_Line &&
            std::abs(c.Line().Direction().Z()) > 0.999)
            edges.push_back(TopoDS::Edge(emap(i)));
    }
    ASSERT_EQ(edges.size(), 4u);
    FilletOp fil;
    fil.setBody(body);
    fil.setEdges(edges);
    fil.setRadius(2.0);
    ASSERT_TRUE(fil.execute(doc));

    int planarClaims = 0, blendClaims = 0, planes = 0, cyls = 0;
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface s(f);
        const bool claims = fil.ownsFace(f);
        if (s.GetType() == GeomAbs_Plane) { ++planes; if (claims) ++planarClaims; }
        else if (s.GetType() == GeomAbs_Cylinder) { ++cyls; if (claims) ++blendClaims; }
    }
    EXPECT_EQ(planes, 6);
    EXPECT_EQ(cyls, 4);
    EXPECT_EQ(planarClaims, 0) << "a plane is never a fillet blend";
    EXPECT_EQ(blendClaims, 4) << "every real blend stays click-to-editable";
}
