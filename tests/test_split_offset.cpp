// Split Body with an OFF-CENTRE plane (#split-dialog work).
//
// SplitBodyOp always accepted an arbitrary gp_Pln, but every caller handed it a
// plane through the body's bounding-box centre - the three Split X/Y/Z buttons
// had no way to say anything else. The new Split dialog offers an offset, so
// the off-centre case stops being theoretical: these pin that the two halves
// come out where the plane actually is, and that undo puts the body back.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "modeling/SplitBodyOp.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <vector>

namespace {

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

// A 20mm cube at the origin; its centre is (10,10,10).
int makeCube(Document& doc) {
    return doc.addBody(BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape(), "Cube");
}

} // namespace

TEST(SplitOffset, CentrePlaneHalvesTheBody) {
    Document doc;
    const int id = makeCube(doc);

    SplitBodyOp op;
    op.setBody(id);
    op.setSplitPlane(gp_Pln(gp_Pnt(10, 10, 10), gp_Dir(1, 0, 0)));
    ASSERT_TRUE(op.execute(doc));

    const int other = op.getSecondBodyId();
    ASSERT_GE(other, 0);
    EXPECT_NEAR(volumeOf(doc.getBody(id)),    4000.0, 1.0);
    EXPECT_NEAR(volumeOf(doc.getBody(other)), 4000.0, 1.0);
}

TEST(SplitOffset, OffsetPlaneCutsWhereItIsPut) {
    Document doc;
    const int id = makeCube(doc);

    // 5mm past the centre along X: pieces of 15mm and 5mm depth.
    SplitBodyOp op;
    op.setBody(id);
    op.setSplitPlane(gp_Pln(gp_Pnt(15, 10, 10), gp_Dir(1, 0, 0)));
    ASSERT_TRUE(op.execute(doc));

    const int other = op.getSecondBodyId();
    ASSERT_GE(other, 0);
    std::vector<double> vols{volumeOf(doc.getBody(id)), volumeOf(doc.getBody(other))};
    std::sort(vols.begin(), vols.end());
    EXPECT_NEAR(vols[0], 2000.0, 1.0);   // 5 x 20 x 20
    EXPECT_NEAR(vols[1], 6000.0, 1.0);   // 15 x 20 x 20
}

TEST(SplitOffset, PlaneClearOfTheBodyDoesNothing) {
    Document doc;
    const int id = makeCube(doc);
    const size_t before = doc.getAllBodyIds().size();

    // Well outside the cube. The dialog clamps the offset inside the body for
    // exactly this reason - a miss must not add a history step that produced
    // no second body.
    SplitBodyOp op;
    op.setBody(id);
    op.setSplitPlane(gp_Pln(gp_Pnt(500, 10, 10), gp_Dir(1, 0, 0)));
    const bool ok = op.execute(doc);
    if (ok) {
        // If the kernel accepted it, it must at least not have invented a body.
        EXPECT_EQ(doc.getAllBodyIds().size(), before);
        EXPECT_NEAR(volumeOf(doc.getBody(id)), 8000.0, 1.0);
    } else {
        EXPECT_EQ(doc.getAllBodyIds().size(), before);
    }
}

TEST(SplitOffset, UndoRestoresTheWholeBody) {
    Document doc;
    const int id = makeCube(doc);
    const size_t before = doc.getAllBodyIds().size();

    SplitBodyOp op;
    op.setBody(id);
    op.setSplitPlane(gp_Pln(gp_Pnt(12, 10, 10), gp_Dir(0, 1, 0)));
    ASSERT_TRUE(op.execute(doc));
    ASSERT_EQ(doc.getAllBodyIds().size(), before + 1);

    ASSERT_TRUE(op.undo(doc));
    EXPECT_EQ(doc.getAllBodyIds().size(), before);
    EXPECT_NEAR(volumeOf(doc.getBody(id)), 8000.0, 1.0);
}

TEST(SplitOffset, ParamsRoundTrip) {
    Document doc;
    const int id = makeCube(doc);

    SplitBodyOp op;
    op.setBody(id);
    op.setSplitPlane(gp_Pln(gp_Pnt(15, 10, 10), gp_Dir(1, 0, 0)));
    ASSERT_TRUE(op.execute(doc));

    // A reloaded step has to cut in the same place, or replaying a saved
    // project quietly moves the cut back to the middle.
    SplitBodyOp reloaded;
    ASSERT_TRUE(reloaded.deserializeParams(op.serializeParams()));
    EXPECT_EQ(reloaded.getBodyId(), id);

    Document doc2;
    const int id2 = makeCube(doc2);
    ASSERT_EQ(id2, id);
    ASSERT_TRUE(reloaded.execute(doc2));
    std::vector<double> vols{volumeOf(doc2.getBody(id2)),
                             volumeOf(doc2.getBody(reloaded.getSecondBodyId()))};
    std::sort(vols.begin(), vols.end());
    EXPECT_NEAR(vols[0], 2000.0, 1.0);
    EXPECT_NEAR(vols[1], 6000.0, 1.0);
}
