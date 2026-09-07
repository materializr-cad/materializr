// Regression tests for: editing an operation that is UPSTREAM of a boolean /
// delete in a RELOADED project silently did nothing.
//
// Root cause (project-box.materializr, 2026-06): boolean & delete ops carried
// no serialised params, so on reload they came back as baked ReplayOps that
// replay a stale saved shape. Editing a fillet feeding a downstream union thus
// rebuilt the fillet but the baked union overwrote it — the change vanished and
// the "Edit Fillet" affordance disappeared (its regenerated faces matched
// nothing in the stale body). The fix makes BooleanOp/DeleteOp rehydrate as
// real, re-executable ops (and restore consumed bodies under their original id
// via putBody so an editStep replay can re-run them).

#include "core/Document.h"
#include "core/Units.h"
#include "core/History.h"
#include "core/Operation.h"
#include "io/ProjectIO.h"
#include "modeling/FilletOp.h"
#include "modeling/MoveHoleOp.h"
#include "modeling/BooleanOp.h"
#include "modeling/DeleteOp.h"
#include "modeling/TransformOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
#include "modeling/DuplicateSketchOp.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>
#include <gp_Dir.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "test_tmp_path.h"

namespace {

TopoDS_Shape boxAt(double x, double y, double z) {
    return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), 10.0, 10.0, 10.0).Shape();
}

std::vector<TopoDS_Edge> firstEdge(const TopoDS_Shape& s) {
    for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next())
        return { TopoDS::Edge(ex.Current()) };
    return {};
}

double volume(Document& d, int id) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(d.getBody(id), g);
    return g.Mass();
}

double surfaceArea(Document& d, int id) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(d.getBody(id), g);
    return g.Mass();
}

gp_Pnt centreOfMass(Document& d, int id) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(d.getBody(id), g);
    return g.CentreOfMass();
}

} // namespace

// The headline regression: a reloaded Boolean Union downstream of an editable
// fillet must RECOMPUTE when the fillet is edited — not bake over the change.
TEST(ReloadEdit, EditingFilletUpstreamOfReloadedBooleanPropagates) {
    // --- 1. Produce the "saved" geometry by running the real ops once. Two
    //        disjoint boxes; fillet an edge of the tool (B), then union into A.
    Document src;
    int A = src.addBody(boxAt(0, 0, 0), "A");
    int B = src.addBody(boxAt(20, 0, 0), "B");
    const TopoDS_Shape A0 = src.getBody(A);
    const TopoDS_Shape B0 = src.getBody(B);

    FilletOp f0;
    f0.setBody(B);
    f0.setEdges(firstEdge(B0));
    f0.setRadius(1.0);
    ASSERT_TRUE(f0.execute(src));
    const TopoDS_Shape Bf = src.getBody(B);

    BooleanOp b0;
    b0.setTargetBodyId(A);
    b0.setToolBodyId(B);
    b0.setMode(BooleanMode::Union);
    ASSERT_TRUE(b0.execute(src));
    const TopoDS_Shape Uf = src.getBody(A);

    const std::string fParams = f0.serializeParams();
    const std::string bParams = b0.serializeParams();
    ASSERT_FALSE(bParams.empty()) << "BooleanOp must serialise params to reload as a real op";

    // --- 2. Simulate project reload (mirrors Application::rebuildHistoryFromProject):
    //        fresh doc at the FINAL saved state, history rebuilt from real ops.
    Document doc;
    doc.putBody(A, A0, "A");
    doc.putBody(B, B0, "B");
    doc.updateBody(A, Uf);   // final state: A holds the union ...
    doc.removeBody(B);       // ... and B was consumed.

    History H;
    {
        auto f = std::make_unique<FilletOp>();
        ASSERT_TRUE(f->deserializeParams(fParams));
        Operation::ReloadState rs;
        rs.modifiedBefore = {{B, B0}};
        rs.modifiedAfter  = {{B, Bf}};
        ASSERT_TRUE(f->rehydrateFromReload(rs, doc));
        H.pushExecuted(std::move(f));
    }
    {
        auto b = std::make_unique<BooleanOp>();
        ASSERT_TRUE(b->deserializeParams(bParams));
        EXPECT_EQ(b->getTargetBodyId(), A);
        EXPECT_EQ(b->getToolBodyId(), B);
        EXPECT_EQ(b->getMode(), BooleanMode::Union);
        Operation::ReloadState rs;
        rs.modifiedBefore = {{A, A0}};
        rs.modifiedAfter  = {{A, Uf}};
        rs.deletedBefore  = {{B, Bf}};
        // The crux: boolean must rehydrate as a REAL op (true), not bake.
        ASSERT_TRUE(b->rehydrateFromReload(rs, doc));
        H.pushExecuted(std::move(b));
    }

    const double vBefore = volume(doc, A);

    // --- 3. Edit the fillet to a larger radius and replay the history.
    auto* f = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(H.getStep(0)));
    ASSERT_NE(f, nullptr) << "fillet step must reload as a real, editable FilletOp";
    f->setRadius(3.0);
    ASSERT_TRUE(H.editStep(0, doc))
        << "editStep must succeed — re-running the fillet and the downstream union";

    const double vAfter = volume(doc, A);

    // A larger fillet removes more material from B, so the union volume must
    // shrink. If the boolean had baked (the bug), vAfter would equal vBefore.
    EXPECT_LT(vAfter, vBefore - 1.0)
        << "the fillet edit did not propagate through the reloaded boolean "
           "(vBefore=" << vBefore << " vAfter=" << vAfter << ")";
}

// REAL file round-trip: a fillet saved to a .materializr file and re-loaded must
// come back EDITABLE. The other ReloadEdit tests simulate reload in memory (they
// hand-build the params + reload shapes), so they can't see a BREP edge-ordering
// change across the file write/read that would make the saved edge index resolve
// to the WRONG edge — which would silently un-edit every reloaded fillet. This
// test actually writes the file and reads it back.
TEST(ReloadEdit, FilletSurvivesRealFileRoundTrip) {
    // 1. Box + fillet, run for real.
    Document src;
    int B = src.addBody(boxAt(0, 0, 0), "B");
    const TopoDS_Shape B0 = src.getBody(B);
    FilletOp f0;
    f0.setBody(B);
    f0.setEdges(firstEdge(B0));
    f0.setRadius(1.0);
    ASSERT_TRUE(f0.execute(src));
    const TopoDS_Shape Bf = src.getBody(B);
    const double vSaved = volume(src, B);

    // 2. Build the savable ProjectHistory (mirrors Application's save loop).
    using materializr::ProjectHistory;
    using materializr::ProjectHistoryStep;
    using materializr::ProjectIO;
    ProjectHistory hist;
    hist.present = true;
    hist.initialState = {{B, B0}};
    ProjectHistoryStep st;
    st.typeId      = f0.typeId();          // "fillet"
    st.name        = f0.name();
    st.description = f0.description();
    st.params      = f0.serializeParams(); // radius + edge index vs B0
    st.changed     = {{B, Bf}};
    hist.steps     = {st};
    ASSERT_FALSE(st.params.empty()) << "fillet must serialise params to save";

    // 3. Save to a temp file, then load it back into a fresh doc/history.
    const std::string path = mzrtest::tmpPath("mtz_fillet_roundtrip.materializr");
    ASSERT_TRUE(ProjectIO::save(path, src, &hist).success);

    Document doc;
    ProjectHistory loaded;
    ASSERT_TRUE(ProjectIO::load(path, doc, &loaded).success);
    std::remove(path.c_str());

    // 4. The fillet step + its params must have survived the FILE.
    ASSERT_EQ(loaded.steps.size(), 1u);
    EXPECT_EQ(loaded.steps[0].typeId, "fillet");
    ASSERT_FALSE(loaded.steps[0].params.empty())
        << "fillet params lost in the file round-trip";
    ASSERT_EQ(loaded.initialState.size(), 1u);
    const TopoDS_Shape B0r = loaded.initialState[0].second;        // box, from file
    ASSERT_EQ(loaded.steps[0].changed.size(), 1u);
    const TopoDS_Shape Bfr = loaded.steps[0].changed[0].second;    // filleted, from file

    // 5. Rehydrate the fillet from the LOADED params + LOADED shapes — this is
    //    exactly where an edge-index mismatch after BREP read would fail.
    auto op = std::make_unique<FilletOp>();
    ASSERT_TRUE(op->deserializeParams(loaded.steps[0].params));
    Operation::ReloadState rs;
    rs.modifiedBefore = {{B, B0r}};
    rs.modifiedAfter  = {{B, Bfr}};
    ASSERT_TRUE(op->rehydrateFromReload(rs, doc))
        << "reloaded fillet failed to rehydrate — edge identity lost across save";

    // 6. Edit flow mirrors History::editStep: roll the body back to its
    //    pre-fillet state, then re-run. With the SAME radius it must rebuild the
    //    ORIGINAL filleted body — proving the saved index resolved the RIGHT edge
    //    from the file, not just any edge. (execute() reads doc.getBody, so the
    //    rollback is required; without it the sharp edge is already consumed.)
    doc.updateBody(B, B0r);
    ASSERT_TRUE(op->execute(doc))
        << "reloaded fillet couldn't re-execute against its pre-fillet body";
    EXPECT_NEAR(volume(doc, B), vSaved, 1e-6)
        << "reloaded fillet rebuilt a DIFFERENT shape — wrong edge resolved from file";

    // And it's genuinely editable: a bigger radius removes more material.
    doc.updateBody(B, B0r);
    op->setRadius(3.0);
    ASSERT_TRUE(op->execute(doc)) << "reloaded fillet couldn't re-execute at a new radius";
    EXPECT_LT(volume(doc, B), vSaved - 1e-3)
        << "editing the reloaded fillet's radius had no effect (wrong/lost edge)";
}

// REAL file round-trip for a MOVE-HOLE: it must reload as a real, editable op
// (seed wall resolved from the file) instead of baked geometry. Move-hole used
// to have no reload support at all, so every move-hole baked on reload — which
// also tripped the "frozen feature" warning on brand-new projects.
TEST(ReloadEdit, MoveHoleSurvivesRealFileRoundTrip) {
    using materializr::ProjectHistory;
    using materializr::ProjectHistoryStep;
    using materializr::ProjectIO;

    // 1. Box with a centred through-hole, then slide the hole +X for real.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20.0, 20.0, 10.0).Shape();
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(10, 10, -1), gp_Dir(0, 0, 1)), 2.0, 12.0).Shape();
    TopoDS_Shape holed = BRepAlgoAPI_Cut(box, cyl).Shape();

    Document src;
    int B = src.addBody(holed, "B");
    const TopoDS_Shape B0 = src.getBody(B);

    TopoDS_Face seed; // the hole's cylindrical wall
    for (TopExp_Explorer ex(B0, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        Handle(Geom_Surface) s = BRep_Tool::Surface(f);
        if (!s.IsNull() && s->IsKind(STANDARD_TYPE(Geom_CylindricalSurface))) {
            seed = f; break;
        }
    }
    ASSERT_FALSE(seed.IsNull()) << "need the hole's cylindrical wall as the seed";

    MoveHoleOp mh;
    mh.setBody(B);
    mh.setSeedWall(seed);
    mh.setMoveVector(gp_Vec(5.0, 0.0, 0.0));
    ASSERT_TRUE(mh.execute(src));
    const TopoDS_Shape Bmoved = src.getBody(B);
    const gp_Pnt cmSaved = centreOfMass(src, B); // CoM shifts as the hole moves off-centre

    // 2. Build the savable history, save to a real file, load it back.
    ProjectHistory hist;
    hist.present = true;
    hist.initialState = {{B, B0}};
    ProjectHistoryStep st;
    st.typeId      = mh.typeId();          // "move_hole"
    st.name        = mh.name();
    st.description = mh.description();
    st.params      = mh.serializeParams(); // body + move vector + seed-wall index
    st.changed     = {{B, Bmoved}};
    hist.steps     = {st};
    ASSERT_FALSE(st.params.empty()) << "move-hole must serialise params to save";

    const std::string path = mzrtest::tmpPath("mtz_movehole_roundtrip.materializr");
    ASSERT_TRUE(ProjectIO::save(path, src, &hist).success);
    Document doc;
    ProjectHistory loaded;
    ASSERT_TRUE(ProjectIO::load(path, doc, &loaded).success);
    std::remove(path.c_str());

    ASSERT_EQ(loaded.steps.size(), 1u);
    EXPECT_EQ(loaded.steps[0].typeId, "move_hole");
    ASSERT_FALSE(loaded.steps[0].params.empty())
        << "move-hole params lost in the file round-trip";
    const TopoDS_Shape B0r = loaded.initialState[0].second;

    // 3. Rehydrate from the LOADED params + shapes — the seed-wall index must
    //    resolve against the box read back from BREP.
    auto op = std::make_unique<MoveHoleOp>();
    ASSERT_TRUE(op->deserializeParams(loaded.steps[0].params));
    Operation::ReloadState rs;
    rs.modifiedBefore = {{B, B0r}};
    rs.modifiedAfter  = {{B, loaded.steps[0].changed[0].second}};
    ASSERT_TRUE(op->rehydrateFromReload(rs, doc))
        << "reloaded move-hole failed to rehydrate — seed wall lost across save";

    // 4. Re-run against the rolled-back body (what editStep does); the hole must
    //    land in the SAME place — proving the seed wall + vector survived.
    doc.updateBody(B, B0r);
    ASSERT_TRUE(op->execute(doc)) << "reloaded move-hole couldn't re-execute";
    EXPECT_LT(centreOfMass(doc, B).Distance(cmSaved), 1e-6)
        << "reloaded move-hole landed the hole in a different place";
}

// Free-space sketch push/pull with cut-intersecting: subtracts from VISIBLE
// intersecting bodies (separately), skips hidden ones, and falls back to a new
// body when it hits nothing. (The "free-space sketch that runs into a body
// cuts it" feature.)
namespace {
PushPullOp makeCutOp(const TopoDS_Face& profile) {
    PushPullOp op;
    PushPullOp::Target t; t.profile = profile; t.sourceBodyId = -1;
    op.setTargets({t});
    op.setDistance(15.0);
    op.setSymmetric(true);          // sweep both ways → guaranteed to reach
    op.setCutIntersecting(true);
    return op;
}
TopoDS_Face xyProfile(double x0, double x1, double y0, double y1) {
    return BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0,0,0), gp_Dir(0,0,1)),
                                   x0, x1, y0, y1).Face();
}
} // namespace

TEST(SmartCut, IntersectingVisibleBodyIsCutNotNewBody) {
    Document d;
    int b = d.addBody(boxAt(0,0,0), "box");
    const double v0 = volume(d, b);
    auto op = makeCutOp(xyProfile(4,6,4,6));   // small column through the box
    ASSERT_TRUE(op.execute(d));
    EXPECT_EQ(d.getAllBodyIds().size(), 1u) << "should NOT create a new body";
    EXPECT_LT(volume(d, b), v0 - 1.0) << "box should lose the cut volume";
}

TEST(SmartCut, NonIntersectingFallsBackToNewBody) {
    Document d;
    int b = d.addBody(boxAt(0,0,0), "box");
    const double v0 = volume(d, b);
    auto op = makeCutOp(xyProfile(100,102,100,102)); // far away
    ASSERT_TRUE(op.execute(d));
    EXPECT_EQ(d.getAllBodyIds().size(), 2u) << "should create a new body";
    EXPECT_DOUBLE_EQ(volume(d, b), v0) << "the box must be untouched";
}

TEST(SmartCut, HiddenBodiesAreSkipped) {
    Document d;
    int b = d.addBody(boxAt(0,0,0), "box");
    d.setBodyVisible(b, false);
    const double v0 = volume(d, b);
    auto op = makeCutOp(xyProfile(4,6,4,6));
    ASSERT_TRUE(op.execute(d));
    EXPECT_DOUBLE_EQ(volume(d, b), v0) << "a hidden body must not be cut";
    EXPECT_EQ(d.getAllBodyIds().size(), 2u) << "falls back to a new body";
}

TEST(SmartCut, TwoBodiesCutSeparately) {
    Document d;
    int b1 = d.addBody(boxAt(0,0,0), "b1");
    int b2 = d.addBody(boxAt(20,0,0), "b2");
    const double v1 = volume(d, b1), v2 = volume(d, b2);
    auto op = makeCutOp(xyProfile(-1,31,4,6));  // slab spanning both
    ASSERT_TRUE(op.execute(d));
    EXPECT_EQ(d.getAllBodyIds().size(), 2u) << "both bodies stay separate";
    EXPECT_LT(volume(d, b1), v1 - 1.0);
    EXPECT_LT(volume(d, b2), v2 - 1.0);
}

// A cut whose sketch IS attached to a body (sourceBodyId set) must still cut
// THROUGH to the other bodies in its path — the original bug report: a sketch
// drawn on the lid, cut downward, only cut the lid and ignored the box.
TEST(SmartCut, AttachedCutGoesThroughOtherBodies) {
    Document d;
    int lid = d.addBody(boxAt(0,0,0), "lid");   // the source body
    int box = d.addBody(boxAt(20,0,0), "box");  // another body in the path
    const double vl = volume(d, lid), vb = volume(d, box);
    PushPullOp op;
    PushPullOp::Target t;
    t.profile = xyProfile(-1,31,4,6);
    t.sourceBodyId = lid;                       // sketch attached to the lid
    op.setTargets({t});
    op.setDistance(-15.0);                       // cut direction
    op.setSymmetric(true);
    op.setCutIntersecting(true);
    ASSERT_TRUE(op.execute(d));
    EXPECT_EQ(d.getAllBodyIds().size(), 2u);
    EXPECT_LT(volume(d, lid), vl - 1.0) << "the source (lid) is cut";
    EXPECT_LT(volume(d, box), vb - 1.0) << "the other body in the path is also cut";
}

// BooleanOp params round-trip cleanly (target/tool/mode), for each mode.
TEST(ReloadEdit, BooleanParamsRoundTrip) {
    for (BooleanMode m : {BooleanMode::Union, BooleanMode::Subtract, BooleanMode::Intersect}) {
        BooleanOp a;
        a.setTargetBodyId(7);
        a.setToolBodyId(12);
        a.setMode(m);
        BooleanOp b;
        ASSERT_TRUE(b.deserializeParams(a.serializeParams()));
        EXPECT_EQ(b.getTargetBodyId(), 7);
        EXPECT_EQ(b.getToolBodyId(), 12);
        EXPECT_EQ(b.getMode(), m);
    }
}

// Editing a fillet UPSTREAM of a reloaded transform must propagate — the
// transform re-applies to the edited live body instead of baking its stale
// result over the change.
TEST(ReloadEdit, EditUpstreamOfReloadedTransformPropagates) {
    Document src;
    int B = src.addBody(boxAt(0, 0, 0), "B");
    const TopoDS_Shape B0 = src.getBody(B);

    FilletOp f0;
    f0.setBody(B);
    f0.setEdges(firstEdge(B0));
    f0.setRadius(1.0);
    ASSERT_TRUE(f0.execute(src));
    const TopoDS_Shape Bf = src.getBody(B);

    TransformOp t0;
    t0.setBodyId(B);
    t0.setType(TransformType::Translate);
    t0.setTranslation(50.0, 0.0, 0.0);
    ASSERT_TRUE(t0.execute(src));
    const TopoDS_Shape Bt = src.getBody(B);

    const std::string fParams = f0.serializeParams();
    const std::string tParams = t0.serializeParams();
    ASSERT_FALSE(tParams.empty()) << "TransformOp must serialise params";

    // Reload: fresh doc at the final (translated) state, history rebuilt.
    Document doc;
    doc.putBody(B, B0, "B");
    doc.updateBody(B, Bt);
    History H;
    {
        auto f = std::make_unique<FilletOp>();
        ASSERT_TRUE(f->deserializeParams(fParams));
        Operation::ReloadState rs;
        rs.modifiedBefore = {{B, B0}}; rs.modifiedAfter = {{B, Bf}};
        ASSERT_TRUE(f->rehydrateFromReload(rs, doc));
        H.pushExecuted(std::move(f));
    }
    {
        auto t = std::make_unique<TransformOp>();
        ASSERT_TRUE(t->deserializeParams(tParams));
        Operation::ReloadState rs;
        rs.modifiedBefore = {{B, Bf}}; rs.modifiedAfter = {{B, Bt}};
        ASSERT_TRUE(t->rehydrateFromReload(rs, doc));
        H.pushExecuted(std::move(t));
    }

    const double aBefore = surfaceArea(doc, B);
    auto* f = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(H.getStep(0)));
    ASSERT_NE(f, nullptr);
    f->setRadius(3.0);
    ASSERT_TRUE(H.editStep(0, doc));
    const double aAfter = surfaceArea(doc, B);
    EXPECT_GT(std::abs(aAfter - aBefore), 1.0)
        << "fillet edit did not propagate through the reloaded transform";
}

// Legacy transforms (no params blob) reconstruct their rigid transform from the
// step's before/after snapshots.
TEST(ReloadEdit, RigidTrsfReconstruction) {
    TopoDS_Shape before = boxAt(0, 0, 0);
    gp_Trsf t; t.SetTranslation(gp_Vec(7.0, -3.0, 11.0));
    BRepBuilderAPI_Transform xf(before, t, true);
    xf.Build();
    ASSERT_TRUE(xf.IsDone());
    TopoDS_Shape after = xf.Shape();

    gp_Trsf recovered;
    ASSERT_TRUE(TransformOp::rigidTrsfBetween(before, after, recovered));
    // A probe point on `before` maps to the same place under the recovered trsf.
    gp_Pnt p(2.0, 5.0, 1.0);
    EXPECT_LT(p.Transformed(t).Distance(p.Transformed(recovered)), 1e-6);
}

// Sketch-edit history descriptions name the geometry and its measured size,
// instead of a generic "Add sketch element".
TEST(SketchHistory, MeaningfulDescriptions) {
    using materializr::Sketch;
    using materializr::SketchEditOp;
    auto desc = [](std::function<void(Sketch&)> build) {
        auto before = std::make_shared<Sketch>();
        auto after  = std::make_shared<Sketch>();
        build(*after);
        return SketchEditOp(after, before, after).description();
    };
    // Every quantity carries its own unit suffix (fmtLength), so a caption reads
    // correctly whatever display unit is active — it is formatted at render
    // time from mm, never stored. Expectations are built the same way rather
    // than spelled out, so they hold in any unit.
    using materializr::fmtLength;
    EXPECT_EQ(desc([](Sketch& s){
        int p0 = s.addPoint({0,0}),  p1 = s.addPoint({80,0});
        int p2 = s.addPoint({80,45}), p3 = s.addPoint({0,45});
        s.addLine(p0,p1); s.addLine(p1,p2); s.addLine(p2,p3); s.addLine(p3,p0);
    }), "Rectangle " + fmtLength(80) + " \xC3\x97 " + fmtLength(45));
    EXPECT_EQ(desc([](Sketch& s){ int c = s.addPoint({5,5}); s.addCircle(c, 10.0); }),
              "Circle \xC3\x98" + fmtLength(20));
    EXPECT_EQ(desc([](Sketch& s){ int a = s.addPoint({0,0}), b = s.addPoint({30,40});
                                  s.addLine(a,b); }), "Line " + fmtLength(50));
    EXPECT_EQ(desc([](Sketch& s){ int c = s.addPoint({0,0}), a = s.addPoint({8,0}),
                                  b = s.addPoint({0,8}); s.addArc(c,a,b,8.0); }),
              "Arc R" + fmtLength(8));

    // And the same caption re-renders in the new unit after a switch — nothing
    // is cached in mm text.
    materializr::setCurrentUnit(materializr::LengthUnit::In);
    EXPECT_EQ(desc([](Sketch& s){ int a = s.addPoint({0,0}), b = s.addPoint({25.4f,0});
                                  s.addLine(a,b); }), "Line 1.000 in");
    materializr::setCurrentUnit(materializr::LengthUnit::Mm);
}

// A transactional editStep whose replay fails must restore the model to its
// last-good state — never leave a half-built body. (The fix for "editing a
// circle dropped my hollow and fillets and left a broken cube".)
TEST(EditStep, TransactionalRevertOnFailure) {
    Document d;
    int b = d.addBody(boxAt(0,0,0), "box");
    History H;
    auto f = std::make_unique<FilletOp>();
    f->setBody(b);
    f->setEdges(firstEdge(d.getBody(b)));
    f->setRadius(1.0);
    ASSERT_TRUE(H.pushOperation(std::move(f), d));   // valid filleted body
    const double vGood = volume(d, b);

    // Edit the fillet to a radius the box can't carry, then replay transactionally.
    auto* fop = const_cast<FilletOp*>(dynamic_cast<const FilletOp*>(H.getStep(0)));
    fop->setRadius(1000.0);
    bool ok = H.editStep(0, d, /*transactional=*/true);
    EXPECT_FALSE(ok) << "an impossible fillet radius must fail the replay";
    EXPECT_NEAR(volume(d, b), vGood, 1e-6)
        << "model restored to the last-good filleted state, not left un-filleted/broken";
    EXPECT_EQ(d.getAllBodyIds().size(), 1u);
}

// A circle added without a constraint is diameter-editable from history: the
// panel writes the new radius to the after-snapshot, and execute() pushes the
// resized sketch onto the live one (centre unchanged).
TEST(SketchHistory, CircleDiameterEditableFromHistory) {
    using materializr::Sketch;
    using materializr::SketchEditOp;
    auto before = std::make_shared<Sketch>();
    auto after  = std::make_shared<Sketch>();
    int ctr = after->addPoint({3, 4});
    int cid = after->addCircle(ctr, 10.0);     // Ø20
    auto live = std::make_shared<Sketch>();
    *live = *after;                             // edit already applied to live

    SketchEditOp op(live, before, after);
    // Panel edit → writes the new radius into the after-snapshot.
    after->setCircleRadius(cid, 7.5);          // Ø15
    Document doc;
    ASSERT_TRUE(op.execute(doc));               // Apply Changes

    double r = -1; glm::vec2 c{};
    for (const auto& cc : live->getCircles()) if (cc.id == cid) r = cc.radius;
    for (const auto& p : live->getPoints())   if (p.id == ctr) c = p.pos;
    EXPECT_NEAR(r, 7.5, 1e-9) << "live circle resized to the new radius";
    EXPECT_NEAR(c.x, 3.0, 1e-9); EXPECT_NEAR(c.y, 4.0, 1e-9) << "centre unchanged";
}

// A sketch is stored as a chain of FULL snapshots (one per sketchedit step).
// Editing a circle's diameter at the step that introduced it must carry forward
// into every later snapshot of the same sketch — otherwise the next step's
// snapshot overwrites the change on replay before any extrude/pushpull reads it,
// and the edit silently "applies" with no visible effect. (The fix for
// "circle edit applied but the hole didn't change size".)
TEST(SketchHistory, CircleDiameterEditPropagatesThroughLaterSnapshots) {
    using materializr::Sketch;
    using materializr::SketchEditOp;
    Document doc;

    auto live = std::make_shared<Sketch>();
    doc.addSketch(live, "Sketch 1");

    // step 0: add the circle (Ø20). step 1: add a line — its full snapshot
    // still carries the circle, at the ORIGINAL radius.
    auto before0 = std::make_shared<Sketch>();             // empty
    auto after0  = std::make_shared<Sketch>();
    int ctr = after0->addPoint({0, 0});
    int cid = after0->addCircle(ctr, 10.0);                // Ø20

    auto before1 = std::make_shared<Sketch>(*after0);      // carries the circle
    auto after1  = std::make_shared<Sketch>(*after0);
    int a = after1->addPoint({0, 0}), b = after1->addPoint({50, 0});
    after1->addLine(a, b);

    History H;
    *live = *after0;
    H.pushExecuted(std::make_unique<SketchEditOp>(live, before0, after0));
    *live = *after1;
    H.pushExecuted(std::make_unique<SketchEditOp>(live, before1, after1));

    // Edit the diameter at the introducing step (Ø20 -> Ø15).
    auto* op0 = const_cast<SketchEditOp*>(
        dynamic_cast<const SketchEditOp*>(H.getStep(0)));
    ASSERT_NE(op0, nullptr);
    op0->editCircleRadius(cid, 7.5);                       // Ø15

    H.propagateSketchValueEdits(0, doc);
    ASSERT_TRUE(H.editStep(0, doc, /*transactional=*/true));

    // After replaying BOTH snapshots, the live sketch must carry the new radius.
    double r = -1;
    for (const auto& c : live->getCircles()) if (c.id == cid) r = c.radius;
    EXPECT_NEAR(r, 7.5, 1e-9)
        << "later full-snapshot step clobbered the diameter edit";

    // And the later snapshot itself was rewritten, so save/reload + undo keep it.
    double rSnap = -1;
    for (const auto& c : after1->getCircles()) if (c.id == cid) rSnap = c.radius;
    EXPECT_NEAR(rSnap, 7.5, 1e-9) << "downstream snapshot not updated in place";
}

// Duplicating a sketch produces an INDEPENDENT copy: editing the copy must not
// touch the original (the box/lid use case — same layout, different hole sizes).
// Undo removes the copy cleanly; redo brings it back with its geometry intact.
TEST(SketchHistory, DuplicateSketchIsIndependentAndUndoable) {
    using materializr::Sketch;

    Document doc;
    auto src = std::make_shared<Sketch>();
    int ctr = src->addPoint({0, 0});
    int cid = src->addCircle(ctr, 2.5);                 // Ø5 (heat-set)
    int srcId = doc.addSketch(src, "Sketch 1");

    auto copy = std::make_shared<Sketch>(*src);
    auto op = std::make_unique<DuplicateSketchOp>();
    op->setCopy(copy, srcId, "Sketch 1 copy");
    DuplicateSketchOp* raw = op.get();
    ASSERT_TRUE(op->execute(doc));
    int newId = raw->newSketchId();
    History H;
    H.pushExecuted(std::move(op));

    ASSERT_GE(newId, 0);
    ASSERT_NE(newId, srcId) << "copy must get its own id";
    ASSERT_NE(doc.getSketch(newId), nullptr);

    // Shrink the COPY's hole to screw size; the original must stay Ø5.
    doc.getSketch(newId)->setCircleRadius(cid, 1.5);    // Ø3 (screw)
    double rSrc = -1, rCopy = -1;
    for (const auto& c : doc.getSketch(srcId)->getCircles()) if (c.id == cid) rSrc = c.radius;
    for (const auto& c : doc.getSketch(newId)->getCircles()) if (c.id == cid) rCopy = c.radius;
    EXPECT_NEAR(rSrc, 2.5, 1e-9) << "editing the copy must not change the original";
    EXPECT_NEAR(rCopy, 1.5, 1e-9);

    // Undo removes the copy entirely (no stranded empty husk).
    ASSERT_TRUE(H.undo(doc));
    EXPECT_EQ(doc.getSketch(newId), nullptr) << "undo must remove the duplicate";
    EXPECT_NE(doc.getSketch(srcId), nullptr) << "original survives";

    // Redo brings it back. (Its geometry is whatever the copy held when undone;
    // here we just confirm the sketch reappears under the same id.)
    ASSERT_TRUE(H.redo(doc));
    EXPECT_NE(doc.getSketch(newId), nullptr) << "redo restores the duplicate";
}

// DeleteOp reloads as a real op: params round-trip and rehydrate recovers the
// deleted shape so an editStep replay can roll it back.
TEST(ReloadEdit, DeleteRehydrates) {
    Document src;
    int id = src.addBody(boxAt(0, 0, 0), "victim");
    const TopoDS_Shape shape = src.getBody(id);

    DeleteOp d0;
    d0.setBodyId(id);
    ASSERT_TRUE(d0.execute(src));
    const std::string params = d0.serializeParams();
    ASSERT_FALSE(params.empty());

    DeleteOp d1;
    ASSERT_TRUE(d1.deserializeParams(params));
    EXPECT_EQ(d1.getBodyId(), id);

    Document doc;  // freshly reloaded: the body is already gone
    Operation::ReloadState rs;
    rs.deletedBefore = {{id, shape}};
    ASSERT_TRUE(d1.rehydrateFromReload(rs, doc));

    // undo() restores the body under its ORIGINAL id (so upstream refs resolve).
    ASSERT_TRUE(d1.undo(doc));
    EXPECT_NO_THROW(doc.getBody(id));
}
