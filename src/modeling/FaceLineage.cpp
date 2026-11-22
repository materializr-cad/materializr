#include <cstdio>
#include <cstdlib>
#include "FaceLineage.h"
#include "GenerationLedger.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepTools_History.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>

#include <TopExp_Explorer.hxx>
#include <algorithm>
#include <cmath>

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

void completeStable(FaceIdMap& m, const TopoDS_Shape& shape,
                    const std::function<int()>& mint, StableMint& memo) {
    if (shape.IsNull()) return;
    std::vector<TopoDS_Shape> todo;
    std::vector<std::pair<int, double>> sig;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        if (idsFor(m, ex.Current())) continue;
        todo.push_back(ex.Current());
        int type = -1;
        double area = 0.0;
        try {
            type = static_cast<int>(BRepAdaptor_Surface(TopoDS::Face(ex.Current())).GetType());
            GProp_GProps g;
            BRepGProp::SurfaceProperties(ex.Current(), g);
            area = g.Mass();
        } catch (...) {}
        sig.push_back({type, area});
    }
    bool same = memo.ids.size() == todo.size() && memo.sig.size() == sig.size();
    for (size_t i = 0; same && i < sig.size(); ++i)
        same = memo.sig[i].first == sig[i].first &&
               std::abs(memo.sig[i].second - sig[i].second) <=
                   1e-4 * std::max(1.0, std::abs(sig[i].second));
    if (!same) {
        memo.ids.clear();
        for (size_t i = 0; i < todo.size(); ++i) memo.ids.push_back(mint());
        memo.sig = std::move(sig);
    }
    for (size_t i = 0; i < todo.size(); ++i) addId(m, todo[i], memo.ids[i]);
}

std::string mintIdsToString(const StableMint& memo) {
    std::string out;
    for (size_t i = 0; i < memo.ids.size(); ++i)
        out += (i ? "," : "") + std::to_string(memo.ids[i]);
    return out;
}

std::string mintSigToString(const StableMint& memo) {
    std::string out;
    char b[64];
    for (size_t i = 0; i < memo.sig.size(); ++i) {
        std::snprintf(b, sizeof(b), "%s%d:%.9g", i ? "," : "", memo.sig[i].first, memo.sig[i].second);
        out += b;
    }
    return out;
}

void mintFromStrings(StableMint& memo, const std::string& ids, const std::string& sig) {
    StableMint m;
    for (size_t q = 0; q < ids.size();) {
        const size_t c = ids.find(',', q);
        m.ids.push_back(std::atoi(ids.substr(q, c == std::string::npos ? std::string::npos : c - q).c_str()));
        if (c == std::string::npos) break;
        q = c + 1;
    }
    for (size_t q = 0; q < sig.size();) {
        const size_t c = sig.find(',', q);
        const std::string tok = sig.substr(q, c == std::string::npos ? std::string::npos : c - q);
        const size_t col = tok.find(':');
        if (col != std::string::npos)
            m.sig.push_back({std::atoi(tok.c_str()), std::atof(tok.c_str() + col + 1)});
        if (c == std::string::npos) break;
        q = c + 1;
    }
    // Both lists or neither: a half-read pair would make the next replay reuse ids
    // against the wrong faces.
    if (!m.ids.empty() && m.ids.size() == m.sig.size()) memo = std::move(m);
}
} }
