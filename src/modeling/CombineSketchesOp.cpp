#include "CombineSketchesOp.h"
#include <algorithm>
#include <unordered_map>

using namespace materializr;

namespace {
int maxId(const Sketch& s) {
    int m = 0;
    for (const auto& p : s.getPoints())   m = std::max(m, p.id);
    for (const auto& l : s.getLines())    m = std::max(m, l.id);
    for (const auto& c : s.getCircles())  m = std::max(m, c.id);
    for (const auto& a : s.getArcs())     m = std::max(m, a.id);
    for (const auto& sp : s.getSplines()) m = std::max(m, sp.id);
    for (const auto& g : s.getPolygons()) m = std::max(m, g.id);
    for (const auto& k : s.getConstraints()) m = std::max(m, k.id);
    return m;
}
} // namespace

void CombineSketchesOp::mergeInto(Sketch& dst, const Sketch& src) {
    int next = maxId(dst) + 1;
    std::unordered_map<int, int> id; // src id -> dst id (points + elements)

    // Take the old id before re-id'ing: in `id[p.id] = p.id = next++` the right
    // side is evaluated first (C++17), so the map would be keyed by the NEW id
    // and every later lookup of an old id would miss or hit the wrong element.
    for (auto p : src.getPoints())   { int old = p.id; p.id = next++; id[old] = p.id; dst.addRawPoint(p); }
    for (auto l : src.getLines())    {
        int old = l.id; l.id = next++; id[old] = l.id;
        l.startPointId = id.count(l.startPointId) ? id[l.startPointId] : l.startPointId;
        l.endPointId   = id.count(l.endPointId)   ? id[l.endPointId]   : l.endPointId;
        dst.addRawLine(l);
    }
    for (auto c : src.getCircles())  {
        int old = c.id; c.id = next++; id[old] = c.id;
        c.centerPointId = id.count(c.centerPointId) ? id[c.centerPointId] : c.centerPointId;
        dst.addRawCircle(c);
    }
    for (auto a : src.getArcs())     {
        int old = a.id; a.id = next++; id[old] = a.id;
        a.centerPointId = id.count(a.centerPointId) ? id[a.centerPointId] : a.centerPointId;
        a.startPointId  = id.count(a.startPointId)  ? id[a.startPointId]  : a.startPointId;
        a.endPointId    = id.count(a.endPointId)    ? id[a.endPointId]    : a.endPointId;
        dst.addRawArc(a);
    }
    for (auto sp : src.getSplines()) {
        int old = sp.id; sp.id = next++; id[old] = sp.id;
        for (int& cp : sp.controlPointIds) if (id.count(cp)) cp = id[cp];
        dst.addRawSpline(sp);
    }
    for (auto g : src.getPolygons()) {
        int old = g.id;
        g.id = next++;
        g.centerPointId = id.count(g.centerPointId) ? id[g.centerPointId] : g.centerPointId;
        for (int& v : g.vertexPointIds) if (id.count(v)) v = id[v];
        // lineIds reference the polygon's generated lines; remap any we know.
        for (int& li : g.lineIds) if (id.count(li)) li = id[li];
        id[old] = g.id;
        dst.addRawPolygon(g);
    }
    for (auto k : src.getConstraints()) {
        // Drop a constraint whose referenced entity didn't come across.
        if (k.entityA >= 0 && !id.count(k.entityA)) continue;
        if (k.entityB >= 0 && !id.count(k.entityB)) continue;
        k.id = next++;
        if (k.entityA >= 0) k.entityA = id[k.entityA];
        if (k.entityB >= 0) k.entityB = id[k.entityB];
        dst.addRawConstraint(k);
    }
    dst.setNextId(next);
}

bool CombineSketchesOp::execute(Document& doc) {
    if (m_targetId < 0) return false;
    auto target = doc.getSketch(m_targetId);
    if (!target) return false;
    // Merge from the captured snapshots (not the live others) so this is
    // re-applies cleanly on redo even after undo re-created the others.
    for (const auto& snap : m_otherSnaps) mergeInto(*target, snap);
    for (int id : m_otherIds) doc.removeSketch(id);
    return true;
}

bool CombineSketchesOp::undo(Document& doc) {
    if (m_targetId < 0) return false;
    auto target = doc.getSketch(m_targetId);
    if (target) *target = m_targetBefore;
    // Re-create the absorbed sketches; track their (new) ids so a redo removes
    // the right ones.
    m_otherIds.clear();
    for (size_t i = 0; i < m_otherSnaps.size(); ++i) {
        auto sk = std::make_shared<Sketch>(m_otherSnaps[i]);
        int newId = doc.addSketch(sk, m_otherNames[i]);
        doc.setSketchVisible(newId, m_otherVisible[i] != 0);
        m_otherIds.push_back(newId);
    }
    return true;
}

std::string CombineSketchesOp::description() const {
    return "Combine " + std::to_string(m_otherSnaps.size() + 1) + " sketches";
}
