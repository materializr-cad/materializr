#pragma once

#include <TopoDS_Shape.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepTools_History.hxx>
#include <GProp_GProps.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace materializr {

// Angular tolerance for every ShapeUpgrade_UnifySameDomain in the codebase.
//
// OCCT's default is Precision::Angular() = 1e-12, which is TIGHTER THAN ITS OWN
// BOOLEAN OUTPUT. The coplanar faces a cut or fuse leaves behind have normals
// agreeing to ~1e-9, not 1e-12, so unify silently declines to merge them and
// the split shows to the user as a seam line across an otherwise flat face -
// issue #81, hit on an imported-and-edited STEP part.
//
// Measured on that part (left nacelle.mzr, 189 faces, 10 coplanar-adjacent
// face pairs) with a probe that counts adjacent planar faces sharing a plane:
//
//     default            -> faces 189, coplanar pairs 10   (unify does nothing)
//     linear 1e-4 only   -> faces 189, coplanar pairs 10   (linear is NOT it)
//     angular 1e-9 only  -> faces 183, coplanar pairs  0
//     angular 1e-3 only  -> faces 160, coplanar pairs  0
//
// 1e-9 is deliberately the tightest value that still clears every seam: it
// merges only the six faces that were genuinely one, where 1e-3 merges 29. Do
// not loosen it to "fix" some other merge without measuring what else it
// starts merging - this tolerance decides which faces cease to exist, and
// downstream fillet/chamfer references are resolved against those faces.
constexpr double kUnifyAngularTol = 1.0e-9;

// ─── Adopting a unify result ────────────────────────────────────────────────
//
// UnifySameDomain is COSMETIC: it drops redundant boundaries between faces that
// were always one face. It must never change the shape of the part, and it must
// never decide whether an operation succeeds. Both of those happened.
//
// Measured on Steve's "robot ass" project (2026-08-20). Union of a 7.36 mm3
// wedge onto a 219.69 mm3 body:
//
//     BRepAlgoAPI_Fuse   -> vol 227.049 = 219.691 + 7.357, 1 solid, valid
//     after unify        -> vol 161.075, INVALID, and still 9 faces
//
// It merged NOTHING (9 faces in, 9 out) and ate 29% of the volume on the way
// through. Every flag combination did it - concatBSplines off, faces-only,
// edges-only, OCCT's default angular tolerance - so it is not a tuning
// question. BooleanOp then validity-checked the mangled shape, got false, and
// reported "Fuse failed even with fuzzy" four times over a fuse that had
// succeeded perfectly each time. From the user's chair the Union button was
// simply dead.
//
// So: check the result is still the same solid before adopting it - a visible
// seam is a blemish, a silently reshaped part is data loss, and a refused
// operation is neither. Callers go through unifySameDomain() below rather than
// calling this directly, because the shape to fall back to has to be a copy
// taken beforehand; see the note there.
//
// MergeFacesOp has guarded itself this way since it shipped (its own note
// records unify moving geometry by 5.5e-6 relative on the nacelle). This is
// that same guard, made shared, because every other caller adopted the result
// unconditionally.
inline bool unifyIsSafe(const TopoDS_Shape& before, const TopoDS_Shape& after,
                        const char* who) {
    if (after.IsNull()) return false;
    try {
        // Volume first: it is the cheaper of the two checks and it is what
        // catches a reshape that still passes the validity check.
        GProp_GProps g0, g1;
        BRepGProp::VolumeProperties(before, g0);
        BRepGProp::VolumeProperties(after,  g1);
        const double v0 = g0.Mass(), v1 = g1.Mass();
        // Same relative slack MergeFacesOp uses, so the two guards agree on
        // what counts as "unchanged".
        const double tol = 1e-4 * std::max(1.0, std::fabs(v0));
        if (std::fabs(v0 - v1) > tol) {
            std::fprintf(stderr, "[Unify] %s: volume %.6f -> %.6f; keeping the "
                                 "un-merged shape.\n", who ? who : "?", v0, v1);
            return false;
        }
        if (!BRepCheck_Analyzer(after).IsValid()) {
            std::fprintf(stderr, "[Unify] %s: merge produced an invalid shape; "
                                 "keeping the un-merged one.\n", who ? who : "?");
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

// Merge same-domain faces, and hand back something that is still the part.
//
// ShapeUpgrade_UnifySameDomain EDITS ITS INPUT IN PLACE. Measured on the same
// union: the fuse result reads 227.049 and valid, unify runs, and the original
// handle now reads 161.075 and invalid alongside the returned one. Checking the
// result and "keeping the pre-unify shape" therefore fixes nothing on its own -
// by then there is no pre-unify shape left to keep. Only a deep copy taken
// BEFORE the merge survives it. (The merge's inputs, if it is a boolean result,
// are not touched - verified separately.)
//
// So: copy first, merge, and return the copy when the merge is not adoptable.
// The merge still runs on the caller's shape, so a merge we DO adopt carries a
// BRepTools_History whose keys are the caller's own sub-shapes and face lineage
// is unaffected - that is the common path and it behaves exactly as before.
// On the rejected path the caller gets the spare copy, whose sub-shapes are new:
// a ledger keyed on the corrupted original will not resolve against it, so that
// one operation can lose its face lineage. A downstream reference re-resolves
// geometrically; an operation that refuses outright does not. That is the whole
// trade.
inline TopoDS_Shape unifySameDomain(const TopoDS_Shape& shape, const char* who,
                                    bool concatBSplines = true,
                                    Handle(BRepTools_History)* historyOut = nullptr) {
    if (shape.IsNull()) return shape;
    TopoDS_Shape spare;
    try {
        spare = BRepBuilderAPI_Copy(shape).Shape();
    } catch (...) {
        spare = TopoDS_Shape();
    }
    // No spare means no way back, so do not risk the merge at all.
    if (spare.IsNull()) return shape;
    try {
        ShapeUpgrade_UnifySameDomain u(shape, /*edges=*/true, /*faces=*/true,
                                       concatBSplines);
        u.SetAngularTolerance(kUnifyAngularTol);
        u.Build();
        const TopoDS_Shape merged = u.Shape();
        // Compare against the spare: `shape` itself may already be mutated.
        if (unifyIsSafe(spare, merged, who)) {
            if (historyOut) *historyOut = u.History();
            return merged;
        }
    } catch (...) {}
    return spare;
}

} // namespace materializr