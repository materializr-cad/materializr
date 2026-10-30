// Construction planes / axes across a replay and a reload: a hidden one stays
// hidden, and its id - which history steps name it by - survives the file.
//
// Any upstream sketch edit replays the history from the first affected step,
// which undoes and re-executes a later Construction Plane step: undo removed the
// plane and execute re-added a fresh, VISIBLE one, so a plane the user had
// unticked popped back after every edit to the first sketch (tester report).

#include "core/Document.h"
#include "modeling/ConstructionAxisOp.h"
#include "modeling/ConstructionPlaneOp.h"
#include "io/ProjectIO.h"
#include "test_tmp_path.h"

#include <gtest/gtest.h>

TEST(ConstructionVisibility, HiddenPlaneStaysHiddenThroughUndoAndRedo) {
    Document doc;
    ConstructionPlaneOp op;
    op.setType(PlaneCreationType::XZ);
    ASSERT_TRUE(op.execute(doc));
    ASSERT_EQ(doc.getAllPlaneIds().size(), 1u);
    const int id = doc.getAllPlaneIds().front();
    EXPECT_TRUE(doc.isPlaneVisible(id)) << "a new plane is shown";

    doc.setPlaneVisible(id, false);
    ASSERT_TRUE(op.undo(doc));
    EXPECT_EQ(doc.getAllPlaneIds().size(), 0u);
    ASSERT_TRUE(op.execute(doc));
    ASSERT_NE(doc.getPlane(id), nullptr) << "re-added under the same id";
    EXPECT_FALSE(doc.isPlaneVisible(id)) << "the replay un-hid the plane";

    doc.setPlaneVisible(id, true);
    ASSERT_TRUE(op.undo(doc));
    ASSERT_TRUE(op.execute(doc));
    EXPECT_TRUE(doc.isPlaneVisible(id)) << "showing it again sticks too";
}

TEST(ConstructionVisibility, HiddenAxisStaysHiddenThroughUndoAndRedo) {
    Document doc;
    ConstructionAxisOp op;
    ASSERT_TRUE(op.execute(doc));
    ASSERT_EQ(doc.getAllAxisIds().size(), 1u);
    const int id = doc.getAllAxisIds().front();

    doc.setAxisVisible(id, false);
    ASSERT_TRUE(op.undo(doc));
    ASSERT_TRUE(op.execute(doc));
    ASSERT_NE(doc.getAxis(id), nullptr);
    EXPECT_FALSE(doc.isAxisVisible(id)) << "the replay un-hid the axis";
}

// Plane ids survive a save + load, and a file from before that (its plane came
// back renumbered) is repaired by the Construction Plane step on reload.
TEST(ConstructionIds, PlaneAndAxisKeepTheirIdsThroughSaveAndLoad) {
    Document doc;
    gp_Pln pln(gp_Pnt(0, 0, 20), gp_Dir(0, 0, 1));
    doc.addPlane(pln, "Spare");                       // id 1
    const int pid = doc.addPlane(pln, "XZ Plane", 4); // id 4, as in the report
    const int aid = doc.addAxis(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0), "Spin", 7);
    doc.setPlaneVisible(pid, false);
    const std::string path = mzrtest::tmpPath("mtz_plane_ids.materializr");
    ASSERT_TRUE(materializr::ProjectIO::save(path, doc).success);

    Document back;
    ASSERT_TRUE(materializr::ProjectIO::load(path, back).success);
    ASSERT_NE(back.getPlane(pid), nullptr) << "plane 4 must load as plane 4";
    EXPECT_EQ(back.getPlaneName(pid), "XZ Plane");
    EXPECT_FALSE(back.isPlaneVisible(pid));
    ASSERT_NE(back.getAxis(aid), nullptr) << "axis 7 must load as axis 7";
    EXPECT_EQ(back.addPlane(pln, "Next"), 5) << "new ids continue past the loaded ones";
}

TEST(ConstructionIds, ReloadRepairsAPlaneRenumberedByAnOlderLoad) {
    // The step says plane 4; the document (loaded by an older build) has the
    // same plane as id 1.
    ConstructionPlaneOp op;
    ASSERT_TRUE(op.deserializeParams(
        "id=4;type=1;px=0;py=0;pz=20;nx=0;ny=0;nz=1;xx=1;xy=0;xz=-0;name=XZ Plane"));
    Document doc;
    const int loaded = doc.addPlane(gp_Pln(gp_Pnt(0, 0, 35), gp_Dir(0, 0, 1)), "XZ Plane");
    ASSERT_EQ(loaded, 1);
    doc.setPlaneVisible(loaded, false);

    ASSERT_TRUE(op.rehydrateFromReload(Operation::ReloadState{}, doc))
        << "the step must reload editable, not frozen";
    EXPECT_EQ(doc.getPlane(1), nullptr);
    ASSERT_NE(doc.getPlane(4), nullptr);
    EXPECT_FALSE(doc.isPlaneVisible(4)) << "the renumber keeps the plane as it was";
    EXPECT_NEAR(doc.getPlane(4)->plane.Location().Z(), 35.0, 1e-9)
        << "and keeps its moved position";

    // Two planes with the step's name: can't tell which - stays frozen.
    ConstructionPlaneOp op2;
    ASSERT_TRUE(op2.deserializeParams(
        "id=9;type=1;px=0;py=0;pz=0;nx=0;ny=0;nz=1;xx=1;xy=0;xz=0;name=Twin"));
    Document twins;
    twins.addPlane(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), "Twin");
    twins.addPlane(gp_Pln(gp_Pnt(0, 0, 5), gp_Dir(0, 0, 1)), "Twin");
    EXPECT_FALSE(op2.rehydrateFromReload(Operation::ReloadState{}, twins));
}
