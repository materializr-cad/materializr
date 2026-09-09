# Materializr 0.5.2

Polish + correctness pass on the v0.5.1 release. The headline fix is that
sketch-region push/pull no longer accumulates ghost prisms on the vertical
wall during a slider drag; the rest is dimension-readout accuracy, viewport
navigation upgrades, and a unified snap control.

## Highlights

### Sketch push/pull doesn't band anymore

Free-floating sketch push/pull (sketch on a construction plane, no parent body)
used to stack every preview prism on top of the previous one during a drag,
producing a striped vertical wall that only cleared on commit. Fixed by routing
`Document::removeBody` through a new `BodyRemovedEvent` that drops the
renderer's mesh + edge slots immediately, instead of waiting for a full
rebuild that never came mid-drag.

Same change also fixed a per-frame leak that was marking every invisible body
dirty 60 times per second on a 100-body project - push/pull preview now stays
smooth.

### Body dimension editor

Selecting a single body shows an editable **X / Y / Z** dimension block in the
Properties panel using the user-Z-up convention (X / Y the floor, Z up). Type
a new extent value to scale that axis, anchored at the body's bbox-min corner
so growth happens along +axis only - predictable, no centre drift.

Body bbox readouts now use OCCT's `BRepBndLib::AddOptimal` (analytic bounds,
no tessellation + tolerance padding) so a Ø80 mm cylinder reads exactly
`80.000 × 80.000 × 20.000` instead of `80.007 × 80.005 × 20.010`. Same fix for
the Measure tool's bbox readout.

### Cursor-aware mouse wheel + `F` to frame

- **Scroll dollies toward whatever the cursor's over.** Hover a small
  off-origin part, scroll in, you land on it. No more pan-zoom-tilt dance.
  Falls back to a target-plane projection over empty space so empty scrolling
  matches the old behaviour.
- **`F` key fits to selection** (or all visible bodies if none selected).
  Suppressed in sketch-edit and while typing in a field.

### Arc sweep readout + 15° snap

While placing the third click on the Arc tool:

- Live sweep angle readout in degrees, pinned 14 px right of the cursor.
- Common fractions tagged automatically - `90.0° (¼)`, `180.0° (½)`,
  `270.0° (¾)`, `360.0° (full)` - within 1.5° of canonical.
- **15° snap**: when the natural sweep is within ±5° of a 15° multiple, the
  cursor jumps to the exact apex via `d = (L/2)·tan(θ/4)`, keeping you on the
  same side of the chord so the arc doesn't flip. Defers to the global snap
  toggle.

### Angle constraint arc

Dimension labels for the `Angle` sketch constraint now render an actual arc at
the lines' intersection vertex, SolidWorks-style, with the °-label hugging the
outside of the arc midpoint. Auto-detects the vertex from the closest endpoint
pair, so it handles loose Coincident pairs too.

### Bold operation arrows

Push/Pull, Extrude, Fillet, Chamfer drag arrows are now amber
(`255, 200, 60`) with a black halo and ~2× the arrowhead size. Sketch
inferences and the move-gizmo translate readout keep the lighter style - they
need to coexist with selection highlights.

### JetBrains Mono UI font

ImGui's default ProggyClean has the classic 0/8/B confusion problem in dim
readouts. The AppImage now bundles JetBrains Mono Regular (~270 KB) at
`share/materializr/fonts/`, loaded at ImGui init. Slashed zero, distinct 8/B/6.

### Unified snap control

Snap on/off + step (`0.1 / 0.5 / 1 / 10 mm`) is now exclusively the corner
widget next to the ViewCube. Removed the duplicate checkboxes from the
Settings dialog and from both Toolbar groups. The threshold slider remains in
Settings as a separate parameter.

## Smaller fixes

- Move-gizmo readout reports the body's **bottom** along the up axis (Z) rather
  than its centre. A cylinder sitting on Z=0 reads `Z 0.00`; drag up 10 mm
  reads `Z 10.00`.
- Snap-widget no longer eats clicks meant for the picker and no longer
  pollutes the parent layout cursor (was triggering ImGui's
  boundary-extension assert and rendering subsequent items under the button).

## Known limitations carried forward

- Sketch-as-construction-plane: sketches on body faces still don't ride along
  when their host body is moved with the gizmo (task #97).
- Push/Pull on complex projects still re-tessellates all touched bodies per
  preview frame; the partial-rebuild path exists but doesn't cover all op
  paths (task #99).

## Upgrading

`.materializr` v3 files from 0.5.1 load unchanged. Drop the new AppImage in
place of the old one (Gear Lever / autostart / desktop).
