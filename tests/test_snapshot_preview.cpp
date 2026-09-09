// SnapshotPreviewJob runs a snapshot-body op (Shell, Draft, Scale Face,
// Fillet) on a private copy of the gesture-start body, in a scratch Document
// under the live id, and reports the scratch body: the off-thread preview for
// the InteractiveOpController SnapshotBody engine.
#include "app/SnapshotPreview.h"
#include "core/Document.h"
#include "modeling/ChamferOp.h"
#include "modeling/FilletOp.h"
#include "modeling/ScaleFaceOp.h"
#include "modeling/ShellOp.h"
#include "modeling/TaperOp.h"

#include <gtest/gtest.h>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <memory>
#include <vector>

using materializr::SnapshotPreviewJob;
using materializr::SnapshotPreviewResult;

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
    double bestZ = -1e300;
    for (TopExp_Explorer e(box, TopAbs_FACE); e.More(); e.Next()) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(e.Current(), g);
        if (g.CentreOfMass().Z() > bestZ) {
            bestZ = g.CentreOfMass().Z();
            best = TopoDS::Face(e.Current());
        }
    }
    return best;
}

std::vector<TopoDS_Edge> edgesOf(const TopoDS_Face& f) {
    std::vector<TopoDS_Edge> out;
    for (TopExp_Explorer e(f, TopAbs_EDGE); e.More(); e.Next()) out.push_back(TopoDS::Edge(e.Current()));
    return out;
}

// True when no shape in `params` is a sub-shape of `body` (by IsSame).
bool disjoint(const std::vector<TopoDS_Shape>& params, const TopoDS_Shape& body) {
    TopTools_IndexedMapOfShape subs;
    TopExp::MapShapes(body, subs);
    for (const auto& p : params)
        if (subs.Contains(p)) return false;
    return true;
}

bool allIn(const std::vector<TopoDS_Shape>& params, const TopoDS_Shape& body) {
    TopTools_IndexedMapOfShape subs;
    TopExp::MapShapes(body, subs);
    for (const auto& p : params)
        if (!subs.Contains(p)) return false;
    return true;
}

// Run `make(id)` inline on a document holding `body` under `id`, return the body.
template <class Make>
TopoDS_Shape inlineResult(const TopoDS_Shape& body, Make make) {
    Document doc;
    const int id = doc.addBody(body, "b");
    std::unique_ptr<Operation> op = make(id);
    if (!op->execute(doc)) return TopoDS_Shape();
    return doc.getBody(id);
}

std::unique_ptr<Operation> shellOp(int id, const TopoDS_Face& open, double t) {
    auto op = std::make_unique<ShellOp>();
    op->setBody(id);
    op->setThickness(t);
    op->addFaceToRemove(open);
    return op;
}

} // namespace

TEST(SnapshotPreview, ShellOnTheCopyEqualsTheInlineExecute) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 20, 10).Shape();
    const TopoDS_Face top = topFace(box);
    const TopoDS_Shape direct = inlineResult(box, [&](int id) { return shellOp(id, top, 1.0); });
    ASSERT_FALSE(direct.IsNull());

    const int liveId = 7;
    auto job = SnapshotPreviewJob::prepare(liveId, box, shellOp(liveId, top, 1.0));
    ASSERT_TRUE(job);
    SnapshotPreviewResult r = job->run();
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(volume(r.shape), volume(direct), 1e-6);
    EXPECT_EQ(faceCount(r.shape), faceCount(direct));
    EXPECT_NEAR(volume(box), 20.0 * 20.0 * 10.0, 1e-9); // the snapshot is untouched
}

TEST(SnapshotPreview, ParametersArePointedAtTheCopyNotTheSnapshot) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 20, 10).Shape();
    const TopoDS_Face top = topFace(box);
    auto job = SnapshotPreviewJob::prepare(3, box, shellOp(3, top, 1.0));
    ASSERT_TRUE(job);
    const std::vector<TopoDS_Shape> params = job->params();
    ASSERT_EQ(params.size(), 1u);
    EXPECT_TRUE(disjoint(params, box));       // never a live sub-shape
    EXPECT_TRUE(allIn(params, job->copy()));  // the copy's own face
    EXPECT_EQ(params[0].Orientation(), top.Orientation());
    EXPECT_TRUE(disjoint({job->copy()}, box));
}

TEST(SnapshotPreview, FilletAndScaleFaceMatchInline) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 20, 10).Shape();
    const TopoDS_Face top = topFace(box);
    const std::vector<TopoDS_Edge> edges = edgesOf(top);

    auto fillet = [&](int id) -> std::unique_ptr<Operation> {
        auto op = std::make_unique<FilletOp>();
        op->setBody(id);
        op->setEdges(edges);
        op->setRadius(2.0);
        return op;
    };
    const TopoDS_Shape directF = inlineResult(box, fillet);
    ASSERT_FALSE(directF.IsNull());
    auto jobF = SnapshotPreviewJob::prepare(5, box, fillet(5));
    ASSERT_TRUE(jobF);
    EXPECT_EQ(jobF->params().size(), edges.size());
    EXPECT_TRUE(disjoint(jobF->params(), box));
    SnapshotPreviewResult rf = jobF->run();
    ASSERT_TRUE(rf.ok);
    EXPECT_NEAR(volume(rf.shape), volume(directF), 1e-6);
    EXPECT_EQ(faceCount(rf.shape), faceCount(directF));
    EXPECT_LT(volume(rf.shape), volume(box)); // it did fillet

    auto scale = [&](int id) -> std::unique_ptr<Operation> {
        auto op = std::make_unique<ScaleFaceOp>();
        op->setBody(id);
        op->setFace(top);
        op->setScaleUV(150.0, 150.0);
        op->setLength(4.0);
        op->setMode(ScaleFaceOp::Mode::Pinch);
        return op;
    };
    const TopoDS_Shape directS = inlineResult(box, scale);
    ASSERT_FALSE(directS.IsNull());
    auto jobS = SnapshotPreviewJob::prepare(5, box, scale(5));
    ASSERT_TRUE(jobS);
    SnapshotPreviewResult rs = jobS->run();
    ASSERT_TRUE(rs.ok);
    EXPECT_NEAR(volume(rs.shape), volume(directS), 1e-6);
    EXPECT_EQ(faceCount(rs.shape), faceCount(directS));
    EXPECT_GT(volume(rs.shape), volume(box)); // it did widen
}

TEST(SnapshotPreview, DraftMatchesInline) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 20, 10).Shape();
    auto taper = [&](int id) -> std::unique_ptr<Operation> {
        auto op = std::make_unique<TaperOp>();
        op->setBody(id);
        op->setDirection(0, 0, 1);
        op->setNeutralPoint(0, 0, 0);
        op->setAngleDeg(5.0);
        // Draft every side wall.
        for (TopExp_Explorer e(box, TopAbs_FACE); e.More(); e.Next()) {
            GProp_GProps g;
            BRepGProp::SurfaceProperties(e.Current(), g);
            const double z = g.CentreOfMass().Z();
            if (z > 1e-6 && z < 10.0 - 1e-6) op->addFace(TopoDS::Face(e.Current()));
        }
        return op;
    };
    const TopoDS_Shape direct = inlineResult(box, taper);
    ASSERT_FALSE(direct.IsNull());
    auto job = SnapshotPreviewJob::prepare(2, box, taper(2));
    ASSERT_TRUE(job);
    EXPECT_EQ(job->params().size(), 4u);
    EXPECT_TRUE(disjoint(job->params(), box));
    SnapshotPreviewResult r = job->run();
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(volume(r.shape), volume(direct), 1e-6);
    EXPECT_NE(volume(r.shape), volume(box));
}

TEST(SnapshotPreview, AParameterOutsideTheSnapshotRefusesToPrepare) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 20, 10).Shape();
    const TopoDS_Shape other = BRepPrimAPI_MakeBox(5, 5, 5).Shape();
    EXPECT_FALSE(SnapshotPreviewJob::prepare(1, box, shellOp(1, topFace(other), 1.0)));
    EXPECT_FALSE(SnapshotPreviewJob::prepare(-1, box, shellOp(1, topFace(box), 1.0)));
    EXPECT_FALSE(SnapshotPreviewJob::prepare(1, TopoDS_Shape(), shellOp(1, topFace(box), 1.0)));
    EXPECT_FALSE(SnapshotPreviewJob::prepare(1, box, nullptr));
}

TEST(SnapshotPreview, ARefusedOpReportsNotOkAndLeavesTheSnapshot) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 20, 10).Shape();
    // A wall thicker than the box: the offset cannot be built.
    auto job = SnapshotPreviewJob::prepare(1, box, shellOp(1, topFace(box), 50.0));
    ASSERT_TRUE(job);
    SnapshotPreviewResult r = job->run();
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.shape.IsNull());
    EXPECT_NEAR(volume(box), 4000.0, 1e-9);
    EXPECT_EQ(faceCount(box), 6);
}

TEST(SnapshotPreview, ChamferOnALocatedBodyMatchesInline) {
    // A body carrying a non-identity location: ModifiedShape must find the
    // located edges, and the result must equal the inline execute.
    gp_Trsf move;
    move.SetTranslation(gp_Vec(100.0, -50.0, 30.0));
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 20, 10).Shape().Moved(TopLoc_Location(move));
    const TopoDS_Face top = topFace(box);
    const std::vector<TopoDS_Edge> edges = edgesOf(top);
    auto chamfer = [&](int id) -> std::unique_ptr<Operation> {
        auto op = std::make_unique<ChamferOp>();
        op->setBody(id);
        op->setEdges(edges);
        op->setDistance(1.5);
        return op;
    };
    const TopoDS_Shape direct = inlineResult(box, chamfer);
    ASSERT_FALSE(direct.IsNull());
    auto job = SnapshotPreviewJob::prepare(9, box, chamfer(9));
    ASSERT_TRUE(job);
    EXPECT_EQ(job->params().size(), edges.size());
    EXPECT_TRUE(disjoint(job->params(), box));
    EXPECT_TRUE(allIn(job->params(), job->copy()));
    SnapshotPreviewResult r = job->run();
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(volume(r.shape), volume(direct), 1e-6);
    EXPECT_EQ(faceCount(r.shape), faceCount(direct));
    EXPECT_LT(volume(r.shape), volume(box));
    GProp_GProps g;
    BRepGProp::VolumeProperties(r.shape, g);
    EXPECT_NEAR(g.CentreOfMass().X(), 110.0, 0.5); // still where the body is
}
