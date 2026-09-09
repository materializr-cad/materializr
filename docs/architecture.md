# Architecture

A high-level tour of how the codebase is organised and the patterns it leans
on. Aimed at someone about to dig in and make changes.

## Source layout

```
src/
├── app/          # Application lifecycle, main loop, input handling,
│                 #   and the interactive-op controllers (see below)
├── core/         # Document, History, Operation, Selection, EventBus, Material
├── modeling/     # CAD operations (Extrude, PushPull, Fillet, Boolean, Sketch, …)
├── plugin/       # Plugin registry + contribution types (toolbar/command/IO/tool)
├── plugins/      # Each operation, tool, and IO format registered as a plugin
├── viewport/     # 3D rendering (Camera, Grid, ShapeRenderer, Gizmo, ViewCube, Picker)
├── ui/           # ImGui panels (Toolbar, History, Items, Properties, …)
├── io/           # File I/O (STEP, STL, IGES, glTF, ProjectIO, Settings, FileDialogs)
└── *_main.cpp    # Per-platform entry points (main, android_main, ios_main)
shaders/          # GLSL shaders (grid, mesh, outline)
tests/            # Google Test unit tests
android/          # Gradle project + JNI glue for the Android build
scripts/          # Build scripts (AppImage packaging)
packaging/        # macOS .dmg, Windows NSIS installer, winget manifests
.github/          # CI workflows
```

## Plugin registry

Modelling operations, interactive tools, and IO formats are registered through
a **plugin registry** (`src/plugin`). Each feature in `src/plugins` declares
its toolbar buttons, commands, and handlers in a `REGISTER_PLUGIN(...)` block
that runs at startup. The adaptive toolbar surfaces those buttons based on
the current selection context (nothing / face / edge / body / sketch / sketch
region).

This means adding a new modelling op is mostly a matter of dropping a new
`*Plugin.cpp` next to the others and listing it in `CMakeLists.txt`; the rest
of the app discovers it automatically.

Interactive tools that need viewport drag events (the gizmo-based ones,
push/pull, extrude, fillet/chamfer, the face ops) can't express themselves
through the plugin-tool interface, so a plugin registers only their toolbar
entry point and the tool itself is an **interactive-op controller** - see
below.

## Core design patterns

- **Command pattern** - every modelling operation derives from `Operation`
  (`src/core/Operation.h`) and stores enough state to `undo()` itself. Push
  /Pull, fillet, chamfer, extrude, delete, transform, mirror, split - all
  undoable.
- **Linear history** - `History` (`src/core/History.cpp`) maintains an ordered
  list of operations. The model is the result of replaying them. `editStep`
  walks back to a step in place (via per-op `undo()` then re-`execute()`)
  rather than clearing and replaying from scratch, so the base bodies and
  body ids stay stable.
- **`captureDiff()` for history persistence** - each op reports the set of
  modified/created/deleted body shapes it produced (read from its stored
  undo data, not via `undo()`/`execute()`). A reverse walk over those diffs
  reconstructs the body state after each step, which is what the project file
  stores. On reload the history is rebuilt as `ReplayOp` instances that
  restore the right body set per step.
- **Adaptive UI, one catalogue** - toolbar content changes with the selection
  type (nothing, face, edge, body, sketch, sketch region). `Toolbar::railTools()`
  is the single source of *which* tools a context offers; all three interface
  layouts read it. Each layout keeps its own *presentation* - the classic
  palette has section headers and a three-across Move/Rotate/Scale row, the
  modern and im-touch rails are flat icon lists - so classic's buttons are
  gated on `catalogOffers(action)` and anything the layout doesn't place
  itself is rendered by `renderCatalogRemainder()`. Adding a tool to the
  catalogue therefore reaches every layout. (Sketch mode is deliberately
  outside this: its palette is inseparable from its chrome - solver badge,
  inference cycle, polygon-sides popout, constraint buttons keyed to
  selection arity.)
- **Interactive-op controllers** - every op with a live preview is an
  `InteractiveOpController` (`src/app/InteractiveOpController.h`) registered in
  one array. Membership is what drives the Esc/Enter chains, single-flight
  cancellation, gizmo suppression and viewport input dispatch, so adding an op
  no longer means hand-editing four separate lists. A controller implements a
  few small hooks; the lifecycle and panel scaffold come from the base.
- **Three preview models** - an interactive op previews in one of three ways,
  and picking the wrong one is the classic source of bugs here:
  - `SnapshotBody` - snapshot one body, run a fresh transient op against it
    each frame. Shell, Draft, Scale Face, Projection, Defeature, Resize
    Cylindrical, and fillet/chamfer *creation*.
  - `LiveOp` - keep ONE op instance and toggle it `undo()`/`execute()` against
    the document, recording it with `pushExecuted()` at commit. Required
    whenever the op CREATES bodies, because the same instance re-uses the ids
    it minted - otherwise every created body changes id on every frame.
    Extrude and Push/Pull. `LiveOpPreview` (`src/app/LiveOpPreview.h`) is the
    same engine for ops that aren't controllers (Pattern, Loft, Boundary Fill,
    the construction popups).
  - `HistoryEdit` - the op is already ON History and its parameter is what the
    user is dragging (re-opening a committed fillet). The preview has to BE
    the real replay, or downstream steps flicker out for the drag.
    `HistoryEditPreview` guards it with a whole-document snapshot.

  None of them push a real History step per preview frame. That pattern -
  `pushOperation` then `undo` on the next change - was removed everywhere: its
  `canUndo()` guard is not "my preview is on top", so any mid-gesture history
  touch undid the user's work instead.
- **Separation of concerns** - geometry kernel (OCCT), UI (ImGui), and
  rendering (OpenGL) are kept apart. `core/` and `modeling/` know nothing
  about ImGui or OpenGL; `ui/` and `viewport/` know nothing about the
  modelling operations beyond their public API.

## Tech stack

| Component | Technology |
|-----------|-----------|
| Geometry kernel | OpenCASCADE Technology (OCCT) 7.9.3 - the same version on every platform, built from source for the Linux/Docker builds |
| UI framework | Dear ImGui (pinned release tag of the docking branch) |
| Windowing / input | SDL2 - one backend on desktop, Android and iOS alike |
| Rendering | OpenGL 3.3 Core on desktop, OpenGL ES 3.0 on mobile, PBR shading |
| GL loader | Mesa prototypes (Linux), GLEW (Windows), `OpenGL.framework` (macOS), GLES3 headers direct (Android/iOS) |
| Math | GLM |
| HTTP | libcurl (Help → Check for Updates); stubbed out on mobile |
| Build | CMake 3.20+ |
| Packaging | AppImage (Linux), NSIS installer + portable zip (Windows), `.dmg` (macOS), APK/AAB (Android) |
| Testing | Google Test |
| Language | C++17 |

## Cross-platform notes

The codebase is kept platform-portable with localised `#ifdef _WIN32` blocks
rather than parallel implementations:

- **GL loader** (`src/gl_common.h`) - Mesa prototypes on Linux, GLEW on
  Windows, `<OpenGL/gl3.h>` on macOS, `<GLES3/gl3.h>` on mobile. The GLES
  branch also intercepts `glShaderSource` to rewrite `#version 330 core`
  shaders as `#version 300 es`, so the ~15 renderers need no per-platform
  source.
- **File dialogs** (`src/io/FileDialogs.cpp`) - POSIX `dirent`/`stat` on
  Linux, `std::filesystem` on Windows.
- **Settings path** (`src/io/Settings.cpp`) - `$XDG_CONFIG_HOME` / `~/.config`
  on Linux, `%APPDATA%` on Windows.
- **ImGui layout file** - relative `imgui.ini` on Linux (AppImage runs from a
  writable cwd), `%APPDATA%\materializr\imgui.ini` on Windows (Program Files
  isn't writable for non-admin processes).
- **OpenCASCADE discovery** (`CMakeLists.txt`) - Windows (vcpkg) and macOS
  (Homebrew) both ship a clean CMake config; a source build at a custom prefix
  takes the same clean path via `-DMZR_OCCT_PREFIX`. Only the distro-apt Linux
  path needs the libtbb-symlink patching.
- **Entry points** - `src/main.cpp` on desktop, `src/android_main.cpp` and
  `src/ios_main.cpp` on mobile, each behind an SDL main. Platform file pickers
  and storage live in `android_files.cpp` / `ios_files.mm` behind
  `mobile_files.h`.
