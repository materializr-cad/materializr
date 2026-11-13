#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <vector>
#include <string>
#include <cstddef>   // size_t

enum class PatternType { Linear, Radial };

class PatternOp : public Operation {
public:
    PatternOp();
    ~PatternOp() override = default;

    // Parameters
    void setBody(int id);
    // Pattern SEVERAL source bodies in one step: each gets its own run of
    // copies. The first id is also the "primary" one getBodyId() reports.
    void setBodies(const std::vector<int>& ids);
    void setType(PatternType t);
    void setCount(int n);
    void setLinearSpacing(double x, double y, double z);
    void setRadialAxis(double ax, double ay, double az);
    // World-space point that the radial axis passes through. Default is the
    // origin; setting it elsewhere shifts the rotation centre so the body's
    // copies orbit around that point instead of (0,0,0).
    void setRadialOrigin(double ox, double oy, double oz);
    void setTotalAngle(double deg);

    // Getters
    int getBodyId() const { return m_bodyId; }
    // Every source body: the primary first, then any extras.
    std::vector<int> getBodyIds() const;
    PatternType getType() const { return m_type; }
    int getCount() const { return m_count; }
    const std::vector<int>& getCreatedBodyIds() const { return m_createdBodyIds; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Pattern"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "pattern"; }
    OperationDiff captureDiff() const override;
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    std::vector<int> m_extraBodyIds;   // sources beyond the primary (multi-select)
    PatternType m_type = PatternType::Linear;
    int m_count = 3;

    // Linear params
    double m_spacingX = 5.0;
    double m_spacingY = 0.0;
    double m_spacingZ = 0.0;

    // Radial params
    double m_axisX = 0.0;
    double m_axisY = 1.0;
    double m_axisZ = 0.0;
    double m_originX = 0.0;
    double m_originY = 0.0;
    double m_originZ = 0.0;
    double m_totalAngle = 360.0;

    std::vector<int> m_createdBodyIds;
    // Persisted across undo so a redo reinserts each pattern copy under its
    // previous id, letting Document's tombstone restore re-supply folder /
    // colour / visibility / name.
    std::vector<int> m_reuseBodyIds;
    size_t m_reuseIdx = 0;
};
