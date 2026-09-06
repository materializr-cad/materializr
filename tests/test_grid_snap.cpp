// Regression: with snap-to-grid on, a click must land ON the snap lattice, and
// the grid the viewport draws must BE that lattice. Steve, 2026-07-31, at a
// 0.1 mm grid: "I cannot draw a line on that snap grid" — the grid appeared to
// wander arbitrarily. Two independent causes, one test each:
//
//   1. SketchTool::snap() — directional inference guides (perpendicular- and
//      parallel-to-previous, axis-from-point, angle snap) returned a point on
//      their guide LINE, grid-aligned on the guide's dominant axis only. The
//      free coordinate came out wherever the geometry put it, so placed points
//      drifted off the lattice a few hundredths of a millimetre at a time.
//      A guide that represents CONTACT with existing geometry (landing on an
//      edge) is the deliberate exception and keeps its exact on-edge position:
//      buildWires splits that edge at the contact point, and a point rounded
//      off the edge silently stops closing the region.
//
//      CONTRACT REFINED 2026-09-03. "Both coordinates on the lattice" cannot
//      be required of a DIAGONAL guide: a 35 deg bisector on a 1 mm grid
//      passes through essentially no lattice crossings, so rounding both
//      coordinates moves the point off the ray — up to half a diagonal cell,
//      which near the anchor is degrees of angular error (measured: 8.4 deg
//      on a 3 mm leg off a 70 deg corner) while the guide still highlights
//      and claims the exact angle. The old code demanded both and was
//      self-contradictory: it preserved a borrowed direction from
//      rectifyNearAxis and then destroyed that same direction with onLattice
//      one line later. Steve's call, asked and answered: a guide you aimed
//      down is honoured EXACTLY, and the grid gets the freedom that's left.
//      So the requirement is now that every placed point sits on a grid LINE
//      — its dominant coordinate exactly on a step — rather than necessarily
//      on a grid CROSSING. The drift this test was written against was BOTH
//      coordinates wandering at once (x=9.0033 and y=12.2012 in the original
//      report); one pinned coordinate is what makes a placement legible and
//      repeatable, and that is what is asserted below.
//
//   2. Sketch::latticeAnchor — the anchor the drawn grid is laid out from.
//      Rounding world XYZ and projecting onto the sketch plane (what shipped)
//      is NOT a lattice point in-plane: the drawn grid sat 10–50% of a cell
//      off the lattice clicks land on, so no click could ever appear to sit on
//      a drawn line.

#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <gtest/gtest.h>
#include <utility>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <set>
#include <string>

// SketchTool's Text / SVG stamp paths are not part of materializr_core (they
// pull in font rendering); stub them so this links.
namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using materializr::InferenceGuide;
using materializr::Sketch;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {

// How far `v` sits from the nearest multiple of `step`.
double offLattice(double v, double step) {
    return std::fabs(v - std::round(v / step) * step);
}

} // namespace

// ─── 1. clicks land on the lattice ───────────────────────────────────────────
TEST(GridSnap, PlacedPointsLandOnTheLattice) {
    const float step = 0.1f;

    Sketch sk;
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setGridStep(step);
    tool.setSnapToGridEnabled(true);
    // Full is the shipping default and the tier that fires the most guides.
    tool.setInferenceLevel(SketchTool::InferenceLevel::Full);

    // Existing geometry, so the inference engine has something to grab: a
    // closed frame plus a circle, the way a real working sketch looks.
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({20.0f, 0.0f});
    int c = sk.addPoint({20.0f, 12.0f});
    int d = sk.addPoint({0.0f, 12.0f});
    sk.addLine(a, b); sk.addLine(b, c); sk.addLine(c, d); sk.addLine(d, a);
    int ctr = sk.addPoint({7.3f, 4.6f});
    sk.addCircle(ctr, 2.0);
    const std::set<int> preexisting{a, b, c, d, ctr};

    tool.setMode(SketchToolMode::Line);

    // A chain drawn at deliberately off-lattice cursor positions, routed near
    // the existing geometry so perpendicular / on-edge / axis guides fire.
    const glm::vec2 clicks[] = {
        {3.021f, 2.037f}, {9.114f, 2.052f}, {9.087f, 7.973f},
        {14.962f, 7.941f}, {14.933f, 11.968f}, {2.973f, 11.949f},
    };
    for (glm::vec2 p : clicks) {
        tool.onMouseMove(p);   // the guides are computed on hover
        tool.onMouseDown(p);
        tool.onMouseUp(p);
    }

    int placed = 0, onCrossing = 0;
    for (const auto& pt : sk.getPoints()) {
        if (preexisting.count(pt.id)) continue;
        ++placed;
        const double dx = offLattice(pt.pos.x, step);
        const double dy = offLattice(pt.pos.y, step);
        // At least one coordinate exactly on a step: the point sits on a drawn
        // grid line, so the placement is legible and repeatable even when an
        // honoured diagonal guide puts the other coordinate between lines.
        EXPECT_LE(std::min(dx, dy), 1e-4)
            << "point " << pt.id << " (" << pt.pos.x << ", " << pt.pos.y
            << ") is off the " << step << "mm grid on BOTH axes — that is the "
               "free-floating drift this test exists to catch";
        if (std::max(dx, dy) <= 1e-4) ++onCrossing;
    }
    EXPECT_GT(placed, 0) << "the chain committed no points — test drew nothing";
    // Honouring a diagonal guide is the exception, not the rule: a chain drawn
    // around axis-aligned geometry should still land mostly on crossings.
    EXPECT_GE(onCrossing * 2, placed)
        << "only " << onCrossing << " of " << placed << " points landed on a "
           "lattice crossing — the grid has stopped being the default";
}

// A point landing ON an existing edge stays on that edge. The lattice may
// choose WHERE along the edge, but must not lift the point off it, or the
// region walker can no longer route a loop through the split.
TEST(GridSnap, ContactWithAnEdgeStaysOnTheEdge) {
    const float step = 0.1f;

    Sketch sk;
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setGridStep(step);
    tool.setSnapToGridEnabled(true);
    tool.setInferenceLevel(SketchTool::InferenceLevel::Full);

    // A single diagonal edge — a lattice point almost never sits exactly on
    // one, so this is the case where the two demands genuinely conflict.
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 7.0f});
    sk.addLine(a, b);

    tool.setMode(SketchToolMode::Line);
    // Aim just off the middle of the diagonal.
    const glm::vec2 target(5.02f, 3.53f);
    tool.onMouseMove(target);
    tool.onMouseDown(target);
    tool.onMouseUp(target);

    bool sawContact = false;
    for (const auto& pt : sk.getPoints()) {
        if (pt.id == a || pt.id == b) continue;
        // Distance from the point to the infinite line through a→b.
        glm::vec2 ab(10.0f, 7.0f);
        float len = glm::length(ab);
        float cross = std::fabs(ab.x * pt.pos.y - ab.y * pt.pos.x) / len;
        if (cross < 1e-3f) sawContact = true;
    }
    EXPECT_TRUE(sawContact)
        << "the point placed on the diagonal is no longer on it";
}

// ─── 2. the drawn grid is the same lattice ───────────────────────────────────
// The grid the user SEES is laid from the anchor, every effective step. The
// cursor snaps to multiples of the effective step from the PLANE ORIGIN. Those
// coincide only while the anchor is itself a multiple of the EFFECTIVE step —
// so once zoom scales the step, re-anchoring on the base is not enough.
//
// This is the case the test above structurally cannot reach: it uses one step
// for both roles, so it stays green whichever step the anchor was built from.
TEST(GridSnap, AnchorFollowsTheZoomScaledStepNotTheBase) {
    const gp_Ax3 ax(gp_Pnt(12.37, 5.02, 3.5), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0));
    const gp_Pln pln(ax);
    const gp_Pnt lookAt(20.0, 9.0, 3.5);

    auto uvOf = [&](const gp_Pnt& p) {
        const gp_Vec rel(ax.Location(), p);
        return std::pair<double, double>{rel.Dot(gp_Vec(ax.XDirection())),
                                         rel.Dot(gp_Vec(ax.YDirection()))};
    };

    const double base = 1.0;
    for (double effective : {10.0, 100.0}) {   // the zoomed-OUT direction
        // What the code used to do: anchor on the base, draw every effective.
        const auto onBase = uvOf(Sketch::latticeAnchor(pln, lookAt, base));
        EXPECT_GT(std::max(offLattice(onBase.first,  effective),
                           offLattice(onBase.second, effective)), 1e-6)
            << "a base-built anchor is expected to sit OFF the effective "
               "lattice at effective=" << effective
            << " — if this ever passes, the case being guarded is gone";

        // What it does now: anchor on the effective step, so every drawn line
        // lands exactly where the cursor can.
        const auto onEff = uvOf(Sketch::latticeAnchor(pln, lookAt, effective));
        EXPECT_LE(offLattice(onEff.first,  effective), 1e-6)
            << "effective=" << effective << " u=" << onEff.first;
        EXPECT_LE(offLattice(onEff.second, effective), 1e-6)
            << "effective=" << effective << " v=" << onEff.second;
    }

    // The zoomed-IN direction was always safe — a finer effective step divides
    // the base — but it is asserted so the claim is checked, not assumed.
    const auto fine = uvOf(Sketch::latticeAnchor(pln, lookAt, base));
    EXPECT_LE(offLattice(fine.first,  0.1), 1e-6);
    EXPECT_LE(offLattice(fine.second, 0.1), 1e-6);
}

TEST(GridSnap, LatticeAnchorSitsOnTheSnapLattice) {
    struct Case { const char* name; gp_Ax3 ax; gp_Pnt lookAt; };
    // A sketch started on a face gets whatever plane origin the geometry has —
    // fractional world coordinates are the norm, not the exception.
    const Case cases[] = {
        {"XY plane at a fractional origin",
         gp_Ax3(gp_Pnt(12.37, 5.02, 3.5), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)),
         gp_Pnt(20.0, 9.0, 3.5)},
        {"XZ face of a 2mm wall",
         gp_Ax3(gp_Pnt(1.25, 0.0, 7.68), gp_Dir(0, 1, 0), gp_Dir(1, 0, 0)),
         gp_Pnt(30.0, 0.0, 40.0)},
        {"tilted plane",
         gp_Ax3(gp_Pnt(4.44, 2.22, 1.11), gp_Dir(0, 0.6, 0.8), gp_Dir(1, 0, 0)),
         gp_Pnt(10.0, 10.0, 10.0)},
    };

    for (double step : {1.0, 0.1}) {
        for (const Case& cs : cases) {
            const gp_Pln pln(cs.ax);
            const gp_Pnt anchor = Sketch::latticeAnchor(pln, cs.lookAt, step);

            // Express the anchor in the frame snapping rounds in: sketch (u,v)
            // from the plane origin along X/YDirection (see sketchToWorld).
            const gp_Pnt o = cs.ax.Location();
            const gp_Vec rel(o, anchor);
            const double u = rel.Dot(gp_Vec(cs.ax.XDirection()));
            const double v = rel.Dot(gp_Vec(cs.ax.YDirection()));

            EXPECT_LE(offLattice(u, step), 1e-6)
                << cs.name << " @ step " << step << ": u=" << u;
            EXPECT_LE(offLattice(v, step), 1e-6)
                << cs.name << " @ step " << step << ": v=" << v;

            // Still ON the plane, and still where it was asked to be: measured
            // against the target's own projection, it may only move by half a
            // cell along each axis (the anchor doubles as the camera target,
            // so it has to stay put).
            EXPECT_LE(std::fabs(pln.Distance(anchor)), 1e-6) << cs.name;
            const gp_Pnt projected = cs.lookAt.Translated(
                gp_Vec(pln.Axis().Direction()) *
                -gp_Vec(o, cs.lookAt).Dot(gp_Vec(pln.Axis().Direction())));
            EXPECT_LE(anchor.Distance(projected), step * 0.71 + 1e-6)
                << cs.name << " @ step " << step
                << ": anchor wandered further than half a cell";
        }
    }
}

// A pointing tolerance is a SCREEN distance. Deriving it from the grid failed
// in both directions: uncapped it was 152 mm under a foot grid, so a click on
// empty space cut geometry 15 cm away; capped at 10 mm it was 5 mm in a view
// where one pixel is 8 mm, which is sub-pixel — nothing could be picked at all.
TEST(GridSnap, PointingToleranceTracksTheScreenNotTheModel) {
    SketchTool t;
    const float cap = SketchTool::kToleranceStepCapMm;
    const float px  = SketchTool::kPointingRadiusPx;

    // Millimetre work, zoomed so a pixel is a fraction of a mm: the grid floor
    // dominates and every existing sketch behaves exactly as it always has.
    t.setPixelScale(0.027f);            // ~40 mm across a 1500 px viewport
    for (float mm : { 0.1f, 0.5f, 1.0f, 10.0f }) {
        t.setGridStep(mm);
        EXPECT_FLOAT_EQ(std::max(mm, px * 0.027f), t.tolStep()) << "mm grid: " << mm;
    }

    // Feet: one pixel is ~8 mm, so the SCREEN term takes over and the target
    // stays a constant handful of pixels instead of collapsing under a pixel.
    t.setGridStep(304.8f);
    t.setPixelScale(8.128f);            // ~40 ft across a 1500 px viewport
    EXPECT_FLOAT_EQ(px * 8.128f, t.tolStep());
    EXPECT_GT(t.tolStep(), cap) << "the 10 mm cap would have been sub-pixel here";
    // The same gesture, in pixels, whatever the unit.
    EXPECT_NEAR(px, t.tolStep() / 8.128f, 1e-3);

    // The LATTICE is untouched by any of this — a foot grid is still a foot.
    EXPECT_FLOAT_EQ(304.8f, t.getGridStep());
}

// Pointing tolerances must not scale without bound when the grid does.
// The grid step follows the display unit, so a 1 ft grid is 304.8 mm — and
// trim, pick, inference and hover distances all derived from it directly.
// That put the trim threshold at max(0.3, 304.8*0.5) = 152 mm: a click on
// empty space could cut geometry 15 cm away, with grid snapping OFF.
TEST(GridSnap, ToleranceStepIsCappedWhileTheLatticeIsNot) {
    SketchTool t;
    const float cap = SketchTool::kToleranceStepCapMm;
    t.setPixelScale(0.0f);   // no frame yet: the grid term alone

    // Every grid the presets ever offered in millimetres behaves EXACTLY as
    // before — the cap is the largest of them, so nothing existing moves.
    for (float mm : { 0.1f, 0.5f, 1.0f, 10.0f }) {
        t.setGridStep(mm);
        EXPECT_FLOAT_EQ(mm, t.tolStep()) << "mm grid unchanged: " << mm;
    }

    // A foot of lattice, but pointing stays human-scaled.
    t.setGridStep(304.8f);
    EXPECT_FLOAT_EQ(cap, t.tolStep());
    EXPECT_NEAR(5.0f, std::max(0.3f, t.tolStep() * 0.5f), 1e-4)
        << "the trim threshold must stay millimetres, not become 152 mm";

    // The LATTICE itself is not capped — a foot grid still snaps to a foot.
    // The LATTICE itself is not capped: tolStep() is a ceiling on POINTING
    // distance only, and the snap arithmetic keeps using m_gridStep. Asserted
    // on the accessor because snap() is private; the lattice sites were left
    // reading m_gridStep deliberately.
    EXPECT_FLOAT_EQ(304.8f, t.getGridStep())
        << "the grid must still be a foot even though tolerances are not";
}
