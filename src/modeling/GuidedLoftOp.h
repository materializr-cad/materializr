#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <vector>
#include <string>

namespace materializr { class Sketch; }

// Guided loft ("loft with rails"): one CLOSED base profile swept away from its
// plane while conforming to 1–2 OPEN rail curves. The rails are sampled by
// height above the base plane; at each height the base profile is scaled about
// its centroid so it passes through the rail points, and the resulting stack
// of generated sections is skinned with ThruSections (with a point apex when
// the rails converge). This is the "pyramid with rounded sides" case a plain
// section loft can't express - a plain loft treats every sketch as a
// cross-section in sequence, so perpendicular "wall" profiles weave through
// themselves.
//
// v1 contract (kept deliberately narrow):
//   * base = closed planar wire; rails = open wires, each gaining height
//     monotonically-ish above the base plane (sampled, linearly interpolated).
//   * 1 rail  -> uniform scaling of the base profile.
//   * 2 rails -> anisotropic scaling: rail 1 drives the scale along its own
//     in-plane azimuth, rail 2 drives the perpendicular axis. Best when the
//     rails sit roughly 90° apart around the base (the common corner case).
class GuidedLoftOp : public Operation {
public:
    GuidedLoftOp();
    ~GuidedLoftOp() override = default;

    void setBase(const TopoDS_Wire& wire, const gp_Pln& plane);
    // `sketchId` = the sketch the rail was taken from (-1 = none).
    void addRail(const TopoDS_Wire& wire, int sketchId = -1);
    void clearRails();
    // The sketch the base profile came from, so the loft can follow it.
    void setBaseSketch(int sketchId) { m_baseSketchId = sketchId; }
    int  getBaseSketchId() const { return m_baseSketchId; }
    int  getRailSketchId(int i) const {
        return i >= 0 && i < static_cast<int>(m_railSketchIds.size()) ? m_railSketchIds[i] : -1;
    }
    int  railSketchCount() const { return static_cast<int>(m_railSketchIds.size()); }
    bool usesSketch(int sketchId) const;
    // Re-take the base profile, its plane and every rail from their sketches as
    // they are now (the cascade's pinned final state when set). All-or-nothing:
    // false when a source is missing or no longer usable.
    bool rebuildFromSketches(Document& doc);
    void setSolid(bool solid);
    void setSamples(int n);          // generated intermediate sections
    int railCount() const { return static_cast<int>(m_rails.size()); }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Guided Loft"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "guided_loft"; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;
    OperationDiff captureDiff() const override;

private:
    TopoDS_Wire m_base;
    gp_Pln m_basePlane;
    std::vector<TopoDS_Wire> m_rails;
    int m_baseSketchId = -1;
    std::vector<int> m_railSketchIds;   // parallel to m_rails
    bool m_solid = true;
    int m_samples = 24;

    int m_createdBodyId = -1;
};
