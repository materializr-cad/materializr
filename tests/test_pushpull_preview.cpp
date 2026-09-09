// PreviewJob runs PushPullOp::execute() on a scratch copy of the bodies and
// reports the live bodies' new shapes; PushPullOp::setPrecomputed() applies
// such a result without re-running the booleans. Together they are the
// off-thread push/pull preview.
#include "app/PushPullPreview.h"
#include "core/BodyChanges.h"
#include "core/Document.h"
#include "modeling/FaceLineage.h"
#include "modeling/PushPullOp.h"

#include <gtest/gtest.h>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>

#include <cmath>

using materializr::PreviewJob;
using materializr::PreviewParams;
using materializr::PreviewResult;
using materializr::PreviewTarget;

namespace {

double volume(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

int faceCount(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
}

// The +Z face of an axis-aligned box.
TopoDS_Face topFace(const TopoDS_Shape& box) {
    TopoDS_Face best;
    double bestZ = -1e9;
    for (TopExp_Explorer e(box, TopAbs_FACE); e.More(); e.Next()) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(e.Current(), g);
        if (g.CentreOfMass().Z() > bestZ) { bestZ = g.CentreOfMass().Z(); best = TopoDS::Face(e.Current()); }
    }
    return best;
}

// Run the real op on `doc` for the same gesture: the reference.
TopoDS_Shape directResult(Document& doc, int bodyId, const TopoDS_Face& profile,
                          double distance, bool cutIntersecting) {
    PushPullOp op;
    PushPullOp::Target t;
    t.profile = profile;
    t.sourceBodyId = bodyId;
    op.setTargets({t});
    op.setDistance(distance);
    op.setCutIntersecting(cutIntersecting);
    EXPECT_TRUE(op.execute(doc));
    return doc.getBody(bodyId);
}

} // namespace

TEST(PushPullPreview, FuseOnAHostBodyEqualsADirectExecuteAndLeavesTheLiveDocumentAlone) {
    Document live;
    // Two unrelated bodies first, so the host's live id differs from the id
    // it gets in the scratch document: results must come back under LIVE ids.
    live.addBody(BRepPrimAPI_MakeBox(gp_Pnt(200.0, 0.0, 0.0), 5.0, 5.0, 5.0).Shape(), "far1");
    live.addBody(BRepPrimAPI_MakeBox(gp_Pnt(300.0, 0.0, 0.0), 5.0, 5.0, 5.0).Shape(), "far2");
    const int id = live.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape(), "block");
    ASSERT_GE(id, 2);
    const TopoDS_Shape before = live.getBody(id);
    const materializr::BodySnapshot originals = materializr::snapshotBodies(live);

    PreviewTarget t;
    t.profile = topFace(before);
    t.sourceBodyId = id;
    PreviewParams p;
    p.distance = 5.0;
    std::unique_ptr<PreviewJob> job = PreviewJob::prepare(originals, {t}, p);
    ASSERT_TRUE(job);
    EXPECT_EQ(job->copiedBodies(), 1u);
    PreviewResult r = job->run();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.bodies.size(), 1u);
    EXPECT_EQ(r.bodies[0].first, id);
    EXPECT_TRUE(r.created.empty());
    // Nothing on the live side moved.
    EXPECT_TRUE(live.getBody(id).IsEqual(before));

    Document ref;
    const int rid = ref.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape(), "block");
    const TopoDS_Shape direct = directResult(ref, rid, topFace(ref.getBody(rid)), 5.0, false);
    EXPECT_NEAR(volume(r.bodies[0].second), volume(direct), 1e-6);
    EXPECT_NEAR(volume(r.bodies[0].second), 20.0 * 20.0 * 15.0, 1e-6);
    EXPECT_EQ(faceCount(r.bodies[0].second), faceCount(direct));
}

TEST(PushPullPreview, ACutThroughTheModelReachesTheSecondBodyToo) {
    // A negative drag cuts the host and every visible body in the tool's path.
    // Host 20x20 at z 0..10, cut 8 deep: the tool spans z 2..10. A post
    // 10x10x15 at z -10..5 runs through the host and loses its top 3 mm; a
    // hidden twin of it is left alone (the real op skips hidden bodies); a distant
    // body is out of reach and not even copied.
    auto build = [](Document& d, int& host, int& post, int& hidden, int& outOfReach) {
        d.addBody(BRepPrimAPI_MakeBox(gp_Pnt(200.0, 0.0, 0.0), 5.0, 5.0, 5.0).Shape(), "far0"); // shifts live ids off the scratch ids
        host = d.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape(), "host");
        post = d.addBody(BRepPrimAPI_MakeBox(gp_Pnt(5.0, 5.0, -10.0), 10.0, 10.0, 15.0).Shape(), "post");
        hidden = d.addBody(BRepPrimAPI_MakeBox(gp_Pnt(5.0, 5.0, -10.0), 10.0, 10.0, 15.0).Shape(), "hidden");
        d.setBodyVisible(hidden, false);
        outOfReach = d.addBody(BRepPrimAPI_MakeBox(gp_Pnt(100.0, 0.0, 0.0), 5.0, 5.0, 5.0).Shape(), "far");
    };
    Document live;
    int host, post, hidden, outOfReach;
    build(live, host, post, hidden, outOfReach);
    const materializr::BodySnapshot originals = materializr::snapshotBodies(live);

    PreviewTarget t;
    t.profile = topFace(live.getBody(host));
    t.sourceBodyId = host;
    PreviewParams p;
    p.distance = -8.0;
    p.cutIntersecting = true;
    std::unique_ptr<PreviewJob> job = PreviewJob::prepare(originals, {t}, p);
    ASSERT_TRUE(job);
    EXPECT_EQ(job->copiedBodies(), 2u); // host and post
    PreviewResult r = job->run();
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.bodies.size(), 2u);
    double hostVol = -1.0, postVol = -1.0;
    for (const auto& [id, s] : r.bodies) {
        if (id == host) hostVol = volume(s);
        if (id == post) postVol = volume(s);
    }
    EXPECT_NEAR(hostVol, 20.0 * 20.0 * 2.0, 1e-6);
    EXPECT_NEAR(postVol, 10.0 * 10.0 * 12.0, 1e-6);
    // The live document is untouched, hidden included.
    for (const auto& [id, st] : originals) EXPECT_TRUE(live.getBody(id).IsEqual(st.shape)) << id;

    // Same gesture run for real: the reference.
    Document ref;
    int rh, rp, rhid, rfar;
    build(ref, rh, rp, rhid, rfar);
    PushPullOp op;
    PushPullOp::Target rt;
    rt.profile = topFace(ref.getBody(rh));
    rt.sourceBodyId = rh;
    op.setTargets({rt});
    op.setDistance(-8.0);
    op.setCutIntersecting(true);
    ASSERT_TRUE(op.execute(ref));
    EXPECT_NEAR(hostVol, volume(ref.getBody(rh)), 1e-6);
    EXPECT_NEAR(postVol, volume(ref.getBody(rp)), 1e-6);
    EXPECT_NEAR(volume(ref.getBody(rhid)), 10.0 * 10.0 * 15.0, 1e-6);
}

TEST(PushPullPreview, AStaleFaceProfileIsStillPrepared) {
    // The profile is a face of some other body (the host was rebuilt under
    // it): prepare() must neither throw (ModifiedShape does, for a shape
    // that was not copied) nor refuse; the op re-resolves against the copy.
    Document live;
    const int host = live.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape(), "host");
    const TopoDS_Shape other = BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape();
    const materializr::BodySnapshot originals = materializr::snapshotBodies(live);
    PreviewTarget t;
    t.profile = topFace(other); // same geometry, not a face of the host
    t.sourceBodyId = host;
    PreviewParams p;
    p.distance = 5.0;
    std::unique_ptr<PreviewJob> job = PreviewJob::prepare(originals, {t}, p);
    ASSERT_TRUE(job);
    // The worker must get its own copy: no TShape of the prepared profile
    // (face, edges, vertices) may be one of the live profile's.
    ASSERT_EQ(job->profiles().size(), 1u);
    // (TopExp_Explorer over TopAbs_SHAPE is empty by definition; map the
    // descendants instead and make sure some were collected.)
    TopTools_IndexedMapOfShape liveParts, preparedParts;
    TopExp::MapShapes(t.profile, liveParts);
    TopExp::MapShapes(job->profiles()[0], preparedParts);
    ASSERT_GT(preparedParts.Extent(), 1); // the face plus its edges and vertices
    for (int i = 1; i <= preparedParts.Extent(); ++i)
        EXPECT_FALSE(liveParts.Contains(preparedParts(i))) << "shared sub-shape " << i;
    PreviewResult r = job->run();
    EXPECT_TRUE(r.ok);
    ASSERT_EQ(r.bodies.size(), 1u);
    EXPECT_NEAR(volume(r.bodies[0].second), 20.0 * 20.0 * 15.0, 1e-6);
    EXPECT_EQ(r.bodies[0].first, host);
}

TEST(PushPullPreview, AFreeProfileWithNoHostAndNoSketchIsCopiedToo) {
    // The third kind of target (neither host body nor sketch region): the
    // worker must still get its own copy, and the sweep becomes a new body.
    Document live;
    live.addBody(BRepPrimAPI_MakeBox(gp_Pnt(200.0, 0.0, 0.0), 5.0, 5.0, 5.0).Shape(), "far");
    const TopoDS_Shape other = BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape();
    const materializr::BodySnapshot originals = materializr::snapshotBodies(live);
    PreviewTarget t;
    t.profile = topFace(other);
    t.sourceBodyId = -1;
    t.sketchId = -1;
    PreviewParams p;
    p.distance = 5.0;
    std::unique_ptr<PreviewJob> job = PreviewJob::prepare(originals, {t}, p);
    ASSERT_TRUE(job);
    ASSERT_EQ(job->profiles().size(), 1u);
    TopTools_IndexedMapOfShape liveParts, preparedParts;
    TopExp::MapShapes(t.profile, liveParts);
    TopExp::MapShapes(job->profiles()[0], preparedParts);
    ASSERT_GT(preparedParts.Extent(), 1);
    for (int i = 1; i <= preparedParts.Extent(); ++i)
        EXPECT_FALSE(liveParts.Contains(preparedParts(i))) << "shared sub-shape " << i;
    PreviewResult r = job->run();
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.bodies.empty());
    ASSERT_EQ(r.created.size(), 1u);
    EXPECT_NEAR(volume(r.created[0]), 20.0 * 20.0 * 5.0, 1e-6);
}

TEST(PushPullPreview, AMeshImportInTheToolsPathIsNotCopied) {
    // Copying a 100k-face import costs more than the preview gains; the real
    // op still cuts it, once, at commit.
    Document live;
    const int host = live.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape(), "host");
    const int import = live.addBody(BRepPrimAPI_MakeBox(gp_Pnt(5.0, 5.0, -10.0), 10.0, 10.0, 15.0).Shape(), "stl");
    live.setBodyMesh(import, true);
    const materializr::BodySnapshot originals = materializr::snapshotBodies(live);
    EXPECT_TRUE(originals.at(import).mesh);
    PreviewTarget t;
    t.profile = topFace(live.getBody(host));
    t.sourceBodyId = host;
    PreviewParams p;
    p.distance = -8.0;
    p.cutIntersecting = true;
    std::unique_ptr<PreviewJob> job = PreviewJob::prepare(originals, {t}, p);
    ASSERT_TRUE(job);
    EXPECT_EQ(job->copiedBodies(), 1u);
}

TEST(PushPullPreview, PrecomputedResultIsAppliedWithoutBooleansAndUndone) {
    Document doc;
    const int id = doc.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape(), "block");
    const TopoDS_Shape before = doc.getBody(id);
    const TopoDS_Shape replacement = BRepPrimAPI_MakeBox(20.0, 20.0, 15.0).Shape();
    const TopoDS_Shape extra = BRepPrimAPI_MakeBox(gp_Pnt(50, 0, 0), 5.0, 5.0, 5.0).Shape();

    PushPullOp op;
    PushPullOp::Target t;
    t.profile = topFace(before);
    t.sourceBodyId = id;
    op.setTargets({t});
    op.setDistance(5.0);
    // The host carries face lineage; a landed-then-undone preview must give
    // it back (updateBody drops it, the op snapshots it for undo).
    materializr::topo::FaceIdMap ids;
    for (TopExp_Explorer e(before, TopAbs_FACE); e.More(); e.Next())
        ids.push_back({e.Current(), {100 + static_cast<int>(ids.size())}});
    doc.setBodyFaceIds(id, ids);

    PushPullOp::Precomputed pre;
    pre.bodies.emplace_back(id, replacement);
    pre.created.push_back(extra);
    op.setPrecomputed(std::move(pre));
    ASSERT_TRUE(op.execute(doc));
    EXPECT_TRUE(op.usedPrecomputed());
    EXPECT_TRUE(doc.getBody(id).IsEqual(replacement));
    ASSERT_EQ(doc.getAllBodyIds().size(), 2u);
    const int created = doc.getAllBodyIds()[1];
    EXPECT_TRUE(doc.getBody(created).IsEqual(extra));

    ASSERT_TRUE(op.undo(doc));
    EXPECT_TRUE(doc.getBody(id).IsEqual(before));
    EXPECT_EQ(doc.getAllBodyIds().size(), 1u);
    ASSERT_NE(doc.bodyFaceIds(id), nullptr);
    EXPECT_EQ(doc.bodyFaceIds(id)->size(), 6u);
    for (const auto& seeded : ids) {
        const std::vector<int>* got = materializr::topo::idsFor(*doc.bodyFaceIds(id), seeded.face);
        ASSERT_NE(got, nullptr);
        EXPECT_EQ(*got, seeded.ids);
    }

    // One-shot: the next execute runs the real booleans.
    ASSERT_TRUE(op.execute(doc));
    EXPECT_FALSE(op.usedPrecomputed());
    EXPECT_NEAR(volume(doc.getBody(id)), 20.0 * 20.0 * 15.0, 1e-6);
}

TEST(PushPullPreview, ZeroDistanceOrNoTargetsPreparesNothing) {
    Document live;
    const int id = live.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape(), "block");
    const materializr::BodySnapshot originals = materializr::snapshotBodies(live);
    PreviewTarget t;
    t.profile = topFace(live.getBody(id));
    t.sourceBodyId = id;
    PreviewParams p;
    p.distance = 0.0;
    EXPECT_FALSE(PreviewJob::prepare(originals, {t}, p));
    p.distance = 5.0;
    EXPECT_FALSE(PreviewJob::prepare(originals, {}, p));
}
