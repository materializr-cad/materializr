#include "ui/LengthField.h"
#include "PatternOp.h"
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <imgui.h>
#include <cmath>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PatternOp::PatternOp() = default;

void PatternOp::setBody(int id) {
    m_bodyId = id;
}

void PatternOp::setType(PatternType t) {
    m_type = t;
}

void PatternOp::setCount(int n) {
    m_count = n;
}

void PatternOp::setLinearSpacing(double x, double y, double z) {
    m_spacingX = x;
    m_spacingY = y;
    m_spacingZ = z;
}

void PatternOp::setRadialAxis(double ax, double ay, double az) {
    m_axisX = ax;
    m_axisY = ay;
    m_axisZ = az;
}

void PatternOp::setRadialOrigin(double ox, double oy, double oz) {
    m_originX = ox;
    m_originY = oy;
    m_originZ = oz;
}

void PatternOp::setTotalAngle(double deg) {
    m_totalAngle = deg;
}

bool PatternOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_count < 2) {
        return false;
    }

    try {
        TopoDS_Shape sourceShape = doc.getBody(m_bodyId);
        if (sourceShape.IsNull()) {
            return false;
        }

        m_createdBodyIds.clear();
        m_reuseIdx = 0; // walks m_reuseBodyIds as each pattern copy is emitted

        for (int i = 1; i < m_count; ++i) {
            gp_Trsf trsf;

            if (m_type == PatternType::Linear) {
                // Translate by i * spacing
                gp_Vec offset(
                    i * m_spacingX,
                    i * m_spacingY,
                    i * m_spacingZ
                );
                trsf.SetTranslation(offset);
            } else {
                // Radial: rotate by i * (totalAngle / count) around axis
                double stepAngle = (m_totalAngle / m_count) * i;
                double stepAngleRad = stepAngle * M_PI / 180.0;
                gp_Ax1 axis(gp_Pnt(m_originX, m_originY, m_originZ),
                            gp_Dir(m_axisX, m_axisY, m_axisZ));
                trsf.SetRotation(axis, stepAngleRad);
            }

            BRepBuilderAPI_Transform transform(sourceShape, trsf, true);
            transform.Build();
            if (!transform.IsDone()) {
                // Undo any bodies created so far
                for (int createdId : m_createdBodyIds) {
                    doc.removeBody(createdId);
                }
                m_createdBodyIds.clear();
                return false;
            }

            // Reuse the prior id for this slot on redo so the body's folder
            // / colour / visibility / name come back via Document's tombstone
            // restore. On first execute m_reuseBodyIds is empty so addOrPutBody
            // allocates a fresh id.
            int newId = (m_reuseIdx < m_reuseBodyIds.size())
                          ? m_reuseBodyIds[m_reuseIdx] : -1;
            doc.addOrPutBody(newId, transform.Shape(), "Pattern " + std::to_string(i));
            m_createdBodyIds.push_back(newId);
            ++m_reuseIdx;
        }

        return true;
    } catch (...) {
        // Clean up on exception
        for (int createdId : m_createdBodyIds) {
            try { doc.removeBody(createdId); } catch (...) {}
        }
        m_createdBodyIds.clear();
        return false;
    }
}

bool PatternOp::undo(Document& doc) {
    try {
        // Remove all created pattern copies
        for (int createdId : m_createdBodyIds) {
            doc.removeBody(createdId);
        }
        // Save the ids for the next redo so addOrPutBody picks up the
        // tombstoned folder / colour / visibility / name per copy.
        m_reuseBodyIds = std::move(m_createdBodyIds);
        m_createdBodyIds.clear();
        m_reuseIdx = 0;
        return true;
    } catch (...) {
        return false;
    }
}

std::string PatternOp::description() const {
    std::string typeStr = (m_type == PatternType::Linear) ? "Linear" : "Radial";
    return typeStr + " Pattern, " + std::to_string(m_count) + " copies";
}

std::string PatternOp::serializeParams() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "body=%d;type=%d;count=%d;sx=%.6f;sy=%.6f;sz=%.6f;ax=%.6f;ay=%.6f;az=%.6f;"
        "ox=%.6f;oy=%.6f;oz=%.6f;angle=%.6f",
        m_bodyId, static_cast<int>(m_type), m_count,
        m_spacingX, m_spacingY, m_spacingZ,
        m_axisX, m_axisY, m_axisZ,
        m_originX, m_originY, m_originZ, m_totalAngle);
    return buf;
}

bool PatternOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        double d = std::atof(val.c_str());
        int    i = std::atoi(val.c_str());
        if      (key == "body")  { m_bodyId = i; any = true; }
        else if (key == "type")  { m_type = (i == 1) ? PatternType::Radial : PatternType::Linear; any = true; }
        else if (key == "count") { m_count = i; any = true; }
        else if (key == "sx")    { m_spacingX = d; any = true; }
        else if (key == "sy")    { m_spacingY = d; any = true; }
        else if (key == "sz")    { m_spacingZ = d; any = true; }
        else if (key == "ax")    { m_axisX = d; any = true; }
        else if (key == "ay")    { m_axisY = d; any = true; }
        else if (key == "az")    { m_axisZ = d; any = true; }
        else if (key == "ox")    { m_originX = d; any = true; }
        else if (key == "oy")    { m_originY = d; any = true; }
        else if (key == "oz")    { m_originZ = d; any = true; }
        else if (key == "angle") { m_totalAngle = d; any = true; }
        pos = end + 1;
    }
    return any;
}

bool PatternOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    // deserializeParams has already restored m_bodyId + the pattern params.
    // Without a valid source body the op can't re-execute, so keep it as a
    // baked ReplayOp instead.
    if (m_bodyId < 0) return false;
    // A pattern with count >= 2 must have created copies. An empty created
    // set means the file was saved before PatternOp had captureDiff (the
    // step's diff is missing) — decline so undo doesn't silently no-op.
    if (state.created.empty()) return false;

    // The bodies this step created ARE the pattern copies (copies 1..count-1;
    // copy 0 is the original source, which the op never creates). Adopt them
    // as our post-execution state so undo() removes exactly these and a redo
    // / parameter-edit reuses their ids. Order matches execution order
    // (preserved through save/load), so m_reuseBodyIds lines up on redo.
    m_createdBodyIds = state.created;
    m_reuseBodyIds.clear();
    m_reuseIdx = 0;
    return true;
}

void PatternOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Pattern"));
    ImGui::Separator();

    const char* typeItems[] = { "Linear", "Circular" };
    int typeIndex = static_cast<int>(m_type);
    if (ImGui::Combo(materializr::tr("Type"), &typeIndex, typeItems, 2)) {
        m_type = static_cast<PatternType>(typeIndex);
    }

    materializr::inputNumberInt("Count", &m_count);
    if (m_count < 2) {
        m_count = 2;
    }

    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);

    if (m_type == PatternType::Linear) {
        materializr::lengthField(materializr::tr("Spacing X"), &m_spacingX);
        materializr::lengthField(materializr::tr("Spacing Y"), &m_spacingY);
        materializr::lengthField(materializr::tr("Spacing Z"), &m_spacingZ);
    } else {
        materializr::inputNumber(materializr::tr("Axis X"), &m_axisX, 0.1, 1.0, "%g");
        materializr::inputNumber(materializr::tr("Axis Y"), &m_axisY, 0.1, 1.0, "%g");
        materializr::inputNumber(materializr::tr("Axis Z"), &m_axisZ, 0.1, 1.0, "%g");
        materializr::inputNumber(materializr::tr("Total Angle"), &m_totalAngle, 1.0, 15.0, "%.1f");
    }

    if (!m_createdBodyIds.empty()) {
        ImGui::Text(materializr::tr("Created %d copies"), static_cast<int>(m_createdBodyIds.size()));
    }
}

OperationDiff PatternOp::captureDiff() const {
    // The pattern's copies are the bodies this step created. Without this the
    // step saved an EMPTY diff: copies masqueraded as initial-state bodies and
    // undoing the reloaded step removed nothing.
    OperationDiff d;
    for (int id : m_createdBodyIds)
        if (id >= 0) d.created.push_back(id);
    return d;
}
