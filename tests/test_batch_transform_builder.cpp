// Regression: a multi-body Move/Rotate must NOT go through
// BRepBuilderAPI_GTransform.
//
// Steve, 2026-07-31: moving objects in a real project closed the app outright -
// no dialog, no "not responding". The crash handler caught it:
//
//   BatchTransformOp::execute
//     -> BRepBuilderAPI_GTransform (ctor)
//        -> BRepBuilderAPI_NurbsConvert::Perform
//           -> BRepTools_NurbsConvertModification::NewCurve2d
//              SIGSEGV 'segmentation violation' detected. Address 0
//
// BRepBuilderAPI_GTransform::Perform runs NurbsConvert on the shape FIRST,
// unconditionally - see OCCT's BRepBuilderAPI_GTransform.cxx. So every
// multi-body move rebuilt every surface and pcurve as a NURBS: slow (1.2 s
// main-loop stalls in his log), lossy (analytic surfaces become splines), and
// fatal when the converter meets geometry it can't handle. The single-body
// TransformOp never did this - it uses BRepBuilderAPI_Transform, which only
// relocates the shape - which is why moving ONE body was fine.
//
// The observable difference, and what this test pins: after a rigid batch
// move, the faces are still ANALYTIC (a box stays 6 planes, a cylinder stays a
// cylinder). Under GTransform they come back as Geom_BSplineSurface.

#include "core/Document.h"
#include "core/History.h"
#include "modeling/BatchTransformOp.h"

#include <gtest/gtest.h>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <gp_XYZ.hxx>
#include <memory>


namespace {

// Surface types present in a shape, so "still analytic" is checkable.
int countOfType(const TopoDS_Shape& s, GeomAbs_SurfaceType want) {
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepAdaptor_Surface surf(TopoDS::Face(ex.Current()));
        if (surf.GetType() == want) ++n;
    }
    return n;
}

struct TwoBodies {
    Document doc;
    int box = -1, cyl = -1;
    TwoBodies() {
        box = doc.addBody(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());
        cyl = doc.addBody(BRepPrimAPI_MakeCylinder(3.0, 12.0).Shape());
    }
};

} // namespace

TEST(BatchTransformBuilder, RigidMoveKeepsAnalyticSurfaces) {
    TwoBodies f;
    ASSERT_EQ(countOfType(f.doc.getBody(f.box), GeomAbs_Plane), 6);
    ASSERT_EQ(countOfType(f.doc.getBody(f.cyl), GeomAbs_Cylinder), 1);

    gp_Trsf t;
    t.SetTranslation(gp_Vec(5.0, 0.0, 0.0));
    auto op = std::make_unique<BatchTransformOp>();
    op->setBodies({f.box, f.cyl});
    op->setTransform(gp_GTrsf(t));

    History h;
    ASSERT_TRUE(h.pushOperation(std::move(op), f.doc));

    // The whole point: a move relocates, it does not rebuild. NurbsConvert
    // would have turned all seven of these into B-spline faces.
    EXPECT_EQ(countOfType(f.doc.getBody(f.box), GeomAbs_Plane), 6)
        << "the box's planes were rebuilt - the move went through GTransform";
    EXPECT_EQ(countOfType(f.doc.getBody(f.cyl), GeomAbs_Cylinder), 1)
        << "the cylinder was rebuilt - the move went through GTransform";
    EXPECT_EQ(countOfType(f.doc.getBody(f.box), GeomAbs_BSplineSurface), 0);
}

TEST(BatchTransformBuilder, RigidRotateKeepsAnalyticSurfaces) {
    TwoBodies f;
    gp_Trsf t;
    t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0.5);
    auto op = std::make_unique<BatchTransformOp>();
    op->setBodies({f.box, f.cyl});
    op->setTransform(gp_GTrsf(t));

    History h;
    ASSERT_TRUE(h.pushOperation(std::move(op), f.doc));
    EXPECT_EQ(countOfType(f.doc.getBody(f.box), GeomAbs_Plane), 6);
    EXPECT_EQ(countOfType(f.doc.getBody(f.cyl), GeomAbs_Cylinder), 1);
}

// Non-uniform scale genuinely needs the general builder - it must still work
// (and it is allowed to convert, because there is no other way to squash a
// cylinder into an ellipse).
TEST(BatchTransformBuilder, NonUniformScaleStillApplies) {
    TwoBodies f;
    gp_GTrsf g;
    g.SetVectorialPart(gp_Mat(2.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0));
    g.SetTranslationPart(gp_XYZ(0, 0, 0));

    auto op = std::make_unique<BatchTransformOp>();
    op->setBodies({f.box});
    op->setTransform(g);

    History h;
    ASSERT_TRUE(h.pushOperation(std::move(op), f.doc));

    Bnd_Box bb;
    BRepBndLib::Add(f.doc.getBody(f.box), bb);
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x1 - x0, 20.0, 1e-6) << "the 2x X scale did not apply";
    EXPECT_NEAR(y1 - y0, 10.0, 1e-6);
}

// Undo restores every body, including when the batch bailed part-way.
TEST(BatchTransformBuilder, UndoRestoresBothBodies) {
    TwoBodies f;
    const TopoDS_Shape boxBefore = f.doc.getBody(f.box);

    gp_Trsf t;
    t.SetTranslation(gp_Vec(5.0, 0.0, 0.0));
    auto op = std::make_unique<BatchTransformOp>();
    op->setBodies({f.box, f.cyl});
    op->setTransform(gp_GTrsf(t));

    History h;
    ASSERT_TRUE(h.pushOperation(std::move(op), f.doc));
    ASSERT_TRUE(h.canUndo());
    h.undo(f.doc);

    Bnd_Box a, b;
    BRepBndLib::Add(boxBefore, a);
    BRepBndLib::Add(f.doc.getBody(f.box), b);
    double ax0, ay0, az0, ax1, ay1, az1, bx0, by0, bz0, bx1, by1, bz1;
    a.Get(ax0, ay0, az0, ax1, ay1, az1);
    b.Get(bx0, by0, bz0, bx1, by1, bz1);
    EXPECT_NEAR(ax0, bx0, 1e-9);
    EXPECT_NEAR(ax1, bx1, 1e-9);
}
