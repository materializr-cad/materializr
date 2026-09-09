# Materializr 0.6.0

Construction planes get their own end-to-end workflow this release - create,
position, rotate, select, sketch-on, all without faking it through sketches.
Plus a bunch of polish on the dim-editor / push-pull / snap UI.

## Headline: Construction Planes are real

The Construction Plane popup has been around since 0.5.0, but until now planes
weren't visible, selectable, or manipulatable after commit. This release fixes
all of that:

### Create

Same popup as before - XY / XZ / YZ radios (now in Z-up convention so "XY"
is the floor) or Parallel-to-Face, plus an offset slider. The preview pushes
to history and **auto-selects** so the gizmo lands on it immediately.

The popup also has a new typeable `Rotate by N° around X/Y/Z` field for
dialling in exact angles that the gizmo snap won't land on.

### Visualise

Planes render as a translucent blue 100 × 100 mm quad with a darker
border. Selecting a plane swaps the fill + border to a warm amber so you
can see which one is "live" without the gizmo on top.

While you're sketching in ortho, planes hide entirely - clean drawing
canvas. Orbit out of ortho and they're back.

### Manipulate

Click a committed plane → highlight only (Tools panel shows
**Sketch on this Plane**, **Move**, **Rotate**). Click Move or Rotate
(or press W / E) to arm the gizmo for that plane.

- **Move** drags the plane along world axes. A cursor-pinned readout
  shows `Δ N.NN mm | Origin M.MM mm` - left is this drag's offset along
  the plane's own normal, right is the absolute distance from world
  origin along the same normal. Both snap to the grid step.
- **Rotate** spins the plane around its origin. Snap is 5° (hard) with
  snap-on, soft 15° otherwise - much finer than the 15° / soft-45° body
  default. The cursor pill shows `N.N° about X/Y/Z` and the popup's
  offset slider tracks the gizmo so the value reflects the live state,
  not just the slider history.

### Sketch on it

The Tools panel's **Sketch on this Plane** routes through the same
enter-sketch path the XY/XZ/YZ start-sketch buttons use, just with the
plane's stored `gp_Pln` as the host. The plane vanishes during sketch
editing (ortho only) so the canvas stays clean.

## Body dimension editor moved into the Scale popup

The Properties panel's per-axis X/Y/Z dim editor was functionally a glorified
scale, so it's been folded into the Scale gizmo popup. The popup now has a
**% / mm** toggle:

- **%** - the existing percent-of-current behaviour, multi-body safe.
- **mm** - single-body only. Fields pre-fill with the body's live bbox
  extents in Z-up convention; typing applies a per-axis scale anchored
  at the body's bbox-min corner so growth happens along +axis only.

`Uniform` works in both modes (mirrors percent in %, mirrors the ratio
in mm). The Properties panel still shows the bbox as a read-only
`Size: 80.00 × 80.00 × 20.00 mm`.

## Plugin render passes

The plugin system now invokes registered `RenderPassContribution`
entries each frame - `ConstructionPlanePlugin` uses this to own its
`PlaneRenderer` end-to-end (no more host coupling). New plugins can
register custom GL passes (initialize-once + per-frame render) without
touching `Application`.

`PluginContext::isInSketchMode()` is the first new accessor on the
plugin side, letting plugins suppress visuals during sketch-edit.

## Smaller fixes / polish

- **Push/Pull snaps to the grid step.** Whatever the corner widget
  says (0.1 / 0.5 / 1 / 10 mm) - drag, slider, typed value all snap.
- **Fillet / Chamfer readout pinned to cursor**, matching the arc-angle
  preview's UX so your eyes stay on what you're dragging.
- **Snap on/off + step is exclusively the corner widget.** Removed
  duplicates from Settings and both Toolbar groups; the threshold
  slider stays in Settings.
- **Arc angle preview** rounds to the same value the cursor will actually
  land on.
- **Plane gizmo drag bug** - was pushing a phantom `TransformOp` with
  `bodyId = -1` for plane-only drags, crashing on the next launch with
  `Body not found: -1`. Plane drags now write through `Document::setPlane`
  directly and the body/sketch commit branches are skipped. Plus a
  defensive `Document::putBody` reject for `id < 0` and a try-catch in
  the full mesh-rebuild loop so a corrupt project still loads.

## Known limitations carried forward

- Construction planes don't persist explicitly yet (they're rebuilt by
  the `ConstructionPlaneOp` history step on reload, which works in
  practice but ties their lifecycle to the history). Items panel listing
  still TODO.
- The construction-plane quad's hide rule is **inverted** from intent:
  it disappears the moment you leave ortho while still in sketch-edit
  (orbit, perspective, etc.), and reappears when you snap back to ortho
  via `View Sketch`. Exiting sketch entirely or saving also restores it
  normally. Cosmetic only - clicking through doesn't block any
  workflow; will be flipped in a follow-up.

## Upgrading

`.materializr` v3 files from 0.5.x load unchanged. Drop the new AppImage
in place of the old one.
