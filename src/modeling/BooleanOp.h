#pragma once
#include "../core/Operation.h"
#include "GenerationLedger.h"
#include "FaceLineage.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <string>

enum class BooleanMode { Union, Subtract, Intersect };

class BooleanOp : public Operation {
public:
    BooleanOp();
    ~BooleanOp() override = default;

    // Parameters
    void setTargetBodyId(int id);
    void setToolBodyId(int id);
    void setMode(BooleanMode mode);
    // Keep the tool body after the operation instead of consuming it. Used by
    // Subtract's "keep the cutter bodies" option, and to keep a cutter alive
    // while it's subtracted from several targets (consumed only on its last use).
    void setKeepTool(bool keep) { m_keepTool = keep; }

    // Getters
    int getTargetBodyId() const { return m_targetBodyId; }
    int getToolBodyId() const { return m_toolBodyId; }
    BooleanMode getMode() const { return m_mode; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Boolean"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "boolean"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override {
        return {m_targetBodyId, m_toolBodyId};
    }
    // A boolean references its target/tool purely by body id (+ a mode enum),
    // so it can reload as a fully editable real op - recomputing from upstream
    // geometry on edit instead of baking a stale result over it (the bug where
    // editing a fillet upstream of a reloaded union silently did nothing).
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_targetBodyId = -1;
    int m_toolBodyId = -1;
    BooleanMode m_mode = BooleanMode::Union;
    bool m_keepTool = false;  // keep the tool body instead of consuming it

    // For undo
    TopoDS_Shape m_previousTargetShape;
    TopoDS_Shape m_previousToolShape;
    // Input face lineage of both bodies, captured at execute; undo restores
    // them so a PARTIAL replay (editStep starting after this boolean) still
    // has ancestry for the ops it re-runs.
    materializr::topo::FaceIdMap m_prevTargetFaceIds;
    materializr::topo::FaceIdMap m_prevToolFaceIds;
    // Ids minted to complete the published map (faces with no inherited
    // ancestry - e.g. when the inputs carried no lineage at all). REUSED
    // across re-executes while the uncovered-face count is unchanged, so a
    // downstream op's stored face-id pairs stay valid through a replay.
    std::vector<int> m_mintedIds;
    // Generation map of the last execute(): input FACES (of BOTH the target
    // and the tool) -> the faces/edges they produced. Lets the "gen" naming
    // strategy name a boolean SEAM sub-shape by the two faces that made it.
    materializr::topo::GenerationLedger m_ledger;
public:
    const materializr::topo::GenerationLedger& generationLedger() const {
        return m_ledger;
    }
private:
    int m_removedToolId = -1;
};
