// Two Loft bugs from the 1.8.0-beta.5 tester list.
//   * A loft section taken from a sketch drawn ON a body face used the whole host
//     face as its base (the biggest region) - the loft "was affected by the object
//     the sketch was created on", although that body was never selected.
//   * A guided-loft rail drawn on the far side of the base plane was read upside
//     down, so the loft climbed the wrong way carrying a profile that tapered
//     backwards (a cone came out as an hourglass).
#include "core/Document.h"
#include "core/History.h"
#include "modeling/GuidedLoftOp.h"
#include "modeling/LoftOp.h"
#include "modeling/Sketch.h"

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gtest/gtest.h>
#include <cmath>
#include <memory>

using namespace materializr;

TEST(LoftSection, SketchOnAFaceTakesTheDrawnShapeNotTheFace) {
    // A 50-wide cylinder, top at z=10, with a 20 x 20 square drawn on top.
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(25.0, 10.0).Shape();
    TopoDS_Face top;
    for (TopExp_Explorer ex(cyl, TopAbs_FACE); ex.More(); ex.Next()) {
        Bnd_Box bb; BRepBndLib::Add(ex.Current(), bb);
        double a, b, c, d, e, f; bb.Get(a, b, c, d, e, f);
        if (std::abs(c - 10.0) < 1e-6 && std::abs(f - 10.0) < 1e-6) top = TopoDS::Face(ex.Current());
    }
    ASSERT_FALSE(top.IsNull());
    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    const int p0 = sk.addPoint({-10, -10}), p1 = sk.addPoint({10, -10});
    const int p2 = sk.addPoint({10, 10}),   p3 = sk.addPoint({-10, 10});
    sk.addLine(p0, p1); sk.addLine(p1, p2); sk.addLine(p2, p3); sk.addLine(p3, p0);
    sk.setSourceFace(top);

    std::vector<TopoDS_Wire> holes;
    bool closed = false;
    TopoDS_Wire w = LoftOp::sectionFromSketch(&sk, holes, &closed);
    ASSERT_TRUE(closed);
    Bnd_Box bb; BRepBndLib::Add(w, bb);
    double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x1 - x0, 20.0, 1e-3) << "took the host face's outline instead of the square";
    EXPECT_NEAR(y1 - y0, 20.0, 1e-3);
}

namespace {
double radiusAtZ(const TopoDS_Shape& body, double z) {
    BRepAlgoAPI_Section sec(body, gp_Pln(gp_Pnt(0, 0, z), gp_Dir(0, 0, 1)), Standard_False);
    sec.Build();
    Bnd_Box bb; BRepBndLib::Add(sec.Shape(), bb);
    if (bb.IsVoid()) return -1.0;
    double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
    return 0.5 * (x1 - x0);
}
}

TEST(GuidedLoftRails, RailBelowTheBasePlaneTapersTheRightWay) {
    for (const double dir : { 1.0, -1.0 }) {
        Document doc; History hist;
        auto base = std::make_shared<Sketch>();
        base->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
        base->addCircle(base->addPoint({0, 0}), 10.0);
        // Rail in the XZ plane: from the rim (x=10, z=0) to (x=4, z=22*dir).
        auto rail = std::make_shared<Sketch>();
        rail->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, -1, 0), gp_Dir(1, 0, 0))));
        const int r0 = rail->addPoint({10, 0});
        const int r1 = rail->addPoint({4, static_cast<float>(22.0 * dir)});
        rail->addLine(r0, r1);
        const int ib = doc.addSketch(base), ir = doc.addSketch(rail);
        std::vector<TopoDS_Wire> holes; bool closed = false;
        auto w = LoftOp::sectionFromSketch(base.get(), holes, &closed);
        ASSERT_TRUE(closed);
        auto op = std::make_unique<GuidedLoftOp>();
        op->setBase(w, base->getPlane());
        op->setBaseSketch(ib);
        op->addRail(rail->buildOpenWire(), ir);
        ASSERT_TRUE(hist.pushOperation(std::move(op), doc)) << "dir " << dir;
        const TopoDS_Shape body = doc.getBody(doc.getAllBodyIds().front());
        // Wide at the base, narrow at the far end - whichever side the rail is on.
        const double zNear = dir * 1.0, zFar = dir * 21.0;
        const double rNear = radiusAtZ(body, zNear), rFar = radiusAtZ(body, zFar);
        EXPECT_GT(rNear, 8.5) << "dir " << dir;
        EXPECT_LT(rFar, 5.5) << "dir " << dir;
        Bnd_Box bb; BRepBndLib::Add(body, bb);
        double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
        if (dir > 0) EXPECT_NEAR(z1, 22.0, 0.1); else EXPECT_NEAR(z0, -22.0, 0.1);
    }
}

// 1.8.0 beta tester list 11-14-26: a rail drawn THROUGH the base plane (up on one
// side, down on the other) is two rails in one stroke. It used to be folded onto
// a single climb - the profile pinched to nothing right after the base, swelled
// back, then closed again - instead of a body each way. Now each side is lofted
// on its own and the halves are joined into one solid.
TEST(GuidedLoftRails, RailThroughTheBasePlaneLoftsBothSides) {
    Document doc; History hist;
    // The tester's file: a 5 x 5 square on a plane whose normal is +Y, and a "<"
    // rail in the XY plane from (0, 2.5) back to (-2.5, 0) on the base plane and
    // on to (0, -2.5).
    auto base = std::make_shared<Sketch>();
    base->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0), gp_Dir(1, 0, 0))));
    const int a = base->addPoint({2.5f, 2.5f}), b = base->addPoint({-2.5f, 2.5f});
    const int c = base->addPoint({-2.5f, -2.5f}), d = base->addPoint({2.5f, -2.5f});
    base->addLine(a, b); base->addLine(b, c); base->addLine(c, d); base->addLine(d, a);
    auto rail = std::make_shared<Sketch>();
    rail->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    const int top = rail->addPoint({0, 2.5f}), mid = rail->addPoint({-2.5f, 0}),
              bot = rail->addPoint({0, -2.5f});
    rail->addLine(mid, top); rail->addLine(mid, bot);
    const int ib = doc.addSketch(base), ir = doc.addSketch(rail);
    std::vector<TopoDS_Wire> holes; bool closed = false;
    auto w = LoftOp::sectionFromSketch(base.get(), holes, &closed);
    ASSERT_TRUE(closed);
    auto op = std::make_unique<GuidedLoftOp>();
    op->setBase(w, base->getPlane());
    op->setBaseSketch(ib);
    op->addRail(rail->buildOpenWire(), ir);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));

    ASSERT_EQ(1u, doc.getAllBodyIds().size()) << "the two halves should be one body";
    const TopoDS_Shape body = doc.getBody(doc.getAllBodyIds().front());
    EXPECT_TRUE(BRepCheck_Analyzer(body).IsValid());
    int nSolids = 0;
    for (TopExp_Explorer ex(body, TopAbs_SOLID); ex.More(); ex.Next()) ++nSolids;
    EXPECT_EQ(1, nSolids);
    // Reaches 2.5 each way along the plane normal (Y)...
    Bnd_Box bb; BRepBndLib::Add(body, bb);
    double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(y1, 2.5, 0.05);
    EXPECT_NEAR(y0, -2.5, 0.05);
    // ...and is two square pyramids (base 5 x 5, height 2.5) base to base.
    GProp_GProps vp; BRepGProp::VolumeProperties(body, vp);
    EXPECT_NEAR(vp.Mass(), 2.0 * (25.0 * 2.5 / 3.0), 0.5);
}

namespace {
// The 11-14-26 tester's examples 3 and 4: a 5 x 5 square on a +Y-normal plane and,
// in the XY plane, ONE slanted outline line that crosses the base plane at x = -2.5,
// drawn together with construction lines (the vertical spine, the ground line...)
// that must play no part in the rail. Returns the single body.
TopoDS_Shape loftSlantedRail(Document& doc, History& hist, glm::vec2 from, glm::vec2 to) {
    auto base = std::make_shared<Sketch>();
    base->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0), gp_Dir(1, 0, 0))));
    const int a = base->addPoint({2.5f, 2.5f}), b = base->addPoint({-2.5f, 2.5f});
    const int c = base->addPoint({-2.5f, -2.5f}), d = base->addPoint({2.5f, -2.5f});
    base->addLine(a, b); base->addLine(b, c); base->addLine(c, d); base->addLine(d, a);

    auto rail = std::make_shared<Sketch>();
    rail->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    // Construction first (ids below the cut), then the real outline.
    const int o  = rail->addPoint({0, 0});
    const int sp = rail->addPoint({0, to.y > from.y ? to.y : from.y});
    const int sn = rail->addPoint({0, to.y < from.y ? to.y : from.y});
    const int firstConstruction = rail->addLine(o, sp);
    rail->addLine(sp, sn);
    rail->setConstructionSince(firstConstruction, true);
    const int p0 = rail->addPoint(from), pm = rail->addPoint(0.5f * (from + to)),
              p1 = rail->addPoint(to);
    rail->addLine(p0, pm); rail->addLine(pm, p1);        // the outline, in two pieces

    const int ib = doc.addSketch(base), ir = doc.addSketch(rail);
    std::vector<TopoDS_Wire> holes; bool closed = false;
    auto w = LoftOp::sectionFromSketch(base.get(), holes, &closed);
    EXPECT_TRUE(closed);
    auto op = std::make_unique<GuidedLoftOp>();
    op->setBase(w, base->getPlane());
    op->setBaseSketch(ib);
    TopoDS_Wire rw = rail->buildOpenWire();
    EXPECT_FALSE(rw.IsNull()) << "construction lines leaked into / broke the rail";
    op->addRail(rw, ir);
    EXPECT_TRUE(hist.pushOperation(std::move(op), doc));
    EXPECT_EQ(1u, doc.getAllBodyIds().size());
    return doc.getAllBodyIds().empty() ? TopoDS_Shape() : doc.getBody(doc.getAllBodyIds().front());
}

double frustum(double h, double s0, double s1) {      // square sides s0 -> s1 over height h
    const double a0 = s0 * s0, a1 = s1 * s1;
    return h / 3.0 * (a0 + a1 + std::sqrt(a0 * a1));
}
}

// Example 3: apex above, the section WIDENS below the plane.
TEST(GuidedLoftRails, SlantedRailThroughThePlaneShrinksUpAndWidensDown) {
    Document doc; History hist;
    const TopoDS_Shape body = loftSlantedRail(doc, hist, {0.0f, 3.0f}, {-5.0f, -3.0f});
    ASSERT_FALSE(body.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(body).IsValid());
    Bnd_Box bb; BRepBndLib::Add(body, bb);
    double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(y1, 3.0, 0.05);
    EXPECT_NEAR(y0, -3.0, 0.05);
    GProp_GProps vp; BRepGProp::VolumeProperties(body, vp);
    EXPECT_NEAR(vp.Mass(), frustum(3.0, 5.0, 0.0) + frustum(3.0, 5.0, 10.0), 1.0);
}

// Example 4: the mirror - widens above, closes to a point below.
TEST(GuidedLoftRails, SlantedRailThroughThePlaneWidensUpAndShrinksDown) {
    Document doc; History hist;
    const TopoDS_Shape body = loftSlantedRail(doc, hist, {-5.0f, 2.5f}, {0.0f, -2.5f});
    ASSERT_FALSE(body.IsNull());
    EXPECT_TRUE(BRepCheck_Analyzer(body).IsValid());
    Bnd_Box bb; BRepBndLib::Add(body, bb);
    double x0, y0, z0, x1, y1, z1; bb.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(y1, 2.5, 0.05);
    EXPECT_NEAR(y0, -2.5, 0.05);
    GProp_GProps vp; BRepGProp::VolumeProperties(body, vp);
    EXPECT_NEAR(vp.Mass(), frustum(2.5, 5.0, 10.0) + frustum(2.5, 5.0, 0.0), 1.0);
}
