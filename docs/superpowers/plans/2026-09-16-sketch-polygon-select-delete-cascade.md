# Sketch polygon Select+Delete cascade corruption

_Round 1 revision - see the review log for what Codex caught and why each item
below changed._

Separate from PR #119 (`fix/sketch-polygon-trim-pick-priority`) - do not touch
that branch, but **this fix depends on it**: PR #119 is still open (not yet
merged into `upstream/main` as of 2026-09-16 - confirmed via `gh pr view 119`,
`state: OPEN`, `mergedAt: null`). `Sketch::removeElement`'s existing
polygon-id cascade, `pickSketchElement`'s `polygonLineIds` exclusion set, and
`tests/test_sketch_trim_polygon.cpp` all ship IN #119, not on `main` yet - a
branch cut from `upstream/main` alone is missing all of it. Branch:
`fix/sketch-polygon-select-delete-cascade`, reset onto
`origin/fix/sketch-polygon-trim-pick-priority` @ `4697d77` (PR #119's current
tip) instead. #119's branch itself is untouched; this is a normal
depends-on-an-open-PR situation and will need a rebase (not a merge conflict)
once #119 lands on `main`.

## Confirmed bug

`Sketch::addPolygon` (src/modeling/Sketch.cpp:495) creates a polygon's center
point, N vertex points, and N edge lines as ordinary `SketchPoint`/`SketchLine`
entries - the polygon record (`SketchPolygon`) just remembers their ids in
`centerPointId`, `vertexPointIds`, `lineIds`.

`SketchTool::handleSelectTool` (SketchTool.cpp:2287) and its point picker
`findCoincidentPoint` (SketchTool.cpp:2229) have **no polygon-ownership
awareness** - they hit-test `sketch.getPoints()` / `sketch.getLines()`
directly, so a click on a polygon's center, a vertex, or an edge is picked
exactly like any standalone point/line and lands in
`m_selectedPoints`/`m_selectedLines`.

`Application::deleteSelectedSketchElements` (Application.cpp:6647) then calls
`m_activeSketch->removeElement(id)` for each selected id **directly** - never
the owning polygon's id.

`Sketch::removeElement` (Sketch.cpp:704) only cascades (drops the polygon's
owned lines + the polygon record) when `id` equals a polygon's **own** id
(the `for (const auto& p : m_polygons) if (p.id != id) continue;` block). A
line id, a vertex-point id, or the center-point id never matches that
comparison, so the cascade never fires:

- **Edge line selected+deleted**: `removeElement` erases it from `m_lines`
  (real, visible gap in the polygon's outline) but `SketchPolygon::lineIds`
  keeps the now-dangling id forever. `sides`/`vertexPointIds` still claim a
  complete N-gon that no longer geometrically exists.
- **Vertex point selected+deleted**: erased from `m_points`, but
  `vertexPointIds` keeps the dangling id, and the two edge lines that used to
  meet there now reference a point that doesn't exist (`Sketch::getPoint`
  returns null for it - every consumer that reads those lines' endpoints via
  `getPoint` silently no-ops/skips, so this degrades rendering and picking
  rather than crashing).
- **Center point selected+deleted**: same shape - `centerPointId` left
  dangling, `drawPolygons()`'s center-cross marker (SketchRenderer.cpp:580)
  silently stops drawing for that polygon (`lutPoint` returns null).

Note `pruneOrphanPoints()` (Sketch.cpp:750) does **not** clean any of this up:
it treats every id in a *surviving* polygon's `vertexPointIds`/`centerPointId`
as "used" unconditionally, so it never re-derives the polygon's real shape
from `m_lines`/`m_points` - it only prunes points no longer referenced by
anything, which a still-dangling `lineIds`/`vertexPointIds` entry prevents
from ever triggering for the *other*, still-alive vertices.

### Blast radius beyond the immediate corruption

- **Persistence** (`io/ProjectIO.cpp:381-389,1735-1743`,
  `modeling/SketchEditOp.cpp:277-285` for undo snapshots): both serialize
  `lineIds`/`vertexPointIds` verbatim, so the dangling id round-trips through
  save/load and undo/redo instead of ever self-healing.
- **CombineSketchesOp::mergeInto** (CombineSketchesOp.cpp:49-57): builds an
  `id[]` remap table only from ids it actually walks in `src.getLines()` /
  `src.getPoints()`. A dangling `lineIds`/`vertexPointIds` entry was already
  removed from those containers, so `id.count(li)` is false and the stale
  *source-sketch-numbered* integer is copied into the destination sketch
  **unmapped**. If that raw number happens to collide with a real, unrelated
  element id already present in the destination sketch, the merged polygon's
  `lineIds` now falsely claims ownership of somebody else's line.
- **SketchTool.cpp:3101-3103** (`pickSketchElement`, the trim picker's
  polygon-ownership exclusion set, added in PR #119) reads `lineIds` to build
  its skip-set. A falsely-claimed id from the collision above would make the
  Trim tool silently refuse to pick that unrelated line as a plain line
  (`if (polygonLineIds.count(ln.id)) continue;`, SketchTool.cpp:3107) -
  degraded trim usability on geometry that was never part of any polygon.
  (Confirmed this is *not* independently exploitable via Trim on the
  already-corrupted polygon itself: Trim's own picker walks
  `vertexPointIds` point-to-point rather than trusting `lineIds`/real lines
  in `sketch.getLines()`, so clicking the gap still resolves to
  `pickType == "polygon"` and commits `FullDelete` on the whole polygon,
  which cascades correctly and is idempotent against the already-missing
  line/point.)

Confirmed by direct reading of `handleSelectTool`, `deleteSelectedSketchElements`,
`removeElement`, `pruneOrphanPoints`, `pickSketchElement`, `ProjectIO.cpp`, and
`CombineSketchesOp.cpp` - not from the prior review-subagent traces alone.

## Design decision

Three options, per the deferral note on PR #119:

1. **Refuse** - `removeElement` no-ops on a polygon-owned id. Rejected: silently
   drops the user's delete with no feedback, and is inconsistent with the
   already-shipped Trim precedent (option 3) for the identical situation.
2. **Detach** - strip the id from the owning polygon's `lineIds`/`vertexPointIds`
   and leave the rest of the polygon standing. Rejected: `SketchPolygon` is
   `centerPointId` + `radius` + `sides` - a rigid regular N-gon by
   construction. There is no valid `SketchPolygon` state for "hexagon missing
   one edge/vertex"; this would require either changing the type's meaning or
   silently downgrading it to a loose line/point soup, which is a much bigger,
   unscoped change.
3. **Cascade to full delete** - selected: matches the Trim tool's existing,
   already-shipped behavior (any click on a polygon edge -> `pickType ==
   "polygon"` -> `FullDelete` of the whole polygon, SketchTool.cpp:3164,
   3322) and matches the existing ad hoc ownership guard in
   `SketchTool::removeLastSplinePoint` (SketchTool.cpp:2947-2953, which
   checks polygon `centerPointId`/`vertexPointIds` before calling
   `removeElement` on a point precisely to avoid deleting a polygon out from
   under itself). Deleting any one piece of a polygon deletes the whole
   polygon - `SketchPolygon` is one indivisible generated unit, everywhere
   else in the codebase already treats it that way.

## Fix

Four coordinated pieces - Round 1 review found the single `removeElement`
redirect alone was unsafe. Each piece below closes one gap Codex identified.

### 1. `Sketch::removeElement` - cycle-proof, all-owners redirect

At the one choke point every deletion path already funnels through
(Sketch.cpp:704). Before the existing per-category erases: skip the redirect
entirely when `id` already names a live polygon (this is what makes the
recursion provably terminate - see below); otherwise collect **every**
polygon that owns `id` via its center, a vertex, or an edge line, and cascade
each of them:

```cpp
void Sketch::removeElement(int id) {
    // If `id` already names a live polygon, go straight to the existing
    // whole-polygon cascade below and skip the ownership scan entirely. This
    // is what makes the redirect below provably terminate in one recursion
    // step: every redirect target is a real m_polygons[i].id, and a call
    // with that id can never re-enter this branch, regardless of what a
    // polygon's centerPointId/vertexPointIds/lineIds claims (self-reference
    // or a cycle between two polygons is normally impossible - ids are
    // unique and machine-generated - but ProjectIO/addRaw*/CombineSketchesOp
    // never validate ids on load or merge, so corrupted/merged data can
    // contain one; without this guard that would stack-overflow instead of
    // just misbehaving).
    bool isPolygonId = std::any_of(m_polygons.begin(), m_polygons.end(),
        [id](const SketchPolygon& p) { return p.id == id; });

    if (!isPolygonId) {
        // A polygon's vertices and edge lines are generated together
        // (addPolygon) and its center point can additionally be an existing,
        // pre-shared point (handlePolygonTool welds onto one via
        // findCoincidentPoint) - either way SketchPolygon (centerPointId +
        // radius + sides) has no valid state for "missing one edge/vertex".
        // Deleting any owned piece by its own id (reachable via ordinary
        // point/line picking - handleSelectTool has no polygon awareness)
        // must delete the whole polygon instead, same as clicking any edge
        // in the Trim tool already does (pickSketchElement's FullDelete
        // case). A point can be the shared center of MORE THAN ONE polygon -
        // collect every owner before touching any of them, so a shared point
        // takes out every polygon that owns it, not just the first found.
        std::vector<int> owningPolygonIds;
        for (const auto& p : m_polygons) {
            bool owned = p.centerPointId == id ||
                std::find(p.vertexPointIds.begin(), p.vertexPointIds.end(), id) !=
                    p.vertexPointIds.end() ||
                std::find(p.lineIds.begin(), p.lineIds.end(), id) !=
                    p.lineIds.end();
            if (owned) owningPolygonIds.push_back(p.id);
        }
        if (!owningPolygonIds.empty()) {
            for (int polyId : owningPolygonIds) removeElement(polyId);
            return;
        }
    }

    m_lines.erase(...);   // existing body unchanged from here down
    ...
}
```

`owningPolygonIds` is collected in a separate pass before any mutation, so
there's no range-for-vs-erase aliasing hazard iterating `m_polygons` while
recursing.

A point shared between a polygon and other geometry (the welded center, or a
vertex a later Line-tool click welded onto) is NOT removed by this cascade
itself - `removeElement(polyId)` only erases that polygon's own lines and its
`SketchPolygon` record, never a point directly. The point survives exactly as
long as something still references it; a caller's subsequent
`pruneOrphanPoints()` call (every real call site already makes one - see
Application.cpp:6660, SketchTool.cpp:3624) is what actually drops it, and
only when nothing else does. Covered by regression tests 5-6 below.

`handleSelectTool`'s picking side needs no change at all - it keeps picking
the raw line/point id exactly as before, which is fine now that
`removeElement` redirects it correctly. The batch-processing callers
(`Application::deleteSelectedSketchElements`, `undoLastStamp`) and
`dropLineChainTail` each need their own additional change - pieces 2 and 3
below - because Round 1/2 review found they were relying on assumptions about
per-call `removeElement` behavior (or, for `dropLineChainTail`, about line
lookup) that this redirect breaks or exposes.

No change to `SketchTool::removeLastSplinePoint`'s existing ownership guard
(SketchTool.cpp:2947-2953) - that guard exists to **avoid** calling
`removeElement` at all when a spline-in-progress snapped onto a polygon
vertex the user isn't trying to delete; it is solving a different problem
(don't touch geometry outside this action) and stays correct as-is. It was
also the direct precedent that pieces 2 and 3 below now mirror.

### 2. New `Sketch::removeElements(const std::vector<int>& ids)` - resolve the
whole batch's polygon ownership before mutating anything

Round 1's finding (fixed by simply reordering points-before-lines) turned out
to be one instance of a more general problem, per Round 2: **any** batch that
selects more than one piece of the *same* polygon - two shared vertices, a
shared vertex plus a shared center, etc. - breaks regardless of processing
order, because the very first piece processed cascades the polygon away, and
every *subsequent* piece from that same polygon then finds no live owner and
falls through to plain, unconditional per-category erasure. A per-call,
stateless `removeElement(id)` cannot close this on its own - the batch has to
be resolved against the *pre-mutation* state as a whole, once, before
anything is deleted.

New method on `Sketch` (Sketch.cpp, alongside `removeElement`):

```cpp
void Sketch::removeElements(const std::vector<int>& ids) {
    // Resolve every polygon touched by ANY id in this batch against the
    // sketch's state BEFORE deleting anything. A batch selection can contain
    // more than one piece of the same polygon (two vertices, an edge plus a
    // vertex, ...) - deleting the first piece's owning polygon (see
    // removeElement) would make a later piece from the SAME polygon
    // undetectable as polygon-owned by the time its turn comes, and it would
    // fall through to plain, unconditional erasure - corrupting any OTHER,
    // unselected geometry still sharing that point. Resolving the whole
    // batch up front and skipping ids already covered by a polygon's own
    // cascade closes that window for any size/order of batch.
    std::unordered_set<int> polygonOwnedIds;
    std::vector<int> polygonsToDelete;
    for (int id : ids) {
        for (const auto& p : m_polygons) {
            bool owned = p.centerPointId == id ||
                std::find(p.vertexPointIds.begin(), p.vertexPointIds.end(), id) !=
                    p.vertexPointIds.end() ||
                std::find(p.lineIds.begin(), p.lineIds.end(), id) !=
                    p.lineIds.end();
            if (!owned) continue;
            polygonOwnedIds.insert(id);
            if (std::find(polygonsToDelete.begin(), polygonsToDelete.end(), p.id) ==
                polygonsToDelete.end())
                polygonsToDelete.push_back(p.id);
        }
    }
    for (int polyId : polygonsToDelete) removeElement(polyId);
    for (int id : ids) {
        if (polygonOwnedIds.count(id)) continue; // handled via its polygon above
        removeElement(id);
    }
}
```

Every `removeElement(polyId)` call here passes a real, currently-live
polygon id collected directly from `p.id`, so it takes piece 1's
`isPolygonId==true` fast path straight to the ordinary cascade - no
duplicated ownership-scan logic, no conflict with piece 1.

`Application::deleteSelectedSketchElements` (Application.cpp:6647) switches
from its two separate loops to one combined-batch call:

```cpp
std::vector<int> ids(pts.begin(), pts.end());
ids.insert(ids.end(), lns.begin(), lns.end());
m_activeSketch->removeElements(ids);
```

`SketchTool::undoLastStamp` (SketchTool.cpp:4036-4044) gets the same
treatment - `for (int id : ids) m_sketch->removeElement(id);` becomes
`m_sketch->removeElements(ids);` - for the same reason: a stamp's placed-ids
list is exactly the kind of externally-assembled batch that could contain
more than one piece of a polygon (if a stamp ever places one), and the fix is
free once `removeElements` exists.

Covered by regression tests 6 and 9.

### 3. `SketchTool::dropLineChainTail` - two independent gaps

SketchTool.cpp:388-420 (Line-tool chain backtracking, e.g. pressing
Backspace/Escape mid-chain). Round 1 and Round 2 each found a distinct way
this function can now delete an entire unrelated, pre-existing polygon as a
side effect of the user backtracking their OWN just-drawn line - both fixed.

**Gap A (Round 1) - the tail point's "still used" check doesn't know about
polygon centers.** SketchTool.cpp:408-413:

```cpp
bool stillUsed = false;
for (const auto& l : m_sketch->getLines())
    if (l.startPointId == tail || l.endPointId == tail) { stillUsed = true; break; }
if (!stillUsed) m_sketch->removeElement(tail);
```

If the chain's last vertex (`tail`) welded onto a pre-existing polygon's
center point (the Line tool welds onto existing points the same way
`handlePolygonTool` does), that center is never itself a line endpoint by
construction (only a polygon's *vertices* are). `stillUsed` reads false even
though the point is still needed, and `removeElement(tail)` (with piece 1)
now deletes the whole polygon - the same failure shape
`removeLastSplinePoint` already guards against for splines
(SketchTool.cpp:2947-2953).

Fix: extend the check the same way `removeLastSplinePoint` does - also check
`centerPointId` (the actual gap) and `vertexPointIds` (defense in depth;
already implied by the line check in well-formed data, but cheap and keeps
the two guards visibly symmetric):

```cpp
bool stillUsed = false;
for (const auto& l : m_sketch->getLines())
    if (l.startPointId == tail || l.endPointId == tail) { stillUsed = true; break; }
if (!stillUsed)
    for (const auto& pg : m_sketch->getPolygons()) {
        if (pg.centerPointId == tail) { stillUsed = true; break; }
        if (std::find(pg.vertexPointIds.begin(), pg.vertexPointIds.end(), tail) !=
            pg.vertexPointIds.end()) { stillUsed = true; break; }
    }
if (!stillUsed) m_sketch->removeElement(tail);
```

**Gap B (Round 2) - the segment lookup can pick a pre-existing polygon edge
instead of the line the user just drew.** SketchTool.cpp:400-407 finds "the"
segment to delete by scanning `sketch.getLines()` for the first line whose
endpoints match `(prev, tail)`:

```cpp
for (const auto& l : m_sketch->getLines()) {
    if ((l.startPointId == prev && l.endPointId == tail) ||
        (l.startPointId == tail && l.endPointId == prev)) {
        m_sketch->removeElement(l.id);
        break;
    }
}
```

If the user's new segment happens to trace exactly over an existing polygon
edge (both ends welded onto that polygon's existing vertices), TWO lines now
share the same endpoint pair: the polygon's original edge (created earlier,
so it sorts first in `m_lines`) and the user's new duplicate. This scan picks
the polygon's edge - the FIRST match, not necessarily the one the user just
drew - and (with piece 1) deletes the whole polygon. Round 2 correctly
rejected treating this as "pre-existing ambiguity, just louder now": the
fix is small and removes the ambiguity outright rather than living with it.

Fix: track the segment's own line id directly instead of re-deriving it by
endpoint match. `handleLineTool` (SketchTool.cpp:2452) already computes
`newLineId = m_sketch->addLine(...)` for every committed segment and
currently discards it (`(void)newLineId;`, SketchTool.cpp:2456). Add a
parallel `std::vector<int> m_lineChainSegmentIds` next to the existing
`m_lineChain` (point ids) in `SketchTool.h:569`, push `newLineId` onto it in
lockstep with `m_lineChain.push_back(endPointId)` (SketchTool.cpp:2491),
`clear()` it everywhere `m_lineChain.clear()` already fires (chain start
SketchTool.cpp:2418, auto-close SketchTool.cpp:2483, cancel
SketchTool.cpp:385), and have `dropLineChainTail` pop the exact id from it:

```cpp
int tail = m_lineChain.back();
// (the `prev` lookup variable is no longer needed - removed)

// Delete the EXACT segment line this chain step created (tracked in
// m_lineChainSegmentIds, pushed in lockstep with m_lineChain in
// handleLineTool) - not re-derived from the endpoint pair, which picks the
// WRONG line whenever another line (e.g. a pre-existing polygon edge)
// already shares those same two endpoints.
if (!m_lineChainSegmentIds.empty()) {
    m_sketch->removeElement(m_lineChainSegmentIds.back());
    m_lineChainSegmentIds.pop_back();
}
```

This also incidentally fixes a PRE-EXISTING (not polygon-related, not caused
by this plan) correctness bug: even before piece 1, the old endpoint-lookup
could silently delete the wrong line of any duplicate-endpoint pair, not just
a polygon's. Worth calling out since it's a nice side effect, not claiming it
as part of this fix's motivation.

Both gaps covered by regression tests 7 and 10.

## UX consequence (explicitly accepted, not a bug)

After this fix, Select-clicking ONE edge of a hexagon still only highlights
that one line (`handleSelectTool` is unchanged), but pressing Delete removes
the WHOLE hexagon, not just the clicked edge. This is a deliberate, visible
behavior change from "silently corrupts and leaves a gap" to "deletes more
than the highlighted selection implies."

Accepted as-is, scoped as a data-corruption fix rather than a selection-UX
redesign: it exactly mirrors the Trim tool's already-shipped, user-facing
precedent (PR #119) of "any interaction with one polygon edge acts on the
whole polygon." Making the Select-mode highlight itself expand to the whole
polygon on hover/click (so the UI previews what Delete is about to do, the
way Trim's hover preview does - SketchTool.cpp:3153-3167,3507-3514) is a
reasonable follow-up but is selection-UX scope, not corruption-fix scope;
left for a separate change if wanted.

## Regression tests

New file `tests/test_sketch_select_delete_polygon.cpp`, same style as
`tests/test_sketch_trim_polygon.cpp` (drive `SketchTool` in `Select` mode via
the public `onMouseDown`, then run the exact same
`removeElements`+`pruneOrphanPoints` sequence the revised
`Application::deleteSelectedSketchElements` runs, since `Application` itself
pulls in the full `Document`/viewport/OCCT stack and isn't unit-testable in
isolation).

`handleSelectTool` snaps the click position first (unlike Trim, which uses
the raw cursor - SketchTool.cpp:73-75), so every test polygon uses
`sides=4, rotation=0, radius=10` with `kStep=1.0`: vertices land at
`(±10,0),(0,±10)`, edge midpoints at `(±5,±5)`, center at `(0,0)` - all
already on the 1.0 lattice, so `snap()` cannot move the click off target
regardless of tolerance/threshold tuning elsewhere.

1. `SelectClickOnEdgeThenDeleteCascadesWholePolygon` - click Select on a
   polygon edge midpoint (5,5), assert it lands in `getSelectedLines()` as
   the raw line id (proves the picking side is unchanged/still "naive"), then
   run the delete+prune sequence and assert: polygon gone from
   `getPolygons()`, every one of its own line ids gone from `getLines()`, its
   vertex+center points gone from `getPoints()` (pruned), and an unrelated
   standalone line/points elsewhere in the sketch is completely untouched.
2. `SelectClickOnVertexThenDeleteCascadesWholePolygon` - same, but the click
   lands on a vertex point (10,0) (`getSelectedPoints()`), covering the
   vertex-point-direct-delete path.
3. `SelectClickOnCenterThenDeleteCascadesWholePolygon` - same, but the click
   lands on the polygon's center point (0,0).
4. `StandaloneGeometryStillDeletesNormally` - Select+Delete on a plain
   non-polygon line and a plain point still just removes that one element
   (no polygon in the sketch at all) - guards against the new ownership scan
   over-triggering.
5. `SharedCenterPointSurvivesPolygonCascade` - weld the polygon's center onto
   the endpoint of a pre-existing standalone line (`addPolygon(existingPtId,
   ...)`, mirroring `handlePolygonTool`'s `findCoincidentPoint` reuse), then
   Select+Delete an edge of the polygon: the polygon and its own lines are
   gone, but the shared center point and the standalone line survive -
   `pruneOrphanPoints` must not drop a point still referenced by surviving
   geometry.
6. `RemoveElementsResolvesWholeBatchBeforeMutating` - Round 1 finding #2 AND
   Round 2 finding #1 (the reorder alone wasn't enough; this tests the actual
   shipped `removeElements` batch resolution). Weld TWO different standalone
   lines' endpoints onto TWO different vertices of the SAME polygon (so the
   polygon has two independently-shared vertices). Select a polygon edge line
   plus BOTH shared vertex points in one action (`ids` = all three,
   mirroring `deleteSelectedSketchElements`'s combined pts+lns vector), call
   `sk.removeElements(ids)`: the polygon and its own lines are gone, but
   BOTH standalone lines and both shared vertex points survive intact. This
   is exactly the case a naive points-before-lines reorder still breaks (the
   first shared vertex's cascade would orphan protection for the second) -
   asserting both survive is what actually proves the batch-wide resolution
   works, not just the single-shared-point case test 5 already covers.
7. `BacktrackOntoPolygonCenterDoesNotDeletePolygon` - Round 1 finding #3 /
   piece 3 Gap A. Create a polygon, then drive the Line tool
   (`SketchToolMode::Line`) to click-place a line segment ending exactly on
   the polygon's center point (welds via `findCoincidentPoint`), then call
   `dropLineChainTail()`: the just-drawn segment is removed, but the
   pre-existing polygon (record, all its lines, all its vertices, its
   center) is completely untouched.
8. `RemoveElementToleratesSelfReferencingPolygonData` - Round 1 finding #4,
   direct unit test of the cycle guard rather than trying to construct it via
   the tool layer. Build a `SketchPolygon` by hand via `addRawPolygon` whose
   `lineIds` contains its own `id` (simulating corrupted/merged project
   data), then call `removeElement` on that polygon's id: it must return
   (not stack-overflow) and leave the sketch in the same state as deleting an
   uncorrupted polygon by its own id would.
9. `SharedCenterAcrossTwoPolygonsDeletesBothLeavesLineIntact` - Round 2's
   explicit ask. Two polygons welded onto the same center point (second
   `addPolygon` call reuses the first's `centerPointId`), plus a standalone
   line ALSO welded onto that same shared center. Select+delete the center
   point (`removeElements({centerPointId})`): both polygons and all of both
   polygons' own lines are gone, but the center point and the standalone line
   survive (still needed by the line).
10. `BacktrackOverExistingPolygonEdgeRemovesOnlyTheNewDuplicate` - Round 2
    finding #2 / piece 3 Gap B. Create a polygon, then drive the Line tool to
    draw a new segment tracing exactly over one of the polygon's existing
    edges (both clicks weld onto that edge's two existing vertices, so a
    duplicate-endpoint line now exists), then call `dropLineChainTail()`:
    exactly the newly-drawn duplicate line is gone (`getLines().size()` back
    to what it was before the duplicate was drawn), and the pre-existing
    polygon - record, every original line, every vertex - is byte-for-byte
    untouched (same ids all still present).
11. `LineChainSegmentIdsStaySyncedAcrossRepeatedBacktrackAndCancelRestart` -
    Round 3 note. Draw a 4-segment chain, call `dropLineChainTail()` twice in
    a row (drops two segments back-to-back) and assert
    `m_lineChain.size() == m_lineChainSegmentIds.size()` holds after each
    call, plus each popped id actually matches a line that existed and is now
    gone. Then start a fresh chain (new `onMouseDown` at a new anchor,
    exercising the SketchTool.cpp:2418 clear path) and a cancelled one
    (`onEscape`/cancel mid-chain, exercising the SketchTool.cpp:385 clear
    path), asserting `m_lineChainSegmentIds` is empty at the start of each
    new chain exactly like `m_lineChain` is - guards the parallel-vector
    invariant piece 3 Gap B's fix depends on, across every place
    `m_lineChain.clear()` already fires.

Register in `tests/CMakeLists.txt` mirroring the `test_sketch_trim_polygon`
block (add_executable / target_link_libraries / add_test).

## Verification

- Build + run the new test binary plus `test_sketch_trim_polygon` (make sure
  the two don't regress each other) and the full suite.
- `tools/units_audit.py` regeneration if the Sketch.cpp edit shifts any
  tracked line numbers (per project memory: msvc-core-target-gap /
  stale-test-binaries-on-header-edit gates).
- Grep new code for `\bfar\b|\bnear\b` (Windows MSVC macro gotcha, per
  project memory) - not expected to apply here, no such identifiers used.
