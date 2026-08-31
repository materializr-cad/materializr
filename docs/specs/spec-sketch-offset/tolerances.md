# Tolerances and validation predicates

Companion to `SPEC.md`. Added after round 2 of review, which correctly found that the spec required a "documented geometry tolerance" and an "OCCT topology tolerance" without ever naming a value — leaving zero-distance refusal, welding, self-intersection, wire closure and every test expectation implementation-defined.

## Named tolerances

All in sketch millimetres.

### Why these are scale-aware

An earlier draft fixed every tolerance at `1e-6` mm. Review round 3 showed that is below what the data can represent: `SketchPoint::pos` is `glm::vec2`, single precision, and a float ULP measured at CAD extents is

| coordinate | float ULP |
|---|---|
| 1 mm | 1.2e-7 mm |
| 100 mm | **7.6e-6 mm** |
| 1000 mm | 6.1e-5 mm |
| 10000 mm | 9.8e-4 mm |

So a `1e-6` predicate degenerates to exact equality beyond ~10 mm from the origin, and its meaning changes as a sketch is translated — the same loop validates differently at the origin and at (1000, 1000). At 10000 mm the ULP nearly reaches the minimum offset distance itself.

**The primary fix is not a bigger number: it is not comparing positions at all.** `SketchLine` already stores `startPointId` and `endPointId`, so two entities share a vertex exactly when they reference the same point id. Adjacency is built on ids — exact, scale-free, immune to translation. Positional comparison survives only where identity cannot answer the question.

### Values

| Name | Value | Governs |
|---|---|---|
| `kMinOffsetDistance` | `max(1e-3, 4 × weldTol(extent))` | `\|d\|` below this is refused **before** OCCT is called |
| `weldTol(p)` | `max(1e-6, 8 × ulp(maxAbsCoord(p)))` | coincident-but-distinct point detection (fallback only) |
| `kZeroLengthEdgeTol` | `weldTol` at the entity's own coordinates | an entity shorter than this is degenerate |
| `kIntersectionTol` | `weldTol` at the intersection's coordinates | entity-pair intersection and tangency |

`weldTol` is evaluated in **double precision** on positions promoted from the stored floats, and `maxAbsCoord` is taken over the point being tested, so the tolerance tracks local magnitude rather than the sketch's global extent. `kMinOffsetDistance` is tied to the weld tolerance so an offset can never be smaller than the distance at which its own output would weld to itself.

None of these scale with zoom, DPI or touch mode. They describe geometry and its storage precision, not pointer precision — which is exactly why `findCoincidentPoint`'s `0.3f * snapScale()` is the wrong tool here (see `SPEC.md`). A user legitimately offsetting by 0.05 mm is comfortably above `kMinOffsetDistance` across the whole working range, and comfortably inside the UI snap radius that would destroy it.

`kMinOffsetDistance` is a refusal, never a clamp. Below it there is no preview and no commit, and mouse-up on or near the source surfaces the small-distance error rather than quietly committing a minimum-sized offset.

## Source-loop validation

Run before `Perform`, on the captured entity ids. Build the adjacency graph in **two stages**:

1. **Exact.** Entities that reference the same `SketchPoint` id meet at that vertex.
2. **Fallback union.** Then union any two *still-distinct* endpoint ids whose positions lie within `weldTol` of each other, in double precision.

Stage 2 is not optional garnish. Geometry drawn by the sketch tools shares real point ids and never needs it — but an imported loop can be geometrically closed while every junction holds two distinct coincident ids. Under stage 1 alone such a loop presents as a chain of degree-1 vertices and is rejected as "open", which is wrong. Stage 2 is what makes an imported closed loop offsettable, and it is the only place the positional tolerance affects topology.

Then require **all** of:

1. Every vertex has degree exactly 2. Degree 1 means an open chain; degree > 2 means a branch.
2. Exactly one connected component. Two components means disjoint loops.
3. No entity shorter than `kZeroLengthEdgeTol`.
4. No two **distinct** entities that are geometrically coincident. (Duplicate *ids* cannot occur — selections are `std::set<int>` — so the id check is retained only as cheap defensive validation. The real hazard is two different ids describing the same segment.)
5. No spline entities.
6. No intersection between any entity pair — line/line, line/arc, arc/arc — at any parameter other than a shared graph vertex, tested at `kIntersectionTol` in double precision.

Rules 1 and 2 read the graph after both stages, so they are exact for drawn geometry and tolerance-dependent only where stage 2 actually fired. Rules 3, 4 and 6 compare positions directly. The common case — a loop drawn with the sketch tools — resolves entirely on ids and never consults a tolerance at all.

Rule 6 is what rejects the bow-tie, and it cannot be replaced by the degree check: a bow-tie satisfies rules 1–5 and still has no unambiguous inside. Tangential contact **at a shared endpoint** is permitted (that is an ordinary smooth join); tangential contact anywhere else is refused.

A lone selected circle bypasses the graph walk and satisfies the contract by construction.

This is a deliberately stricter and simpler predicate than `Sketch::buildRegions()`, which performs synthetic crossing insertion and curve splitting to make sense of messy sketches. Offset does not attempt to repair its input — it refuses it, and says why.

## Canonical winding

Unordered selected entities carry no orientation, so one is imposed. Assemble the validated loop into an ordered wire, then reorient it to **positive signed area (CCW)**. A lone circle has no endpoint walk to derive orientation from, so it is emitted CCW explicitly.

After canonicalisation, a **positive** `Perform` argument is outward. The cursor's signed distance maps onto that convention. Ties — the cursor equidistant from multiple nearest points, most obviously at a circle's centre — resolve **outward**, deterministically.

## Commit transaction

`recordSketchMutation` records before/after state for undo, but it does not roll back an exception or a partially-failed conversion, and `Sketch`'s mutators append directly. All-or-nothing therefore does not follow from the wrapper.

Commit runs in two phases:

1. **Plan.** Convert the entire OCCT result into an in-memory list of intended entities — points, lines, arcs, circles, with orientation already resolved. Any failure here (unexpected curve type, unrepresentable arc, OCCT exception) aborts with no mutation at all, because nothing has been written yet.
2. **Apply.** Only once the plan is complete and valid, write it into the `Sketch` inside `recordSketchMutation`. This phase performs no geometry work that can fail.

The OCCT call itself is wrapped so a `Standard_Failure` becomes a structured error and a toast, never an escape into the frame loop.
