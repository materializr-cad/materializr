#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <string>

// Sweep a closed profile along a path into a new body of constant section: the
// pipe / rail / wire-harness shape that a loft (which morphs between profiles)
// cannot make.
//
// The profile does not have to sit exactly at the start of the path or square to
// it - the user draws a circle roughly at one end of a curve and expects a tube.
// execute() therefore first puts the profile where OpenCASCADE's pipe wants it:
//   * the path end nearest the profile becomes the START (the path is reversed
//     when that is the far end);
//   * the profile is carried onto that end;
//   * with keepPerpendicular (default) it is turned to stand square to the path
//     there, so the section stays the size that was drawn. Off = swept as drawn,
//     at whatever angle it was drawn.
// A closed profile gives a solid; a bend tighter than the section (the tube
// would fold through itself) is refused rather than producing a bad body.
class SweepOp : public Operation {
public:
    SweepOp();
    ~SweepOp() override = default;

    // Parameters
    void setProfile(const TopoDS_Shape& profile); // face, compound of faces, or wire
    void setPath(const TopoDS_Wire& path);
    void setKeepPerpendicular(bool v) { m_perpendicular = v; }
    bool keepPerpendicular() const { return m_perpendicular; }

    // Source sketches, so profile and path can be re-derived on project reload
    // and by the cascade-on-sketch-edit. -1 = not from a sketch.
    void setSketchSources(int profileSketchId, int pathSketchId) {
        m_profileSketchId = profileSketchId;
        m_pathSketchId = pathSketchId;
    }
    int getProfileSketchId() const { return m_profileSketchId; }
    int getPathSketchId() const { return m_pathSketchId; }
    // Re-derive profile and path from the source sketches. False if either is
    // gone or has nothing to give. The profile is every solid region of its sketch
    // (RevolveOp::profileFromSketch); the path is the path sketch's open chain, or
    // its first closed loop when it has no open one.
    bool rebuildFromSketches(Document& doc);

    // The geometry of the sweep, without a document: the result, or a null shape
    // with `why` naming the refusal. Shared by execute() and the tests.
    static TopoDS_Shape build(const TopoDS_Shape& profile, const TopoDS_Wire& path,
                              bool keepPerpendicular, std::string* why = nullptr);

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Sweep"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "sweep"; }
    // Reload support (full history replay): every step must come back as a
    // real editable op, never a frozen ReplayOp.
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;
    OperationDiff captureDiff() const override;

    // Why the last execute() refused, in the UI's words (empty after success).
    const std::string& lastFailure() const { return m_failure; }

private:
    TopoDS_Shape m_profile;
    TopoDS_Wire m_path;
    bool m_perpendicular = true;
    int m_profileSketchId = -1;
    int m_pathSketchId = -1;
    std::string m_failure;

    // For undo
    int m_createdBodyId = -1;
};
