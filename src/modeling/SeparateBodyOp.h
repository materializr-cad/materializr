#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <string>
#include <vector>

// Separate: a body whose shape holds several DISCONNECTED solids (air-gapped
// lumps that got fused into one body object - e.g. a boolean leaving tool
// remnants) is split into one body per solid. The largest lump keeps the
// original body; every other lump becomes a new body the user can inspect or
// delete. Purely a re-parceling of existing geometry - no shape changes.
class SeparateBodyOp : public Operation {
public:
    SeparateBodyOp() = default;
    ~SeparateBodyOp() override = default;

    void setBody(int id) { m_bodyId = id; }
    int getBodyId() const { return m_bodyId; }
    const std::vector<int>& getNewBodyIds() const { return m_newBodyIds; }

    // Number of disconnected solids in a body shape - the menu gate uses this
    // to only offer Separate when there is actually something to separate.
    static int solidCount(const TopoDS_Shape& shape);

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Separate"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "separate_body"; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }

private:
    int m_bodyId = -1;
    TopoDS_Shape m_previousShape;
    // Bodies created for the non-largest lumps. Kept across undo so redo
    // reuses the same ids (folder/colour/visibility restored from tombstones).
    std::vector<int> m_newBodyIds;
};
