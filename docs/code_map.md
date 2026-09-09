# Project Navigation & Code Map

A reference for **where functionality lives** in the Materializr codebase, to
speed up debugging and navigation. It is organised top-down: a high-level
architecture overview and the cross-cutting patterns first, then a
directory-by-directory file reference, then a "where do I look if…" index.

> Roughly 105k lines of C++17 across ~320 files in `src/`. The geometry kernel is
> [OpenCASCADE](https://dev.opencascade.org/) (OCCT); the UI is Dear ImGui on an
> SDL2 + OpenGL (Core on desktop, ES 3.0 on Android) backend.

---

## Architecture at a glance

The code is layered. Higher layers depend on lower ones, not the reverse:

```
 Entry / platform   src/main.cpp · src/android_main.cpp · src/*.{cpp,h}
        │
 Application host    src/app/        the run loop, input, panels wiring,
        │                            interactive-op orchestration
        │
 Plugins             src/plugins/    each feature self-registers its toolbar
        │                            button / menu item / IO format
        │
 Modeling + Core     src/modeling/   Operation subclasses (the actual geometry)
        │            src/core/       Document, History, Selection, events
        │
 View + IO           src/viewport/   renderers, picking, camera, gizmo
                     src/io/         load/save, STEP/STL/IGES/glTF/SVG/DXF/OBJ/3MF/BREP
```

**Data flow for a typical edit:** a plugin's toolbar action (or an interactive
controller in `src/app/`) builds an `Operation`, hands it to `History`, which
calls `Operation::execute(Document&)`. The op mutates bodies in the `Document`,
which marks meshes dirty; the next frame the `src/viewport/` renderers rebuild
from the document. Undo/redo walks the `History` stack calling `undo()`/
`execute()`. Saving serialises the op stack (`src/io/ProjectIO.cpp`); loading
rebuilds it (see **Reload & rehydrate** below).

**Entry points.** `src/main.cpp` is the desktop entry (CLI parsing, a Linux
backtrace handler, then `Application::run()`). `src/android_main.cpp` is the
Android/SDL entry - SDL owns the real `main`, so this is renamed to `SDL_main`
and mirrors desktop minus CLI args.

---

## Cross-cutting concepts

These patterns recur everywhere; understanding them once explains most files.

### The Operation model (`src/core/Operation.h`)
Every modeling action is an `Operation` subclass. The key virtuals:
- `execute(Document&)` / `undo(Document&)` - do/undo the geometry change.
- `name()` / `description()` / `typeId()` - display + a stable serialization id.
- `renderProperties()` - the op draws its own editor in the Properties panel.
- `captureDiff()` - reports body changes *non-destructively* (read from stored
  undo data) so history can be serialised without re-running geometry.
- `serializeParams()` / `deserializeParams()` - round-trip the op's input
  parameters (radii, distances, axes) as an opaque one-line blob.
- `rehydrateFromReload()` - restore a deserialised op to its post-execution
  state so it stays **parameter-editable across sessions**. Returns `false` by
  default; ops that reference raw sub-shapes (fillet edges, face pulls) leave it
  unimplemented because they need topological naming first - those fall back to
  a baked `ReplayOp`.
- `plannedBodyIds()` / `cloneForBody()` - used by the **threads-last** reflow
  (an op touching a threaded body is re-ordered before the trailing Thread step
  so its boolean runs on clean geometry).
- `setProgressReporter()` / `reportProgress()` - long ops (thread cutting, dense
  projection) pump the event loop and allow cancel.
- `lastGoodParams()` / `rememberGoodParams()` - lets a rejected edit roll back
  to the last values that executed cleanly.

### History & reload (`src/core/History.*`, `src/modeling/OperationFactory.*`, `ReplayOp.*`)
`History` owns the op stack and the current index. On project load, each saved
step is rebuilt via `OperationFactory::create(typeId)` → `deserializeParams` →
`rehydrateFromReload`; if an op type opts out, the loader bakes a `ReplayOp`
(replays stored geometry for undo/redo but isn't re-editable). `History` also
enforces threads-last and exposes `dropRedoTail()`.

### Plugin system (`src/plugin/`)
Features register themselves rather than being wired into the host. A plugin is
a `REGISTER_PLUGIN("Name", initFn)` macro (`PluginMacro.h`) that auto-registers
at static-init; `ForceLink.cpp` calls each plugin's force-link symbol so the
linker doesn't strip the translation unit. In `initFn`, a plugin receives a
`PluginContext` (`PluginContext.h`) - its window into `Document`, `History`,
`SelectionManager`, `EventBus`, `Camera` - and registers **contributions**
(`Contributions.h`): toolbar buttons, commands, menu items, IO formats, render
passes, property sections, overlays. Each contribution carries a
`SelectionContext` so buttons show/hide based on what's selected. Ops that need
viewport+popup plumbing call `ctx.requestInteractiveOp("name")`; the host picks
it up next frame (see interactive controllers).

> **`src/plugin/` (singular)** = the plugin *infrastructure*.
> **`src/plugins/` (plural)** = the ~33 actual feature plugins.

### EventBus (`src/core/EventBus.h`, `src/core/Events.h`)
A typed publish/subscribe bus decouples producers from the host. Events include
`SelectionChangedEvent`, `DocumentModifiedEvent`, `SketchEditedEvent` (drives
the downstream re-execute cascade), `BodyRemovedEvent` (renderer drops the mesh
immediately to avoid "banding"), plane/axis lifecycle events, `ToastEvent`
(non-UI code surfaces a message without depending on `Application`), and
`ShutdownEvent`.

### Interactive-op controllers (`src/app/InteractiveOpController.*`)
**Every** op with a live preview is an `InteractiveOpController` subclass, and
they all live in one array on `Application`. Membership is what drives the
Esc/Enter chains, single-flight cancellation, gizmo suppression and viewport
input/overlay dispatch - so adding an op no longer means hand-editing four
separate lists, which was a recurring source of half-registered tools.

Three structs are the whole boundary, and no ImGui or renderer type crosses
them: `IopContext` (what a controller needs from the document side),
`IopViewport` (pointer state, pick ray, camera frame) and `IopOverlay` /
`IopGizmo3D` (drawing). Screen positions are `glm::vec2`, colours are packed
`0xAABBGGRR`.

Two render phases, and they must not be mixed up: `drawGizmos3D()` runs during
the 3D pass while the viewport FBO is still bound, so its meshes depth-test
against the scene; `drawOverlay()` runs later, while the ImGui draw list is
built. A raw GL call in the second phase hits the window framebuffer and the
UI paints straight over it.

A controller picks one of three preview models - see *Three preview models* in
`architecture.md` for what each is for and why choosing wrong is a bug:
`SnapshotBody`, `LiveOp`, `HistoryEdit`.

### Coordinate convention
The world is **Y-up** (ground = world XZ plane); the **UI is Z-up** (the user's
Z = world Y). User `(x, y, z)` maps to world `(x, z, y)`. Primitives and exports
apply this conversion so parts stand up correctly for printing.

---

## Directory reference

### `src/` - entry points & platform glue
| File | Purpose |
|---|---|
| `main.cpp` | Desktop entry: CLI parse (`--safe-mode`, `--verbose`), Linux signal/backtrace handler, constructs and runs `Application`. |
| `android_main.cpp` | Android/SDL entry (renamed to `SDL_main`); desktop `main` minus CLI args. |
| `android_platform.{cpp,h}` | Android platform glue - asset/data paths, JNI bridge bits. |
| `android_files.{cpp,h}` | Android Storage Access Framework file open/save via content URIs. |
| `android_shims.cpp` | Small shims/stubs for symbols absent on the Android NDK. |
| `gl_common.h` | Single switch point for GL headers (desktop Core vs GLES3). |
| `gl_shader.cpp` | Shader compile/link helper used by the renderers. |
| `touch_mode.h` | Runtime touch-mode flag - adapts gestures and hit-target sizes. |
| `ui_scale.h` | DPI / UI scale factor used across panels. |

### `src/app/` - the application host
| File | Purpose |
|---|---|
| `Application.{cpp,h}` | The host/god-class: lifecycle, the main run loop, render-on-demand gating, settings, panel ownership, font resolution. Split across the files below (one class, multiple translation units). |
| `Application_Viewport.cpp` | Viewport input: camera control, picking, gizmo handling, grid, box-select, sketch picking (largest TU). |
| `Application_InteractiveOps.cpp` | What is left of the ops here: the sketch↔body **link model**, `cascadeFromSketchEdit`, re-derive vs rigid-move logic, relink, sketch-region picking, and the ops that aren't controllers yet (thread, revolve, pattern, loft, sketch-pattern). |
| `Application_Dialogs.cpp` | Modal popups: Revolve/Lathe, Thread, Text, Subtract, dimension/scale, plus their progress frames. |
| `layout/LayoutCommon.{cpp,h}` | Chrome shared by ALL interface layouts: dockspace host, the menu item lists (incl. plugin menu contributions), overflow popup, shared undo helpers. Add features/plugin entry points HERE so every layout gets them - see the header's contract comment. |
| `layout/classic/ClassicLayout.cpp` | Classic layout (`UiLayout::Classic`): main menu bar + touch panel-collapse handles (the docked panels render from `run()`). |
| `layout/modern/ModernLayout.cpp` | Modern layout (`UiLayout::Modern`): top app bar, left tool rail, right side panel; pins the viewport rect. |
| `layout/imtouch/ImTouchLayout.cpp` | im-touch layout (`UiLayout::ImTouch`): full-bleed viewport with floating overlays - chip, tool bar, model tree, history timeline, create FAB. |
| `InteractiveOpController.{cpp,h}` | Base class + `IopContext`: lifecycle, panel scaffold, and the three preview models. |
| `IopViewport.h` | The viewport/drawing side of the boundary - `IopViewport`, `IopOverlay`, `IopGizmo3D`. |
| `FaceOpControllers.{cpp,h}` | Shell, Draft (TaperOp), Scale-Face, Project-Sketch, Defeature, Resize-Cylindrical, Move-Face. |
| `ExtrudeController.{cpp,h}` | Interactive Extrude - the first `LiveOp` user. |
| `PushPullController.{cpp,h}` + `PushPullState.h` | Interactive Push/Pull, incl. the ghost preview for dense bodies and the smart-cut commit reroute. |
| `EdgeOpController.{cpp,h}` | Fillet / Chamfer. The only op with two preview models: `SnapshotBody` to create, `HistoryEdit` to re-open a committed one. |
| `LiveOpPreview.{cpp,h}` | The `LiveOp` engine for ops that are NOT controllers - Pattern, Loft, Boundary Fill, the construction popups. |
| `HistoryEditPreview.{cpp,h}` | Whole-document snapshot + `editStep` replay + restore-on-failure, behind `HistoryEdit`. |
| `MoveFaceState.h` | Move Face's parameter block (owned by its controller). |
| `CylindricalPick.{cpp,h}` | The cylindrical-face pick result, returned by value rather than left in members. |
| `ProjectSession.{cpp,h}` | Per-tab project session state. |
| `ui_layout_bridge.{cpp,h}` | Global getter/setter so plugins can read and switch the interface layout without a `PluginContext` round-trip. |
| `UserAxes.h` | Helper for user-facing axis definitions. |
| `Window.{cpp,h}` | SDL window + GL context creation, foreground/focus state, swap. |
| `IconData.h` | Embedded window/app icon bytes. |

### `src/core/` - document model & app services
| File | Purpose |
|---|---|
| `Document.{cpp,h}` | The model: bodies, folders, sketches, construction planes/axes; add/remove/lookup; the source of truth renderers read. |
| `History.{cpp,h}` | Op stack, current index, undo/redo, threads-last enforcement, `dropRedoTail`, reload rebuild. |
| `Operation.h` | Abstract base for every modeling op (see Operation model above). |
| `SelectionManager.{cpp,h}` | Current selection (bodies/faces/edges/sketches/regions); fires `SelectionChangedEvent`. |
| `EventBus.h` | Typed pub/sub. |
| `Events.h` | All event struct definitions. |
| `VariableManager.{cpp,h}` | Named user variables/parameters usable in dimensions. |
| `VersionManager.{cpp,h}` | In-project version snapshots + auto-save. |
| `Material.{cpp,h}` | Material/appearance definitions for bodies. |
| `NumFormat.h` | Shared numeric formatting helpers for UI labels/fields. |
| `Verbose.{cpp,h}` | `--verbose` logging plumbing. |

### `src/modeling/` - the geometry operations & sketch engine
The heart of the kernel-facing code. Grouped by role:

**Sketch engine**
| File | Purpose |
|---|---|
| `Sketch.{cpp,h}` | Sketch entity: points, lines, arcs, circles, splines, regions; the `m_detached` body-link flag. |
| `SketchSolver.{cpp,h}` | Constraint solver. |
| `SketchConstraints.h` | Constraint type definitions. |
| `SketchTool.{cpp,h}` | Interactive drawing tool: inference snapping, draw modes, Text placement entry. |
| `SketchEditOp.{cpp,h}` | An editable sketch as a history step. |
| `SketchTransformOp.{cpp,h}` | Move a sketch's plane (with link/de-link bookkeeping). |
| `CombineSketchesOp.{cpp,h}` · `DuplicateSketchOp.{cpp,h}` | Merge / duplicate sketches. |
| `SvgImport.{cpp,h}` | SVG → sketch loops (paths, CSS inlining, `<text>` via system fonts, stroke→outline). |
| `TextSketchOp.{cpp,h}` | TrueType text → closed sketch outlines (bundled fonts). |
| `ProjectSketchOp.{cpp,h}` | Engrave/emboss a sketch onto a flat or curved face. |

**Solid creation**
| File | Purpose |
|---|---|
| `PrimitiveOp.{cpp,h}` | Box / Cylinder / Sphere / Cone / Torus (Z-up authored). |
| `ExtrudeOp.{cpp,h}` | Extrude a sketch region into a solid (carries `ExtrudeMode`); remembers its regions for the sketch-edit cascade. |
| `RevolveOp.{cpp,h}` | Spin a profile around an axis - UI "Lathe" (sketch) / "Revolve" (body). |
| `LoftOp.{cpp,h}` · `SweepOp.{cpp,h}` | Loft between profiles (N sections) / sweep a profile along a path (dormant - no UI, replay-only). |
| `GuidedLoftOp.{cpp,h}` | Loft steered by guide curves. |
| `BoundaryFillOp.{cpp,h}` | Fill a closed boundary of faces/edges into a solid. |

**Modify / feature**
| File | Purpose |
|---|---|
| `PushPullOp.{cpp,h}` | Push/pull a face (both directions, snapshot preview engine). |
| `FilletOp.{cpp,h}` · `ChamferOp.{cpp,h}` | Edge blends / bevels (asymmetric chamfer supported); native OCCT build first, escalating fallbacks after. |
| `BlendCut.{cpp,h}` | Cut-based blend fallback: sweep the blend cross-section along the edge and subtract, for edges a surface feature crosses (#55). |
| `ShellOp.{cpp,h}` | Hollow a body removing a face. |
| `ThreadOp.{cpp,h}` | Validated internal/external screw threads (per-turn dual-family engine). |
| `DefeatureOp.{cpp,h}` | Remove features/faces from a body. |

**Direct face editing**
| File | Purpose |
|---|---|
| `MoveFaceOp.{cpp,h}` | Move a face, respecting feature boundaries. |
| `ScaleFaceOp.{cpp,h}` · `TaperOp.{cpp,h}` | Scale a face (U/V) / draft angle. `TaperOp` is shown to the user as **Draft**; `typeId()` stays `"taper"` because it is the on-disk key. |
| `MoveHoleOp.{cpp,h}` · `ResizeCylindricalOp.{cpp,h}` | Reposition a hole / resize a hole or boss to an exact diameter. |

**Boolean & split**
| File | Purpose |
|---|---|
| `BooleanOp.{cpp,h}` | Cut/Fuse/Common with escalating fuzzy + `BRepCheck_Analyzer` validity gate; multi-target Subtract with keep-tool. |
| `SplitBodyOp.{cpp,h}` | Split a body by a plane. |
| `SeparateBodyOp.{cpp,h}` | Split a body's disconnected solids into individual bodies. |

**Transform / placement / arrange**
| File | Purpose |
|---|---|
| `TransformOp.{cpp,h}` | Rigid body move (with `addFollowSketch` / `addDetachSketch` for the link model). |
| `AxisTransformOp.{cpp,h}` · `PlaneTransformOp.{cpp,h}` | Move/rotate construction axes / planes. |
| `MirrorOp.{cpp,h}` · `AlignOp.{cpp,h}` · `CopyOp.{cpp,h}` · `DeleteOp.{cpp,h}` | Mirror / align / copy / delete. |
| `PatternOp.{cpp,h}` | Linear & Circular (formerly "Radial") patterns. |

**Construction geometry**
| File | Purpose |
|---|---|
| `ConstructionPlaneOp.{cpp,h}` · `ConstructionAxisOp.{cpp,h}` | Create derived planes / axes. |

**Reload & indexing**
| File | Purpose |
|---|---|
| `OperationFactory.{cpp,h}` | typeId → fresh op, for re-editable reloaded history. |
| `ReplayOp.{cpp,h}` | Baked geometry-replay fallback for ops that can't yet rehydrate. |
| `SubShapeIndex.{cpp,h}` | Map ops to the sub-shapes (faces/edges) they generated. |
| `FaceLineage.{cpp,h}` | Face → ancestry-id map (topological naming): which op made a face, surviving downstream splits/merges (#49/#51). |
| `FaceSurfSig.h` | Geometric "same underlying surface?" test - tells created faces from re-trimmed ones for history-hover highlights. |

> Note: there is **no** `RotateOp` - rotation goes through `TransformOp` /
> `AxisTransformOp` driven by the gizmo. (The old map listed one in error.)

### `src/io/` - persistence & exchange
| File | Purpose |
|---|---|
| `ProjectIO.{cpp,h}` | Native `.mzr` / `.materializr` save/load (v3 gzip format): bodies, sketches, full op history. Transactional on failure - a bad file leaves an empty document. |
| `ProjectRecovery.{cpp,h}` | Crash-recovery autosave of the whole project (quiescence-debounced) + restore prompt. |
| `Settings.{cpp,h}` | Persisted app settings (incl. `--safe-mode` recovery); ints clamped and strings sanitized on the `.cfg` round trip. |
| `SketchRecovery.{cpp,h}` | Draft autosave sidecar + restore prompt for an uncommitted sketch. |
| `StepIO.{cpp,h}` · `IgesIO.{cpp,h}` | STEP / IGES import + export (IGES applies the Y-up/Z-up conversion). |
| `BrepIO.{cpp,h}` | OCCT BREP import + export (exact geometry, no tessellation). |
| `StlIO.{cpp,h}` · `StlExport.{cpp,h}` · `GltfExport.{cpp,h}` | STL import / STL / glTF export (Z-up corrected). |
| `MeshDecimate.{cpp,h}` | Mesh simplification for imported STL. |
| `ObjExport.{cpp,h}` · `ThreeMfExport.{cpp,h}` | OBJ / 3MF export. |
| `SvgExport.{cpp,h}` | Sketch → SVG at 1:1 mm (laser / 2.5D CNC). |
| `DxfExport.{cpp,h}` · `DxfImport.{cpp,h}` | Sketch → DXF export / DXF → sketch import (laser/CNC profiles). |
| `ImageDecode.{cpp,h}` | PNG/JPEG decode (stb_image) for reference images. |
| `FileDialogs.{cpp,h}` | Native open/save dialog wrapper (scrubs AppImage env when spawning). |
| `portable-file-dialogs.h` | Vendored single-header native-dialog bridge. |

### `src/ui/` - panels, dialogs, theme
| File | Purpose |
|---|---|
| `Toolbar.{cpp,h}` | The main toolbar; context-sensitive buttons (e.g. Lathe vs Revolve). |
| `PropertiesPanel.{cpp,h}` | Per-selection / per-op editor; link hint + Re-link button. |
| `HistoryPanel.{cpp,h}` | History tree, Today/Yesterday/date buckets, Apply-changes. |
| `ItemsPanel.{cpp,h}` | Bodies / folders / sketches / planes / axes tree. |
| `MaterialPanel.{cpp,h}` | Material assignment. |
| `MeasureTool.{cpp,h}` | Measurement tool + panel. |
| `SectionPanel.{cpp,h}` | Section View controls. |
| `VariablePanel.{cpp,h}` | User variable editor. |
| `VersionPanel.{cpp,h}` | Version snapshots UI. |
| `DimensionInput.{cpp,h}` | Inline dimension / value entry widget. |
| `DrawingView.{cpp,h}` | 2D drawing/sketch view. |
| `ShortcutsPanel.{cpp,h}` · `HelpPanel.{cpp,h}` · `AboutDialog.{cpp,h}` | Shortcuts / Help → User Guide / About. |
| `StatusBar.{cpp,h}` | Bottom status bar. |
| `ToastNotification.{cpp,h}` | Transient toast messages (consumes `ToastEvent`). |
| `UpdateChecker.{cpp,h}` | Help → Check for Updates (libcurl HTTPS GET). |
| `ThemeManager.{cpp,h}` · `UiTheme.h` | ImGui theme / colours. |

### `src/viewport/` - rendering, picking, camera
| File | Purpose |
|---|---|
| `Viewport.{cpp,h}` | Viewport orchestration - sets up passes, owns render targets. |
| `Camera.{cpp,h}` | Orbit/pan/zoom camera, projection, view matrices. |
| `ShapeRenderer.{cpp,h}` | Tessellates + draws solid bodies (PBR). |
| `EdgeRenderer.{cpp,h}` | Body edge/wireframe drawing. |
| `SketchRenderer.{cpp,h}` | Sketch geometry + selected-circle/arc highlights. |
| `Picker.{cpp,h}` | Ray/screen picking of bodies, faces, edges. |
| `BoxSelect.{cpp,h}` | Rubber-band box selection. |
| `SelectionHighlight.{cpp,h}` | Highlight overlay for the current selection. |
| `Gizmo.{cpp,h}` | Translate/rotate manipulator. |
| `Grid.{cpp,h}` | Infinite world grid shader (grazing-angle fade fix). |
| `AxisRenderer.{cpp,h}` · `PlaneRenderer.{cpp,h}` | Construction axes / planes draw. |
| `ViewCube.{cpp,h}` | Orientation cube. |
| `SectionView.{cpp,h}` | Shader clip plane + section-curve overlay. |
| `RefImageRenderer.{cpp,h}` | Reference-image planes in the viewport (textured quads, opacity). |
| `BackgroundRenderer.{cpp,h}` | Gradient/background pass. |
| `PBRShaders.h` | Embedded PBR shader sources. |

### `src/plugin/` - plugin infrastructure
| File | Purpose |
|---|---|
| `PluginRegistry.{cpp,h}` | Static registry of all plugins; the host iterates it at startup. |
| `PluginContext.{cpp,h}` | A plugin's handle to document/history/selection/events/camera + the `register*` contribution methods. |
| `Contributions.h` | Contribution structs (toolbar, command, menu, IO, render pass, property, overlay) + `SelectionContext`. |
| `PluginMacro.h` | `REGISTER_PLUGIN` auto-registration + force-link macro. |
| `InteractiveTool.h` | Interface for a plugin-supplied modal viewport tool. |

### `src/plugins/` - the feature plugins
Each self-registers via `REGISTER_PLUGIN`. `ForceLink.cpp` exists to keep them
from being stripped.
| File | Registers |
|---|---|
| `CoreCommandsPlugin.cpp` | New/Open/Save/Undo/Redo and core menu items. |
| `SketchPlugin.cpp` | Start-sketch + the sketch toolset. |
| `ExtrudePlugin.cpp` · `PushPullPlugin.cpp` | Extrude / push-pull. |
| `RevolvePlugin.cpp` · `LoftPlugin.cpp` | Lathe/Revolve / Loft (incl. guided + N-section). |
| `BoundaryFillPlugin.cpp` · `RefImagePlugin.cpp` | Boundary Fill / reference-image import + overlay. |
| `FilletPlugin.cpp` · `ChamferPlugin.cpp` · `ShellPlugin.cpp` | Fillet / chamfer / shell. |
| `BooleanPlugin.cpp` | Subtract (multi-target modal) / Union / Intersect. |
| `SplitBodyPlugin.cpp` · `MirrorPlugin.cpp` · `PatternPlugin.cpp` | Split / mirror / Linear+Circular pattern. |
| `PrimitivesPlugin.cpp` | Box/Cylinder/Sphere/Cone/Torus buttons. |
| `TransformPlugin.cpp` · `GizmoDragPlugin.cpp` · `CopyPlugin.cpp` · `DeletePlugin.cpp` | Move/Rotate / gizmo-drag → TransformOp / copy / delete. |
| `ConstructionPlanePlugin.cpp` · `ConstructionAxisPlugin.cpp` | Add Plane… / Add Axis + their render passes. |
| `StepIOPlugin.cpp` · `IgesIOPlugin.cpp` · `BrepIOPlugin.cpp` · `StlImportPlugin.cpp` · `StlExportPlugin.cpp` · `GltfExportPlugin.cpp` · `SvgImportPlugin.cpp` · `DxfImportPlugin.cpp` · `ObjExportPlugin.cpp` · `ThreeMfExportPlugin.cpp` | IO-format contributions. |
| `TutorialPlugin.cpp` | Onboarding overlay (non-modal). |
| `ForceLink.cpp` | Calls every plugin's force-link symbol. |

### `src/third_party/`
| File | Purpose |
|---|---|
| `nanosvg.h` | Vendored SVG parser used by SVG import. |
| `stb_image.h` | Vendored image decoder (PNG/JPEG…) behind `ImageDecode` for reference images. |

---

## Where do I look if…

- **A modeling operation is wrong** → its `*Op.{cpp,h}` in `src/modeling/`
  (start with `execute()`), and `Operation.h` for the lifecycle contract.
- **A toolbar button / menu item is missing or mis-gated** → the owning plugin
  in `src/plugins/` and its `SelectionContext` in the contribution.
- **A reopened project lost its editability** → `rehydrateFromReload` on that
  op, `OperationFactory.cpp`, and the `ReplayOp` fallback path.
- **Selection won't pick / highlights wrong** → `src/viewport/Picker.cpp`,
  `SelectionHighlight.cpp`, `SelectionManager.cpp`.
- **A sketch edit doesn't propagate to its body** → the link model in
  `Application_InteractiveOps.cpp` (`cascadeFromSketchEdit`, `bodySafelyRederivable`).
- **An interactive popup op (shell/draft/scale-face) misbehaves** →
  `FaceOpControllers.cpp` + `InteractiveOpController.cpp`, popups in
  `Application_Dialogs.cpp`.
- **Push/Pull, Extrude or Fillet/Chamfer misbehaves** → their own controllers,
  `PushPullController.cpp` / `ExtrudeController.cpp` / `EdgeOpController.cpp`.
  If the symptom is "the preview left something behind" or "a body changed id
  mid-drag", suspect the preview model first - see *Three preview models* in
  `architecture.md`.
- **A tool is missing from ONE layout** → it isn't in `Toolbar::railTools()`,
  which is the catalogue all three layouts read. (Sketch mode is the one
  exception; it still has per-layout lists.)
- **File save/load or an export format is broken** → the matching file in
  `src/io/` (native = `ProjectIO.cpp`).
- **A panel renders wrong** → the matching `src/ui/*Panel.cpp`.
- **Something draws/fades incorrectly in the scene** → the matching renderer in
  `src/viewport/`.
- **A cross-component reaction is missing** → the event in `Events.h` and who
  subscribes via `EventBus`.
- **Android-only behaviour** → `src/android_*` plus `touch_mode.h`.

---

*Keep this map current when adding a directory, a plugin, or an op type - it is
the fastest on-ramp into the codebase.*
