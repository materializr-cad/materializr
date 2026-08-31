# OCCT offset behaviour — measured, not assumed

Companion to `SPEC.md`. Three assumptions the design rested on were settled by a throwaway spike before the spec committed to them. Recorded here so an implementer does not re-litigate them, and so a future OCCT bump has a baseline to re-measure against.

**Measured against:** OCCT 7.9.3, macOS 15 / Apple M4, 2026-08-31. Version confirmed, not assumed: `/opt/homebrew/opt/opencascade` → `Cellar/opencascade/7.9.3`, and `Standard_Version.hxx` defines `OCC_VERSION_COMPLETE "7.9.3"` — the version CMake pins.

**Method:** a standalone ~90-line program, deliberately not kept. It built two wires and called `BRepOffsetAPI_MakeOffset(wire, GeomAbs_Arc)` then `Perform(d)`, classifying every result edge with `BRepAdaptor_Curve::GetType()`.

- **Closed source:** 40×20 rectangle with the two right-hand corners rounded at r=5 — 6 edges (4 `Line`, 2 `Circle`). Chosen because it mixes both curve types and has a finite inner radius, so inward offsets can be pushed past collapse.
- **Open source:** an L of two lines, (0,0)→(30,0)→(30,20).

## Results

| Input | Offset | Result edges | Line | Circle | Other |
|---|---|---|---|---|---|
| closed rounded rect | source | 6 | 4 | 2 | 0 |
| closed rounded rect | +3 (outward) | 8 | 4 | 4 | 0 |
| closed rounded rect | −3 (inward) | 10 | 4 | 6 | 0 |
| closed rounded rect | −6 (offset > r) | 4 | 4 | 0 | 0 |
| closed rounded rect | −11 (collapse) | **0** | 0 | 0 | 0 |
| closed rounded rect | −40 (past collapse) | **0** | 0 | 0 | 0 |
| open L | +3 | 7 | 4 | 3 | 0 |
| open L | −3 | 7 | 4 | 3 | 0 |

## Spike 2 — the cases spike 1 could not see

Adversarial review flagged that spike 1 generalised from a single fixture, and that counting edges cannot detect a wire split. Both were fair. Spike 2 added fixtures and counted **wires** as well as edges, and ran `BRepCheck_Analyzer` on each result.

| Fixture | Offset | wires | edges | Line | Circle | Other | valid |
|---|---|---|---|---|---|---|---|
| concave L (40×40, 20×20 bite) | +3 | 1 | 11 | 6 | 5 | 0 | yes |
| concave L | −3 | 1 | 7 | 6 | 1 | 0 | yes |
| dumbbell (4-wide neck) | −1 | 1 | 16 | 12 | 4 | 0 | yes |
| dumbbell | −3 | **2** | 14 | 10 | 4 | 0 | yes |
| bow-tie (self-intersecting) | +3 | 1 | 6 | 4 | 2 | 0 | **yes** |
| bow-tie | −3 | 1 | 6 | 4 | 2 | 0 | **yes** |
| circle r=10 | ±3 | 1 | 1 | 0 | 1 | 0 | yes |
| circle r=10 | −10 (== r) | — | — | — | — | — | **NOT DONE** |
| circle r=10 | −12 (> r) | — | — | — | — | — | **NOT DONE** |
| circle r=10 | 0 | 1 | 1 | 0 | 1 | 0 | yes |
| circle r=10 | 1e-9 | 1 | 1 | 0 | 1 | 0 | yes |

Four results change the contract:

- **A single `Perform` can return more than one wire.** The dumbbell's neck pinches at −3 and the result splits in two. Spike 1 counted edges through `TopExp_Explorer(TopAbs_EDGE)`, which aggregates across wires, so it was structurally incapable of noticing. Drives the multi-wire refusal in CAP-5.
- **A self-intersecting source succeeds and looks healthy.** The bow-tie returns one wire, six edges, and `IsValid() == true`. Nothing downstream distinguishes it from good output — the source must be validated as simple *before* `Perform`.
- **There are two distinct failure modes, not one.** The circle at offset ≥ radius returns `IsDone() == false`, whereas the rectangle past collapse returns `IsDone() == true` with an empty shape. Handling only the second, as the first draft did, misses the first.
- **Zero and near-zero offsets succeed**, returning geometry coincident with the source. Hence the tolerance floor applied before OCCT is called.

## Finding 1 — only Line and Circle come back

`Other = 0` in every row of **both** tables — concave, split, self-intersecting and circular fixtures included. For line/arc input, 7.9.3 emits exclusively `GeomAbs_Line` and `GeomAbs_Circle`.

This is the load-bearing result: mapping the offset wire back to `SketchLine` / `SketchArc` / `SketchCircle` needs no BSpline fallback, so CAP-3's editability guarantee is reachable. Without it the design would have had to degrade to SvgImport's sample-to-polyline approach and give up dimensionable output.

**Scope of the claim.** This holds across every fixture tested here, which is a broader base than spike 1 had — but it is not a proof over all valid input, and the spec does not treat it as one. The implementation still classifies every result edge at runtime and refuses anything else (CAP-5), so an unmeasured case produces a refusal rather than silent corruption.

The edge-count growth is the `GeomAbs_Arc` join style at work, and it is why CAP-3's success criterion has to describe two arc populations: a **join fillet** at a previously-sharp corner has radius `|d|` and no source arc to compare against, unlike a derived arc at `source ± d`. Outward `+3` adds four fillet arcs at the rectangle's two sharp left corners plus the two existing round corners, taking Circle from 2 to 4. At `−6` the offset exceeds the r=5 corner radius, those arcs vanish entirely, and a clean 4-line rectangle remains — the degrading-gracefully case, and still valid output.

## Finding 2 — collapse succeeds with an empty shape

> Superseded in part by spike 2: this is one of **two** failure modes. See the circle rows above for the `IsDone() == false` path.


At `−11` and `−40` the loop collapses. `MakeOffset` does **not** report failure: `IsDone()` returns true and `Shape()` is non-null. It simply contains zero edges.

So `if (!mk.IsDone() || mk.Shape().IsNull())` — the guard `SvgImport.cpp:657` uses — passes here and would let the tool commit nothing while appearing to have worked. The edge count must be checked as well. This drives CAP-5 and the second constraint in `SPEC.md`.

## Finding 3 — open wires are a trap, not a failure

The open L returns 7 edges for **both** `+3` and `−3` — the same count, because `MakeOffset` on an open wire produces a closed racetrack outline *around* the path, not a parallel curve on one side. The sign barely matters.

That is exactly the stroke-to-outline behaviour `SvgImport` wants, and exactly not what a user means by "offset this profile". Because it succeeds rather than erroring, a naive implementation would ship confidently wrong geometry. This is the concrete reason `SPEC.md` scopes v1 to closed loops — an empirical result, not caution.

## Result validation is required, not implied

The spike ran `BRepCheck_Analyzer` on every result and reported `valid=yes` throughout. That is measurement, not a guarantee: these fixtures are small and well-conditioned. The contract therefore *requires* the check at runtime — closure, `IsValid()`, and the same non-self-intersection predicate used on the source — rather than inferring result simplicity from source simplicity.

## What to re-measure on an OCCT bump

Finding 1, first: if a future version starts emitting `GeomAbs_BSplineCurve` for arc input, CAP-3 breaks and the refusal path in the last constraint starts firing on valid input. Findings 2 and 3 are behavioural and less likely to move, but the collapse case is cheap to re-check.
