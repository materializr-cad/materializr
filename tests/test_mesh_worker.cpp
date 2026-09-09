// MeshWorker meshes a private copy of a body on a worker thread and hands back
// one triangulation per live face; land() moves them onto the live faces.
// These tests check the result is the mesh a direct run would have produced.
#include "viewport/MeshWorker.h"
#include "app/GhostMesh.h"
#include "core/MeshParams.h"

#include <gtest/gtest.h>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Vec.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <chrono>
#include <thread>
#include <vector>

using materializr::MeshWorker;

namespace {

constexpr float kDefl = 0.1f, kAng = 0.3f;

// Per-face triangle counts, in explorer order (-1 for an unmeshed face).
std::vector<int> triangleCounts(const TopoDS_Shape& s) {
    std::vector<int> v;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) {
        TopLoc_Location l;
        Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(e.Current()), l);
        v.push_back(t.IsNull() ? -1 : t->NbTriangles());
    }
    return v;
}

// Per-face sum of node coordinates, in explorer order: the same mesh landed
// on the wrong face, or in the wrong frame, changes this.
std::vector<double> nodeSums(const TopoDS_Shape& s) {
    std::vector<double> v;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) {
        TopLoc_Location l;
        Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(e.Current()), l);
        double sum = 0.0;
        if (!t.IsNull())
            for (int i = 1; i <= t->NbNodes(); ++i) {
                const gp_Pnt p = t->Node(i);
                sum += p.X() + p.Y() + p.Z();
            }
        v.push_back(sum);
    }
    return v;
}

// A self-intersecting planar wire: the mesher leaves this face untriangulated,
// every time. Stands in for the fused-tori class of body.
TopoDS_Face bowtieFace() {
    BRepBuilderAPI_MakePolygon p(gp_Pnt(0, 0, 0), gp_Pnt(10, 10, 0), gp_Pnt(10, 0, 0), gp_Pnt(0, 10, 0), true);
    return BRepBuilderAPI_MakeFace(p.Wire(), true).Face();
}

TopoDS_Shape holePlate(int nx = 8, int ny = 8) {
    TopoDS_Shape plate = BRepPrimAPI_MakeBox(nx * 15.0, ny * 15.0, 10.0).Shape();
    TopoDS_Compound holes;
    BRep_Builder bb;
    bb.MakeCompound(holes);
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j)
            bb.Add(holes, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(7.5 + i * 15.0, 7.5 + j * 15.0, -1.0), gp_Dir(0, 0, 1)), 3.0, 12.0).Shape());
    return BRepAlgoAPI_Cut(plate, holes).Shape();
}

std::vector<MeshWorker::Result> waitAll(MeshWorker& w) {
    std::vector<MeshWorker::Result> all;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& r : w.collect()) all.push_back(std::move(r));
        if (w.pending() == 0 && !all.empty()) {
            for (auto& r : w.collect()) all.push_back(std::move(r)); // anything finished between the two calls
            return all;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return all;
}

} // namespace

TEST(MeshWorker, LandsTheMeshADirectRunWouldProduce) {
    TopoDS_Shape shape = holePlate();
    // Reference: the same body meshed directly (Delabella is deterministic).
    TopoDS_Shape reference = BRepBuilderAPI_Copy(shape, Standard_True, Standard_False).Shape();
    BRepMesh_IncrementalMesh direct(reference, materializr::meshParams(kDefl, kAng, true));
    const std::vector<int> expected = triangleCounts(reference);

    MeshWorker worker;
    worker.request(7, shape, kDefl, kAng);
    std::vector<MeshWorker::Result> results = waitAll(worker);
    ASSERT_EQ(results.size(), 1u);
    const MeshWorker::Result& r = results[0];
    EXPECT_EQ(r.bodyId, 7);
    EXPECT_EQ(r.tshape, shape.TShape().get());
    EXPECT_EQ(r.unmeshedFaces, 0);
    EXPECT_GT(r.millis, 0.0);
    // Nothing touched the live shape before land().
    for (int n : triangleCounts(shape)) EXPECT_EQ(n, -1);
    EXPECT_EQ(MeshWorker::land(r), static_cast<int>(expected.size()));
    EXPECT_EQ(triangleCounts(shape), expected);
    const std::vector<double> sums = nodeSums(shape), want = nodeSums(reference);
    ASSERT_EQ(sums.size(), want.size());
    for (size_t i = 0; i < sums.size(); ++i) EXPECT_NEAR(sums[i], want[i], 1e-6) << "face " << i;
}

TEST(MeshWorker, LandsTheEdgePolygonsToo) {
    // A mesher pass leaves every edge with a polygon on its faces'
    // triangulations; a landed result must too, or anything reading
    // BRep_Tool::PolygonOnTriangulation on the live shape (the push/pull
    // ghost, for one) finds nothing and takes its slow path forever.
    TopoDS_Shape shape = holePlate();
    MeshWorker worker;
    EXPECT_TRUE(worker.request(9, shape, kDefl, kAng));
    std::vector<MeshWorker::Result> results = waitAll(worker);
    ASSERT_EQ(results.size(), 1u);
    ASSERT_GT(MeshWorker::land(results[0]), 0);
    int edgesChecked = 0;
    for (TopExp_Explorer fe(shape, TopAbs_FACE); fe.More(); fe.Next()) {
        const TopoDS_Face face = TopoDS::Face(fe.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        ASSERT_FALSE(tri.IsNull());
        for (TopExp_Explorer ee(face, TopAbs_EDGE); ee.More(); ee.Next()) {
            const TopoDS_Edge edge = TopoDS::Edge(ee.Current());
            if (BRep_Tool::Degenerated(edge)) continue;
            EXPECT_FALSE(BRep_Tool::PolygonOnTriangulation(edge, tri, loc).IsNull());
            ++edgesChecked;
        }
    }
    EXPECT_GT(edgesChecked, 100);
    // Seam edges (one per hole wall) keep BOTH polygons, forward and reversed:
    // the single-polygon update would leave one and the edge no longer closed.
    // Every edge occurrence's polygon, seam sides included, must be the one
    // a direct mesher pass installs. Delabella is deterministic, so a direct
    // mesh of a copy numbers its nodes identically (test 1 relies on that)
    // and the node arrays compare exactly; a swapped or duplicated seam side
    // shows up here.
    TopoDS_Shape reference = BRepBuilderAPI_Copy(shape, Standard_True, Standard_False).Shape();
    BRepMesh_IncrementalMesh direct(reference, materializr::meshParams(kDefl, kAng, true));
    int closedSeams = 0, occurrences = 0, mismatched = 0;
    TopExp_Explorer fe(shape, TopAbs_FACE), fr(reference, TopAbs_FACE);
    for (; fe.More() && fr.More(); fe.Next(), fr.Next()) {
        const TopoDS_Face face = TopoDS::Face(fe.Current()), ref = TopoDS::Face(fr.Current());
        TopLoc_Location loc, rloc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        Handle(Poly_Triangulation) rtri = BRep_Tool::Triangulation(ref, rloc);
        TopExp_Explorer ee(face, TopAbs_EDGE), er(ref, TopAbs_EDGE);
        for (; ee.More() && er.More(); ee.Next(), er.Next()) {
            const TopoDS_Edge edge = TopoDS::Edge(ee.Current()), redge = TopoDS::Edge(er.Current());
            ASSERT_EQ(edge.Orientation(), redge.Orientation());
            if (BRep_Tool::Degenerated(edge)) continue;
            if (edge.Orientation() == TopAbs_FORWARD && BRep_Tool::IsClosed(edge, tri, loc)) ++closedSeams;
            Handle(Poly_PolygonOnTriangulation) p = BRep_Tool::PolygonOnTriangulation(edge, tri, loc);
            Handle(Poly_PolygonOnTriangulation) rp = BRep_Tool::PolygonOnTriangulation(redge, rtri, rloc);
            ASSERT_FALSE(p.IsNull());
            ASSERT_FALSE(rp.IsNull());
            ++occurrences;
            const TColStd_Array1OfInteger& n = p->Nodes();
            const TColStd_Array1OfInteger& rn = rp->Nodes();
            bool same = n.Length() == rn.Length();
            for (int k = n.Lower(); same && k <= n.Upper(); ++k)
                same = n(k) == rn(rn.Lower() + (k - n.Lower()));
            if (!same) ++mismatched;
        }
        EXPECT_FALSE(ee.More() || er.More()); // both faces have the same edge occurrences
    }
    EXPECT_FALSE(fe.More() || fr.More()); // and both shapes the same faces
    EXPECT_GT(closedSeams, 0); // the hole walls; how many faces a boolean makes of them is OCCT's business
    EXPECT_GT(occurrences, 300);
    EXPECT_EQ(mismatched, 0);
    // And the consumer that motivated it: the ghost builds from a landed face.
    std::vector<float> ghost;
    EXPECT_TRUE(materializr::ghostPrismMesh(TopoDS::Face(TopExp_Explorer(shape, TopAbs_FACE).Current()),
                                            gp_Vec(0, 0, 5.0), ghost));
    EXPECT_FALSE(ghost.empty());
}

TEST(MeshWorker, ReportsFacesTheMesherCouldNotTriangulate) {
    // The caller uses the count to keep such a body on the synchronous path
    // (tessellate() Cleans and re-meshes it in the frame anyway).
    TopoDS_Compound c;
    BRep_Builder bb;
    bb.MakeCompound(c);
    bb.Add(c, BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());
    bb.Add(c, bowtieFace());
    MeshWorker worker;
    worker.request(2, c, kDefl, kAng);
    std::vector<MeshWorker::Result> results = waitAll(worker);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].unmeshedFaces, 1);
    EXPECT_EQ(results[0].faces.size(), 7u);
    EXPECT_EQ(MeshWorker::land(results[0]), 6); // the box faces still land
}

TEST(MeshWorker, LandsOnAMovedBody) {
    // The live shape carries a Location; triangulations live on the TShape in
    // local coordinates, so the result must not depend on where the body is.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    gp_Trsf t;
    t.SetTranslation(gp_Vec(100.0, 0.0, 50.0));
    TopoDS_Shape moved = box.Moved(TopLoc_Location(t));
    MeshWorker worker;
    worker.request(3, moved, kDefl, kAng);
    std::vector<MeshWorker::Result> results = waitAll(worker);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(MeshWorker::land(results[0]), 6);
    for (int n : triangleCounts(box)) EXPECT_EQ(n, 2); // the unmoved handle sees the same TShape
}

TEST(MeshWorker, AResultWithEveryFaceBareLandsNothing) {
    // The caller must then fall back to the in-frame path; see MeshDispatch.
    MeshWorker worker;
    worker.request(4, bowtieFace(), kDefl, kAng);
    std::vector<MeshWorker::Result> results = waitAll(worker);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].faces.size(), 1u);
    EXPECT_EQ(results[0].unmeshedFaces, 1);
    EXPECT_EQ(MeshWorker::land(results[0]), 0);
}

TEST(MeshWorker, NewestRequestForABodyWins) {
    // Hold the worker while three requests for one body queue up: only the
    // last one is meshed. pause() makes this deterministic; no mesh time is
    // used as a barrier.
    TopoDS_Shape a = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    TopoDS_Shape b = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    TopoDS_Shape c = BRepPrimAPI_MakeBox(30.0, 30.0, 30.0).Shape();
    MeshWorker worker;
    worker.pause();
    worker.request(2, a, kDefl, kAng);
    worker.request(2, b, kDefl, kAng);
    worker.request(2, c, kDefl, kAng);
    EXPECT_EQ(worker.pending(), 1u);
    worker.resume();
    std::vector<MeshWorker::Result> results = waitAll(worker);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].bodyId, 2);
    EXPECT_EQ(results[0].tshape, c.TShape().get());
    EXPECT_EQ(worker.pending(), 0u);
}
