// Deleting a sketch goes through History (DeleteSketchOp) so it can be undone:
// undo restores the same Sketch under the same id with its name and visibility.

#include "core/Document.h"
#include "core/History.h"
#include "modeling/Sketch.h"
#include "modeling/DeleteSketchOp.h"
#include "modeling/DeleteConstructionOp.h"
#include "modeling/OperationFactory.h"
#include <BRepPrimAPI_MakeBox.hxx>

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <memory>

using materializr::Sketch;

TEST(DeleteSketch, UndoRestoresSameIdNameVisibility) {
    Document doc;
    History hist;
    auto sk = std::make_shared<Sketch>();
    int a = sk->addPoint(glm::vec2(0, 0));
    int b = sk->addPoint(glm::vec2(10, 0));
    sk->addLine(a, b);
    int id = doc.addSketch(sk, "Mine");
    doc.setSketchVisible(id, false);

    auto op = std::make_unique<DeleteSketchOp>();
    op->setSketchId(id);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    EXPECT_EQ(doc.getSketch(id), nullptr);

    ASSERT_TRUE(hist.undo(doc));
    EXPECT_EQ(doc.getSketch(id), sk);
    EXPECT_EQ(doc.getSketchName(id), "Mine");
    EXPECT_FALSE(doc.isSketchVisible(id));

    ASSERT_TRUE(hist.redo(doc));
    EXPECT_EQ(doc.getSketch(id), nullptr);
    ASSERT_TRUE(hist.undo(doc));
    EXPECT_EQ(doc.getSketch(id), sk);
}

TEST(DeleteSketch, MissingSketchRefused) {
    Document doc;
    DeleteSketchOp op;
    op.setSketchId(42);
    EXPECT_FALSE(op.execute(doc));
}

// 11-3-26: undoing a sketch delete put the sketch at the END of the Items list.
TEST(DeleteSketch, UndoRestoresTheSketchToItsSlot) {
    Document doc;
    History hist;
    int ids[3];
    for (int i = 0; i < 3; ++i) {
        auto sk = std::make_shared<Sketch>();
        sk->addPoint(glm::vec2(float(i), 0));
        ids[i] = doc.addSketch(sk, "S" + std::to_string(i));
    }
    auto op = std::make_unique<DeleteSketchOp>();
    op->setSketchId(ids[0]);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    ASSERT_TRUE(hist.undo(doc));
    EXPECT_EQ(doc.getAllSketchIds(), (std::vector<int>{ids[0], ids[1], ids[2]}));
}

// 11-3-26: "also delete the bodies built from it" took two Undos to bring back -
// the sketch first, each body on a later one. It is one step now.
TEST(DeleteSketch, BodiesDeletedWithTheSketchComeBackInOneUndo) {
    Document doc;
    History hist;
    auto sk = std::make_shared<Sketch>();
    sk->addPoint(glm::vec2(0, 0));
    const int sid = doc.addSketch(sk, "Mine");
    const int body = doc.addBody(BRepPrimAPI_MakeBox(10, 10, 10).Shape(), "Box");

    auto op = std::make_unique<DeleteSketchOp>();
    op->setSketchId(sid);
    op->addBodyDelete(body);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    EXPECT_EQ(doc.getSketch(sid), nullptr);
    EXPECT_TRUE(doc.getAllBodyIds().empty());
    EXPECT_EQ(hist.stepCount(), 1);

    ASSERT_TRUE(hist.undo(doc));
    EXPECT_EQ(doc.getSketch(sid), sk);
    EXPECT_EQ(doc.getAllBodyIds(), std::vector<int>{body});

    ASSERT_TRUE(hist.redo(doc));
    EXPECT_TRUE(doc.getAllBodyIds().empty());
}

// ... and they come back in the order they were in, not reversed.
TEST(DeleteSketch, BodiesComeBackInTheirOriginalOrder) {
    Document doc;
    History hist;
    auto sk = std::make_shared<Sketch>();
    sk->addPoint(glm::vec2(0, 0));
    const int sid = doc.addSketch(sk, "Mine");
    const int b1 = doc.addBody(BRepPrimAPI_MakeBox(10, 10, 10).Shape(), "A");
    const int b2 = doc.addBody(BRepPrimAPI_MakeBox(5, 5, 5).Shape(), "B");

    auto op = std::make_unique<DeleteSketchOp>();
    op->setSketchId(sid);
    op->addBodyDelete(b1);
    op->addBodyDelete(b2);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    ASSERT_TRUE(hist.undo(doc));
    EXPECT_EQ(doc.getAllBodyIds(), (std::vector<int>{b1, b2}));
}

// 11-3-26: Delete on a construction plane wrote straight to the Document, so
// Ctrl+Z undid the step BEFORE it and a Redo then re-created the plane.
TEST(DeleteConstruction, PlaneDeleteUndoRestoresIdNameVisibility) {
    Document doc;
    History hist;
    const int id = doc.addPlane(gp_Pln(gp_Pnt(5, 0, 0), gp_Dir(1, 0, 0)), "Side");
    doc.setPlaneVisible(id, false);

    ASSERT_TRUE(deleteConstructionEntity(&hist, doc, false, id));
    EXPECT_EQ(doc.getPlane(id), nullptr);

    ASSERT_TRUE(hist.undo(doc));
    ASSERT_NE(doc.getPlane(id), nullptr);
    EXPECT_EQ(doc.getPlaneName(id), "Side");
    EXPECT_FALSE(doc.isPlaneVisible(id));
    EXPECT_NEAR(doc.getPlane(id)->plane.Location().X(), 5.0, 1e-9);

    ASSERT_TRUE(hist.redo(doc));
    EXPECT_EQ(doc.getPlane(id), nullptr);
}

TEST(DeleteConstruction, AxisDeleteUndoRestores) {
    Document doc;
    History hist;
    const int id = doc.addAxis(gp_Pnt(1, 2, 3), gp_Dir(0, 0, 1), "Spin");
    ASSERT_TRUE(deleteConstructionEntity(&hist, doc, true, id));
    EXPECT_EQ(doc.getAxis(id), nullptr);
    ASSERT_TRUE(hist.undo(doc));
    ASSERT_NE(doc.getAxis(id), nullptr);
    EXPECT_EQ(doc.getAxisName(id), "Spin");
}

// A step that comes back from a saved project can still undo: the entity is gone
// from the saved list, so the blob has to carry it.
TEST(DeleteConstruction, SurvivesSerializeRoundTrip) {
    Document doc;
    const int id = doc.addPlane(gp_Pln(gp_Pnt(0, 7, 0), gp_Dir(0, 1, 0)), "Back; plate=1");
    DeleteConstructionOp op;
    op.setPlaneId(id);
    ASSERT_TRUE(op.execute(doc));
    const std::string blob = op.serializeParams();

    auto again = OperationFactory::create("delete_construction");
    ASSERT_NE(again, nullptr);
    ASSERT_TRUE(again->deserializeParams(blob));
    Operation::ReloadState rs;
    ASSERT_TRUE(again->rehydrateFromReload(rs, doc));
    ASSERT_TRUE(again->undo(doc));
    ASSERT_NE(doc.getPlane(id), nullptr);
    EXPECT_EQ(doc.getPlaneName(id), "Back; plate=1");
    EXPECT_NEAR(doc.getPlane(id)->plane.Location().Y(), 7.0, 1e-9);
}
