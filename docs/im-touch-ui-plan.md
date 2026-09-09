# "im-touch" tablet UI - implementation plan

> **Historical note (2026-07):** this plan shipped, then the naming and file
> layout were reorganized. What this document calls the "im-touch shell" is
> now the **Modern** layout, and the "lite" variant is now the **im-touch**
> layout; the coupled `imTouchUi`/`imTouchLite` flags became the `UiLayout`
> enum (`classic | modern | imtouch`, settings key `uiLayout`). The code moved
> from `Application_TouchShell.cpp` to `src/app/layout/{classic,modern,imtouch}/`
> with the shared fundamentals in `src/app/layout/LayoutCommon.*`.

Goal: an opt-in tablet shell (Settings → Interface → **im-touch UI**) that
replaces the desktop docking layout (menu bar + dockable panels + status bar)
with a purpose-built touch layout, per the mockup:

```
┌────────────────────────────────────────────────────────────────┐
│ ◼ Materializr / mug.mzr              ↺  ↻  [⤢ Focus]  [⋯]      │  top app bar
├──────┬──────────────────────────────────────┬──────────────────┤
│TOOLS │                                      │ [Items] History  │
│ ▭    │                                      │ BODIES           │
│Sketch│                                      │ ☑ Mug        ⋯   │
│ ⇕    │                                      │ ☑ Handle     ⋯   │
│ Push │            3D viewport               │ SKETCHES         │
│▐▛▜▌  │          (edge to edge)              │ ☐ base sketch ⋯  │
│Extrde│                                      │ ☐ handle prof ⋯  │
│ ...  │                                      │ CONSTRUCTION     │
│      │                                      │ ☐ YZ Plane    ⋯  │
│[FULL]│                                      │                  │
└──────┴──────────────────────────────────────┴──────────────────┘
```

Everything is additive and gated on the new setting: the desktop UI stays the
default and byte-identical when the toggle is off. Both shells share all
app logic (ToolAction dispatch, panels' data, History, SelectionManager) -
this is a *presentation* fork, not a logic fork.

---

## Current architecture (what we build on)

| Piece | Where | Reused how |
|---|---|---|
| Panels as dockable ImGui windows | `src/ui/*Panel.cpp` - each does its own `ImGui::Begin("Items")` etc. | Refactor each into `renderContent()` (body only) + thin `render()` wrapper (window + Begin). Touch shell calls `renderContent()` inside its own containers. |
| Tool dispatch | `Toolbar::render() → ToolAction` (`src/ui/Toolbar.h`), handled in `Application`; plugins add buttons via `renderPluginButtons()` | Same `ToolAction` enum + handler. The rail is a new *view* over the same actions. |
| Contextual tool sets | `Toolbar::render{Body,Face,Edge,Sketch,NoSelection}Tools()` - selection-driven | Extract the *catalogue* (what's available now) from the *rendering* (buttons) - see Phase 3. |
| Dock layout | `Application::renderDockspace()` (DockHost + imgui.ini) | Skipped entirely in touch shell (fixed windows, no docking, no ini churn). |
| Menu bar | `Application::renderMenuBar()` - File/Edit/View/Help | Menu *items* move into the top bar's ⋯ overflow popup; extract shared helpers so both shells call the same actions. |
| Status bar | `src/ui/StatusBar.cpp` overlay | Not shown in mockup: project name → top bar; selection/tool hint → small viewport chip (Phase 4); rest dropped in touch shell. |
| Theming | `ThemeManager` (Dark/Light), `UiTheme.h` helpers | New style-pack pushed on top when the touch shell is active. |
| Settings | `AppSettings` (`src/io/Settings.h`) + `SettingsPanel` | New bool, new checkbox in Interface section. |
| Touch input | `touch_mode.h`, `Window.cpp` finger path | Unchanged. `imTouchUi` is orthogonal to `touchMode` (layout vs input); enabling im-touch UI *suggests* touch mode on but doesn't require it. |

## Phase 0 - Setting + shell switch (skeleton)

1. `AppSettings::imTouchUi = false` (`src/io/Settings.h`) + serialize in
   `Settings.cpp` (key `imTouchUi`).
2. `SettingsPanel` → Interface section: checkbox **"im-touch UI (tablet
   layout)"** with a one-line description. Takes effect next frame - no
   restart. (The docking layout persists untouched in imgui.ini, so toggling
   back is lossless.)
3. `Application::run()` frame body:
   ```cpp
   if (m_settings.imTouchUi) renderTouchShell();   // new
   else { renderDockspace(); renderMenuBar(); ... existing path ... }
   ```
   `renderTouchShell()` lives in a new `src/app/Application_TouchShell.cpp`
   (pattern: `Application_Viewport.cpp` already splits big UI code out).
4. Skeleton shell: four fixed, non-docked windows (NoTitleBar/NoResize/NoMove/
   NoCollapse) laid out from `WorkPos/WorkSize` (safe-area aware for free -
   beginFrame() already insets the work rect on iOS):
   - `##TouchTopBar` - full width × 56·s
   - `##TouchRail`   - 84·s wide, below top bar, left
   - `##TouchRight`  - 320·s wide, below top bar, right
   - viewport host - the remaining center rect; render the existing central
     `"Viewport"` window pinned there with `SetNextWindowPos/Size` (it already
     tolerates being undocked - it's an ordinary window).
   (s = `Window::uiScale()`.)

**Exit criteria:** toggle swaps between untouched desktop UI and a hollow
skeleton (grey bars + live viewport) at runtime, on all platforms.

## Phase 1 - Touch theme + widget kit + icons

1. **Icon font.** Bundle [Iconoir](https://iconoir.com) (MIT; the upstream
   npm package is SVG-only, so we ship the TTF build from
   [Silbad/iconoir-font](https://github.com/Silbad/iconoir-font), also MIT -
   `assets/fonts/Iconoir.ttf` + licenses + FONT-CREDITS.md) and a generated
   codepoint header `src/ui/IconsIconoir.h` (1385 glyphs, PUA U+E000…).
   Semantic aliases (`MZ_ICON_*`) live in `src/ui/TouchIcons.h` so a design
   pass swaps glyphs in one place. The FULL range is merged into the atlas at
   font load (`MergeMode`) - any glyph is always renderable; atlas cost is
   acceptable. Iconoir has CAD-ready names: `extrude`, `fillet-3d`, `axes`,
   `angle-tool`, `frame-select`, `box-3d-*`, `design-pencil`, `ruler`.
   Regenerate the header from the font build's `iconoir.json`:
   ```
   python3 -c "import json;d=json.load(open('iconoir.json'));\
   print('\n'.join(f'#define ICON_IC_{k.upper().replace(chr(45),chr(95))} ...' for k in d))"
   # full snippet: see git history of src/ui/IconsIconoir.h generation
   ```
2. **`src/ui/TouchTheme.h/.cpp`** - a style-pack applied inside the shell
   (Push/Pop around the shell render so desktop dialogs opened from it still
   look right):
   - geometry: `FrameRounding/ChildRounding/PopupRounding = 10·s`,
     `WindowRounding = 14·s`, `FramePadding = (14,10)·s`,
     `ItemSpacing = (10,12)·s`, min touch target 44pt enforced by the kit.
   - palette (from mockup, over the existing dark theme): window `#101318`,
     bars `#0B0D11`, viewport backdrop unchanged, accent `#8FB4F2` fill /
     `#3D6FD9` outline, section-header grey `#8A8F98`, hairline separators
     `#22252B`. Light-theme variants derived the same way `ThemeManager`
     does (Phase 5 polish; dark first - the mockup is dark).
3. **Widget kit** (`src/ui/TouchWidgets.h/.cpp`) - the five primitives the
   mockup is made of, so screens stay declarative:
   - `railButton(icon, label, active)` - 64·s square, icon over 10pt label,
     accent-filled rounded rect when active
   - `pillButton(icon, label)` / `iconButton(icon)` - top-bar buttons
   - `segmented(items, activeIdx)` - the Items|History switcher
   - `sectionHeader(text)` - small-caps grey ("BODIES")
   - `listRow(checkbox, label, overflowMenuId)` - 44·s row: leading
     checkbox (visibility), label, trailing ⋯ that opens a popup

**Exit criteria:** a debug screen (behind the toggle) showing every widget;
icons crisp at 1×/2×; no style leakage into the desktop shell.

## Phase 2 - Top bar + right panel (content into containers)

1. **Panel refactor (mechanical, shared):** split `ItemsPanel::render()` and
   `HistoryPanel::render()` into `render()` (desktop window wrapper, existing
   look) + `renderContent()` (everything between Begin/End). No behavior
   change on desktop - verify with the existing UI side-by-side.
2. **Top bar:** logo chip (reuse `icon.png` - load once as a GL texture, or
   draw a rounded-rect monogram to start), app name + `/ <project basename>`
   (from `m_currentProjectPath`, "New project" fallback - same source as
   StatusBar), then right-aligned: undo/redo (`m_history->canUndo/canRedo`,
   same calls as the Edit menu), **Focus** toggle, ⋯ overflow.
   - ⋯ overflow popup = the four menus flattened into one grouped popup
     (Open/Save/Save As/Export ▸, Edit ▸, View ▸, Settings, Help, About).
     Extract each `renderMenuBar()` section into `renderFileMenuItems()` etc.
     so the desktop menu bar and the overflow popup share one item list -
     no duplicated MenuItems to drift.
3. **Right panel:** `segmented({Items, History})` (persist choice in
   `AppSettings::touchRightTab`); body scroll region hosts the refactored
   `renderContent()`s. Restyle ItemsPanel content *in touch mode only* via
   the widget kit: group headers (BODIES/SKETCHES/CONSTRUCTION - the panel
   already has these groupings), `listRow` per item with the visibility
   checkbox and the ⋯ popup wrapping the panel's existing per-item context
   menu items (rename/delete/edit - already implemented as right-click menus;
   reuse the same code via a shared `renderItemActions(item)`).

**Exit criteria:** top bar + right panel fully functional over a live
project; every action verified identical to its desktop counterpart.

## Phase 3 - Tool rail (the real work) - **DELIVERED**

> **Status (2026-08-06, `27e80f5`):** the catalogue exists and all three
> layouts read it. It landed as `Toolbar::railTools()` rather than the
> `availableTools()` sketched below, and it took the opposite approach to
> step 1: the desktop renderers were NOT replaced by a generic renderer over
> the catalogue. Classic's presentation is genuinely different - section
> headers, a three-across Move/Rotate/Scale row, a Fabrication group - and a
> generic renderer would have flattened all of it. Instead the catalogue owns
> **availability** (`catalogOffers(action)` gates each classic button) and each
> layout owns **presentation**, with `renderCatalogRemainder()` rendering
> anything a layout doesn't place itself. That last part is the anti-drift
> net: a tool added to the catalogue reaches every layout even if nobody
> writes a button for it. Sketch mode is deliberately excluded - its palette
> is inseparable from its chrome (solver badge, inference cycle, polygon-sides
> popout, constraint buttons keyed to selection arity).

The rail must show *the right tools for the current context* - exactly what
`Toolbar` already computes, but its catalogue is welded to immediate-mode
rendering across `render{NoSelection,Body,Face,Edge,Sketch*}Tools()`.

1. **Extract a tool catalogue.** New `Toolbar::availableTools()` returning
   `std::vector<ToolEntry>{ icon, label, ToolAction, enabled, active,
   sectionBreak }` computed from the same selection/history state the render
   functions use today. Implement by refactoring each `render*Tools()` into a
   catalogue-builder + a generic desktop renderer over it (desktop look
   unchanged - same buttons, same order, same tooltips from a `tip` field).
   Plugin buttons (`renderPluginButtons`) contribute entries the same way
   (`PluginContext` already carries label + callback; icon defaults to a
   generic puzzle glyph until plugins declare one).
2. **Rail rendering:** vertical scroll of `railButton`s from the catalogue;
   the mockup's 7 (Sketch, Push, Extrude, Shell, Fillet, Move, Rotate) are
   just the body-context catalogue. Overflow beyond ~8 entries scrolls;
   dropdown-style entries (Primitives…, Add Plane…) open as popups anchored
   right of the rail.
3. **Sketch mode:** the rail swaps to the sketch catalogue (Line, Circle,
   Rect, Arc, …, active-tool highlight from `setActiveSketchMode`), and the
   top bar grows a green **Finish** / red **Discard** pair (the two actions
   that must never be hunted for). Constraint buttons - selection-dependent
   and numerous - go into a "Constrain ▸" rail popup, not the rail itself.
4. **FULL pill** (bottom-left, always visible): cycles
   normal → viewport-only (rail + right panel hidden) → back. **Focus** (top
   bar) = right panel hidden only. Both persist (`leftPanelHidden` /
   `rightPanelHidden` reused - same semantics as the desktop collapse
   chevrons at `Application.cpp:1313`).
5. **Op parameter popups** (Extrude distance, fillet radius, dimension
   input): already floating windows (`##ExtrudeInput` etc. in
   `Application_Viewport.cpp`) - restyle via the kit inside the shell and
   verify they clear the rail/panel and the on-screen keyboard.

**Exit criteria:** full modeling loop on the rail - sketch → constrain →
finish → extrude → fillet → move - without ever needing the desktop UI;
desktop toolbar pixel-unchanged.

## Phase 4 - Polish + mobile integration

- Viewport chip (bottom-center, auto-hides): active tool + selection summary
  (the StatusBar info worth keeping in touch land).
- Toasts reposition below the top bar; DimensionInput anchors above the
  keyboard (mobileShow/HideTextInput hooks already exist).
- Defaults: `imTouchUi = true` on `MZ_MOBILE` first-run once Phase 3 is
  stable (keep `false` until then); Settings toggle stays for tablets with
  keyboards.
- Orientation/resize reflow (Stage Manager later; safe areas already
  handled), theme-toggle correctness, Android back button = close popup.

## Phase 5 - QA + docs

- All four CI builds green; manual pass on Linux desktop (toggle on/off),
  Android tablet, iPad.
- Screenshots for README/metadata; changelog entry; `docs/` update.

## Risks / open questions

| Risk | Mitigation |
|---|---|
| `Toolbar` catalogue refactor destabilizes the desktop toolbar | Real risk, and it did bite: under a FACE selection `renderBodyTools` runs as a fall-through for the Transform row only, where the catalogue is full of face tools - the remainder net re-rendered every one. Caught on the headless rig, fixed with a `primaryContext` flag. Verify each selection context visually. |
| Two shells drift over time | Shared catalogue + shared menu-item lists + shared panel content are the drift firewalls; shell code is layout-only |
| ImGui fixed windows + saved imgui.ini interact badly | Touch shell windows use `##` names not present in the ini and `NoSavedSettings`; dockspace never renders while active |
| Icon licensing | Lucide is ISC; add to FONT-CREDITS.md + licenses/ like existing fonts |
| Per-row ⋯ popups on touch (fat-finger) | 44pt rows via kit; popups sized by the same kit |
| Focus/FULL state confusion with existing panel-hide settings | Reuse the same two bools so desktop and touch agree on what's hidden |

## Order & effort (single dev)

| Phase | Effort | Lands as |
|---|---|---|
| 0 setting + skeleton | 0.5–1 d | PR 1 |
| 1 theme + icons + kit | 1–2 d | PR 1 or 2 |
| 2 top bar + right panel | 2–3 d | PR 2 (needs panel refactor) |
| 3 tool rail + catalogue | 3–5 d | PR 3 (the big one) |
| 4 polish + defaults | 2–3 d | PR 4 |
| 5 QA + docs | 1–2 d | PR 4 |

**Total ≈ 2–3 working weeks.** Phases 0–2 are low-risk and immediately
demoable on the iPad; Phase 3 is where the design decisions live.
