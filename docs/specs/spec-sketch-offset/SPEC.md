---
id: SPEC-sketch-offset
companions:
  - occt-offset-findings.md
  - integration-points.md
  - state-machine.md
  - tolerances.md
sources: []
---

> **Canonical contract.** This SPEC and the files in `companions:` are the complete, preservation-validated contract for what to build, test, and validate. Source documents listed in frontmatter are for traceability — consult them only if you need narrative rationale or prose color this contract intentionally omits.

# Sketch Offset Tool

## Why

A pain to solve. Materializr's sketch mode ships thirteen tools — `Select, Line, Circle, Rectangle, Arc, Spline, Polygon, Trim, Text, Svg, Mirror, Dimension, Airfoil`, plus a `None` sentinel — and not one of them produces a parallel copy of a curve. Offset is table stakes in 2D CAD: it is how you get wall thickness, a clearance ring around a boss, a pocket outline inset from a face boundary, or a cut line allowing for tool width. Without it a user draws the second loop by hand and dimensions every segment to keep it parallel, which is slow and drifts out of true the moment the first loop changes.

The capability is already proven in-tree. `SvgImport.cpp:657` calls `BRepOffsetAPI_MakeOffset` to convert SVG strokes into outlines, so OCCT's 2D offset works on this build against the pinned OCCT 7.9.3. What is missing is a sketch-mode tool in front of it, and a conversion of the result back into editable sketch entities rather than the flat polyline SvgImport settles for.

## Capabilities

- **CAP-1** — drag to offset
  - **intent:** With a valid closed loop selected, the user drags to place a parallel copy: the side of the loop the cursor is on picks the direction, the cursor's distance sets the magnitude, and a live ghost preview tracks both.
  - **success:** Dragging outward from a selected 40×20 rectangle produces a preview enclosing the source; dragging inward produces one enclosed by it. The full input protocol and every cancellation path are defined in `state-machine.md`; a test drives those transitions directly rather than inferring them.

- **CAP-2** — typed exact distance
  - **intent:** The user commits an exact offset distance by typing it, rather than relying on drag precision.
  - **success:** Per generated entity, an analytic invariant holds — a line maps to a parallel line at perpendicular distance `d`; an arc derived from a source arc has the same centre and radius `source ± d`; a join fillet has radius exactly `|d|` centred on the source vertex. Sampling "every point" is **not** the criterion: it cannot establish an everywhere-property, and perpendicular distance is undefined at source vertices. An earlier draft also demanded a Hausdorff bound; it named no metric, bound or fixture, and the analytic invariants already pin every generated entity, so it was removed rather than elaborated.

- **CAP-3** — output is editable geometry
  - **intent:** The offset result is ordinary sketch entities, so it can be selected, dimensioned, constrained, trimmed and extruded like drawn geometry.
  - **success:** Demonstrated end to end, not inferred from entity types: offset a rounded rectangle, then (a) select and dimension a generated entity, (b) confirm `buildRegions()` yields a region for the ring between source and result, (c) extrude that ring, (d) save, reload, and confirm every generated entity returns with matching position, radius and arc winding. Emitting `SketchLine`/`SketchArc` alone proves none of this.

- **CAP-4** — invalid selection is refused
  - **intent:** A selection that is not exactly one simple closed loop is rejected with a message naming the actual problem, instead of the tool guessing.
  - **success:** Each of these produces a **distinct** toast and leaves the sketch unmodified: open chain; two disjoint loops; a branching graph (any vertex of degree > 2); a self-intersecting loop; a selection containing a spline; a zero-length or duplicate edge. A selected polygon is **not** refused — `SketchPolygon` carries a generated `lineIds` vector and `SketchTool` has no polygon selection set, so a polygon reaches the walk as ordinary lines.

- **CAP-5** — degenerate and unrepresentable offsets are refused
  - **intent:** An offset that collapses, splits, or cannot be built reports failure rather than appearing to succeed or committing something the user did not ask for.
  - **success:** Each refusal path leaves the entity count unchanged. Four have a known fixture and are each tested with their own message: `IsDone()==false` (circle offset ≥ radius); `IsDone()==true` with an empty shape (rectangle inward past collapse); more than one wire (dumbbell whose neck pinches); any curve type other than `GeomAbs_Line`/`GeomAbs_Circle`. Two further checks — the result wire not closed, and `BRepCheck_Analyzer::IsValid()` failing — have **no known fixture** on OCCT 7.9.3: every measured result was valid. They are required as defence against large or numerically awkward offsets and share one refusal message; if a fixture is ever found, it gets its own test and message. `|d|` below `kMinOffsetDistance` or non-finite is refused before OCCT is called at all.

- **CAP-6** — output persists
  - **intent:** Offset geometry behaves like every other sketch entity across undo and file round-trips, and cannot be corrupted by edits made while the tool is live.
  - **success:** Offset, then undo, restores the pre-offset entity count exactly. Offset, save, reload reproduces every generated entity. Deleting or undoing a source entity while Offset is active cancels the operation rather than committing against stale ids.

## Constraints

- `SketchToolMode::Offset` must be **appended after `Airfoil`**, never inserted. The enum's header records that the toolbar mirrors the raw enum index and hardcodes those indices, so a mid-list insert silently highlights the wrong button.
- **The source wire must be validated as simple and non-self-intersecting before `Perform`.** A bow-tie source returns one wire, six edges, and `BRepCheck_Analyzer::IsValid() == true` — structurally valid, geometrically meaningless. No downstream check catches this. Measured; see `occt-offset-findings.md`.
- **Success requires six checks, not one:** `IsDone()`; a non-empty shape; exactly one wire; that wire closed; `BRepCheck_Analyzer::IsValid()` on the result; and every edge classifying as `GeomAbs_Line` or `GeomAbs_Circle`. The first three each fail independently on a real fixture. The closure and validity checks are not redundant with source validation — a simple source does not guarantee a simple result at large or numerically awkward offsets. The result must additionally pass the **same non-self-intersection predicate** used on the source, run on the **phase-1 entity plan** (where entities and point ids exist) and before phase 2 applies it. It cannot run on the raw OCCT result: that is `TopoDS` edges, and the predicate is defined over sketch entities and ids.
- **`|d|` must be finite and at least `kMinOffsetDistance`, checked before OCCT is called** — and **refused** below it, never clamped to it. Offsets of `0` and `1e-9` succeed and return coincident geometry, which welding would then corrupt. All tolerances are named with values in `tolerances.md`.
- **Offset must not weld through `findCoincidentPoint`.** Its threshold is `0.3f * snapScale()` — a UI snap radius — so a valid 0.1 mm offset would weld its output onto the source. Weld result-to-result endpoints only, at `weldTol`, never result-to-source.
- **The source loop is validated by an explicit predicate, not by outcome.** Adjacency is **two-stage**: exact shared point id first (scale-free, and the only stage drawn geometry ever needs, since `SketchLine` already references `startPointId`/`endPointId`), then a `weldTol` union of still-distinct coincident endpoints, which is what lets an imported closed loop validate instead of reading as an open chain. Then: every vertex degree exactly 2; one connected component; no degenerate or geometrically-duplicated entities; and no non-vertex intersection between any entity pair. That last rule is what rejects a bow-tie, which passes every other check. Full predicate and all tolerance values in `tolerances.md`.
- **Positional tolerances must be scale-aware and evaluated in double.** `SketchPoint::pos` is `glm::vec2`, and a float ULP is 7.6e-6 mm at 100 mm and 6.1e-5 mm at 1000 mm — so a fixed `1e-6` predicate falls below representable resolution and changes meaning as a sketch is translated away from the origin.
- **`beginOffset` must restore the captured selection after `setMode`.** `setMode` clears the element selection for every mode but `Select`, so the source would otherwise not be selected during the operation or after commit — breaking the nesting behaviour the spec promises.
- **The assembled wire is reoriented to a canonical winding** (positive signed area / CCW; a lone circle emitted CCW explicitly) before `Perform`, so that a positive argument means outward. Unordered selected entities carry no orientation of their own.
- **Escape needs an explicit Offset branch** in `Application.cpp`'s sketch-mode handler. Escape there cancels only for Dimension or `isPlacing()`; otherwise it calls `exitSketchMode()`. Without the branch, cancelling an offset would exit the entire sketch.
- **Every exit from Offset funnels through `cancelOffset()`** — `setMode`, sketch exit, popup close, Escape, failed commit — mirroring how `setMode` already clears `m_mirrorActive`.
- **Failures are reported via `Application::showToast`, not stdout.** `beginOffset`/`commitOffset` return a structured error enum; a bool cannot carry CAP-4 and CAP-5's distinct messages.
- **Arc conversion must be orientation-aware.** Sketch arcs use a start→end CCW convention while OCCT edges carry their own orientation, and full circles must be distinguished from partial arcs. Mirror already has to swap endpoints after reflection for the same reason.
- **The input wire is built from the selected entity ids**, not from `Sketch::buildWires()`, which returns bare OCCT wires with no link back to sketch ids.
- **Commit adds ordinary sketch entities and nothing else** — no new modeling op, no `serializeParams`/`deserializeParams`.
- **Commit is a two-phase transaction**, because all-or-nothing does not follow from `recordSketchMutation`: it records before/after for undo but rolls back neither an exception nor a partial conversion, and `Sketch`'s mutators append directly. Phase 1 converts the entire OCCT result into an in-memory entity plan and can fail freely, having written nothing; phase 2 applies a complete valid plan and performs no geometry work that can fail. `Standard_Failure` becomes a structured error and a toast, never an escape into the frame loop.
- The feature lives in `SketchTool.{h,cpp}` with **`Application` as its only host**. `src/plugins/SketchPlugin.cpp` is reference code that is deliberately not wired to any command or input path, and needs no Offset implementation.

## Non-goals

- **Associativity.** One-shot; editing the source afterwards does not move the copy, matching Mirror.
- **Open chains.** OCCT wraps an open path in a closed racetrack outline rather than offsetting one side. Refused.
- **Splines.** A spline's offset is not a spline, so support would mean sampled segments and would break CAP-3.
- **Self-intersecting sources.** Refused by validation rather than handled.
- **Splitting results.** When an inward offset pinches a loop into two wires the operation is refused, not committed as two loops. Revisit only if users ask.
- **Multiple simultaneous loops**, one per invocation. **A 3D face-offset tool** — `ShellOp` covers that. **Join-style choice** — corners are filleted (`GeomAbs_Arc`), no miter option.

## Success signal

A user draws a rectangle, selects it, hits Offset, drags inward, types `3`, and immediately extrudes the ring between the two loops into a walled box — without hand-drawing the second rectangle or dimensioning a single segment to keep it parallel.

## Assumptions

- Offset distances are millimetres, matching every other typed sketch value.
- A single selected circle counts as a closed loop and offsets to a concentric circle.
- The existing `parseFinite` is the right validator for typed entry; Offset accepts an unsigned magnitude, with direction coming exclusively from the cursor.
- The tolerance values in `tolerances.md` are sensible defaults for a millimetre-scale CAD sketch. They are stated so they can be argued with, not because they were measured against user complaints.

## Open Questions

- Should Offset be reachable by keyboard shortcut? The project's own notes record that letter shortcuts are unreliable immediately after a mode change, so the toolbar button may be the only dependable entry point. Deliberately left open: this is polish, not implementation-blocking.
_(The plugin-architecture blocker that stood here through review rounds 2–5 is **resolved** — see below.)_

## Resolved by the maintainer

- **Plugin architecture — decided, no longer blocking.** `CONTRIBUTING.md` has been amended to carve out in-sketch tools: a new `SketchToolMode` lives in `src/modeling/SketchTool.{h,cpp}` with dispatch in `Application`, while modeling ops, importers, exporters and body-level tools keep the plugin rule unchanged. The carve-out states *why* rather than just granting an exemption — `InteractiveTool::handleInput()` is never called from `Application`, so a plugin-hosted sketch tool would receive no viewport input, which is exactly why `SketchPlugin.cpp`'s `SketchModeTool` sits unwired. Offset therefore follows the 14 existing sketch tools, and the spec and the contribution policy now agree.

  The alternative — building input routing so sketch tools can be true plugins — remains the right long-term move and is the "future input-routing migration" that `SketchPlugin.cpp` already names. It is a separate infrastructure project and is deliberately not gating this tool.

## Assumptions retired by review

- ~~The Line tool's typed-value popup is reusable unmodified.~~ Retired twice over. The popup the assumption first pointed at (`##SketchDim`) is in the unwired plugin; the live one (`##SketchDimInput`) funnels into `applyDimension`, which hard-requires `m_isPlacing` and ends `default: return false`. Offset never sets `m_isPlacing`, so a label case would have produced a field that silently does nothing. Offset gets a bespoke panel instead — see `state-machine.md`.
- ~~Post-commit selection is an open question.~~ Resolved: the **source** stays selected, so repeated offsets nest from one selection. Diverges from Mirror deliberately, and sidesteps `commitMirror`'s out-params leaving generated arcs and circles unselected.
- ~~Construction-flag behaviour is an open question.~~ Resolved: output is always real geometry and inherits nothing, because extruding it is the success signal's whole point.
