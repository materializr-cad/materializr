# Design: Move/Scale/Rotate Face - loft within the adjacent feature

**Status:** design draft (not built). Captures the plan for the multi-feature
face-transform rework + the interim fail-safe.

## Problem

The unified face transform (Move / Scale / Rotate Face, `MoveFaceOp`) lofts the
selected face to an *opposite cap* and rebuilds the **entire** solid between
them via `BRepOffsetAPI_ThruSections`. On a body with intermediate features it
erases them:

- A funnel → step → spout. Scale the **spout's bottom face** (faces down) and it
  lofts all the way up to the **funnel mouth** (the only up-facing cap),
  destroying the step and the spout into a single cone.
- The step/shoulder faces the *same* direction as the scaled face, so it isn't
  even an "opposite cap" candidate - "nearest opposite" doesn't help.

This silently destroys geometry, which violates the project's stated principle
("operations validate and *refuse* rather than silently produce garbage").

## Target behaviour

Transform a face and loft **only within the adjacent feature**, leaving the rest
of the body intact. Scaling the spout bottom should taper the spout from the
(scaled) bottom up to the spout's top cross-section, where it meets the step -
the funnel above is untouched.

## Approach

Instead of hunting for an existing opposite cap, find where the **adjacent
feature ends** and synthesize a loft target there.

1. **Feature-extent finder.** From the selected face, take its boundary edges,
   find the side-wall face(s) sharing them, and walk along them (−N) until the
   cross-section stops being constant - i.e. the wall's far edge loop, where it
   meets a step / a different face. That far loop is the feature boundary.
   Synthesize a planar (or ruled) cap face from it. On a plain prism this loop
   *is* the opposite cap, so the simple case is unchanged.

2. **Snapshot-anchored capture.** Derive the anchor(s) **once at op `begin`, from
   the captured snapshot - not the live body, not per-frame.** Hold them for the
   whole interactive lifetime: every drag frame, every face in a multi-select,
   every move in the sequence. Discard on commit/cancel. Because the controller
   already snapshots the body at begin, "hold the invisible face for a few moves"
   = anchor to the snapshot. This is stable under live drag (anchor constant,
   only the moved face slides) and correct for multi-face (one anchor per
   selected face, all frozen against the same snapshot). Chained ops (commit →
   re-select → move again) re-derive fresh - correct, since the body changed.

3. **Loft + partial replacement (the hard part).** Loft selected-face →
   synthesized cap, then **cut the old feature out and sew/union the lofted
   replacement back** into the untouched remainder - rather than rebuilding the
   whole solid base→top. This is BRep surgery (remove faces, sew, fix solid) and
   is where this becomes a real engine rework.

Build in that order; phase 3 sits on 1–2 once they're proven.

## Interim fail-safe (ship-blocker fix)

Phase 1 alone yields a safe stopgap: if the adjacent feature's extent does **not**
reach the chosen base (i.e. there's a step / another feature in between), the
transform would erase geometry - so **refuse with guidance** instead of
silently destroying it:

> "Can't transform this face - it would erase the steps below it. Build the
> change with Extrude + a union, or transform a face that spans the whole end."

This restores the refuse-don't-corrupt guarantee on multi-feature bodies while
keeping the (correct) behaviour on simple bodies, and is far cheaper than the
full rework. **Recommended before any public release.**

## Notes
- `MoveFaceOp.cpp` base-selection is currently "nearest opposite face" (an
  untested change); the rework supersedes that path entirely.
- Multi-face transforms today: `TaperController` collects multiple faces; the
  unified Move/Scale/Rotate path must carry the anchor-per-face design above.
