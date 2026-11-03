// Sweep: a closed profile along a path into a body of constant section (the
// pipe a loft cannot make - issue #167). The profile need not sit exactly at the
// start of the path or square to it; SweepOp puts it there.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "modeling/OperationFactory.h"
#include "modeling/Sketch.h"
#include "modeling/SweepOp.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Circle.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <cmath>
#include <memory>

using namespace materializr;

namespace {
const double kPi = 3.14159265358979323846;

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

// A disc of radius r centred at c, lying in the plane with normal n.
TopoDS_Face disc(gp_Pnt c, gp_Dir n, double r) {
    gp_Circ circ(gp_Ax2(c, n), r);
    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(circ).Edge();
    return BRepBuilderAPI_MakeFace(BRepBuilderAPI_MakeWire(e).Wire()).Face();
}

TopoDS_Wire line(gp_Pnt a, gp_Pnt b) {
    return BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(a, b).Edge()).Wire();
}

// A quarter turn of radius R about the Z axis through the origin, starting at
// (R, 0, 0) heading +Y, ending at (0, R, 0).
TopoDS_Wire quarterArc(double R) {
    gp_Pnt a(R, 0, 0), m(R * std::cos(kPi / 4), R * std::sin(kPi / 4), 0), b(0, R, 0);
    Handle(Geom_TrimmedCurve) arc = GC_MakeArcOfCircle(a, m, b).Value();
    return BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(arc).Edge()).Wire();
}
} // namespace

TEST(Sweep, CircleAlongAStraightPathIsACylinder) {
    std::string why;
    TopoDS_Shape s = SweepOp::build(disc({0, 0, 0}, gp_Dir(1, 0, 0), 5.0),
                                    line({0, 0, 0}, {40, 0, 0}), true, &why);
    ASSERT_FALSE(s.IsNull()) << why;
    EXPECT_NEAR(volumeOf(s), kPi * 25.0 * 40.0, kPi * 25.0 * 40.0 * 0.005);
    EXPECT_TRUE(BRepCheck_Analyzer(s).IsValid());
}

TEST(Sweep, CircleAlongAnArcFollowsTheBend) {
    // Pappus: area x the length the centroid travels.
    const double R = 30.0, r = 4.0;
    TopoDS_Shape s = SweepOp::build(disc({R, 0, 0}, gp_Dir(0, 1, 0), r), quarterArc(R), true);
    ASSERT_FALSE(s.IsNull());
    const double expected = kPi * r * r * (kPi / 2.0 * R);
    EXPECT_NEAR(volumeOf(s), expected, expected * 0.01);
}

TEST(Sweep, ProfileAwayFromThePathIsCarriedToItsStart) {
    // Drawn 25 mm off to the side and nowhere near the path: still a tube that
    // starts where the path does.
    TopoDS_Shape s = SweepOp::build(disc({0, 25, 0}, gp_Dir(1, 0, 0), 5.0),
                                    line({0, 0, 0}, {40, 0, 0}), true);
    ASSERT_FALSE(s.IsNull());
    EXPECT_NEAR(volumeOf(s), kPi * 25.0 * 40.0, kPi * 25.0 * 40.0 * 0.005);
    Bnd_Box b; BRepBndLib::Add(s, b);
    double x0, y0, z0, x1, y1, z1; b.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x0, 0.0, 1e-3);
    EXPECT_NEAR(x1, 40.0, 1e-3);
    EXPECT_NEAR(y0, -5.0, 1e-3) << "centred on the path, not where it was drawn";
}

TEST(Sweep, ProfileAtTheFarEndSweepsBackwards) {
    TopoDS_Shape s = SweepOp::build(disc({40, 0, 0}, gp_Dir(1, 0, 0), 5.0),
                                    line({0, 0, 0}, {40, 0, 0}), true);
    ASSERT_FALSE(s.IsNull());
    EXPECT_NEAR(volumeOf(s), kPi * 25.0 * 40.0, kPi * 25.0 * 40.0 * 0.005);
    // ... and along a bend, where the direction matters.
    const double R = 30.0, r = 4.0;
    TopoDS_Shape t = SweepOp::build(disc({0, R, 0}, gp_Dir(1, 0, 0), r), quarterArc(R), true);
    ASSERT_FALSE(t.IsNull());
    const double expected = kPi * r * r * (kPi / 2.0 * R);
    EXPECT_NEAR(volumeOf(t), expected, expected * 0.01);
}

TEST(Sweep, ATiltedProfileIsSquaredToThePath) {
    // A circle drawn 40 degrees off square, as freehand sketching leaves it.
    gp_Dir tilted(std::cos(40.0 * kPi / 180.0), std::sin(40.0 * kPi / 180.0), 0.0);
    TopoDS_Shape square = SweepOp::build(disc({0, 0, 0}, tilted, 5.0),
                                         line({0, 0, 0}, {40, 0, 0}), true);
    ASSERT_FALSE(square.IsNull());
    EXPECT_NEAR(volumeOf(square), kPi * 25.0 * 40.0, kPi * 25.0 * 40.0 * 0.005)
        << "the section keeps the size that was drawn";
    // Left as drawn it is swept at its own angle - a different, smaller body.
    TopoDS_Shape asDrawn = SweepOp::build(disc({0, 0, 0}, tilted, 5.0),
                                          line({0, 0, 0}, {40, 0, 0}), false);
    ASSERT_FALSE(asDrawn.IsNull());
    EXPECT_LT(volumeOf(asDrawn), volumeOf(square) * 0.9);
}

TEST(Sweep, ARingProfileMakesAHollowTube) {
    gp_Ax2 ax(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0));
    TopoDS_Wire outer = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Circ(ax, 6.0)).Edge()).Wire();
    TopoDS_Wire inner = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(gp_Circ(ax, 4.0)).Edge()).Wire();
    BRepBuilderAPI_MakeFace mk(outer);
    mk.Add(TopoDS::Wire(inner.Reversed()));
    ASSERT_TRUE(mk.IsDone());
    TopoDS_Shape s = SweepOp::build(mk.Face(), line({0, 0, 0}, {30, 0, 0}), true);
    ASSERT_FALSE(s.IsNull());
    const double expected = kPi * (36.0 - 16.0) * 30.0;
    EXPECT_NEAR(volumeOf(s), expected, expected * 0.005);
}

TEST(Sweep, ACornerInThePathStillMakesAValidBody) {
    // The reporter's path is a polyline: the corner must not break the sweep.
    BRepBuilderAPI_MakeWire mw;
    mw.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(30, 0, 0)).Edge());
    mw.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(30, 0, 0), gp_Pnt(30, 30, 0)).Edge());
    std::string why;
    TopoDS_Shape s = SweepOp::build(disc({0, 0, 0}, gp_Dir(1, 0, 0), 3.0), mw.Wire(), true, &why);
    ASSERT_FALSE(s.IsNull()) << why;
    EXPECT_TRUE(BRepCheck_Analyzer(s).IsValid());
    const double straight = kPi * 9.0 * 60.0;
    EXPECT_NEAR(volumeOf(s), straight, straight * 0.1);
}

TEST(Sweep, ABendTighterThanTheSectionIsRefusedWithAReason) {
    std::string why;
    TopoDS_Shape s = SweepOp::build(disc({3, 0, 0}, gp_Dir(0, 1, 0), 5.0), quarterArc(3.0), true, &why);
    EXPECT_TRUE(s.IsNull()) << "a radius-5 tube round a radius-3 bend folds through itself";
    EXPECT_FALSE(why.empty());
}

TEST(Sweep, AnOpenWireProfileHasNoVolume) {
    std::string why;
    TopoDS_Shape s = SweepOp::build(line({0, -5, 0}, {0, 5, 0}), line({0, 0, 0}, {40, 0, 0}), true, &why);
    EXPECT_TRUE(s.IsNull());
    EXPECT_FALSE(why.empty());
}

TEST(Sweep, OpExecutesUndoesRedoesUnderTheSameId) {
    Document doc;
    History hist;
    auto op = std::make_unique<SweepOp>();
    op->setProfile(disc({0, 0, 0}, gp_Dir(1, 0, 0), 5.0));
    op->setPath(line({0, 0, 0}, {40, 0, 0}));
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    ASSERT_EQ(doc.getAllBodyIds().size(), 1u);
    const int id = doc.getAllBodyIds().front();

    ASSERT_TRUE(hist.undo(doc));
    EXPECT_TRUE(doc.getAllBodyIds().empty()) << "undo removes the swept body";
    ASSERT_TRUE(hist.redo(doc));
    ASSERT_EQ(doc.getAllBodyIds().size(), 1u);
    EXPECT_EQ(doc.getAllBodyIds().front(), id);
}

TEST(Sweep, SerializesAndReloadsEditable) {
    SweepOp op;
    op.setProfile(disc({0, 0, 0}, gp_Dir(1, 0, 0), 5.0));
    op.setPath(line({0, 0, 0}, {40, 0, 0}));
    op.setKeepPerpendicular(false);
    op.setSketchSources(3, 4);
    const std::string blob = op.serializeParams();

    auto again = OperationFactory::create("sweep");
    ASSERT_NE(again, nullptr);
    ASSERT_TRUE(again->deserializeParams(blob));
    auto* sw = dynamic_cast<SweepOp*>(again.get());
    ASSERT_NE(sw, nullptr);
    EXPECT_FALSE(sw->keepPerpendicular());
    EXPECT_EQ(sw->getProfileSketchId(), 3);
    EXPECT_EQ(sw->getPathSketchId(), 4);
    Document doc;
    EXPECT_TRUE(sw->execute(doc)) << "the stored geometry is enough to run again";
}

namespace {
// A profile sketch (a circle on the YZ plane) and a path sketch (a line on XY).
struct SketchedSweep {
    Document doc;
    std::shared_ptr<Sketch> profile = std::make_shared<Sketch>();
    std::shared_ptr<Sketch> path = std::make_shared<Sketch>();
    int pid = -1, wid = -1;
    SketchedSweep() {
        profile->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0))));
        profile->addCircle(profile->addPoint({0, 0}), 5.0);
        path->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
        path->addLine(path->addPoint({0, 0}), path->addPoint({40, 0}));
        pid = doc.addSketch(profile, "Profile");
        wid = doc.addSketch(path, "Path");
    }
};
}

TEST(Sweep, BuildsFromTwoSketches) {
    SketchedSweep f;
    SweepOp op;
    op.setSketchSources(f.pid, f.wid);
    ASSERT_TRUE(op.rebuildFromSketches(f.doc));
    ASSERT_TRUE(op.execute(f.doc)) << op.lastFailure();
    EXPECT_NEAR(volumeOf(f.doc.getBody(f.doc.getAllBodyIds().front())),
                kPi * 25.0 * 40.0, kPi * 25.0 * 40.0 * 0.005);
}

TEST(Sweep, FollowsEditsToItsSketchesThroughTheCascade) {
    SketchedSweep f;
    History hist;
    auto op = std::make_unique<SweepOp>();
    op->setSketchSources(f.pid, f.wid);
    ASSERT_TRUE(op->rebuildFromSketches(f.doc));
    ASSERT_TRUE(hist.pushOperation(std::move(op), f.doc));
    EXPECT_TRUE(opBuiltFromSketch(hist.getStep(0), f.pid));
    EXPECT_TRUE(opBuiltFromSketch(hist.getStep(0), f.wid));

    // Make the path longer: the cascade re-derives the op and the body follows.
    f.path->addLine(f.path->getLines().front().endPointId, f.path->addPoint({60, 0}));
    int matched = 0;
    const int earliest = rederiveSketchDrivenOps(hist, f.doc, f.wid, &matched);
    EXPECT_EQ(matched, 1);
    ASSERT_EQ(earliest, 0);
    ASSERT_TRUE(hist.editStep(0, f.doc, true));
    EXPECT_NEAR(volumeOf(f.doc.getBody(f.doc.getAllBodyIds().front())),
                kPi * 25.0 * 60.0, kPi * 25.0 * 60.0 * 0.01);
}

// A curve drawn in a sketch must reach the sweep as real curves, not as dozens of
// tiny straight segments (which the pipe builder fails on) - this is what the
// reporter's curve looks like.
TEST(Sweep, ASplineSketchPathMakesAValidTube) {
    auto path = std::make_shared<Sketch>();
    path->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    std::vector<int> pts;
    // A gentle S-bend, 60 mm long.
    for (auto p : {glm::vec2(0, 0), glm::vec2(15, 8), glm::vec2(30, 0), glm::vec2(45, -8), glm::vec2(60, 0)})
        pts.push_back(path->addPoint(p));
    path->addSpline(pts);
    TopoDS_Wire w = path->buildOpenWire(true);
    ASSERT_FALSE(w.IsNull());
    GProp_GProps lp; BRepGProp::LinearProperties(w, lp);

    std::string why;
    TopoDS_Shape s = SweepOp::build(disc({0, 0, 0}, gp_Dir(1, 0, 0), 3.0), w, true, &why);
    ASSERT_FALSE(s.IsNull()) << why;
    EXPECT_TRUE(BRepCheck_Analyzer(s).IsValid());
    const double expected = kPi * 9.0 * lp.Mass();
    EXPECT_NEAR(volumeOf(s), expected, expected * 0.03);
}

TEST(Sweep, ALineArcLineSketchPathKeepsItsArc) {
    auto path = std::make_shared<Sketch>();
    path->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    // 20 along +X, a quarter arc of radius 15 turning +Y, then 20 along +Y.
    const int a = path->addPoint({0, 0}), b = path->addPoint({20, 0});
    const int c = path->addPoint({20, 15}), d = path->addPoint({35, 15});
    const int e = path->addPoint({35, 35});
    path->addLine(a, b);
    path->addArc(c, b, d, 15.0);
    path->addLine(d, e);
    TopoDS_Wire w = path->buildOpenWire(true);
    ASSERT_FALSE(w.IsNull());
    GProp_GProps lp; BRepGProp::LinearProperties(w, lp);
    EXPECT_NEAR(lp.Mass(), 20.0 + kPi / 2.0 * 15.0 + 20.0, 0.05) << "the arc is an arc, not a chord";

    std::string why;
    TopoDS_Shape s = SweepOp::build(disc({0, 0, 0}, gp_Dir(1, 0, 0), 3.0), w, true, &why);
    ASSERT_FALSE(s.IsNull()) << why;
    const double expected = kPi * 9.0 * lp.Mass();
    EXPECT_NEAR(volumeOf(s), expected, expected * 0.01);
}

// The reporter's picture: a small circle roughly (not exactly) at one end of a
// hand-drawn polyline that bends round, circle a little off square.
TEST(Sweep, AHandDrawnPolylineWithATiltedCircleAtItsFarEnd) {
    auto path = std::make_shared<Sketch>();
    path->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    std::vector<int> ids;
    for (auto p : {glm::vec2(0, 0), glm::vec2(20, 4), glm::vec2(38, 14), glm::vec2(48, 30),
                   glm::vec2(50, 48), glm::vec2(42, 64)})
        ids.push_back(path->addPoint(p));
    for (size_t i = 0; i + 1 < ids.size(); ++i) path->addLine(ids[i], ids[i + 1]);
    TopoDS_Wire w = path->buildOpenWire(true);
    ASSERT_FALSE(w.IsNull());
    GProp_GProps lp; BRepGProp::LinearProperties(w, lp);

    // Near the LAST point, 1.5 mm off it, turned about 25 degrees off square.
    gp_Dir tilted(std::cos(-1.1 + 0.44), std::sin(-1.1 + 0.44), 0.0);
    std::string why;
    TopoDS_Shape s = SweepOp::build(disc({43.0, 65.0, 0}, tilted, 2.5), w, true, &why);
    ASSERT_FALSE(s.IsNull()) << why;
    EXPECT_TRUE(BRepCheck_Analyzer(s).IsValid());
    const double expected = kPi * 6.25 * lp.Mass();
    EXPECT_NEAR(volumeOf(s), expected, expected * 0.03);
}
