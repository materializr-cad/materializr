# Sketch Offset Tool Implementation Plan

> **STATUS: implemented 2026-08-31.** All eight tasks done, plus two follow-ups
> Steve asked for after using it: a commit-feedback fix (the offset was landing
> correctly but read as "nothing happened" - see the spec) and SPLINE SUPPORT,
> which the original plan deferred. 39 tests in `test_sketch_offset` green on
> the build VM, and the whole flow rig-verified end to end. Deviations found during
> implementation are recorded in the spec, marked **[revised]** - the three
> that matter: polygons need no special case (their edges are already real
> `SketchLine`s), the construction-geometry checkbox was dropped (nothing in
> the app can set `isConstruction`, so the offset would create geometry the
> user could never un-mark), and the commit is routed through an
> `offsetReadyToCommit()` flag the app drains rather than mutating from inside
> the tool.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An Onshape-style Offset tool in sketch mode: hover an entity, the connected chain highlights, click to accept, then drag or type a distance to lay a parallel copy. Output is ordinary sketch lines/arcs/circles.

**Architecture:** A new self-contained geometry module `src/modeling/SketchOffset.{h,cpp}` (chain walk → offset → corner fix-up → prune → apply) split plan/apply exactly like `planTrim`/`applyTrim`, so the hover ghost and the commit make the identical decision. A new `SketchToolMode::Offset` two-phase state machine drives it, a GL ghost renders it, and a small ImGui panel modelled on `renderMirrorToolPanel` carries the distance, side, corner style and construction toggle.

**Tech Stack:** C++17, ImGui, glm, OCCT (sketch plane only), GoogleTest + ctest, Unix Makefiles build in `build/`.

**Spec:** `docs/superpowers/specs/2026-08-31-sketch-offset-tool-design.md` - read it first. It carries the chain rules, the sign convention, the corner rules and the validity invariant.

## Global Constraints

- `SketchToolMode` is **APPEND ONLY** - the header says so and the toolbar hardcodes the raw enum indices. `Offset` goes after `Airfoil`, making it **index 14**. Nothing is reordered.
- `ToolAction` gains `SketchOffset`; append it to the sketch group next to `SketchDimension`. It is a UI-only enum (never serialized), but the layouts switch on it.
- **TWO core source lists.** `SketchOffset.cpp` must be added to BOTH `CMakeLists.txt` (root, near line 325) and `tests/CMakeLists.txt` (the `materializr_core` target). Forgetting the second one is a link error in the test suite only.
- **`addArc` sweeps CCW from start to end.** Emit corner arcs with the endpoints ordered so the sweep is CCW; a clockwise corner needs the endpoints swapped. The hairpin case (sweep > 180 degrees) must be tested - see spec test 10.
- `make materializr` skips the test binaries; run a **full** `cmake --build build -j8` before `ctest`.
- **ctest must run UNSANDBOXED** - sandboxed runs false-fail several file-IO suites on /tmp writes (project memory).
- Heavy builds and the full ctest sweep run **on the VM** (`ssh kevin@192.168.1.110`), not on the desktop. After `rsync -a` onto the VM tree, `touch` the changed files or make keeps stale `.o`.
- **Never kill the running app** to test; deploy onto `/home/kevin/AppImages/materializr.appimage` directly.
- Commit messages follow the repo's style (short subject, then a body explaining WHY) and end with the standard `Co-Authored-By: Claude Opus 5 (1M context)` trailer - 957 of 959 commits carry it. (An earlier draft of this plan claimed the opposite; it was wrong.)
- All new user-facing strings go through `materializr::tr(...)`.

---

### Task 1: Chain walk + plan struct (geometry core, no offsetting yet)

**Files:**
- Create: `src/modeling/SketchOffset.h`, `src/modeling/SketchOffset.cpp`
- Create: `tests/test_sketch_offset.cpp`
- Modify: `CMakeLists.txt` (root), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Sketch::getPoints/getLines/getCircles/getArcs/getSplines/getPolygons`, `Sketch::getPoint(int)`.
- Produces:
  ```cpp
  namespace materializr {
  // One source primitive along the walked chain, in travel order.
  struct OffsetSeg {
      enum class Kind { Line, Arc, Circle };
      Kind kind;
      int  sourceId;          // sketch element id this came from
      glm::vec2 p0{0}, p1{0}; // Line: endpoints. Arc: start/end points.
      glm::vec2 c{0};         // Arc/Circle: centre
      float r = 0.0f;         // Arc/Circle: radius
      float a0 = 0.0f;        // Arc: start angle
      float sweep = 0.0f;     // Arc: SIGNED sweep (CCW positive)
  };
  struct OffsetChain {
      std::vector<OffsetSeg> segs;  // ordered, consistently oriented
      bool closed = false;
      bool valid() const { return !segs.empty(); }
  };
  // Resolve the chain containing the element under `pos`. Empty when nothing
  // was hit or the hit element is not offsettable (spline / fromText).
  OffsetChain walkOffsetChain(const Sketch& sk, glm::vec2 pos, float threshold);
  // Densified polyline of the chain, for the hover highlight.
  void densifyChain(const OffsetChain& ch, std::vector<glm::vec2>& out);
  }
  ```
- `walkOffsetChain` needs its own hit test - `pickSketchElement` is `static` inside `SketchTool.cpp` and is not being extracted (no `test_sketch_trim` exists to protect that refactor). Write a small local `pickOffsettable()` in `SketchOffset.cpp`.

- [x] **Step 1: Write the failing tests** - spec test 9 only (T-junction stops at the branch; closed square detected with `closed == true` and 4 segs in travel order; lone line yields 1 seg, `closed == false`; picking a spline yields an empty chain; a `SketchPolygon` yields its `lineIds` as a closed chain).
- [x] **Step 2: Implement** the adjacency map (point id → element ids), the bidirectional walk with the degree-2 rule, and orientation flipping so each seg's `p0` is the entry end.
- [x] **Step 3: Wire the build** - add `SketchOffset.cpp` to both source lists, add the `test_sketch_offset` executable + `add_test` in `tests/CMakeLists.txt` following the `test_sketch_regions` pattern (line 406).
- [x] **Step 4: Verify** - `cmake --build build -j8 && ctest --test-dir build -R test_sketch_offset --output-on-failure` (unsandboxed).

---

### Task 2: Offset the segments and fix up the corners

**Files:**
- Modify: `src/modeling/SketchOffset.{h,cpp}`, `tests/test_sketch_offset.cpp`

**Interfaces:**
```cpp
enum class OffsetCorners { Round, Sharp };
struct OffsetResult {
    std::vector<OffsetSeg> segs;  // the offset geometry, same Kind vocabulary
    bool valid = false;
    const char* rejectReason = nullptr;  // set when !valid
};
// d is SIGNED: positive = right of travel. corners chooses the opening-corner
// style; a Sharp miter with no intersection silently falls back to Round.
OffsetResult offsetChain(const OffsetChain& ch, float d, OffsetCorners corners);
```

- [x] **Step 1: Write the failing tests** - spec tests 1, 2, 5, 6, 7 and 10, plus the shared invariant helper `EXPECT_ON_OFFSET(result, chain, d)` that samples every output seg and asserts `distanceToChain == |d|` within `max(1e-4, 1e-3*|d|)`. **Write the helper first; every later task reuses it.**
- [x] **Step 2: Implement** per-segment offsetting (line → parallel line; arc → concentric arc at `r + d*sign(sweep_side)`; circle → `r + d`, refusing `r + d <= 0`).
- [x] **Step 3: Implement** the corner classification (tangent join / opening / closing) and the three responses from the spec. Local `intersectLineLine` / `intersectLineCircle` / `intersectCircleCircle` helpers live in this TU - do not touch the `static` copies in `SketchTool.cpp`.
- [x] **Step 4: Verify** the CCW ordering rule on the hairpin case explicitly (test 10) before moving on.

---

### Task 3: Prune, validity, and apply to the sketch

**Files:**
- Modify: `src/modeling/SketchOffset.{h,cpp}`, `tests/test_sketch_offset.cpp`

**Interfaces:**
```cpp
float distanceToChain(const OffsetChain& ch, glm::vec2 p);
// Drop / split segments closer than |d| - eps to the source. Clears `valid`
// and sets `rejectReason` when nothing survives.
void pruneOffset(OffsetResult& res, const OffsetChain& src, float d);
// Commit. Welds coincident endpoints via `weld` (SketchTool::coincidentPoint
// is passed in as a callback so this module stays free of SketchTool).
void applyOffset(Sketch& sk, const OffsetResult& res, bool construction,
                 const std::function<int(glm::vec2)>& weld,
                 std::set<int>& outPoints, std::set<int>& outElements);
```

- [x] **Step 1: Write the failing tests** - spec tests 3, 4, 8 and 11.
- [x] **Step 2: Implement** `distanceToChain` (per-primitive, per the spec), the sample-and-drop pass, and the binary-search split for partially-invalid segments.
- [x] **Step 3: Implement** `applyOffset`, honouring the `addArc` CCW rule and setting `isConstruction`.
- [x] **Step 4: Verify** - the full `test_sketch_offset` suite green, and the invariant helper asserting on every case.

**This is the end of the headless work. Everything above is testable with no GL, no window and no app.**

---

### Task 4: `SketchToolMode::Offset` state machine

**Files:**
- Modify: `src/modeling/SketchTool.h`, `src/modeling/SketchTool.cpp`

**Interfaces:**
- `enum class SketchToolMode` gains `Offset` **at the end** (index 14).
- New public surface, modelled on the Mirror block (`SketchTool.h` lines 206–229):
  ```cpp
  enum class OffsetPhase { Pick, Distance };
  OffsetPhase getOffsetPhase() const;
  bool  hasOffsetChain() const;
  float getOffsetDistance() const;          // signed
  void  setOffsetDistance(float d);         // panel / typed value
  void  flipOffsetSide();
  OffsetCorners getOffsetCorners() const;
  void  setOffsetCorners(OffsetCorners c);
  bool  getOffsetConstruction() const;
  void  setOffsetConstruction(bool b);
  const std::vector<glm::vec2>& getOffsetChainHover() const;   // Pick phase highlight
  const std::vector<glm::vec2>& getOffsetPreview() const;      // Distance phase ghost
  void  commitOffset(std::set<int>& outPoints, std::set<int>& outElements);
  void  cancelOffset();
  ```

- [x] **Step 1:** Append the enum value; add `handleOffsetTool(glm::vec2)` to the `onMouseDown` switch (`SketchTool.cpp` ~line 102) and an Offset branch to `onMouseMove` (~line 155). Offset uses the **raw** cursor for picking, like Trim, and the **snapped** cursor in the Distance phase so grid snap quantises the distance.
- [x] **Step 2:** Pick phase - recompute `walkOffsetChain` + `densifyChain` on move into `m_offsetChainHover`; click captures the chain.
- [x] **Step 3:** Distance phase - derive signed distance from the cursor per the spec, run `offsetChain` + `pruneOffset` each move, densify into `m_offsetPreview`. Set `m_offsetReject` from `rejectReason` for the app to toast.
- [x] **Step 4:** Add an Offset branch to `applyDimension(float)` so a typed value commits at that magnitude on the current side.
- [x] **Step 5:** `onCancel()` - Distance phase returns to Pick; Pick phase clears the tool. Commit returns to Pick and leaves the selection alone (unlike Mirror).
- [x] **Step 6: Verify** - build the app target only; there is no UI yet to click.

---

### Task 5: Ghost rendering

**Files:**
- Modify: `src/viewport/SketchRenderer.{h,cpp}`

- [x] **Step 1:** Add `drawOffsetPreview(const Sketch*, const SketchTool*, const glm::mat4&)` next to `drawTrimHover` (`SketchRenderer.cpp` line 614) - the same 2D-points → `GL_LINES` stream. Two colours: the Pick-phase chain highlight and the Distance-phase result ghost.
- [x] **Step 2:** Call it from the same place `drawTrimHover` is called (line 444).
- [x] **Step 3: Verify** on the headless rig (Xvfb :99 + fakehome, see project memory) - hover a rectangle, confirm the whole loop highlights; click and drag, confirm the ghost tracks and flips sides across the profile. **End the rig task with the `pgrep` cleanup.**

---

### Task 6: Offset panel

**Files:**
- Modify: `src/app/Application_Dialogs.cpp`, `src/app/Application.h`, and wherever `renderMirrorToolPanel()` is called each frame.

- [x] **Step 1:** Add `renderOffsetToolPanel()` immediately after `renderMirrorToolPanel()` (`Application_Dialogs.cpp` line 4254), copying its structure: early-out on mode, `AlwaysAutoResize | NoSavedSettings`, a `bool open` close-box that cancels.
- [x] **Step 2:** Contents - a distance `NumField` (`src/ui/NumField.h`), a **Flip** button, a Round/Sharp corner combo, a "Construction geometry" checkbox, and Offset / Cancel. Greyed-out Offset with the reject reason as a tooltip when the plan is invalid.
- [x] **Step 3:** Commit path - `recordSketchMutation([&]{ m_sketchTool->commitOffset(pts, els); })`, then `markDirty(); m_meshesDirty = true;`. One undo step.
- [x] **Step 4:** Drain `consumeOffsetRejection()` each frame into a toast, matching how the Dimension tool's rejection is surfaced.
- [x] **Step 5: Verify** on the rig - offset in and out on a rectangle, a circle and an L chain; undo returns to exactly the pre-offset sketch.

---

### Task 7: Toolbar and layout wiring

**Files:**
- Modify: `src/ui/Toolbar.h` (`ToolAction`), `src/ui/Toolbar.cpp` (two lists), `src/ui/TouchIcons.h`, `src/app/Application.cpp` (action dispatch), `src/app/layout/imtouch/ImTouchLayout.cpp`

- [x] **Step 1:** `ToolAction::SketchOffset` appended to the sketch group in `Toolbar.h`.
- [x] **Step 2:** **Both** toolbar lists - the rail `add(...)` block (`Toolbar.cpp` ~line 193, next to Trim, `m_activeSketchMode == 14`) and the classic `skBtn("Offset", 14)` list (~line 747). Sketch tools are still listed separately in both; the shared `railTools()` catalogue excludes sketch mode.
- [x] **Step 3:** Icon - `MZ_ICON_OFFSET`. Iconoir has no glyph that reads as "offset"; either reuse `ICON_IC_EXPAND` or add a PUA sentinel drawn as two nested rounded rectangles in `drawIconCentered` (`TouchWidgets.cpp`), following the existing `MZ_ICON_UNFOLD` / `MZ_ICON_PATTERN_*` precedent. **Prefer the sentinel** - the nested-shape reading is the one that makes the tool obvious.
- [x] **Step 4:** Dispatch - `case ToolAction::SketchOffset: if (m_inSketchMode) toggleSketchMode(m_sketchTool.get(), SketchToolMode::Offset); break;` in `Application.cpp` next to the Trim case (~line 2236).
- [x] **Step 5:** im-touch grouping - add `ToolAction::SketchOffset` to the `modify` bucket in `ImTouchLayout.cpp` (~line 864), alongside Trim/Copy/Mirror/patterns. Without this it falls through to `default:` and lands flat in the rail.
- [x] **Step 6: Verify** all three layouts (classic / modern / im-touch) show the button and highlight it when active. The rig defaults to classic - switch layouts explicitly to check the other two.

---

### Task 8: Docs and final sweep

**Files:**
- Modify: `src/ui/HelpPanel.cpp` (line 51 tool list), `src/ui/ShortcutsPanel.cpp` (line 126 wording)

- [x] **Step 1:** Add Offset to the in-app tool lists. Do **not** invent a keyboard shortcut - no sketch-tool letter bindings exist beyond `d`; the shortcuts panel already says these tools are toolbar-only, and that panel has a history of listing bindings that do not exist.
- [x] **Step 2:** Full build + full ctest on the VM, unsandboxed, `-j32`. Confirm no regression in `test_sketch_regions`, `test_sketch_edit_cascade`, `test_full_replay`.
- [x] **Step 3:** Rig sweep - offset a real profile, extrude both the source and the offset, save, reload, confirm the offset geometry round-trips (it is plain sketch elements, so this should be uneventful; verify rather than assume).
- [x] **Step 4:** Commit. Batch the push - Actions artifact storage is tight.

---

## Effort

| Task | Estimate |
|---|---|
| 1 - chain walk | 2 h |
| 2 - offset + corners | 4 h |
| 3 - prune + apply | 3 h |
| 4 - tool state machine | 2 h |
| 5 - rendering | 1 h |
| 6 - panel | 2 h |
| 7 - toolbar wiring | 1.5 h |
| 8 - docs + sweep | 1.5 h |

Roughly **two days**, with the geometry core (tasks 1–3) the bulk of it and fully testable headlessly before any UI exists.

## Follow-ups (not in scope)

Spline offset was in this list and is now DONE (see the spec's "Splines"
section). What remains:

- Offsetting text glyph loops ("embolden").
- Live/associative offset - needs derived geometry in `SketchSolver`.
- An "offset equals an existing dimension" inference.
- Auto-adding Parallel/Equal constraints between source and offset.
