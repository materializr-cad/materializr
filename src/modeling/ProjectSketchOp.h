#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "TopoName.h"
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <string>
#include <vector>

// Project a sketch onto a body face along the sketch-plane normal, then
// ENGRAVE (cut in) or EMBOSS (raise out) the projected regions to a depth -
// the "wrap a logo onto a cylinder" operation. Each region's wires are
// projected onto the target face (BRepProj_Projection), rebuilt as a face on
// that surface, and swept along the projection direction into a stamp tool
// whose cap follows the curvature exactly. Depth is measured along the
// projection direction (a stamp pressed straight in), not along the local
// surface normal.
class ProjectSketchOp : public Operation {
public:
    enum class Mode { Engrave = 0, Emboss = 1 };

    ProjectSketchOp();

    void setBody(int id);
    void setTargetFace(const TopoDS_Face& f);
    void setSketchId(int id);
    void setRegionFilter(std::vector<int> indices); // empty = all regions
    void setDepth(double d);
    void setMode(Mode m);

    int    getBodyId() const { return m_bodyId; }
    double getDepth()  const { return m_depth; }
    Mode   getMode()   const { return m_mode; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Projection"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "project_sketch"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    int m_sketchId = -1;
    TopoDS_Face m_targetFace;       // live face ref (fresh ops)
    std::vector<int> m_faceIndices; // SubShapeIndex ordinals (reloaded ops)
    // Topological name for the target face (see MoveFaceOp): minted on the
    // first execute, resolved when the handle goes stale because an upstream
    // edit rebuilt/moved the face - a stale handle stamps at the OLD plane.
    materializr::topo::Ref m_targetRef;
    std::vector<int> m_regionFilter; // region indices; empty = all
    double m_depth = 1.0;
    Mode m_mode = Mode::Engrave;
    TopoDS_Shape m_previousShape;   // for undo
};
