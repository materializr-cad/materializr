#pragma once
// Which way does a face sketch's grid run?
//
// The plane recovered from a face's surface carries the surface's intrinsic
// parametric X, which is arbitrary: a boolean- or loft-generated plane can be
// rotated any which way, and on a scaled-down box top it sits ~45 degrees off
// the visible edges. So the X axis is chosen from the face's own geometry.
//
// The rule was "the longest straight edge", which is right for a lofted cap and
// WRONG for a symmetric taper: the two longest edges of a trapezoid are its
// diagonals, so the grid rotated to a diagonal on a part the user had built
// symmetric about a world axis.
//
// So: prefer a straight edge that agrees with the world frame, and fall back to
// the longest edge when the face has no such edge (a part deliberately rotated
// off-axis still follows its own geometry). A face with no straight edge at all
// - a circular cap - has nothing to follow and takes a projected world axis.
//
// Lives here, not in Application, so it can be tested: ctest cannot see
// src/app. Header-only on purpose - a new .cpp would have to be added to TWO
// source lists (root CMakeLists.txt and tests/CMakeLists.txt) and one of them
// always gets forgotten.

#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <vector>

namespace materializr {

// Edges this close in angle to the BEST-aligned edge count as equally aligned,
// and length breaks the tie between them. This is a band around the winner, not
// an absolute tolerance: an absolute one is what got this wrong the first time.
// On the reporting part the diagonal ran 4.51 degrees off world Z, inside a 5
// degree tolerance, so it qualified as "aligned" and then won on length - the
// exact edge the rule existed to reject.
inline constexpr double kSketchAxisBandDeg = 0.5;

// A world-aligned edge must still be a real feature of the face. Without this,
// a part deliberately rotated off-axis would hand its grid to whatever tiny
// notch happened to sit square to the world.
//
// Kept low on purpose: on a TALL taper the aligned ends are short against the
// diagonals (the reporting part's were 81.6 against 130.4, and a narrower one
// would be worse), so a high bar would quietly hand the grid back to the
// diagonal - the bug this exists to prevent.
inline constexpr double kSketchAxisMinLenFrac = 0.15;

// The X direction for a sketch plane on `face` whose normal is `n`.
// `surfaceX` is the plane's own parametric X, returned when the face offers
// nothing better.
inline gp_Dir sketchPlaneXDirection(const TopoDS_Face& face, const gp_Dir& n,
                                    const gp_Dir& surfaceX) {
    const gp_Vec nv(n);

    // The world axes, projected into the plane. An axis nearly parallel to the
    // normal projects to nothing and is skipped.
    gp_Vec worldInPlane[3];
    bool haveWorld[3] = {false, false, false};
    const gp_Vec axes[3] = {gp_Vec(1, 0, 0), gp_Vec(0, 1, 0), gp_Vec(0, 0, 1)};
    for (int i = 0; i < 3; ++i) {
        const gp_Vec proj = axes[i] - nv * (axes[i] * nv);
        if (proj.Magnitude() > 1e-6) {
            worldInPlane[i] = proj.Normalized();
            haveWorld[i] = true;
        }
    }

    // Collect every straight edge with its in-plane direction, its length, and
    // how far it sits from the nearest world axis. Circular edges are measured
    // too (arc length only) -- they get no direction vote, but they decide
    // whether the face is ROUND. A flange cap is bounded by circles with one
    // short keyway flat; following that flat rotated the whole grid 17 degrees
    // off the world on a face that is round by any sane reading (Steve's
    // flange.mzr, top of the tube). Edge-following exists for faces whose
    // shape IS the straight edges (a slanted lofted cap); when the boundary is
    // mostly arc, the arcs say "no preferred straight direction here" and the
    // world axes win.
    struct Cand { gp_Dir dir; double len; double angle; };
    std::vector<Cand> cands;
    double longestOverall = 0.0;
    double lineLenTotal = 0.0, arcLenTotal = 0.0;

    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.GetType() == GeomAbs_Circle) {
            arcLenTotal += std::abs(c.LastParameter() - c.FirstParameter()) *
                           c.Circle().Radius();
            continue;
        }
        if (c.GetType() != GeomAbs_Line) continue;
        gp_Pnt p0, p1;
        c.D0(c.FirstParameter(), p0);
        c.D0(c.LastParameter(), p1);
        const gp_Vec ev(p0, p1);
        // Project into the plane; an edge along the normal has no direction here.
        const gp_Vec proj = ev - nv * (ev * nv);
        const double len = proj.Magnitude();
        if (len < 1e-6) continue;
        const gp_Vec dir = proj.Normalized();

        // Direction, not sense - an edge and its reverse are the same axis.
        double best = 180.0;
        for (int i = 0; i < 3; ++i) {
            if (!haveWorld[i]) continue;
            const double d = std::abs(dir * worldInPlane[i]);
            const double a = std::acos(std::min(1.0, d)) * 180.0 / M_PI;
            best = std::min(best, a);
        }
        cands.push_back({gp_Dir(dir), len, best});
        longestOverall = std::max(longestOverall, len);
        lineLenTotal += len;
    }

    // Round face: the boundary is dominated by circular arcs, so no straight
    // edge speaks for the face's orientation. Straight slivers (keyway flats,
    // slot sides) must not steer the grid -- unless one of them already agrees
    // with a world axis, in which case following it changes nothing visible
    // and keeps the edge exactly on the lattice.
    const bool roundFace = arcLenTotal > lineLenTotal * 1.5;

    if (!cands.empty()) {
        double minAngle = 180.0;
        for (const auto& c : cands) minAngle = std::min(minAngle, c.angle);
        if (roundFace && minAngle > 1.0) cands.clear();
    }
    if (!cands.empty()) {
        double minAngle = 180.0;
        for (const auto& c : cands) minAngle = std::min(minAngle, c.angle);

        // The best-aligned edges, longest first. Ranking by angle BEFORE length
        // is the whole correction: the winner is the edge that agrees with the
        // world, and length only separates edges that agree equally well.
        const Cand* pick = nullptr;
        for (const auto& c : cands) {
            if (c.angle > minAngle + kSketchAxisBandDeg) continue;
            if (!pick || c.len > pick->len) pick = &c;
        }
        if (pick && pick->len >= longestOverall * kSketchAxisMinLenFrac)
            return pick->dir;

        // The best-aligned edge is an insignificant sliver: follow the face's
        // own dominant direction instead.
        const Cand* longest = nullptr;
        for (const auto& c : cands)
            if (!longest || c.len > longest->len) longest = &c;
        if (longest) return longest->dir;
    }

    // No straight edge at all (a circular cap, a threaded rod's top). The
    // surface's X is arbitrary and the grid sat visibly askew from the ground
    // grid, so take a world axis instead.
    for (int i = 0; i < 3; ++i)
        if (haveWorld[i]) return gp_Dir(worldInPlane[i]);
    return surfaceX;
}

} // namespace materializr
