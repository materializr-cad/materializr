#pragma once
// Which body does a sketch Subtract actually cut, and which way does it sweep?
//
// Subtract-from-sketch used to demand that the sketch still carry a link to the
// body it was drawn on (Sketch::getSourceBody). Anything else - a sketch on a
// construction plane, on an origin plane, one that had been unlinked - printed a
// line to stderr and returned, so the toolbar button was a silent no-op. And even
// with a live link the cut could quietly do nothing: a tool volume that misses
// the body leaves BRepAlgoAPI_Cut handing back the body unchanged, which passes
// every validity check and lands on History as a step that changed nothing.
//
// So the target is chosen from GEOMETRY instead of provenance: sweep the profile,
// then cut whichever visible body the swept volume actually removes material
// from. The sketch's own body is preferred when it is one of them, so the common
// case is unchanged; the fallbacks are what used to be dead ends.
//
// Lives here, not in Application, so it can be tested: ctest cannot see src/app.
// Header-only on purpose - a new .cpp would have to be added to TWO source lists
// (root CMakeLists.txt and tests/CMakeLists.txt) and one always gets forgotten.

#include <BRepAlgoAPI_Common.hxx>
#include "BoolArgs.h"
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace materializr {
namespace cutpick {

// A cut this small isn't a cut. Sits well above OCCT's boolean noise (two solids
// merely touching along a face can report a sliver of common volume) and well
// below anything a user would draw on purpose.
inline constexpr double kMinRemovedVolume = 1e-6;

// Volume the tool would remove from this body, or 0 if they don't overlap. The
// bounding-box reject first: the common of two far-apart solids is cheap enough
// per pair but not over a hundred-body project.
inline double removedVolume(const TopoDS_Shape& body, const TopoDS_Shape& tool) {
    if (body.IsNull() || tool.IsNull()) return 0.0;
    try {
        Bnd_Box bb, tb;
        BRepBndLib::Add(body, bb);
        BRepBndLib::Add(tool, tb);
        if (bb.IsVoid() || tb.IsVoid() || bb.IsOut(tb)) return 0.0;

        BRepAlgoAPI_Common common;
        materializr::setBooleanShapes(common, body, tool);
        common.Build();
        if (!common.IsDone()) return 0.0;
        const TopoDS_Shape& s = common.Shape();
        if (s.IsNull()) return 0.0;
        GProp_GProps g;
        BRepGProp::VolumeProperties(s, g);
        const double v = g.Mass();
        return (v > 0.0) ? v : 0.0;
    } catch (...) {
        return 0.0;
    }
}

// The body a swept tool volume should be cut from, or -1 when it reaches none.
//
// `preferred` (the sketch's own host body, or -1) wins whenever the tool removes
// anything at all from it - a sketch drawn on a face keeps cutting that face's
// body even where it also clips a neighbour. Otherwise the biggest removal wins,
// which is the only defensible reading of "cut the body it runs into".
inline int pickCutTarget(const std::vector<std::pair<int, TopoDS_Shape>>& bodies,
                         const TopoDS_Shape& tool, int preferred = -1) {
    if (tool.IsNull()) return -1;
    int best = -1;
    double bestVol = kMinRemovedVolume;
    for (const auto& [id, shape] : bodies) {
        const double v = removedVolume(shape, tool);
        if (v <= kMinRemovedVolume) continue;
        if (id == preferred) return id;      // the host body always wins
        if (v > bestVol) { bestVol = v; best = id; }
    }
    return best;
}

// EVERY body the tool volume removes material from, in id order.
//
// The one-target rule is right for the common case - a sketch drawn on a face
// belongs to that face's body - but wrong whenever a profile is meant to pass
// through a stack: only the host got cut, and the rest of the sweep vanished
// into bodies it visibly passed through. This is the opt-in answer to that.
// Deterministic order matters: the caller pushes one operation per body, and
// History replays them in the order they were recorded.
inline std::vector<int> pickAllCutTargets(
        const std::vector<std::pair<int, TopoDS_Shape>>& bodies,
        const TopoDS_Shape& tool) {
    std::vector<int> hits;
    if (tool.IsNull()) return hits;
    for (const auto& [id, shape] : bodies)
        if (removedVolume(shape, tool) > kMinRemovedVolume)
            hits.push_back(id);
    std::sort(hits.begin(), hits.end());
    return hits;
}

// Which way should a cut sweep from a sketch with no host body? +1 runs along the
// plane normal, -1 against it.
//
// A face sketch has an answer for free - the face normal points OUT of its body,
// so a cut goes the other way. A sketch on a construction or origin plane has no
// such convention: its normal points wherever the plane happens to face, and half
// the time that is away from every body, so the arrow started out aimed at empty
// space and the drag had to be given a negative distance to bite. Aim at the
// nearest body's centre instead. Nothing to aim at: +1, an arbitrary choice the
// user can still reverse by dragging the other way.
inline double cutSweepSign(const gp_Pnt& origin, const gp_Dir& normal,
                           const std::vector<TopoDS_Shape>& bodies) {
    double bestDist = 0.0;
    double sign = 1.0;
    bool found = false;
    for (const TopoDS_Shape& s : bodies) {
        if (s.IsNull()) continue;
        try {
            Bnd_Box bb;
            BRepBndLib::Add(s, bb);
            if (bb.IsVoid()) continue;
            double x0, y0, z0, x1, y1, z1;
            bb.Get(x0, y0, z0, x1, y1, z1);
            const gp_Pnt c(0.5 * (x0 + x1), 0.5 * (y0 + y1), 0.5 * (z0 + z1));
            const gp_Vec d(origin, c);
            const double along = d.Dot(gp_Vec(normal));
            // Rank by true distance, not by distance along the normal: a body
            // straddling the plane sits at along ~= 0 and is the obvious target
            // even though a body far off to the side may have a larger |along|.
            const double dist = d.Magnitude();
            if (found && dist >= bestDist) continue;
            bestDist = dist;
            found = true;
            // A body centred exactly on the plane gives no direction; keep +1.
            if (std::abs(along) > 1e-9) sign = (along > 0.0) ? 1.0 : -1.0;
            else                        sign = 1.0;
        } catch (...) {}
    }
    return sign;
}

} // namespace cutpick
} // namespace materializr
