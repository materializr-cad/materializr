# Sketch Fillet tool - plan

Status: DRAFT for review. No code written. Codex plan review is REQUIRED before
implementation (project hard rule) but Codex is usage-capped until 2026-09-22
17:03; independent reviews are recorded in the companion review log and the
Codex round is queued.

Branch: `feature/sketch-fillet-and-revolve-tools` (off merged `main`).

## Revision 1 (independent adversarial review, 2026-09-20) - SUPERSEDES conflicting text below

An independent reviewer checked every claim against the code and returned
REVISE. Accepted changes; where these conflict with the sections below, THESE
WIN (the original text is kept for the review history):

R1-1 (blocker). Tolerances: `buildRegions` treats a point within
`onLineTol = 1e-2` mm of a line as ON it and splits the line there
(Sketch.cpp:1042; `perimeterEpsilon = 1e-3` at :1128). So refuse when
`R < 0.05` mm OR `t*sin(theta) < 0.05` mm (near-collinear: tangent point B
sits t*sin(theta) off line 1). Matrix rows 2 and 4 must not test 0.001 mm or
179.99 deg as legal; add `buildRegions` closure asserts to rows 2 and 4.
R1-2. Float32: `SketchPoint::pos` is float. Store A, B, O as floats FIRST, then
set `arc.radius = |A_f - O_f|` (as the 3-point arc tool does,
SketchTool.cpp:2809). Refuse a sweep under ~1e-3 rad (float atan2 can flip
`end <= start` and the arc becomes ~360 deg). Test tolerances are float-ulp
based, not 1e-6.
R1-3. `Equal` does NOT survive: both lines shorten by t. Drop (or refuse on)
`Equal` on either line and count it in the report. Correction to the facts:
the solver DOES run automatically on Select drag (SketchTool.cpp:307), on
every constraint add/edit (Application.cpp:6340, :6494) and from the
properties panel (PropertiesPanel.cpp:822), so any surviving constraint that
the fillet broke will be "fixed" by pulling A/B off the arc. New test:
fillet a rectangle, edit the surviving Distance, `solve()`, assert |A-O| = R
and the region is still closed.
R1-4. API: `Sketch::replaceLineEndpoint(lineId, oldPtId, newPtId)` (changes
only the endpoint equal to the corner). `setLineEndpoints` could flip a line's
direction and corrupt `DistancePointLine.orientX` and signed `Angle`. The
region cache is safe (geometryHash mixes endpoint ids, Sketch.cpp:1707).
R1-5. Radius phase formula: `R = d_c * sin(theta/2)` where `d_c` is the
cursor's projection on the bisector from C (cursor acts as the arc CENTRE).
The apex-through-cursor formula is 262x sensitive at 170 deg and singular
toward 180. Guard `d_c <= 0`; clamp to the legal maximum.
R1-6. Wiring additions the first draft missed (Offset is the template, and
it needs all of these): request-flag commit (`m_offsetCommitRequested`
analogue, SketchTool.h:272), a dedicated tool panel like `renderOffsetToolPanel`
(Application_Dialogs.cpp:5087) with a typed radius field, `m_isPlacing = true`
during the Radius phase (else `applyDimension` bails, SketchTool.cpp:446, and
Esc breaks), a `cancelFillet`, the `setMode` reset (SketchTool.cpp:55), the
`SketchPlugin.cpp:137` applyDimension route, Esc handling (:353). Correction:
`recordSketchMutation` does NOT wrap every branch of `onMouseDown`
(Dimension bypasses it; Select snapshots manually) - verify Fillet's commit
path is wrapped. Touch: no hover, so tap-to-pick then typed radius or a
second tap. Raw-cursor: say which route (Trim's gate at :74 vs Offset's own
dispatch at :143-146); Fillet takes Offset's. The tool test needs the
`SvgImport::place` / `TextSketch::generate` link stubs
(`test_sketch_trim_polygon.cpp:16-21`).
R1-7. Degenerate inputs: check finiteness and non-zero length BEFORE computing
u, v, sin(theta) (NaN comparisons otherwise pass `t >= length`). Refuse a
corner whose `onCurveId >= 0`. Refuse (or warn) when another point lies within
`onLineTol` of the span being trimmed (a T-junction gets stranded).
R1-8. `pruneOrphanPoints()` is sketch-wide (it also deletes every standalone
Point-tool point). Trim/delete already share that precedent, but prefer
`removeElement(cornerId)` + targeted constraint removal.
R1-9. Extra matrix rows: adjacent-corner shared T point and a digon (u = v);
Offset of a filleted profile with distance > R; undo mid-Radius phase (stale
corner id -> `filletCorner` returns an error, not a crash); zero-length and
coincident-endpoint lines in the refusal row; `findAxisAlignedRect` false
from a non-filleted side too; fillet then `setRectangleSize` is a no-op, not
a crash.
Verified by the reviewer and unchanged: arc CCW convention, `angleInArc`,
`buildWires` arc handling, `removeElement` polygon cascade, missing endpoint
setter, Trim's remove+re-add precedent, rectangle constraint layout, the
`Application.cpp:8111` enum cast, and the numeric example (90 deg corner,
R=5: A=(5,0), B=(0,5), O=(5,5); cross(A-O,B-O) = -25 so start=B, end=A).

## Goal

Onshape-style sketch fillet: pick a corner where two sketch lines meet, replace
it with a tangent arc of radius R, trimming both lines back to the tangent
points. The result is ordinary sketch geometry (line, line, arc), exactly like
the Offset and Mirror tools: no new operation type, no serialization, no
associativity.

Concept sheet: `docs/superpowers/specs/2026-09-20-sketch-fillet-concept.svg`. Its panels 3-4
promised more than v1 delivers - see "Scope" for the honest mapping.

## Scope

v1 (this plan):
1. Core geometry: `Sketch::filletCorner(pointId, radius, FilletResult&)`, pure
   2D, headless-testable in `materializr_core`.
2. Interactive tool `SketchToolMode::Fillet` with two phases, copying the
   Offset tool's Pick / Distance phases: hover a corner (highlight + preview
   arc), click to pick it, move the cursor or type a number to size the arc
   live, click / Enter to commit. This is concept panels 1-3.
3. Later phase (separate PR, not v1): AI tool `fillet_sketch_corner`.

Deferred, explicitly NOT in v1:
- Concept panel 4 and any post-commit resize/"pull a line back": needs fillet
  metadata persisted with the sketch (ProjectIO, SketchEditOp snapshots,
  CombineSketchesOp remap) plus Tangent+Radius constraints. See Decisions.
- Line-to-arc and arc-to-arc fillets. v1 is line-to-line only.
- "Delete the arc restores the sharp corner" (needs the remembered corner).

## Verified facts this plan rests on (from code reading, 2026-09-20)

- Arc = `{id, centerPointId, startPointId, endPointId, radius}`; every consumer
  sweeps CCW from start to end (`SketchRenderer.cpp:520-547`, `angleInArc`
  SketchTool.cpp:3025, `buildWires` Sketch.cpp ~1181). Endpoints must sit at
  distance `radius` from the centre; region building matches by point ID, not
  position, so the arc must SHARE the trimmed lines' new endpoint IDs.
- A corner is two `SketchLine`s sharing one `SketchPoint` id. `getLines()` is
  const and there is NO setter for a line's endpoints. Trim's precedent is
  remove + re-add (new line ids, constraints on the old ids are lost).
- `removeElement(lineId)` on a polygon-owned line deletes the whole polygon
  (PR #121). Polygons have no editable line/vertex lists.
- Rectangles have no record: `addRectangle` adds 4 H/V and 2 Distance
  constraints; `findAxisAlignedRect` re-detects them geometrically.
- `pruneOrphanPoints()` also drops every constraint that references a vanished
  id. The solver is Gauss-Seidel relaxation, NOT automatic after edits; Trim
  never solves and neither should a fillet commit.
- Solver `Tangent` (arc, line) only moves the arc's centre and does not check
  the endpoint sits on the line; combined with `Radius` (which re-projects
  endpoints via `resizeArc`) it can skew neighbouring lines.
- `recordSketchMutation` already wraps every `onMouseDown` in one undo step
  (`Application_Viewport.cpp:6293-6373`), so a fillet commit is one undo step
  with no extra plumbing. The touch/toolbar/i18n wiring list is in "Wiring".

## Decisions

D1. Add NO constraints in v1 (no Tangent, no Radius). Reason: on a rectangle
corner they would fight the H/V/Distance constraints, and the solver's Tangent
is not endpoint-aware. The radius lives in `SketchArc::radius`. Consequence
(stated to the user in the tool's status text): the arc is not parametric;
later drags of a line end will not keep it tangent. This is why concept panel
4 is v2.

D2. Keep the two lines' ids. New method `Sketch::setLineEndpoints(lineId,
startPtId, endPtId)` (small, mirrors `moveEndpointPreservingArcs`'s intent) so
H/V/Parallel/Equal/Perpendicular constraints on the lines survive. Constraints
that reference the CORNER POINT (Distance, Coincident, Fixed) cannot survive
and are dropped by `pruneOrphanPoints`; count them and report "removed N
dimensions". Refuse outright if the corner point is `Fixed`.

D3. Refuse (return an error string, change nothing) when the corner is unsafe:
- point is owned by a `SketchPolygon` (centre or vertex) or either line is a
  polygon line;
- point is a spline control point, an arc/circle endpoint or centre;
- not exactly two lines meet at the point (1, 3+); lines with
  `isConstruction` or `fromText`;
- the two lines are (near) collinear: `sin(theta) < 1e-6`;
- radius not finite or <= 0;
- the tangent distance `t = R / tan(theta/2)` reaches or exceeds either line's
  length (need at least `kMinRemain = 1e-4` mm left);
- the corner point is `Fixed`.
Rectangle corners ARE allowed (the main use case); the rectangle's size
editing silently stops recognising it afterwards (documented, tested).

D4. No auto-clamp of an oversized radius in the core; the core refuses. The
interactive phase clamps the LIVE preview radius to the maximum legal value and
shows it red/limited, so a drag never lands in an illegal state.

D5. One arc per fillet, always the short arc (sweep = pi - theta < pi), CCW
start->end chosen by `cross(A-O, B-O) > 0` (else swap), matching the 3-point
arc tool's swap logic (SketchTool.cpp:2817-2835).

## Geometry (core)

Corner C, unit vectors u, v along the two lines AWAY from C, theta = angle
between them (0 < theta < pi).
- t = R / tan(theta/2)
- A = C + t*u, B = C + t*v (tangent points; become the lines' new endpoints)
- O = C + (R / sin(theta/2)) * normalize(u + v) (arc centre)
- New points: A, B, O (as ids). Lines re-pointed: line1 corner->A, line2
  corner->B. `addArc(O, start, end, R)` in CCW order (D5).
- Remove the corner point only if nothing else references it, then
  `pruneOrphanPoints()`.
Float32 positions: `SketchPoint::pos` is `glm::vec2` (float). Compute in double,
store float; tangent-point-on-circle error must stay < 1e-4 mm for R up to 1e4
(tested).

## Interactive tool

Phases (mirrors Offset): Pick -> Radius.
- Pick: raw (unsnapped) cursor, like Trim/Offset. Nearest eligible corner point
  within the weld radius highlights, with both lines lit. Ineligible corner:
  show a grey "cannot fillet: <reason>" hint using the D3 reason.
- Radius: live radius = the value at which the arc's apex passes through the
  cursor: `R = d / (1/sin(theta/2) - 1)` with `d` = cursor's projection on the
  bisector from C. Typed number + Enter uses the existing `applyDimension`
  path (as Offset). Default radius shown before any movement: 10% of the
  shorter adjacent line, last-used radius remembered per session.
- Commit on click or Enter: `filletCorner`, then stay in the tool in Pick phase
  so a run of corners is fast. Esc backs out of Radius to Pick, then leaves.
- Preview overlay: ghost arc + trimmed-away stubs, drawn like `drawTrimHover`.

## Wiring checklist (from the internals survey)

Append `Fillet` to `SketchToolMode` AFTER `Point` (toolbar highlight casts the
enum to int, `Application.cpp:8111`). Touch: `SketchTool.cpp` raw-cursor gate
(:74), `onMouseDown`/`onMouseMove` dispatch, `setMode` reset, Escape handling;
`SketchRenderer` overlay (`drawFilletPreview`, beside `drawTrimHover`);
`Toolbar.h` ToolAction, `Application.cpp:2438-2443` dispatch; toolbar buttons
in all three places (`Toolbar.cpp` ~199-203, 764-767, `ImTouchLayout.cpp`
modify group 887-893); icon in `TouchIcons.h`; i18n via
`tools/i18n_catalogue.py` + regenerate `src/i18n_catalogue.h`; update
`ShortcutsPanel.cpp:126` list; tests + `tests/CMakeLists.txt`.
Gates to run: em-dash, i18n catalogue drift, `tools/units_audit.py`, the
msvc `far`/`near` grep, full build + ctest.

## Stress-test matrix (implement as tests; each row is an assertion)

Core, `tests/test_sketch_fillet.cpp` (headless):
1. 90-degree corner, R=5: tangent points at (5,0)/(0,5) offsets, centre at
   (5,5), arc endpoints exactly R from centre, sweep = 90 deg, CCW.
2. Acute (30 deg) and obtuse (170 deg) corners: same invariants; t larger for
   acute; near-collinear (179.99 deg) refused; exactly collinear refused.
3. Both line orientations and both point orderings (`start`/`end` swapped on
   either line): arc still CCW, endpoints shared with the lines.
4. Tangency proof: the line direction at A/B is perpendicular to (A-O)/(B-O)
   to 1e-6 (dot product), for R in {0.001, 1, 100, 1e4} (float32 stress).
5. Refusals leave the sketch byte-identical: R<=0, NaN, inf, R too large by
   exactly-at-limit and just-over, line shorter than t, 1 line at point, 3
   lines at point, polygon vertex, polygon centre, spline control point, arc
   endpoint, `isConstruction`, `fromText`, `Fixed` corner.
6. Rectangle: fillet one corner, then all four; region building
   (`buildRegions`) still yields exactly one region with area = w*h -
   4*(R^2 - pi*R^2/4); H/V constraints on the lines survive; the two Distance
   constraints touching a filleted corner are dropped and counted; a
   `findAxisAlignedRect` on a filleted side returns false (documented).
7. Two adjacent corners on the same line with R1+R2 > line length: second is
   refused with a clear reason; with R1+R2 = length - 1e-3 it succeeds.
8. Triangle, pentagon-as-lines, and a polygon-tool pentagon (refused).
9. Chained undo: fillet then `SketchEditOp` undo restores the pre-fillet
   sketch exactly (points, lines, arcs, constraints).
10. Save/load round trip and `CombineSketchesOp::mergeInto` of a filleted
    sketch: arcs and lines remain a closed region.
Tool, `tests/test_sketch_fillet_tool.cpp` (drive via `onMouseDown/Move` like
`test_sketch_trim_polygon.cpp`): hover highlights the right corner; Pick
ignores an ineligible corner; live radius formula matches core; typed value;
Esc semantics; commit leaves tool in Pick phase.
Extrude the filleted profile via `ExtrudeOp` in one test: valid solid, volume =
prism of the filleted area (proves wires/regions consume the arc correctly).

## Risks / open questions for review

- R1. Lack of constraints (D1) may surprise users expecting Onshape's
  parametric fillet. Mitigation: status text; v2 plan.
- R2. `setLineEndpoints` widens `Sketch`'s API; alternative is Trim-style
  remove+re-add (loses constraint ids). Prefer the setter.
- R3. Float32 positions vs tangency tolerance at very large R.
- R4. A corner shared by exactly two lines where one is part of a closed
  loop that also carries a Distance dimension elsewhere: dimension survives
  but is now measured to a different point; acceptable, tested in row 6.

## Out of scope

Line-arc fillet, arc-arc fillet, chamfer (natural sibling: same corner logic,
straight cut; consider after v1), variable radius, 3D body fillet (exists).
