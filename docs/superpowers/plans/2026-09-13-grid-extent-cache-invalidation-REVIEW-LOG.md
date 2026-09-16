# Plan Review Log: Grid minor-tier extent cache invalidation (issue #110)
Started 2026-09-13 12:10 local. MAX_ROUNDS=5. Codex model: gpt-6-astra (from ~/.codex/config.toml), via `codex exec`.

## Round 1 - Codex

The rebuild hook covers the checked mutation paths, but the plan has four material gaps.

1. **Repeated edits can become slower.** Scale Face updates previews during dragging, but the grid's `interactive` guard excludes it. Colour changes also dirty meshes despite unchanged bounds. Removing the throttle can therefore add the reported ~100ms scene scan to every updated frame.
   **Fix:** Require both staleness and the existing 0.25-second cooldown; this preserves edit batching while eliminating scans on unchanged idle frames.

2. **Empty scenes retain the previous project's verdict.** Step 3 assigns `s_hideMinor` only when bounds are nonvoid. Hide/delete every body in a large scene, or switch to an empty document: recomputation leaves it `true` and clears staleness. This preserves an existing defect directly within the cache being changed.
   **Fix:** Compute into a local verdict initialized to `false`, then publish it explicitly for empty and all-hidden scenes.

3. **An exception freezes the cache until another mesh invalidation.** Step 3 clears staleness before scanning and silently catches every exception. Previously, a failed calculation was retried after 0.25 seconds; now the old verdict can persist indefinitely while idle, without diagnostic evidence.
   **Fix:** Define and log an explicit failure fallback, and clear staleness only after publishing either a successful result or that fallback.

4. **The manual checks can pass with broken invalidation.** Orbiting does not activate the grid's `interactive` predicate. Hiding an arbitrary body in the 423-body scene may leave its extent above 100mm, producing no observable tier change. An average FPS reading also cannot prove idle scans stopped.
   **Fix:** Test known extents on both sides of 100mm, empty/all-hidden scenes, session switching, and deferred updates during actual editing; temporarily count scans to verify zero idle recomputations and bounded scans during repeated edits.

VERDICT: REVISE

### Claude's response

Verified point 1 concretely before acting on it: `grep` confirmed Scale Face's live-drag path (`FaceOpControllers.cpp`) never calls `markBodyDirty`/touches `m_dirtyBodyIds` during the drag itself (the async-preview pattern this project already uses for interactive ops bypasses the rebuild-dirty pipeline for exactly this reason), so the *drag* half of point 1 doesn't independently regress. But the *colour-drag* half is real and more general: `ImGui::ColorEdit3`'s picker wheel fires its callback continuously while held, and `setBodyColorAndMark` routes through the same shared `markBodyDirty()` that any other per-body edit does - so a held colour drag would set `m_gridExtentStale` on many consecutive frames despite never changing the scene bounds. Adopted the fix as specified: kept the existing 0.25s value as a rate-limiter gating actual recomputation (not a poll timer deciding whether to look at all), with a pending stale flag persisting across a cooldown period rather than being dropped.

Adopted point 2 as described: `s_hideMinor`/its replacement local is now initialized fresh to `false` on every recomputation pass, not left at its previous value, so an empty or fully-hidden scene resets rather than freezing.

Adopted the concrete half of point 3 (moved the per-body `try` inside the loop instead of around the whole scan, so one bad body no longer discards every other body's contribution for that pass - this makes the realistic failure mode self-healing without new logging infrastructure). Did not add explicit failure logging/an alternate fallback path beyond what's already in the file: the outer exception surface this code can realistically hit is a per-body OCCT lookup failing, which the per-body catch now handles directly, and this file's surrounding code (`rebuildMeshes()`, `captureProjectThumbnailPNG`) uses the same silent-catch-and-continue idiom throughout - adding structured logging here alone would be inconsistent with the file's own conventions for a case that's now largely moot.

Adopted point 4: rewrote Step 5 to test both tier directions across the 100mm threshold, the empty/all-hidden reset from point 2, and specifically the colour-drag scenario the point-1 fix targets, plus an optional throwaway scan-counter diagnostic to directly confirm zero idle recomputation and rate-limited (not per-frame) recomputation during the colour-drag case. Removed the orbit-and-confirm-no-stutter check since orbiting alone never touches the invalidation trigger either before or after this change, so it doesn't test anything the review didn't already flag as vacuous.

## Round 2 - Codex

No remaining material blockers. The retained cooldown prevents per-frame scans during repeated edits, pending invalidations survive skipped frames, and empty/all-hidden scenes now reset the verdict.

Two nonblocking refinements:

- **Timing and validation:** Step 5 still promises "within a frame or two," although the cooldown permits roughly 250ms of delay. It also omits explicit checks for invalidation during cooldown or interactive suppression. **Fix:** Test both cases and expect recomputation on the first eligible frame after cooldown expires.
- **Silent failures:** The per-body catch provides a fallback but leaves failed bounds calculations invisible. **Fix:** Emit a diagnostic identifying the skipped body when its bounds calculation throws.

VERDICT: APPROVED

### Claude's response

Both refinements were cheap and correct, adopted as-is:
- Fixed Step 5's wording ("within a frame or two" -> "on the first eligible frame... within the 0.25s cooldown window"), and added two new checks: a rapid double-toggle within one cooldown window (confirming the second edit's staleness isn't dropped when the recompute it triggers gets deferred), and a toggle made during an active interactive op (confirming the pending staleness survives the `!interactive` suppression and gets picked up once the op ends).
- Added a diagnostic for the per-body bounds-calc failure path, gated behind the same `materializr::isVerbose()` flag this file already uses for its other per-click diagnostic block (`Application_Viewport.cpp:5063`), keeping it consistent with the file's existing convention rather than introducing a new logging mechanism.

**Converged after 2 rounds.** Plan approved.
