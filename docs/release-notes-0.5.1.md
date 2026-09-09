## Materializr 0.5.1 - Smaller files, editable constraints across sessions

Two big upgrades and a pile of polish:

### File format v3 - projects now ~5× smaller

Switched the on-disk geometry to **binary BREP** (OCCT's `BinTools`, no
display triangulation) inside a **gzip-max** wrapper, with a length-prefixed
`PARAMS_LEN` block so op-parameter blobs can carry arbitrary content. A real
project that was 16 MB on disk now lands at **~3 MB**. v2 readers still load
the old text format; new saves write v3 automatically.

### Cross-session constraint editing

Two new paths for tuning constraint values, both surviving save / load:

1. **Live Constraints panel.** Select a sketch (or any region of one)
   outside sketch-edit mode and the Properties panel lists every constraint
   on it. Distance, Radius (shown as Ø diameter), and Angle constraints get
   inline editors that commit on Enter or focus-out, run the solver, and
   push a `SketchEditOp` onto history. Non-dimensional constraints
   (Horizontal, Parallel, …) appear as muted bullet rows so you can see
   what's applied.

2. **History → Properties** for sketch-edit steps now survives reload. The
   project file carries both before+after sketch snapshots for each step,
   and the loader rehydrates them as real `SketchEditOp`s (instead of
   parameterless `ReplayOp`s). Click a sketch-edit step from a year-old
   project, see its constraint values, edit them.

### Fixed

- **Radius constraint actually applies its value.** The solver had a
  no-op branch labelled "informational only"; it now writes the value back
  into the circle/arc's radius field via new `Sketch::setCircleRadius` /
  `setArcRadius` mutators. So a Radius constraint set to 30 mm produces a
  30 mm circle.
- **Taskbar icon resolves correctly under Dash-to-Panel / GNOME shell.**
  GLFW window now sets `WM_CLASS = "Materializr"` (X11) and the matching
  Wayland app-id; the bundled `.desktop` carries `StartupWMClass=
  Materializr` so the running window matches its launcher icon.

### Known limitations

- **No downstream cascade yet.** Editing a sketch constraint resizes the
  sketch correctly, but an Extrude / Push-Pull / Fillet that consumed that
  sketch (or its resulting body) doesn't auto-re-execute. You'll need to
  redo the op manually. Cascade is on the roadmap; the toponaming work
  needed to safely re-run fillets on a re-extruded body is being treated
  as its own future release.

### Install

Download `Materializr-x86_64.AppImage` below, make it executable, run:

```
chmod +x Materializr-x86_64.AppImage
./Materializr-x86_64.AppImage
```

Statically links OpenCASCADE. Tested on Ubuntu 24.04 / GNOME 46.

### Full changelog

See [docs/changelog.md](docs/changelog.md).
