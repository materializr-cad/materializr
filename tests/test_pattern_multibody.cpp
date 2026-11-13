// A pattern over several selected bodies (bug report 11-13-26: Multiply tools
// only acted on the first selected body). Each source gets its own run of
// copies, in one history step, and the extra sources survive save/reload.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/History.h"
#include "modeling/PatternOp.h"
#include <BRepPrimAPI_MakeBox.hxx>

TEST(PatternMultiBody, EverySourceGetsItsOwnCopiesInOneStep) {
    Document doc;
    History hist;
    const int a = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 5, 5, 5).Shape(), "A");
    const int b = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 20, 0), 5, 5, 5).Shape(), "B");

    auto op = std::make_unique<PatternOp>();
    op->setBodies({a, b});
    op->setType(PatternType::Linear);
    op->setCount(3);
    op->setLinearSpacing(10, 0, 0);
    PatternOp* raw = op.get();
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));

    // 2 sources x (3 - 1) copies, on top of the 2 originals.
    EXPECT_EQ(doc.getAllBodyIds().size(), 6u);
    EXPECT_EQ(raw->getCreatedBodyIds().size(), 4u);

    ASSERT_TRUE(hist.undo(doc));
    EXPECT_EQ(doc.getAllBodyIds().size(), 2u);
    ASSERT_TRUE(hist.redo(doc));
    EXPECT_EQ(doc.getAllBodyIds().size(), 6u);
}

TEST(PatternMultiBody, SingleBodySerialisesExactlyAsBefore) {
    PatternOp one;
    one.setBodies({7});
    EXPECT_EQ(one.serializeParams().find("extra="), std::string::npos);
}

TEST(PatternMultiBody, ExtraSourcesRoundTripThroughSerialisation) {
    PatternOp op;
    op.setBodies({3, 5, 9});
    op.setCount(4);
    PatternOp back;
    ASSERT_TRUE(back.deserializeParams(op.serializeParams()));
    EXPECT_EQ(back.getBodyIds(), (std::vector<int>{3, 5, 9}));
    EXPECT_EQ(back.getCount(), 4);
}
