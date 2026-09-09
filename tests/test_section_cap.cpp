// Section View clips the mesh in the fragment shader; without a cap a solid
// reads as a hollow shell. computeSectionCap() slices the body's triangulation
// (the mesh the viewport draws) and fills the loops as a planar face. These
// tests mesh every shape the way the renderer does at Medium quality first.
#include "viewport/SectionCap.h"
#include "core/MeshParams.h"

#include <gtest/gtest.h>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Surface.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <gp_Trsf.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <vector>

using materializr::computeSectionCap;
using materializr::faceMeshes;
using materializr::SectionSlice;
using materializr::sliceSection;

namespace {

void meshLikeRenderer(const TopoDS_Shape& s) {
    BRepMesh_IncrementalMesh m(s, materializr::meshParams(0.1, 0.3, true));
}

double capArea(const std::vector<float>& p) {
    double area = 0.0;
    for (size_t i = 0; i + 9 <= p.size(); i += 9) {
        const double ux = p[i + 3] - p[i], uy = p[i + 4] - p[i + 1], uz = p[i + 5] - p[i + 2];
        const double vx = p[i + 6] - p[i], vy = p[i + 7] - p[i + 1], vz = p[i + 8] - p[i + 2];
        const double nx = uy * vz - uz * vy;
        const double ny = uz * vx - ux * vz;
        const double nz = ux * vy - uy * vx;
        area += 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
    }
    return area;
}

double lineLength(const std::vector<float>& l) {
    double len = 0.0;
    for (size_t i = 0; i + 6 <= l.size(); i += 6) {
        const double dx = l[i + 3] - l[i], dy = l[i + 4] - l[i + 1], dz = l[i + 5] - l[i + 2];
        len += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return len;
}

// Every cap vertex lies on the plane z = height.
void expectOnPlane(const std::vector<float>& pos, float height) {
    for (size_t i = 0; i + 2 < pos.size(); i += 3)
        EXPECT_NEAR(pos[i + 2], height, 1e-3f);
}

TopoDS_Shape boredBox() {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    gp_Ax2 axis(gp_Pnt(10, 10, 0), gp_Dir(0, 0, 1));
    TopoDS_Shape bore = BRepPrimAPI_MakeCylinder(axis, 5.0, 20.0).Shape();
    return BRepAlgoAPI_Cut(box, bore).Shape();
}

} // namespace

TEST(SectionCap, SolidBoxCapEqualsCrossSection) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    meshLikeRenderer(box);
    std::vector<float> pos;
    ASSERT_TRUE(computeSectionCap(box, gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), pos));
    ASSERT_FALSE(pos.empty());
    expectOnPlane(pos, 10.0f);
    EXPECT_NEAR(capArea(pos), 400.0, 1e-6);
}

TEST(SectionCap, HollowBoxCapIsAnnulus) {
    TopoDS_Shape hollow = boredBox();
    meshLikeRenderer(hollow);
    std::vector<float> pos;
    ASSERT_TRUE(computeSectionCap(hollow, gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), pos));
    // The bore is a polygon at the mesh's resolution (about 21 sides at
    // 0.1 mm / 0.3 rad), so the hole comes out ~1.2 mm^2 smaller than the circle.
    EXPECT_NEAR(capArea(pos), 400.0 - M_PI * 25.0, 2.0);
}

TEST(SectionCap, IslandInsideAHoleIsFilled) {
    // A pin standing inside the bore: outer square, hole, and a material loop
    // at nesting depth two. Area = square - bore + pin.
    TopoDS_Shape hollow = boredBox();
    gp_Ax2 axis(gp_Pnt(10, 10, 0), gp_Dir(0, 0, 1));
    TopoDS_Shape pin = BRepPrimAPI_MakeCylinder(axis, 2.0, 20.0).Shape();
    TopoDS_Shape shape = BRepAlgoAPI_Fuse(hollow, pin).Shape();
    meshLikeRenderer(shape);
    std::vector<float> pos;
    ASSERT_TRUE(computeSectionCap(shape, gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), pos));
    EXPECT_NEAR(capArea(pos), 400.0 - M_PI * 25.0 + M_PI * 4.0, 2.0);
}

TEST(SectionCap, RowOfHolesCutThroughTheirCentresIsManyRegions) {
    // The plane through a row of hole centres leaves 21 separate rectangles.
    TopoDS_Shape plate = BRepPrimAPI_MakeBox(300.0, 40.0, 10.0).Shape();
    TopoDS_Compound holes;
    BRep_Builder bb;
    bb.MakeCompound(holes);
    for (int i = 0; i < 20; ++i)
        bb.Add(holes, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(7.5 + i * 15.0, 20.0, -1.0), gp_Dir(0, 0, 1)), 3.0, 12.0).Shape());
    TopoDS_Shape shape = BRepAlgoAPI_Cut(plate, holes).Shape();
    meshLikeRenderer(shape);
    std::vector<float> pos;
    ASSERT_TRUE(computeSectionCap(shape, gp_Pln(gp_Pnt(0, 20, 0), gp_Dir(0, 1, 0)), pos));
    // Each hole edge is a polygon of the mesh; where the plane crosses a chord
    // rather than a node the hole reads up to R(1-cos(pi/N)) narrower, so the
    // area lands a little above the exact 1800 (about 3 mm^2 at Medium).
    EXPECT_NEAR(capArea(pos), 300.0 * 10.0 - 20 * 6.0 * 10.0, 20.0);
}

TEST(SectionCap, MovedBodyIsCutWhereItStands) {
    // Same TShape carrying a Location: the slice must use the placed nodes.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    meshLikeRenderer(box);
    gp_Trsf t;
    t.SetTranslation(gp_Vec(100.0, 0.0, 50.0));
    TopoDS_Shape moved = box.Moved(TopLoc_Location(t));
    std::vector<float> pos;
    ASSERT_TRUE(computeSectionCap(moved, gp_Pln(gp_Pnt(0, 0, 60), gp_Dir(0, 0, 1)), pos));
    expectOnPlane(pos, 60.0f);
    EXPECT_NEAR(capArea(pos), 400.0, 1e-6);
    for (size_t i = 0; i < pos.size(); i += 3) EXPECT_GE(pos[i], 99.9f);
    EXPECT_FALSE(computeSectionCap(moved, gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), pos));
}

TEST(SectionCap, VerticesInOneSnapCellStayDistinct) {
    // Two boxes whose facing corners are 0.8 snap cells apart on both axes:
    // the corners share a grid cell yet sit 1.13 tolerances apart, so each
    // must keep its own id. A grid that held one vertex per cell dropped the
    // second corner, that box's loop broke where it recurred, and only one
    // rectangle was filled.
    TopoDS_Shape a = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(5.00001, 3.00001, 20)).Shape();
    TopoDS_Shape b = BRepPrimAPI_MakeBox(gp_Pnt(5.00009, 3.00009, 0), gp_Pnt(10, 6, 20)).Shape();
    TopoDS_Compound both;
    BRep_Builder bb;
    bb.MakeCompound(both);
    bb.Add(both, a);
    bb.Add(both, b);
    meshLikeRenderer(both);
    std::vector<float> pos;
    ASSERT_TRUE(computeSectionCap(both, gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), pos));
    EXPECT_NEAR(capArea(pos), 5.00001 * 3.00001 + (10 - 5.00009) * (6 - 3.00009), 1e-6);
}

TEST(SectionCap, SubToleranceSegmentsKeepTheLoopClosed) {
    // A prism whose outline has an edge 1.13 tolerances long. Its sliver side
    // face is two triangles, so the plane crosses it as two half-segments of
    // 0.56 tolerances each. Dropping those by length before snapping left the
    // loop open at that edge and the cap vanished.
    const double xy[6][2] = {{0, 0}, {10, 0}, {10, 3}, {4.99999, 2.99999}, {4.99991, 2.99991}, {0, 3}};
    BRepBuilderAPI_MakePolygon poly;
    double twiceArea = 0.0;
    for (int i = 0; i < 6; ++i) {
        poly.Add(gp_Pnt(xy[i][0], xy[i][1], 0.0));
        const int j = (i + 1) % 6;
        twiceArea += xy[i][0] * xy[j][1] - xy[j][0] * xy[i][1];
    }
    poly.Close();
    TopoDS_Shape prism = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(poly.Wire()).Face(), gp_Vec(0, 0, 20)).Shape();
    meshLikeRenderer(prism);
    std::vector<float> pos;
    ASSERT_TRUE(computeSectionCap(prism, gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), pos));
    // The notch is a tenth of the snap tolerance deep, so the collinear merge
    // may flatten it: the area is the outline's to within that notch.
    EXPECT_NEAR(capArea(pos), 0.5 * twiceArea, 1e-3);
}

TEST(SectionCap, UnmeshedShapeGivesNoCap) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    std::vector<float> pos;
    EXPECT_FALSE(computeSectionCap(box, gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), pos));
    EXPECT_TRUE(pos.empty());
}

TEST(SectionCap, NoIntersectionNoCap) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    meshLikeRenderer(box);
    std::vector<float> pos;
    EXPECT_FALSE(computeSectionCap(box, gp_Pln(gp_Pnt(0, 0, 30), gp_Dir(0, 0, 1)), pos));
    EXPECT_TRUE(pos.empty());
}

TEST(SectionCap, TangentPlaneNoCap) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    meshLikeRenderer(box);
    std::vector<float> pos;
    EXPECT_FALSE(computeSectionCap(box, gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), pos));
    EXPECT_TRUE(pos.empty());
    EXPECT_FALSE(computeSectionCap(box, gp_Pln(gp_Pnt(0, 0, 20), gp_Dir(0, 0, 1)), pos));
    EXPECT_TRUE(pos.empty());
}

TEST(SectionSlice, LinesFollowEveryLoop) {
    // The outline is every edge of every loop: the square's four sides and
    // the bore's polygon (a 21-gon at Medium, 31.3 mm around).
    TopoDS_Shape hollow = boredBox();
    meshLikeRenderer(hollow);
    SectionSlice slice;
    ASSERT_TRUE(sliceSection(faceMeshes(hollow), gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), slice));
    ASSERT_FALSE(slice.cap.empty());
    expectOnPlane(slice.lines, 10.0f);
    EXPECT_NEAR(lineLength(slice.lines), 80.0 + 2.0 * M_PI * 5.0, 0.5);
}

TEST(SectionSlice, ABareInnerWallGivesTheOutlineButNoCap) {
    // Strip the bore's cylindrical wall: the outer loop still closes, and a
    // fill would seal the bore solid. Outline only until the body is whole.
    TopoDS_Shape hollow = boredBox();
    meshLikeRenderer(hollow);
    int stripped = 0;
    for (TopExp_Explorer e(hollow, TopAbs_FACE); e.More(); e.Next()) {
        TopoDS_Face face = TopoDS::Face(e.Current());
        if (BRep_Tool::Surface(face)->IsKind(STANDARD_TYPE(Geom_CylindricalSurface))) {
            BRep_Builder().UpdateFace(face, Handle(Poly_Triangulation)());
            ++stripped;
        }
    }
    ASSERT_EQ(stripped, 1);
    SectionSlice slice;
    EXPECT_TRUE(sliceSection(faceMeshes(hollow), gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), slice));
    EXPECT_TRUE(slice.cap.empty());
    EXPECT_NEAR(lineLength(slice.lines), 80.0, 1e-6); // the outer square only
}

TEST(SectionSlice, AMeshEdgeLyingInThePlaneIsDrawnOnce) {
    // Two triangles share the edge AB, which lies in the cutting plane, both
    // with their third node below it; a fifth node above makes the body
    // straddle. Each triangle produces AB, and a duplicate would close a
    // two-vertex "loop" that gets dropped, taking the edge with it.
    Handle(Poly_Triangulation) tri = new Poly_Triangulation(5, 3, Standard_False);
    tri->SetNode(1, gp_Pnt(0, 0, 0));
    tri->SetNode(2, gp_Pnt(10, 0, 0));
    tri->SetNode(3, gp_Pnt(5, 5, -5));
    tri->SetNode(4, gp_Pnt(5, -5, -5));
    tri->SetNode(5, gp_Pnt(5, 0, 5));
    tri->SetTriangle(1, Poly_Triangle(1, 2, 3));
    tri->SetTriangle(2, Poly_Triangle(2, 1, 4));
    tri->SetTriangle(3, Poly_Triangle(1, 2, 5));
    std::vector<materializr::FaceMesh> faces{{tri, Bnd_Box(), gp_Trsf(), false}};
    SectionSlice slice;
    EXPECT_TRUE(sliceSection(faces, gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), slice));
    EXPECT_NEAR(lineLength(slice.lines), 10.0, 1e-6);
    EXPECT_TRUE(slice.cap.empty());
}

TEST(SectionSlice, ABareFaceAwayFromThePlaneStillAllowsTheCap) {
    // The top face is bare, the cut is through the middle: no gap in this
    // slice, so the cap is filled as if the body were whole.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    meshLikeRenderer(box);
    for (TopExp_Explorer e(box, TopAbs_FACE); e.More(); e.Next()) {
        TopoDS_Face face = TopoDS::Face(e.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        bool atZMax = true;
        for (int i = 1; i <= tri->NbNodes(); ++i) atZMax = atZMax && tri->Node(i).Z() > 19.9;
        if (atZMax) BRep_Builder().UpdateFace(face, Handle(Poly_Triangulation)());
    }
    SectionSlice slice;
    EXPECT_TRUE(sliceSection(faceMeshes(box), gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), slice));
    EXPECT_NEAR(capArea(slice.cap), 400.0, 1e-6);
}

TEST(SectionSlice, ABareFaceWithNoUsableBoxCountsAsAGap) {
    // A bare face whose box could not be computed (SetWhole) or is void must
    // read as "gap everywhere", never as "complete": the cap of an otherwise
    // whole box is suppressed in both cases.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    meshLikeRenderer(box);
    const gp_Pln plane(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1));
    {
        std::vector<materializr::FaceMesh> faces = faceMeshes(box);
        materializr::FaceMesh bare{Handle(Poly_Triangulation)(), Bnd_Box(), gp_Trsf(), false};
        bare.box.SetWhole();
        faces.push_back(bare);
        SectionSlice slice;
        EXPECT_TRUE(sliceSection(faces, plane, slice));
        EXPECT_TRUE(slice.cap.empty());
        EXPECT_NEAR(lineLength(slice.lines), 80.0, 1e-6);
    }
    {
        std::vector<materializr::FaceMesh> faces = faceMeshes(box);
        faces.push_back({Handle(Poly_Triangulation)(), Bnd_Box(), gp_Trsf(), false}); // void box
        SectionSlice slice;
        EXPECT_TRUE(sliceSection(faces, plane, slice));
        EXPECT_TRUE(slice.cap.empty());
        EXPECT_NEAR(lineLength(slice.lines), 80.0, 1e-6);
    }
}

TEST(SectionSlice, UnmeshedFaceStillDrawsTheRest) {
    // Strip one side face's triangulation: the loop cannot close, so there is
    // no cap, but the three meshed sides still draw their outline.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
    meshLikeRenderer(box);
    for (TopExp_Explorer e(box, TopAbs_FACE); e.More(); e.Next()) {
        TopoDS_Face face = TopoDS::Face(e.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        bool atXMax = true;
        for (int i = 1; i <= tri->NbNodes(); ++i) atXMax = atXMax && tri->Node(i).X() > 19.9;
        if (atXMax) BRep_Builder().UpdateFace(face, Handle(Poly_Triangulation)());
    }
    SectionSlice slice;
    EXPECT_TRUE(sliceSection(faceMeshes(box), gp_Pln(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), slice));
    EXPECT_TRUE(slice.cap.empty());
    EXPECT_NEAR(lineLength(slice.lines), 60.0, 1e-6);
}
