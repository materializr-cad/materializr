// Shrinking a hole that exits through a NEAR-COPLANAR SEAM.
//
// Found on an imported-and-edited STEP nacelle: shrinking 4mm holes to 3.4mm
// left a hair-thin tube standing proud of the surface. Dissecting the part
// showed cylindrical faces at r = 2.010 - a radius nothing in the design
// produces, but exactly ResizeCylindricalOp's oldR + kRadialPad - bounded by
// two planes whose normals were (0,-1,0) and (-0.007,1,0). Antiparallel to
// within 0.007 rad: ONE flat surface split into two faces 0.4 degrees apart,
// which is #81 geometry.
//
// So the hole's rim sits at two slightly different heights. m_height is a
// single number (the bore face's V range), the fused ring is built to the full
// span, and it pokes past the LOWER of the two caps - leaving its own padded
// wall standing in free air. Band height on the part was 0.0140mm; 2.010 *
// 0.007 = 0.01407. That is the whole bug.
//
// The cure is not in ResizeCylindricalOp - it is to stop the cap being two
// faces. These pin that: merge the caps first and the shrink comes out clean.
//
// Deliberately NOT asserting that the unmerged case still leaves a band. That
// is today's behaviour, not a contract; if the resize op is ever hardened to
// handle a split cap directly, this test should keep passing. What it pins is
// the relationship - merging never makes it worse - plus the absolute promise
// that after a merge there is nothing left behind.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "modeling/MergeFacesOp.h"
#include "modeling/ResizeCylindricalOp.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <vector>

namespace {

constexpr double kTiltRad  = 0.007;  // the seam angle measured on the nacelle
constexpr double kOldR     = 2.0;
constexpr double kNewR     = 1.7;
constexpr double kPadR     = 2.01;   // oldR + ResizeCylindricalOp's kRadialPad
constexpr double kThick    = 10.0;

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

int facesAtRadius(const TopoDS_Shape& s, double r, double tol = 1e-4) {
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepAdaptor_Surface sa(TopoDS::Face(ex.Current()));
        if (sa.GetType() == GeomAbs_Cylinder &&
            std::abs(sa.Cylinder().Radius() - r) < tol) ++n;
    }
    return n;
}

// Planar faces pointing up - the cap(s) the bore exits through.
std::vector<TopoDS_Shape> topCaps(const TopoDS_Shape& s) {
    std::vector<TopoDS_Shape> out;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepAdaptor_Surface sa(TopoDS::Face(ex.Current()));
        if (sa.GetType() != GeomAbs_Plane) continue;
        gp_Dir n = sa.Plane().Axis().Direction();
        if (TopoDS::Face(ex.Current()).Orientation() == TopAbs_REVERSED) n.Reverse();
        if (n.Z() > 0.99) out.push_back(ex.Current());
    }
    return out;
}

// Two blocks fused RAW (no unify, so the seam survives) with the right one
// tilted a hair, and a 4mm hole drilled straight down through the seam.
TopoDS_Shape seamBlockWithHole() {
    const TopoDS_Shape left =
        BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10, 20, kThick).Shape();
    TopoDS_Shape right =
        BRepPrimAPI_MakeBox(gp_Pnt(10, 0, 0), 10, 20, kThick).Shape();
    gp_Trsf t;
    t.SetRotation(gp_Ax1(gp_Pnt(10, 0, kThick), gp_Dir(0, 1, 0)), kTiltRad);
    right = BRepBuilderAPI_Transform(right, t, Standard_True).Shape();

    BRepAlgoAPI_Fuse fuse(left, right);
    fuse.Build();
    TopoDS_Shape body = fuse.Shape();

    TopoDS_Shape drill = BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(10, 10, -5), gp_Dir(0, 0, 1)), kOldR, 30.0).Shape();
    BRepAlgoAPI_Cut hole(body, drill);
    hole.Build();
    return hole.Shape();
}

// 4mm -> 3.4mm on the bore. Returns false if the op refused.
bool shrinkBore(Document& doc, int id) {
    TopoDS_Face bore;
    for (TopExp_Explorer ex(doc.getBody(id), TopAbs_FACE); ex.More(); ex.Next()) {
        BRepAdaptor_Surface sa(TopoDS::Face(ex.Current()));
        if (sa.GetType() == GeomAbs_Cylinder &&
            std::abs(sa.Cylinder().Radius() - kOldR) < 1e-6) {
            bore = TopoDS::Face(ex.Current());
            break;
        }
    }
    if (bore.IsNull()) return false;
    Standard_Real u0, u1, v0, v1;
    BRepTools::UVBounds(bore, u0, u1, v0, v1);

    ResizeCylindricalOp op;
    op.setBody(id);
    op.setAxis(gp_Ax2(gp_Pnt(10, 10, -5.0 + v0), gp_Dir(0, 0, 1)));
    op.setHeight(v1 - v0);
    op.setOldRadii(kOldR, kOldR);
    op.setNewRadii(kNewR, kNewR);
    op.setIsHole(true);
    return op.execute(doc);
}

} // namespace

TEST(ShrinkOverSeam, FixtureReallyHasASeam) {
    // If the raw fuse ever starts unifying these two faces on its own, the rest
    // of this file stops testing anything - so say so loudly here rather than
    // passing vacuously.
    const TopoDS_Shape body = seamBlockWithHole();
    EXPECT_EQ(topCaps(body).size(), 2u);
    EXPECT_EQ(facesAtRadius(body, kOldR), 1);
}

TEST(ShrinkOverSeam, MergingTheCapsFirstLeavesNothingBehind) {
    Document doc;
    const int id = doc.addBody(seamBlockWithHole(), "seam");

    MergeFacesOp mf;
    mf.setBody(id);
    mf.setFaces(topCaps(doc.getBody(id)));
    ASSERT_TRUE(mf.execute(doc)) << "the 0.007 rad seam should be inside the "
                                    "face-scoped tolerance ladder";
    ASSERT_EQ(topCaps(doc.getBody(id)).size(), 1u);

    const double before = volumeOf(doc.getBody(id));
    ASSERT_TRUE(shrinkBore(doc, id));
    const TopoDS_Shape after = doc.getBody(id);

    // The whole point: no padded-radius wall left standing.
    EXPECT_EQ(facesAtRadius(after, kPadR), 0);
    EXPECT_TRUE(BRepCheck_Analyzer(after).IsValid());

    // And it actually shrank the hole rather than doing nothing: the added
    // material is the annulus between the two radii, through the block.
    const double expected = M_PI * (kOldR * kOldR - kNewR * kNewR) * kThick;
    EXPECT_NEAR(volumeOf(after) - before, expected, 0.15 * expected);
    EXPECT_EQ(facesAtRadius(after, kNewR), 1);
}

TEST(ShrinkOverSeam, MergingNeverMakesItWorse) {
    // Relationship, not a snapshot of today's defect. Right now the unmerged
    // shrink leaves a band and the merged one does not; if the resize op is
    // ever taught to handle a split cap itself, both go to zero and this still
    // holds. What must never happen is merging making the result dirtier.
    int leftoverPlain = -1, leftoverMerged = -1;

    {
        Document doc;
        const int id = doc.addBody(seamBlockWithHole(), "plain");
        ASSERT_TRUE(shrinkBore(doc, id));
        leftoverPlain = facesAtRadius(doc.getBody(id), kPadR);
        // Whatever it leaves behind, it must still be a sound solid.
        EXPECT_TRUE(BRepCheck_Analyzer(doc.getBody(id)).IsValid());
    }
    {
        Document doc;
        const int id = doc.addBody(seamBlockWithHole(), "merged");
        MergeFacesOp mf;
        mf.setBody(id);
        mf.setFaces(topCaps(doc.getBody(id)));
        ASSERT_TRUE(mf.execute(doc));
        ASSERT_TRUE(shrinkBore(doc, id));
        leftoverMerged = facesAtRadius(doc.getBody(id), kPadR);
    }

    EXPECT_LE(leftoverMerged, leftoverPlain);
    EXPECT_EQ(leftoverMerged, 0);
}
