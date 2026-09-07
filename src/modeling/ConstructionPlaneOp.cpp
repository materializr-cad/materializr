#include "ui/LengthField.h"
#include "core/Units.h"
#include "ConstructionPlaneOp.h"
#include <cstdio>
#include <cstdlib>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>
#include <gp_Pnt.hxx>
#include <imgui.h>
#include <cmath>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

ConstructionPlaneOp::ConstructionPlaneOp() = default;

void ConstructionPlaneOp::setType(PlaneCreationType type) {
    m_type = type;
}

void ConstructionPlaneOp::setOffset(double distance) {
    m_offset = distance;
}

void ConstructionPlaneOp::setBasePlane(const gp_Pln& plane) {
    m_basePlane = plane;
}

void ConstructionPlaneOp::setPoints(const gp_Pnt& p1, const gp_Pnt& p2,
                                     const gp_Pnt& p3) {
    m_p1 = p1;
    m_p2 = p2;
    m_p3 = p3;
}

void ConstructionPlaneOp::setName(const std::string& name) {
    m_planeName = name;
}

gp_Pln ConstructionPlaneOp::computePlane() const {
    // Plane names are in the user's Z-up convention (user Z = world Y).
    // What the popup labels "XY" is the floor; its normal is the up axis,
    // which in world coords is +Y. Same remap applies to the other two.
    //   user XY  →  world plane normal = world +Y  (floor; offset is height)
    //   user XZ  →  world plane normal = world +Z  (front/back wall)
    //   user YZ  →  world plane normal = world +X  (left/right wall)
    // Offset slides along that normal so the slider reads as user-Z offset
    // for the floor case, user-Y for the front, user-X for the side.
    switch (m_type) {
        case PlaneCreationType::XY: {
            return gp_Pln(gp_Pnt(0, m_offset, 0), gp_Dir(0, 1, 0));
        }

        case PlaneCreationType::XZ: {
            return gp_Pln(gp_Pnt(0, 0, m_offset), gp_Dir(0, 0, 1));
        }

        case PlaneCreationType::YZ: {
            return gp_Pln(gp_Pnt(m_offset, 0, 0), gp_Dir(1, 0, 0));
        }

        case PlaneCreationType::OffsetFromPlane: {
            // Translate the base plane along its normal by the offset distance
            gp_Dir normal = m_basePlane.Axis().Direction();
            gp_Pnt origin = m_basePlane.Axis().Location();
            gp_Pnt newOrigin(
                origin.X() + normal.X() * m_offset,
                origin.Y() + normal.Y() * m_offset,
                origin.Z() + normal.Z() * m_offset
            );
            return gp_Pln(newOrigin, normal);
        }

        case PlaneCreationType::ThroughThreePoints: {
            // Compute plane from 3 points
            gp_Vec v1(m_p1, m_p2);
            gp_Vec v2(m_p1, m_p3);
            gp_Vec normal = v1.Crossed(v2);

            // Check for degenerate case (collinear points)
            if (normal.Magnitude() < 1e-10) {
                // Fall back to XY plane
                return gp_Pln(m_p1, gp_Dir(0, 0, 1));
            }

            gp_Dir dir(normal);
            return gp_Pln(m_p1, dir);
        }

        case PlaneCreationType::ParallelToFace:
        case PlaneCreationType::Midplane:
        case PlaneCreationType::NormalToAxis:
        case PlaneCreationType::TangentToCylinder:
        case PlaneCreationType::ThroughAxis: {
            // All four share the same form: the host pre-computed the normal
            // (stored as m_basePlane's axis) and the through point (m_p1).
            gp_Dir normal = m_basePlane.Axis().Direction();
            return gp_Pln(m_p1, normal);
        }
    }

    // Default: XY
    return gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
}

bool ConstructionPlaneOp::execute(Document& doc) {
    try {
        gp_Pln plane = m_hasLiteralPlane ? m_literalPlane : computePlane();
        // Pass the prior id (kept across undo) as reuseId so a redo — in
        // session or of a reloaded step — restores the plane under the same
        // id, keeping sketches / transform steps that reference it valid.
        m_createdPlaneId = doc.addPlane(plane, m_planeName, m_createdPlaneId);
        return m_createdPlaneId >= 0;
    } catch (...) {
        return false;
    }
}

bool ConstructionPlaneOp::undo(Document& doc) {
    // Actually remove the plane now that Document exposes the API. Without
    // this every preview cycle (radio-click XY/XZ/YZ, drag the offset
    // slider) would stack a fresh plane on top of the previous one — the
    // visible "every selection shows" + "offset has no effect" symptoms.
    // The id itself is KEPT so the next execute() re-adds under it.
    if (m_createdPlaneId >= 0) {
        doc.removePlane(m_createdPlaneId);
    }
    return true;
}

std::string ConstructionPlaneOp::serializeParams() const {
    // Persist the COMPUTED plane (location + normal + in-plane X) plus the
    // created id; the picked-geometry creation inputs can't be rebuilt across
    // sessions, but the resulting plane fully describes the op. The name goes
    // LAST and runs to end-of-blob so user names may contain ';' or '='.
    gp_Pln pln = m_hasLiteralPlane ? m_literalPlane : computePlane();
    const gp_Ax3& ax = pln.Position();
    char buf[420];
    std::snprintf(buf, sizeof(buf),
        "id=%d;type=%d;px=%.9g;py=%.9g;pz=%.9g;"
        "nx=%.9g;ny=%.9g;nz=%.9g;xx=%.9g;xy=%.9g;xz=%.9g;name=%s",
        m_createdPlaneId, static_cast<int>(m_type),
        ax.Location().X(), ax.Location().Y(), ax.Location().Z(),
        ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z(),
        ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z(),
        m_planeName.c_str());
    return buf;
}

bool ConstructionPlaneOp::deserializeParams(const std::string& blob) {
    bool any = false;
    double px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 1, xx = 1, xy = 0, xz = 0;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = blob.substr(pos, eq - pos);
        if (key == "name") {
            // Name runs to end-of-blob (may contain ';' / '=').
            m_planeName = blob.substr(eq + 1);
            any = true;
            break;
        }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string val = blob.substr(eq + 1, end - eq - 1);
        double d = std::atof(val.c_str());
        int    i = std::atoi(val.c_str());
        if      (key == "id")   { m_createdPlaneId = i; any = true; }
        else if (key == "type") { m_type = static_cast<PlaneCreationType>(i); any = true; }
        else if (key == "px") { px = d; any = true; }
        else if (key == "py") { py = d; any = true; }
        else if (key == "pz") { pz = d; any = true; }
        else if (key == "nx") { nx = d; any = true; }
        else if (key == "ny") { ny = d; any = true; }
        else if (key == "nz") { nz = d; any = true; }
        else if (key == "xx") { xx = d; any = true; }
        else if (key == "xy") { xy = d; any = true; }
        else if (key == "xz") { xz = d; any = true; }
        pos = end + 1;
    }
    if (any) {
        try {
            m_literalPlane = gp_Pln(gp_Ax3(gp_Pnt(px, py, pz),
                                           gp_Dir(nx, ny, nz),
                                           gp_Dir(xx, xy, xz)));
            m_hasLiteralPlane = true;
        } catch (...) {
            m_hasLiteralPlane = false; // degenerate dirs → decline rehydration
        }
    }
    return any;
}

bool ConstructionPlaneOp::rehydrateFromReload(const ReloadState& /*state*/,
                                              Document& doc) {
    // The plane itself was persisted as a document entity and is already
    // loaded — verify our recorded id points at it, so undo() removes the
    // right plane and a redo re-adds under the same id.
    if (!m_hasLiteralPlane || m_createdPlaneId < 0) return false;
    return doc.getPlane(m_createdPlaneId) != nullptr;
}

std::string ConstructionPlaneOp::description() const {
    std::string typeStr;
    switch (m_type) {
        case PlaneCreationType::XY:               typeStr = "XY"; break;
        case PlaneCreationType::XZ:               typeStr = "XZ"; break;
        case PlaneCreationType::YZ:               typeStr = "YZ"; break;
        case PlaneCreationType::OffsetFromPlane:   typeStr = "Offset (" + materializr::fmtLength(m_offset) + ")"; break;
        case PlaneCreationType::ThroughThreePoints: typeStr = "3 Points"; break;
        case PlaneCreationType::ParallelToFace:    typeStr = "Parallel to Face"; break;
        case PlaneCreationType::Midplane:           typeStr = "Midplane"; break;
        case PlaneCreationType::NormalToAxis:       typeStr = "Normal to Axis"; break;
        case PlaneCreationType::TangentToCylinder:  typeStr = "Tangent to Cylinder"; break;
        case PlaneCreationType::ThroughAxis:        typeStr = "Through Axis"; break;
    }
    return "Construction Plane: " + m_planeName + " (" + typeStr + ")";
}

void ConstructionPlaneOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Construction Plane"));
    ImGui::Separator();

    // Plane name
    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_planeName.c_str());
    if (ImGui::InputText(materializr::tr("Name"), nameBuf, sizeof(nameBuf))) {
        m_planeName = nameBuf;
    }

    // Plane type
    const char* typeItems[] = {
        "XY", "XZ", "YZ",
        "Offset from Plane",
        "Through 3 Points",
        "Parallel to Face",
        "Midplane",
        "Normal to Axis",
        "Tangent to Cylinder",
        "Through Axis"
    };
    int typeIndex = static_cast<int>(m_type);
    if (ImGui::Combo(materializr::tr("Type"), &typeIndex, typeItems, 10)) {
        m_type = static_cast<PlaneCreationType>(typeIndex);
    }

    // Type-specific parameters
    switch (m_type) {
        case PlaneCreationType::XY:
        case PlaneCreationType::XZ:
        case PlaneCreationType::YZ:
            ImGui::TextWrapped("%s", materializr::tr("Standard reference plane through the origin."));
            break;

        case PlaneCreationType::OffsetFromPlane:
            materializr::lengthField(materializr::tr("Offset Distance"), &m_offset);
            ImGui::TextWrapped("%s", materializr::tr("Creates a plane parallel to the base plane, offset along its normal."));
            break;

        case PlaneCreationType::ThroughThreePoints: {
            double coords1[3] = { m_p1.X(), m_p1.Y(), m_p1.Z() };
            double coords2[3] = { m_p2.X(), m_p2.Y(), m_p2.Z() };
            double coords3[3] = { m_p3.X(), m_p3.Y(), m_p3.Z() };

            { double disp[3] = { materializr::toDisplay(coords1[0]), materializr::toDisplay(coords1[1]), materializr::toDisplay(coords1[2]) };
              if (ImGui::InputScalarN("Point 1", ImGuiDataType_Double, disp, 3, nullptr, nullptr, materializr::lengthFormat())) {
                m_p1.SetCoord(materializr::toMm(disp[0]), materializr::toMm(disp[1]), materializr::toMm(disp[2]));
              } }
            { double disp[3] = { materializr::toDisplay(coords2[0]), materializr::toDisplay(coords2[1]), materializr::toDisplay(coords2[2]) };
              if (ImGui::InputScalarN("Point 2", ImGuiDataType_Double, disp, 3, nullptr, nullptr, materializr::lengthFormat())) {
                m_p2.SetCoord(materializr::toMm(disp[0]), materializr::toMm(disp[1]), materializr::toMm(disp[2]));
              } }
            { double disp[3] = { materializr::toDisplay(coords3[0]), materializr::toDisplay(coords3[1]), materializr::toDisplay(coords3[2]) };
              if (ImGui::InputScalarN("Point 3", ImGuiDataType_Double, disp, 3, nullptr, nullptr, materializr::lengthFormat())) {
                m_p3.SetCoord(materializr::toMm(disp[0]), materializr::toMm(disp[1]), materializr::toMm(disp[2]));
              } }
            break;
        }

        case PlaneCreationType::ParallelToFace: {
            double coords[3] = { m_p1.X(), m_p1.Y(), m_p1.Z() };
            { double disp[3] = { materializr::toDisplay(coords[0]), materializr::toDisplay(coords[1]), materializr::toDisplay(coords[2]) };
              if (ImGui::InputScalarN("Through Point", ImGuiDataType_Double, disp, 3, nullptr, nullptr, materializr::lengthFormat())) {
                m_p1.SetCoord(materializr::toMm(disp[0]), materializr::toMm(disp[1]), materializr::toMm(disp[2]));
              } }
            ImGui::TextWrapped("%s", materializr::tr("Creates a plane parallel to the selected face, passing through the specified point."));
            break;
        }
    }

    if (m_createdPlaneId >= 0) {
        ImGui::Separator();
        ImGui::Text(materializr::tr("Created Plane ID: %d"), m_createdPlaneId);
    }
}
