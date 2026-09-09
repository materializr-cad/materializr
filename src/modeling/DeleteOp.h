#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <string>

class DeleteOp : public Operation {
public:
    DeleteOp();
    ~DeleteOp() override = default;

    void setBodyId(int id);

    // Getters
    int getBodyId() const { return m_bodyId; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Delete"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "delete"; }
    OperationDiff captureDiff() const override;
    // Delete references a body purely by id, so it reloads as a real editable
    // op - its replay just removes the body again, but as a REAL op an editStep
    // can roll it back (restoring the body by id) and re-apply, instead of a
    // baked ReplayOp slamming a stale whole-document state over an upstream edit.
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    TopoDS_Shape m_deletedShape;
    std::string m_deletedName;
    bool m_wasVisible = true;
};
