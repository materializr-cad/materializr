// ghostPrismMesh() builds the push/pull ghost from the profile's own
// triangulation: two caps and one wall quad per boundary polygon segment,
// never the mesher.
#include "app/GhostMesh.h"
#include "core/MeshParams.h"

#include <gtest/gtest.h>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <vector>

using materializr::ghostPrismMesh;

namespace {

TopoDS_Face topFace(const TopoDS_Shape& s) {
    TopoDS_Face best;
    double bz = -1e9;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(e.Current(), g);
        if (g.CentreOfMass().Z() > bz) { bz = g.CentreOfMass().Z(); best = TopoDS::Face(e.Current()); }
    }
    return best;
}

// 2 caps of the profile's triangles + 2 triangles per polygon segment.
int expectedTriangles(const TopoDS_Face& f) {
    TopLoc_Location l;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(f, l);
    int n = 2 * tri->NbTriangles();
    for (TopExp_Explorer e(f, TopAbs_EDGE); e.More(); e.Next()) {
        Handle(Poly_PolygonOnTriangulation) p = BRep_Tool::PolygonOnTriangulation(TopoDS::Edge(e.Current()), tri, l);
        n += 2 * (p->NbNodes() - 1);
    }
    return n;
}

struct Stats { int tris = 0; double zmin = 1e9, zmax = -1e9; bool capNormalsRight = true; bool wallsHorizontal = true; bool windingAgrees = true; };
// zNear is the profile's plane, zFar the swept one; the near cap must face
// against the sweep and the far cap along it.
Stats stats(const std::vector<float>& v, double zNear, double zFar, double sweepZ) {
    Stats s;
    s.tris = static_cast<int>(v.size() / 18);
    // The shader flips the normal of a back-facing triangle, so each
    // triangle's winding must agree with the normal it carries.
    for (size_t i = 0; i + 17 < v.size(); i += 18) {
        const gp_Pnt a(v[i], v[i + 1], v[i + 2]), b(v[i + 6], v[i + 7], v[i + 8]), c(v[i + 12], v[i + 13], v[i + 14]);
        const gp_Vec n(v[i + 3], v[i + 4], v[i + 5]);
        if (gp_Vec(a, b).Crossed(gp_Vec(a, c)).Dot(n) <= 0.0) s.windingAgrees = false;
    }
    for (size_t i = 0; i + 5 < v.size(); i += 6) {
        const double z = v[i + 2], nz = v[i + 5];
        s.zmin = std::min(s.zmin, z);
        s.zmax = std::max(s.zmax, z);
        if (std::abs(nz) > 0.5) { // a cap vertex
            if (std::abs(z - zNear) < 1e-6 && nz * sweepZ > 0) s.capNormalsRight = false;
            if (std::abs(z - zFar) < 1e-6 && nz * sweepZ < 0) s.capNormalsRight = false;
        } else if (std::abs(nz) > 1e-6) {
            s.wallsHorizontal = false;
        }
    }
    return s;
}

} // namespace

TEST(GhostMesh, BoxTopFaceSweptUp) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape();
    BRepMesh_IncrementalMesh m(box, materializr::meshParams(0.1f, 0.3f, true));
    const TopoDS_Face top = topFace(box);
    std::vector<float> v;
    ASSERT_TRUE(ghostPrismMesh(top, gp_Vec(0, 0, 5.0), v));
    ASSERT_EQ(v.size() % 18, 0u);
    const Stats s = stats(v, 10.0, 15.0, 5.0);
    EXPECT_EQ(s.tris, expectedTriangles(top));
    EXPECT_NEAR(s.zmin, 10.0, 1e-6);
    EXPECT_NEAR(s.zmax, 15.0, 1e-6);
    EXPECT_TRUE(s.capNormalsRight);
    EXPECT_TRUE(s.wallsHorizontal);
    EXPECT_TRUE(s.windingAgrees);
}

TEST(GhostMesh, NegativeSweepGoesTheOtherWay) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape();
    BRepMesh_IncrementalMesh m(box, materializr::meshParams(0.1f, 0.3f, true));
    std::vector<float> v;
    ASSERT_TRUE(ghostPrismMesh(topFace(box), gp_Vec(0, 0, -4.0), v));
    const Stats s = stats(v, 10.0, 6.0, -4.0);
    EXPECT_NEAR(s.zmin, 6.0, 1e-6);
    EXPECT_NEAR(s.zmax, 10.0, 1e-6);
    EXPECT_TRUE(s.capNormalsRight); // the near cap (z=10) faces against the sweep, i.e. up
    EXPECT_TRUE(s.windingAgrees);
}

TEST(GhostMesh, AProfileWithAHoleWallsTheHoleToo) {
    TopoDS_Shape plate = BRepPrimAPI_MakeBox(40.0, 40.0, 10.0).Shape();
    TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 20, -1), gp_Dir(0, 0, 1)), 5.0, 12.0).Shape();
    TopoDS_Shape cut = BRepAlgoAPI_Cut(plate, hole).Shape();
    BRepMesh_IncrementalMesh m(cut, materializr::meshParams(0.1f, 0.3f, true));
    const TopoDS_Face top = topFace(cut);
    int edges = 0;
    for (TopExp_Explorer e(top, TopAbs_EDGE); e.More(); e.Next()) ++edges;
    ASSERT_EQ(edges, 5); // four sides and the hole
    std::vector<float> v;
    ASSERT_TRUE(ghostPrismMesh(top, gp_Vec(0, 0, 3.0), v));
    EXPECT_EQ(static_cast<int>(v.size() / 18), expectedTriangles(top));
}

TEST(GhostMesh, AMovedBodysFaceTakesTheFastPath) {
    // Every dragged body carries a Location; the polygon lookup composes the
    // face's and the edge's. It must resolve, or moved bodies would silently
    // fall back to the mesher every frame.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape();
    BRepMesh_IncrementalMesh m(box, materializr::meshParams(0.1f, 0.3f, true));
    gp_Trsf tr;
    tr.SetTranslation(gp_Vec(100.0, 50.0, 30.0));
    TopoDS_Shape moved = box.Moved(TopLoc_Location(tr));
    const TopoDS_Face top = topFace(moved);
    std::vector<float> v;
    ASSERT_TRUE(ghostPrismMesh(top, gp_Vec(0, 0, 5.0), v));
    const Stats s = stats(v, 40.0, 45.0, 5.0);
    EXPECT_EQ(s.tris, expectedTriangles(top));
    EXPECT_NEAR(s.zmin, 40.0, 1e-6);
    EXPECT_NEAR(s.zmax, 45.0, 1e-6);
    EXPECT_TRUE(s.capNormalsRight);
    double xmin = 1e9;
    for (size_t i = 0; i < v.size(); i += 6) xmin = std::min(xmin, static_cast<double>(v[i]));
    EXPECT_NEAR(xmin, 100.0, 1e-6); // world coordinates, not the box's local frame
}

TEST(GhostMesh, UnmeshedProfileGivesNothing) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape();
    std::vector<float> v;
    EXPECT_FALSE(ghostPrismMesh(topFace(box), gp_Vec(0, 0, 5.0), v));
    EXPECT_TRUE(v.empty());
}
