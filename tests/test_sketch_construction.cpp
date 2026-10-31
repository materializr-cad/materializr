// Sketch construction geometry: reference lines, circles, arcs, splines and
// polygons that help you draw but are not part of the shape.
//
// The contract is that they have NO effect on anything the sketch produces.
// A sketch with construction geometry must yield exactly what the same sketch
// without it yields: the same regions, the same profile wires, the same
// extrudable face. These tests pin that for every element kind, across the
// places a builder could forget it (regions, wires, profile shape, the region
// cache), and for the two ways a construction element can still leak in: a
// point only it uses splitting a real edge, and a flag that flips without the
// cache noticing.
#include "io/ProjectIO.h"
#include "modeling/Sketch.h"

#include <gtest/gtest.h>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <functional>
#include <sstream>
#include <vector>

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

int edgeCount(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_EDGE); e.More(); e.Next()) ++n;
    return n;
}

// What a sketch produces, boiled down to numbers that must not move.
struct Output {
    size_t regions = 0;
    double regionArea = 0.0;
    size_t wires = 0;
    double profileArea = 0.0;
    int outerEdges = 0;   // edges on the first region's outer wire
};

Output outputOf(const Sketch& sk) {
    Output o;
    const auto regions = sk.buildRegions();
    o.regions = regions.size();
    for (const auto& r : regions) o.regionArea += areaOf(r.face);
    if (!regions.empty()) o.outerEdges = edgeCount(regions.front().outerWire);
    o.wires = sk.buildWires().size();
    o.profileArea = areaOf(sk.buildProfileShape());
    return o;
}

void expectSame(const Output& a, const Output& b, const char* what) {
    EXPECT_EQ(a.regions, b.regions) << what;
    EXPECT_NEAR(a.regionArea, b.regionArea, 1e-6) << what;
    EXPECT_EQ(a.wires, b.wires) << what;
    EXPECT_NEAR(a.profileArea, b.profileArea, 1e-6) << what;
    EXPECT_EQ(a.outerEdges, b.outerEdges) << what;
}

// A 20x20 square: one region of area 400, one wire, four outer edges.
Sketch square() {
    Sketch sk;
    sk.setPlane(groundPlane());
    sk.addRectangle(glm::vec2(0, 0), glm::vec2(20, 20));
    return sk;
}

// Add something to `sk` and return the id of the element to mark.
using Adder = std::function<int(Sketch&)>;

int addLineAcross(Sketch& sk) {        // diagonal through the square
    return sk.addLine(sk.addPoint({-5, -5}), sk.addPoint({25, 25}));
}
int addLineInside(Sketch& sk) {        // wholly inside, touching nothing
    return sk.addLine(sk.addPoint({5, 5}), sk.addPoint({15, 8}));
}
int addCircleInside(Sketch& sk) {      // a hole, if it were real
    return sk.addCircle(sk.addPoint({10, 10}), 4.0);
}
int addCircleCrossing(Sketch& sk) {    // straddles an edge
    return sk.addCircle(sk.addPoint({20, 10}), 6.0);
}
int addArcInside(Sketch& sk) {         // open arc floating inside, touching nothing
    const int c = sk.addPoint({10, 10});
    return sk.addArc(c, sk.addPoint({18, 10}), sk.addPoint({10, 18}), 8.0);
}
int addSplineAcross(Sketch& sk) {
    return sk.addSpline({sk.addPoint({-4, 3}), sk.addPoint({8, 14}),
                         sk.addPoint({24, 6})});
}
int addPolygonInside(Sketch& sk) {
    return sk.addPolygon(sk.addPoint({10, 10}), 5.0, 6);
}

// `changesWhenReal`: as REAL geometry this alters a region or wire. A line or
// open arc floating inside a shape, touching nothing, does not - which is why
// it is in the list at all: construction handling must not depend on it.
struct Case { const char* name; Adder add; bool changesWhenReal; };

const std::vector<Case>& cases() {
    static const std::vector<Case> c = {
        {"line across",     addLineAcross,     true},
        {"line inside",     addLineInside,     false},
        {"circle inside",   addCircleInside,   true},
        {"circle crossing", addCircleCrossing, true},
        {"arc inside",      addArcInside,      false},
        {"spline across",   addSplineAcross,   true},
        {"polygon inside",  addPolygonInside,  true},
    };
    return c;
}

} // namespace

// The headline contract, for every element kind: mark it construction and the
// sketch's output is exactly the plain square's.
TEST(SketchConstruction, ConstructionGeometryChangesNothingTheSketchProduces) {
    const Output base = outputOf(square());
    ASSERT_EQ(1u, base.regions);
    ASSERT_NEAR(400.0, base.regionArea, 1e-6);

    for (const auto& c : cases()) {
        Sketch sk = square();
        const int id = c.add(sk);
        ASSERT_TRUE(sk.setElementConstruction(id, true)) << c.name;
        ASSERT_TRUE(sk.hasConstructionGeometry()) << c.name;
        expectSame(outputOf(sk), base, c.name);
    }
}

// The same cases as REAL geometry must change the output, or the test above
// proves nothing (a builder that ignored every extra element would pass it).
TEST(SketchConstruction, TheSameGeometryAsRealDoesChangeTheOutput) {
    const Output base = outputOf(square());
    for (const auto& c : cases()) {
        Sketch sk = square();
        c.add(sk);
        const Output real = outputOf(sk);
        const bool changed =
            real.regions != base.regions || real.wires != base.wires ||
            std::abs(real.regionArea - base.regionArea) > 1e-6 ||
            std::abs(real.profileArea - base.profileArea) > 1e-6;
        EXPECT_EQ(c.changesWhenReal, changed) << c.name;
    }
}

// Several construction elements at once, mixed with the real shape.
TEST(SketchConstruction, ManyConstructionElementsTogetherStillChangeNothing) {
    const Output base = outputOf(square());
    Sketch sk = square();
    for (const auto& c : cases())
        sk.setElementConstruction(c.add(sk), true);
    expectSame(outputOf(sk), base, "all at once");
}

// A construction line that starts on a real edge must not split it: the
// profile builder turns any point lying on a line into a vertex, so a point
// only construction uses has to leave with it.
TEST(SketchConstruction, AConstructionPointOnARealEdgeDoesNotSplitIt) {
    Sketch real = square();
    real.addLine(real.addPoint({10, 0}), real.addPoint({10, -8}));   // on the bottom edge
    // Sanity: as a REAL line it does add a vertex to the outer wire.
    // (The line hangs outside the square, so the outer wire is unchanged in
    // shape; what splits is the bottom edge, which now has 5 edges.)
    EXPECT_GT(outputOf(real).outerEdges, 4);

    Sketch sk = square();
    const int id = sk.addLine(sk.addPoint({10, 0}), sk.addPoint({10, -8}));
    sk.setElementConstruction(id, true);
    EXPECT_EQ(4, outputOf(sk).outerEdges);
}

// A construction line started at a real corner keeps that corner: points the
// real geometry also uses stay.
TEST(SketchConstruction, SharedPointsSurviveTheConstructionFilter) {
    Sketch sk = square();
    const size_t before = sk.getPoints().size();
    const int corner = sk.getLines().front().startPointId;
    const int far = sk.addPoint({-10, -10});
    const int id = sk.addLine(corner, far);
    sk.setElementConstruction(id, true);

    const Sketch filtered = sk.withoutConstruction();
    EXPECT_EQ(before, filtered.getPoints().size())
        << "the construction-only endpoint goes, the shared corner stays";
    EXPECT_NE(nullptr, filtered.getPoint(corner));
    EXPECT_EQ(nullptr, filtered.getPoint(far));
    EXPECT_TRUE(filtered.getLines().size() == sk.getLines().size() - 1);
}

// The region cache is keyed by a geometry hash; flipping ONLY the flag must
// invalidate it for every element kind (arcs, splines and polygons were not in
// the hash at all before construction mattered).
TEST(SketchConstruction, FlippingTheFlagInvalidatesTheRegionCache) {
    for (const auto& c : cases()) {
        Sketch sk = square();
        const int id = c.add(sk);
        const size_t asReal = sk.buildRegions().size();       // primes the cache
        sk.setElementConstruction(id, true);
        EXPECT_EQ(1u, sk.buildRegions().size())
            << c.name << ": a stale cache would still answer " << asReal;
        sk.setElementConstruction(id, false);
        EXPECT_EQ(asReal, sk.buildRegions().size())
            << c.name << ": and flipping back must restore it";
    }
}

// A polygon is one thing: marking any of its edges marks the polygon and all of
// its edges, since it has no valid state with one edge missing.
TEST(SketchConstruction, AnEdgeOfAPolygonFlipsTheWholePolygon) {
    Sketch sk = square();
    const int pid = addPolygonInside(sk);
    const auto poly = sk.getPolygons().back();
    ASSERT_EQ(6u, poly.lineIds.size());

    ASSERT_TRUE(sk.setElementConstruction(poly.lineIds[2], true));
    EXPECT_TRUE(sk.isConstructionElement(pid));
    for (int lid : poly.lineIds) EXPECT_TRUE(sk.isConstructionElement(lid));

    sk.setElementConstruction(poly.lineIds[0], false);
    EXPECT_FALSE(sk.isConstructionElement(pid));
    for (int lid : poly.lineIds) EXPECT_FALSE(sk.isConstructionElement(lid));
}

// "Draw as construction" flags only what an action created, however many
// elements and points it made, and never what was there before.
TEST(SketchConstruction, SetConstructionSinceFlagsOnlyNewElements) {
    Sketch sk = square();
    const int firstNew = sk.nextElementId();
    const int a = addLineAcross(sk);
    const int b = addCircleInside(sk);
    sk.setConstructionSince(firstNew, true);

    EXPECT_TRUE(sk.isConstructionElement(a));
    EXPECT_TRUE(sk.isConstructionElement(b));
    for (const auto& l : sk.getLines())
        if (l.id < firstNew) EXPECT_FALSE(l.isConstruction) << "the square's edges";
    EXPECT_EQ(1u, sk.buildRegions().size());
}

// An id that names nothing is refused rather than silently ignored, and a
// construction flag on a missing element does not appear from nowhere.
TEST(SketchConstruction, UnknownIdsAreRefused) {
    Sketch sk = square();
    EXPECT_FALSE(sk.setElementConstruction(987654, true));
    EXPECT_FALSE(sk.isConstructionElement(987654));
    EXPECT_FALSE(sk.hasConstructionGeometry());
}

// A sketch with no construction geometry is not copied or filtered at all in
// the builders: withoutConstruction() of it is just a faithful copy.
TEST(SketchConstruction, WithoutConstructionOfAPlainSketchIsACopy) {
    Sketch sk = square();
    const Sketch copy = sk.withoutConstruction();
    EXPECT_EQ(sk.getPoints().size(), copy.getPoints().size());
    EXPECT_EQ(sk.getLines().size(), copy.getLines().size());
    expectSame(outputOf(copy), outputOf(sk), "plain sketch");
}

// The flag survives the project file for every element kind. Nothing could set
// it before construction became a feature, so this path had never run: a kind
// that wrote the flag but did not read it back would turn the reference lines
// into real geometry the next time the project opened.
TEST(SketchConstruction, EveryKindOfConstructionElementSurvivesSaveAndLoad) {
    Sketch sk = square();
    std::vector<int> marked;
    for (const auto& c : cases()) marked.push_back(c.add(sk));
    for (int id : marked) ASSERT_TRUE(sk.setElementConstruction(id, true));

    std::stringstream ss;
    materializr::ProjectIO::writeSketchBody(ss, sk);
    Sketch back;
    materializr::ProjectIO::parseSketchBody(ss, back);

    for (int id : marked)
        EXPECT_TRUE(back.isConstructionElement(id)) << "element " << id << " lost its flag";
    // The square's own edges were never construction and must not become so.
    const Sketch plain = square();   // named: a range-for over a temporary's member dangles
    for (const auto& l : plain.getLines())
        EXPECT_FALSE(back.isConstructionElement(l.id));
    expectSame(outputOf(back), outputOf(plain), "after a save and load");
}
