# Offset input protocol and state machine

Companion to `SPEC.md`. Added after round 1 of adversarial review, which correctly found that "select, then drag" named no transitions a test could drive: `SketchTool` has no begin-drag/end-drag pair, `onMouseDown` has no Offset dispatch, and `onMouseUp` is generic selection-drag cleanup.

## State

```
Idle ──(toolbar Offset, valid selection)──> Armed ──(mouse-down)──> Dragging
  ^                                            │                        │
  │                                            │ (mouse-up)             │ (mouse-up)
  └────────(cancelOffset from any state)───────┴────> Committed ────────┘
```

`m_offsetActive` is true in **Armed** and **Dragging** only.

| From | Trigger | To | Effect |
|---|---|---|---|
| Idle | `ToolAction::SketchOffset` with a valid loop | Armed | capture source ids + source-scoped hash; `setMode(Offset)`; **restore the captured selection**; `d = 0`, direction = outward |
| Idle | `ToolAction::SketchOffset` with an invalid loop | Idle | toast the specific reason; mode unchanged |
| Armed | `onMouseMove` | Armed | update `d` and side from the cursor; refresh ghost |
| Armed | `onMouseDown` | Dragging | latch the drag |
| Dragging | `onMouseMove` | Dragging | update `d` and side; refresh ghost |
| Dragging | `onMouseUp` | Committed → Armed | commit at current `d`; source stays selected, so the tool re-arms for a nested offset |
| Armed/Dragging | typed value + Enter | Committed → Armed | commit at the typed magnitude; direction still from last cursor side |
| Armed/Dragging | Escape | Idle | `cancelOffset()`; mode → Select; **sketch stays open** |
| Armed/Dragging | tool switch (`setMode`) | Idle | `cancelOffset()` |
| Armed/Dragging | sketch exit, popup close, failed commit | Idle | `cancelOffset()` |
| Armed/Dragging | source geometry hash changed | Idle | `cancelOffset()` + toast; see below |

The tool re-arms rather than returning to Idle after a commit, which is what makes repeated offsets nest from a single selection. That is also why the **source** stays selected — see the resolved open question in `SPEC.md`.

**Keeping it selected takes explicit work.** `setMode` runs `clearElementSelection()` for every mode except `Select` (`SketchTool.cpp:37`), so the natural `capture ids; setMode(Offset)` sequence leaves the source unselected and the promise unkept. `beginOffset` must restore the captured selection *after* `setMode`, and the re-arm path after a commit must preserve it rather than re-deriving it.

## Direction

Ambiguous for concave loops and undefined at a circle's centre, so it is not "the side the cursor is on" naively.

Compute a signed distance from the cursor to the **canonically oriented** source wire (positive signed area / CCW; see `tolerances.md`), taking the nearest point on the wire and the sign from its outward normal there. After canonicalisation a positive `Perform` argument is outward. Ties — equidistant nearest points, most obviously the cursor at a circle's centre — resolve **outward**, deterministically.

Magnitude is `|signed distance|`, **refused** below `kMinOffsetDistance`, not clamped to it. Below the floor no ghost is drawn and no commit is possible; a mouse-up there surfaces the small-distance error. An earlier draft said "clamped", contradicting the refusal rule in `SPEC.md` — refusal wins.

Typed entry supplies magnitude only; the sign is whatever the cursor last indicated. This is why the assumption in `SPEC.md` says Offset accepts an unsigned magnitude.

Direction initialises to **outward** at Arm, so a user who opens the panel, types a value and presses Enter without moving the mouse gets a deterministic result rather than one depending on stale cursor state.

## Escape needs its own branch

`Application.cpp`'s sketch-mode Escape handler cancels only when the mode is `Dimension` or `isPlacing()` is true, and otherwise calls `exitSketchMode()`. `isPlacing()` is driven by `m_isPlacing`, which Offset never sets. **Adding `m_offsetActive` alone does not make Escape work** — it makes Escape close the whole sketch. The branch must be added explicitly, ordered alongside the existing Dimension branch, and must leave the sketch open.

## Stale source ids

Source ids are captured at Arm, as Mirror does. Unlike Mirror, the window here is long enough to matter: the user can undo, delete, or edit while the ghost is live, after which the captured ids point at absent or different geometry.

Rather than blocking undo/delete (invasive, and surprising), snapshot a hash at Arm and compare it before every preview refresh and before commit; any mismatch cancels with a toast.

**It must be a source-scoped hash, not `Sketch::geometryHash()`.** Review round 2 caught this: `geometryHash()` is FNV-1a over every point, line, circle and arc in the sketch, so committing offset output changes it — and because the machine re-arms after a commit, the very next preview would detect the tool's *own output* as a stale-source edit and cancel itself. Hash only the captured source entities, which is unaffected by geometry added elsewhere.

The hash must cover **every geometry-defining field**, not just endpoints: entity type, entity id, every referenced point id *and* that point's position, plus all scalar fields. Circle and arc centres matter specifically — `SketchCircle` is `{centerPointId, radius}` and `SketchArc` is `{centerPointId, startPointId, endPointId, radius}` (`Sketch.h:43-58`), so a hash of "endpoints and radii" would miss a dragged circle centre entirely and let the tool commit against geometry that had moved under it.

## Typed entry wiring

Offset gets a **bespoke panel in `Application_Dialogs.cpp`**, modelled on Mirror's floating window: a distance field, a Commit button, a Cancel button, and a close box.

It deliberately does **not** reuse the `##SketchDimInput` popup. Review round 2 established why: both of that popup's branches funnel into `SketchTool::applyDimension`, which opens `if (!m_sketch || !m_isPlacing || value <= 0.0f) return false;` and whose mode switch ends `default: return false`. Offset never sets `m_isPlacing`, so adding a label case would have produced a text field that silently does nothing. Extending `applyDimension` to special-case a non-placing mode would bend a function whose whole contract is mid-placement dimensioning.

The bespoke panel also settles cancellation cleanly, which the popup could not: `##SketchDimInput` passes `nullptr` as its `p_open`, so it has no close control for "popup close cancels Offset" to refer to.

Parsing reuses `parseFinite`, then applies the finiteness check and `kMinOffsetDistance` floor from `tolerances.md` before OCCT sees the value.
