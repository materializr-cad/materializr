# Codex adversarial review log

The adversarial review the app-wide display-units plan went through before any
code was written. Five rounds, 34 findings, 32 accepted; the two rejected are
argued below and one of those the reviewer later withdrew itself.

Started 2026-09-03. MAX_ROUNDS=5. Reviewer: Codex `gpt-5.6-sol`, CLI 0.144.1,
read-only every round.

Kept because the reasoning is worth more than the verdict: several decisions in
the units code look arbitrary until you can see which alternative was tried and
what broke. Round 2 findings 1 and 3 in particular are two crash classes caught
before they were written.

## Round 1 - Codex: DID NOT RUN (backend 404)

Two attempts at 15:16 and 15:17. Threads `01a067d6-…` (killed by my own `| head -1`
SIGPIPE - my error) and `01a067d7-a5aa-7303-89b2-19a50bb639d2` (genuine failure). Codex's
first request, `GET https://chatgpt.com/backend-api/codex/models?client_version=0.144.1`,
returned `404 Not Found` (cf-ray AMS), then the responses websocket 404'd on every retry.
A trivial `"Reply OK"` exec fails identically, so it is not plan-, prompt- or
model-related (`gpt-5.6-sol` is still in `models_cache.json`; it fails before model
selection). Not a usage limit, not capacity - a new failure class: endpoint 404.
No critique received. The plan's own independent adversarial pass (see Risks) stands as
the only cross-check so far.

Probe at 11:54:54 (after a 9-minute wait): trivial exec returns `OK` - backend restored.
Round 1 re-launched.

## Round 1 - Codex (thread `01a067fb-ac64-7620-87d3-0f8b5013557a`): VERDICT: REVISE

18 findings, each verified against code by Codex. Condensed (full text in the scratchpad
verdict file this session):
1. `InputDouble` cannot retain a `2in` suffix - the popup's `inputNumber` edits a double and
   rewrites the buffer from it; `parseLength` afterwards recovers nothing. → InputText.
2. Neither the popup nor `PropertiesPanel.cpp:890` calls `VariableManager::evaluate`; the
   plan's "parseLength → evaluate" ordering describes code that does not exist.
3. Properties constraint editor: `shown` (`:862`), buffer refill, `padVal` and commit must
   convert as one transaction; radius converts before halving.
4. `inputLength` sentinel defaults are defeated: nearly every caller passes explicit
   `0.1/1.0` + `%g`/`%.3f`, which would become display-unit steps.
5. Unit switch while an input is active → value interpreted in a different unit than shown.
6. `EnterReturnsTrue` ≠ focus-out; `PropertiesPanel.cpp:891` names it `justDeactivated` but
   never checks `IsItemDeactivatedAfterEdit` - pre-existing bug the wrapper would inherit.
7. Missed length inputs: `ConstructionPlaneOp.cpp:266`, `AlignOp.cpp:85`,
   `RevolveOp.cpp:283`, `ProjectSketchOp.cpp:536`, `SectionPanel.cpp:50`,
   `Application_Dialogs.cpp:5178` (Unfold thickness).
8. Sliders (section offset, text height, chord, SVG width, ref-image, pattern distance)
   have mm min/max/step; a converted value against mm bounds is off by up to 304.8×.
9. Sketch-offset editor `Application_Dialogs.cpp:4401` `inputNumber("%.3f mm")` missed.
10. Touch `amountField` mutates its pointer directly (`EdgeOpController.cpp:827`) - needs a
    shared wrapper, not per-site conversion.
11. Controller drag paths: define the authoritative mm member and when display buffers
    reseed, or a stale buffer overwrites a newer drag value.
12. More literals: `ThreadOp.cpp:2142/2160`, `ResizeCylindricalOp.cpp:707`,
    `ScaleFaceOp.cpp:316`; catalogue strings "Body spans", "automatic", "lead", "Snap step".
13. "Every caption formats live" is false: `ReplayOp` returns stored `m_description`,
    `BatchTransformOp` stored `m_desc`; Align emits raw unitless coordinates.
14. i18n: `Diameter (mm)` must become a `%s` template, not concatenation.
15. Test 3 says `^2`, Decisions say `²` - contradiction. Volume declared, never formatted or
    tested.
16. Tests miss the dangerous workflows (halving order, active-unit edit, drag races, slider
    bounds, touch field, Properties path).
17. Global mutable `currentUnit()` makes the invariant unenforceable; tests order-dependent.
18. Simpler alternative: complete `LengthField/LengthSlider/LengthText` widgets taking and
    returning mm, not scalar helpers + ad-hoc shadows.

### Response

**Accepted 1–16 and the widget half of 18** - see "Revisions after Round 1" below. Of note,
2 and 13 are over-claims of mine that Codex caught by reading the code: expression support
does not exist at the named sites, and two `description()`s return stored strings.

**Rejected 17 (and the "drop the global" half of 18), with reason.** Threading an immutable
`DisplayUnits` context through ~60 UI call sites is a large refactor of a single-threaded
ImGui app, and the codebase already uses exactly this shape for language (`tr()` reads a
process-global). Mitigations adopted instead: the setter is called from `Application` only
(settings apply + combo); `Units.h` is included from `src/ui` and `src/app` - never from
`src/modeling` or `src/core` model code - and a grep for `Units.h` under `src/modeling`
is a review check; tests use an RAII `ScopedUnit` restorer so no state leaks between cases.
If a background renderer ever appears, this is the first thing to revisit, and the header
says so.

## Round 2 - Codex (same thread): VERDICT: REVISE

10 findings, all verified:
1. R2's `ImGui::ClearActiveID()` inside `setCurrentUnit()` crashes at startup -
   `Application.cpp:898` applies settings before an ImGui context exists (`GImGui` null) -
   and contradicts `Units.h` having no ImGui dependency.
2. Test 15's "injected clear-active hook" adds mutable callback state to the unit layer just
   to be testable; ownership/reset unspecified.
3. R5's "`IsItemActive()` at the top of every frame" is not implementable: it describes the
   LAST submitted item, so it would inspect an unrelated widget and reseed the active field.
4. `lengthTextField` cannot edit angles, yet both target locations hold Angle constraints;
   routing an angle through it applies length scaling.
5. R4's pseudocode parses every commit as a length before branching on angle, and leaves
   `typed` undefined.
6. `lengthText` passes a `std::string` to an ImGui variadic `%s` - UB.
7. The "never include `Units.h` from `src/modeling`" boundary conflicts with R8, which has
   `ExtrudeOp` etc. call `fmtLength()` from `description()`. An indirect include evades the
   grep while keeping the coupling. Don't claim the include boundary enforces the invariant.
8. Therefore the mitigations for 17 are insufficient as written: startup setter use before
   ImGui, and model `description()` methods observing presentation state. RAII does settle
   the test-order concern.
9. Commit plan, Verification and Risks still describe the superseded `inputLength` +
   "mechanical one-word rename" strategy.
10. Test 10 still factors its helper out of `inputLength`.

### Response
**Accepted all ten.** 1 and 3 are bugs I would have shipped. 7/8 are right and I'm
conceding the point honestly rather than moving the grep: `description()` methods ARE
presentation methods that happen to live on modeling classes; the plan now says so and
stops pretending an include boundary enforces the invariant. What remains of the 17
disagreement is narrow - a UI-independent process-global setting vs a threaded context -
and Codex's own text accepts RAII for the test-leak half. See "Revisions after Round 2".

## Round 3 - Codex (same thread): VERDICT: REVISE

Opens: "the retained global unit state is acceptable for this single-threaded application"
- finding 17 withdrawn. Four concrete issues:
1. Commit 2 calls `Application::applyDisplayUnitChange`, introduced only in commit 3 -
   not buildable as sequenced.
2. Commit 1 lists test 10, which tests `lengthFieldCommit` from commit 3; commit 3 then
   lists 10–16 again.
3. `lengthSlider` is `double*` only; the target sliders (`m_sectionOffset`, text height,
   airfoil chord, ref-image width, pattern distances) are `float*`. Ad-hoc shadows would
   undermine the centralised design.
4. R14's `char out[128]` silently truncates longer translated sentences - a new i18n bug.

### Response
**Accepted all four.** 1 and 2 were sequencing slips from rewriting the commit plan in
place; 3 is the same float-member class Risks 4 already names, applied to sliders; 4 is
correct - several translated readouts are sentences, not labels. Fixes: `applyDisplayUnitChange`
moves to commit 2; commit 1 carries tests 1–7 only; `lengthSlider` gets a `float*` overload
through a local double; `lengthText` sizes dynamically with a counting `snprintf` pass.

## Round 4 - Codex (same thread): VERDICT: REVISE

Two gaps:
1. R12 reads `IsItemActive()` AFTER `InputText` has consumed `buf`, so an external model
   change or a unit switch shows a stale buffer for one frame - the reseed decision must be
   made BEFORE submitting the item.
2. R14's one- and two-quantity overloads can't express verified readouts: three coordinates
   (`PropertiesPanel.cpp:83`), count + three coordinates (`Application_Viewport.cpp:4501`),
   count + area/length (`PropertiesPanel.cpp:165`) - they'd fall back to ad-hoc formatting.

### Response
**Accepted both.** 1: the activity test moves before the item, using the field's stable ID
(`ImGui::GetActiveID() == ImGui::GetID(label)` in the same ID scope), and the buffer is
seeded on that result before `InputText` runs - R12 rewritten. 2: replaced the fixed
overloads with one type-safe variadic translated formatter plus a `fmtVec3` helper - R14
rewritten. Both are precisely the "ad-hoc shadow / ad-hoc formatting" failure modes the
widget-centralisation exists to prevent, so they belong in the widgets, not at sites.

## Round 5 - Codex (same thread): VERDICT: APPROVED

"No material flaws remain. The revised plan now covers conversion ordering, active edits,
float shadows, widget bounds, i18n formatting, audit completeness, and buildable commit
sequencing." Two non-blocking cleanups, both applied: the mutation checklist renamed
`applyLengthEdit` → `lengthFieldCommit`; R12 notes `ImGui::GetActiveID()` needs
`<imgui_internal.h>` (already used elsewhere in the repo).

### Outcome
Converged in 5 of 5 rounds - 34 findings raised, 32 accepted, 2 rejected with reasons (one
later withdrawn by Codex itself). What the argument changed, in order of consequence:
scalar helpers became complete mm-in/mm-out widgets; expression support was removed as a
false claim; two crash/heisenbug classes (`ClearActiveID` pre-context, `IsItemActive` on the
wrong item) were caught before code; the include-boundary claim was replaced with an honest
statement; an audit inventory became a commit; seven missed sites were added.
