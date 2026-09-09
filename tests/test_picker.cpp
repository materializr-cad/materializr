// Hover picking runs Picker::pick on every rendered frame the cursor rests on
// a body. It used to call BRepMesh_IncrementalMesh(shape, 0.1) each time "to
// make sure the shape is tessellated" - but the mesher rebuilds its whole data
// model per call even when it changes nothing (9 ms on a 54-face part, 66 ms
// on a 1683-face part), and at Low quality (0.5 mm) it actually RE-meshed the
// body finer than the renderer asked for. These tests pin the fix: the picker
// reuses the renderer's triangulation and snaps the hit onto the exact surface.

#include "core/Document.h"
#include "viewport/Camera.h"
#include "viewport/Picker.h"

#include <gtest/gtest.h>

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>

#include <vector>

using materializr::Camera;
using materializr::Picker;
using materializr::PickResult;

namespace {

constexpr float kW = 800.0f;
constexpr float kH = 600.0f;

// Mesh the way the renderer does at Low quality (0.5 mm linear deflection).
void meshLikeRendererLow(const TopoDS_Shape& s) {
    BRepMesh_IncrementalMesh m(s, materializr::meshParams(0.5, 0.5, true));
}

// One entry per face: the Poly_Triangulation it carries (null if none).
std::vector<const void*> triangulations(const TopoDS_Shape& s) {
    std::vector<const void*> v;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) {
        TopLoc_Location l;
        v.push_back(BRep_Tool::Triangulation(TopoDS::Face(e.Current()), l).get());
    }
    return v;
}

Camera framing(const TopoDS_Shape& s) {
    Bnd_Box bb;
    BRepBndLib::Add(s, bb);
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    Camera cam;
    cam.setAspect(kW / kH);
    cam.zoomToFit(glm::vec3(x0, y0, z0), glm::vec3(x1, y1, z1));
    return cam;
}

PickResult pickCentre(const TopoDS_Shape& s, Document& doc) {
    Camera cam = framing(s);
    Picker picker;
    return picker.pick(kW * 0.5f, kH * 0.5f, kW, kH, cam, doc);
}

} // namespace

TEST(Picker, ReusesTheRenderersTriangulation) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 10.0).Shape();
    meshLikeRendererLow(box);
    const auto before = triangulations(box);
    ASSERT_FALSE(before.empty());
    for (const void* t : before) ASSERT_NE(t, nullptr);

    Document doc;
    const int id = doc.addBody(box, "plate");
    PickResult r = pickCentre(box, doc);
    ASSERT_TRUE(r.hit);
    EXPECT_EQ(r.bodyId, id);

    // The pick must not have touched the mesh: same triangulation objects.
    EXPECT_EQ(triangulations(box), before);
}

TEST(Picker, DoesNotMeshABodyTheRendererNeverMeshed) {
    // A body the renderer could not tessellate is not drawn, so the picker
    // must neither hit it nor try to mesh it (that retry would run the
    // mesher every hovered frame).
    TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 10.0).Shape();
    for (const void* t : triangulations(box)) ASSERT_EQ(t, nullptr);

    Document doc;
    doc.addBody(box, "plate");
    PickResult r = pickCentre(box, doc);
    EXPECT_FALSE(r.hit);
    for (const void* t : triangulations(box)) EXPECT_EQ(t, nullptr);
}

TEST(Picker, RepeatedHoverPicksAgree) {
    // The hovered-frame path caches each body's edge polylines; a cache hit
    // must return exactly what the cold pick returned.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 10.0).Shape();
    meshLikeRendererLow(box);
    Document doc;
    doc.addBody(box, "plate");
    Camera cam = framing(box);
    Picker picker;
    PickResult a = picker.pick(kW * 0.4f, kH * 0.45f, kW, kH, cam, doc);
    PickResult b = picker.pick(kW * 0.4f, kH * 0.45f, kW, kH, cam, doc);
    ASSERT_TRUE(a.hit);
    ASSERT_FALSE(a.nearestEdge.IsNull());
    EXPECT_TRUE(a.nearestEdge.IsSame(b.nearestEdge));
    EXPECT_FLOAT_EQ(a.edgeScreenDist, b.edgeScreenDist);
}

TEST(Picker, NearestEdgeFollowsABodyThatMoved) {
    // Same TShape, new Location: a polyline cache keyed on the TShape alone
    // would keep reporting edges at the old position. The moved box spans
    // z in [20, 30]; the original spans [0, 10].
    TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 10.0).Shape();
    meshLikeRendererLow(box);
    Document doc;
    const int id = doc.addBody(box, "plate");
    Picker picker;
    PickResult a = picker.pick(kW * 0.5f, kH * 0.5f, kW, kH, framing(box), doc);
    ASSERT_TRUE(a.hit);
    ASSERT_FALSE(a.nearestEdge.IsNull());

    gp_Trsf t;
    t.SetTranslation(gp_Vec(0.0, 0.0, 20.0));
    TopoDS_Shape moved = BRepBuilderAPI_Transform(box, t, /*copy=*/false).Shape();
    ASSERT_TRUE(moved.IsPartner(box));
    doc.updateBody(id, moved);

    PickResult b = picker.pick(kW * 0.5f, kH * 0.5f, kW, kH, framing(moved), doc);
    ASSERT_TRUE(b.hit);
    ASSERT_FALSE(b.nearestEdge.IsNull());
    double zMin = 1e9;
    for (TopExp_Explorer v(b.nearestEdge, TopAbs_VERTEX); v.More(); v.Next())
        zMin = std::min(zMin, BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).Z());
    EXPECT_GE(zMin, 19.9);
}

TEST(Picker, BodyMovedIntoViewIsHit) {
    // Same TShape, new Location far from the old one, camera re-framed on
    // the new position. A bounding box cached on the TShape alone would
    // still describe the old position, cull the ray and report a miss.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 10.0).Shape();
    meshLikeRendererLow(box);
    Document doc;
    const int id = doc.addBody(box, "plate");
    Picker picker;
    PickResult a = picker.pick(kW * 0.5f, kH * 0.5f, kW, kH, framing(box), doc);
    ASSERT_TRUE(a.hit);

    gp_Trsf t;
    t.SetTranslation(gp_Vec(1000.0, 0.0, 0.0));
    TopoDS_Shape moved = BRepBuilderAPI_Transform(box, t, /*copy=*/false).Shape();
    ASSERT_TRUE(moved.IsPartner(box));
    doc.updateBody(id, moved);

    PickResult b = picker.pick(kW * 0.5f, kH * 0.5f, kW, kH, framing(moved), doc);
    EXPECT_TRUE(b.hit);
    EXPECT_GE(b.hitPoint.x, 999.0f);
}

TEST(Picker, HitPointIsSnappedToTheExactSurface) {
    // A sphere: every hit lands on a curved face, where a 0.5 mm chord mesh
    // puts the ray/triangle intersection visibly off the true surface. The
    // measure tool consumes hitPoint, so it must lie ON the surface.
    TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(10.0).Shape();
    meshLikeRendererLow(sphere);

    Document doc;
    doc.addBody(sphere, "ball");
    PickResult r = pickCentre(sphere, doc);
    ASSERT_TRUE(r.hit);
    EXPECT_NEAR(glm::length(r.hitPoint), 10.0f, 1e-3f);
}

TEST(Picker, UnchangedInputsAreAnsweredFromTheLastResult) {
    // Hover calls pick() every rendered frame; an idle cursor must not walk
    // the document again, and any input pick() reads changing must.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 10.0).Shape();
    meshLikeRendererLow(box);
    Document doc;
    doc.addBody(box, "plate");
    Camera cam = framing(box);
    Picker picker;
    PickResult a = picker.pick(kW * 0.5f, kH * 0.5f, kW, kH, cam, doc);
    ASSERT_TRUE(a.hit);
    EXPECT_FALSE(picker.lastPickWasCached());
    PickResult b = picker.pick(kW * 0.5f, kH * 0.5f, kW, kH, cam, doc);
    EXPECT_TRUE(picker.lastPickWasCached());
    EXPECT_EQ(a.bodyId, b.bodyId);
    EXPECT_EQ(a.faceIndex, b.faceIndex);
    EXPECT_EQ(a.hitPoint, b.hitPoint);

    picker.pick(kW * 0.5f + 1.0f, kH * 0.5f, kW, kH, cam, doc);
    EXPECT_FALSE(picker.lastPickWasCached()); // cursor moved
    picker.pick(kW * 0.5f + 1.0f, kH * 0.5f, kW, kH, cam, doc);
    EXPECT_TRUE(picker.lastPickWasCached());
    cam.orbit(5.0f, 0.0f);
    picker.pick(kW * 0.5f + 1.0f, kH * 0.5f, kW, kH, cam, doc);
    EXPECT_FALSE(picker.lastPickWasCached()); // camera moved
    picker.invalidate();
    picker.pick(kW * 0.5f + 1.0f, kH * 0.5f, kW, kH, cam, doc);
    EXPECT_FALSE(picker.lastPickWasCached()); // re-mesh reported by the app
}

TEST(Picker, DocumentChangesRunThePickAgain) {
    // Hiding, showing or moving a body, or adding a construction plane,
    // changes what pick() reads, so the previous result must not be reused.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 10.0).Shape();
    meshLikeRendererLow(box);
    Document doc;
    const int id = doc.addBody(box, "plate");
    Camera cam = framing(box);
    Picker picker;
    auto pick = [&] { return picker.pick(kW * 0.5f, kH * 0.5f, kW, kH, cam, doc); };
    ASSERT_TRUE(pick().hit);
    pick();
    ASSERT_TRUE(picker.lastPickWasCached());

    doc.setBodyVisible(id, false);
    EXPECT_FALSE(pick().hit);
    EXPECT_FALSE(picker.lastPickWasCached());
    doc.setBodyVisible(id, true);
    EXPECT_TRUE(pick().hit);
    EXPECT_FALSE(picker.lastPickWasCached());

    gp_Trsf t;
    t.SetTranslation(gp_Vec(0.0, 0.0, 20.0));
    doc.updateBody(id, BRepBuilderAPI_Transform(box, t, false).Shape());
    pick();
    EXPECT_FALSE(picker.lastPickWasCached());
    pick();
    ASSERT_TRUE(picker.lastPickWasCached());

    const int pid = doc.addPlane(gp_Pln(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), "p");
    pick();
    EXPECT_FALSE(picker.lastPickWasCached());
    doc.setPlaneVisible(pid, false);
    pick();
    EXPECT_FALSE(picker.lastPickWasCached());
}
