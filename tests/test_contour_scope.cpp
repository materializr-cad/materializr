// 11-16-26 (1.8.0b6_2): a feature applied to ONE contour of a sketch (Subtract on
// the 20 mm circle) was also applied to every contour drawn afterwards - Multiply
// on a 10 mm circle cut the copies, but so would any circle drawn by hand. The
// tester's rule:
//   - applied to the sketch as a whole: later contours are swept too (unchanged);
//   - applied to picked contours: only COPIES of those are (Multiply, Mirror),
//     found through the sketch's copy provenance (Sketch::setElementOrigin).
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "io/ProjectIO.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/Sketch.h"
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <cmath>
#include <memory>
#include <sstream>

using namespace materializr;

namespace {
double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return std::abs(g.Mass());
}
double totalVolume(const Document& doc) {
    double v = 0.0;
    for (int id : doc.getAllBodyIds()) v += vol(doc.getBody(id));
    return v;
}
gp_Pln groundPlane() { return gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))); }
const double kPi = 3.14159265358979323846;
double disc(double r) { return kPi * r * r; }

// Returns the circle id.
int addCircleAt(Sketch& sk, glm::vec2 c, double r) { return sk.addCircle(sk.addPoint(c), r); }
// What Multiply does: a new circle that remembers where it came from.
int copyCircle(Sketch& sk, int srcCircleId, glm::vec2 c) {
    double r = 0;
    for (const auto& ci : sk.getCircles()) if (ci.id == srcCircleId) r = ci.radius;
    const int nc = addCircleAt(sk, c, r);
    sk.setElementOrigin(nc, srcCircleId);
    return nc;
}
TopoDS_Face regionAt(const Sketch& sk, glm::vec2 p) {
    for (const auto& r : sk.buildRegions())
        if (sk.isPointInRegion(r, p)) return r.face;
    return TopoDS_Face();
}
// Extrude one circle's region as a new body, scoped as asked.
void extrudeRegion(ExtrudeOp& ex, Document& doc, int sid, glm::vec2 inside,
                   double dist, SketchScope scope) {
    auto sk = doc.getSketch(sid);
    ex.setSketchSource(sid);
    ex.setSketchScope(scope);
    ex.setProfile(regionAt(*sk, inside));
    ex.setDistance(dist);
    ex.setMode(ExtrudeMode::NewBody);
    ASSERT_TRUE(ex.execute(doc));
}
// What the cascade does after a sketch edit.
double replay(ExtrudeOp& ex, Document& doc, bool latest = true) {
    ex.undo(doc);
    EXPECT_TRUE(ex.rebuildProfileFromSketch(doc, true, latest));
    EXPECT_TRUE(ex.execute(doc));
    return totalVolume(doc);
}
} // namespace

TEST(ContourScope, OriginIsFlattenedAndSurvivesDeletingTheMiddleCopy) {
    Sketch sk;
    sk.setPlane(groundPlane());
    const int a = addCircleAt(sk, {0, 0}, 5);
    const int b = copyCircle(sk, a, {20, 0});
    const int c = copyCircle(sk, b, {40, 0});      // a copy of a copy
    EXPECT_EQ(sk.elementRoot(a), a);
    EXPECT_EQ(sk.elementRoot(b), a);
    EXPECT_EQ(sk.elementRoot(c), a) << "points at the original, not the middle copy";
    sk.removeElement(b);
    EXPECT_EQ(sk.elementRoot(c), a);
}

TEST(ContourScope, SignaturesTellCopiesFromSeparatelyDrawnContours) {
    Sketch sk;
    sk.setPlane(groundPlane());
    const int a = addCircleAt(sk, {0, 0}, 5);
    copyCircle(sk, a, {20, 0});
    addCircleAt(sk, {40, 0}, 5);                    // same size, drawn by hand
    const auto sigA = sk.faceRootSignature(regionAt(sk, {0, 0}));
    const auto sigCopy = sk.faceRootSignature(regionAt(sk, {20, 0}));
    const auto sigHand = sk.faceRootSignature(regionAt(sk, {40, 0}));
    ASSERT_FALSE(sigA.empty());
    EXPECT_EQ(sigA, sigCopy);
    EXPECT_NE(sigA, sigHand);
}

TEST(ContourScope, ProvenanceSurvivesTheProjectFormat) {
    Sketch sk;
    sk.setPlane(groundPlane());
    const int a = addCircleAt(sk, {0, 0}, 5);
    const int b = copyCircle(sk, a, {20, 0});
    std::ostringstream os;
    ProjectIO::writeSketchBody(os, sk);
    std::istringstream is(os.str());
    Sketch back;
    ProjectIO::parseSketchBody(is, back, "SKETCH_END");
    EXPECT_EQ(back.elementRoot(b), a);
    EXPECT_EQ(back.elementRoot(a), a);
}

// The tester's file: cut one circle, then Multiply a different one.
TEST(ContourScope, AContourScopedExtrudeFollowsItsCopiesButNotOtherCircles) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 10);                   // the big one, left alone
    const int small = addCircleAt(*sk, {30, 0}, 3); // the one the feature is applied to
    const int sid = doc.addSketch(sk);

    ExtrudeOp ex;
    extrudeRegion(ex, doc, sid, {30, 0}, 4.0, SketchScope::Contours);
    EXPECT_NEAR(totalVolume(doc), disc(3) * 4.0, 1.0);

    copyCircle(*sk, small, {50, 0});                // Multiply
    const int multiplied = copyCircle(*sk, small, {60, 0});
    copyCircle(*sk, multiplied, {70, 0});           // a copy of a copy
    addCircleAt(*sk, {0, 30}, 3);                   // same size, drawn by hand
    EXPECT_NEAR(replay(ex, doc), 4 * disc(3) * 4.0, 2.0)
        << "the original and its three copies; not the hand-drawn circle or the big one";
}

TEST(ContourScope, AWholeSketchExtrudeStillTakesEverythingDrawnLater) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    const int small = addCircleAt(*sk, {30, 0}, 3);
    const int sid = doc.addSketch(sk);

    ExtrudeOp ex;
    extrudeRegion(ex, doc, sid, {30, 0}, 4.0, SketchScope::Whole);
    copyCircle(*sk, small, {50, 0});
    addCircleAt(*sk, {0, 30}, 3);                   // drawn by hand
    EXPECT_NEAR(replay(ex, doc), 3 * disc(3) * 4.0, 2.0);
}

TEST(ContourScope, AContourScopedFeatureAdoptsEvenWhenItIsNotTheLatest) {
    // Several features on one sketch: a whole-sketch one only adopts when it is
    // the latest, but a contour one adopts ITS copies whichever it is.
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    const int small = addCircleAt(*sk, {30, 0}, 3);
    const int sid = doc.addSketch(sk);
    ExtrudeOp ex;
    extrudeRegion(ex, doc, sid, {30, 0}, 4.0, SketchScope::Contours);
    copyCircle(*sk, small, {50, 0});
    EXPECT_NEAR(replay(ex, doc, /*latest=*/false), 2 * disc(3) * 4.0, 2.0);
}

TEST(ContourScope, ScopeAndSignaturesSurviveSaveAndReload) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    const int small = addCircleAt(*sk, {30, 0}, 3);
    const int sid = doc.addSketch(sk);
    ExtrudeOp ex;
    extrudeRegion(ex, doc, sid, {30, 0}, 4.0, SketchScope::Contours);
    const std::string blob = ex.serializeParams();
    EXPECT_NE(blob.find(";scope=1"), std::string::npos) << blob;
    EXPECT_NE(blob.find(";sigs="), std::string::npos) << blob;

    ExtrudeOp again;
    ASSERT_TRUE(again.deserializeParams(blob));
    EXPECT_EQ(again.getSketchScope(), SketchScope::Contours);
    copyCircle(*sk, small, {50, 0});
    addCircleAt(*sk, {0, 30}, 3);
    ex.undo(doc);   // the original op's body goes; `again` rebuilds on its own
    ASSERT_TRUE(again.rebuildProfileFromSketch(doc, true, true));
    ASSERT_TRUE(again.execute(doc));
    EXPECT_NEAR(totalVolume(doc), 2 * disc(3) * 4.0, 2.0);
}

TEST(ContourScope, AFileWithoutAScopeKeepsAdoptingEverything) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {30, 0}, 3);
    const int sid = doc.addSketch(sk);
    ExtrudeOp ex;
    extrudeRegion(ex, doc, sid, {30, 0}, 4.0, SketchScope::Whole);
    std::string blob = ex.serializeParams();
    EXPECT_EQ(blob.find("scope="), std::string::npos) << "whole is the unmarked default";

    ExtrudeOp old;
    ASSERT_TRUE(old.deserializeParams(blob));
    EXPECT_EQ(old.getSketchScope(), SketchScope::Whole);
}

namespace {
std::unique_ptr<PushPullOp> pushRegion(Document& doc, int sid, glm::vec2 inside,
                                       double dist, SketchScope scope) {
    auto sk = doc.getSketch(sid);
    auto regions = sk->buildRegions();
    int idx = -1;
    for (size_t i = 0; i < regions.size(); ++i)
        if (sk->isPointInRegion(regions[i], inside)) idx = static_cast<int>(i);
    EXPECT_GE(idx, 0);
    auto op = std::make_unique<PushPullOp>();
    op->setTargets({PushPullOp::Target{}});
    op->setDistance(dist);
    op->setSketchSource(0, sid, idx, regions[idx].representativePoint);
    op->setSketchScope(sid, scope);
    return op;
}
double replayPush(PushPullOp& op, Document& doc, int sid, bool latest = true) {
    op.undo(doc);
    EXPECT_TRUE(op.rebuildProfileFromSketch(doc, sid, true, latest));
    EXPECT_TRUE(op.execute(doc));
    return totalVolume(doc);
}
} // namespace

TEST(ContourScope, PushPullFollowsCopiesOfItsContourButNotOtherCircles) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 10);
    const int small = addCircleAt(*sk, {30, 0}, 3);
    const int sid = doc.addSketch(sk);
    auto op = pushRegion(doc, sid, {30, 0}, 4.0, SketchScope::Contours);
    ASSERT_TRUE(op->rebuildProfileFromSketch(doc, sid));
    ASSERT_TRUE(op->execute(doc));
    EXPECT_NEAR(totalVolume(doc), disc(3) * 4.0, 1.0);

    copyCircle(*sk, small, {50, 0});
    addCircleAt(*sk, {0, 30}, 3);                   // by hand
    EXPECT_NEAR(replayPush(*op, doc, sid, /*latest=*/false), 2 * disc(3) * 4.0, 2.0);
}

TEST(ContourScope, PushPullWholeSketchStillTakesEverythingAndRoundTrips) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {30, 0}, 3);
    const int sid = doc.addSketch(sk);
    auto op = pushRegion(doc, sid, {30, 0}, 4.0, SketchScope::Whole);
    ASSERT_TRUE(op->rebuildProfileFromSketch(doc, sid));
    ASSERT_TRUE(op->execute(doc));
    EXPECT_EQ(op->serializeParams().find(";cn"), std::string::npos);
    addCircleAt(*sk, {0, 30}, 3);
    EXPECT_NEAR(replayPush(*op, doc, sid), 2 * disc(3) * 4.0, 2.0);

    // And the contour flavour keeps its scope through a save.
    Document doc2;
    auto sk2 = std::make_shared<Sketch>();
    sk2->setPlane(groundPlane());
    const int s2 = addCircleAt(*sk2, {30, 0}, 3);
    const int sid2 = doc2.addSketch(sk2);
    auto c = pushRegion(doc2, sid2, {30, 0}, 4.0, SketchScope::Contours);
    ASSERT_TRUE(c->rebuildProfileFromSketch(doc2, sid2));
    ASSERT_TRUE(c->execute(doc2));
    const std::string blob = c->serializeParams();
    EXPECT_NE(blob.find(";cn" + std::to_string(sid2) + "=1"), std::string::npos) << blob;
    EXPECT_NE(blob.find(";ug" + std::to_string(sid2) + "="), std::string::npos) << blob;
    PushPullOp again;
    ASSERT_TRUE(again.deserializeParams(blob));
    EXPECT_EQ(again.getSketchScope(sid2), SketchScope::Contours);
    copyCircle(*sk2, s2, {50, 0});
    addCircleAt(*sk2, {0, 30}, 3);
    ASSERT_TRUE(again.rebuildProfileFromSketch(doc2, sid2, true, true));
    EXPECT_EQ(again.targetCount(), 2) << "the original and its copy; not the hand-drawn one";
}
