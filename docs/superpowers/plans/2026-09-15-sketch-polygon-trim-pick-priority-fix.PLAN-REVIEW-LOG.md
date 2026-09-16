# Plan Review Log: Sketch polygon trim pick-priority fix
Started 2026-09-15. MAX_ROUNDS=5. Model: gpt-6-astra (CLI default, `~/.codex/config.toml`).

## Round 1 - Codex

The production fix is plausible, but the regression plan has material problems:

1. The supplied tests do not compile: `const auto* poly = nullptr;` cannot deduce a pointer
   type from `nullptr`. Fix: explicit `const materializr::SketchPolygon* poly = nullptr;`.
2. The focused test command would run no tests: `tests/CMakeLists.txt` registers explicit
   executable names, CTest does not discover GoogleTest suite names. Fix: register the
   executable explicitly and run `ctest -R '^test_sketch_trim_polygon$'`.
3. The standalone-line assertion accepts incorrect trim geometry (any endpoint with x>1
   passes, including a wrong "kept the whole line" result). Fix: assert the exact surviving
   segment.
4. The cascade test cannot detect deletion of unrelated geometry or failed constraint
   cleanup - a single polygon with no other geometry would pass even if `removeElement`
   nuked the whole sketch. Fix: add unrelated geometry + a dangling constraint and assert
   both are handled correctly (unrelated survives, constraint is pruned).

VERDICT: REVISE

### Claude's response

Accepted all four points as correct, in full:

1. Replaced the `nullptr`-deduction pattern with a `findPolygon()` helper returning
   `const SketchPolygon*` in all three tests.
2. Read `tests/CMakeLists.txt:463-465` to confirm the explicit-registration pattern (no glob,
   no `gtest_discover_tests`). Added the exact `add_executable`/`target_link_libraries`/
   `add_test` block mirroring `test_sticky_rim`, and corrected the plan's build-verification
   command to `ctest -R '^test_sketch_trim_polygon$'` (executable name, not suite name).
3. Rewrote the standalone-line test with `lineWithEndpointsExists()`/exact-endpoint
   assertions: proves the surviving segment is exactly `(0,0)-(10,0)` and the left half is
   gone, not just "some line with x>1 exists".
4. Rewrote the cascade test to add a second, disjoint standalone line (proves the cascade
   only removes the polygon's own `lineIds`, via an exact line-count-delta assertion) and a
   `Horizontal` constraint on one of the polygon's edges (proves `pruneOrphanPoints()`'s
   existing dangling-constraint sweep, `Sketch.cpp:754-760`, actually fires once the cascade
   invalidates that line id - confirmed by reading that this sweep is generic across all
   entity types already, so no new constraint-cleanup code is needed in Task 2, only the test).

Nothing rejected - all four critiques were concrete, correct, and cheap to fix.

## Round 2 - Codex

The revisions resolve the compile error, test registration mismatch, weak segment assertion,
and main cascade-isolation gap. No remaining implementation blocker found.

Nonblocking improvements:
- Add shared-vertex retention and orphan-point assertions; the revised cascade test still
  covers only disjoint geometry.
- Remove stale "no CMakeLists changes" and "most likely no change needed" text.
- Add `--no-tests=error` to the focused CTest command to prevent silent success if
  registration fails.

VERDICT: APPROVED

### Claude's response

Applied the two cheap, purely textual nonblocking fixes (removed the stale "no CMakeLists
changes" hedge now that Task 3 explicitly modifies it; added `--no-tests=error` to the
focused ctest command). Declined the shared-vertex/orphan-point test addition for this pass:
it is explicitly flagged nonblocking, the plan already demonstrates the cascade is scoped
correctly via the disjoint-geometry assertion, and `pruneOrphanPoints()`'s point-sharing
behavior (a polygon vertex point also used by unrelated geometry survives) is pre-existing,
unmodified logic this change does not touch - adding it would test code this plan does not
change, not the fix itself.

## Outcome

Converged in 2 rounds. Plan at
`docs/superpowers/plans/2026-09-15-sketch-polygon-trim-pick-priority-fix.md` is APPROVED.
Awaiting user sign-off before any implementation.
