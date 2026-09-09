#include "ConstructionAxisOp.h"
#include <cstdio>
#include <cstdlib>

#include <imgui.h>
#include <cmath>
#include "../i18n.h"

ConstructionAxisOp::ConstructionAxisOp() = default;

void ConstructionAxisOp::setType(AxisCreationType type)        { m_type = type; }
void ConstructionAxisOp::setOrigin(const gp_Pnt& origin)        { m_origin = origin; }
void ConstructionAxisOp::setDirection(const gp_Dir& direction)  { m_direction = direction; }
void ConstructionAxisOp::setPoints(const gp_Pnt& p1, const gp_Pnt& p2) {
    m_p1 = p1; m_p2 = p2;
}
void ConstructionAxisOp::setName(const std::string& name)       { m_axisName = name; }

void ConstructionAxisOp::computeAxis(gp_Pnt& outOrigin, gp_Dir& outDir) const {
    switch (m_type) {
        case AxisCreationType::WorldX:
            outOrigin = m_origin;
            outDir = gp_Dir(1, 0, 0);
            return;
        case AxisCreationType::WorldY:
            // user Z (up) maps to world Y. "WorldY" here is the actual world
            // Y direction, which in user Z-up convention is the up axis.
            outOrigin = m_origin;
            outDir = gp_Dir(0, 1, 0);
            return;
        case AxisCreationType::WorldZ:
            outOrigin = m_origin;
            outDir = gp_Dir(0, 0, 1);
            return;
        case AxisCreationType::TwoPoints: {
            outOrigin = m_p1;
            gp_Vec v(m_p1, m_p2);
            if (v.Magnitude() > 1e-9) {
                outDir = gp_Dir(v);
            } else {
                outDir = gp_Dir(1, 0, 0);
            }
            return;
        }
        case AxisCreationType::ThroughFaceNormal:
        case AxisCreationType::FromCylinderAxis:
        case AxisCreationType::AlongEdge:
        case AxisCreationType::TwoPlanesIntersection:
            // Caller fills m_origin + m_direction (resolved geometry). Pass through.
            outOrigin = m_origin;
            outDir = m_direction;
            return;
    }
    outOrigin = m_origin;
    outDir = gp_Dir(1, 0, 0);
}

bool ConstructionAxisOp::execute(Document& doc) {
    try {
        gp_Pnt o; gp_Dir d;
        if (m_hasLiteralAxis) { o = m_literalOrigin; d = m_literalDir; }
        else computeAxis(o, d);
        // Prior id (kept across undo) is passed as reuseId so a redo - in
        // session or of a reloaded step - restores the axis under the same id,
        // keeping Revolve / pattern steps that reference it valid.
        m_createdAxisId = doc.addAxis(o, d, m_axisName, m_createdAxisId);
        return m_createdAxisId >= 0;
    } catch (...) {
        return false;
    }
}

bool ConstructionAxisOp::undo(Document& doc) {
    // The id is KEPT so the next execute() re-adds under it.
    if (m_createdAxisId >= 0) {
        doc.removeAxis(m_createdAxisId);
    }
    return true;
}

std::string ConstructionAxisOp::serializeParams() const {
    // Persist the COMPUTED axis + created id (picked-geometry inputs aren't
    // reconstructable across sessions). Name LAST, runs to end-of-blob.
    gp_Pnt o; gp_Dir d;
    if (m_hasLiteralAxis) { o = m_literalOrigin; d = m_literalDir; }
    else computeAxis(o, d);
    char buf[300];
    std::snprintf(buf, sizeof(buf),
        "id=%d;type=%d;ox=%.9g;oy=%.9g;oz=%.9g;dx=%.9g;dy=%.9g;dz=%.9g;name=%s",
        m_createdAxisId, static_cast<int>(m_type),
        o.X(), o.Y(), o.Z(), d.X(), d.Y(), d.Z(), m_axisName.c_str());
    return buf;
}

bool ConstructionAxisOp::deserializeParams(const std::string& blob) {
    bool any = false;
    double ox = 0, oy = 0, oz = 0, dx = 0, dy = 0, dz = 1;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = blob.substr(pos, eq - pos);
        if (key == "name") {
            m_axisName = blob.substr(eq + 1); // to end-of-blob
            any = true;
            break;
        }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string val = blob.substr(eq + 1, end - eq - 1);
        double d = std::atof(val.c_str());
        int    i = std::atoi(val.c_str());
        if      (key == "id")   { m_createdAxisId = i; any = true; }
        else if (key == "type") { m_type = static_cast<AxisCreationType>(i); any = true; }
        else if (key == "ox") { ox = d; any = true; }
        else if (key == "oy") { oy = d; any = true; }
        else if (key == "oz") { oz = d; any = true; }
        else if (key == "dx") { dx = d; any = true; }
        else if (key == "dy") { dy = d; any = true; }
        else if (key == "dz") { dz = d; any = true; }
        pos = end + 1;
    }
    if (any) {
        try {
            m_literalOrigin = gp_Pnt(ox, oy, oz);
            m_literalDir = gp_Dir(dx, dy, dz);
            m_hasLiteralAxis = true;
        } catch (...) {
            m_hasLiteralAxis = false; // zero-length dir → decline rehydration
        }
    }
    return any;
}

bool ConstructionAxisOp::rehydrateFromReload(const ReloadState& /*state*/,
                                             Document& doc) {
    // The axis is persisted as a document entity and already loaded - verify
    // the recorded id resolves so undo() removes the right one.
    if (!m_hasLiteralAxis || m_createdAxisId < 0) return false;
    return doc.getAxis(m_createdAxisId) != nullptr;
}

std::string ConstructionAxisOp::description() const {
    const char* typeStr = "X";
    switch (m_type) {
        case AxisCreationType::WorldX:            typeStr = "World X"; break;
        case AxisCreationType::WorldY:            typeStr = "World Y (Z-up)"; break;
        case AxisCreationType::WorldZ:            typeStr = "World Z"; break;
        case AxisCreationType::TwoPoints:         typeStr = "Two points"; break;
        case AxisCreationType::ThroughFaceNormal: typeStr = "Face normal"; break;
        case AxisCreationType::FromCylinderAxis:  typeStr = "Cylinder axis"; break;
        case AxisCreationType::AlongEdge:         typeStr = "Along edge"; break;
        case AxisCreationType::TwoPlanesIntersection: typeStr = "Plane intersection"; break;
    }
    return std::string("Construction Axis (") + typeStr + ")";
}

void ConstructionAxisOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Construction Axis"));
    ImGui::Separator();
    ImGui::Text(materializr::tr("Name: %s"), m_axisName.c_str());
    ImGui::Text(materializr::tr("Origin: (%.2f, %.2f, %.2f)"),
                m_origin.X(), m_origin.Y(), m_origin.Z());
    ImGui::Text(materializr::tr("Direction: (%.3f, %.3f, %.3f)"),
                m_direction.X(), m_direction.Y(), m_direction.Z());
}
