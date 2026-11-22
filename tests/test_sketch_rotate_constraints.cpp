// Rotating a selection with the sketch gizmo must not fight the constraints
// that pin it to the axes.
//
// A rectangle drawn with the Rectangle tool is four sides held Horizontal /
// Vertical plus two dimensions. The gizmo turned its points and then re-solved
// with those constraints still in place, so at a quarter turn the solver was
// asked for a side that was horizontal and vertical at once and collapsed the
// shape onto a line (#187).

#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>

using materializr::Constraint;
using materializr::ConstraintType;
using materializr::Sketch;
using materializr::SketchSolver;

namespace {

struct Rect {
    Sketch sk;
    int p[4];
    int l[4];
};

void addC(Sketch& sk, ConstraintType t, int a, int b = -1, double v = 0.0) {
    Constraint c{};
    c.type = t; c.entityA = a; c.entityB = b; c.value = v;
    sk.addConstraint(c);
}

void build(Rect& r) {
    r.p[0] = r.sk.addPoint({10.0f, 5.0f});
    r.p[1] = r.sk.addPoint({-10.0f, 5.0f});
    r.p[2] = r.sk.addPoint({-10.0f, -5.0f});
    r.p[3] = r.sk.addPoint({10.0f, -5.0f});
    for (int i = 0; i < 4; ++i) r.l[i] = r.sk.addLine(r.p[i], r.p[(i + 1) % 4]);
    addC(r.sk, ConstraintType::Horizontal, r.l[0]);
    addC(r.sk, ConstraintType::Vertical,   r.l[1]);
    addC(r.sk, ConstraintType::Horizontal, r.l[2]);
    addC(r.sk, ConstraintType::Vertical,   r.l[3]);
    addC(r.sk, ConstraintType::Distance, r.p[0], r.p[1], 20.0);
    addC(r.sk, ConstraintType::Distance, r.p[1], r.p[2], 10.0);
}

glm::vec2 at(const Sketch& sk, int id) { return sk.getPoint(id)->pos; }

void expectRect(const Rect& r) {
    EXPECT_NEAR(glm::length(at(r.sk, r.p[1]) - at(r.sk, r.p[0])), 20.0f, 1e-2f);
    EXPECT_NEAR(glm::length(at(r.sk, r.p[2]) - at(r.sk, r.p[1])), 10.0f, 1e-2f);
    EXPECT_NEAR(glm::length(at(r.sk, r.p[3]) - at(r.sk, r.p[2])), 20.0f, 1e-2f);
    EXPECT_NEAR(glm::length(at(r.sk, r.p[0]) - at(r.sk, r.p[3])), 10.0f, 1e-2f);
    glm::vec2 a = at(r.sk, r.p[1]) - at(r.sk, r.p[0]);
    glm::vec2 b = at(r.sk, r.p[2]) - at(r.sk, r.p[1]);
    EXPECT_NEAR(glm::dot(glm::normalize(a), glm::normalize(b)), 0.0f, 1e-3f);
}

void rotate(Rect& r, double deg) {
    std::vector<std::pair<int, glm::vec2>> orig;
    for (int id : r.p) orig.push_back({id, r.sk.getPoint(id)->pos});
    const auto base = r.sk.getConstraints();
    r.sk.rotatePointsAbout(orig, {0.0f, 0.0f}, deg * M_PI / 180.0, base);
    SketchSolver solver;
    solver.solve(r.sk);
}

} // namespace

TEST(SketchRotateConstraints, QuarterTurnKeepsTheRectangle) {
    Rect r; build(r);
    rotate(r, 90.0);
    expectRect(r);
    // 90 degrees swaps the sides: the 20 mm side now runs vertically.
    EXPECT_NEAR(std::abs(at(r.sk, r.p[1]).x - at(r.sk, r.p[0]).x), 0.0f, 1e-2f);
    EXPECT_NEAR(std::abs(at(r.sk, r.p[1]).y - at(r.sk, r.p[0]).y), 20.0f, 1e-2f);
}

TEST(SketchRotateConstraints, ThreeQuartersAndHalfTurnsToo) {
    { Rect r; build(r); rotate(r, 270.0); expectRect(r); }
    { Rect r; build(r); rotate(r, -90.0); expectRect(r); }
    { Rect r; build(r); rotate(r, 180.0); expectRect(r); }
}

TEST(SketchRotateConstraints, AnyOtherAngleKeepsTheRectangle) {
    for (double deg : {15.0, 45.0, 60.0, 135.0, 200.0}) {
        Rect r; build(r);
        rotate(r, deg);
        expectRect(r);
        glm::vec2 a = at(r.sk, r.p[1]) - at(r.sk, r.p[0]);
        double got = std::atan2(a.y, a.x) * 180.0 / M_PI;
        double want = std::atan2(0.0, -1.0) * 180.0 / M_PI + deg;
        double diff = std::fmod(got - want + 540.0, 360.0) - 180.0;
        EXPECT_NEAR(diff, 0.0, 0.1) << "deg " << deg;
    }
}

TEST(SketchRotateConstraints, ReapplyingStartsFromTheOriginalConstraints) {
    // The gizmo calls this every frame and again for a typed angle; the
    // second call must not build on the first one's rewrites.
    Rect r; build(r);
    std::vector<std::pair<int, glm::vec2>> orig;
    for (int id : r.p) orig.push_back({id, r.sk.getPoint(id)->pos});
    const auto base = r.sk.getConstraints();
    r.sk.rotatePointsAbout(orig, {0.0f, 0.0f}, 0.7, base);
    r.sk.rotatePointsAbout(orig, {0.0f, 0.0f}, 0.0, base);
    ASSERT_EQ(r.sk.getConstraints().size(), base.size());
    for (size_t i = 0; i < base.size(); ++i)
        EXPECT_EQ(r.sk.getConstraints()[i].type, base[i].type);
    SketchSolver solver; solver.solve(r.sk);
    expectRect(r);
}
