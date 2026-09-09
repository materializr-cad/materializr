# Sketch Dimension Tool - Design Spec

**Date:** 2026-07-19
**Status:** Approved (design), pending implementation
**Branch:** `feature/sketch-dimension-tool`

## Summary

An Onshape-style dimension tool for sketch mode, bound to the `d` key. The user
clicks sketch entities (points, lines, circles, arcs - regular or construction),
places a dimension label with a second click, and types a value. The tool
creates driving constraints: distances, lengths, diameters, and angles.

This replaces the current workflow where dimensioning existing geometry requires
the right-click "Add Constraint" menu, which offers no value input at creation
time and no label placement.

## Goals

- `d` activates a Dimension tool inside sketch mode; `Esc` returns to Select.
- Dimension existing geometry: line length, circle/arc diameter, point-to-point
  distance, point-to-line distance, parallel line-to-line distance, angle
  between non-parallel lines.
- Construction geometry dimensions identically to regular geometry.
- Onshape-style label placement: the label follows the cursor after entity
  selection; a click fixes its position (persisted); an inline value input
  opens prefilled with the measured value.
- Tool stays active after each dimension so several can be placed in a row.

## Non-Goals

- Driven/reference dimensions (read-only dims on over-constrained geometry).
- Dimensions on splines, polygons-as-units, or text glyph geometry.
- Circle-to-line or circle-to-circle distance dimensions.
- 3D (non-sketch) dimensioning - the existing `MeasureTool` covers measuring.

## UX Flow

### Activation

- `d` key in sketch mode → `SketchToolMode::Dimension`. Guards: not when a text
  field has focus (`io.WantTextInput`), not with Ctrl held (Ctrl+D = duplicate).
- A toolbar button in the sketch toolbar activates the same mode (touch users
  have no keyboard).
- `Esc` while in Dimension mode with no pending picks → back to Select mode.
  `Esc` with pending picks → clear picks, stay in Dimension mode.
- Overlay hint text (same pattern as other sketch tools) shows current state:
  "DIMENSION - click an entity" / "click second entity or place label" /
  "click to place label".

### Entity picking

Hover highlights the entity under the cursor (reuses Select-mode hit-testing
tolerances). Click sequence:

| First pick | Then | Result |
|---|---|---|
| circle or arc | - | diameter dim → placing state |
| line | click empty space / place label | line length (Distance between endpoints) |
| line | click second line, parallel within 1° | `DistancePointLine` (first line's start point ↔ second line) |
| line | click second line, non-parallel | `Angle` |
| line | click point | `DistancePointLine` (point ↔ line) |
| point | click point | `Distance` |
| point | click line | `DistancePointLine` (point ↔ line) |
| point | click empty space | no-op (a lone point has no dimension); pick stays pending |

Clicking a spline, text geometry (`fromText`), or empty space on first pick does
nothing.

### Label placement and value input

1. Once the entity set is complete, a ghost label showing the measured value
   follows the cursor, with a thin leader line from the geometry anchor.
2. Click fixes the label: the offset from the auto anchor (sketch-space vec2) is
   stored on the constraint and persisted.
3. An inline input opens at the label, prefilled with the measured value
   (diameter for circles/arcs, degrees for angles, mm otherwise), text
   pre-selected.
4. `Enter` → constraint takes the typed value, solver runs.
   `Esc` (or clicking away) → constraint keeps the measured value.
5. Either way the constraint is created as one undoable mutation, and the tool
   returns to its idle picking state.

Duplicate policy: creating a dimension on a pair that already carries the same
constraint type replaces the old value instead of stacking a duplicate
(matches the existing `applyDimension` dedup behavior).

## Architecture

### New constraint type

`ConstraintType::DistancePointLine` - **appended** to the enum (append-only
policy for serialization stability).

- `entityA` = point id, `entityB` = line id.
- `value` = perpendicular distance from the point to the **infinite** line.
- Residual in `SketchSolver.cpp`:
  `|cross2(dir, p − a)| / |dir| − value`, where `a` is the line's start point
  and `dir` its direction. Degenerate (zero-length) lines return residual 0,
  same guard style as existing constraints.
- Line-to-line distance is expressed as this constraint using the first line's
  start point, so no separate line-line type exists.

### Constraint label offset

`Constraint` gains:

```cpp
double labelOffX = 0.0;  // sketch-space offset of the dimension label
double labelOffY = 0.0;  // from its auto-computed anchor; 0,0 = auto placement
```

`(0,0)` means "legacy / auto placement" - existing constraints and old files
keep today's automatic label positioning.

### Tool state machine (`SketchTool`)

`SketchToolMode::Dimension` added to the mode enum. New state on `SketchTool`:

- `m_dimPickA` / `m_dimPickB` - picked entity refs (kind + id).
- `m_dimPhase` - `PickFirst | PickSecondOrPlace | PlaceLabel`.
- Pair-resolution helper returning the constraint type + entity ids for a
  completed pick set (unit-testable pure logic).

`SketchTool` owns picking and phase transitions. It does **not** create the
constraint itself - it exposes the resolved pending dimension; the application
layer commits it through the undo-recording path (same split as the existing
`applyDimension` / `recordMutation` pattern in `SketchPlugin`).

### Input routing and rendering (`Application_Viewport.cpp`)

- Routes sketch-space clicks/moves to the tool when Dimension mode is active
  (same plumbing as other sketch tool modes).
- Renders: hover highlight, ghost label + leader line during placement, inline
  value input (ImGui window at the label position, `EnterReturnsTrue`).
- Existing dimension-label pass extends to:
  - use `labelOff` when non-zero, falling back to today's auto offsets;
  - draw a thin leader line from the geometry anchor to the label when the
    label carries a stored offset;
  - render `DistancePointLine` labels (anchor: midpoint of the perpendicular
    foot segment).
- The existing click-to-edit popup (`##DimEdit`) works unchanged for the new
  type - it edits `c.value` generically.

### Key binding (`Application.cpp`)

In the sketch-mode key handling block: plain `d` (no Ctrl, `!io.WantTextInput`)
sets the sketch tool to Dimension mode. Sits beside the existing Ctrl+D
duplicate handler, which keeps priority via its Ctrl guard.

### Persistence (`ProjectIO.cpp`)

K-line gains two trailing fields:

```
K id type entityA entityB value valueY labelOffX labelOffY
```

- Writer always emits the two new fields.
- Reader reads them with a guarded extraction; missing fields (old files) →
  `0 0` (auto placement). Old readers ignore trailing tokens on the line, so
  files written by the new build still load in old builds (minus offsets).
- `DistancePointLine` round-trips as its enum int, consistent with the
  append-only policy already documented in the writer.

### Discoverability

- `Toolbar.cpp`: Dimension button in the sketch tool group.
- `HelpPanel.cpp`: shortcut entry for `d`.

## Touched Files

| File | Change |
|---|---|
| `src/modeling/SketchConstraints.h` | enum append, label offset fields |
| `src/modeling/SketchSolver.cpp` | `DistancePointLine` residual + satisfied check |
| `src/modeling/SketchTool.h/.cpp` | Dimension mode state machine |
| `src/app/Application.cpp` | `d` key binding |
| `src/app/Application_Viewport.cpp` | picking routing, ghost label, inline input, label rendering |
| `src/io/ProjectIO.cpp` | K-line extension, guarded read |
| `src/ui/Toolbar.cpp` | sketch toolbar button |
| `src/ui/HelpPanel.cpp` | shortcut documentation |
| `tests/` | new headless tests (below) |

## Error Handling

- Zero-length line in `DistancePointLine` residual → 0 (no NaN into solver).
- Deleting a referenced point/line: existing constraint-orphan cleanup path
  must also cover the new type (entityB is a line id - verify the cleanup
  checks line ids for it).
- Value input: parsed with the existing `parseFinite` guard; non-positive
  distance/diameter rejected (input stays open); angle clamped to (0°, 180°).
- Over-constraint: no special casing - solver's existing
  `FullyConstrained/OverConstrained` status badge reports it, matching current
  behavior for the right-click menu.

## Testing

Headless (ctest - run **unsandboxed**; sandboxed runs false-fail on /tmp writes):

1. **Solver:** `DistancePointLine` - free point converges to the target
   distance from a fixed line; degenerate line doesn't NaN.
2. **Pair resolution:** unit test the pure pick-resolution helper - all rows of
   the picking table above map to the right constraint type/entities,
   parallel-vs-angle threshold behaves at the 1° boundary.
3. **Persistence:** round-trip a sketch containing a `DistancePointLine`
   constraint with non-zero label offsets; load a legacy K-line (6 fields) and
   confirm offsets default to 0.
4. **Dedup:** dimensioning the same pair twice replaces the value, doesn't
   stack.

Manual (GUI): place each dimension kind, drag-place labels, type values, undo/
redo, save/reload, construction-line variant.
