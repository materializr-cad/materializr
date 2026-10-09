// SketchRegionWorker (#130): sketch regions built off the main thread.
//
// The first viewport click used to run Sketch::buildRegions' general fuse on
// the main thread - minutes on a tablet for a sketch lying along a B-spline
// host face's edges. Picking now asks the worker. The contract:
//   - an off-thread build adopts the SAME regions a main-thread build makes;
//   - a host face the fuse passes through unsplit comes back as the LIVE
//     face, not the worker's private copy of it;
//   - a build is only adopted by the geometry it was made for, and a newer
//     request for the same sketch replaces an older queued one;
//   - nothing is built until asked, and an unready sketch reports so.

#include "modeling/Sketch.h"
#include "modeling/SketchRegionWorker.h"

#include <gtest/gtest.h>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <vector>

using materializr::Sketch;
using materializr::SketchRegionWorker;

namespace {

constexpr int kGenerousMs = 30000; // a CI box under load; these builds take ms

gp_Pln xyPlane(double z = 0.0) {
    return gp_Pln(gp_Ax3(gp_Pnt(0, 0, z), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
}

// Two overlapping rectangles: the fuse makes three regions (two crescents
// and the lens), which exercises the general fuse rather than a lone face.
void overlappingRects(Sketch& sk) {
    sk.setPlane(xyPlane());
    sk.addRectangle(glm::vec2(0, 0), glm::vec2(40, 20));
    sk.addRectangle(glm::vec2(20, 10), glm::vec2(60, 30));
}

std::vector<double> sortedAreas(const std::vector<Sketch::Region>& regions) {
    std::vector<double> a;
    for (const auto& r : regions) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(r.face, g);
        a.push_back(std::abs(g.Mass()));
    }
    std::sort(a.begin(), a.end());
    return a;
}

// The +Z face of a 100x100x10 box: a host face for a sketch at z = 10.
TopoDS_Face boxTopFace() {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(-20, -20, 0), 100, 100, 10).Shape();
    for (TopExp_Explorer fx(box, TopAbs_FACE); fx.More(); fx.Next()) {
        TopoDS_Face f = TopoDS::Face(fx.Current());
        GProp_GProps g;
        BRepGProp::SurfaceProperties(f, g);
        if (std::abs(g.CentreOfMass().Z() - 10.0) < 1e-6) return f;
    }
    return TopoDS_Face();
}

} // namespace

TEST(SketchRegionWorker, OffThreadBuildMatchesMainThreadBuild) {
    Sketch reference;
    overlappingRects(reference);
    const auto expected = sortedAreas(reference.buildRegions());
    ASSERT_EQ(expected.size(), 3u) << "two overlapping rectangles = three regions";

    Sketch sk;
    overlappingRects(sk);
    SketchRegionWorker worker;
    ASSERT_TRUE(worker.ensureWithin(sk, kGenerousMs));
    ASSERT_TRUE(sk.regionsCached()) << "the result must land in the sketch's own cache";

    const auto got = sortedAreas(sk.buildRegions());
    ASSERT_EQ(got.size(), expected.size());
    for (size_t i = 0; i < got.size(); ++i)
        EXPECT_NEAR(got[i], expected[i], 1e-6) << "region " << i;
}

TEST(SketchRegionWorker, NothingBuiltUntilAskedAndPendingIsReported) {
    Sketch sk;
    overlappingRects(sk);
    SketchRegionWorker worker;
    worker.pause();
    EXPECT_FALSE(sk.regionsCached());
    EXPECT_FALSE(worker.building(sk));

    EXPECT_FALSE(worker.ensure(sk)) << "a cold sketch is not ready on the first ask";
    EXPECT_TRUE(worker.building(sk));
    EXPECT_FALSE(worker.ensureWithin(sk, 20)) << "paused: the wait must give up on time";
    EXPECT_FALSE(sk.regionsCached());
    EXPECT_EQ(worker.pending(), 1u) << "asking again must not queue a second build";

    worker.resume();
    ASSERT_TRUE(worker.ensureWithin(sk, kGenerousMs));
    EXPECT_FALSE(worker.building(sk));
    EXPECT_EQ(sk.buildRegions().size(), 3u);
}

TEST(SketchRegionWorker, StaleBuildIsNotAdoptedAndNewerRequestReplacesIt) {
    Sketch sk;
    overlappingRects(sk);
    SketchRegionWorker worker;
    worker.pause();
    EXPECT_FALSE(worker.ensure(sk));
    const uint64_t before = sk.regionKey();

    // Edit the sketch while its build is still queued: a third, disjoint
    // rectangle. The old build is for geometry that no longer exists.
    sk.addRectangle(glm::vec2(100, 0), glm::vec2(120, 20));
    ASSERT_NE(sk.regionKey(), before);
    EXPECT_FALSE(worker.ensure(sk));
    EXPECT_EQ(worker.pending(), 1u) << "the newer request replaces the queued one";

    worker.resume();
    ASSERT_TRUE(worker.ensureWithin(sk, kGenerousMs));
    EXPECT_EQ(sk.buildRegions().size(), 4u) << "regions are for the CURRENT geometry";

    // And a result can never be installed under someone else's key.
    EXPECT_FALSE(sk.adoptRegions(before, {}));
    EXPECT_EQ(sk.buildRegions().size(), 4u);
}

TEST(SketchRegionWorker, UnsplitHostFaceComesBackAsTheLiveFace) {
    const TopoDS_Face host = boxTopFace();
    ASSERT_FALSE(host.IsNull());

    // A rectangle wholly OFF the host face: the fuse splits neither, so the
    // host face passes through as itself. On the worker that is its private
    // copy, which must be mapped back to the live face on adoption.
    Sketch sk;
    sk.setPlane(xyPlane(10.0));
    sk.setSourceFace(host);
    sk.addRectangle(glm::vec2(200, 200), glm::vec2(220, 220));

    Sketch reference = sk;
    const auto ref = reference.buildRegions();
    bool refHasHost = false;
    for (const auto& r : ref) refHasHost |= r.face.IsSame(host);
    ASSERT_TRUE(refHasHost) << "precondition: the main-thread build returns the live host face";

    SketchRegionWorker worker;
    ASSERT_TRUE(worker.ensureWithin(sk, kGenerousMs));
    const auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), ref.size());
    int hostRegions = 0;
    for (const auto& r : regions) {
        if (!r.face.IsSame(host)) continue;
        ++hostRegions;
        EXPECT_TRUE(r.outerWire.IsNull() == false);
        bool wireOnLiveFace = false;
        for (TopExp_Explorer w(host, TopAbs_WIRE); w.More(); w.Next())
            wireOnLiveFace |= w.Current().IsSame(r.outerWire);
        EXPECT_TRUE(wireOnLiveFace) << "the outer wire must be the live face's too";
    }
    EXPECT_EQ(hostRegions, 1);
}

TEST(SketchRegionWorker, HostFaceSplitBySketchMatchesMainThread) {
    const TopoDS_Face host = boxTopFace();
    ASSERT_FALSE(host.IsNull());
    Sketch sk;
    sk.setPlane(xyPlane(10.0));
    sk.setSourceFace(host);
    sk.addRectangle(glm::vec2(0, 0), glm::vec2(30, 30));

    Sketch reference = sk;
    const auto refRegions = reference.buildRegions();
    const auto expected = sortedAreas(refRegions);

    SketchRegionWorker worker;
    ASSERT_TRUE(worker.ensureWithin(sk, kGenerousMs));
    const auto regions = sk.buildRegions();
    const auto got = sortedAreas(regions);
    ASSERT_EQ(got.size(), expected.size());
    for (size_t i = 0; i < got.size(); ++i)
        EXPECT_NEAR(got[i], expected[i], 1e-6) << "region " << i;

    // Same ORDER too: a push/pull stores its region by index and rebuilds
    // the profile from that index later (PushPullOp::rebuildProfileFromSketch),
    // possibly from a main-thread build. The worker fuses a COPY of the host
    // face, so this holds only if the fuse's output order does not depend on
    // which TShape it was handed.
    for (size_t i = 0; i < regions.size(); ++i) {
        const glm::vec2 a = regions[i].representativePoint;
        const glm::vec2 b = refRegions[i].representativePoint;
        EXPECT_NEAR(a.x, b.x, 1e-3) << "region " << i;
        EXPECT_NEAR(a.y, b.y, 1e-3) << "region " << i;
    }
}

TEST(SketchRegionWorker, DestructorJoinsWithBuildsStillQueued) {
    Sketch a, b;
    overlappingRects(a);
    overlappingRects(b);
    b.addRectangle(glm::vec2(100, 0), glm::vec2(120, 20));
    {
        SketchRegionWorker worker(1);
        worker.pause();
        worker.ensure(a);
        worker.ensure(b);
        EXPECT_EQ(worker.pending(), 2u);
    } // must return, and must not touch a or b afterwards
    EXPECT_FALSE(a.regionsCached());
    EXPECT_FALSE(b.regionsCached());
}
