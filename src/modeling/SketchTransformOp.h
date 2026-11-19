#pragma once
#include "../core/Operation.h"
#include <gp_Trsf.hxx>
#include <gp_Pln.hxx>
#include <string>
#include <vector>

namespace materializr {

// Lightweight transform op for a single sketch's plane. Used when the user
// drags the Move/Rotate gizmo on a selected standalone sketch (one with no
// source body - the body-attached case still routes through TransformOp's
// sketch-propagation path). Stores the before-plane explicitly for undo so
// it works even if m_transform itself isn't perfectly invertible numerically.
class SketchTransformOp : public Operation {
public:
    SketchTransformOp() = default;
    ~SketchTransformOp() override = default;

    void setSketch(int id) { m_sketchId = id; }
    int getSketchId() const { return m_sketchId; }
    void setTransform(const gp_Trsf& t) { m_transform = t; }

    // Link-state change to apply alongside the move so one undo reverts both:
    //   -1 = leave as-is, 0 = re-link (clear detached), 1 = de-link (set detached)
    void setDetachTarget(int mode) { m_detachMode = mode; }

    // Sketches drawn on a body this sketch drives: they take the same rigid
    // transform in this one op, so the body, its host sketch and everything on
    // it move as a unit and a single undo puts all of it back (issue #185).
    void addFollower(int sketchId) { m_followers.push_back({sketchId, gp_Pln(), false}); }
    int followerCount() const { return (int)m_followers.size(); }
    int getFollowerId(int i) const { return m_followers[i].id; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;

    std::string name() const override { return "Sketch Transform"; }
    std::string description() const override { return m_description; }
    void renderProperties() override {}
    std::string typeId() const override { return "sketchtransform"; }
    // Reload support (full history replay): every step must come back as a
    // real editable op, never a frozen ReplayOp.
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_sketchId = -1;
    gp_Trsf m_transform;     // applied to the sketch's plane on execute
    gp_Pln  m_planeBefore;   // captured on first execute, restored on undo
    bool m_haveBefore = false;
    int  m_detachMode = -1;        // see setDetachTarget
    bool m_detachedBefore = false; // captured on first execute, restored on undo
    bool m_haveDetachBefore = false;
    struct Follower { int id; gp_Pln before; bool have; };
    std::vector<Follower> m_followers;
    std::string m_description = "Sketch transform";
};

} // namespace materializr
