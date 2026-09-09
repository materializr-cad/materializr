#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <vector>
#include <string>

// Boundary Fill: N closed profiles on (typically non-parallel) planes, each
// treated as a SILHOUETTE of the solid viewed along its plane normal. Every
// profile is extruded symmetrically through the others' combined extent and
// the prisms are boolean-INTERSECTED - the classic visual-hull construction.
// Three orthogonal sketches (top + front + side) carve exactly the solid that
// matches all three outlines: Steve's "pyramid with two rounded sides" from a
// ground square and two curved wall profiles, the traced-photos → object
// workflow, etc.
//
// Deliberately built from extrude + Common only - no surface fitting, no
// ordering sensitivity, no seam matching. Profiles that don't mutually
// overlap simply produce an empty intersection and the op fails cleanly.
// Holes in a profile become channels: the hole's prism is subtracted from
// that silhouette's prism before intersecting.
class BoundaryFillOp : public Operation {
public:
    BoundaryFillOp();
    ~BoundaryFillOp() override = default;

    // One silhouette: closed outer wire (+ optional hole wires) on `plane`.
    void addProfile(const TopoDS_Wire& outer,
                    const std::vector<TopoDS_Wire>& holes, const gp_Pln& plane);
    void clearProfiles();
    int profileCount() const { return static_cast<int>(m_outers.size()); }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Boundary Fill"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "boundary_fill"; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;
    OperationDiff captureDiff() const override;

private:
    std::vector<TopoDS_Wire> m_outers;
    std::vector<std::vector<TopoDS_Wire>> m_holes;   // parallel to m_outers
    std::vector<gp_Pln> m_planes;                    // parallel to m_outers

    int m_createdBodyId = -1;
};
