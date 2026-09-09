// A multi-body gizmo transform ("Move/Rotate/Scale N bodies") must reload as a
// REAL op that re-applies to the LIVE bodies - not a baked snapshot that
// re-slams stale geometry over an upstream edit on replay (the "batchtransform
// bakes" bug: a repair beneath a threaded body reverted once the step re-landed).
#include "modeling/BatchTransformOp.h"
#include "modeling/OperationFactory.h"
#include "core/Document.h"
#include "core/Operation.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Trsf.hxx>
#include <memory>

using namespace materializr;

namespace {
void bbox(const TopoDS_Shape& s, double& x0, double& x1) {
    Bnd_Box b; BRepBndLib::Add(s, b);
    double y0,z0,y1,z1; b.Get(x0,y0,z0,x1,y1,z1);
}
gp_GTrsf translationX(double dx) {
    gp_Trsf t; t.SetTranslation(gp_Vec(dx, 0, 0));
    return gp_GTrsf(t);
}
} // namespace

// A batch transform is registered in the factory and round-trips through
// serialize/deserialize, then re-applies to whatever the bodies CURRENTLY are.
TEST(BatchTransformReload, ReAppliesToLiveGeometryAfterReload) {
    Document doc;
    int a = doc.addBody(BRepPrimAPI_MakeBox(10, 10, 10).Shape(), "A");
    int b = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 20, 0), 10, 10, 10).Shape(), "B");

    // Move both +40 in X.
    BatchTransformOp op;
    op.setBodies({a, b});
    op.setTransform(translationX(40.0));
    op.setLabel("Move (2 bodies)");
    ASSERT_TRUE(op.execute(doc));
    double x0, x1;
    bbox(doc.getBody(a), x0, x1);
    EXPECT_NEAR(x0, 40.0, 1e-6) << "A moved +40";
    bbox(doc.getBody(b), x0, x1);
    EXPECT_NEAR(x0, 40.0, 1e-6) << "B moved +40";

    // The factory must know the type (else it reloads as a baked ReplayOp).
    auto fresh = OperationFactory::create("batchtransform");
    ASSERT_TRUE(fresh != nullptr) << "batchtransform must be registered";

    // Round-trip the parameters.
    std::string blob = op.serializeParams();
    ASSERT_TRUE(fresh->deserializeParams(blob));

    // Rehydrate against the pre-op state (what editStep rolls back to).
    Operation::ReloadState rs;
    rs.modifiedBefore.push_back({a, BRepPrimAPI_MakeBox(10, 10, 10).Shape()});
    rs.modifiedBefore.push_back({b, BRepPrimAPI_MakeBox(gp_Pnt(0,20,0),10,10,10).Shape()});
    ASSERT_TRUE(fresh->rehydrateFromReload(rs, doc));

    // THE FIX: an upstream edit changed body A's geometry (a taller box). Roll
    // the doc back to that EDITED pre-transform state and replay the batch op -
    // it must transform the EDITED geometry, not restore a stale snapshot.
    doc.updateBody(a, BRepPrimAPI_MakeBox(10, 10, 30).Shape()); // A is now taller
    doc.updateBody(b, BRepPrimAPI_MakeBox(gp_Pnt(0,20,0),10,10,10).Shape());
    ASSERT_TRUE(fresh->execute(doc)) << "reloaded batch op must re-run";

    // A moved +40 AND kept its edited height (30, not the original 10).
    Bnd_Box bb; BRepBndLib::Add(doc.getBody(a), bb);
    double ax0,ay0,az0,ax1,ay1,az1; bb.Get(ax0,ay0,az0,ax1,ay1,az1);
    EXPECT_NEAR(ax0, 40.0, 1e-6) << "edited A still moved +40 on replay";
    EXPECT_NEAR(az1, 30.0, 1e-6)
        << "the transform applied to the EDITED geometry (height 30), "
           "not a baked snapshot (would be 10)";
}

// Undo restores every body it moved.
TEST(BatchTransformReload, UndoRestoresAllBodies) {
    Document doc;
    int a = doc.addBody(BRepPrimAPI_MakeBox(10, 10, 10).Shape(), "A");
    int b = doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 20, 0), 10, 10, 10).Shape(), "B");
    BatchTransformOp op;
    op.setBodies({a, b});
    op.setTransform(translationX(40.0));
    ASSERT_TRUE(op.execute(doc));
    ASSERT_TRUE(op.undo(doc));
    double x0, x1;
    bbox(doc.getBody(a), x0, x1); EXPECT_NEAR(x0, 0.0, 1e-6);
    bbox(doc.getBody(b), x0, x1); EXPECT_NEAR(x0, 0.0, 1e-6);
}
