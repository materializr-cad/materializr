#include "ui/LengthField.h"
#include "core/Units.h"
#include "PrimitiveOp.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <gp_Pnt.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <imgui.h>
#include "../ui/NumField.h"

#include <cstdio>
#include <sstream>
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

PrimitiveOp::PrimitiveOp() = default;

namespace {

// The UI is Z-up (user Z = the floor-up axis) but the world is Y-up (the ground
// grid is the world XZ plane). User (x,y,z) maps to world (x, z, y): user Z (up)
// → world Y, user Y (depth) → world Z. Primitives are authored in user coords and
// built in world here, so cylinders/cones/torus STAND UP along the user's up axis
// instead of lying along OCCT's default Z (the user's depth).
gp_Pnt worldPnt(double ox, double oy, double oz) {
    return gp_Pnt(ox, oz, oy);
}
// Ax2 at the user origin with the user's up (world Y) as the primary direction.
gp_Ax2 axisAt(double ox, double oy, double oz) {
    return gp_Ax2(worldPnt(ox, oy, oz), gp_Dir(0.0, 1.0, 0.0));
}

const char* kindLabel(PrimitiveOp::Kind k) {
    switch (k) {
        case PrimitiveOp::Kind::Box:      return "Box";
        case PrimitiveOp::Kind::Cylinder: return "Cylinder";
        case PrimitiveOp::Kind::Sphere:   return "Sphere";
        case PrimitiveOp::Kind::Cone:     return "Cone";
        case PrimitiveOp::Kind::Torus:    return "Torus";
    }
    return "Primitive";
}

} // namespace

bool PrimitiveOp::execute(Document& doc) {
    try {
        TopoDS_Shape s;
        switch (m_kind) {
            case Kind::Box:
                if (m_x <= 0.0 || m_y <= 0.0 || m_z <= 0.0) return false;
                // User W(x)/D(y)/H(z) → world X / Z / Y so Height grows upward.
                s = BRepPrimAPI_MakeBox(worldPnt(m_ox, m_oy, m_oz),
                                        m_x, m_z, m_y).Shape();
                break;
            case Kind::Cylinder:
                if (m_radius <= 0.0 || m_height <= 0.0) return false;
                s = BRepPrimAPI_MakeCylinder(axisAt(m_ox, m_oy, m_oz),
                                             m_radius, m_height).Shape();
                break;
            case Kind::Sphere:
                if (m_radius <= 0.0) return false;
                s = BRepPrimAPI_MakeSphere(worldPnt(m_ox, m_oy, m_oz),
                                           m_radius).Shape();
                break;
            case Kind::Cone:
                // OCCT allows a zero top radius (true cone tip). Equal radii
                // would just be a cylinder — accept either way. Both radii
                // zero is degenerate and crashes OCCT, so reject explicitly.
                if (m_radius < 0.0 || m_topRadius < 0.0 || m_height <= 0.0 ||
                    (m_radius <= 0.0 && m_topRadius <= 0.0))
                    return false;
                s = BRepPrimAPI_MakeCone(axisAt(m_ox, m_oy, m_oz),
                                         m_radius, m_topRadius, m_height).Shape();
                break;
            case Kind::Torus:
                // R > r is the geometric strict minimum: R = r is the horn-
                // torus singularity (zero-diameter hole) and R < r is a
                // self-intersecting spindle torus. Anything strictly greater
                // gives a clean ring torus that OCCT meshes fine — even
                // R = r * 1.001 produces a valid (very-thin-walled) donut.
                if (m_radius <= 0.0 || m_minorRadius <= 0.0 ||
                    m_radius <= m_minorRadius) return false;
                s = BRepPrimAPI_MakeTorus(axisAt(m_ox, m_oy, m_oz),
                                          m_radius, m_minorRadius).Shape();
                break;
        }
        if (s.IsNull()) return false;
        m_createdShape  = s;
        std::string nm  = m_name.empty() ? std::string(kindLabel(m_kind)) : m_name;
        m_createdBodyId = doc.addBody(s, nm);
        return m_createdBodyId >= 0;
    } catch (...) {
        return false;
    }
}

bool PrimitiveOp::undo(Document& doc) {
    if (m_createdBodyId < 0) return false;
    try {
        doc.removeBody(m_createdBodyId);
        return true;
    } catch (...) {
        return false;
    }
}

std::string PrimitiveOp::description() const {
    std::string lbl = kindLabel(m_kind);
    char buf[96];
    switch (m_kind) {
        case Kind::Box:
            std::snprintf(buf, sizeof(buf), " %sx%sx%s", materializr::fmtLength(m_x).c_str(), materializr::fmtLength(m_y).c_str(), materializr::fmtLength(m_z).c_str()); break;
        case Kind::Cylinder:
            std::snprintf(buf, sizeof(buf), " R%s x H%s", materializr::fmtLength(m_radius).c_str(), materializr::fmtLength(m_height).c_str()); break;
        case Kind::Sphere:
            std::snprintf(buf, sizeof(buf), " R%s", materializr::fmtLength(m_radius).c_str()); break;
        case Kind::Cone:
            std::snprintf(buf, sizeof(buf), " R%s→%s x H%s", materializr::fmtLength(m_radius).c_str(), materializr::fmtLength(m_topRadius).c_str(), materializr::fmtLength(m_height).c_str()); break;
        case Kind::Torus:
            std::snprintf(buf, sizeof(buf), " R%s r%s", materializr::fmtLength(m_radius).c_str(), materializr::fmtLength(m_minorRadius).c_str()); break;
    }
    return lbl + std::string(buf);
}

void PrimitiveOp::renderProperties() {
    ImGui::Text("%s", kindLabel(m_kind));
    ImGui::Separator();
    switch (m_kind) {
        case Kind::Box:
            materializr::lengthField(materializr::tr("Width (X)"), &m_x);
            materializr::lengthField(materializr::tr("Depth (Y)"), &m_y);
            materializr::lengthField(materializr::tr("Height (Z)"), &m_z);
            break;
        case Kind::Cylinder:
            materializr::lengthField(materializr::tr("Radius"), &m_radius);
            materializr::lengthField(materializr::tr("Height"), &m_height);
            break;
        case Kind::Sphere:
            materializr::lengthField(materializr::tr("Radius"), &m_radius);
            break;
        case Kind::Cone:
            materializr::lengthField(materializr::tr("Bottom radius"), &m_radius);
            materializr::lengthField(materializr::tr("Top radius"), &m_topRadius);
            materializr::lengthField(materializr::tr("Height"), &m_height);
            break;
        case Kind::Torus:
            materializr::lengthField(materializr::tr("Major radius"), &m_radius);
            materializr::lengthField(materializr::tr("Minor radius"), &m_minorRadius);
            break;
    }
    ImGui::Spacing();
    ImGui::Text("%s", materializr::tr("Origin"));
    materializr::lengthField("X", &m_ox);
    materializr::lengthField("Y", &m_oy);
    materializr::lengthField("Z", &m_oz);
    ImGui::Text(materializr::tr("Body ID: %d"), m_createdBodyId);

    // Same validation feedback the create popup shows — Apply Changes runs
    // execute() which short-circuits on invalid params, and History's
    // lastGoodParams rescue restores the previous shape. Without this
    // warning the user has no idea WHY the radius "doesn't take" until
    // something downstream crashes. (Steve: editing a torus to R = r in
    // History crashed.)
    const char* problem = nullptr;
    switch (m_kind) {
    case Kind::Box:
        if (m_x <= 0.0 || m_y <= 0.0 || m_z <= 0.0)
            problem = "All three extents must be > 0.";
        break;
    case Kind::Cylinder:
        if (m_radius <= 0.0 || m_height <= 0.0)
            problem = "Radius and height must be > 0.";
        break;
    case Kind::Sphere:
        if (m_radius <= 0.0) problem = "Radius must be > 0.";
        break;
    case Kind::Cone:
        if (m_radius < 0.0 || m_topRadius < 0.0 || m_height <= 0.0 ||
            (m_radius <= 0.0 && m_topRadius <= 0.0))
            problem = "Height > 0 and at least one positive radius.";
        break;
    case Kind::Torus:
        if (m_radius <= 0.0 || m_minorRadius <= 0.0)
            problem = "Major and minor must both be > 0.";
        else if (m_radius <= m_minorRadius)
            problem = "Major must exceed minor (equal = horn-torus "
                      "singularity; smaller = self-intersecting spindle).";
        break;
    }
    if (problem) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                           materializr::tr("Invalid: %s\nApply will be rolled back."), problem);
        ImGui::PopTextWrapPos();
    }
}

OperationDiff PrimitiveOp::captureDiff() const {
    OperationDiff d;
    if (m_createdBodyId >= 0) d.created.push_back(m_createdBodyId);
    return d;
}

std::string PrimitiveOp::serializeParams() const {
    std::ostringstream o;
    o << "kind=" << static_cast<int>(m_kind)
      << ";name=" << m_name
      << ";ox=" << m_ox << ";oy=" << m_oy << ";oz=" << m_oz
      << ";x=" << m_x << ";y=" << m_y << ";z=" << m_z
      << ";r=" << m_radius << ";h=" << m_height
      << ";tr=" << m_topRadius << ";mr=" << m_minorRadius;
    return o.str();
}

bool PrimitiveOp::deserializeParams(const std::string& blob) {
    // Tiny KEY=VALUE;KEY=VALUE… parser. The same shape is emitted by
    // serializeParams above; unknown keys are silently ignored.
    size_t i = 0, n = blob.size();
    while (i < n) {
        size_t eq = blob.find('=', i);
        if (eq == std::string::npos) break;
        size_t sc = blob.find(';', eq);
        if (sc == std::string::npos) sc = n;
        std::string k = blob.substr(i, eq - i);
        std::string v = blob.substr(eq + 1, sc - eq - 1);
        try {
            if      (k == "kind") m_kind = static_cast<Kind>(std::stoi(v));
            else if (k == "name") m_name = v;
            else if (k == "ox") m_ox = std::stod(v);
            else if (k == "oy") m_oy = std::stod(v);
            else if (k == "oz") m_oz = std::stod(v);
            else if (k == "x")  m_x  = std::stod(v);
            else if (k == "y")  m_y  = std::stod(v);
            else if (k == "z")  m_z  = std::stod(v);
            else if (k == "r")  m_radius      = std::stod(v);
            else if (k == "h")  m_height      = std::stod(v);
            else if (k == "tr") m_topRadius   = std::stod(v);
            else if (k == "mr") m_minorRadius = std::stod(v);
        } catch (...) { /* keep prior value */ }
        i = sc + 1;
    }
    return true;
}

bool PrimitiveOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    // A primitive creates exactly one body; pick up that id so undo / redo
    // round-trip across a project save/load without re-running execute().
    if (!state.created.empty()) {
        m_createdBodyId = state.created.front();
        return true;
    }
    return false;
}

} // namespace materializr
