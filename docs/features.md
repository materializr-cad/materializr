# Features

Full list of what's in the box, grouped by area.

For how to *use* these, see the
[wiki](https://github.com/materializr-cad/materializr/wiki) - Getting Started,
Keyboard Shortcuts and Settings live there.

## Modelling operations

- **Push / Pull** - unified extrude/cut. Select a body face → **Push/Pull** → an
  arrow points out along the face normal; drag it (or type) to add material
  (positive) or cut in (negative), with a live mm measurement. Also works on
  sketch regions. Unions auto-merge coplanar/cotangent faces so seams disappear.
- **Extrude** - interactive extrude with live preview, draft angle,
  boolean modes.
- **Revolve** - profile around an axis, 0–360°.
- **Loft** - through multiple cross-section profiles.
- **Fillet** - pick edge(s) → **Fillet** → drag the outward handle (or type) to
  set the radius, with live preview and a measurement readout.
- **Chamfer** - same flow as Fillet for a chamfer distance.
- **Shell** - hollow a solid with uniform wall thickness.
- **Offset Face** - push or pull individual faces.
- **Split X / Y / Z** - divide the selected body with a plane through its
  bounding-box centre, perpendicular to the chosen axis.
- **Boolean** - Union, Subtract, Intersect (Ctrl+click two bodies); unions
  are post-processed with `ShapeUpgrade_UnifySameDomain` so smooth merges
  don't leave a seam edge between the original bodies.
- **Move / Rotate / Scale** - interactive gizmos selected from the Transform group:
  arrows (move), rings (rotate, **soft-snaps to 45°**), cubes (scale, **per-axis
  or uniform**); each shows a live mm / degree / percent readout while dragging.
  Scale also has a side panel for exact X/Y/Z percentages. Move applies to every
  body in the selection at once.
- **Mirror** - one **Mirror** button → popup to mirror across **X / Y / Z** or
  **a face you then click**.
- **Linear Pattern** - repeat along a direction.
- **Radial Pattern** - repeat around an axis.
- **Copy / Duplicate** - clone selected bodies (drops at the same position;
  drag the new copy with the Move gizmo to place it).
- **Delete** - remove bodies (undoable, including from the Items panel).
- **Align** - point-to-point snap.
- **Construction Planes** - custom reference planes for sketching.
- **Threads** - cut a real helical thread into a cylindrical face, external on
  a boss or internal in a hole. Five cross-sections (Standard V, Trapezoidal /
  ACME, Square, Buttress, Rounded), a fit clearance for printed threads that
  have to actually assemble, multi-start for bottle-cap style quarter-turn
  closures, and an explicit groove width so a coarse pitch can carry a narrow
  groove (a wire seat or grip spiral rather than a fastener). Threads reflow:
  later cuts reorder beneath the thread and it re-cuts in the background.
- **Boundary Fill** - build a solid from silhouette sketches on different
  planes; each is extruded through the others and the result is their
  intersection. The way to reconstruct a shape from traced reference photos.
- **Projection (engrave / emboss)** - project a sketch onto a face along its
  normal and cut in or raise out to a depth, so a logo or text wraps onto a
  cylinder.
- **Repair Geometry** - delete picked faces and heal the surrounding ones back
  together: take a baked fillet back to a sharp edge so it can be re-applied,
  or clean a round off an imported part.
- **Separate** - split a body that holds several disconnected solids into
  individual bodies.
- **Unfold / Flatten** - lay a body (or picked faces) flat into a 2D pattern
  with cut and fold lines, for a laser cutter, CNC, or a printed template.
- **Reference images** - import a photo onto a construction plane, calibrate
  its scale against a ruler in the shot, and sketch over it.

## 2D Sketching

- **Tools**: Line, Circle, Rectangle, Arc, Spline, Polygon, Text, Trim.
- **Select / Move tool** - pick existing points and lines (Ctrl+click adds);
  drag a selected element to translate the whole selection. Double-click in
  empty space selects every element in the sketch.
- **Sketch transforms** - Copy / Mirror / Rotate buttons act on the current
  selection (or the whole sketch if nothing is selected). Copy and Mirror
  create duplicates that are auto-selected so you can drag them into place;
  Rotate enters an interactive drag-to-rotate mode around the centroid.
- **Sketch from scratch on a base plane** - with nothing selected, **Sketch on
  XY / XZ / YZ** starts a sketch on that base plane with the matching standard
  view (Top / Front / Right); no body required.
- **Sketch on any planar face** - toolbar button when a face is selected
  or right-click → *Sketch on this Face*; the sketch's plane is taken
  directly from the face.
- **Persistent sketches** - finishing a sketch saves it into the document's
  Items panel; it doesn't auto-extrude. Select it (or a region of it) later to
  **Edit Sketch** (continue drawing) or **Push/Pull** an enclosed region.
- **General region detection** - the sketch's geometry is partitioned into
  atomic planar regions using the boolean engine, so *intersecting and
  overlapping shapes* (the lens between two circles, an annulus, the cells
  on either side of an open dividing line) each become individually
  selectable - not just simple closed loops.
- **Region hover & multi-select** - hover a region in the viewport to
  highlight it; click to select, Ctrl+click to add more regions to a
  push/pull operation. A wider catch area around the boundary lines makes
  thin regions easy to grab.
- **Manifold extrusion** - outer wire + inner wires get assembled into a
  single planar face with holes (e.g. a sketched ring extrudes into a tube).
- **Inline dimension input** - while placing a Line / Circle / Polygon /
  Rectangle, type the length, radius, or side directly; the value is
  applied from the click anchor toward the cursor direction.
- **Live dimension overlay** - a measuring annotation (offset dimension line
  with arrowheads + value) follows the operation as you work: line length,
  circle **diameter**, both rectangle sides while sketching; depth while
  extruding; distance while moving a body with the gizmo.
- **Snap** - to existing sketch points, to circle/arc perimeters, and
  threshold-based to grid lines (snaps only when close).
- **Adjustable grid step** - pick 0.1, 0.5, 1, or 10 mm; the same step
  drives the visual grid, sketch snap, and gizmo translate snap. Every 10th
  line is brighter; every 100th is brighter still so large distances are easy
  to read.
- **Infinite plane-aligned grid** - sketching on any plane (or any face) shows
  the world grid laid on that plane, so neighbouring faces and reference
  geometry stay reachable.
- **Concentric / shared anchors** - clicking on an existing point in
  Circle / Polygon / Arc mode reuses that point so concentric shapes
  actually share a center vertex.
- **Constraints**: Coincident, Horizontal, Vertical, Distance, Radius,
  Parallel, Perpendicular, Tangent, Equal, Concentric.
- **Auto-constraining** - snaps to horizontal/vertical when lines are
  near-aligned.

## Camera / View

- **Auto-orthographic on sketch entry** - when you start a sketch, the
  camera snaps to an orthographic view looking straight down the sketch's
  plane normal, framed to the source face.
- **Pan and zoom preserve ortho**; **orbit** exits ortho back to perspective.
- **"Look at Sketch"** button - appears in the sketch toolbar only when
  the camera isn't ortho, returns to the aligned ortho view.
- **3D Navigation Cube** - projected cube in the top-right corner that
  rotates with the view. Click a face for an orthographic snap, click a
  corner dot for an isometric snap, click a side arrow for a 90° rotation,
  or drag the cube body to free-orbit. The drag direction can be inverted in
  Settings.
- **Level (turntable) orbit** - the default mouse orbit keeps the horizon
  flat; the free trackball is available as a toggle in Settings.

## Selection System

| Action | Selects |
|--------|---------|
| Click near edge | Edge (for Fillet/Chamfer) |
| Click on face | Face (for Extrude / Push&nbsp;Pull / Sketch on Face) |
| Click on sketch region | SketchRegion (for Push/Pull) |
| Double-click | Whole body (shows transform gizmo) |
| Ctrl+click | Add to multi-selection (faces, regions, edges, bodies) |
| Drag in empty space | Box-select bodies whose screen bbox falls inside |
| Right-click face | Context menu (Sketch on Face, Extrude, Select Body) |
| Click empty space | Clear selection |

Selection is **occlusion-aware**: edges and sketch regions hidden behind a
solid can't be picked through it - only what's visible is selectable.

## Interactive Tools

All major operations provide **live preview** and an on-screen **measurement**
as you drag or type:

- **Push/Pull** - face-normal arrow you drag (or type a distance); mm readout
  follows it. Starts at 0 (no change). Also works on selected sketch regions.
- **Extrude** - drag in viewport or type distance, slider, Enter to confirm.
- **Fillet / Chamfer** - outward drag handle on the edge sets the radius
  /distance (drag away from the edge to grow, from 0.1 mm), with a measurement
  and live preview; or type the value. Starts at 0.
- **Move (Gizmo)** - 3-axis arrows with threshold-based grid snap; mm readout.
  Translates every selected body together.
- **Rotate (Gizmo)** - rings; free rotation that soft-snaps to 45°, degree
  readout.
- **Scale (Gizmo)** - per-axis cubes-on-bars (or uniform), 1% snap, percent
  readout, plus a side panel for exact X/Y/Z percentages.
- **Cancel mid-drag with Escape** - pressing Escape while still dragging
  a gizmo, an extrude, a push/pull, a fillet/chamfer, or a sketch tool
  reverts the body / cancels the preview so the model returns to exactly
  where it started.

## Rendering

- PBR Cook-Torrance BRDF shading with tone mapping.
- **Per-body colour** - bodies default to a uniform light grey; each row in the
  Items panel has a colour swatch that opens a hue-wheel picker.
- **Smoother curved faces** - fillets, holes and cylinders use a tighter
  angular-deflection tessellation so rounded geometry reads cleanly without
  visibly faceting.
- 12 material presets (Steel, Aluminum, Copper, Gold, Plastics, Wood,
  Glass, Rubber, Ceramic, Concrete).
- Gradient background.
- Edge wireframe overlay.
- Per-face / per-edge / per-body / per-region selection highlighting
  (outline only, no z-fighting).
- Infinite grid (renders on the active sketch plane; emphasized 10th and
  100th lines).
- Construction plane visualisation.

## UI / UX

- **Adaptive toolbar** - shows relevant tools based on selection
  (nothing / edge / face / body / sketch / sketch region).
- **Design History** - every operation recorded, undo/redo (Ctrl+Z / Y),
  breakpoints, deletion (with conflict detection). Reloaded projects show
  steps marked `(reloaded)` - undo/redo replays the stored geometry but
  re-editing their parameters isn't supported.
- **Items panel** - bodies *and* sketches with visibility, rename (double-click
  or right-click), delete (Delete key or right-click), and a per-body **colour
  swatch** (right edge) that opens a hue-wheel picker. Body deletes are undoable.
- **Interactions panel** - a live reference of the viewport controls
  (camera / select / transform / sketch), docked above the Items panel; the
  camera rows reflect the current mouse bindings.
- **Properties panel** - edit any operation's parameters after creation.
  Clicking a fillet or chamfer face in the viewport jumps straight to its
  history step.
- **Material panel** - assign PBR materials to bodies.
- **Measure tool** - distance, area, edge length, bounding box.
- **Section View** - cut through solids with interactive clipping plane.
- **Transform Gizmo** - 3-axis arrows + rotation rings + scale cubes.
- **Box / Marquee selection**.
- **Dark / Light themes**.
- **Status bar** - body count, selection info, current tool.
- **Variables & Expressions** - named parameters (e.g. `width=50`,
  `height=width*0.6`).
- **Version snapshots** - auto-save + manual save with labels, restore
  any version.
- **Settings** (File → Settings) - theme, mouse bindings (orbit/pan
  buttons or trackpad mode), level vs free orbit, ViewCube drag invert,
  autosave on/off + interval. Preferences persist across launches in a
  forward-compatible config file.
- **Help menu** - User Guide, Keyboard Shortcuts, Check for Updates (queries
  GitHub for the latest release), About dialog with credits.
- **Toast notifications**.
- **Tabbed projects** - several projects open at once, each with its own
  history, camera and crash-recovery snapshot. Ctrl+Tab cycles them. The tab
  strip is drawn per layout (classic: inside the viewport; modern: pills in
  the top bar; im-touch: a chip that opens a sheet).
- **Home screen** - a grid of recent projects with thumbnails baked into the
  save files, plus New Project. Reachable any time from File → Home Screen,
  and it no longer closes what you're working on: opening something from
  there lands in a new tab.
- **Reopen last session** - optionally brings back every tab you had open
  when you quit (Settings → General).
- **Crash recovery** - per-tab snapshots; a crash with several projects open
  offers them all back at once, one tab each.
- **Cross-project parts** - copy bodies between projects: `File → Import →
  From Project…`, or right-click a body and send it to a new tab.

## File I/O

| Format | Import | Export |
|--------|:------:|:------:|
| Native `.mzr` / `.materializr` project (bodies + colours + sketches + history + thumbnail) | yes | yes |
| STEP (.step / .stp) | yes | yes |
| IGES (.iges / .igs) | yes | yes |
| BREP (.brep) | yes | yes |
| STL (.stl) | yes | yes |
| 3MF (.3mf) | - | yes |
| OBJ (.obj) | - | yes |
| glTF / GLB (.glb / .gltf) | - | yes |
| SVG (.svg) | yes | per-sketch |
| DXF (.dxf) | yes | per-sketch |

`File → Export` writes every **visible** body to one file, with the parts in
their real positions - that's what a multi-body or print-in-place model needs.
To export a subset, select the bodies, right-click one, and use the **Export**
submenu; it takes the whole selection. Hiding a body also excludes it from a
File → Export.
