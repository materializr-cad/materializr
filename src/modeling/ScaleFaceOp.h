#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <string>
#include <vector>

// Scale a planar END face of a body - pinch (or flare) the body toward a
// scaled copy of that face's profile over a blend length. The winglet op:
// take a wing's tip face, scale it to 30%, and the skin tapers into it.
//
// Two modes, both built from boolean-safe constructions (the loft runs
// between two TRANSFORMED COPIES of the same wire, so profile
// compatibility is exact; boolean interfaces are planar):
//   EXTEND - loft cap-outline → scaled outline offset outward by L; fuse.
//            Adds a tapered tip of length L beyond the current face.
//   PINCH  - cut the last L off the body, intersect that tip piece with a
//            pinching frustum, fuse it back. Reshapes existing material.
class ScaleFaceOp : public Operation {
public:
    enum class Mode { Extend = 0, Pinch = 1 };

    ScaleFaceOp();

    void setBody(int id);
    void setFace(const TopoDS_Face& f);
    void setScalePercent(double s); // uniform: sets both U and V
    void setScaleUV(double su, double sv); // percent along the face's
                                           // in-plane X / Y directions
    void setLength(double l);       // blend length along the face normal
    void setMode(Mode m);
    std::vector<TopoDS_Shape*> shapeParams() override { return {&m_face}; }

    int    getBodyId() const { return m_bodyId; }
    double getScalePercent() const { return 0.5 * (m_scaleU + m_scaleV); }
    double getScaleU() const { return m_scaleU; }
    double getScaleV() const { return m_scaleV; }
    double getLength() const { return m_length; }
    Mode   getMode() const { return m_mode; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Scale Face"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "scale_face"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    TopoDS_Face m_face;             // live ref (fresh ops)
    std::vector<int> m_faceIndices; // SubShapeIndex ordinal (reloaded ops)
    double m_scaleU = 30.0; // percent along the face plane's XDirection
    double m_scaleV = 30.0; // percent along the face plane's YDirection
    double m_length = 10.0;
    Mode m_mode = Mode::Extend;
    TopoDS_Shape m_previousShape;
};
