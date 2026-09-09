#pragma once
#include "../core/Operation.h"
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <string>
#include <vector>

// Undoable construction-axis transform (gizmo move). Mirrors PlaneTransformOp:
// the edit is applied live to the Document; this records before/after
// (origin, direction) snapshots so undo/redo swap between them via
// Document::setAxis. Owns no bodies (empty captureDiff), so it never touches
// getBody(-1) - the crash that made axis/plane drags skip history in 0.6.0.
// Pushed via History::pushExecuted. Holds a batch so a multi-axis drag is one
// undo step.
class AxisTransformOp : public Operation {
public:
    struct Entry {
        int     axisId;
        gp_Pnt  beforeOrigin;
        gp_Dir  beforeDir;
        gp_Pnt  afterOrigin;
        gp_Dir  afterDir;
    };

    AxisTransformOp(std::string label, std::vector<Entry> entries)
        : m_label(std::move(label)), m_entries(std::move(entries)) {}
    AxisTransformOp() = default;    // reload path (OperationFactory)

    bool execute(Document& doc) override;   // apply the "after" poses (redo)
    bool undo(Document& doc) override;       // restore the "before" poses

    std::string name() const override { return m_label; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "axis_transform"; }
    // Reload support - mirrors PlaneTransformOp.
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    std::string        m_label;
    std::vector<Entry> m_entries;
};
