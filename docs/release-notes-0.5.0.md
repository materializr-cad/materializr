## Materializr 0.5.0 - loft, sketches as construction planes, snap widget

This release brings a working **Loft** op, a workflow where any sketch can
serve as a movable construction plane, a corner **snap-grid widget** that
persists its choice across launches, and the real M-cube app icon baked into
the AppImage.

### Highlights

- **Loft between two sketch profiles** - closed or open. A new toolbar entry
  in the Sketch and Region panels opens a live-previewed popup with
  **Solid / Shell**, **Smooth / Ruled**, and **Reverse profile B vertex
  order** toggles. Best results when both profiles sit on parallel planes
  with similar topology (the tooltip explains why orthogonal-plane lofts
  fan into a tent surface - that's ThruSections being honest, not a bug).
- **Sketches act as movable construction planes.** Select a sketch outside
  ortho / sketch-edit, click the new **Move** or **Rotate** under the
  Sketch panel's "Transform" header, and a gizmo appears at the sketch's
  centroid. Dragging rewrites the sketch's plane (the artwork rides along),
  with snap-to-grid honouring absolute world positions and rotate snapping
  to 15° detents when snap is on. A three-input **Move Sketch** popup
  offers exact X / Y / Z translations as an alternative to dragging.
- **Snap-grid corner widget**, next to the ViewCube. Small square showing
  the current step (0.1 / 0.5 / 1 / 10 mm) with a solid-blue border when
  snap is on. Left-click opens settings; right-click quick-toggles snap.
  The choice persists to `~/.config/materializr/settings.cfg` so it
  survives launches.
- **Open sketches are pickable in the viewport.** The picker now falls
  back to an edge-distance test when no closed region matches, and the
  body-vs-sketch occlusion threshold is loosened so a sketch lying on a
  body face never gets rejected as "behind it". Box-select also picks
  sketches.
- **Editable dimension constraints from the History panel.** Distance,
  Radius (shown as diameter), and Angle constraints get inline value
  fields; the snapshot re-solves immediately and Apply Changes propagates
  the new value through the rest of history.
- **Construction Plane popup.** Replaces the no-op New Construction Plane
  button with a live-previewed popup (XY / XZ / YZ + Parallel-to-face +
  offset). Plane visibility in the Items panel + viewport is still a TODO
  for 0.6 - for now the sketch-as-construction-plane workflow above is the
  recommended path.

### Polish

- Gizmo readouts label the dragged axis and match the Z-up convention
  (red = X, green = Z, blue = Y).
- Gizmo translate snap is absolute-position (lands on grid intersections)
  instead of delta-snap. Rotate is hard 15° when snap-on, free with a
  7° soft-snap near 45° when off.
- Gizmo dimension line draws from the world origin to the current pivot -
  easier to read "where is this now" than "how far did I drag".
- Gizmo hides while any interactive op (Push/Pull, Loft, Construction
  Plane, Pattern, Shell, Resize, …) is active.
- Toolbar tooltips wrap across multiple lines instead of running off one.
- Sketch-on-XY / XZ / YZ now land in visibly distinct camera views.
- Real **icon.png** baked into the AppImage at 256 / 512 hicolor sizes,
  resized by ImageMagick during the Docker build.

### Fixed

- **Vertical / open sketches were unpickable** in the viewport: the picker
  only tested closed regions, and a too-tight occlusion threshold rejected
  sketches lying on a body face. Both fixed.
- **Push/pull on a reloaded sketch around an existing hole** produced a
  solid bar instead of a tube. `Sketch::buildRegions` now grafts inner
  wires from the source face onto each sketch face whose outer wire fully
  contains them.

### Carried into 0.6.x

- Construction-plane visibility in the Items panel + viewport.
- Construction axes (and on top of them: Revolve, Threads).
- Push/Pull artifacting on multi-target preview.
- Partial mesh rebuild during push/pull.

### Install

Download `Materializr-x86_64.AppImage` below, make it executable, and run:

```
chmod +x Materializr-x86_64.AppImage
./Materializr-x86_64.AppImage
```

The binary is statically-linked against OpenCASCADE; no system deps to
install. Tested on Ubuntu 24.04 / GNOME 46.

### Full changelog

See [docs/changelog.md](docs/changelog.md) for the verbose breakdown.
