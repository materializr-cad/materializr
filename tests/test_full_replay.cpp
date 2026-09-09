// FULL HISTORY REPLAY - the contract Steve asked for: a saved project's every
// step reloads as a REAL, editable, re-executable operation (zero frozen
// ReplayOps), and the whole chain replays from step 0 - including after an
// upstream sketch edit. The chain below exercises every op type that ships in
// a project file's history alongside the ones with existing coverage:
//   extrude, sketchedit, fillet, shell, thread, copy, align, mirror,
//   split_body, transform, construction_plane, plane_transform,
//   construction_axis, axis_transform, sketchtransform, loft, sweep, delete, and a face-driven extrude.
// The save/load goes through the real ProjectIO; the reload loop mirrors
// Application's (OperationFactory create -> deserializeParams ->
// rehydrateFromReload with the step's before/after diff).

#include "core/Document.h"
#include "core/History.h"
#include "io/ProjectIO.h"
#include "modeling/OperationFactory.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/FilletOp.h"
#include "modeling/ShellOp.h"
#include "modeling/ThreadOp.h"
#include "modeling/CopyOp.h"
#include "modeling/AlignOp.h"
#include "modeling/MirrorOp.h"
#include "modeling/SplitBodyOp.h"
#include "modeling/TransformOp.h"
#include "modeling/ConstructionPlaneOp.h"
#include "modeling/ConstructionAxisOp.h"
#include "modeling/PlaneTransformOp.h"
#include "modeling/AxisTransformOp.h"
#include "modeling/SketchTransformOp.h"
#include "modeling/LoftOp.h"
#include "modeling/SweepOp.h"
#include "modeling/DeleteOp.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <cstdio>
#include <map>
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

TopoDS_Face topFaceOf(const TopoDS_Shape& body) {
    TopoDS_Face best;
    double bestZ = -1e18;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        GProp_GProps g;
        BRepGProp::SurfaceProperties(f, g);
        if (g.CentreOfMass().Z() > bestZ) { bestZ = g.CentreOfMass().Z(); best = f; }
    }
    return best;
}

// Mirror of Application::captureProjectHistory's history walk (headless: no
// previews or thread recuts to drain).
ProjectHistory captureHistory(History& hist, Document& doc) {
    ProjectHistory h;
    h.present = true;
    const int n = hist.currentStep() + 1;
    std::map<int, TopoDS_Shape> cur;
    for (int id : doc.getAllBodyIds()) cur[id] = doc.getBody(id);
    std::vector<ProjectHistoryStep> steps(n);
    for (int i = n - 1; i >= 0; --i) {
        const Operation* op = hist.getStep(i);
        if (!op) continue;
        steps[i].typeId = op->typeId();
        steps[i].name = op->name();
        steps[i].description = op->description();
        steps[i].enabled = op->isEnabled();
        if (auto* sk = dynamic_cast<const materializr::SketchEditOp*>(op))
            steps[i].params = sk->serializeWithDocument(doc);
        else
            steps[i].params = op->serializeParams();
        if (!op->isEnabled()) continue;
        OperationDiff d = op->captureDiff();
        for (const auto& [id, before] : d.modifiedBefore) {
            auto it = cur.find(id);
            if (it != cur.end())
                steps[i].changed.push_back(std::make_pair(id, it->second));
            cur[id] = before;
        }
        for (int id : d.created) {
            auto it = cur.find(id);
            if (it != cur.end())
                steps[i].changed.push_back(std::make_pair(id, it->second));
            cur.erase(id);
        }
        for (const auto& [id, before] : d.deletedBefore) {
            steps[i].deleted.push_back(id);
            cur[id] = before;
        }
    }
    for (const auto& [id, shp] : cur)
        h.initialState.push_back(std::make_pair(id, shp));
    h.steps = std::move(steps);
    return h;
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

} // namespace

TEST(FullReplay, EveryOpReloadsEditableAndChainReplays) {
    Document doc;
    History hist;
    auto push = [&](std::unique_ptr<Operation> op, const char* what) {
        ASSERT_TRUE(hist.pushOperation(std::move(op), doc)) << what;
    };

    // 1. extrude - base slab from a sketch.
    int pa[4];
    auto skA = makeRect(0, 0, 30, 20, pa);
    int sidA = doc.addSketch(skA);
    {
        auto ext = std::make_unique<ExtrudeOp>();
        ext->setSketchSource(sidA);
        ext->setDistance(10.0);
        ASSERT_TRUE(ext->rebuildProfileFromSketch(doc));
        push(std::move(ext), "extrude A");
    }
    const int bodyA = doc.getAllBodyIds().front();

    // 2. sketchedit - widen the slab 30 -> 34 and cascade the extrude.
    {
        auto before = std::make_shared<Sketch>(*skA);
        skA->movePoint(pa[1], {34.0f, 0.0f});
        skA->movePoint(pa[2], {34.0f, 20.0f});
        auto after = std::make_shared<Sketch>(*skA);
        hist.pushExecuted(std::make_unique<materializr::SketchEditOp>(skA, before, after));
        auto* ext = dynamic_cast<ExtrudeOp*>(
            const_cast<Operation*>(hist.getStep(0)));
        ASSERT_NE(ext, nullptr);
        ASSERT_TRUE(ext->rebuildProfileFromSketch(doc));
        ASSERT_TRUE(hist.editStep(0, doc, /*transactional=*/true));
    }

    // 3. fillet - a vertical corner of the slab.
    {
        auto fil = std::make_unique<FilletOp>();
        fil->setBody(bodyA);
        fil->setEdges({verticalEdgeAt(doc.getBody(bodyA), 34.0, 20.0)});
        fil->setRadius(2.0);
        push(std::move(fil), "fillet A");
    }

    // 4. shell - hollow the slab, top open.
    {
        auto sh = std::make_unique<ShellOp>();
        sh->setBody(bodyA);
        sh->setThickness(1.5);
        sh->addFaceToRemove(topFaceOf(doc.getBody(bodyA)));
        push(std::move(sh), "shell A");
    }

    // 5. extrude - a cylinder (thread target), well away from the slab.
    auto skB = std::make_shared<Sketch>();
    skB->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    int cB = skB->addPoint({80.0f, 10.0f});
    skB->addCircle(cB, 6.0);
    int sidB = doc.addSketch(skB);
    int bodyB = -1;
    {
        auto ext = std::make_unique<ExtrudeOp>();
        ext->setSketchSource(sidB);
        ext->setDistance(12.0);
        ASSERT_TRUE(ext->rebuildProfileFromSketch(doc));
        push(std::move(ext), "extrude B");
        for (int id : doc.getAllBodyIds())
            if (id != bodyA) bodyB = id;
        ASSERT_GE(bodyB, 0);
    }

    // 6. thread - on the cylinder (swept fast path).
    {
        auto th = std::make_unique<ThreadOp>();
        th->setBody(bodyB);
        th->setAxis(gp_Ax2(gp_Pnt(80, 10, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
        th->setRadius(6.0);
        th->setLength(12.0);
        th->setPitch(2.0);
        th->setDepth(0.8);
        th->setIsHole(false);
        th->setRightHanded(true);
        push(std::move(th), "thread B");
    }

    // 7. copy - duplicate the slab at an offset.
    int bodyC = -1;
    {
        auto cp = std::make_unique<CopyOp>();
        cp->setSourceBodyId(bodyA);
        cp->setOffset(60.0, 0.0, 0.0);
        push(std::move(cp), "copy A->C");
        for (int id : doc.getAllBodyIds())
            if (id != bodyA && id != bodyB) bodyC = id;
        ASSERT_GE(bodyC, 0);
    }

    // 8. align - nudge the copy.
    {
        auto al = std::make_unique<AlignOp>();
        al->setBodyId(bodyC);
        al->setSourcePoint(gp_Pnt(60, 0, 0));
        al->setTargetPoint(gp_Pnt(60, 30, 0));
        push(std::move(al), "align C");
    }

    // 9. mirror - the slab across a custom plane, keeping the original.
    {
        auto mi = std::make_unique<MirrorOp>();
        mi->setBody(bodyA);
        mi->setPlane(MirrorPlane::Custom);
        mi->setCustomPlane(gp_Ax2(gp_Pnt(-10, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 0, 1)));
        push(std::move(mi), "mirror A");
    }

    // 10. split - the copy, halfway up.
    {
        auto sp = std::make_unique<SplitBodyOp>();
        sp->setBody(bodyC);
        sp->setSplitPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 5), gp_Dir(0, 0, 1),
                                        gp_Dir(1, 0, 0))));
        push(std::move(sp), "split C");
    }

    // 11. transform - move the copy's lower half.
    {
        auto tr = std::make_unique<TransformOp>();
        tr->setBodyId(bodyC);
        tr->setType(TransformType::Translate);
        tr->setTranslation(0.0, 0.0, 30.0);
        push(std::move(tr), "transform C");
    }

    // 12/13. construction plane + a gizmo move of it.
    {
        auto cp = std::make_unique<ConstructionPlaneOp>();
        cp->setType(PlaneCreationType::XY);
        cp->setOffset(25.0);
        cp->setName("Test Plane");
        push(std::move(cp), "construction plane");
        ASSERT_FALSE(doc.getAllPlaneIds().empty());
        const int planeId = doc.getAllPlaneIds().back();
        PlaneTransformOp::Entry e;
        e.planeId = planeId;
        e.before = gp_Pln(gp_Ax3(gp_Pnt(0, 0, 25), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
        e.after  = gp_Pln(gp_Ax3(gp_Pnt(0, 0, 40), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
        doc.setPlane(planeId, e.after);   // the gizmo applies live...
        hist.pushExecuted(std::make_unique<PlaneTransformOp>(
            "Move Plane", std::vector<PlaneTransformOp::Entry>{e}));
    }

    // 14/15. construction axis + a gizmo move of it.
    {
        auto ax = std::make_unique<ConstructionAxisOp>();
        ax->setType(AxisCreationType::TwoPoints);
        ax->setPoints(gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 10));
        ax->setName("Test Axis");
        if (hist.pushOperation(std::move(ax), doc) &&
            !doc.getAllAxisIds().empty()) {
            const int axisId = doc.getAllAxisIds().back();
            AxisTransformOp::Entry e;
            e.axisId = axisId;
            e.beforeOrigin = gp_Pnt(0, 0, 0); e.beforeDir = gp_Dir(0, 0, 1);
            e.afterOrigin  = gp_Pnt(5, 5, 0); e.afterDir  = gp_Dir(0, 0, 1);
            doc.setAxis(axisId, e.afterOrigin, e.afterDir);
            hist.pushExecuted(std::make_unique<AxisTransformOp>(
                "Move Axis", std::vector<AxisTransformOp::Entry>{e}));
        }
    }

    // 16. sketchtransform - move a standalone sketch's plane.
    {
        auto skS = std::make_shared<Sketch>();
        skS->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 60), gp_Dir(0, 0, 1),
                                    gp_Dir(1, 0, 0))));
        int p0 = skS->addPoint({0.0f, 0.0f});
        skS->addCircle(p0, 4.0);
        int sidS = doc.addSketch(skS);
        auto st = std::make_unique<materializr::SketchTransformOp>();
        st->setSketch(sidS);
        gp_Trsf t;
        t.SetTranslation(gp_Vec(10, 0, 0));
        st->setTransform(t);
        push(std::move(st), "sketch transform");
    }

    // 17. loft - between two rectangles at different heights.
    {
        BRepBuilderAPI_MakePolygon w1, w2;
        w1.Add(gp_Pnt(120, 0, 0));  w1.Add(gp_Pnt(140, 0, 0));
        w1.Add(gp_Pnt(140, 20, 0)); w1.Add(gp_Pnt(120, 20, 0)); w1.Close();
        w2.Add(gp_Pnt(125, 5, 15)); w2.Add(gp_Pnt(135, 5, 15));
        w2.Add(gp_Pnt(135, 15, 15)); w2.Add(gp_Pnt(125, 15, 15)); w2.Close();
        auto lo = std::make_unique<LoftOp>();
        lo->addProfile(w1.Wire());
        lo->addProfile(w2.Wire());
        lo->setSolid(true);
        push(std::move(lo), "loft");
    }

    // 18. sweep - a small disc along an L path; then 19. delete it.
    int sweepBody = -1;
    {
        std::vector<int> preIds = doc.getAllBodyIds();
        gp_Circ c(gp_Ax2(gp_Pnt(160, 0, 0), gp_Dir(0, 1, 0)), 2.0);
        TopoDS_Edge ce = BRepBuilderAPI_MakeEdge(c).Edge();
        TopoDS_Wire cw = BRepBuilderAPI_MakeWire(ce).Wire();
        TopoDS_Face profile = BRepBuilderAPI_MakeFace(cw).Face();
        BRepBuilderAPI_MakePolygon path;
        path.Add(gp_Pnt(160, 0, 0));
        path.Add(gp_Pnt(160, 30, 0));
        path.Add(gp_Pnt(160, 30, 20));
        auto sw = std::make_unique<SweepOp>();
        sw->setProfile(profile);
        sw->setPath(path.Wire());
        push(std::move(sw), "sweep");
        for (int id : doc.getAllBodyIds()) {
            bool pre = false;
            for (int p : preIds) if (p == id) pre = true;
            if (!pre) sweepBody = id;
        }
        ASSERT_GE(sweepBody, 0);
        auto de = std::make_unique<DeleteOp>();
        de->setBodyId(sweepBody);
        push(std::move(de), "delete sweep");
    }

    // 20. extrude, face-driven - picked profile persists as a BREP blob.
    {
        gp_Circ c(gp_Ax2(gp_Pnt(200, 0, 0), gp_Dir(0, 0, 1)), 5.0);
        TopoDS_Edge ce = BRepBuilderAPI_MakeEdge(c).Edge();
        TopoDS_Wire cw = BRepBuilderAPI_MakeWire(ce).Wire();
        TopoDS_Face pf = BRepBuilderAPI_MakeFace(cw).Face();
        auto ext = std::make_unique<ExtrudeOp>();
        ext->setProfile(pf);         // no sketch source on purpose
        ext->setDistance(8.0);
        push(std::move(ext), "extrude face-driven");
    }

    const int nSteps = hist.currentStep() + 1;
    std::fprintf(stderr, "[full-replay] chain built: %d steps\n", nSteps);

    // ── Save through the real ProjectIO, load into a fresh document. ──
    ProjectHistory saved = captureHistory(hist, doc);
    ASSERT_EQ(static_cast<int>(saved.steps.size()), nSteps);
    const std::string path = mzrtest::tmpPath("mtz_full_replay.materializr");
    ASSERT_TRUE(ProjectIO::save(path, doc, &saved).success);
    Document doc2;
    ProjectHistory loaded;
    ASSERT_TRUE(ProjectIO::load(path, doc2, &loaded).success);
    std::remove(path.c_str());
    ASSERT_EQ(loaded.steps.size(), saved.steps.size());

    // ── The app's reload loop: EVERY step must come back editable. ──
    History hist2;
    std::map<int, TopoDS_Shape> running;
    for (const auto& [id, shp] : loaded.initialState) running[id] = shp;
    int frozen = 0;
    for (size_t i = 0; i < loaded.steps.size(); ++i) {
        const auto& st = loaded.steps[i];
        Operation::ReloadState rs;
        for (const auto& [id, shape] : st.changed) {
            if (running.find(id) == running.end()) {
                rs.created.push_back(id);
                rs.createdAfter.push_back({id, shape});
            } else {
                rs.modifiedBefore.push_back(std::make_pair(id, running[id]));
                rs.modifiedAfter.push_back(std::make_pair(id, shape));
            }
        }
        for (int id : st.deleted) {
            auto it = running.find(id);
            if (it != running.end())
                rs.deletedBefore.push_back(std::make_pair(id, it->second));
        }
        for (const auto& [id, shape] : st.changed) running[id] = shape;
        for (int id : st.deleted) running.erase(id);

        std::unique_ptr<Operation> op;
        if (st.typeId == "sketchedit" && !st.params.empty())
            op = ProjectIO::rehydrateSketchEditOp(st.params, doc2);
        if (!op) {
            auto candidate = OperationFactory::create(st.typeId);
            if (candidate && !st.params.empty() &&
                candidate->deserializeParams(st.params) &&
                candidate->rehydrateFromReload(rs, doc2)) {
                op = std::move(candidate);
            }
        }
        if (!op) {
            ++frozen;
            ADD_FAILURE() << "step " << i << " [" << st.typeId
                          << "] reloaded FROZEN (params: "
                          << st.params.substr(0, 60) << ")";
            continue;
        }
        hist2.pushExecuted(std::move(op));
    }
    ASSERT_EQ(frozen, 0) << "full history replay requires ZERO frozen steps";
    ASSERT_EQ(hist2.stepCount(), nSteps);

    // ── Full replay: re-execute the whole chain on the loaded document. ──
    ASSERT_TRUE(hist2.editStep(0, doc2, /*transactional=*/true))
        << "the reloaded chain must replay end to end";

    // ── Parametric proof: edit the base sketch and replay again. ──
    auto skA2 = doc2.getSketch(sidA);
    ASSERT_NE(skA2, nullptr);
    // widen 34 -> 44 (points keep their ids through save/load)
    skA2->movePoint(pa[1], {44.0f, 0.0f});
    skA2->movePoint(pa[2], {44.0f, 20.0f});
    auto* ext2 = dynamic_cast<ExtrudeOp*>(
        const_cast<Operation*>(hist2.getStep(0)));
    ASSERT_NE(ext2, nullptr);
    ASSERT_TRUE(ext2->rebuildProfileFromSketch(doc2));
    doc2.setCascadeSketchOverride(sidA, std::make_shared<Sketch>(*skA2));
    const bool replayed = hist2.editStep(0, doc2, /*transactional=*/true);
    doc2.clearCascadeSketchOverrides();
    ASSERT_TRUE(replayed) << "edited chain must replay";
    Bnd_Box bb;
    BRepBndLib::Add(doc2.getBody(bodyA), bb);
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x1, 44.0, 0.5) << "slab followed the sketch edit through the "
                                  "reloaded, fully-replayed history";
    EXPECT_GT(volumeOf(doc2.getBody(bodyA)), 0.0);
}
