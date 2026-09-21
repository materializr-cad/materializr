# Plan review log: sketch fillet tool + revolve_sketch AI tool

Plans: `2026-09-20-sketch-fillet-tool.md`, `2026-09-20-revolve-sketch-ai-tool.md`
(the small `2026-09-20-lathe-on-profile-click.md` change is already
implemented; see its own plan).

Codex is usage-capped until 2026-09-22 17:03, so the required Codex plan
review has NOT happened. Independent reviews run instead; the Codex round is
queued and must happen before any implementation code for these two plans and
before anything is pushed upstream.

## Round A - independent adversarial reviewers (2026-09-20)

Two general-purpose read-only reviewers, one per plan, each told to verify
every claim against the code and attack the design. Both returned REVISE.

Fillet plan: tolerances unreachable (buildRegions `onLineTol` 1e-2 mm), float32
claim false, `Equal` constraint does not survive and the solver does run
automatically, `setLineEndpoints` can flip line direction, the apex-through-
cursor radius formula is ill-conditioned, wiring checklist missed the Offset
tool's panel / request-flag / `m_isPlacing` requirements, plus degenerate
inputs and matrix gaps.

Revolve plan: `lastError()` after a failed `pushOperation` is a use-after-free,
D1/D4 contradict on sweep sense, `region_indices` must not be serialized
(ExtrudeOp persists interior points), hardening belongs in the dispatcher not
`execute` (replay), "crosses the axis" is only defined for a coplanar axis,
1e-6 tolerance false-positives on float sketches, plus test-matrix errors.

Response: all accepted; folded into each plan as "Revision 1", which
supersedes conflicting original text. One user-visible decision recorded, not
silently made: the revolve tool's partial-angle sweep sense matches the UI
(no negation) in v1; a user-space right-hand convention is the documented
alternative.

## Round B - Codex (QUEUED for after 2026-09-22 17:03)

Run `claudex-loop:codex-review` on both plans (Revision 1 included) before any
implementation. Same session per plan, MAX_ROUNDS 5.
