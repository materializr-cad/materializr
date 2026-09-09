// ResizeCylindricalOp (Edit Diameter) must confine its change to the resized
// segment. Shrinking one segment of a STEPPED rod used to pad the annular cut
// over the whole body, carving the [newR,oldR] shell out of a LARGER coaxial
// neighbour and turning it into a tube (Steve's gift-box bolt: an 11mm end
// resized to 10.8 hollowed the adjacent 13mm section to an ID-11mm tube).
#include <gtest/gtest.h>

#include "core/Document.h"
#include "modeling/ResizeCylindricalOp.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>

using namespace materializr;

namespace {
bool solidAt(const TopoDS_Shape& s, double x, double y, double z) {
    BRepClass3d_SolidClassifier c(s, gp_Pnt(x, y, z), 1e-6);
    return c.State() == TopAbs_IN;
}
} // namespace

TEST(ResizeCylindrical, ShrinkEndKeepsLargerNeighbourSolid) {
    // 13mm section (r6.5) [0,20] + 11mm end (r5.5) [20,32], one solid.
    TopoDS_Shape big = BRepPrimAPI_MakeCylinder(6.5, 20.0).Shape();
    TopoDS_Shape end = BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(0, 0, 20), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)),
        5.5, 12.0).Shape();
    TopoDS_Shape rod = BRepAlgoAPI_Fuse(big, end).Shape();
    Document doc;
    int id = doc.addBody(rod, "rod");

    ResizeCylindricalOp op;
    op.setBody(id);
    op.setAxis(gp_Ax2(gp_Pnt(0, 0, 20), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    op.setHeight(12.0);
    op.setOldRadii(5.5, 5.5);
    op.setNewRadii(5.4, 5.4);   // 11mm -> 10.8mm
    op.setIsHole(false);
    ASSERT_TRUE(op.execute(doc));
    TopoDS_Shape res = doc.getBody(id);
    ASSERT_FALSE(res.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());

    // The 11mm end IS shrunk to r5.4: solid just inside, void just outside.
    EXPECT_TRUE(solidAt(res, 5.35, 0, 26)) << "end core removed";
    EXPECT_FALSE(solidAt(res, 5.45, 0, 26)) << "end not shrunk";
    // The 13mm neighbour stays a SOLID rod - no internal void ring.
    EXPECT_TRUE(solidAt(res, 5.45, 0, 10))
        << "neighbour hollowed into a tube (the bug)";
    EXPECT_TRUE(solidAt(res, 0.0, 0, 10)) << "neighbour core gone";
    EXPECT_TRUE(solidAt(res, 6.0, 0, 10)) << "neighbour wall gone";
}

TEST(ResizeCylindrical, ShrinkStandaloneRodStillWorks) {
    // A plain rod (both ends free flat caps) must still shrink cleanly - the
    // free-end padding path.
    TopoDS_Shape rod = BRepPrimAPI_MakeCylinder(5.5, 20.0).Shape();
    Document doc;
    int id = doc.addBody(rod, "rod");
    ResizeCylindricalOp op;
    op.setBody(id);
    op.setAxis(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
    op.setHeight(20.0);
    op.setOldRadii(5.5, 5.5);
    op.setNewRadii(5.4, 5.4);
    op.setIsHole(false);
    ASSERT_TRUE(op.execute(doc));
    TopoDS_Shape res = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(res).IsValid());
    EXPECT_TRUE(solidAt(res, 5.35, 0, 10));
    EXPECT_FALSE(solidAt(res, 5.45, 0, 10));
}
