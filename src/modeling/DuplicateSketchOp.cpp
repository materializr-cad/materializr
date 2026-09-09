#include "DuplicateSketchOp.h"
#include <TopoDS_Face.hxx>
#include <cstdio>

bool DuplicateSketchOp::execute(Document& doc) {
    if (!m_sketchCopy) return false;
    // A duplicate is an INDEPENDENT sketch, never a second driver of the
    // source's body (issue #21). The deep copy carried the source's
    // m_sourceBodyId AND its bound host face, which made the copy:
    //   - cascade edits back into the original body (it wasn't detached),
    //   - re-bind that host face - pulling the body face's wires into the copy's
    //     region (the stray translucent fill over the body),
    //   - aim push/pull + extrude at the ORIGINAL body instead of a new one, so
    //     push/pull did nothing and extrude was degenerate (perimeter fused flat,
    //     inherited holes inverted).
    // Sever the body/face link so the copy builds its region from its OWN loops
    // (clearing the source face re-keys the region cache) and any edit / push-pull
    // / extrude produces a fresh body. Idempotent, so redo/reload stay correct.
    m_sketchCopy->setSourceBody(-1);
    m_sketchCopy->setSourceFace(TopoDS_Face());
    m_sketchCopy->setDetachedFromBody(false);
    if (m_newSketchId < 0) {
        // First run: allocate a fresh id.
        m_newSketchId = doc.addSketch(m_sketchCopy, m_name);
    } else {
        // Redo (or reload replay): re-insert under the SAME id. putSketch
        // replaces in place, so this is idempotent if the sketch is already
        // present (e.g. loaded from the project's sketch list).
        doc.putSketch(m_newSketchId, m_sketchCopy, m_name);
    }
    doc.setSketchVisible(m_newSketchId, true);
    return true;
}

bool DuplicateSketchOp::undo(Document& doc) {
    // Remove the copy entirely (m_sketchCopy keeps the geometry alive for redo).
    if (m_newSketchId >= 0) doc.removeSketch(m_newSketchId);
    return true;
}

std::string DuplicateSketchOp::description() const {
    if (!m_name.empty()) return "Duplicate \xE2\x86\x92 " + m_name;
    return "Duplicate sketch";
}

std::string DuplicateSketchOp::serializeParams() const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "src=%d;new=%d", m_sourceId, m_newSketchId);
    return buf;
}

bool DuplicateSketchOp::deserializeParams(const std::string& blob) {
    int src = -1, nw = -1;
    std::sscanf(blob.c_str(), "src=%d;new=%d", &src, &nw);
    m_sourceId = src;
    m_newSketchId = nw;
    return true;
}

bool DuplicateSketchOp::rehydrateFromReload(const ReloadState& /*state*/,
                                            Document& doc) {
    // The duplicated sketch was saved in the project's sketch list and is
    // already loaded under m_newSketchId. Bind to it so undo/redo work across
    // sessions; if it's gone, fall back to a baked (inert) step.
    if (m_newSketchId < 0) return false;
    m_sketchCopy = doc.getSketch(m_newSketchId);
    if (!m_sketchCopy) return false;
    m_name = doc.getSketchName(m_newSketchId);
    return true;
}
