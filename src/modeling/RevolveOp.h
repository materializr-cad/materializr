#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <string>

enum class RevolveMode { NewBody, Union, Subtract, Intersect };

class RevolveOp : public Operation {
public:
    RevolveOp();
    ~RevolveOp() override = default;

    // Parameters
    void setProfile(const TopoDS_Shape& profile);
    void setAxis(const gp_Ax1& axis);
    void setAngle(double degrees); // 0-360
    void setMode(RevolveMode mode);
    void setTargetBody(int bodyId);
    // Remember the source sketch so the profile can be re-derived on project
    // reload (and by a future cascade-on-sketch-edit). -1 = not from a sketch.
    void setSketchSource(int sketchId) { m_sketchId = sketchId; }

    // Re-derive m_profile from the source sketch's first closed region -
    // exactly what the Revolve popup used at creation time. Returns false if
    // there's no source sketch, it's gone, or it has no closed region.
    bool rebuildProfileFromSketch(Document& doc);

    // Getters
    double getAngle() const { return m_angle; }
    RevolveMode getMode() const { return m_mode; }
    int getTargetBody() const { return m_targetBodyId; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Lathe"; }
    std::string description() const override;
    void renderProperties() override;
    // typeId stays "revolve" so existing project files still load.
    std::string typeId() const override { return "revolve"; }
    OperationDiff captureDiff() const override;
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    TopoDS_Shape m_profile;
    int m_sketchId = -1; // source sketch for profile re-derivation
    gp_Ax1 m_axis = gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0));
    double m_angle = 360.0;
    RevolveMode m_mode = RevolveMode::NewBody;
    int m_targetBodyId = -1;

    // For undo
    int m_createdBodyId = -1;
    TopoDS_Shape m_previousTargetShape;

    // Axis direction components for UI editing
    double m_axisDirX = 0.0;
    double m_axisDirY = 1.0;
    double m_axisDirZ = 0.0;
    double m_axisOriginX = 0.0;
    double m_axisOriginY = 0.0;
    double m_axisOriginZ = 0.0;
};
