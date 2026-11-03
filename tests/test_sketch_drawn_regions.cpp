// A sketch started on a body face carries the host face's leftover area as one
// more selectable region. Push/Pull on the WHOLE sketch must act on what was
// drawn only - otherwise an 8 mm hole in a plate cuts the entire plate face
// (the leftover rode along as an extra target).
//
// drawnRegions() is the filter: regions inside at least one drawn closed loop.

#include "modeling/Sketch.h"

#include <gtest/gtest.h>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

using materializr::Sketch;

namespace {

gp_Pln xyPlane() {
    return gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
}

// A 40 x 40 host face centred on the origin, on the sketch plane.
TopoDS_Face hostFace() {
    return BRepBuilderAPI_MakeFace(xyPlane(), -20.0, 20.0, -20.0, 20.0).Face();
}

void addCircleAt(Sketch& sk, glm::vec2 c, double r) {
    sk.addCircle(sk.addPoint(c), r);
}

} // namespace

TEST(SketchDrawnRegions, HostLeftoverIsRegionButNotDrawn) {
    Sketch sk;
    sk.setPlane(xyPlane());
    sk.setSourceFace(hostFace());
    addCircleAt(sk, {0, 0}, 4.0);
    addCircleAt(sk, {10, 0}, 1.5);

    auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 3u) << "two circles + the host face's leftover";

    auto drawn = sk.drawnRegions(regions);
    ASSERT_EQ(drawn.size(), 2u) << "the leftover must not count as drawn";
    for (int i : drawn) {
        const glm::vec2 p = regions[i].representativePoint;
        const bool inBig   = glm::length(p - glm::vec2(0, 0))  < 4.0f;
        const bool inSmall = glm::length(p - glm::vec2(10, 0)) < 1.5f;
        EXPECT_TRUE(inBig || inSmall) << "drawn region " << i << " is outside every circle";
    }
}

TEST(SketchDrawnRegions, NestedCirclesBothDrawn) {
    // A ring and its inner disc are both drawn; only the host leftover drops.
    Sketch sk;
    sk.setPlane(xyPlane());
    sk.setSourceFace(hostFace());
    addCircleAt(sk, {0, 0}, 10.0);
    addCircleAt(sk, {0, 0}, 4.0);

    auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 3u) << "inner disc, ring, host leftover";
    EXPECT_EQ(sk.drawnRegions(regions).size(), 2u);
}

TEST(SketchDrawnRegions, NoHostFaceMeansEveryRegionIsDrawn) {
    Sketch sk;
    sk.setPlane(xyPlane());
    addCircleAt(sk, {0, 0}, 4.0);
    addCircleAt(sk, {20, 0}, 3.0);

    auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(sk.drawnRegions(regions).size(), 2u);
}

TEST(SketchDrawnRegions, EmptyInputGivesEmpty) {
    Sketch sk;
    sk.setPlane(xyPlane());
    EXPECT_TRUE(sk.drawnRegions({}).empty());
}

// 11-3-26 (1.8.0.b2_2, Sketch 2): two thin crescents, each closed by a short
// straight cap. The area centroid of a crescent is outside it, so the
// representative point fell back to the middle of the wire's FIRST edge - the
// cap - i.e. exactly ON the boundary, where the point-in-polygon test is a coin
// toss. One crescent lost its tint; both were perfectly good regions.
TEST(SketchDrawnRegions, ThinCrescentsGetAnInteriorRepresentativePoint) {
    Sketch sk;
    sk.setPlane(xyPlane());
    const int c = sk.addPoint({0, 0});
    const float yc = 4.58f;
    auto crescent = [&](float sign) {
        // outer r=20 / inner r=19, from x=-a..+a, closed by two horizontal caps
        const float xo = std::sqrt(20.0f * 20.0f - yc * yc);
        const float xi = std::sqrt(19.0f * 19.0f - yc * yc);
        const float y = sign * yc;
        const int o1 = sk.addPoint({-xo, y}), o2 = sk.addPoint({xo, y});
        const int i1 = sk.addPoint({-xi, y}), i2 = sk.addPoint({xi, y});
        if (sign < 0) {
            sk.addArc(c, o1, o2, 20.0);
            sk.addArc(c, i1, i2, 19.0);
        } else {
            sk.addArc(c, o2, o1, 20.0);
            sk.addArc(c, i2, i1, 19.0);
        }
        sk.addLine(o1, i1);
        sk.addLine(o2, i2);
    };
    crescent(-1.0f);
    crescent(+1.0f);

    auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 2u);
    for (const auto& r : regions)
        EXPECT_TRUE(sk.isPointInRegion(r, r.representativePoint))
            << "representative point must be inside its region, not on its edge";
    EXPECT_EQ(sk.drawnRegions(regions).size(), 2u) << "both closed contours are drawn";
}
