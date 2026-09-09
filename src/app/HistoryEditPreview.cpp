#include "HistoryEditPreview.h"
#include "../core/Document.h"
#include "../core/History.h"
#include <set>

namespace materializr {

void HistoryEditPreview::begin(Document& doc, History& hist) {
    m_bodies.clear();
    for (int id : doc.getAllBodyIds()) {
        try { m_bodies[id] = doc.getBody(id); } catch (...) {}
    }
    hist.snapshotAllEditState();
    m_active = true;
}

bool HistoryEditPreview::replay(int stepIndex, Document& doc, History& hist) {
    if (hist.editStep(stepIndex, doc)) return true;
    restore(doc, hist);
    return false;
}

bool HistoryEditPreview::restore(Document& doc, History& hist) {
    if (m_bodies.empty()) return false;
    // Put every snapshotted body back; drop any body a failed replay spawned.
    std::set<int> want;
    for (const auto& [id, shp] : m_bodies) {
        try { doc.putBody(id, shp); } catch (...) {}
        want.insert(id);
    }
    for (int id : doc.getAllBodyIds())
        if (!want.count(id)) { try { doc.removeBody(id); } catch (...) {} }
    // The preview replays mutated ops' resolution members against bodies we
    // just discarded - that state has to come back too, or the step wedges.
    hist.restoreAllEditState();
    // Nothing re-executed, so tell history the model is fully applied and
    // undo/redo stay consistent with the restored bodies.
    hist.markFullyApplied();
    return true;
}

void HistoryEditPreview::clear() {
    m_bodies.clear();
    m_active = false;
}

} // namespace materializr
