# Sketch Offset Tool - Design

**Status:** implemented 2026-08-31. Deviations from the original design are marked **[revised]** below.
**Plan:** `docs/superpowers/plans/2026-08-31-sketch-offset-tool.md`.

## Goal

An Onshape-style Offset tool in sketch mode: hover a sketch entity, the whole
connected chain highlights, click to accept it, then move the cursor (or type a
number) to lay a parallel copy of that chain at a fixed distance. Commit
produces ordinary sketch lines/arcs/circles.

## Non-goals (deliberate, v1)

- **No associativity.** The result is a one-shot copy, exactly like the
  interactive Mirror (`SketchTool::commitMirror`). A live offset that
  re-derives when the source moves needs derived-geometry machinery in
  `SketchSolver` that does not exist; it is a separate, much larger job.
- **No new operation type, no serialization, no topo naming.** The output is
  plain sketch elements, so save/load, full replay and `test_full_replay` are
  untouched.
- ~~**No splines.**~~ **[revised - splines ARE supported, added 2026-08-31]**
  See "Splines" below. The original reasoning still holds - there is no exact
  offset of a B-spline - so the implementation is explicitly an approximation
  with a stated error budget, rather than a refusal.
- **No construction-geometry toggle.** **[revised]** The design called for a
  "make it construction geometry" checkbox on the panel. Dropped: `isConstruction`
  is honoured by `buildWires()` and round-trips through save/load, but nothing in
  the app can set or clear it - there is no construction UI anywhere. Shipping the
  checkbox only here would create geometry the user can never un-mark. The first
  construction-geometry UI is its own feature.
- **No text glyphs.** `fromText` elements are excluded. "Embolden this word"
  is a legitimate want, but a five-letter word is hundreds of edges and the
  corner/prune cost is quadratic-ish in a chain that long.
- **No 3D face offset.** Sketch-plane only.

## Why not OCCT

`BRepOffsetAPI_MakeOffset` would handle corners and self-intersection for us,
and `Sketch::buildWires()` / `buildOpenWire()` already produce the input wire.
Rejected for v1 because:

- There is no wire → sketch-element converter, and offset B-splines do not map
  onto `Sketch::addSpline` (see above), so curved chains come back approximated.
- `MakeOffset` on **open** wires is historically unreliable; open chains are
  half the use cases.
- It is far too heavy to run per-frame for the live preview, so we would need
  a second, native implementation for the ghost anyway - and then the ghost and
  the commit could disagree, which is exactly the class of bug
  `planTrim`/`applyTrim` was structured to avoid.

Native 2D offset instead. `SketchTool.cpp` already carries
`intersectLineLine`, `intersectLineCircle`, `intersectLineArc` and
`intersectCircleCircle` (lines 2728–2790) - the corner fix-up kit - but they
are `static` in that TU, so `SketchOffset.cpp` gets its own small, tested
copies rather than a risky extraction (there is no `test_sketch_trim`, so
refactoring Trim's helpers is unprotected).

## Chain selection

Sketch elements share point **ids**, so adjacency is exact: two elements are
adjacent iff they share an endpoint id.

- Walk both directions from the picked element while the shared endpoint has
  degree exactly 2 among offsettable elements.
- Stop at degree != 2 (branch or free end), or when the walk returns to the
  start element - that is a **closed** chain.
- A circle is a closed chain of one element; no walk.
- **[revised]** Polygons need no special case at all: `Sketch::addPolygon`
  emits its edges as real `SketchLine` elements, so the ordinary line walk
  picks one up as an ordinary closed loop.
- Splines, `fromText` elements and construction/non-construction mixing all
  terminate the walk (mixing is allowed to terminate rather than error - the
  user gets the sub-chain they pointed at).

The walk produces an **ordered, consistently oriented** list of source
segments: each entry is a line (p0→p1) or an arc (centre, r, start angle,
signed sweep), with the tangent direction of travel.

## Side and distance

- The offset normal for a segment is its tangent rotated −90° ("right of
  travel"); that side is positive.
- The **cursor picks the sign**: project the cursor onto the chain, take the
  signed distance along that normal, and use its sign. Its magnitude is the
  distance, unless a typed value overrides it.
- This gives inward/outward on a closed loop for free, with no winding test.
- Consistent with the arc tool's existing convention, quoted in `SketchTool.h`:
  "the cursor's side of the chord decides which way it bows: that is a
  direction, not a dimension, so no number can express it."

## Corner rules

At each shared source vertex, between the two offset segments, classify by
`cross(tangent_in, tangent_out)` against the offset sign:

- **Tangent join** (|cross| ~ 0, e.g. line → tangent arc): no corner work, the
  offset endpoints already coincide.
- **Opening corner** (the offsets pull apart) - two styles:
  - **Round** (default): an arc centred on the *source* vertex, radius |d|,
    from the offset-in end to the offset-out start. Always valid, always
    exactly |d| from the source. This is the geometrically correct offset.
  - **Sharp**: extend both offset segments to their intersection (miter).
    Line/line via `intersectLineLine` on the infinite lines; line/arc and
    arc/arc via the circle intersections, choosing the root nearest the source
    vertex. **Falls back to Round when no intersection exists** (near-parallel
    segments would miter to infinity).
- **Closing corner** (the offsets overlap): intersect and trim both back to the
  intersection nearest the corner. If they do not intersect within their
  extents, leave them - the prune pass below handles it.

## Validity invariant and pruning

The one rule that makes this tractable without a straight-skeleton or a general
polygon-clipping pass:

> **Every point of a valid offset lies at distance exactly |d| from the source
> chain, and never closer.**

After corner fix-up:

1. Sample each candidate segment (lines by length, arcs by swept angle; min 8
   samples).
2. Test `distanceToChain(p) >= |d| - eps`, `eps = max(1e-4, 1e-3 * |d|)`.
3. Fully invalid segments are dropped. Partially invalid segments are split at
   the crossing parameter (binary search between a valid and an invalid
   sample) and the valid part kept.
4. If **nothing** survives, the plan is invalid: refuse the commit and toast
   ("Offset distance too large for this profile") through the same one-shot
   rejection channel the Dimension tool uses (`consumeDimRejection`).

`distanceToChain` is per-primitive: point-to-segment with the parameter clamped
to [0,1]; point-to-arc as `abs(|p-c| - r)` when the point's angle falls inside
the sweep, else the nearer endpoint; point-to-circle as `abs(|p-c| - r)`.

Honest limitation: at extreme offsets this can leave a small gap where a
general clipper would produce a clean junction. It will never emit
self-intersecting garbage, which is the failure mode that matters.

## Splines

An aerofoil section is two splines plus a trailing-edge line, and SVG imports
carry them too, so refusing splines refused the shapes people most want to
offset.

There is no exact offset of a B-spline, so this is an approximation with a
declared budget rather than a claim of exactness:

1. **Sample.** The chain carries a spline as a dense POLYLINE (the same
   `sampleSpline2D` sampling `buildWires()` uses, so the offset is measured
   against the curve the user actually sees and extrudes).
2. **Offset pointwise.** Each sample moves along its normal, where the normal
   comes from the *averaged* adjacent segment directions - averaging is what
   keeps a smooth curve smooth instead of faceting it.
3. **Refit at commit.** Control points are chosen along the offset curve and
   interpolated the way `Sketch` does; the count doubles until the interpolated
   curve lands within tolerance of the target.

Three things fell out of testing that the design did not anticipate:

- **The control-point cap matters more than the tolerance.** Chasing a 0.01 mm
  fit produced ~40 control points for a 5-point source - a solid mass of vertex
  markers that hides the curve and is horrible to edit. An offset of a 5-point
  spline should be about a 5-point spline, so the count is capped at **twice
  the source's** (min 8, max 48) and the resulting sub-0.1 mm deviation is
  accepted.
- **The prune band must be sized to the representation.** The analytic epsilon
  (1e-3 mm) is far tighter than a sampled offset's own error (curvature x
  spacing^2), so it condemned good geometry and chopped one smooth offset into
  three abutting fragments. Chains containing a spline get
  `eps = max(0.02 mm, 1% of |d|)`.
- **Sharp corners cannot miter against a curve.** A sampled curve has no
  analytic intersection, so `joinPoint` reports failure for any spline pair and
  the corner rounds instead - which is exact.

Still excluded: **text glyphs**. Text and SVG *lettering* come in as hundreds of
short `fromText` LINES, not splines, so they are untouched by this. Offsetting
them would be "embolden the letters" - a reasonable want, but a separate
feature with its own performance question.

## Output mapping

- Offset line → two new points + `addLine`.
- Offset arc → `addArc(centre, start, end, r)`. **Trap:** `addArc` sweeps CCW
  from start to end (see the comment in `commitMirror`). Emit arcs with
  start/end ordered so the sweep is CCW; for a clockwise-travelled arc, swap
  the endpoints - geometrically identical, and sketch arcs are undirected for
  region building.
- **[revised]** The original claim that a hairpin yields a corner arc of
  "nearly 360 degrees" was wrong. A round corner spans the angle between the
  two tangents, which is at most 180 degrees (a cusp). The genuine >180 degree
  case is a SOURCE arc that already sweeps more than half a turn, and that is
  what the test exercises.
- **[revised]** Consecutive committed segments must share ONE point id, or the
  result is a pile of disconnected edges and `buildRegions()` finds no loop.
  `applyOffset` keeps a local position→id cache for exactly this.
- Offset circle → `addCircle` with `r + d`; refuse when `r + d <= 0`.
- Endpoints weld onto coincident existing geometry via
  `SketchTool::coincidentPoint()`, same as Mirror.
- Result inherits the source chain's `isConstruction`; the panel checkbox can
  force construction on.
- Commit returns the new point/element ids, mirroring
  `commitMirror(outPoints, outLines)`.

## Interaction

Two phases, matching the two-step Escape convention documented on
`SketchTool::isPlacing()`:

- **Pick** - hover highlights the resolved chain; click captures it.
- **Distance** - cursor drives distance and side, ghost previews live; click or
  Enter commits, a typed number commits at that value (`applyDimension`), Esc
  returns to Pick, a second Esc leaves the tool.

After a commit the tool **stays in Offset** and returns to Pick (repeat offsets
are common, and Trim likewise stays active). Unlike Mirror it does not hand
back to Select or rewrite the selection.

Undo: the commit is wrapped in `recordSketchMutation` - one step. **[revised]**
A click or a typed value only *requests* the commit, via `offsetReadyToCommit()`;
the app drains that flag and performs the commit, so the click path and the
typed-value path funnel through one wrapped call rather than mutating from
inside the tool. (`applyDimension` is not wrapped at all three of its call
sites, so a tool-side mutation there would have silently skipped undo.)

## Inferences

They help the **distance**, not the geometry. Routing the drag cursor through
the existing snap path gives grid-rounded distances and endpoint alignment for
free. A dedicated "offset equals an existing dimension" inference would be
genuinely useful and is explicitly out of scope.

## Test matrix

Headless, no GL, no OCCT beyond what `Sketch` already pulls in. A shared helper
asserts the invariant (every sampled output point at |d| from the source) in
every case.

1. Square loop, offset out, Round - 4 lines + 4 quarter-circle corners; area
   `(a+2d)^2 - (4-pi)d^2`.
2. Square loop, offset out, Sharp - 4 lines; area exactly `(a+2d)^2`.
3. Square loop, offset in, `d < a/2` - area `(a-2d)^2`.
4. Square loop, offset in, `d > a/2` - plan invalid, nothing emitted.
5. Open L chain - convex side gains one corner element, concave side trims.
6. Circle - `r+d` and `r-d`; `r-d <= 0` refuses.
7. Line + tangent arc - no spurious corner inserted at the tangent join.
8. Slot (2 lines + 2 semicircular arcs, closed) - offset out is a concentric
   slot; offset in past the arc radius refuses.
9. Chain walk - T-junction stops at the branch; closed loop detected; a lone
   line with two free ends offsets as a single line.
10. Hairpin - round corner arc with sweep > 180 degrees round-trips through
    `addArc` and back out at radius |d|.
11. Sign - cursor on either side of an open chain flips the result across the
    source.

All 39 tests pass on the build VM. **[revised]** Cases 1, 2 and 8 originally
failed on fixtures, not on the code: an arc bulging the wrong way turns a
tangent join into a cusp and a slot into a bowtie. Sketch arcs always sweep CCW
from start to end, so the stored point order decides which way an arc bulges -
worth remembering when writing any arc fixture.
