// Sketch regions: the near-coincident-edge tolerance, and regions' identity
// across renumbering.
//
// A sketch line drawn along a host-face edge lands a hair off it (sketch points
// are float32). OCCT's general fuse then split the host face into sliver
// regions and, on a lofted B-spline plate, spent 25 s doing it; the build now
// treats edges within Sketch's kRegionFuzzMm as coincident. That renumbers a
// sketch's regions (199 became 5 on the robot dog), so the ops that persist a
// region also persist its representative point and look the region up by it.
//
//   - the fuzzy build merges a sliver that a hair of overhang would have made;
//   - Sketch::regionAtAnchor finds a region by a point inside it;
//   - PushPullOp follows its saved point, not its saved index, and still
//     accepts a legacy blob that has only the index;
//   - ProjectSketchOp round-trips its filter's points.

#include "core/Document.h"
#include "modeling/ProjectSketchOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/Sketch.h"

#include <gtest/gtest.h>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <memory>
#include <string>

using materializr::Sketch;

namespace {

gp_Pln groundPlane() {
    return gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
}

double areaOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(s, g);
    return std::abs(g.Mass());
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return std::abs(g.Mass());
}

// Two disjoint rectangles of different size: A = 10x10 (area 100), B = 20x20
// (area 400), so a region is recognisable by its area whatever its index is.
std::shared_ptr<Sketch> twoRects() {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    sk->addRectangle(glm::vec2(0, 0), glm::vec2(10, 10));
    sk->addRectangle(glm::vec2(30, 0), glm::vec2(50, 20));
    return sk;
}

int regionWithArea(const std::vector<Sketch::Region>& regions, double area) {
    for (size_t i = 0; i < regions.size(); ++i)
        if (std::abs(areaOf(regions[i].face) - area) < 0.5) return static_cast<int>(i);
    return -1;
}

} // namespace

TEST(RegionFuzz, ASliverFromAHairOfOverhangIsMergedAway) {
    // The sketch rectangle's left edge sits 3e-4 mm OUTSIDE the host's left edge:
    // the exact build makes a 3e-4 x 80 strip region (0.024 mm^2) there.
    Sketch sk;
    sk.setPlane(groundPlane());
    sk.setSourceFace(BRepBuilderAPI_MakeFace(groundPlane(), 0.0, 100.0, 0.0, 100.0).Face());
    sk.addRectangle(glm::vec2(-0.0003f, 10.0f), glm::vec2(60.0f, 90.0f));

    const auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 2u) << "the rectangle and the rest of the host face - no sliver";
    for (const auto& r : regions)
        EXPECT_GT(areaOf(r.face), 1.0) << "a sliver region survived the fuzzy build";
}

TEST(RegionAnchor, RegionAtAnchorFindsTheRegionHoldingThePoint) {
    auto sk = twoRects();
    const auto regions = sk->buildRegions();
    ASSERT_EQ(regions.size(), 2u);
    const int a = regionWithArea(regions, 100.0), b = regionWithArea(regions, 400.0);
    ASSERT_GE(a, 0); ASSERT_GE(b, 0);
    EXPECT_EQ(sk->regionAtAnchor(regions, glm::vec2(5, 5)), a);
    EXPECT_EQ(sk->regionAtAnchor(regions, glm::vec2(40, 10)), b);
    EXPECT_EQ(sk->regionAtAnchor(regions, glm::vec2(20, 5)), -1) << "between the rectangles";
    // A region's own representative point is, by construction, in that region.
    for (size_t i = 0; i < regions.size(); ++i)
        EXPECT_EQ(sk->regionAtAnchor(regions, regions[i].representativePoint), static_cast<int>(i));
}

namespace {

// A free-floating push/pull of `dist` from region `idx`, naming it with `anchor`
// when given; rebuilt from the sketch the way a reload or a cascade does, then
// executed. Returns the volume of the body it made.
double pushedVolume(int idx, const glm::vec2* anchor, double dist = 5.0) {
    Document doc;
    auto sk = twoRects();
    const int sid = doc.addSketch(sk);
    const auto regions = sk->buildRegions();

    PushPullOp op;
    PushPullOp::Target t;
    t.profile = regions[idx].face;   // replaced by the rebuild below
    op.setTargets({t});
    op.setDistance(dist);
    if (anchor) op.setSketchSource(0, sid, idx, *anchor);
    else        op.setSketchSource(0, sid, idx);
    EXPECT_TRUE(op.rebuildProfileFromSketch(doc, sid));
    EXPECT_TRUE(op.execute(doc));
    double v = 0.0;
    for (int id : doc.getAllBodyIds()) v += volumeOf(doc.getBody(id));
    return v;
}

} // namespace

TEST(RegionAnchor, PushPullFollowsItsPointNotItsIndex) {
    auto probe = twoRects();
    const auto regions = probe->buildRegions();
    const int a = regionWithArea(regions, 100.0), b = regionWithArea(regions, 400.0);
    ASSERT_GE(a, 0); ASSERT_GE(b, 0);

    // Legacy: only the index, so the index is what is followed.
    EXPECT_NEAR(pushedVolume(a, nullptr), 100.0 * 5.0, 1.0);
    EXPECT_NEAR(pushedVolume(b, nullptr), 400.0 * 5.0, 1.0);

    // The index says A but the saved point is inside B: the numbering has moved
    // since the index was taken, and the point wins.
    const glm::vec2 insideB = regions[b].representativePoint;
    EXPECT_NEAR(pushedVolume(a, &insideB), 400.0 * 5.0, 1.0);

    // A point that is in no region (the sketch was edited away from it) falls
    // back to the index, as a legacy op does.
    const glm::vec2 nowhere(20.0f, 5.0f);
    EXPECT_NEAR(pushedVolume(a, &nowhere), 100.0 * 5.0, 1.0);
}

TEST(RegionAnchor, PushPullSavesItsPointAndStillReadsALegacyBlob) {
    PushPullOp op;
    PushPullOp::Target t;
    op.setTargets({t});
    op.setDistance(3.0);
    op.setSketchSource(0, 7, 12, glm::vec2(1.5f, -2.25f));
    const std::string blob = op.serializeParams();
    ASSERT_NE(blob.find(";a0=1.500000:-2.250000"), std::string::npos) << blob;

    PushPullOp back;
    ASSERT_TRUE(back.deserializeParams(blob));
    EXPECT_EQ(back.serializeParams(), blob) << "the point must survive a round trip";

    // A blob written before points existed: s/r/b only.
    PushPullOp legacy;
    ASSERT_TRUE(legacy.deserializeParams("dist=3.000000;sym=0;cut=0;count=1;s0=7;r0=12;b0=-1"));
    const std::string out = legacy.serializeParams();
    EXPECT_EQ(out.find(";a0="), std::string::npos) << "no point was saved, so none is invented";
    EXPECT_NE(out.find(";r0=12"), std::string::npos) << "the index is kept";
    EXPECT_EQ(legacy.getSketchIdAt(0), 7);
}

TEST(RegionAnchor, ProjectSketchFilterKeepsItsPointsThroughASaveAndLoad) {
    ProjectSketchOp op;
    op.setSketchId(2);
    op.setBody(3);
    op.setRegionFilter({4, 9}, {glm::vec2(1.0f, 2.0f), glm::vec2(-3.5f, 0.25f)});
    const std::string blob = op.serializeParams();
    ASSERT_NE(blob.find(";regions=4,9"), std::string::npos) << blob;
    ASSERT_NE(blob.find(";ranch=1.000000:2.000000,-3.500000:0.250000"), std::string::npos) << blob;

    ProjectSketchOp back;
    ASSERT_TRUE(back.deserializeParams(blob));
    EXPECT_EQ(back.serializeParams(), blob);

    // A filter with no points (an old file) stays index-only.
    ProjectSketchOp legacy;
    ASSERT_TRUE(legacy.deserializeParams("body=3;sketch=2;depth=1.000000;mode=0;regions=4,9"));
    EXPECT_EQ(legacy.serializeParams().find(";ranch="), std::string::npos);

    // A points list that does not match the filter's length is not trusted.
    ProjectSketchOp mismatched;
    mismatched.setRegionFilter({1, 2, 3}, {glm::vec2(0.0f, 0.0f)});
    EXPECT_EQ(mismatched.serializeParams().find(";ranch="), std::string::npos);
}
