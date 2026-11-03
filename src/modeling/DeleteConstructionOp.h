#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "../core/History.h"
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <optional>
#include <string>

// Delete a construction plane or axis, undoably. Plane and axis deletion used to
// write straight to the Document with no history step, so Ctrl+Z undid whatever
// came BEFORE the delete (and a later Redo re-ran the plane's creation step,
// making a plane "reappear" out of nowhere).
//
// execute() removes the entity; undo() re-adds it under the SAME id (reuseId),
// with its name, visibility and - for a plane - the reference image / mesh
// trace it hosted, so sketches and Move Plane steps that name the id stay valid.
//
// The params blob carries the entity's literal geometry so a step that comes
// back from a saved project can still undo (the entity is gone from the saved
// plane/axis list by then).
class DeleteConstructionOp : public Operation {
public:
    DeleteConstructionOp() = default;
    ~DeleteConstructionOp() override = default;

    void setPlaneId(int id) { m_isAxis = false; m_id = id; }
    void setAxisId(int id)  { m_isAxis = true;  m_id = id; }
    bool isAxis() const { return m_isAxis; }
    int  targetId() const { return m_id; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return m_isAxis ? "Delete Axis" : "Delete Plane"; }
    std::string description() const override;
    void renderProperties() override {}
    std::string typeId() const override { return "delete_construction"; }

    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    bool m_isAxis = false;
    int m_id = -1;
    std::string m_name;
    bool m_visible = true;
    bool m_captured = false;

    gp_Pln m_plane;
    double m_halfSize = 50.0;
    gp_Pnt m_origin;
    gp_Dir m_direction = gp_Dir(1, 0, 0);
    double m_halfLength = 50.0;

    // Hosted payloads (planes only). In-session only: a reference photo is not
    // worth duplicating into every saved delete step.
    std::optional<RefImageEntry>  m_refImage;
    std::optional<MeshTraceEntry> m_meshTrace;
};

// The one way UI deletes a construction plane / axis: through History when there
// is one (so Undo restores it), straight from the Document otherwise.
inline bool deleteConstructionEntity(History* history, Document& doc, bool isAxis, int id) {
    if (!history) {
        if (isAxis) doc.removeAxis(id); else doc.removePlane(id);
        return true;
    }
    auto op = std::make_unique<DeleteConstructionOp>();
    if (isAxis) op->setAxisId(id); else op->setPlaneId(id);
    return history->pushOperation(std::move(op), doc);
}
