// Face-lineage propagation THROUGH a push/pull - the class behind Steve's
// body-412 gift-box drift. PushPull rebuilds a body via a boolean against a
// transient prism and doc.updateBody CLEARS the FaceIdMap, so before the fix
// every downstream fillet/chamfer lost its edge lineage and drifted onto the
// wrong edges on replay. These pin the contract: a push/pull must re-publish a
// map, an untouched bystander face must keep its ancestry id, and the id must
// be STABLE across a re-execute (what makes replay deterministic).

#include "modeling/TopoName.h"
#include "modeling/FaceLineage.h"
#include "modeling/PushPullOp.h"
#include "modeling/FilletOp.h"
#include "core/Document.h"
#include "modeling/Sketch.h"
#include "modeling/ExtrudeOp.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <algorithm>
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
    for (int i = 0; i < 4; ++i) sk->addLine(pid[i], pid[(i + 1) % 4]);
    return sk;
}

TopoDS_Face topCap(const TopoDS_Shape& body) {
    TopoDS_Face best; double bestZ = -1e18;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
        if (g.CentreOfMass().Z() > bestZ) { bestZ = g.CentreOfMass().Z(); best = f; }
    }
    return best;
}

// The planar face whose outward normal is ~-Z (the bottom cap at z=0): a
// bystander a top-face push never touches, so it stays IsSame-identical.
TopoDS_Face bottomCap(const TopoDS_Shape& body) {
    TopoDS_Face best; double bestZ = 1e18;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
        if (g.CentreOfMass().Z() < bestZ) { bestZ = g.CentreOfMass().Z(); best = f; }
    }
    return best;
}

int idOf(const Document& doc, int body, const TopoDS_Face& f) {
    const auto* m = doc.bodyFaceIds(body);
    if (!m) return -1;
    const auto* ids = topo::idsFor(*m, f);
    return (ids && !ids->empty()) ? ids->front() : -1;
}

} // namespace

// A push/pull must re-publish a face lineage map (updateBody clears it) and
// give every face an id. (A fresh extrude publishes no map - the input has no
// ancestry - so the push MINTS ids; the contract is that the map exists and is
// complete afterwards, which is what downstream fillets key on.)
TEST(TopoPushPullLineage, PushRepublishesCompleteMap) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = doc.getAllBodyIds().front();

    // Push the TOP face out by 5 (the box grows to z=15).
    PushPullOp pp;
    PushPullOp::Target t; t.profile = topCap(doc.getBody(body)); t.sourceBodyId = body;
    pp.setTargets({ t }); pp.setDistance(5.0);
    ASSERT_TRUE(pp.execute(doc));

    const auto* m1 = doc.bodyFaceIds(body);
    ASSERT_NE(m1, nullptr);
    ASSERT_FALSE(m1->empty())
        << "push/pull must re-publish face lineage (updateBody cleared it)";
    // COMPLETE: every face of the result must carry an id, else a fillet's
    // face-id-pair capture (all-or-nothing) can't fire.
    for (TopExp_Explorer ex(doc.getBody(body), TopAbs_FACE); ex.More(); ex.Next())
        EXPECT_NE(topo::idsFor(*m1, ex.Current()), nullptr)
            << "every result face must have a lineage id after a push";
}

// The bystander CARRY contract: once a body carries lineage (established by a
// first push), a SECOND push on the top face must leave the untouched bottom
// cap's id unchanged - carried through, not re-minted to something new.
TEST(TopoPushPullLineage, BystanderIdCarriesThroughSecondPush) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = doc.getAllBodyIds().front();

    PushPullOp pp1;
    PushPullOp::Target t1; t1.profile = topCap(doc.getBody(body)); t1.sourceBodyId = body;
    pp1.setTargets({ t1 }); pp1.setDistance(5.0);
    ASSERT_TRUE(pp1.execute(doc));
    int bottomId = idOf(doc, body, bottomCap(doc.getBody(body)));
    ASSERT_GE(bottomId, 0) << "bottom cap must carry an id after the first push";

    PushPullOp pp2;
    PushPullOp::Target t2; t2.profile = topCap(doc.getBody(body)); t2.sourceBodyId = body;
    pp2.setTargets({ t2 }); pp2.setDistance(3.0);
    ASSERT_TRUE(pp2.execute(doc));
    EXPECT_EQ(idOf(doc, body, bottomCap(doc.getBody(body))), bottomId)
        << "the untouched bottom cap must CARRY its id through a second push";
}

// The stable-id contract that makes replay deterministic: a bystander id must
// be the SAME after a second execute (what a history replay does).
TEST(TopoPushPullLineage, BystanderIdStableAcrossReExecute) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = doc.getAllBodyIds().front();

    PushPullOp pp;
    PushPullOp::Target t; t.profile = topCap(doc.getBody(body)); t.sourceBodyId = body;
    pp.setTargets({ t }); pp.setDistance(5.0);
    ASSERT_TRUE(pp.execute(doc));
    int bottomId1 = idOf(doc, body, bottomCap(doc.getBody(body)));
    ASSERT_GE(bottomId1, 0);

    // Re-execute the whole chain (a replay): rebuild the base, re-run the push.
    ASSERT_TRUE(ext.execute(doc));
    ASSERT_TRUE(pp.execute(doc)) << "push/pull must re-resolve + re-run";
    int bottomId2 = idOf(doc, body, bottomCap(doc.getBody(body)));
    EXPECT_EQ(bottomId1, bottomId2)
        << "bottom-cap id must be stable across a re-execute (else fillets drift)";
}

// A push/pull CUT (negative distance) must also re-publish lineage.
TEST(TopoPushPullLineage, CutRepublishesLineage) {
    Document doc;
    int pid[4];
    auto sk = makeRect(20.0, 10.0, pid);
    int sid = doc.addSketch(sk);
    ExtrudeOp ext; ext.setSketchSource(sid); ext.setDistance(10.0);
    ASSERT_TRUE(ext.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(ext.execute(doc));
    int body = doc.getAllBodyIds().front();

    // First push publishes lineage; record the bottom-cap id it minted.
    PushPullOp pp0;
    PushPullOp::Target t0; t0.profile = topCap(doc.getBody(body)); t0.sourceBodyId = body;
    pp0.setTargets({ t0 }); pp0.setDistance(5.0);
    ASSERT_TRUE(pp0.execute(doc));
    int bottomId = idOf(doc, body, bottomCap(doc.getBody(body)));
    ASSERT_GE(bottomId, 0);

    // Pull the TOP face DOWN by 4 (a cut into the box).
    PushPullOp pp;
    PushPullOp::Target t; t.profile = topCap(doc.getBody(body)); t.sourceBodyId = body;
    pp.setTargets({ t }); pp.setDistance(-4.0);
    ASSERT_TRUE(pp.execute(doc));

    const auto* m1 = doc.bodyFaceIds(body);
    ASSERT_NE(m1, nullptr);
    ASSERT_FALSE(m1->empty()) << "a push/pull cut must re-publish face lineage";
    EXPECT_EQ(idOf(doc, body, bottomCap(doc.getBody(body))), bottomId)
        << "the untouched bottom cap must keep its id through a cut";
}
