// #153: mirror a body about the world origin planes. The popup only offered the
// plane on the body's own bounding-box side, so a part and its twin either side
// of the origin took a manual move afterwards.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/History.h"
#include "modeling/MirrorOp.h"
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>

namespace {
Bnd_Box box(const TopoDS_Shape& s) { Bnd_Box b; BRepBndLib::Add(s, b); return b; }
}

TEST(MirrorOrigin, YzPlaneMirrorsAcrossXEqualsZeroAndKeepsTheOriginal) {
    Document doc;
    History hist;
    const int id = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(10, 0, 0), 10, 5, 5).Shape(), "Box");
    auto op = std::make_unique<MirrorOp>();
    op->setBody(id);
    op->setPlane(MirrorPlane::YZ);
    op->setKeepOriginal(true);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    ASSERT_EQ(doc.getAllBodyIds().size(), 2u);

    double x0, y0, z0, x1, y1, z1;
    box(doc.getBody(id)).Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x0, 10.0, 1e-6);
    int twin = -1;
    for (int b : doc.getAllBodyIds()) if (b != id) twin = b;
    box(doc.getBody(twin)).Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x0, -20.0, 1e-6);
    EXPECT_NEAR(x1, -10.0, 1e-6);
    EXPECT_NEAR(y1, 5.0, 1e-6) << "only X flips";

    ASSERT_TRUE(hist.undo(doc));
    EXPECT_EQ(doc.getAllBodyIds().size(), 1u);
}
