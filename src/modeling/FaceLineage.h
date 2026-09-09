#pragma once
// Face lineage - the "gen" coverage for op-PRODUCED faces (TopoName.h's
// designed-but-unimplemented general kernel), scoped to the question that
// actually bites: which op made the face under the cursor, after downstream
// ops (booleans especially) modified, SPLIT or merged it? (#49/#51)
//
// Model: every body carries a FaceIdMap - face → the stable int ids of its
// ANCESTRY. A fillet/chamfer mints fresh ids for its blend/bevel faces and
// records them; every downstream op PROPAGATES the map through its
// GenerationLedger's modified-map (a split bevel's pieces all inherit the
// bevel's id - the case no geometric scheme can trace). Ownership is then a
// set-intersection, not a geometry hunt.
//
// STRICTLY ADDITIVE: ops that don't propagate simply leave the next map empty
// (Document::updateBody clears it), and every consumer falls back to the
// existing geometric path. Old saves have no lineage section → same fallback
// → behaviour identical to before this file existed.

#include <TopoDS_Shape.hxx>
#include <Standard_Handle.hxx>
#include <functional>
#include <utility>
#include <vector>

class BRepTools_History;

namespace materializr {
namespace topo {

struct GenerationLedger;

struct FaceIds {
    TopoDS_Shape face;
    std::vector<int> ids;   // ancestry ids (usually 1; >1 after merges)
};
using FaceIdMap = std::vector<FaceIds>;

// The ids attached to `face` in `m`, or nullptr. Matches by IsSame.
const std::vector<int>* idsFor(const FaceIdMap& m, const TopoDS_Shape& face);

// Attach `id` to `face` in `m` (creates or extends the entry, no duplicates).
void addId(FaceIdMap& m, const TopoDS_Shape& face, int id);

// Carry input maps through one executed op into its result shape. For each
// input entry: faces the ledger reports as MODIFIED pass their ids to every
// output face (splits fan out, merges union); faces absent from the ledger
// that survive IsSame-identical in the result carry through directly.
// Give EVERY face of `shape` an id: faces already in `m` keep theirs, the
// rest mint fresh ones via `mint` (deterministic shape order). Full coverage
// is what lets ops reference ARBITRARY faces (a chamfer's asymmetric
// reference face, an edge's adjacent faces) by id instead of guessing.
void complete(FaceIdMap& m, const TopoDS_Shape& shape,
              const std::function<int()>& mint);

FaceIdMap propagate(
    const std::vector<std::pair<const FaceIdMap*, TopoDS_Shape>>& inputs,
    const GenerationLedger& led, const TopoDS_Shape& result);

// Carry a completed FaceIdMap through a post-build rewrite described by a
// BRepTools_History (e.g. ShapeUpgrade_UnifySameDomain merging coplanar faces)
// onto `result`: each face passes its ids to its successor(s) - a merge unions
// both parents' ids, a removed face drops out, an unchanged face carries
// through. Handles the faces the PRODUCING op left untouched (absent from its
// ledger) that the rewrite nonetheless rebuilt - the case propagate()'s
// membership guard silently drops, re-minting fresh ids (the union / push-pull
// face-id drift). Returns `in` unchanged when `hist` is null.
FaceIdMap carryThrough(const FaceIdMap& in,
                       const Handle(BRepTools_History)& hist,
                       const TopoDS_Shape& result);

} // namespace topo
} // namespace materializr
