#include "SubShapeIndex.h"
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <gp_Pnt.hxx>
#include <gp_Lin.hxx>
#include <gp_Circ.hxx>
#include <cmath>
#include <cstdlib>

namespace SubShapeIndex {

int indexOf(const TopoDS_Shape& shape, const TopoDS_Shape& sub,
            TopAbs_ShapeEnum type) {
    if (shape.IsNull() || sub.IsNull()) return 0;
    try {
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(shape, type, map);
        return map.FindIndex(sub); // 0 when absent
    } catch (...) {
        return 0;
    }
}

TopoDS_Shape at(const TopoDS_Shape& shape, int index, TopAbs_ShapeEnum type) {
    if (shape.IsNull() || index <= 0) return {};
    try {
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(shape, type, map);
        if (index > map.Extent()) return {};
        return map.FindKey(index);
    } catch (...) {
        return {};
    }
}

std::string serialize(const TopoDS_Shape& shape,
                      const std::vector<TopoDS_Shape>& subs,
                      TopAbs_ShapeEnum type) {
    std::string out;
    if (shape.IsNull()) return out;
    try {
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(shape, type, map);
        for (const auto& s : subs) {
            int idx = map.FindIndex(s);
            if (idx <= 0) continue;
            if (!out.empty()) out += ',';
            out += std::to_string(idx);
        }
    } catch (...) {}
    return out;
}

std::vector<int> parse(const std::string& csv) {
    std::vector<int> out;
    size_t pos = 0;
    while (pos < csv.size()) {
        size_t end = csv.find(',', pos);
        if (end == std::string::npos) end = csv.size();
        int v = std::atoi(csv.substr(pos, end - pos).c_str());
        if (v > 0) out.push_back(v);
        pos = end + 1;
    }
    return out;
}

bool resolveAll(const TopoDS_Shape& shape, const std::vector<int>& indices,
                TopAbs_ShapeEnum type, std::vector<TopoDS_Shape>& out) {
    out.clear();
    if (shape.IsNull() || indices.empty()) return false;
    try {
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(shape, type, map);
        for (int idx : indices) {
            if (idx <= 0 || idx > map.Extent()) { out.clear(); return false; }
            out.push_back(map.FindKey(idx));
        }
        return true;
    } catch (...) {
        out.clear();
        return false;
    }
}

namespace {

bool edgeMidpoint(const TopoDS_Edge& e, gp_Pnt& out) {
    try {
        BRepAdaptor_Curve c(e);
        out = c.Value((c.FirstParameter() + c.LastParameter()) * 0.5);
        return true;
    } catch (...) { return false; }
}

// Find the edge in `map` that lies on the same carrier geometry as `oldEdge`.
// Returns a null edge when no candidate matches.
TopoDS_Edge rebindOne(const TopTools_IndexedMapOfShape& map,
                      const TopoDS_Edge& oldEdge) {
    constexpr double kDistTol = 1e-3;   // mm
    constexpr double kDirTol  = 0.9999; // |dot| ≈ within 0.8°

    BRepAdaptor_Curve oldC;
    try { oldC.Initialize(oldEdge); } catch (...) { return {}; }
    gp_Pnt oldMid;
    if (!edgeMidpoint(oldEdge, oldMid)) return {};

    TopoDS_Edge best;
    double bestMidDist = 1e100;
    for (int i = 1; i <= map.Extent(); ++i) {
        const TopoDS_Edge& cand = TopoDS::Edge(map.FindKey(i));
        try {
            BRepAdaptor_Curve c(cand);
            if (c.GetType() != oldC.GetType()) continue;
            bool carrierMatch = false;
            if (oldC.GetType() == GeomAbs_Line) {
                gp_Lin l0 = oldC.Line(), l1 = c.Line();
                carrierMatch =
                    std::abs(l0.Direction().Dot(l1.Direction())) > kDirTol &&
                    l1.Distance(l0.Location()) < kDistTol;
            } else if (oldC.GetType() == GeomAbs_Circle) {
                gp_Circ c0 = oldC.Circle(), c1 = c.Circle();
                carrierMatch =
                    c0.Location().Distance(c1.Location()) < kDistTol &&
                    std::abs(c0.Axis().Direction().Dot(c1.Axis().Direction())) > kDirTol &&
                    std::abs(c0.Radius() - c1.Radius()) < kDistTol;
            } else {
                // Free-form curves: midpoint proximity only, and tight - a
                // wrong match here would silently blend the wrong edge.
                // (A relaxed unique-winner pass below rescues fragmented /
                // merged curves whose midpoint shifted, #54.)
                gp_Pnt mid;
                carrierMatch = edgeMidpoint(cand, mid) &&
                               mid.Distance(oldMid) < kDistTol;
            }
            if (!carrierMatch) continue;
            gp_Pnt mid;
            if (!edgeMidpoint(cand, mid)) continue;
            double d = mid.Distance(oldMid);
            if (d < bestMidDist) { bestMidDist = d; best = cand; }
        } catch (...) { continue; }
    }
    if (!best.IsNull()) return best;

    // Relaxed second chance (#54): a fragmented or merged curved edge keeps
    // its carrier but shifts its midpoint past the strict tolerance. Accept a
    // same-curve-type candidate within 2 mm ONLY when it wins unambiguously
    // (runner-up at least twice as far) - never a coin-flip re-bind.
    TopoDS_Edge loose;
    double d1 = 1e100, d2 = 1e100;
    for (int i = 1; i <= map.Extent(); ++i) {
        const TopoDS_Edge& cand = TopoDS::Edge(map.FindKey(i));
        try {
            BRepAdaptor_Curve c(cand);
            if (c.GetType() != oldC.GetType()) continue;
            gp_Pnt mid;
            if (!edgeMidpoint(cand, mid)) continue;
            double d = mid.Distance(oldMid);
            if (d < d1) { d2 = d1; d1 = d; loose = cand; }
            else if (d < d2) { d2 = d; }
        } catch (...) { continue; }
    }
    if (!loose.IsNull() && d1 < 2.0 && d2 > d1 * 2.0) return loose;
    return {};
}

} // namespace

bool rebindEdges(const TopoDS_Shape& shape, std::vector<TopoDS_Edge>& edges) {
    if (shape.IsNull() || edges.empty()) return false;
    try {
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(shape, TopAbs_EDGE, map);

        std::vector<TopoDS_Edge> rebound;
        rebound.reserve(edges.size());
        for (const auto& e : edges) {
            if (map.Contains(e)) {          // still a live sub-shape - keep
                rebound.push_back(e);
                continue;
            }
            TopoDS_Edge r = rebindOne(map, e);
            if (r.IsNull()) return false;   // genuinely gone - caller decides
            rebound.push_back(r);
        }
        edges = std::move(rebound);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace SubShapeIndex
