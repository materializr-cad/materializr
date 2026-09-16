# Sketch Polygon Trim Pick-Priority Fix - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans or superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clicking Trim on a polygon's edge must delete the whole polygon (as documented at
`SketchTool.cpp:2963-2964`), not silently partial-trim the underlying `SketchLine` and corrupt
the polygon's bookkeeping.

**Root cause (confirmed by reading, not assumed):**

1. `Sketch::addPolygon` (`Sketch.cpp:495-535`) creates each polygon edge as a real `SketchLine`
   via `addLine`, recorded in both `sketch.getLines()` AND `SketchPolygon::lineIds`.
2. `pickSketchElement` (`SketchTool.cpp:3075-3144`) runs a **Lines** loop before a **Polygons**
   loop. Both loops call `distSqPointSegment` on the identical two endpoint positions for a
   polygon edge, producing an identical `dsq`. The Polygons loop's `dsq < bestDistSq` is a tie
   (never strictly true) because the Lines loop already claimed that exact distance, so
   `pickType` always resolves to `"line"` for a polygon edge - `planTrim`'s
   `pickType == "polygon"` branch (`SketchTool.cpp:3262`, `TrimAction::Kind::FullDelete`) is
   dead code for direct clicks.
3. Because `pickType == "line"`, `planTrim`'s Line branch (`SketchTool.cpp:3267-3310`) plans a
   partial trim (or full delete) of that one `SketchLine` by id. `applyTrim`'s Line branch
   (`SketchTool.cpp:3516-3528`) calls `sketch.removeElement(lineId)` and re-adds the surviving
   sub-segment(s) as new freestanding lines - never touching `SketchPolygon::lineIds`. The
   polygon record survives in `Sketch::m_polygons` with one `lineIds` entry now dangling
   (pointing at a removed line id) and, in the multi-intersection case, a piece of its own
   perimeter silently replaced by an unrelated freestanding line.
4. Second, independent bug found while tracing the fix (not previously reported): even once
   `pickType == "polygon"` is reachable, `applyTrim`'s FullDelete branch
   (`SketchTool.cpp:3511-3513`) just calls `sketch.removeElement(polygonId)`, and
   `Sketch::removeElement` (`Sketch.cpp:690-720`) only erases the matching id from
   `m_polygons` - it does **not** cascade to the polygon's `lineIds`. Per
   `SketchRenderer.cpp:576` ("Polygons are already made of lines, so they render automatically
   through drawLines()"), the renderer draws every `SketchLine` in `m_lines` regardless of
   polygon ownership. So "delete a polygon" would remove the `SketchPolygon` bookkeeping record
   but leave all N edge lines rendering forever - the polygon visually never disappears. This
   must be fixed in the same change, or fixing bug #2/#3 just trades one corruption for another
   user-visible one.

**Fix, two parts:**

- **A - pick priority:** in `pickSketchElement`, build the set of every polygon's `lineIds`
  once, and skip those ids in the Lines loop (`continue`) so a polygon edge can only ever be
  claimed by the Polygons loop. Chosen over reordering the two loops (checking Polygons before
  Lines) because reordering only fixes this by relying on the two `distSqPointSegment` call
  sites producing a bit-exact tie - true today (no `-ffast-math`/`-ffp-contract=fast` in this
  project's `CMakeLists.txt`, confirmed by grep) but an undocumented invariant a future compiler
  flag or refactor could silently break. Explicit dedup encodes the actual intent and has no FP
  dependence.
- **B - delete cascade:** in `Sketch::removeElement`, when `id` matches a `SketchPolygon`,
  also erase every id in that polygon's `lineIds` from `m_lines` (in addition to erasing the
  polygon record). This is the single place both `SketchTool::handleTrimTool`'s FullDelete path
  and any future "delete this polygon by id" caller go through, and it matches the class's
  existing contract of fully removing an element - `pruneOrphanPoints()` (already called by
  every existing `removeElement` caller) then sweeps the now-unreferenced vertex/center points,
  so no change needed there.

**Non-goals:** splines are unaffected (spline curve points are never stored as standalone
`SketchLine`/`SketchPoint` entries shared with another pick type) and are out of scope. No
change to `TrimAction`, `planTrim`'s dispatch, or `collectLineIntersections`.

**Tech Stack:** C++17, GoogleTest, existing `Sketch`/`SketchTool` classes. `tests/CMakeLists.txt`
registers each test executable explicitly (no glob) - the new test file needs its own
`add_executable`/`target_link_libraries`/`add_test` block, mirroring `test_sticky_rim`'s
(`tests/CMakeLists.txt:463-465`).

**Spec:** This plan document; bug originally reported against
`src/modeling/SketchTool.cpp:3075` pick-priority behavior.

## Global Constraints

- Do not change `TrimAction::Kind` values, `planTrim`'s branch structure, or any public
  `SketchTool` method signature - only the body of `pickSketchElement` (part A) and
  `Sketch::removeElement` (part B).
- `pruneOrphanPoints()`'s existing "used" scan already treats a still-present
  `SketchPolygon::vertexPointIds`/`centerPointId` as used and drops points no longer referenced
  once the polygon record is gone - verify this stays true after part B (it does; part B removes
  the polygon record and its lines in the same `removeElement` call, before
  `handleTrimTool`'s subsequent `pruneOrphanPoints()` call runs).
- No em dashes, in any spelling (the literal character or its UTF-8 escape), in any new code
  comment or commit message - this repo gates on `tools/no_em_dashes.py`. Fix silently if
  introduced; do not narrate the cleanup.
- Run `ctest` UNSANDBOXED after the build (project memory: sandboxed ctest fails unrelated
  file-IO suites on denied `/tmp` writes and looks like a regression when it is not).
- `rm` stale test object files before rebuilding after any header/source edit inside a fast
  edit-build cycle (project memory: `touch` does not reliably invalidate `make`'s "current"
  object within the same second).
- No `Co-Authored-By: Claude` trailer on any commit in this repo (existing project convention).

---

### Task 1: Fix `pickSketchElement` pick priority (dedup polygon-owned lines)

**Files:**
- Modify: `src/modeling/SketchTool.cpp` (`pickSketchElement`, ~line 3075-3144)

**Interfaces:** No signature change; internal behavior only.

- [x] **Step 1: Build the polygon-owned-line-id set and skip those ids in the Lines loop**

  In `pickSketchElement`, before the `// Lines` loop, add:

  ```cpp
  std::unordered_set<int> polygonLineIds;
  for (const auto& po : sketch.getPolygons())
      for (int lid : po.lineIds) polygonLineIds.insert(lid);
  ```

  In the `// Lines` loop, skip ids owned by a polygon:

  ```cpp
  for (const auto& ln : sketch.getLines()) {
      if (polygonLineIds.count(ln.id)) continue;
      const SketchPoint* a = sketch.getPoint(ln.startPointId);
      ...
  ```

  Add `#include <unordered_set>` to `SketchTool.cpp` if not already present (check first - 
  the file already includes several STL containers).

  A plain standalone line (not part of any polygon) is unaffected: it is never in
  `polygonLineIds`, so the Lines loop still claims it exactly as before, and the Polygons loop
  (unchanged) still only fires for actual polygon edges.

- [x] **Step 2: Verify** - `grep -n "no_em_dashes\|#include <unordered_set>" src/modeling/SketchTool.cpp`
  to confirm the include is present and no em dash was introduced in the new comment/code.

### Task 2: Cascade-delete a polygon's owned lines in `Sketch::removeElement`

**Files:**
- Modify: `src/modeling/Sketch.cpp` (`removeElement`, ~line 690-720)

**Interfaces:** No signature change (`void Sketch::removeElement(int id)`).

- [x] **Step 1: Cascade polygon → lines before erasing the polygon record**

  In `removeElement`, before the existing `m_polygons.erase(...)` block, collect the line ids
  owned by any polygon matching `id` and erase them from `m_lines` too:

  ```cpp
  for (const auto& p : m_polygons) {
      if (p.id != id) continue;
      for (int lid : p.lineIds) {
          m_lines.erase(
              std::remove_if(m_lines.begin(), m_lines.end(),
                  [lid](const SketchLine& l) { return l.id == lid; }),
              m_lines.end());
      }
      break;
  }
  ```

  Placement: after the existing `m_lines.erase(...)` block that matches `id` directly (so a
  plain line-by-id removal is untouched) and before `m_polygons.erase(...)`. The existing
  top-of-function `m_lines.erase` for `id` itself is a no-op here since `id` is a polygon id,
  not a line id - this new block is what actually removes the polygon's N edges.

  Update the doc comment above `removeElement` in `Sketch.h:142-143` if one is added elsewhere
  in this pass; the existing `pruneOrphanPoints` comment at `Sketch.h:147-148` ("removeElement
  deliberately does NOT prune [points]") stays accurate and does not need editing - this change
  only affects line cascade, not point pruning.

- [x] **Step 2: Verify** - re-read the edited `removeElement` to confirm ordering (lines cascade
  before `m_polygons.erase`, since the loop reads `p.lineIds` from the still-present record).

### Task 3: Regression test

**Files:**
- Create: `tests/test_sketch_trim_polygon.cpp`
- Modify: `tests/CMakeLists.txt` (add the explicit registration block, no glob exists)

**Interfaces:** Test-only. `handleTrimTool`/`computeTrimHover` (`SketchTool.h:631-632`) turned out
to be **private** on inspection during implementation (this plan's original draft wrongly assumed
public, which would not have compiled) - drive Trim through the public dispatch instead:
`SketchTool::setMode(SketchToolMode::Trim)` then `onMouseDown`/`onMouseMove`, which call the
private methods internally with the same raw, unsnapped cursor (`SketchTool.cpp:127-128,
231-232`), so it is behaviorally identical. Also uses `Sketch::getPolygons()`/`getLines()`/
`getPoints()`/`getConstraints()`/`addRawConstraint()`.

- [x] **Step 1: Write the test file**

  Follow the wiring pattern from `tests/test_sticky_rim.cpp` (stub the two link-time
  dependencies `SvgImport::place`/`TextSketch::generate` that `SketchTool.cpp` references but
  are not part of this test's link set) and `tests/test_sketch_offset.cpp` (direct `Sketch`
  construction, no OCCT/document/viewport involved - `Sketch`/`SketchTool` are pure 2D-geometry
  classes).

  Confirmed by reading `tests/CMakeLists.txt:463-465`: this repo registers each test
  executable explicitly (`add_executable`/`target_link_libraries`/`add_test`), it does not glob
  `test_*.cpp` or use `gtest_discover_tests`. `ctest -R <name>` matches the **executable name**
  passed to `add_test`, not individual `TEST(...)` suite names inside it.

  ```cpp
  #include <gtest/gtest.h>

  #include "modeling/Sketch.h"
  #include "modeling/SketchConstraints.h"
  #include "modeling/SketchSolver.h"
  #include "modeling/SketchTool.h"
  #include "modeling/SvgImport.h"
  #include "modeling/TextSketchOp.h"

  #include <glm/glm.hpp>

  namespace materializr {
  int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
  int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                           glm::vec2, float, float) { return 0; }
  }

  using materializr::Constraint;
  using materializr::ConstraintType;
  using materializr::Sketch;
  using materializr::SketchPolygon;
  using materializr::SketchSolver;
  using materializr::SketchTool;
  using materializr::SketchToolMode;

  namespace {
  constexpr float kStep = 1.0f;
  constexpr float kTol = 1e-4f;

  // handleTrimTool/computeTrimHover are private; drive them the same way the
  // app does, through the public Trim-mode dispatch in onMouseDown/onMouseMove
  // (SketchTool.cpp:127-128, 231-232). Trim uses the raw, unsnapped cursor in
  // both, so this is exactly equivalent to calling the private methods.
  SketchTool makeTrimTool(Sketch& sk, SketchSolver& solver) {
      SketchTool tool;
      tool.setSketch(&sk);
      tool.setSolver(&solver);
      tool.setGridStep(kStep);
      tool.setMode(SketchToolMode::Trim);
      return tool;
  }

  void clickTrim(SketchTool& tool, glm::vec2 pos) { tool.onMouseDown(pos); }
  void hoverTrim(SketchTool& tool, glm::vec2 pos) { tool.onMouseMove(pos); }

  const SketchPolygon* findPolygon(const Sketch& sk, int id) {
      for (const auto& p : sk.getPolygons()) if (p.id == id) return &p;
      return nullptr;
  }

  bool lineExists(const Sketch& sk, int id) {
      for (const auto& l : sk.getLines()) if (l.id == id) return true;
      return false;
  }

  // True if some line's endpoints match (p1,p2) in either direction, within kTol.
  bool lineWithEndpointsExists(const Sketch& sk, glm::vec2 p1, glm::vec2 p2) {
      for (const auto& l : sk.getLines()) {
          const auto* a = sk.getPoint(l.startPointId);
          const auto* b = sk.getPoint(l.endPointId);
          if (!a || !b) continue;
          bool forward = glm::length(a->pos - p1) < kTol && glm::length(b->pos - p2) < kTol;
          bool reverse = glm::length(a->pos - p2) < kTol && glm::length(b->pos - p1) < kTol;
          if (forward || reverse) return true;
      }
      return false;
  }
  } // namespace

  // Clicking Trim on a polygon edge must delete the whole polygon (not
  // partial-trim the underlying SketchLine), cascade-remove exactly its own
  // owned lines and a dangling constraint on one of them, and leave unrelated
  // geometry (a separate standalone line sharing no points with the polygon)
  // completely untouched.
  TEST(SketchTrimPolygon, ClickOnEdgeDeletesWholePolygonCascadeOnly) {
      Sketch sk;
      SketchSolver solver;
      SketchTool tool = makeTrimTool(sk, solver);

      int centerId = sk.addPoint({0.0f, 0.0f});
      int polyId = sk.addPolygon(centerId, /*radius=*/10.0, /*sides=*/6, /*rotationRad=*/0.0);
      ASSERT_GE(polyId, 0);

      const SketchPolygon* poly = findPolygon(sk, polyId);
      ASSERT_NE(poly, nullptr);
      std::vector<int> ownedLineIds = poly->lineIds;
      ASSERT_FALSE(ownedLineIds.empty());

      // A dangling constraint on one of the polygon's own edges must be
      // pruned along with it (Sketch::pruneOrphanPoints already drops any
      // constraint whose entity id is no longer valid; this proves the
      // removeElement cascade actually invalidates that id).
      Constraint horiz;
      horiz.id = 999001;
      horiz.type = ConstraintType::Horizontal;
      horiz.entityA = ownedLineIds[0];
      sk.addRawConstraint(horiz);
      ASSERT_EQ(sk.getConstraints().size(), 1u);

      // Unrelated geometry: a standalone line far away, sharing no points
      // with the polygon at all. Must survive completely unchanged.
      int farP1 = sk.addPoint({100.0f, 0.0f});
      int farP2 = sk.addPoint({110.0f, 0.0f});
      int farLineId = sk.addLine(farP1, farP2);

      const size_t linesBefore = sk.getLines().size();

      // Midpoint of the first edge.
      const auto* a = sk.getPoint(poly->vertexPointIds[0]);
      const auto* b = sk.getPoint(poly->vertexPointIds[1]);
      glm::vec2 mid = (a->pos + b->pos) * 0.5f;

      clickTrim(tool, mid);

      EXPECT_EQ(findPolygon(sk, polyId), nullptr);
      for (int lid : ownedLineIds) {
          EXPECT_FALSE(lineExists(sk, lid)) << "polygon edge line " << lid << " survived FullDelete";
      }
      // Exactly the polygon's own lines disappeared - nothing else.
      EXPECT_EQ(sk.getLines().size(), linesBefore - ownedLineIds.size());

      EXPECT_TRUE(sk.getConstraints().empty())
          << "constraint on a deleted polygon edge was not pruned";

      ASSERT_TRUE(lineExists(sk, farLineId));
      const auto* fp1 = sk.getPoint(farP1);
      const auto* fp2 = sk.getPoint(farP2);
      ASSERT_NE(fp1, nullptr);
      ASSERT_NE(fp2, nullptr);
      EXPECT_NEAR(fp1->pos.x, 100.0f, kTol);
      EXPECT_NEAR(fp2->pos.x, 110.0f, kTol);
  }

  // A standalone line (not part of any polygon) still partial-trims normally
  // - guards against the pick-priority fix over-broadly excluding real lines.
  // Asserts the exact surviving geometry, not just "something remains".
  TEST(SketchTrimPolygon, StandaloneLineStillPartialTrimsExactly) {
      Sketch sk;
      SketchSolver solver;
      SketchTool tool = makeTrimTool(sk, solver);

      glm::vec2 leftEnd{-10.0f, 0.0f};
      glm::vec2 origin{0.0f, 0.0f};
      glm::vec2 rightEnd{10.0f, 0.0f};
      int p1 = sk.addPoint(leftEnd);
      int p2 = sk.addPoint(rightEnd);
      int lineId = sk.addLine(p1, p2);

      // A crossing line at x=0 gives the trim an intersection to split at.
      glm::vec2 crossTop{0.0f, 10.0f}, crossBottom{0.0f, -10.0f};
      int p3 = sk.addPoint(crossBottom);
      int p4 = sk.addPoint(crossTop);
      int crossId = sk.addLine(p3, p4);

      clickTrim(tool, {-5.0f, 0.0f}); // left half of the horizontal line

      EXPECT_FALSE(lineExists(sk, lineId));
      // Exactly the right half must survive: origin -> rightEnd.
      EXPECT_TRUE(lineWithEndpointsExists(sk, origin, rightEnd))
          << "surviving segment is not exactly (0,0)-(10,0)";
      // The left half must NOT survive in any form.
      EXPECT_FALSE(lineWithEndpointsExists(sk, leftEnd, origin))
          << "left half of the trimmed line was not removed";
      // The crossing line is untouched.
      ASSERT_TRUE(lineExists(sk, crossId));
      EXPECT_TRUE(lineWithEndpointsExists(sk, crossBottom, crossTop));
  }

  // Hover preview on a polygon edge outlines the full closed loop (FullDelete
  // preview), not just the one segment under the cursor.
  TEST(SketchTrimPolygon, HoverOnEdgePreviewsWholeLoop) {
      Sketch sk;
      SketchSolver solver;
      SketchTool tool = makeTrimTool(sk, solver);

      int centerId = sk.addPoint({0.0f, 0.0f});
      int polyId = sk.addPolygon(centerId, 10.0, 6, 0.0);
      const SketchPolygon* poly = findPolygon(sk, polyId);
      ASSERT_NE(poly, nullptr);

      const auto* a = sk.getPoint(poly->vertexPointIds[0]);
      const auto* b = sk.getPoint(poly->vertexPointIds[1]);
      glm::vec2 mid = (a->pos + b->pos) * 0.5f;

      hoverTrim(tool, mid);
      // FullDelete preview densifies every vertex plus a closing repeat of
      // the first (densifyTrimPreview's polygon branch, SketchTool.cpp:3447-3454).
      EXPECT_GT(tool.getTrimHoverPoints().size(), poly->vertexPointIds.size());
  }
  ```

  `getTrimHoverPoints()` is confirmed public (`SketchTool.h:463`). `onMouseDown`/`onMouseMove`/
  `setMode` (used by `clickTrim`/`hoverTrim`/`makeTrimTool` above) are confirmed public too
  (`SketchTool.h`, before the `private:` at line 524).

- [x] **Step 2: Register the test executable** - this repo does not glob test files. Add, in
  the same alphabetical/grouped spot as the other `test_sketch_*` entries in
  `tests/CMakeLists.txt` (mirroring `test_sticky_rim`'s three lines at `tests/CMakeLists.txt:463-465`):

  ```cmake
  add_executable(test_sketch_trim_polygon test_sketch_trim_polygon.cpp)
  target_link_libraries(test_sketch_trim_polygon PRIVATE materializr_core gtest gtest_main)
  add_test(NAME test_sketch_trim_polygon COMMAND test_sketch_trim_polygon)
  ```

- [x] **Step 3: Build and run** - from `build/`, rebuild and run just the new suite:
  `ctest -R '^test_sketch_trim_polygon$' --no-tests=error --output-on-failure` (UNSANDBOXED per
  project memory; the executable name is the ctest name, not the `TEST(...)` suite name;
  `--no-tests=error` makes a registration miss fail loud instead of reporting a silent pass),
  then the full suite to confirm no regressions: `ctest --output-on-failure`.

## Manual verification (optional, if a GUI check is wanted before landing)

Per project memory, GUI smoke launches are avoided by default (minimize-sandbox-bypass) and
`/review-panel` is user-invoked only. If the user wants a manual check: run the app, draw a
polygon, switch to Trim, click one edge, confirm the whole polygon disappears and no stray line
remains at that edge.
