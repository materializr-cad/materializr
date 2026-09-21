# AI tool `revolve_sketch` (wraps the existing RevolveOp / "Lathe") - plan

Status: DRAFT for review. No code written. Codex plan review is REQUIRED before
implementation (project hard rule); Codex is usage-capped until 2026-09-22
17:03. Independent reviews go in the companion review log; Codex round queued.

Branch: `feature/sketch-fillet-and-revolve-tools`.

## Revision 1 (independent adversarial review, 2026-09-20) - SUPERSEDES conflicting text below

An independent reviewer checked every claim against the code and returned
REVISE. Accepted changes; where these conflict with the sections below, THESE
WIN:

R1-1 (blocker). No `lastError()` read after a failed `pushOperation`:
`History::pushOperation` takes the `unique_ptr` by value and destroys the op
when `execute` returns false (History.cpp:33, 87-89), so `raw->lastError()`
is a use-after-free. Do all pre-checks in the dispatcher BEFORE
`pushOperation`; the op keeps only null/finite/commit-guard checks.
R1-2 (blocker). D1/D4 contradiction resolved by choosing ONE convention: match
the UI in v1. `applyRevolve` uses the raw mapped world direction with no
negation (Application_Dialogs.cpp:4353-4366). The tool does the same for every
axis source, so a partial angle sweeps the same way as the Lathe popup. State
the sense in the result text in words, and add a UI-vs-tool parity test.
(The reviewer's derivation - a uniform negation gives a user-space
right-hand rotation for every axis - is recorded as the alternative if a
user-space convention is wanted later; it is a user-visible decision.)
R1-3 (blocker). D2 dropped for v1: NO `region_indices`. Indices shift when
regions change; `ExtrudeOp` persists interior sample points (`m_regionPts`,
ExtrudeOp.cpp:332-348, 600), not indices. v1 revolves the LARGEST region (what
the UI does) and says so in the result text when the sketch has more than one
region. Reload behaviour then matches creation with no `RevolveOp` format
change. (`serializeParams` uses `char buf[256]` at RevolveOp.cpp:257, so any
appended field would truncate silently - another reason to defer.) `RevolveOp`
is also not in the sketch-edit cascade (Application_InteractiveOps.cpp:
1173-1190 covers only `ExtrudeOp` and `PushPullOp`).
R1-4. Hardening placement: `execute` also runs on Disable/Enable replay and
reflow (History.cpp:571-640); a stricter guard would fail legacy ops. Keep only
null-target, finite-angle and the `commitGuard`-style volume/validity check in
`execute`, run BEFORE `addOrPutBody` (else a failed NewBody leaves a body
behind). Put the crossing/perpendicular checks in the dispatcher
(creation-time only). `doc.getBody` on a stale id may throw into the silent
catch, so check the target explicitly.
R1-5. "Crosses the axis" is only well-defined when the axis lies in the
profile plane (`|d.n| ~ 0` and plane passes through the axis). For a
coplanar axis: transform the face to the axis frame and use
`BRepBndLib::AddOptimal(useTri=false, useTol=false)` on the signed distance
(plain `Add` inflates by tolerance -> false positives on touching/tangent
profiles); sampling is unnecessary. If the axis is perpendicular to the plane,
refuse early ("sketch plane is perpendicular to the axis": a full 360 sweeps
the face onto itself, zero volume). If the axis is parallel to the plane but
offset by h, folding happens when in-plane s takes both signs; refuse or rely
on the validity/volume guard, and say which. Tolerance is ~1e-4 mm, not 1e-6
(sketch points are floats, ~1e-6 error at 10 mm).
R1-6. Direction: v1 supports a positive angle in (0, 360] only and states the
fixed sweep sense in the description; a signed angle is deferred.
R1-7. Axis inputs: a missing/unknown `axis_id` is an ERROR (the UI silently
falls back to axis (0,0,1)); there is no `requireAxisId` helper yet - add one.
Accept `axis_id` alone as `construction`. Read the axis direction at call
time (`flipAxisDirection`, Document.h:408, makes it mutable) and describe the
sweep relative to the direction `describe_scene` shows. Reuse `withExtrudeMode`
(AiToolSchema.cpp:52-58) for mode/target; stray `axis_id` handling follows
the `target_body_id` "ignored" precedent, tested.
R1-8. Test-matrix corrections: the existing dispatcher test sketch plane is
horizontal (test_ai_tool_dispatcher.cpp:1529), so axis `z` is degenerate
there - use a vertical sketch plane that contains the axis for the axis tests.
Row 2: 359.9999999 is within 1e-6 of 360 and takes the FULL branch. Row 11:
a profile at +x lies ON axis x - use +y for axis x. Expected quadrants for a
90 deg sweep (user space): axis z, profile at +x -> x>=0,y>=0; axis y,
profile at +z -> z>=0,x>=0. Pappus row: 2*pi*22.5*50 = 7068.58 mm^3 holds only
for radial width 5 and axial height 10 - state it (radial width 10 gives
8482.3). Add: perpendicular plane (full and partial), axis parallel but
offset (fold), tangent circle, positive volume for every axis (no inside-out
solid), revolve onto a thread/shell body (reflow), a legacy blob through
`OperationFactory`, and the UI-vs-tool parity test.
R1-9. Factual fixes: the `ExtrudeOp` guard is at ExtrudeOp.cpp:476-482; the
`extrudeSketch` handler spans AiToolDispatcher.cpp:1568-1683; ExtrudeOp has no
separate "did not consume the target" check beyond the mass check.

## Goal

Let the AI assistant spin an existing sketch's profile around an axis into a
solid (or cut/join it with an existing body), the way the UI "Lathe" button
does. Mirrors `extrude_sketch`. The UI already ships this operation
(`RevolveOp`, toolbar "Lathe" for a selected sketch, "Revolve" for a body);
the assistant simply has no tool for it. The plan also hardens `RevolveOp`
itself, because the tool would expose its unchecked failure modes to a model
that cannot see the viewport.

## Verified facts (code reading, 2026-09-20)

- `RevolveOp::execute` (RevolveOp.cpp:53-152): rebuilds the axis from six
  doubles (zero-length direction -> false); uses `BRepPrimAPI_MakeRevol`, full
  sweep when angle within 1e-6 of 360; modes NewBody/Union/Subtract/Intersect
  via `setBooleanShapes`. Whole body wrapped in try/catch returning bare
  `false` (no message).
- The op does NOT clamp or validate the angle (only the UI slider clamps).
  `setAngle` accepts 0, negative, NaN.
- Boolean modes never null-check the target body, the boolean result, or run
  `BRepCheck_Analyzer`/volume checks. `ExtrudeOp.cpp:472-481` has exactly such
  a commit guard; `RevolveOp` has none. A subtract that consumes the target
  would "succeed".
- Nothing checks a profile that crosses the axis (self-overlapping solid,
  `IsDone()` can still be true) or a degenerate profile.
- `rebuildProfileFromSketch` (RevolveOp.cpp:173-197) picks the LARGEST-bbox
  region (holes included), matching `applyRevolve` (Application_Dialogs.cpp:
  4374-4392). Other disjoint regions are silently dropped. `serializeParams`
  stores only the sketch id, so reload always re-derives the largest region.
- UI axis choices: any construction axis, or world X / Y / Z through the
  origin ("user" axes; user Y -> world Z, user Z -> world Y). Profile is
  `regions[bestIdx].face`, already world-space. The user<->world map has
  determinant -1; `rotateBody` negates its angle to compensate.
- There are NO unit tests for `RevolveOp` (only a kernel probe,
  `probe_kernel_matrix.cpp:306`). Nothing covers execute, modes, undo,
  serialize/deserialize, or reload.
- `extrude_sketch` pattern to copy: `requireSketchId`/`requireBodyId`,
  `region_indices` validation, mode enum + `target_body_id`, `pushOperation`,
  `markMeshesDirty` (AiToolDispatcher.cpp:1568-1662; schema AiToolSchema.cpp
  ~337; tests test_ai_tool_dispatcher.cpp ~1289-1560).

## Tool contract

`revolve_sketch`
- `sketch_id` (number, required)
- `axis` (string, required): one of `x`, `y`, `z` (user space, through the
  origin, same convention as `construction_axis`), `sketch_u`, `sketch_v` (the
  sketch plane's own X / Y direction through the sketch origin), or
  `construction` with `axis_id`.
- `axis_id` (number, required iff axis = construction): id from
  `describe_scene`.
- `angle_degrees` (number, optional, default 360): must be finite and in
  (0, 360].
- `region_indices` (integer array, optional): same semantics as
  `extrude_sketch` but see D2.
- `mode` (string, optional, default `new_body`): `new_body | union | subtract
  | intersect`; `target_body_id` required unless `new_body`.

Result text: new body id, volume, and the sweep sense actually used, e.g.
"Revolved sketch 3 360 deg about user z axis into new body 7 (volume 1234.5
mm^3)". Error messages are specific (the op only returns `false`; the
dispatcher runs its own pre-checks so failures explain themselves).

## Decisions

D1. Sweep sense convention: right-hand rule about the axis direction AS THE
USER SEES IT (user space, Z-up). Because user->world is a reflection, the
mapped world axis direction must be NEGATED so a positive angle is
counter-clockwise viewed from the axis's +end in user space. Pinned by a test
(quadrant occupied by a 90 degree revolve), not left to reasoning.

D2. Regions: extend `RevolveOp` with optional `m_regionIndices`, serialized in
`serializeParams` (absent = legacy behaviour, so old project files load
unchanged), and used by `rebuildProfileFromSketch`. Without this a reloaded
document would silently revolve a different region than the AI chose. If no
`region_indices` are given, keep the UI's largest-region behaviour but SAY SO
in the result text when the sketch has more than one region.
Multiple selected regions go in as a compound like `extrude_sketch` does; a
disjoint set revolves each. Decision needing review: whether multi-region
compounds are worth the risk in v1 or v1 accepts exactly one region.

D3. Harden `RevolveOp::execute` (benefits the UI too):
- reject angle non-finite or outside (0, 360];
- null-check target body for boolean modes;
- after the kernel call: result non-null, `BRepCheck_Analyzer` valid, volume >
  epsilon; for Subtract/Intersect also that the result is non-empty; for
  Subtract that it did not consume the whole target (same guard as
  `ExtrudeOp.cpp:472-481`);
- profile-crosses-axis check: signed distance of the profile's vertices and
  sampled edge points to the axis line; points strictly on both sides (beyond
  `1e-6` mm) -> fail with "profile crosses the axis". Touching/lying on the
  axis stays legal.
- give `execute` a way to report a reason (`lastError()` string on the op) so
  the dispatcher stops returning "the operation failed to execute".

D4. Axis `sketch_u` / `sketch_v`: origin = `sketch->getPlane().Location()`,
direction = the plane's `XDirection()` / `YDirection()` (world coordinates,
already handedness-correct - do NOT apply the user-axis negation here; see
the sweep-sense test for this axis). Not in the UI today; new logic.

D5. Free `axis_x/y/z` + origin is deliberately excluded for v1 (users and
models rarely need it; construction axes cover it; `construction_axis` tool
can create one first).

## Stress-test matrix (each row an assertion)

Op level, new `tests/test_revolve_op.cpp` (headless, the coverage gap):
1. Full 360 revolve of a 10x5 rectangle whose near edge is at radius 20 about
   the sketch V axis: volume equals Pappus `2*pi*rbar*A` = `2*pi*22.5*50` to
   1e-3 relative; valid solid.
2. Partial angles 90, 180, 359.9999999 (treated near-full? define), 0.0001:
   volume scales with angle/360; caps present; angle 360 vs 359.9999995
   boundary at the 1e-6 threshold does not produce a seam-broken solid.
3. Profile touching the axis (rectangle edge on the axis): valid solid, no
   degenerate faces (`BRepCheck_Analyzer`). Profile straddling the axis:
   refused with the crossing message, document unchanged.
4. Degenerate profiles: zero-area (collinear "face"), sliver 1e-9 wide,
   profile with a hole (ring cross-section -> hollow solid, volume checked).
5. Angle 0, -90, NaN, +inf, 360.0001, 1e9: refused, no history step added.
6. Zero-length axis direction, axis NaN: refused.
7. Union / Subtract / Intersect with a real target: results valid; Subtract
   that removes the entire target is refused and target unchanged; Intersect
   with a disjoint body (empty result) refused; missing/stale target id
   refused.
8. Undo/redo restores body set and the previous target shape exactly;
   redo after undo yields identical volume.
9. `serializeParams`/`deserializeParams` round trip, including
   `m_regionIndices`; a legacy blob (no region field) loads with legacy
   behaviour; `rehydrateFromReload` re-derives the same profile after the
   sketch is edited-then-saved-then-reloaded (uses `test_full_replay`
   patterns, `/tmp`-writing suites run unsandboxed).
Dispatcher level, extend `tests/test_ai_tool_dispatcher.cpp`:
10. Each axis token (`x`,`y`,`z`,`sketch_u`,`sketch_v`,`construction`): the
    revolved bbox lies where expected; user-space y vs z not swapped
    (asymmetric profile offset in user Y catches a swap).
11. Sweep-sense pin (D1): profile at user +x, axis `z`, 90 degrees -> solid
    occupies user quadrant x>=0,y>=0. Repeat for axis `x` and `y` and for
    `sketch_v`.
12. Arg validation matrix: unknown sketch id, non-numeric ids, missing
    `axis_id`, `axis_id` given without `construction`, stray `axis_id`,
    unknown axis string (case-folding consistency with the other tools),
    `mode` without `target_body_id`, region index out of range / duplicate /
    non-integer / non-array, empty sketch (no closed region), angle strings
    ("90" as string must be rejected, not coerced).
13. Multi-region sketch: default revolves largest and says so; explicit
    `region_indices` revolves the chosen one and reload re-derives that same
    region (D2 proof); compound of two disjoint regions (if kept in v1).
14. Undo removes the new body; a failed revolve adds no history step and no
    body; `markMeshesDirty` fires only on success (same style as
    `ExtrudeSketchUndo...`).
15. Schema tests: `revolve_sketch` in `AllToolsContainsExactlyTheExpected
    Tools`, param shape test (required/optional split).
Performance/limits: revolving a 500-vertex text-glyph sketch region and a
spline-bounded profile completes and validates (or is refused with a message,
never crashes or hangs); wall-clock budget asserted loosely (< 10 s).

## Wiring

`AiToolSchema.cpp` entry (+ `boolean` param type is available upstream but not
needed; use `str`/`num`/`intArray`); `AiToolDispatcher.cpp` function +
dispatch line; the system-prompt text in `AiToolSchema.cpp` may need one
sentence on axes; `tools/units_audit.py` allowlist entries for any new `mm`
literals in result text; `tests/CMakeLists.txt` for `test_revolve_op`; run
em-dash, units audit, full build + ctest.

## Risks / open questions for review

- R1. D2 changes an existing op's serialized format (additive, optional).
  Must prove old projects load byte-identically; consider instead restricting
  v1 to largest-region and deferring `region_indices`.
- R2. Profile-crosses-axis detection by sampling can miss a thin crossing
  between samples; use exact edge/axis tests where the edge is a line, sample
  densely for curves, and rely on `BRepCheck_Analyzer` as the backstop.
- R3. Hardening `RevolveOp::execute` changes UI behaviour (previously
  "successful" bad revolves now fail). That is the intent, but needs a UI-side
  message so the user sees why (status text), not a silent no-op.
- R4. Sweep-sense negation (D1) is the classic place for a silent mirror bug;
  hence tests 11 and 10's asymmetric profile.

## Out of scope

Revolve of a body (the UI "Revolve" rotate-a-body mode; `rotate_body` covers
it), free-form axes (D5), revolve around a sketch line, 3D thin/surface
revolve, symmetric (both-sides) revolve.
