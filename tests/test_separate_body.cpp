// Separate: a body holding disconnected solids (air-gapped lumps fused into
// one body object - the boolean-remnant bug Steve hit) splits into one body
// per lump, largest keeping the source. Also checks the menu gate (solidCount)
// and clean undo/redo.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/History.h"
#include "modeling/SeparateBodyOp.h"

#include <BRep_Builder.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

using namespace materializr;

namespace {
double vol(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}
// A compound of three air-gapped boxes: one large (main) + two small strays,
// mimicking the light cover's b22 (main + two edge remnants).
TopoDS_Shape threeLumpBody() {
    TopoDS_Compound comp;
    BRep_Builder b;
    b.MakeCompound(comp);
    b.Add(comp, BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10, 10, 10).Shape()); // 1000
    b.Add(comp, BRepPrimAPI_MakeBox(gp_Pnt(50, 0, 0), 2, 2, 2).Shape());   // 8
    b.Add(comp, BRepPrimAPI_MakeBox(gp_Pnt(-50, 0, 0), 3, 3, 3).Shape());  // 27
    return comp;
}
} // namespace

TEST(SeparateBody, SolidCountGate) {
    EXPECT_EQ(SeparateBodyOp::solidCount(
                  BRepPrimAPI_MakeBox(1.0, 1.0, 1.0).Shape()),
              1);
    EXPECT_EQ(SeparateBodyOp::solidCount(threeLumpBody()), 3);
    EXPECT_EQ(SeparateBodyOp::solidCount(TopoDS_Shape()), 0);
}

TEST(SeparateBody, SplitsIntoOneBodyPerLump) {
    Document doc;
    History hist;
    int id = doc.addBody(threeLumpBody(), "lump");
    const double total = vol(doc.getBody(id));

    ASSERT_TRUE(hist.pushOperation(std::make_unique<SeparateBodyOp>(),
                                   doc) == false)
        << "guard: op with no body set must not push";
    // Real op.
    auto op = std::make_unique<SeparateBodyOp>();
    op->setBody(id);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));

    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 3u) << "three lumps -> three bodies";
    // Source keeps the LARGEST lump (the 1000 box).
    EXPECT_NEAR(vol(doc.getBody(id)), 1000.0, 1e-6);
    // Each resulting body is a single solid; total volume conserved.
    double sum = 0;
    for (int b : ids) {
        EXPECT_EQ(SeparateBodyOp::solidCount(doc.getBody(b)), 1)
            << "body " << b << " still compound after separate";
        sum += vol(doc.getBody(b));
    }
    EXPECT_NEAR(sum, total, 1e-6);
}

TEST(SeparateBody, UndoRestoresSingleBody) {
    Document doc;
    History hist;
    int id = doc.addBody(threeLumpBody(), "lump");
    const double total = vol(doc.getBody(id));

    auto op = std::make_unique<SeparateBodyOp>();
    op->setBody(id);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    ASSERT_EQ(doc.getAllBodyIds().size(), 3u);

    ASSERT_TRUE(hist.undo(doc));
    EXPECT_EQ(doc.getAllBodyIds().size(), 1u) << "undo removes the split bodies";
    EXPECT_EQ(SeparateBodyOp::solidCount(doc.getBody(id)), 3);
    EXPECT_NEAR(vol(doc.getBody(id)), total, 1e-6);

    // Redo splits again.
    ASSERT_TRUE(hist.redo(doc));
    EXPECT_EQ(doc.getAllBodyIds().size(), 3u) << "redo re-splits";
}

TEST(SeparateBody, RefusesSingleSolid) {
    Document doc;
    int id = doc.addBody(BRepPrimAPI_MakeBox(5.0, 5.0, 5.0).Shape(), "solid");
    SeparateBodyOp op;
    op.setBody(id);
    EXPECT_FALSE(op.execute(doc)) << "nothing to separate on a single solid";
}
