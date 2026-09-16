# Plan Review Log: Sketch polygon Select+Delete cascade corruption fix
Started 2026-09-16. MAX_ROUNDS=5. Codex model: gpt-6-astra (config.toml default,
via `codex exec` not the built-in review subcommand - per tools/codex.md
2026-09-08 entry, astra works fine for plain `codex exec`).
Thread: 01a0aa5a-ac89-7540-a7b0-4bd4eb90a9e6

## Round 1 - Codex

1. **Blocking: plan assumes code absent from this branch.** At `d1b378f`
   (upstream/main), `Sketch::removeElement` does not cascade a polygon's
   lines at all; `test_sketch_trim_polygon.cpp` and the `polygonLineIds`
   picker exclusion (attributed to PR #119) are absent too. Fix: base the
   branch on PR #119's tip, not `upstream/main` directly.
2. **Batch selection can still corrupt unrelated geometry.** Lines-then-points
   order in `deleteSelectedSketchElements`: deleting an edge cascades the
   polygon away first; a later, separately-selected shared vertex point then
   falls through to unconditional erase, orphaning any other geometry that
   still needed it. Test 5 (as drafted) only selected an edge and missed this.
3. **Line-tool backtracking becomes destructive.** `dropLineChainTail`'s
   `stillUsed` check only looks at line endpoints; a polygon's center point
   is never a line endpoint by construction, so backtracking onto a
   pre-existing polygon's center (welded via `findCoincidentPoint`, same as
   the Polygon tool) now deletes that whole unrelated polygon.
4. **Recursion doesn't guard against corrupted/merged data.** `addRawPolygon`
   and project loading never validate ids. A stale-id merge collision (already
   named in the plan's own blast-radius section) could produce a
   self-referencing or cross-referencing (cyclic) polygon `lineIds`/
   `vertexPointIds`; the then-recursive redirect would never terminate.
5. **Shared ownership resolved arbitrarily.** Two polygons can share one
   center point; the scan stopped at the first owner, leaving the *other*
   silently undeleted despite the "any owned piece deletes the whole polygon"
   policy.

VERDICT: REVISE

### Claude's response

All five accepted and fixed in the plan:

1. Verified directly (`gh pr view 119`: OPEN, `mergedAt: null`; grepped the
   PR #119 artifacts on a fresh `upstream/main` checkout - absent). Branch
   reset onto `origin/fix/sketch-polygon-trim-pick-priority` @ `4697d77`
   instead (PR #119's own branch untouched, just used as this branch's base -
   a normal depends-on-an-open-PR relationship). Re-verified the PR #119
   artifacts are present after the reset.
2. Accepted. Swapped `deleteSelectedSketchElements`'s delete order to
   points-then-lines (piece 2 in the revised plan) - a one-line reorder that
   makes a selected polygon-owned point always find its still-live owning
   polygon before any line-triggered cascade could remove the record out from
   under it. Added regression test 6.
3. Accepted. Extended `dropLineChainTail`'s `stillUsed` check to also treat a
   polygon's `centerPointId`/`vertexPointIds` as "used" (piece 3), mirroring
   the existing `removeLastSplinePoint` guard exactly. Added regression test
   7. The same finding's second half (segment lookup can pick the wrong
   duplicate-endpoint line) is a pre-existing, unrelated Line-tool ambiguity
   this change doesn't introduce - documented as an explicit out-of-scope
   follow-up rather than fixed here.
4. Accepted. Restructured `removeElement`'s redirect: skip the ownership scan
   entirely whenever `id` already names a live polygon. This bounds the
   redirect to exactly one recursion step by construction (every redirect
   target is a real, currently-live polygon id, so it can never re-enter the
   scan branch), independent of whatever a corrupted polygon's own
   `lineIds`/`vertexPointIds` claims - no visited-set or depth counter needed.
   Added regression test 8 (hand-built self-referencing `SketchPolygon` via
   `addRawPolygon`).
5. Accepted. Changed the scan to collect *every* owning polygon id before
   redirecting (into a `std::vector<int>` gathered in a separate pass, so no
   range-for/erase aliasing either), and cascade all of them. Matches the
   stated policy exactly instead of being first-in-`m_polygons`-wins.

## Round 2 - Codex

Confirmed: branch dependency correct (HEAD `4697d77`, cascade + Trim tests
present), collect-before-mutate and the live-polygon-id fast path address the
original aliasing/recursion concerns. Two remaining:

1. **Points-first still corrupts multi-point selections.** Select TWO
   vertices of the SAME polygon, each attached to a different unselected
   standalone line. The first point's deletion cascades the polygon; the
   second point then finds no owner (already gone) and falls through to
   unconditional erasure, orphaning its line. Reordering only moved the
   failure inside the points pass - it didn't close it. Test 6 (as drafted)
   selected only one point and missed this.
2. **Backtracking over a polygon edge still deletes the entire polygon.**
   `dropLineChainTail`'s endpoint lookup can choose the OLDER (pre-existing
   polygon) edge over the newly drawn duplicate. Before this change that
   silently removed the wrong stray line; now it removes an entire polygon.
   Documenting the ambiguity as "pre-existing" doesn't contain a regression
   in severity this change causes.

Also: the plan promised but never wrote a two-polygons-sharing-a-center test.

VERDICT: REVISE

### Claude's response

Both accepted, plus the missing test added.

1. Accepted - the reorder was a narrower fix than the actual problem. Added
   `Sketch::removeElements(const std::vector<int>&)`: resolves ownership for
   the WHOLE batch against pre-mutation state in one pass, collects every
   distinct owning polygon id, cascades each once, then processes the
   remaining (non-polygon-owned) ids individually. `removeElement(id)` stays
   as the safe single-id primitive (still needed as-is, and it's what
   `removeElements` calls internally for each resolved polygon id - always
   hitting the live-polygon fast path, no duplicated scan logic).
   `deleteSelectedSketchElements` now builds one combined ids vector and
   makes a single `removeElements` call instead of two loops;
   `undoLastStamp` converted the same way for the same class of hazard.
   Replaced test 6 with a version that selects two independently-shared
   vertices of the same polygon (the case a bare reorder cannot fix) and
   asserts both survive.
2. Accepted - fixed at the root instead of documenting it. `handleLineTool`
   already computes each segment's own line id (`newLineId`,
   SketchTool.cpp:2452) and was discarding it. Added a parallel
   `m_lineChainSegmentIds` vector pushed in lockstep with the existing
   `m_lineChain` (point ids); `dropLineChainTail` now pops and removes that
   exact id instead of re-deriving "the" segment by an endpoint-pair scan
   that picks the first (oldest) match. Removes the ambiguity outright - also
   incidentally fixes the same mis-pick for any duplicate-endpoint line pair,
   not just a polygon's, though that's a side effect, not the motivation.
   Added test 10.
3. Added test 9 (two polygons sharing one center point, plus a standalone
   line also on that point): both polygons deleted, point and line survive.

## Round 3 - Codex

No remaining material blockers. Batch resolution preserves both shared
vertices regardless of order; segment-id tracking removes the duplicate-edge
ambiguity; the planned `m_lineChainSegmentIds.clear()` sites cover every
existing `m_lineChain.clear()` path. Two minor notes (non-blocking):
- Regression-test preamble should say `removeElements`+`pruneOrphanPoints`
  (it still said `removeElement`, singular, after piece 2 changed).
- Add a test for the parallel-vector invariant under repeated backtracking
  and cancel/restart.

VERDICT: APPROVED

### Claude's response

Both folded in: fixed the preamble wording, and added test 11
(`LineChainSegmentIdsStaySyncedAcrossRepeatedBacktrackAndCancelRestart`) -
two consecutive `dropLineChainTail()` calls plus a fresh-chain-start and a
cancelled chain, asserting `m_lineChain`/`m_lineChainSegmentIds` stay the
same size and both clear together at every existing clear site.

## Outcome

APPROVED after 3 rounds. Plan converged from a single-choke-point
`removeElement` redirect to four coordinated pieces (cycle-proof
all-owners `removeElement` redirect, a batch-aware `removeElements` for
multi-id selections, and two independent `dropLineChainTail` fixes) plus 11
regression tests. Awaiting user sign-off before any implementation code is
written (skill hard rule: no code during the loop).
