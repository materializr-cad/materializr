// A sketch wrapped onto a cylinder must read the same from outside as it does
// in the sketch. The wrap used a fixed u direction, which only holds for one
// relation between the cylinder's axis and the sketch's Y; a standing cylinder
// (axis along the sketch's up) came out mirrored (1.8.0 beta report: engraved
// text mirrored on a Cylinder primitive).
#include <gtest/gtest.h>

#include "core/Document.h"
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
double engravedX(const gp_Dir& axisDir) {
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
