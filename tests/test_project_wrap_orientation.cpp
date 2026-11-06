// A sketch wrapped onto a cylinder must read the same from outside as it does
// in the sketch. The wrap used a fixed u direction, which only holds for one
// relation between the cylinder's axis and the sketch's Y; a standing cylinder
// (axis along the sketch's up) came out mirrored (1.8.0 beta report: engraved
// text mirrored on a Cylinder primitive).
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "modeling/DeleteSketchOp.h"
#include "modeling/ProjectSketchOp.h"
#include "modeling/Sketch.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <memory>

using namespace materializr;

namespace {
// First moment (mass * centroid) of a solid.
gp_Vec moment(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return gp_Vec(gp_Pnt(0, 0, 0), g.CentreOfMass()) * g.Mass();
}

// Engrave an "L" drawn RIGHT of the sketch origin onto a cylinder whose axis
// runs along `axisDir`; return the x (viewer's right) of the removed material.
double engravedX(const gp_Dir& axisDir, bool mirror = false) {
    Document doc;
    TopoDS_Shape cylShape =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), axisDir), 10.0, 40.0).Shape();
    // Centre the cylinder on the origin so +/- axis both span the sketch.
    gp_Trsf mv;
    mv.SetTranslation(gp_Vec(axisDir) * -20.0);
    cylShape.Move(TopLoc_Location(mv));
    const int body = doc.addBody(cylShape, "Cyl");

    auto sk = std::make_shared<Sketch>();
    // Viewer looks down -Z from z = 30: sketch X is world X (right), Y is world Y (up).
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 30), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    const glm::vec2 pts[] = {{2, -4}, {8, -4}, {8, -2}, {4, -2}, {4, 6}, {2, 6}};
    int ids[6];
    for (int i = 0; i < 6; ++i) ids[i] = sk->addPoint(pts[i]);
    for (int i = 0; i < 6; ++i) sk->addLine(ids[i], ids[(i + 1) % 6]);
    const int sid = doc.addSketch(sk, "Sketch 1");

    TopoDS_Face side;
    for (TopExp_Explorer e(doc.getBody(body), TopAbs_FACE); e.More(); e.Next()) {
        BRepAdaptor_Surface a(TopoDS::Face(e.Current()));
        if (a.GetType() == GeomAbs_Cylinder) side = TopoDS::Face(e.Current());
    }
    if (side.IsNull()) return 1e9;

    const gp_Vec before = moment(doc.getBody(body));
    GProp_GProps g0; BRepGProp::VolumeProperties(doc.getBody(body), g0);
    ProjectSketchOp op;
    op.setBody(body);
    op.setTargetFace(side);
    op.setSketchId(sid);
    op.setDepth(1.0);
    op.setMode(ProjectSketchOp::Mode::Engrave);
    op.setMirror(mirror);
    if (!op.execute(doc)) return 1e9;
    const gp_Vec after = moment(doc.getBody(body));
    GProp_GProps g1; BRepGProp::VolumeProperties(doc.getBody(body), g1);
    const double removed = g0.Mass() - g1.Mass();
    if (removed < 1e-6) return 1e9;
    return (before - after).X() / removed;
}
} // namespace

TEST(ProjectWrapOrientation, MarkRightOfOriginStaysRightWhateverTheAxisDirection) {
    EXPECT_GT(engravedX(gp_Dir(0, 1, 0)), 0.5) << "axis along sketch +Y: mirrored";
    EXPECT_GT(engravedX(gp_Dir(0, -1, 0)), 0.5) << "axis along sketch -Y: mirrored";
}

// The wrap can only guess which way round the result should read, so the user
// chooses: "Mirrored" flips the selection about its OWN centre line - it lands
// where it was drawn but reads the other way round (the L's foot points the
// other way, which moves the centre of the removed material the other way).
TEST(ProjectWrapOrientation, MirroredFlipsTheMarkInPlace) {
    const double asDrawn = engravedX(gp_Dir(0, 1, 0), false);
    const double mirrored = engravedX(gp_Dir(0, 1, 0), true);
    ASSERT_LT(std::fabs(asDrawn), 100.0);
    ASSERT_LT(std::fabs(mirrored), 100.0);
    EXPECT_GT(mirrored, 0.5) << "still on the right of the origin, not swung round the body";
    EXPECT_GT(std::fabs(asDrawn - mirrored), 0.3) << "it must actually read the other way";
    EXPECT_NEAR(0.5 * (asDrawn + mirrored), 10.0 * std::sin(0.5), 0.6)
        << "mirrored about the middle of the drawn extent (x = 2..8)";
}

// 11-6-26: an engraving/embossing made with Project did not follow its sketch -
// editing the sketch left it where it was, and deleting the sketch left it in the
// body. It is a sketch-driven step like an extrude: the cascade replays from it,
// and a sketch delete switches it off.
TEST(ProjectCascade, FollowsItsSketchAndGoesWhenTheSketchIsDeleted) {
    Document doc;
    History hist;
    // A standing cylinder (axis along the sketch's up), centred on the origin.
    TopoDS_Shape cylShape =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)), 10.0, 40.0).Shape();
    gp_Trsf mv;
    mv.SetTranslation(gp_Vec(0, -20, 0));
    cylShape.Move(TopLoc_Location(mv));
    const int body = doc.addBody(cylShape, "Cyl");
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 30), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    const glm::vec2 pts[] = {{-3, 2}, {3, 2}, {3, 8}, {-3, 8}};
    int ids[4];
    for (int i = 0; i < 4; ++i) ids[i] = sk->addPoint(pts[i]);
    for (int i = 0; i < 4; ++i) sk->addLine(ids[i], ids[(i + 1) % 4]);
    const int sid = doc.addSketch(sk, "Mark");

    TopoDS_Face side;
    for (TopExp_Explorer e(doc.getBody(body), TopAbs_FACE); e.More(); e.Next()) {
        BRepAdaptor_Surface a(TopoDS::Face(e.Current()));
        if (a.GetType() == GeomAbs_Cylinder) side = TopoDS::Face(e.Current());
    }
    ASSERT_FALSE(side.IsNull());
    GProp_GProps g0; BRepGProp::VolumeProperties(doc.getBody(body), g0);
    const double whole = g0.Mass();

    auto pj = std::make_unique<ProjectSketchOp>();
    pj->setBody(body);
    pj->setTargetFace(side);
    pj->setSketchId(sid);
    pj->setDepth(1.0);
    ASSERT_TRUE(hist.pushOperation(std::move(pj), doc));
    GProp_GProps g1; BRepGProp::VolumeProperties(doc.getBody(body), g1);
    const double engraved = g1.Mass();
    ASSERT_LT(engraved, whole - 1e-3);

    int matched = 0;
    EXPECT_EQ(rederiveSketchDrivenOps(hist, doc, sid, &matched), 0);
    EXPECT_EQ(matched, 1);
    EXPECT_TRUE(opBuiltFromSketch(hist.getStep(0), sid));

    // Make the mark twice as tall; the replay engraves the new shape.
    sk->movePoint(ids[2], {3, 14});
    sk->movePoint(ids[3], {-3, 14});
    ASSERT_TRUE(hist.editStep(0, doc, /*transactional=*/true));
    GProp_GProps g2; BRepGProp::VolumeProperties(doc.getBody(body), g2);
    EXPECT_LT(g2.Mass(), engraved - 1e-3) << "the engraving followed the edited sketch";

    // Delete the sketch: the engraving goes with it, one Undo brings both back.
    auto del = std::make_unique<DeleteSketchOp>();
    del->setSketchId(sid);
    del->addSuppressedStep(const_cast<Operation*>(hist.getStep(0)));
    ASSERT_TRUE(hist.pushOperation(std::move(del), doc));
    GProp_GProps g3; BRepGProp::VolumeProperties(doc.getBody(body), g3);
    EXPECT_NEAR(g3.Mass(), whole, 1e-6);
    ASSERT_TRUE(hist.undo(doc));
    GProp_GProps g4; BRepGProp::VolumeProperties(doc.getBody(body), g4);
    EXPECT_NEAR(g4.Mass(), g2.Mass(), 1e-6);
}
