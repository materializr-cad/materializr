#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "TopoName.h"
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <string>
#include <vector>

// Taper (draft) selected faces of a body: tilt each face by an angle about
// a NEUTRAL PLANE (which stays fixed) along a PULL DIRECTION - OCCT's
// BRepOffsetAPI_DraftAngle, the molding-draft operation. This is the
// direct-modeling "tilt a face": a cylinder wall tapers into a cone, a
// box's sides into a pyramid frustum - without needing Parasolid-class
// free-face moves.
class TaperOp : public Operation {
public:
    TaperOp();

    void setBody(int id);
    void addFace(const TopoDS_Face& f);
    void clearFaces();
    void setDirection(double x, double y, double z);
    void setNeutralPoint(double x, double y, double z);
    void setAngleDeg(double a);

    int    getBodyId()  const { return m_bodyId; }
    double getAngleDeg() const { return m_angleDeg; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    // User-facing name is "Draft" (the manufacturing term); typeId() below
    // stays "taper" because it is the on-disk key.
    std::string name() const override { return "Draft"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "taper"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    std::vector<TopoDS_Shape*> shapeParams() override {
        std::vector<TopoDS_Shape*> out;
        for (TopTools_ListIteratorOfListOfShape it(m_faces); it.More(); it.Next())
            out.push_back(&it.ChangeValue());
        return out;
    }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    TopTools_ListOfShape m_faces;   // live face refs (fresh ops)
    std::vector<int> m_faceIndices; // SubShapeIndex ordinals (reloaded ops)
    // Topological names for the drafted faces (see ShellOp): minted on the
    // first execute, resolved when the stored handles go stale because an
    // upstream edit rebuilt the body - without them a cascade replay strands
    // the taper on faces of a body that no longer exists.
    std::vector<materializr::topo::Ref> m_faceRefs;
    double m_dirX = 0.0, m_dirY = 1.0, m_dirZ = 0.0; // pull direction
    double m_nX = 0.0, m_nY = 0.0, m_nZ = 0.0;       // point on neutral plane
    double m_angleDeg = 5.0;
    TopoDS_Shape m_previousShape; // for undo
};
