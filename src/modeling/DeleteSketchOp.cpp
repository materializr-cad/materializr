#include "DeleteSketchOp.h"
#include <cstdio>

void DeleteSketchOp::addBodyDelete(int bodyId) {
    auto del = std::make_unique<DeleteOp>();
    del->setBodyId(bodyId);
    m_bodyDeletes.push_back(std::move(del));
}

bool DeleteSketchOp::execute(Document& doc) {
    if (m_sketchId < 0) return false;
    auto sk = doc.getSketch(m_sketchId);
    if (!sk) return false;
    m_sketch = sk;
    m_name = doc.getSketchName(m_sketchId);
    m_visible = doc.isSketchVisible(m_sketchId);
    m_position = doc.sketchPosition(m_sketchId);
    // A body that is already gone (an earlier step removed it) is simply not
    // part of this delete.
    for (auto& del : m_bodyDeletes) del->execute(doc);
    doc.removeSketch(m_sketchId);
    return true;
}

bool DeleteSketchOp::undo(Document& doc) {
    if (m_sketchId < 0 || !m_sketch) return false;
    doc.putSketch(m_sketchId, m_sketch, m_name, m_position);
    doc.setSketchVisible(m_sketchId, m_visible);
    // In the order they were deleted, not the reverse: the bodies are independent
    // of each other, and putBody appends, so the original order of the Items list
    // only survives if they go back the way they left.
    for (auto& del : m_bodyDeletes) del->undo(doc);
    return true;
}

OperationDiff DeleteSketchOp::captureDiff() const {
    OperationDiff d;
    for (const auto& del : m_bodyDeletes) {
        OperationDiff c = del->captureDiff();
        d.deletedBefore.insert(d.deletedBefore.end(), c.deletedBefore.begin(),
                               c.deletedBefore.end());
    }
    return d;
}

std::string DeleteSketchOp::description() const {
    if (!m_name.empty()) return "Delete \xE2\x86\x92 " + m_name;
    return "Delete sketch";
}

std::string DeleteSketchOp::serializeParams() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "id=%d", m_sketchId);
    return buf;
}

bool DeleteSketchOp::deserializeParams(const std::string& blob) {
    int id = -1;
    std::sscanf(blob.c_str(), "id=%d", &id);
    m_sketchId = id;
    return true;
}

bool DeleteSketchOp::rehydrateFromReload(const ReloadState& /*state*/, Document& /*doc*/) {
    // The sketch is gone from the saved project; nothing to restore on undo.
    return false;
}
