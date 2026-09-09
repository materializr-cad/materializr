#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <TopoDS_Shape.hxx>
#include <string>

namespace materializr {

// One Op that handles all five primitives so save/load and history bookkeeping
// stays in a single place. Each Kind has its own parameter set; unused fields
// are ignored. Defaults land an axis-aligned shape at the world origin -
// users place / size it afterwards via the gizmo.
class PrimitiveOp : public Operation {
public:
    enum class Kind { Box, Cylinder, Sphere, Cone, Torus };

    PrimitiveOp();
    ~PrimitiveOp() override = default;

    void setKind(Kind k) { m_kind = k; }
    Kind getKind() const { return m_kind; }

    // Box: XYZ extents in mm.
    void setBoxExtents(double x, double y, double z) {
        m_x = x; m_y = y; m_z = z;
    }
    // Cylinder / Cone / Sphere / Torus: see field comments below.
    void setRadius(double r)        { m_radius = r; }
    void setHeight(double h)        { m_height = h; }
    void setTopRadius(double r)     { m_topRadius = r; }
    void setMinorRadius(double r)   { m_minorRadius = r; }
    // World position of the primitive's reference point (cube origin / axis
    // base / sphere centre / torus centre).
    void setOrigin(double x, double y, double z) {
        m_ox = x; m_oy = y; m_oz = z;
    }
    void setName(const std::string& n) { m_name = n; }

    // Operation interface
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Primitive"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "primitive"; }
    OperationDiff captureDiff() const override;

    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    Kind   m_kind   = Kind::Box;
    // Box extents.
    double m_x = 10.0, m_y = 10.0, m_z = 10.0;
    // Cylinder / Cone / Sphere main radius; Torus = MAJOR radius.
    double m_radius = 5.0;
    // Cylinder / Cone height. Sphere / Torus ignore.
    double m_height = 10.0;
    // Cone top radius only. Cylinder = ignore (use m_radius).
    double m_topRadius = 0.0;
    // Torus minor (tube) radius only.
    double m_minorRadius = 2.0;
    // World origin (box: corner; others: axis base / centre).
    double m_ox = 0.0, m_oy = 0.0, m_oz = 0.0;

    std::string m_name;
    int m_createdBodyId = -1;
    TopoDS_Shape m_createdShape; // remembered so undo/redo restore identically.
};

} // namespace materializr
