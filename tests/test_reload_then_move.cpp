// Moving a sketch after a project has been SAVED AND REOPENED (#185, Iron47). The whole
// history is rebuilt from the file, then the sketch is moved and everything replays: the
// extrude, the chamfers and a chamfer built on a chamfer must all follow, none switched off,
// and the body must come out the same shape in its new place.
//
// The frame is Iron47's: a thin square frame (a 24 mm square with a 20 mm hole, so the
// region samples recorded for it find nothing and the extrude replays from its saved
// footprint), chamfered on its corners and then on the bevels' own edges.

#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "io/ProjectIO.h"
#include "modeling/ChamferOp.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/OperationFactory.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
#include "modeling/SketchTransformOp.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <cstdio>
#include <map>
#include <memory>
#include "test_tmp_path.h"

using namespace materializr;

namespace {

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
        if (auto* sk = dynamic_cast<const SketchEditOp*>(op)) steps[i].params = sk->serializeWithDocument(doc);
        else steps[i].params = op->serializeParams();
        if (!op->isEnabled()) continue;
        OperationDiff d = op->captureDiff();
        for (const auto& [id, before] : d.modifiedBefore) {
            auto it = cur.find(id);
            if (it != cur.end()) steps[i].changed.push_back({id, it->second});
            cur[id] = before;
        }
        for (int id : d.created) {
            auto it = cur.find(id);
            if (it != cur.end()) steps[i].changed.push_back({id, it->second});
            cur.erase(id);
        }
        for (const auto& [id, before] : d.deletedBefore) { steps[i].deleted.push_back(id); cur[id] = before; }
    }
    for (const auto& [id, shp] : cur) h.initialState.push_back({id, shp});
    h.steps = std::move(steps);
    return h;
}

double vol(const TopoDS_Shape& s) { GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass(); }
double minZ(const TopoDS_Shape& s) {
    Bnd_Box b; BRepBndLib::Add(s, b);
    double x0, y0, z0, x1, y1, z1; b.Get(x0, y0, z0, x1, y1, z1);
    return z0;
}

std::vector<TopoDS_Edge> edgesWhere(const TopoDS_Shape& s, bool (*pick)(const gp_Pnt&, const gp_Pnt&)) {
    std::vector<TopoDS_Edge> out;
    TopTools_IndexedMapOfShape m; TopExp::MapShapes(s, TopAbs_EDGE, m);
    for (int i = 1; i <= m.Extent(); ++i) {
        TopoDS_Edge e = TopoDS::Edge(m(i));
        BRepAdaptor_Curve c(e);
        if (pick(c.Value(c.FirstParameter()), c.Value(c.LastParameter()))) out.push_back(e);
    }
    return out;
}

bool outerVertical(const gp_Pnt& a, const gp_Pnt& b) {
    return std::abs(a.X() - b.X()) < 1e-6 && std::abs(a.Y() - b.Y()) < 1e-6 &&
           std::abs(std::abs(a.X()) - 12.0) < 1e-6 && std::abs(std::abs(a.Y()) - 12.0) < 1e-6;
}
bool shortTop(const gp_Pnt& a, const gp_Pnt& b) {
    return std::abs(a.Z() - 20.0) < 1e-6 && std::abs(b.Z() - 20.0) < 1e-6 && a.Distance(b) < 3.0;
}

std::shared_ptr<Sketch> frameSketch() {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    sk->addRectangle({-12, -12}, {12, 12});
    sk->addRectangle({-10, -10}, {10, 10});
    return sk;
}

} // namespace

static void runScene(bool stripMint) {
    Document doc; History hist;
    const int sid = doc.addSketch(frameSketch(), "Sketch 1");
    auto ext = std::make_unique<ExtrudeOp>();
    ext->setSketchSource(sid);
    ext->setDistance(20.0);
    ASSERT_TRUE(ext->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(hist.pushOperation(std::move(ext), doc));
    const int b = doc.getAllBodyIds().front();
    const double frame = 24.0 * 24.0 * 20.0 - 20.0 * 20.0 * 20.0;
    ASSERT_NEAR(vol(doc.getBody(b)), frame, 1e-3);

    auto verticals = edgesWhere(doc.getBody(b), outerVertical);
    ASSERT_EQ(verticals.size(), 4u);
    auto c1 = std::make_unique<ChamferOp>();
    c1->setBody(b); c1->setEdges(verticals); c1->setDistance(1.4); c1->setSourceSketch(sid);
    ASSERT_TRUE(hist.pushOperation(std::move(c1), doc));
    auto shorts = edgesWhere(doc.getBody(b), shortTop);
    ASSERT_EQ(shorts.size(), 4u);
    auto c2 = std::make_unique<ChamferOp>();
    c2->setBody(b); c2->setEdges(shorts); c2->setDistance(0.6); c2->setSourceSketch(sid);
    ASSERT_TRUE(hist.pushOperation(std::move(c2), doc));
    const double built = vol(doc.getBody(b));

    // Save, and open it in a fresh document the way the app does.
    ProjectHistory saved = captureHistory(hist, doc);
    if (stripMint)
        for (auto& st : saved.steps) {
            const size_t a = st.params.find(";mint=");
            if (a == std::string::npos) continue;
            const size_t b2 = st.params.find(';', st.params.find(";msig=", a) + 1);
            st.params.erase(a, (b2 == std::string::npos ? st.params.size() : b2) - a);
        }
    const std::string path = mzrtest::tmpPath("mtz_reload_then_move.materializr");
    ASSERT_TRUE(ProjectIO::save(path, doc, &saved).success);
    Document doc2;
    ProjectHistory loaded;
    ASSERT_TRUE(ProjectIO::load(path, doc2, &loaded).success);
    std::remove(path.c_str());
    History hist2;
    std::map<int, TopoDS_Shape> running;
    for (const auto& [id, shp] : loaded.initialState) running[id] = shp;
    for (const auto& st : loaded.steps) {
        Operation::ReloadState rs;
        for (const auto& [id, shape] : st.changed) {
            if (running.find(id) == running.end()) { rs.created.push_back(id); rs.createdAfter.push_back({id, shape}); }
            else { rs.modifiedBefore.push_back({id, running[id]}); rs.modifiedAfter.push_back({id, shape}); }
        }
        for (const auto& [id, shape] : st.changed) running[id] = shape;
        std::unique_ptr<Operation> op;
        auto cand = OperationFactory::create(st.typeId);
        if (cand && cand->deserializeParams(st.params) && cand->rehydrateFromReload(rs, doc2)) op = std::move(cand);
        ASSERT_TRUE(op) << st.typeId << " reloaded frozen";
        hist2.pushExecuted(std::move(op));
    }
    ASSERT_EQ(hist2.stepCount(), 3);
    EXPECT_NEAR(vol(doc2.getBody(b)), built, 1e-3);

    // Move the sketch and replay.
    auto mv = std::make_unique<SketchTransformOp>();
    mv->setSketch(sid);
    gp_Trsf t; t.SetTranslation(gp_Vec(0, 0, 40));
    mv->setTransform(t);
    ASSERT_TRUE(hist2.pushOperation(std::move(mv), doc2));
    auto links = computeSketchBodyLinks(hist2);
    const int earliest = rederiveSketchDrivenOps(hist2, doc2, sid);
    ASSERT_GE(earliest, 0);
    doc2.setCascadeSketchOverride(sid, std::make_shared<Sketch>(*doc2.getSketch(sid)));
    const bool ok = hist2.editStep(earliest, doc2, /*transactional=*/true);
    doc2.clearCascadeSketchOverrides();
    ASSERT_TRUE(ok) << "failing step " << hist2.lastEditFailStep();
    for (int i = 0; i < 3; ++i)
        EXPECT_TRUE(hist2.getStep(i)->isEnabled()) << "step " << i << " [" << hist2.getStep(i)->typeId() << "] switched off";
    EXPECT_NEAR(minZ(doc2.getBody(b)), 40.0, 1e-6) << "the body moved with the sketch";
    EXPECT_NEAR(vol(doc2.getBody(b)), built, 1e-3) << "and is the same shape";
    (void)links;
}

TEST(ReloadThenMove, ChamferedFrameFollowsAMovedSketch) { runScene(false); }
// A file saved before the unnamed-face ids were kept (runScene(true) strips the mint= of its
// chamfers) cannot follow a move with a chamfer built on another chamfer: the second one's
// edges are named by ids that are lost, and it is switched off. Everything else follows.
