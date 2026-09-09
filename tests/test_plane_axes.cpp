// The gizmo's in-plane axes must not depend on which way a face normal
// happens to point. Steve, 2026-08-03: holes moving in x/z and y/z were fine
// but x/y "the gizmo buttons are reversed" - because axis B used to be
// cross(N, A), which flips with N, and buildVoid resolved those holes' entry
// mouth to the underside (N = -Z), putting the green arrow on -Y.
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cstdio>
#include <string>
#include "core/PlaneAxes.h"

using materializr::inPlaneAxes;

namespace {

// Which world axis this direction is, as a signed report: +1/-1 per component.
std::string axisName(const glm::vec3& v) {
    const char* n[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        if (v[i] > 0.99f)  return std::string("+") + n[i];
        if (v[i] < -0.99f) return std::string("-") + n[i];
    }
    return "oblique";
}

} // namespace

// THE BUG: flipping the normal must not flip an arrow. Every axis-aligned
// plane, both facings, has to give the same two in-plane directions.
TEST(PlaneAxes, FlippingTheNormalKeepsTheSameArrows) {
    const glm::vec3 normals[3] = {{1,0,0}, {0,1,0}, {0,0,1}};
    for (const glm::vec3& n : normals) {
        glm::vec3 aPos, bPos, aNeg, bNeg;
        inPlaneAxes(n, aPos, bPos);
        inPlaneAxes(-n, aNeg, bNeg);
        std::printf("  N=%s -> (%s, %s)   N=%s -> (%s, %s)\n",
                    axisName(n).c_str(), axisName(aPos).c_str(), axisName(bPos).c_str(),
                    axisName(-n).c_str(), axisName(aNeg).c_str(), axisName(bNeg).c_str());
        EXPECT_NEAR(glm::dot(aPos, aNeg), 1.0f, 1e-5f)
            << "axis A flipped when the normal did";
        EXPECT_NEAR(glm::dot(bPos, bNeg), 1.0f, 1e-5f)
            << "axis B flipped when the normal did";
    }
}

// The arrows are coloured by the world axis they most align with, so they have
// to point along the POSITIVE one or the colour lies about the direction.
TEST(PlaneAxes, AxesPointAlongPositiveWorldAxes) {
    struct Case { glm::vec3 n; glm::vec3 wantA; glm::vec3 wantB; };
    const Case cases[] = {
        {{0,0,1},  {1,0,0}, {0,1,0}},   // hole through the top   -> X, Y
        {{0,0,-1}, {1,0,0}, {0,1,0}},   // ...and from underneath -> same
        {{0,1,0},  {1,0,0}, {0,0,1}},   // through a side         -> X, Z
        {{0,-1,0}, {1,0,0}, {0,0,1}},
        {{1,0,0},  {0,1,0}, {0,0,1}},   // through the end        -> Y, Z
        {{-1,0,0}, {0,1,0}, {0,0,1}},
    };
    for (const Case& c : cases) {
        glm::vec3 a, b;
        inPlaneAxes(c.n, a, b);
        EXPECT_NEAR(glm::dot(a, c.wantA), 1.0f, 1e-5f)
            << "N=" << axisName(c.n) << " axis A came out " << axisName(a);
        EXPECT_NEAR(glm::dot(b, c.wantB), 1.0f, 1e-5f)
            << "N=" << axisName(c.n) << " axis B came out " << axisName(b);
    }
}

// Still a usable basis: orthonormal and genuinely in the plane.
TEST(PlaneAxes, StaysOrthonormalAndInPlane) {
    const glm::vec3 normals[] = {
        {0,0,1}, {0,0,-1}, {0,1,0}, {1,0,0},
        glm::normalize(glm::vec3(1,1,0)),
        glm::normalize(glm::vec3(1,2,3)),
        glm::normalize(glm::vec3(-4,1,-2)),
    };
    for (const glm::vec3& n : normals) {
        glm::vec3 a, b;
        inPlaneAxes(n, a, b);
        EXPECT_NEAR(glm::length(a), 1.0f, 1e-5f);
        EXPECT_NEAR(glm::length(b), 1.0f, 1e-5f);
        EXPECT_NEAR(glm::dot(a, n), 0.0f, 1e-5f) << "axis A left the plane";
        EXPECT_NEAR(glm::dot(b, n), 0.0f, 1e-5f) << "axis B left the plane";
        EXPECT_NEAR(glm::dot(a, b), 0.0f, 1e-5f) << "axes are not perpendicular";
    }
}
