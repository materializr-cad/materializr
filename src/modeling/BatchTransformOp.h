#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "FaceLineage.h"
#include <TopoDS_Shape.hxx>
#include <gp_GTrsf.hxx>
#include <map>
#include <string>
#include <utility>
#include <vector>

// A multi-body gizmo transform (Move / Rotate / Scale N bodies at once).
//
// It USED to be recorded as a baked ReplayOp snapshot, so on reload it re-slammed
// its stored geometry over any upstream edit whenever the model replayed through
// it - the "batchtransform bakes" bug: a repair beneath a threaded body reverted
// the moment this step re-landed. Storing the affine transform + the body ids
// instead makes it reload as a REAL op that re-applies to the LIVE bodies, so
// upstream edits propagate through it (parity with single-body TransformOp,
// generalised to gp_GTrsf - which covers non-uniform scale - and N bodies).
class BatchTransformOp : public Operation {
public:
    BatchTransformOp() = default;
    ~BatchTransformOp() override = default;

    void setBodies(std::vector<int> ids) { m_bodyIds = std::move(ids); }
    void setTransform(const gp_GTrsf& g) { m_gtrsf = g; }
    void setLabel(std::string l) { m_label = std::move(l); }
    void setDescription(std::string d) { m_desc = std::move(d); }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return m_label.empty() ? "Transform" : m_label; }
    std::string description() const override { return m_desc; }
    void renderProperties() override {}
    std::string typeId() const override { return "batchtransform"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return m_bodyIds; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    std::vector<int> m_bodyIds;
    gp_GTrsf m_gtrsf;   // identity by default; rigid or non-uniform affine
    std::string m_label, m_desc;
    // Undo: each body's shape + face lineage before this op (updateBody wipes
    // the map, and a partial replay never re-runs the op that minted it).
    std::vector<std::pair<int, TopoDS_Shape>> m_previousShapes;
    std::map<int, materializr::topo::FaceIdMap> m_prevFaceIds;
};
