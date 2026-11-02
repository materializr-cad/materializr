// Guards the extrude footprint-recovery fallback. When a sketch-sourced
// extrude's stored region seed fails to re-match on replay (the region moved
// or its topology changed), the rebuild must fall back to the SAVED footprint
// - never to the #53 "ALL regions" catastrophe that sweeps every region of the
// sketch (grabbing unrelated features and corrupting everything downstream).
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Operation.h"
#include "modeling/Sketch.h"
#include "modeling/ExtrudeOp.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <memory>
#include <vector>

using namespace materializr;

namespace {
double vol(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}
} // namespace

TEST(ExtrudeRecoveredProfile, SeedMissFallsBackToFootprintNotAllRegions) {
    Document doc;
    // Two DISJOINT square regions in one sketch: A (to extrude) and B (an
    // unrelated feature that must never get swept by this extrude).
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    sk->addRectangle(glm::vec2(0, 0), glm::vec2(10, 10));   // A, area 100
    sk->addRectangle(glm::vec2(20, 0), glm::vec2(30, 10));  // B, area 100
    int sid = doc.addSketch(sk, "Sketch 1");

    // Extrude ONLY A (seed at its centre).
    auto op = std::make_unique<ExtrudeOp>();
    op->setSketchSource(sid);
    op->deserializeParams(
        "sketch=1;dist=5;dir=0;mode=0;target=-1;draft=0;regions=5.0:5.0");
    ASSERT_TRUE(op->rebuildProfileFromSketch(doc));
    op->setDistance(5.0);
    op->setMode(ExtrudeMode::NewBody);
    ASSERT_TRUE(op->execute(doc));
    int bid = -1;
    for (int id : doc.getAllBodyIds()) bid = id;
    ASSERT_GE(bid, 0);
    const TopoDS_Shape aBody = doc.getBody(bid);

    // Simulate a project RELOAD: a fresh op restores its params and rehydrates
    // against the reload state, which is where the saved-footprint recovery
    // populates. (Mirrors ProjectIO.)
    auto rop = std::make_unique<ExtrudeOp>();
    ASSERT_TRUE(rop->deserializeParams(
        "sketch=1;dist=5;dir=0;mode=0;target=-1;draft=0;regions=5.0:5.0"));
    rop->setSketchSource(sid);
    Operation::ReloadState rs;
    rs.created.push_back(bid);
    rs.createdAfter.push_back({bid, aBody});
    ASSERT_TRUE(rop->rehydrateFromReload(rs, doc));

    // Now MOVE region A (+40 in x) so the stored seed (5,5) lands in NO face.
    std::vector<int> aCorners;
    for (const auto& p : sk->getPoints())
        if (p.pos.x <= 10.5f && p.pos.y <= 10.5f) aCorners.push_back(p.id);
    ASSERT_EQ(aCorners.size(), 4u);
    for (int id : aCorners) {
        const SketchPoint* p = sk->getPoint(id);
        sk->movePoint(id, glm::vec2(p->pos.x + 40.0f, p->pos.y));
    }

    // Re-derive + re-execute: the seed misses, so the fallback must pick the
    // SAVED footprint (region A alone → body volume ~500), NOT all regions
    // (A + B → ~1000, the catastrophe that would sweep unrelated feature B).
    ASSERT_TRUE(rop->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(rop->execute(doc));
    int rbid = -1;
    for (int id : doc.getAllBodyIds()) rbid = id;
    EXPECT_NEAR(vol(doc.getBody(rbid)), 500.0, 5.0)
        << "seed miss swept the wrong regions (ALL-regions catastrophe): "
           "body volume " << vol(doc.getBody(rbid));
}

// A hole resized in its sketch must be followed by the body. The extrude's
// recorded region points are a grid over the ORIGINAL region; shrinking the
// circle leaves some of them outside every face, and that used to make the
// rebuild fall back to the saved (old-size) footprint - the 8 mm hole stayed
// 8 mm while the sketch said 6.
TEST(ExtrudeRecoveredProfile, ShrinkingARegionIsFollowedNotRefused) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    const int centre = sk->addPoint({0.0f, 0.0f});
    const int circle = sk->addCircle(centre, 4.0);
    const int sid = doc.addSketch(sk, "Sketch 1");

    // Region points the way a real extrude records them: a grid over the disc,
    // including points between radius 3 and 4.
    const char* params =
        "sketch=1;dist=5;dir=0;mode=0;target=-1;draft=0;"
        "regions=0:0,0:2,0:-2,0:3.5,0:-3.5,3.5:0,-3.5:0,2.5:2.5,-2.5:2.5,2.5:-2.5,-2.5:-2.5";
    auto op = std::make_unique<ExtrudeOp>();
    ASSERT_TRUE(op->deserializeParams(params));
    op->setSketchSource(sid);
    ASSERT_TRUE(op->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(op->execute(doc));
    int bid = -1;
    for (int id : doc.getAllBodyIds()) bid = id;
    ASSERT_GE(bid, 0);
    const TopoDS_Shape original = doc.getBody(bid);
    EXPECT_NEAR(vol(original), M_PI * 16.0 * 5.0, 0.5);

    // Reload-style op, with the saved footprint populated.
    auto rop = std::make_unique<ExtrudeOp>();
    ASSERT_TRUE(rop->deserializeParams(params));
    rop->setSketchSource(sid);
    Operation::ReloadState rs;
    rs.created.push_back(bid);
    rs.createdAfter.push_back({bid, original});
    ASSERT_TRUE(rop->rehydrateFromReload(rs, doc));

    sk->setCircleRadius(circle, 3.0);          // 8 mm -> 6 mm
    ASSERT_TRUE(rop->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(rop->execute(doc));
    int rbid = -1;
    for (int id : doc.getAllBodyIds()) rbid = id;
    EXPECT_NEAR(vol(doc.getBody(rbid)), M_PI * 9.0 * 5.0, 0.5)
        << "the body kept the old radius: body volume " << vol(doc.getBody(rbid));
}

// A footprint recovered from a cut through a curved body covers only SOME of
// the cut's regions. The ring of small holes must not be swapped out for it.
TEST(ExtrudeRecoveredProfile, FootprintCoveringOnlyOneRegionDoesNotEraseTheOthers) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    sk->addCircle(sk->addPoint({0.0f, 0.0f}), 4.0);      // the only one the footprint knows
    sk->addCircle(sk->addPoint({20.0f, 0.0f}), 3.0);
    sk->addCircle(sk->addPoint({-20.0f, 0.0f}), 3.0);
    const int sid = doc.addSketch(sk, "Sketch 1");

    // A body whose planar footprint on z=0 is ONLY the central disc (the small
    // holes' caps would lie off the plane on a curved body).
    TopoDS_Shape disc;
    {
        auto one = std::make_shared<Sketch>();
        one->setPlane(sk->getPlane());
        one->addCircle(one->addPoint({0.0f, 0.0f}), 4.0);
        Document d2; const int s2 = d2.addSketch(one, "S");
        auto e = std::make_unique<ExtrudeOp>();
        e->setSketchSource(s2); e->setDistance(5.0); e->setMode(ExtrudeMode::NewBody);
        ASSERT_TRUE(e->rebuildProfileFromSketch(d2));
        ASSERT_TRUE(e->execute(d2));
        for (int id : d2.getAllBodyIds()) disc = d2.getBody(id);
    }

    const char* params =
        "sketch=1;dist=5;dir=0;mode=0;target=-1;draft=0;"
        "regions=0:0,0:2,2:0,20:0,19:0,21:0,-20:0,-19:0,-21:0";
    auto rop = std::make_unique<ExtrudeOp>();
    ASSERT_TRUE(rop->deserializeParams(params));
    rop->setSketchSource(sid);
    Operation::ReloadState rs;
    rs.created.push_back(1);
    rs.createdAfter.push_back({1, disc});
    ASSERT_TRUE(rop->rehydrateFromReload(rs, doc));

    ASSERT_TRUE(rop->rebuildProfileFromSketch(doc));
    ASSERT_TRUE(rop->execute(doc));
    int bid = -1;
    for (int id : doc.getAllBodyIds()) bid = id;
    const double expect = (M_PI * 16.0 + 2.0 * M_PI * 9.0) * 5.0;
    EXPECT_NEAR(vol(doc.getBody(bid)), expect, 1.0)
        << "the small circles were swapped for the footprint: " << vol(doc.getBody(bid));
}
