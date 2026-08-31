# Integration points

Companion to `SPEC.md`. Every file a sketch tool has to touch to become reachable, drawn and committable, traced from Mirror — the closest existing analogue, and the template the spec follows.

Line numbers are as of 2026-08-31 and will drift; the symbol names are the durable part.

## `SketchPlugin.cpp` is not a host — read this first

An earlier draft of this file named `src/plugins/SketchPlugin.cpp` as Offset's mode host. **That was wrong**, and adversarial review caught it. The plugin's own registration says so:

> The plugin `SketchModeTool` below is intentionally NOT wired to any command or button: `Application` never calls `InteractiveTool::handleInput()`, so the tool can't receive viewport input, and once activated its `renderOverlay()` runs outside any ImGui window — which spawned a stray "Debug" window. The class is kept only as a reference for a future input-routing migration.

Two consequences an implementer must not miss:

1. **Offset needs no code in `SketchPlugin.cpp` at all.** Adding it there would create a second, unreachable, untested path. `Application` is the only host.
2. **Neither typed-value popup is the right one.** `##SketchDim` in this file is dead alongside the rest of the class. The live `##SketchDimInput` in `Application_Viewport.cpp` is also unusable here: it funnels into `SketchTool::applyDimension`, which hard-requires `m_isPlacing` (a flag Offset never sets) and ends `default: return false`. Offset gets a **bespoke floating panel** modelled on Mirror's — see `state-machine.md`.

This also reopens a question the spec had treated as settled: `CONTRIBUTING.md:45` requires a new tool to live in `src/plugins/` and forbids bolting feature logic into core `Application`, and the sketch tools do all of the latter. That is now recorded in `SPEC.md` as a **blocking** maintainer decision — not an open question, and not something this spec resolves by assertion.

## The Mirror trace

| File | What Mirror does there | Offset equivalent |
|---|---|---|
| `src/modeling/SketchTool.h:19` | `SketchToolMode::Mirror` in the enum | **Append** `Offset` after `Airfoil` — see below |
| `src/modeling/SketchTool.h:206-229` | `isMirrorActive` / `getMirrorAnchor` / `setMirrorAngle` / `beginMirror` / `cancelMirror` / `getMirrorPreview` / `commitMirror`, plus the private state block at ~550 | `beginOffset` / `cancelOffset` / `setOffsetDistance` / `getOffsetPreview` / `commitOffset` |
| `src/modeling/SketchTool.cpp:3422+` | the `// --- Interactive Mirror ---` implementation block | a sibling `// --- Interactive Offset ---` block |
| `src/ui/Toolbar.h:20` | `ToolAction::SketchMirror` in the enum | `ToolAction::SketchOffset` |
| `src/ui/Toolbar.cpp:204` | icon button in the sketch-transform group | button + tooltip |
| `src/ui/Toolbar.cpp:759` | the classic-panel button | button + tooltip |
| `src/app/Application.cpp:2329` | `case ToolAction::SketchMirror:` guards `m_inSketchMode`/`m_activeSketch`/`m_sketchTool`, calls `beginMirror()`, reports when there is nothing to act on | same shape, calling `beginOffset()` |
| `src/app/layout/modern/ModernLayout.cpp:501` | dispatch case | dispatch case |
| `src/app/layout/imtouch/ImTouchLayout.cpp:866` | dispatch case | dispatch case |
| `src/app/Application_Viewport.cpp:2313-2350` | draws the dashed axis and the live ghost via `getMirrorPreview` | draws the ghost via `getOffsetPreview` |
| `src/app/Application_Dialogs.cpp:4261-4317` | the floating "Mirror" window: commit button wraps `commitMirror` in `recordSketchMutation`, Cancel and close both call `cancelMirror` | `renderOffsetToolPanel()` — same window shape, with the distance field |
| `src/app/Application.h:128` | `void renderMirrorToolPanel();` declaration | `void renderOffsetToolPanel();` beside it |
| `src/app/Application.cpp:7569` | `renderMirrorToolPanel();` in the per-frame panel block | `renderOffsetToolPanel();` beside it |

Note `src/viewport/SketchRenderer.cpp` is **not** on this list despite referencing `SketchToolMode` — Mirror's preview is drawn in `Application_Viewport.cpp`, not the renderer. Follow Mirror.

Three additions Mirror does not need: `Application.cpp` must gain an **Escape branch** for Offset (see `state-machine.md`); `SketchTool::setMode` must clear Offset state where it already clears `m_mirrorActive`; and `beginOffset` must **restore the captured selection after** `setMode`, since `setMode` calls `clearElementSelection()` for every mode but `Select`.

## The two things that are easy to get wrong

**Enum ordering.** `SketchTool.h:15` carries an explicit `APPEND ONLY` warning: the toolbar mirrors the active tool as the enum's raw index and hardcodes those indices, so inserting `Offset` anywhere but the end silently highlights the wrong button — the header's own example is Dimension becoming 13 while the button still tests 12. Append after `Airfoil`.

**Undo comes from the wrapper, not the op.** Mirror gets undo because its commit runs inside `recordSketchMutation` (`Application_Dialogs.cpp:4302`), which snapshots the sketch — not because `MirrorOp` exists (that is the separate 3D body mirror, `modeling/MirrorOp.h`). Wrapping `commitOffset` the same way is what satisfies CAP-6, and is why the spec needs no new modeling op.

## Geometry reference

`SvgImport.cpp:657` is the existing `BRepOffsetAPI_MakeOffset` call site and the right thing to read first — but copy its setup, not its output handling. It samples the offset wire down to a polyline via `sampleOffsetWire` because it only needs a render outline; doing that here would forfeit CAP-3. Walk the result edges with `BRepAdaptor_Curve` instead. `src/ui/PropertiesPanel.cpp:127-138` shows the `GetType()` / `GeomAbs_Circle` classification pattern already in use in this codebase.

Its guard is also insufficient here — see Finding 2 in `occt-offset-findings.md`.

## Commit shape

`commitMirror` (`SketchTool.cpp`) is the model for the *shape* of writing generated geometry back: remap ids through a lambda, then emit via `addPoint` / `addLine` / `addCircle` / `addArc`. Copy that structure — but not its welding, and not its commit timing.

Three things not to copy blindly:

**The weld radius.** `commitMirror` welds through `findCoincidentPoint`, which matches anything within `0.3f * snapScale()` — a UI *snap* radius, not a geometric tolerance. Mirror can live with that because a reflection lands far from its source; an offset does not. A valid 0.1 mm offset would weld its output straight onto the source and collapse. Offset welds result-to-result endpoints only, at the scale-aware `weldTol` in `tolerances.md`, and never welds to source geometry.

**The single-phase commit.** `commitMirror` mutates the sketch as it converts. Offset cannot: it must build a complete entity plan first and only then apply it, because `recordSketchMutation` does not roll back a partial failure. See the commit transaction in `tolerances.md`.

**The out-param asymmetry.** `commitMirror`'s out-params are `std::set<int>& outPoints, std::set<int>& outLines` — points and lines only — even though it emits circles, arcs and splines too. They exist to restore the selection after commit, so mirrored circles and arcs end up unselected. Offset sidesteps this entirely: the **source** stays selected after commit (resolved in `SPEC.md`), which is also what lets repeated offsets nest.

## Error delivery

`ToolAction::SketchMirror` reports failure with `std::fprintf(stdout, "Mirror: nothing to mirror\n")` — invisible to anyone running the app normally. CAP-4 and CAP-5 require distinct, user-visible messages, so Offset cannot copy this. `Application::showToast(const std::string&, double seconds = 4.0)` (`Application.h:120`) already exists and is the channel; `beginOffset`/`commitOffset` return a structured error enum so each refusal maps to its own toast.
