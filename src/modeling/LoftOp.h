#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <vector>
#include <string>

namespace materializr { class Sketch; }

class LoftOp : public Operation {
public:
    LoftOp();
    ~LoftOp() override = default;

    // Parameters
    void addProfile(const TopoDS_Wire& wire);
    // Profile with holes: `outer` is the outer boundary, `holes` the inner
    // boundary wires (e.g. the inner circle of a concentric pair). The holes
    // are lofted into their own inner solids and cut from the outer loft, so a
    // ring-section profile produces a tube instead of a solid cylinder.
    void addProfile(const TopoDS_Wire& outer, const std::vector<TopoDS_Wire>& holes);
    // Section taken from a sketch: remembers which one (and whether the panel's
    // Flip reversed it) so the loft can follow that sketch when it is edited,
    // moved or deleted - see rebuildFromSketches.
    void addProfile(const TopoDS_Wire& outer, const std::vector<TopoDS_Wire>& holes,
                    int sketchId, bool reversed);
    void clearProfiles();

    // The section a sketch contributes: its outermost closed region's outer wire,
    // plus that region's holes (concentric profiles decompose into several
    // regions; taking the outermost keeps the inner loops as channels). Falls
    // back to the first closed wire when there is no region. `*fromRegion` is
    // true when the wire came from a region (a closed loop). Shared by Loft,
    // Guided Loft and Boundary Fill.
    static TopoDS_Wire sectionFromSketch(materializr::Sketch* sk,
                                         std::vector<TopoDS_Wire>& holesOut,
                                         bool* fromRegionOut = nullptr);

    // Re-take every sketch-sourced section from its sketch as it is now (the
    // cascade's pinned final state when one is set). All-or-nothing: false when
    // a source sketch has gone or no longer has a closed shape, leaving the
    // stored sections untouched.
    bool rebuildFromSketches(Document& doc);
    bool usesSketch(int sketchId) const;
    // Sketch behind section i, or -1 (a face section).
    int getProfileSketchId(int i) const {
        return i >= 0 && i < static_cast<int>(m_profileSketchIds.size()) ? m_profileSketchIds[i] : -1;
    }
    void setSolid(bool solid);   // true = solid, false = shell
    void setRuled(bool ruled);   // true = ruled surface, false = smooth
    // BRIDGE MODE: every section came from a face of ONE body. Instead of
    // adding the loft as a new body (which then cannot be unioned -- the fuse
    // of skin-tight tangent contact defeats OCCT at every setting), the loft
    // CONSUMES those faces: they are removed from the body, the loft walls are
    // sewn into the holes, and the body is replaced by the single merged
    // solid. No boolean runs at any point.
    void setBridge(int bodyId, const std::vector<TopoDS_Shape>& sourceFaces);

    // Getters
    bool isSolid() const { return m_solid; }
    bool isRuled() const { return m_ruled; }
    int profileCount() const { return static_cast<int>(m_profiles.size()); }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Loft"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "loft"; }
    // Reload support (full history replay): every step must come back as a
    // real editable op, never a frozen ReplayOp.
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;
    OperationDiff captureDiff() const override;

private:
    std::vector<TopoDS_Wire> m_profiles;
    // Parallel to m_profiles: the hole wires for each profile (empty if none).
    // m_holeProfiles[i] are the holes of m_profiles[i].
    std::vector<std::vector<TopoDS_Wire>> m_holeProfiles;
    // Parallel to m_profiles: the sketch each section came from (-1 = a face),
    // and whether Flip reversed it.
    std::vector<int> m_profileSketchIds;
    std::vector<char> m_profileReversed;
    bool m_solid = true;
    bool m_ruled = false;
    // Bridge mode (see setBridge). previousShape backs undo.
    int m_bridgeBodyId = -1;
    std::vector<TopoDS_Shape> m_bridgeFaces;
    TopoDS_Shape m_bridgePreviousShape;
    bool m_bridged = false;   // this execute() replaced the body (vs added one)

    // For undo
    int m_createdBodyId = -1;
};
