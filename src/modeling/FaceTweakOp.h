#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "FaceTweak.h"
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <string>

// History step for a LOCAL face move (see FaceTweak.h for the engine and why it
// has to exist at all).
//
// Deliberately separate from MoveFaceOp rather than a mode of it. The two answer
// the same gesture with different geometry: MoveFaceOp shears the whole body
// through a GTransform, this rebuilds only the faces meeting the one that moved.
// Neither is a bug version of the other — a designer tapering a part wants the
// shear, a designer nudging a boss upright wants the rebuild — so they stay two
// steps in the timeline, each replaying as what the user chose.
class FaceTweakOp : public Operation {
public:
    FaceTweakOp() = default;
    ~FaceTweakOp() override = default;

    void setBody(int id) { m_bodyId = id; }
    void setFace(const TopoDS_Face& f) { m_face = f; }
    // Any rigid transform that leaves the face planar. An in-plane slide is
    // refused as NoChange — see FaceTweak.h; that is geometry, not a gap.
    void setTransform(const gp_Trsf& t) { m_xf = t; }

    int getBodyId() const { return m_bodyId; }
    const gp_Trsf& transform() const { return m_xf; }
    // Why the last execute() declined, so the panel can print it verbatim
    // instead of a generic "that didn't work".
    materializr::tweak::Refusal refusal() const { return m_refusal; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Tweak Face"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "face_tweak"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;
    void snapshotEditState() override;
    void restoreEditState() override;

private:
    // Normal + centroid of the picked face, so a replay onto a rebuilt body can
    // find it again. Same scheme ShellOp and MergeFacesOp use for their picked
    // faces — a face on a primitive or an imported body has no sketch feature
    // to name it by, which is what FaceAnchor would need.
    void captureAnchor();
    TopoDS_Face rebind(const TopoDS_Shape& base) const;

    int m_bodyId = -1;
    TopoDS_Face m_face;
    gp_Trsf m_xf;

    gp_Dir m_anchorNormal{0, 0, 1};
    gp_Pnt m_anchorPoint{0, 0, 0};
    bool m_haveAnchor = false;

    TopoDS_Shape m_previousShape;
    materializr::tweak::Refusal m_refusal = materializr::tweak::Refusal::None;

    struct EditSnap {
        TopoDS_Face face;
        TopoDS_Shape previousShape;
        gp_Trsf xf;
        bool valid = false;
    };
    EditSnap m_editSnap;
};
