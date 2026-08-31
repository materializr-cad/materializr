# Spec Review Log: sketch Offset tool

Adversarial cross-model review of `docs/specs/spec-sketch-offset/`.
Started 2026-08-31. MAX_ROUNDS=5. Reviewer: Codex `gpt-5.6-sol`, read-only every round.

Claude is the final arbiter: critiques are incorporated or rejected with a logged reason.
Revisions route through `.memlog.md` then re-derive `SPEC.md`, per bmad-spec's single-writer rule.

## Round 1 — Codex (`gpt-5.6-sol`, read-only)

## Material findings

1. **The integration plan treats `SketchPlugin.cpp` as a live mode host, but it is dead reference code.** The plugin explicitly says its `SketchModeTool` is not wired to any command or viewport input ([src/plugins/SketchPlugin.cpp:257](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/plugins/SketchPlugin.cpp:257), [src/plugins/SketchPlugin.cpp:262](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/plugins/SketchPlugin.cpp:262)). Implementing Offset there would create an untested second path, while the real path is `Application`.  
   **Fix:** State explicitly that `SketchPlugin.cpp` requires no Offset implementation, or remove it from the claimed mode-host architecture.

2. **CAP-1’s Escape behavior conflicts with the existing sketch state machine.** Escape only calls `onCancel()` for Dimension or when `isPlacing()` is true; otherwise it exits sketch mode ([src/app/Application.cpp:3136](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application.cpp:3136), [src/app/Application.cpp:3170](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application.cpp:3170)). Merely adding `m_offsetActive` like Mirror will not make Escape cancel Offset, and “byte-identical” does not say whether the sketch remains open.  
   **Fix:** Require an explicit Offset-active Escape branch that cancels Offset and returns to Select without exiting the sketch.

3. **“Drag to offset” has no implementable input protocol.** The proposed API has no begin-drag/end-drag methods, and `onMouseDown` has no Offset dispatch ([src/modeling/SketchTool.cpp:102](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:102)); `onMouseUp` is currently generic selection-drag cleanup. The spec never defines whether toolbar activation, mouse-down, hover, or mouse-up starts and commits an offset.  
   **Fix:** Specify exact transitions for activation, mouse-down, drag, mouse-up, typed commit, Cancel, and repeated offsets.

4. **The typed-input assumption is wrong or at least materially incomplete.** The reusable sketch popup appears only when `hasPreview()` is true and calls the general `applyDimension(v)`, accepting only finite positive values ([src/plugins/SketchPlugin.cpp:118](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/plugins/SketchPlugin.cpp:118), [src/plugins/SketchPlugin.cpp:126](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/plugins/SketchPlugin.cpp:126)). Worse, that popup is in the unwired plugin. The real Mirror UI is a bespoke window.  
   **Fix:** Define Offset’s real `Application_Dialogs.cpp` input path, parsing rules, focus behavior, and whether sign is accepted or direction comes exclusively from the cursor.

5. **The OCCT “total mapping” claim is unsupported extrapolation.** One rounded rectangle on one platform does not establish that every valid line/arc loop produces only lines and circles. Self-intersecting loops, concave loops, tangent/reversed arcs, coincident edges, and near-zero radii were not measured. The document nevertheless claims totality “for all valid input.”  
   **Fix:** Narrow the claim to tested fixtures and require runtime topology/curve validation plus tests for concave, self-intersecting, tangent, and near-degenerate loops.

6. **Self-intersecting source and result topology is unspecified.** “Closed loop” does not mean simple Jordan loop. A bow-tie line loop is graph-closed but has no unambiguous inside/outside; large concave offsets may split into multiple wires or self-intersect. Checking only nonzero edge count would accept these cases.  
   **Fix:** Require a simple, non-self-intersecting source wire and validate that the result is the permitted number of closed, non-self-intersecting wires.

7. **Multiple result wires are not addressed.** OCCT returns a `TopoDS_Shape`, not necessarily one wire. The existing SVG helper explicitly takes only the first wire ([src/modeling/SvgImport.cpp:625](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SvgImport.cpp:625), [src/modeling/SvgImport.cpp:640](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SvgImport.cpp:640)). A concave inward offset can split into multiple components; “edge count non-zero” does not decide whether to commit all, one, or refuse.  
   **Fix:** Specify whether multiple closed result wires are committed or rejected, and test the chosen behavior.

8. **Direction selection is ambiguous for concave geometry and circles.** “The side of the loop the cursor is on” lacks a defined nearest-point, winding, or point-in-region rule. In a concavity, the cursor may be globally outside but locally on the inward normal side; at the center of a circle the nearest point is non-unique.  
   **Fix:** Define signed distance using an oriented simple wire plus deterministic nearest-point tie-breaking, or reduce v1 to explicit Inside/Outside controls.

9. **Zero, NaN, infinity, and sub-tolerance distances have no contract.** The spec says nothing about zero or non-finite drag results, and typed parsing behavior is only assumed. OCCT and `glm` should not receive NaN. Zero can return coincident geometry that welding then corrupts.  
   **Fix:** Require finite `|d|` above a documented geometry tolerance and reject zero/sub-tolerance values before OCCT.

10. **Copying Mirror’s welding policy breaks small valid offsets.** `findCoincidentPoint` welds anything within `0.3 * snapScale()` mm ([src/modeling/SketchTool.cpp:1991](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:1991), [src/modeling/SketchTool.cpp:1994](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:1994)). Therefore a valid 0.1 mm offset can weld new endpoints directly onto source endpoints, producing collapsed or nonparallel output.  
    **Fix:** Do not use the UI snap radius for Offset; weld only result-to-result endpoints using OCCT’s much smaller topology tolerance, and never weld to source geometry unless explicitly intended.

11. **Arc conversion is underspecified and risks constructing the complementary arc.** Sketch arcs use a start-to-end CCW convention ([src/modeling/Sketch.cpp:241](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:241)), while OCCT edges carry orientation. Mirror already must swap endpoints after reflection to preserve winding ([src/modeling/SketchTool.cpp:3582](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:3582)). The plan merely says classify with `BRepAdaptor_Curve`; it does not require honoring edge orientation or distinguishing a full circle from an arc.  
    **Fix:** Specify orientation-aware endpoint extraction and explicit full-circle versus partial-arc conversion tests, including major arcs.

12. **The loop validator is not defined tightly enough to be testable.** It must decide how duplicate selected IDs, coincident-but-distinct endpoints, repeated edges, zero-length lines, circle-plus-lines, branches, construction entities, and selected points affect “exactly one closed loop.” `buildWires()` already contains nontrivial splitting of crossings and coincident geometry ([src/modeling/Sketch.cpp:930](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:930), [src/modeling/Sketch.cpp:954](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:954)); recreating only a simple ID walk will disagree with extrusion topology.  
    **Fix:** Define a graph-validation algorithm and tolerances, including degree requirements, permitted entity types, degeneracy checks, and construction-geometry policy.

13. **Stale captured IDs are not protected against undo or deletion during preview.** Mirror captures IDs at activation ([src/modeling/SketchTool.cpp:3429](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:3429)). Offset proposes the same pattern, but the spec does not freeze mutation commands. Undo, Delete, or another edit can remove or replace source entities before preview or commit, leading to partial lookup failure or offsetting different geometry under reused assumptions.  
    **Fix:** Either block undo/redo/delete while Offset is active or snapshot and validate a source-geometry revision before every preview and commit, cancelling on mismatch.

14. **Tool-switch cancellation is only implied, not guaranteed by the proposed state.** `setMode()` explicitly clears Mirror state ([src/modeling/SketchTool.cpp:31](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:31), [src/modeling/SketchTool.cpp:53](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:53)). Adding Offset state without updating this method leaves stale captured IDs and preview state.  
    **Fix:** Make all exits from Offset funnel through `cancelOffset()`, including `setMode`, sketch exit, popup close, Escape, and failed commit.

15. **CAP-2’s “every sampled point” criterion can reject valid offsets and cannot establish correctness.** Sampling does not prove an everywhere property, “perpendicular distance” is undefined at source vertices, and join fillets are offset from a corner rather than perpendicular to a unique source edge. Conversely, sparse sampling can miss localized errors.  
    **Fix:** Test analytic invariants per mapped line/arc plus Hausdorff/minimum-distance bounds, with separate expected rules for join arcs.

16. **CAP-3 overpromises “dimensioned, constrained, trimmed and extruded exactly like drawn geometry.”** Emitting entity types alone does not prove a closed weld graph, valid arc orientation, selection support, solver compatibility, or successful region/extrusion construction. The current `addArc` representation depends on center, endpoints, radius, and CCW interpretation.  
    **Fix:** Add end-to-end acceptance tests that select and dimension generated entities, build closed wires/regions, trim them, save/reload, and extrude the resulting ring.

17. **CAP-5 hardcodes one collapse threshold without defining sign/orientation.** “Inward by 11 mm” is only meaningful after direction conventions are fixed. For the rounded rectangle used by the spike, topology changes before collapse; other loops can split rather than become empty.  
    **Fix:** Define inward independently of wire orientation and test empty, split, self-intersecting, and topology-changing outcomes separately.

18. **The selection and construction policies are left as open questions despite affecting the contract.** Post-commit selection determines repeated-offset behavior and whether generated circles/arcs can be manipulated immediately. Construction status determines whether construction loops can be offset and whether output participates in extrusion. These are implementation-blocking choices, not optional polish.  
    **Fix:** Resolve both before implementation: define source selection after commit, all generated entity selection sets, and construction-flag inheritance/output behavior.

19. **“No new untrusted-input parsing surface” is a false security claim.** Typed numeric text is still input parsing, and OCCT is receiving user-created potentially degenerate geometry. Reusing existing serialization does not remove the need for finite/range checks and exception containment.  
    **Fix:** Replace that claim with explicit validation bounds and require exception-safe OCCT invocation with no partial commit.

20. **Error delivery is not integrated.** The analogous toolbar action only prints Mirror failure to stdout ([src/app/Application.cpp:2329](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application.cpp:2329), [src/app/Application.cpp:2334](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application.cpp:2334)), but CAP-4/CAP-5 require user-visible, distinct messages. The proposed `beginOffset()` Boolean cannot carry the “actual problem.”  
    **Fix:** Define a structured result/error enum from validation and commit, and route each failure through the app’s toast UI.

VERDICT: REVISE
### Claude's response — Round 1

I verified every finding I acted on against the source rather than taking it on trust.

**Confirmed by reading the code — accepted in full:**

- **F1 (SketchPlugin is dead code).** `REGISTER_PLUGIN(Sketch, ...)` states the tool is
  "intentionally NOT wired to any command or button ... kept only as a reference for a
  future input-routing migration" (`SketchPlugin.cpp:257-266`). My `integration-points.md`
  called it the mode host. That was wrong, and it reopens the CONTRIBUTING question I had
  told the user was settled. Rewritten; Application is the only host.
- **F2 (Escape).** `Application.cpp:3170` — inside sketch mode, Escape cancels only for
  Dimension or `isPlacing()`, else it calls `exitSketchMode()`. With `m_offsetActive` set
  and `isPlacing()` false, CAP-1's "Escape cancels" would have **exited the sketch
  entirely**. Sharpest finding of the round. Explicit Offset branch now required.
- **F4 (typed popup).** The `##SketchDim` popup I assumed reusable is in the dead plugin.
  The live one is `##SketchDimInput` (`Application_Viewport.cpp:7032-7118`), gated on
  `hasPreview()` with a per-mode label switch at :7055. Reusable, but via a different
  mechanism than the spec claimed, and `hasPreview()` must be made true for Offset.
- **F10 (weld radius).** `findCoincidentPoint` uses `0.3f * snapScale()`
  (`SketchTool.cpp:1991`) — a UI snap radius. A valid 0.1 mm offset would weld its output
  straight onto the source. Genuine bug avoided.
- **F14 (setMode).** `setMode` clears `m_mirrorActive` explicitly (`SketchTool.cpp:53`);
  Offset state needs the same or it survives a tool switch.
- **F20 (error delivery).** Mirror only does `fprintf(stdout, ...)` (`Application.cpp:2334`),
  which cannot satisfy CAP-4/CAP-5. `Application::showToast(text, seconds)`
  (`Application.h:120`) already exists and is the right channel.

**Confirmed by a second spike rather than by argument — accepted:**

Findings 5, 6, 7 and 8 all questioned evidence I did not have, so I measured instead of
answering. New fixtures: concave L, dumbbell (pinching neck), bow-tie, lone circle.

- **F7 is real and I had missed it structurally.** `dumbbell -3` returns **wires=2**. My
  first spike counted edges via `TopExp_Explorer(TopAbs_EDGE)` and never counted wires, so
  it could not have detected a split. The "nonzero edge count" guard is insufficient.
- **F6 is real and worse than stated.** The bow-tie returns wires=1, edges=6,
  `BRepCheck_Analyzer::IsValid() == yes`. A self-intersecting source produces confident,
  structurally valid, meaningless output. Source simplicity must be validated up front.
- **F9 confirmed.** Offsets of `0` and `1e-9` both succeed and return coincident geometry.
- **New, from neither of us:** `circle -10` and `circle -12` return `NOT DONE`, a
  *different* failure mode from the rectangle's IsDone-with-empty-shape. Both paths need
  handling; the spec described only one.
- **F5 partially upheld.** `Other=0` held across every new fixture, which strengthens the
  claim but does not make it universal. Narrowed to the tested fixtures, as asked.

**Accepted with narrowed wording, not as written:**

- **F19 ("no new untrusted-input parsing surface" is a false security claim).** The
  substance is right and the wording is now fixed, but the finding overstates. The claim
  was about *file-format* parsing — the `ParamParse.h` budget/overflow surface hardened
  earlier in this repo — and that part is true: no new `serializeParams` means no new
  blob parser. Typed numeric entry is a different and much smaller surface, already
  handled by the existing `parseFinite`. Reworded to say exactly that, plus explicit
  finite/range bounds and exception-safe OCCT invocation with no partial commit.
- **F18 (open questions are implementation-blocking).** Agreed for two of the three:
  post-commit selection and the construction-flag policy both change the contract, and
  are now decided in the spec. The keyboard-shortcut question is genuine polish — the
  toolbar button is a complete entry point on its own — so it stays open rather than
  being resolved to pad a convergence.

**Rejected: none.** Round 1 found no finding I could show to be wrong.

## Round 2 — Codex

The revision closes most first-round findings, especially OCCT failure modes, stale-source detection, Escape behavior, and runtime curve validation. Several material contradictions remain.

1. **The stale-source hash invalidates itself immediately after every successful commit.** The state machine captures the whole-sketch `geometryHash()` at Arm, compares it before every preview, then transitions `Committed → Armed`. But adding offset entities changes that hash because it includes every point, line, circle, and arc ([src/modeling/Sketch.cpp:1562](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:1562), [src/modeling/Sketch.cpp:1580](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:1580)). The first preview after commit therefore detects its own output as a stale-source edit and cancels.  
   **Fix:** Hash only captured source entities, or explicitly refresh the whole-sketch hash after a successful commit while separately verifying the captured source geometry.

2. **The contract specifies two conflicting typed-input interfaces.** `integration-points.md:34` requires a Mirror-style floating panel with a distance field, while `state-machine.md:50-54` requires reuse of `##SketchDimInput`. These have different lifecycle and close semantics; the inline popup has no close button because `ImGui::Begin` receives `nullptr` ([src/app/Application_Viewport.cpp:7118](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application_Viewport.cpp:7118)), yet the state machine says “popup close” cancels Offset.  
   **Fix:** Choose exactly one UI path and remove the other; define cancellation according to that path’s actual controls.

3. **Adding a label case is insufficient to wire typed Offset commits.** Both desktop and touch branches ultimately call `SketchTool::applyDimension(v)` ([src/app/Application_Viewport.cpp:7229](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application_Viewport.cpp:7229), [src/app/Application_Viewport.cpp:7244](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application_Viewport.cpp:7244)). `applyDimension` requires `m_isPlacing` and handles modes through an explicit switch whose default refuses the value ([src/modeling/SketchTool.cpp:398](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:398), [src/modeling/SketchTool.cpp:407](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:407), [src/modeling/SketchTool.cpp:554](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:554)). Offset explicitly never sets `m_isPlacing`.  
   **Fix:** Require an Offset-specific dispatch before `applyDimension`, or formally extend `applyDimension` to accept active Offset and propagate its structured commit error to a toast.

4. **The geometry tolerance is still not documented.** The spec repeatedly requires a “documented geometry tolerance” and “OCCT topology tolerance,” but no numeric value or derivation appears in any of the four files. This leaves zero-distance refusal, duplicate vertices, self-intersection, result welding, wire closure, and test expectations implementation-dependent.  
   **Fix:** Define named tolerances with units and scaling policy—for example minimum offset, endpoint coincidence, zero-length edge, and intersection tolerances—and use them in acceptance criteria.

5. **Direction still lacks a complete orientation rule.** `state-machine.md:36` derives sign from an “oriented source wire” but never says how unordered selected entities are oriented or which winding is canonical. A selected circle has no endpoint walk to establish orientation, and OCCT’s signed `Perform` direction must then be mapped consistently to the chosen outward normal.  
   **Fix:** Require reconstruction into a canonical winding—such as positive signed area for non-circles—define circle orientation explicitly, and state which `Perform` sign means outward after canonicalization.

6. **“Clamp to tolerance floor” contradicts refusal below tolerance.** `state-machine.md:36` says cursor magnitude is clamped to the floor, while `SPEC.md:40` and `SPEC.md:51` say `|d|` below tolerance is refused before OCCT. Thus mouse-up directly on or very near the source either commits a minimum-sized offset or must fail.  
   **Fix:** Choose one behavior; preferably allow no preview/commit below tolerance and surface the documented small-distance error on commit.

7. **All-or-nothing commit is required but no implementable transaction strategy is specified.** `Sketch` mutators append directly ([src/modeling/Sketch.cpp:96](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:96), [src/modeling/Sketch.cpp:199](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:199), [src/modeling/Sketch.cpp:210](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:210)). `recordSketchMutation` records before/after for undo but does not roll back an exception or partial failed conversion. The required “OCCT exception mid-conversion leaves the sketch untouched” will not follow from the wrapper.  
   **Fix:** Require complete conversion into an intermediate entity plan before mutating `Sketch`, then perform a non-throwing validated commit, or explicitly restore a snapshot on failure.

8. **The source-loop validation remains outcome-based rather than algorithmically specified.** The revision lists cases to reject, but does not define how line-line, line-arc, arc-arc, tangential, overlapping, and endpoint-touch intersections are classified. This matters because the existing region builder performs substantial synthetic crossing and curve splitting rather than a simple degree walk ([src/modeling/Sketch.cpp:930](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:930), [src/modeling/Sketch.cpp:954](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:954), [src/modeling/Sketch.cpp:1044](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.cpp:1044)).  
   **Fix:** Specify the intersection predicates and permitted endpoint contacts, using the newly defined tolerances, and add line-arc and arc-arc fixtures.

9. **The architecture question is implementation-blocking despite being labeled open.** `SPEC.md:59` mandates `Application` as the only host, while `SPEC.md:83` acknowledges that this violates the repository’s explicit rule that new tools live in registered plugins. That rule is real and unambiguous ([CONTRIBUTING.md:41](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/CONTRIBUTING.md:41), [CONTRIBUTING.md:45](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/CONTRIBUTING.md:45)). An implementer cannot simultaneously obey the spec and contribution policy.  
   **Fix:** Obtain and record the maintainer decision now, or scope and specify the minimal plugin infrastructure needed; do not leave the ownership decision open in an implementation contract.

VERDICT: REVISE
### Claude's response — Round 2

**Verified and accepted:**

- **F1 (the hash self-invalidates) — best finding of the run.** `Sketch::geometryHash()`
  is FNV-1a over every point, line, circle and arc (`Sketch.cpp:1562+`). Committing offset
  output therefore changes it, and because the machine re-arms after commit, the next
  preview would detect its own output as a stale-source edit and cancel. The guard I added
  in round 1 to fix a real problem introduced a new one. Now scoped to a hash over the
  captured source entities only.
- **F3 (typed entry cannot work as specced).** `applyDimension` opens with
  `if (!m_sketch || !m_isPlacing || value <= 0.0f) return false;` and its mode switch ends
  `default: return false` (`SketchTool.cpp:398-407, 554`). Offset never sets `m_isPlacing`.
  Adding a label case would have produced a field that silently does nothing.
- **F2 (two conflicting UI paths).** Correct, and both were mine — `integration-points.md`
  described a Mirror-style panel while `state-machine.md` described reusing
  `##SketchDimInput`. Resolved in favour of the bespoke panel, which also dissolves F3:
  a panel with its own commit button never touches `applyDimension`.
- **F6 (clamp vs refuse).** A direct contradiction between two sentences I wrote. Resolved
  to refuse: no preview and no commit below tolerance.
- **F4 (tolerances undocumented), F5 (winding rule incomplete), F7 (no transaction
  strategy), F8 (validation predicates unspecified).** All fair. Each is now specified
  concretely rather than gestured at.

**Escalated rather than resolved — F9 (architecture).**

Codex is right that this cannot stay an open question in an implementation contract, and
right that the rule is unambiguous: `CONTRIBUTING.md:45` says a new tool "belongs in
`src/plugins/`" and "Don't bolt feature logic into the core `Application`." It is equally
true that every existing sketch tool lives in `Application`, and that the one sketch
plugin is deliberately unwired.

So an implementer genuinely cannot satisfy both the spec and the contribution policy. But
the resolution is a maintainer's call about their own repo's direction — follow the
documented rule and build the missing input-routing infrastructure, or follow the actual
code and amend `CONTRIBUTING.md`. Choosing either one unilaterally to close a review
finding would be the wrong kind of convergence. Carried to human gate #2 as a blocking
decision, flagged as blocking rather than left as polish.

## Round 3 — Codex

The revisions resolve the previous tolerance, winding, validation, transaction, and stale-hash objections. A few material issues remain.

1. **Activating Offset clears the source selection that the contract says must stay selected.** The transition captures IDs and then calls `setMode(Offset)`, but `setMode` clears every element selection for any mode other than Select ([src/modeling/SketchTool.cpp:31](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:31), [src/modeling/SketchTool.cpp:37](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.cpp:37)). Consequently the source is not visibly selected during the operation or after commit unless explicitly restored.  
   **Fix:** Require `beginOffset` to restore the captured full selection after `setMode`, and require re-arming and cancellation paths to preserve or restore it deliberately.

2. **The integration trace omits two required files for the bespoke panel.** A new `renderOffsetToolPanel()` needs a declaration alongside `renderMirrorToolPanel()` in `Application.h` ([src/app/Application.h:125](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application.h:125), [src/app/Application.h:128](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application.h:128)) and a per-frame call beside the existing Mirror panel ([src/app/Application.cpp:7568](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/app/Application.cpp:7568)). Neither appears in `integration-points.md`.  
   **Fix:** Add `Application.h` and the panel invocation site in `Application.cpp` to the mandatory integration points.

3. **`integration-points.md:16` still contains the retired popup claim.** It says the live popup is `##SketchDimInput` and directs the implementer to the state machine, while the state machine now correctly mandates a bespoke `Application_Dialogs.cpp` panel.  
   **Fix:** Replace that sentence with the bespoke-panel decision so the canonical companions do not contradict one another.

4. **CAP-2’s Hausdorff criterion remains undecidable.** It requires “a Hausdorff bound” but specifies neither the bound, tolerance, directed-versus-symmetric definition, nor fixture. For topology-changing but accepted offsets, symmetric Hausdorff distance need not equal `|d|`.  
   **Fix:** Define the exact fixture, directed/symmetric metric, sampling or analytic procedure, and numerical acceptance bound—or remove the redundant Hausdorff requirement and rely on the analytic entity invariants.

5. **The `1e-6 mm` predicates are below reliable resolution for the sketch’s coordinate representation at ordinary CAD extents.** Sketch positions are stored as `glm::vec2`, hence single-precision floats ([src/modeling/Sketch.h:17](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.h:17), [src/modeling/Sketch.h:19](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.h:19)). At coordinates around 100–1000 mm, float spacing already exceeds `1e-6 mm`; endpoint welding and intersection classification can therefore change merely with translation away from the origin.  
   **Fix:** Base predicates on shared point/topological identity where possible and choose a tolerance compatible with float ULP at supported sketch extents, or perform validation in double precision with a documented scale-aware term.

6. **Immediate typed commit has no initialized direction.** The panel opens in Armed state with `d = 0`, while typed magnitude uses “whatever the cursor last indicated.” A user can type and press Enter before any Offset-mode mouse move, leaving the sign dependent on stale cursor state or an unspecified default.  
   **Fix:** Initialize direction deterministically to outward at Arm, then allow cursor motion to change it.

7. **Duplicate-edge validation does not match the actual selection representation or CAP-4 wording.** Selected entity IDs are stored in `std::set<int>` ([src/modeling/SketchTool.h:90](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.h:90), [src/modeling/SketchTool.h:125](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/SketchTool.h:125)), so duplicate IDs cannot reach the validator. The meaningful bad case is two distinct IDs representing coincident geometry, but `tolerances.md` only names duplicate IDs explicitly.  
   **Fix:** Define “duplicate edge” as geometrically coincident distinct entities and specify its classification/toast; retain duplicate-ID checking only as defensive validation.

8. **The architecture escalation is valid, but it still prevents approval to implement.** The spec simultaneously mandates `Application` as the only host and declares that choice incompatible with the repository’s contribution policy. I agree this is a maintainer decision rather than something the spec author should silently invent, but an explicitly unresolved blocker still means the contract is not implementable yet.  
   **Fix:** Record the maintainer’s decision, then update either the host constraint or the contribution policy/infrastructure requirement.

VERDICT: REVISE
### Claude's response — Round 3

**Verified and accepted:**

- **F1 (activation clears the selection it promises to keep).** `setMode` runs
  `if (mode != SketchToolMode::Select) clearElementSelection();` (`SketchTool.cpp:37`).
  So `capture ids; setMode(Offset)` leaves the source *unselected* — directly contradicting
  the "source stays selected" decision made in round 1. `beginOffset` must restore the
  captured selection after `setMode`, and re-arm must preserve it.
- **F5 (1e-6 mm is below float resolution) — the best numerical finding of the run.**
  `SketchPoint::pos` is `glm::vec2`, i.e. single precision (`Sketch.h:19`). Measured ULP:
  1.2e-7 mm at 1 mm, **7.6e-6 mm at 100 mm**, 6.1e-5 at 1000 mm, 9.8e-4 at 10000 mm. So the
  `1e-6` predicates are below representable resolution across most of the working range —
  they degrade to exact equality and change meaning as a sketch is translated away from
  the origin. At 10000 mm the ULP nearly reaches `kMinOffsetDistance` itself.
  Two fixes, and the first matters more: adjacency is now built on **shared point ids**
  rather than positions, since `SketchLine` already references `startPointId`/`endPointId`
  — exact, scale-free, and correct by construction. Positional welding survives only as a
  fallback for coincident-but-distinct points, now scale-aware and evaluated in double.
- **F2, F3 (integration trace incomplete and self-contradictory).** Both correct.
  `renderMirrorToolPanel()` is declared at `Application.h:128` and invoked at
  `Application.cpp:7569`; neither site was listed. And `integration-points.md` still
  pointed at `##SketchDimInput` after `state-machine.md` had moved to a bespoke panel —
  I updated one companion and not the other.
- **F6 (typed commit before any mouse move has no direction).** Real. Direction now
  initialises to outward at Arm.
- **F7 (duplicate ids cannot reach the validator).** Correct — selections are
  `std::set<int>` (`SketchTool.h:90`). The real hazard is two *distinct* ids holding
  coincident geometry. Reworded; the id check stays as cheap defensive validation.

**Accepted by removal rather than by specification:**

- **F4 (Hausdorff bound is undecidable as written).** Fair, and Codex offered the right
  remedy as an alternative. The analytic per-entity invariants already pin every generated
  line and arc; the Hausdorff clause added no coverage and named neither metric, bound,
  nor fixture. Removed rather than elaborated — specifying it properly would have been
  ceremony around a check that was already redundant.

**F8 (architecture) — standing disagreement, carried forward deliberately.**

Codex agrees this is a maintainer decision and not mine to invent, but holds that an
explicitly unresolved blocker means the contract is not yet implementable. That position
is correct, and I am not going to manufacture a resolution to clear it. This will be
carried to the human as the tie-break rather than silently converged.

## Round 4 — Codex

The round-3 changes are coherent, but the architecture decision is not the only remaining issue. Two technical contradictions remain.

1. **The positional fallback is defined but cannot participate in adjacency as written.** `tolerances.md:41` says entities meet only when they reference the same point ID, while `tolerances.md:29`, `:33`, and `:50` describe positional welding for coincident-but-distinct points, especially imports. Under the stated graph rule, a geometrically closed imported loop with distinct coincident endpoint IDs instead has degree-1 vertices and is rejected before that fallback helps.  
   **Fix:** Define adjacency as shared-ID first, then union otherwise-distinct endpoint IDs within `weldTol`; clarify that ordinary drawn loops take the exact-ID path while imported geometry may use the fallback.

2. **The source-scoped hash omits geometry-defining center positions.** `state-machine.md:56` says to hash IDs, endpoint positions, and radii. A circle’s geometry also depends on its center point ([src/modeling/Sketch.h:43](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.h:43), [src/modeling/Sketch.h:45](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.h:45)); an arc depends on center, start, and end points ([src/modeling/Sketch.h:50](/Users/laptop/Documents/Coding%20projects/Materialzr/materializr/src/modeling/Sketch.h:50)). Moving a circle or arc center while Offset is active could therefore leave the hash unchanged.  
   **Fix:** Hash entity type, entity ID, every referenced point ID and position—including circle/arc centers—and all geometry-defining scalar fields.

3. **The architecture blocker still prevents implementation approval.** The escalation is appropriate and honestly stated, but until the maintainer chooses the Application-host exception or plugin infrastructure path, the implementer has no authorized architecture. Once the two technical points above are corrected, this maintainer decision would be the only remaining blocker.

VERDICT: REVISE
### Claude's response — Round 4

Both technical findings accepted; both were regressions introduced by round-3 fixes.

- **F1 — my id-based adjacency broke the import case.** Switching adjacency to shared point
  ids fixed the float-precision problem but silently created a new one: an imported loop can
  be geometrically closed while every junction holds two *distinct* coincident ids. Under
  id-equality alone that loop presents as a chain of degree-1 vertices and is rejected as
  "open". Adjacency is now two-stage — exact id match first, then a `weldTol` union of
  still-distinct endpoints — so drawn geometry stays exact and imported geometry still works.
- **F2 — the source hash missed circle and arc centres.** `SketchCircle` is
  `{centerPointId, radius}` and `SketchArc` is `{centerPointId, startPointId, endPointId,
  radius}` (`Sketch.h:43-58`). Hashing "ids, endpoint positions and radii", as I wrote it,
  would leave a dragged circle centre invisible to the staleness check — the exact class of
  bug the check exists to catch. Now specified as every referenced point id and position
  plus all scalar fields.

Round 4 is the second time a fix of mine introduced a defect that the next round caught
(round 2's `geometryHash` was the first). Worth recording: the loop is earning its keep on
the revisions, not only on the original draft.

**F3 — architecture.** Codex states plainly that once these two are corrected, the
maintainer decision is the only remaining blocker. That is the sentence the human needs,
and it is why round 5 is worth spending.

## Round 5 — Codex (final round; MAX_ROUNDS reached)

Two technical issues remain; therefore the architecture decision is not the sole outstanding item.

1. **The canonical documents contradict each other about positional endpoint union.** `SPEC.md:54` says adjacency is built on shared point IDs “not positions,” while `tolerances.md:41-46` correctly requires a second positional-union stage for imported geometry. Both files are canonical, so an implementer could follow either rule.  
   **Fix:** Change `SPEC.md:54` to say adjacency uses exact shared IDs first, followed by the documented `weldTol` fallback union.

2. **Result validation is still too weak.** `SPEC.md:51` declares four sufficient checks: done, nonempty, exactly one wire, and supported curve types. Those do not establish that the single result wire is closed, topologically valid, or non-self-intersecting. The spikes measured `BRepCheck_Analyzer`, but the contract does not require it, and source simplicity does not prove every large or numerically difficult offset result is simple.  
   **Fix:** Require the result wire to be closed, pass `BRepCheck_Analyzer`, and pass the same non-self-intersection predicate before conversion or commit.

The maintainer architecture decision also remains blocking, as intentionally documented.

VERDICT: REVISE
### Claude's response — Round 5

Both findings accepted and applied.

- **F1 — a contradiction I introduced.** Round 3 changed adjacency to shared point ids and
  I updated `tolerances.md` to the correct two-stage rule but left `SPEC.md:54` reading
  "shared point ids, **not positions**". Two canonical documents, two different rules.
  `SPEC.md` now states the two-stage rule.
- **F2 — result validation was genuinely too weak.** The four checks establish that a
  single wire came back with representable curves; they do not establish that it is
  **closed**, topologically valid, or non-self-intersecting. Source simplicity does not
  imply result simplicity at large or numerically awkward offsets. The spikes ran
  `BRepCheck_Analyzer` and saw `valid=yes` throughout, but that is a measurement on small
  well-conditioned fixtures, not a guarantee, and the contract never *required* the check.
  Now six checks, plus the source's non-self-intersection predicate re-applied to the
  result before conversion.

## Outcome — MAX_ROUNDS reached without APPROVED

Recorded honestly: this did **not** converge to `VERDICT: APPROVED`.

Five rounds produced 39 findings. Every one was verified against the source before being
accepted; none was rejected as wrong. Two were narrowed rather than taken as written
(the security-surface claim, and which open questions are truly blocking), and one was
accepted by deleting the requirement rather than specifying it (the Hausdorff bound).

Three rounds caught defects in *fixes from previous rounds* — `geometryHash` self-
invalidating, id-adjacency breaking imported loops, and the `SPEC.md`/`tolerances.md`
contradiction. That is the loop's clearest value: the original draft's flaws were mostly
omissions, but the revisions introduced new bugs that only re-review found.

**Two items remain outstanding, and they are different in kind:**

1. **The round-5 fixes are applied but unreviewed.** The cap fell before a sixth round
   could check them. Given rounds 2, 4 and 5 each found a defect in the prior round's
   fixes, the honest prior is that these may also carry one.
2. **The architecture decision is the maintainer's and is genuinely blocking.**
   `CONTRIBUTING.md:45` requires a new tool in `src/plugins/`; every sketch tool is in
   `Application`; the one sketch plugin is deliberately unwired. No implementer can satisfy
   both the spec and the policy. Codex and I agree on the facts and on who decides.
