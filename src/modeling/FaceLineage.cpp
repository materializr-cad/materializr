#include "FaceLineage.h"
#include "GenerationLedger.h"

#include <BRepTools_History.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>

#include <TopExp_Explorer.hxx>
#include <algorithm>

namespace materializr {
namespace topo {

const std::vector<int>* idsFor(const FaceIdMap& m, const TopoDS_Shape& face) {
    for (const auto& e : m)
        if (e.face.IsSame(face)) return &e.ids;
    return nullptr;
}

void addId(FaceIdMap& m, const TopoDS_Shape& face, int id) {
    for (auto& e : m) {
        if (e.face.IsSame(face)) {
            if (std::find(e.ids.begin(), e.ids.end(), id) == e.ids.end())
                e.ids.push_back(id);
            return;
        }
    }
    m.push_back({face, {id}});
}

FaceIdMap propagate(
    const std::vector<std::pair<const FaceIdMap*, TopoDS_Shape>>& inputs,
    const GenerationLedger& led, const TopoDS_Shape& result) {
    FaceIdMap out;
    if (result.IsNull()) return out;

    // Faces present in the result, for the IsSame-survival carry.
    TopTools_IndexedMapOfShape resultFaces;
    TopExp::MapShapes(result, TopAbs_FACE, resultFaces);

    for (const auto& [map, inShape] : inputs) {
        if (!map) continue;
        (void)inShape; // the ledger's keys already come from the right input
        for (const auto& e : *map) {
            if (e.face.IsNull() || e.ids.empty()) continue;
            bool carried = false;
            if (led.modified.Contains(e.face)) {
                // Modified (possibly into SEVERAL pieces - a boolean split):
                // every piece inherits the ancestry. This is the case no
                // geometric matcher can trace (#51).
                for (const TopoDS_Shape& piece : led.modified.FindFromKey(e.face)) {
                    if (piece.ShapeType() != TopAbs_FACE) continue;
                    // Membership guard: a post-boolean cleanup (Union's
                    // UnifySameDomain) can merge pieces away - only faces
                    // actually present in the result carry ancestry.
                    if (!resultFaces.Contains(piece)) continue;
                    for (int id : e.ids) addId(out, piece, id);
                    carried = true;
                }
            }
            if (!carried && resultFaces.Contains(e.face)) {
                // Untouched by the op - the identical face survives.
                for (int id : e.ids) addId(out, e.face, id);
            }
            // Deleted faces: ancestry ends here, correctly.
        }
    }
    return out;
}

FaceIdMap carryThrough(const FaceIdMap& in,
                       const Handle(BRepTools_History)& hist,
                       const TopoDS_Shape& result) {
    if (hist.IsNull() || result.IsNull()) return in;
    TopTools_IndexedMapOfShape resultFaces;
    TopExp::MapShapes(result, TopAbs_FACE, resultFaces);
    FaceIdMap out;
    for (const auto& e : in) {
        if (e.face.IsNull() || e.ids.empty()) continue;
        if (hist->IsRemoved(e.face)) continue;            // deleted by the rewrite
        const TopTools_ListOfShape& mod = hist->Modified(e.face);
        if (mod.IsEmpty()) {
            if (resultFaces.Contains(e.face))             // unchanged - survives
                for (int id : e.ids) addId(out, e.face, id);
        } else {
            for (TopTools_ListIteratorOfListOfShape it(mod); it.More(); it.Next())
                if (it.Value().ShapeType() == TopAbs_FACE &&
                    resultFaces.Contains(it.Value()))     // → successor(s)
                    for (int id : e.ids) addId(out, it.Value(), id);
        }
    }
    return out;
}

} // namespace topo
} // namespace materializr

namespace materializr { namespace topo {
void complete(FaceIdMap& m, const TopoDS_Shape& shape,
              const std::function<int()>& mint) {
    if (shape.IsNull()) return;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
        if (!idsFor(m, ex.Current()))
            addId(m, ex.Current(), mint());
}
} }
