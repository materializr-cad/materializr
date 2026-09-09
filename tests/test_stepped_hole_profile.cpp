// #60: a stepped/counterbore hole (two concentric circles) inside a part
// outline must NOT extrude the inner circle as a solid plug. Even-odd parity
// nests the inner circle two levels deep (inside the outline AND the outer
// circle) and would fill it solid - a floating lump disconnected from the
// body. buildProfileShape must drop that plug so the hole stays open.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "modeling/Sketch.h"
#include "modeling/ExtrudeOp.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <memory>

using namespace materializr;

namespace {
int solidCount(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer x(s, TopAbs_SOLID); x.More(); x.Next()) ++n;
    return n;
}
// A plate outline with a concentric stepped hole (r=2.5 inner, r=4.5 outer)
// centred inside it. The inner circle ends up at nesting depth 2.
std::shared_ptr<Sketch> steppedHoleSketch() {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    sk->addRectangle(glm::vec2(-20, -20), glm::vec2(20, 20)); // part outline
    int c = sk->addPoint(glm::vec2(0, 0));
    int c2 = sk->addPoint(glm::vec2(0, 0));
    sk->addCircle(c, 2.5f);  // inner (through-hole)
    sk->addCircle(c2, 4.5f); // outer (counterbore)
    return sk;
}
} // namespace

TEST(SteppedHoleProfile, InnerCircleIsNotAFilledDisk) {
    auto sk = steppedHoleSketch();
    TopoDS_Shape prof = sk->buildProfileShape();
    ASSERT_FALSE(prof.IsNull());
    // No face may be a standalone r=2.5 disk (area ~19.635). The profile is
    // the plate with a r=4.5 hole - one island, hole open.
    for (TopExp_Explorer fx(prof, TopAbs_FACE); fx.More(); fx.Next()) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(fx.Current(), g);
        EXPECT_GT(std::abs(g.Mass() - M_PI * 2.5 * 2.5), 1.0)
            << "profile contains a filled inner-circle disk (the plug)";
    }
}

TEST(SteppedHoleProfile, ExtrudeStaysConnected) {
    Document doc;
    auto sk = steppedHoleSketch();
    int sid = doc.addSketch(sk, "Sketch 1");

    ExtrudeOp op;
    op.setSketchSource(sid);
    op.setProfile(sk->buildProfileShape());
    op.setDistance(2.0);
    op.setMode(ExtrudeMode::NewBody);
    ASSERT_TRUE(op.execute(doc));

    int bid = -1;
    for (int id : doc.getAllBodyIds()) bid = id;
    ASSERT_GE(bid, 0);
    const TopoDS_Shape& body = doc.getBody(bid);
    EXPECT_EQ(solidCount(body), 1) << "stepped hole extruded a floating plug";
    // The r=2.5 hole must be open: volume = plate minus the r=4.5 counterbore,
    // NOT plate minus ring plus plug.
    GProp_GProps g;
    BRepGProp::VolumeProperties(body, g);
    const double plate = 40.0 * 40.0 * 2.0;
    const double counterbore = M_PI * 4.5 * 4.5 * 2.0;
    EXPECT_NEAR(g.Mass(), plate - counterbore, 1.0);
}
