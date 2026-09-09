#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <string>

// Construction axis creation modes - mirror the plane catalogue. World X /
// Y / Z give an axis along that world direction through a chosen origin
// point; TwoPoints picks origin + direction from a pair of world points;
// ThroughFaceNormal aligns to a picked face's normal.
enum class AxisCreationType {
    WorldX, WorldY, WorldZ,
    TwoPoints,
    ThroughFaceNormal,
    // Derived-from-selection modes. Like ThroughFaceNormal, the host computes
    // (origin, direction) and feeds them via setOrigin/setDirection; these
    // exist as distinct types only so the history label reads correctly.
    FromCylinderAxis,        // a cylindrical/conical face's centreline
    AlongEdge,               // a straight edge
    TwoPlanesIntersection    // the line where two planes meet
};

class ConstructionAxisOp : public Operation {
public:
    ConstructionAxisOp();
    ~ConstructionAxisOp() override = default;

    void setType(AxisCreationType type);
    void setOrigin(const gp_Pnt& origin);
    void setDirection(const gp_Dir& direction);
    void setPoints(const gp_Pnt& p1, const gp_Pnt& p2);
    void setName(const std::string& name);

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Construction Axis"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "construction_axis"; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

    int getCreatedAxisId() const { return m_createdAxisId; }

private:
    // Resolve the configured creation mode + params into an
    // (origin, direction) pair. Falls back to world-X axis on bad data.
    void computeAxis(gp_Pnt& outOrigin, gp_Dir& outDir) const;

    AxisCreationType m_type = AxisCreationType::WorldZ;
    gp_Pnt m_origin{0, 0, 0};
    gp_Dir m_direction{0, 0, 1};
    gp_Pnt m_p1, m_p2;
    std::string m_axisName = "Axis";
    int m_createdAxisId = -1;

    // Reload support: params persist the COMPUTED (origin, direction) - the
    // picked-geometry inputs aren't reconstructable - so a rehydrated op
    // re-executes from this literal. m_type is kept for the history label.
    gp_Pnt m_literalOrigin{0, 0, 0};
    gp_Dir m_literalDir{0, 0, 1};
    bool m_hasLiteralAxis = false;
};
