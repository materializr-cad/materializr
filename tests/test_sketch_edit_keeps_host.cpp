// Replaying a sketch's edits must not detach it from the body it is drawn on (#185).
//
// A reopened project's sketch-edit snapshots carry no host link (the loader reads it for the
// live sketch only), and SketchEditOp copied the whole snapshot over the live sketch, so the first
// replay of a model - the cascade after one Move - wrote "no host" over every sketch drawn on a
// body. A second Move then left that sketch behind, and the features built from it (a push/pull,
// the chamfers after it) failed to follow and were switched off.

#include "core/Document.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"

#include <gtest/gtest.h>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <memory>

using namespace materializr;

TEST(SketchEditKeepsHost, ExecuteAndUndoLeaveTheHostLinkAlone) {
    Document doc;
    auto live = std::make_shared<Sketch>();
    live->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 20), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    const int sid = doc.addSketch(live, "Sketch 2");
    live->setSourceBody(7);

    // Snapshots as a reopened file gives them: geometry, but no host.
    auto before = std::make_shared<Sketch>();
    auto after = std::make_shared<Sketch>();
    before->setPlane(live->getPlane());
    after->setPlane(live->getPlane());
    after->addRectangle({-3, -3}, {3, 3});
    ASSERT_EQ(after->getSourceBody(), -1);

    SketchEditOp op(live, before, after);
    ASSERT_TRUE(op.execute(doc));
    EXPECT_EQ(live->getSourceBody(), 7) << "execute() wiped the host link";
    EXPECT_FALSE(live->getPoints().empty()) << "and the edit itself still applied";

    ASSERT_TRUE(op.undo(doc));
    EXPECT_EQ(live->getSourceBody(), 7) << "undo() wiped the host link";
    EXPECT_TRUE(live->getPoints().empty());
    (void)sid;
}

TEST(SketchEditKeepsHost, ADetachedSketchStaysDetached) {
    Document doc;
    auto live = std::make_shared<Sketch>();
    doc.addSketch(live, "S");
    live->setSourceBody(3);
    live->setDetachedFromBody(true);
    auto before = std::make_shared<Sketch>();
    auto after = std::make_shared<Sketch>();
    after->addRectangle({0, 0}, {2, 2});
    SketchEditOp op(live, before, after);
    ASSERT_TRUE(op.execute(doc));
    EXPECT_TRUE(live->isDetachedFromBody());
    EXPECT_EQ(live->getSourceBody(), 3);
}
