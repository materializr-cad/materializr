# Grid Minor-Tier Extent Cache Invalidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix issue #110 (steady-state viewport FPS drops to ~8fps once ~400+ bodies are loaded, even fully idle). Root-caused via GL timer queries and wall-clock bisection (spike, not committed): `renderViewport()`'s minor-grid-tier check (`src/app/Application_Viewport.cpp:567-598`) re-walks every visible body's exact B-Rep bounding box via `BRepBndLib::Add` on a fixed 0.25s wall-clock timer, regardless of whether anything changed. On this project's 423-body scene that walk costs 95-110ms per call; because the 0.25s timer aliases against the idle-floor's ~83-183ms frame cadence, it fires roughly every other frame, producing the alternating "cheap frame / ~100ms frame" pattern that reads as ~8fps. The block's own code comment (added 2026-06-02, blame `4bbf2fd0`) was written against and measured on a 65-body project; it was never re-profiled at this scale.

**Architecture:** Replace the wall-clock *poll* with a wall-clock *rate limiter* gated by real invalidation. Add an `Application` member flag, `m_gridExtentStale`, defaulted `true` (so the first frame still computes it, matching current behavior). Set it wherever the scene's body set or visibility can actually change by hooking the single existing choke point both full and partial mesh rebuilds already pass through - `Application::rebuildMeshes()` - right after it captures whether this call was a full rebuild (`m_meshesDirty`) or a partial one (`m_dirtyBodyIds` non-empty), which are exactly the two conditions under which a body could have been added, removed, or had its visibility toggled. The grid-tier check in `renderViewport()` then recomputes the cached verdict only when `m_gridExtentStale` is true **and** the existing 0.25s cooldown has elapsed, clearing staleness only once it actually recomputes (a request that arrives mid-cooldown stays pending, it is not dropped).

The cooldown is kept specifically because `m_dirtyBodyIds` is not exclusively a "topology changed" signal - routed through the single shared `markBodyDirty()` callback, it also fires for changes that never touch the bounding box (a body rename, a body-color edit dragged live off a color wheel via `ImGui::ColorEdit3`, which can call back many times per second while held). Without the cooldown, a rapid string of such non-geometric edits would re-trigger the full `BRepBndLib::Add` scan on every one of those frames instead of at most 4 times/second, which is worse than the current behavior for that case even though it is a large win for the (far more common) truly-idle case this issue is actually about. Keeping the cooldown as a cap, with staleness deciding whether there is anything to check at all, gets both: zero cost while idle, no worse than today's cost while being edited.

**Tech Stack:** C++17, Dear ImGui, OCCT (`BRepBndLib`, `Bnd_Box`), CMake, GoogleTest/ctest.

**Spec:** This plan document (no separate spec file - issue #110 and the spike findings above are the task brief).

## Global Constraints

- Touch only the minor-grid-tier caching mechanism. Do not change the 100mm threshold, the grid rendering itself, or any other part of `renderViewport()`.
- No new heap allocations or per-frame OCCT geometry calls beyond what already exists for the "stale" case.
- `m_gridExtentStale` must default to `true` so a freshly constructed `Application` still computes the verdict on its first eligible frame (parity with the current `s_nextCheckTime = 0.0` initial-fire behavior).
- `rebuildMeshes()` is called far more often than the grid-tier check actually needs (e.g. on every partial per-body edit), which is fine - the cost moved from "unconditional every 0.25s" to "only when something that could change the bounds actually happened", and `rebuildMeshes()` runs regardless of whether the grid staleness flag existed, so this adds no new work to that function, only a bool write.
- After the change, run the full suite from `build/` UNSANDBOXED: `ctest --test-dir build --output-on-failure` (sandboxed ctest false-fails 5 file-IO suites on denied `/tmp` writes - project memory). No test count may drop and no new failures may appear.
- No `Co-Authored-By: Claude` trailer on any commit in this repo (existing project convention for materializr commits).
- This PR already has an open upstream issue (#110) and lives on its own branch - do not bundle it into `fix/step-import-freeze` (PR #111); it is unrelated to that fix.

---

### Task 1: Replace the wall-clock grid-extent throttle with change-driven invalidation

**Files:**
- Modify: `src/app/Application.h`
- Modify: `src/app/Application.cpp`
- Modify: `src/app/Application_Viewport.cpp`

**Interfaces:**
- Produces: `Application::m_gridExtentStale` (private bool member)
- Consumes: existing `Application::m_meshesDirty`, `Application::m_dirtyBodyIds`, `Application::rebuildMeshes()`

- [x] **Step 1: Add the staleness flag**

  In `src/app/Application.h`, immediately after the existing `m_dirtyBodyIds` declaration (around line 1234):

  ```cpp
  std::set<int> m_dirtyBodyIds;
  // Set whenever a rebuildMeshes() pass could have changed the visible body
  // set (full rebuild) or any body's geometry/visibility (partial rebuild) -
  // see rebuildMeshes(). Consumed by renderViewport()'s minor-grid-tier
  // extent check, which recomputes its cached verdict only when this is
  // true and clears it immediately after. Starts true so the first eligible
  // frame still computes the verdict.
  bool m_gridExtentStale = true;
  ```

- [x] **Step 2: Set the flag from the single choke point both rebuild paths pass through**

  In `src/app/Application.cpp`, inside `rebuildMeshes()` (starts at line 3520), right after the existing:

  ```cpp
      const uint32_t rmStart = m_meshesDirty ? SDL_GetTicks() : 0;
      const bool rmWasFull = m_meshesDirty;
  ```

  add:

  ```cpp
      // A full rebuild (body add/remove/reload) or any partial one (a
      // per-body edit routed through markBodyDirty, which covers visibility
      // toggles among other things) can change the scene's bounding extent -
      // see renderViewport()'s minor-grid-tier check.
      if (m_meshesDirty || !m_dirtyBodyIds.empty())
          m_gridExtentStale = true;
  ```

  This must read `m_dirtyBodyIds` before the rest of the function clears it during the partial-rebuild pass.

- [x] **Step 3: Drive the grid-tier check off the flag instead of a timer**

  In `src/app/Application_Viewport.cpp`, replace the existing block (lines 567-598):

  ```cpp
              if (!interactive) {
                  // This used to walk every visible body's bbox on every frame
                  // to decide whether the project is "big enough" to suppress
                  // the 1× minor grid. On a 65-body airplane that's ~65 OCCT
                  // bbox calls per frame; even cheap each, the cumulative
                  // baseline cost is real. We only need this threshold check
                  // to feel responsive - not to update every frame - so cache
                  // the verdict and refresh every ~0.25s. A topology change
                  // can wait that long to flip the grid tier.
                  static double s_nextCheckTime = 0.0;
                  static bool   s_hideMinor    = false;
                  double now = ImGui::GetTime();
                  if (now >= s_nextCheckTime) {
                      try {
                          Bnd_Box bb;
                          bool any = false;
                          for (int id : m_document->getAllBodyIds()) {
                              if (!m_document->isBodyVisible(id)) continue;
                              BRepBndLib::Add(m_document->getBody(id), bb);
                              any = true;
                          }
                          if (any && !bb.IsVoid()) {
                              double xmn,ymn,zmn,xmx,ymx,zmx;
                              bb.Get(xmn,ymn,zmn,xmx,ymx,zmx);
                              double ext = std::max({xmx-xmn, ymx-ymn, zmx-zmn});
                              s_hideMinor = (ext > 100.0);
                          }
                      } catch (...) {}
                      s_nextCheckTime = now + 0.25;
                  }
                  if (s_hideMinor) minorAlpha = 0.0f;
              }
  ```

  with:

  ```cpp
              if (!interactive) {
                  // Walks every visible body's exact bbox to decide whether the
                  // project is "big enough" to suppress the 1x minor grid -
                  // BRepBndLib::Add is a real geometry pass, not a cached-bounds
                  // read, and on a 423-body dense-import scene one walk costs
                  // ~100ms (measured via GL timer queries + wall-clock spike,
                  // issue #110). A 65-body-project-era version of this comment
                  // used to unconditionally poll every ~0.25s - that aliased
                  // against the idle frame cadence and fired every other
                  // frame, which is what produced the reported ~8fps.
                  // m_gridExtentStale (set by rebuildMeshes() only when a body
                  // was actually added, removed, or edited) now gates whether
                  // there is anything to check at all, so a truly idle frame
                  // costs nothing; the 0.25s cooldown below still caps the
                  // worst case for a rapid string of non-geometric edits
                  // (e.g. a body-colour drag off a live colour wheel), which
                  // dirty m_dirtyBodyIds many times a second without ever
                  // changing the bounds.
                  static bool s_hideMinor = false;
                  static double s_nextCheckTime = 0.0;
                  const double now = ImGui::GetTime();
                  if (m_gridExtentStale && now >= s_nextCheckTime) {
                      m_gridExtentStale = false;
                      // Fresh each pass, not left at its previous value: an
                      // empty or fully-hidden scene must clear back to
                      // "not hidden", not freeze at whatever the last
                      // nonempty scene decided.
                      bool hideMinor = false;
                      Bnd_Box bb;
                      bool any = false;
                      for (int id : m_document->getAllBodyIds()) {
                          if (!m_document->isBodyVisible(id)) continue;
                          // Per-body, not around the whole loop: one body with
                          // a stale/invalid shape (a replay-order edge case
                          // elsewhere in this file already treats the same
                          // way) must not blank the bounds of every other
                          // body that resolved fine.
                          try {
                              BRepBndLib::Add(m_document->getBody(id), bb);
                              any = true;
                          } catch (const std::exception& e) {
                              // Same --verbose gate the click-diagnostic block
                              // in this file already uses: a normal launch
                              // doesn't pay a stderr flush for this, but it's
                              // not silently invisible either.
                              if (materializr::isVerbose())
                                  std::fprintf(stderr,
                                      "[GridExtent] body %d bounds failed: %s\n",
                                      id, e.what());
                              continue;
                          } catch (...) { continue; }
                      }
                      if (any && !bb.IsVoid()) {
                          double xmn,ymn,zmn,xmx,ymx,zmx;
                          bb.Get(xmn,ymn,zmn,xmx,ymx,zmx);
                          double ext = std::max({xmx-xmn, ymx-ymn, zmx-zmn});
                          hideMinor = (ext > 100.0);
                      }
                      s_hideMinor = hideMinor;
                      s_nextCheckTime = now + 0.25;
                  }
                  if (s_hideMinor) minorAlpha = 0.0f;
              }
  ```

  Note the two incidental fixes riding along with the invalidation change, both flagged in Codex review round 1 as pre-existing defects in the code being touched: (a) the old code left `s_hideMinor` at its previous value when the scene had no visible bodies (`any == false` or `bb.IsVoid()`), so deleting/hiding every body in a large project never brought the minor grid back - the rewrite resets to a fresh `false` every recomputation; (b) the old code wrapped the entire loop in one `try`, so a single body that throws mid-scan (e.g. a stale id) discarded every other body's contribution for that pass - the rewrite catches per-body and continues.

- [x] **Step 4: Build and run the full suite**

  ```bash
  cmake --build build -j$(sysctl -n hw.ncpu)
  ctest --test-dir build --output-on-failure
  ```

  Expect the existing test count (113) to still pass, 0 failures. No test in the suite currently exercises this exact code path (it is UI/viewport-only, not covered by the headless `materializr_core` test suite), so a passing run means "did not break anything else" - it is not itself proof the fix works.

- [x] **Step 5: Manually verify against the real repro**

  Using `.claude/skills/run-materializr/driver.sh`: build, launch, open the 423-body project used in the original investigation, let it settle idle with no camera movement, and read the on-screen fps counter (`Settings -> Appearance -> Show FPS counter`, or the existing `##ModernFps` chip if already enabled). Expect idle fps to rise from ~8 to roughly the idle floor's ~12-15fps target (the remaining gap to a full 15fps is the separate, smaller idle-floor-stacking issue noted in the issue write-up, not this fix's target). Merely orbiting the camera afterward proves nothing new about invalidation by itself (orbit alone never touches `m_dirtyBodyIds`/`m_meshesDirty`, so it was never going to trigger a recompute either before or after this change) - it is only a smoke check that ordinary use still feels fine.

  The invalidation correctness checks that actually exercise the new code path:
  - Toggle a body's visibility (or delete one) while idle and confirm the minor grid tier updates on the first eligible frame - i.e. within the 0.25s cooldown window, not "within a frame or two": the cooldown can defer an otherwise-immediate recomputation by up to that long, and that's expected, not a bug.
  - Toggle visibility twice in quick succession (well inside one cooldown window) and confirm the SECOND toggle's effect is not lost - `m_gridExtentStale` should still be true and get picked up on the next eligible frame once the cooldown from the first toggle expires, rather than being cleared without ever recomputing.
  - Toggle a body's visibility while an unrelated interactive op is active (e.g. mid gizmo-drag) and confirm the grid check - correctly suppressed by `!interactive` during the drag - still recomputes promptly once the drag ends, rather than the pending staleness being lost while suppressed.
  - Pick a body (or a small handful) whose combined extent straddles the 100mm threshold from both directions: hide/show it to flip the tier one way, then the other, confirming the check reacts both ways, not just once.
  - Hide (or delete) every visible body in the 423-body scene and confirm the minor grid tier returns to shown (`s_hideMinor` resets to `false`) rather than freezing at whatever the last nonempty-scene verdict was - this is the empty/all-hidden fix from Codex round 1, and the old code got it wrong.
  - Drag a body's colour off the live colour-wheel picker (Items panel folder/body colour swatch) for a couple of seconds while watching frame pacing: confirm it does not reintroduce the ~100ms-per-frame stall, since a held colour drag can fire `markBodyDirty` many times a second without ever changing the scene's bounds - this is what the retained 0.25s cooldown is specifically protecting against.
  - Optional, throwaway diagnostic (do not commit): temporarily log a counter each time the `BRepBndLib::Add` scan actually runs, to directly confirm it stays at zero across several seconds of true idle and stays rate-limited (not per-frame) during the colour-drag case above.

---

## Out of scope (tracked separately in issue #110, not this plan)

- The idle-floor wait (`kIdleFloorMs = 66` in `Application.cpp`'s main loop) doesn't budget against the time the frame it just waited for actually took to render, so it stacks additively rather than capping total idle-iteration time - worth roughly 8fps -> 12fps on top of this fix, separately from it. Not touched here.
- GPU-side draw cost (edge pass 6-20ms peak across 423 bodies, likely `GL_LINES` emulation overhead on Apple's Metal-backed GL) is the dominant cost during active camera movement ("low 20s" fps reported), not idle. Not touched here - would need its own design (e.g. edge LOD / silhouette-only at distance), which was NOT what this investigation set out to fix and was not approved for implementation.
