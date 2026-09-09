// Scale Face in BOTH directions.
//
// Steve, 2026-08-04: "scale is supposed to increase or decrease the size of the
// face selected with the side walls following." Pinch did only half of that -
// it was Common(body, frustum), an intersection, which can only ever REMOVE
// material, so >100% returned success and changed nothing. Extend was no
// substitute: it adds a new tapered section on top rather than re-sloping the
// existing walls. Growing now unions the same frustum on instead.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "modeling/ScaleFaceOp.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <cstdio>

namespace {

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

// The +Z top face of a box at the origin.
TopoDS_Face topFace(const TopoDS_Shape& s, double z) {
    for (TopExp_Explorer fx(s, TopAbs_FACE); fx.More(); fx.Next()) {
        const TopoDS_Face& f = TopoDS::Face(fx.Current());
        GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
        if (std::abs(g.CentreOfMass().Z() - z) < 1e-6) return f;
    }
    return {};
}

struct Boxed {
    Document doc;
    int bodyId = -1;
    TopoDS_Face top;
    double vol0 = 0.0;
    Boxed() {
        TopoDS_Shape box = BRepPrimAPI_MakeBox(20.0, 20.0, 20.0).Shape();
        bodyId = doc.addBody(box, "box");
        top = topFace(box, 20.0);
        vol0 = volumeOf(box);
    }
};

} // namespace

// Shrinking is what Pinch is for, and it works.
TEST(ScaleFaceRange, PinchBelow100Shrinks) {
    Boxed f;
    ASSERT_FALSE(f.top.IsNull());
    ScaleFaceOp op;
    op.setBody(f.bodyId);
    op.setFace(f.top);
    op.setMode(ScaleFaceOp::Mode::Pinch);
    op.setScalePercent(50.0);
    op.setLength(20.0);           // full depth: sides follow from the base
    ASSERT_TRUE(op.execute(f.doc));
    const double v = volumeOf(f.doc.getBody(f.bodyId));
    std::printf("  pinch 50%%: %.1f -> %.1f\n", f.vol0, v);
    EXPECT_LT(v, f.vol0) << "pinch below 100% should remove material";
}

// THE REPORT: >100% must now GROW the body, with the side walls flaring out
// from the base - not silently do nothing.
TEST(ScaleFaceRange, PinchAbove100Grows) {
    Boxed f;
    ScaleFaceOp op;
    op.setBody(f.bodyId);
    op.setFace(f.top);
    op.setMode(ScaleFaceOp::Mode::Pinch);
    op.setScalePercent(150.0);
    op.setLength(20.0);
    ASSERT_TRUE(op.execute(f.doc)) << "pinch above 100% refused";
    const double v = volumeOf(f.doc.getBody(f.bodyId));
    std::printf("  pinch 150%%: %.1f -> %.1f\n", f.vol0, v);
    EXPECT_GT(v, f.vol0 + 1e-6)
        << "pinch past 100% did nothing - the Common/Fuse switch regressed";

    // ...and grew into the RIGHT shape, not just "bigger". A 20mm box whose
    // top face goes to 150% is a frustum 20 wide at the base, 30 at the top:
    // prismatoid 20/6*(400 + 4*625 + 900) = 12666.7. Volume alone would pass
    // for any old bulge, so this is what actually pins "the side walls follow".
    EXPECT_NEAR(v, 12666.7, 1.0) << "grew, but not into the expected frustum";
}

// Extend DOES grow: it fuses a tip loft on, so >100% adds material.
TEST(ScaleFaceRange, ExtendAbove100Grows) {
    Boxed f;
    ScaleFaceOp op;
    op.setBody(f.bodyId);
    op.setFace(f.top);
    op.setMode(ScaleFaceOp::Mode::Extend);
    op.setScalePercent(150.0);
    op.setLength(10.0);
    ASSERT_TRUE(op.execute(f.doc)) << "extend refused";
    const double v = volumeOf(f.doc.getBody(f.bodyId));
    std::printf("  extend 150%%: %.1f -> %.1f\n", f.vol0, v);
    EXPECT_GT(v, f.vol0) << "extend past 100% should ADD material";
}
