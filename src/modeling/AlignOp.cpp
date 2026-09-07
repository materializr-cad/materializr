#include "ui/LengthField.h"
#include "AlignOp.h"
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"

AlignOp::AlignOp() = default;

void AlignOp::setBodyId(int id) {
    m_bodyId = id;
}

void AlignOp::setSourcePoint(const gp_Pnt& pt) {
    m_source = pt;
}

void AlignOp::setTargetPoint(const gp_Pnt& pt) {
    m_target = pt;
}

bool AlignOp::execute(Document& doc) {
    if (m_bodyId < 0) {
        return false;
    }

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);

        // Compute translation vector from source to target
        gp_Vec translation(m_source, m_target);

        gp_Trsf trsf;
        trsf.SetTranslation(translation);

        BRepBuilderAPI_Transform transform(m_previousShape, trsf, true);
        transform.Build();
        if (!transform.IsDone()) {
            return false;
        }

        doc.updateBody(m_bodyId, transform.Shape());
        return true;
    } catch (...) {
        return false;
    }
}

bool AlignOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) {
        return false;
    }

    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) {
        return false;
    }
}

std::string AlignOp::description() const {
    return "Align body " + std::to_string(m_bodyId) +
           " from (" + std::to_string(m_source.X()) + ", " +
           std::to_string(m_source.Y()) + ", " +
           std::to_string(m_source.Z()) + ") to (" +
           std::to_string(m_target.X()) + ", " +
           std::to_string(m_target.Y()) + ", " +
           std::to_string(m_target.Z()) + ")";
}

void AlignOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Align"));
    ImGui::Separator();

    materializr::inputNumberInt("Body ID", &m_bodyId);

    double sx = m_source.X(), sy = m_source.Y(), sz = m_source.Z();
    double tx = m_target.X(), ty = m_target.Y(), tz = m_target.Z();

    ImGui::Text("%s", materializr::tr("Source Point"));
    if (materializr::lengthField(materializr::tr("Src X"), &sx) ||
        materializr::lengthField(materializr::tr("Src Y"), &sy) ||
        materializr::lengthField(materializr::tr("Src Z"), &sz)) {
        m_source.SetCoord(sx, sy, sz);
    }

    ImGui::Text("%s", materializr::tr("Target Point"));
    if (materializr::lengthField(materializr::tr("Tgt X"), &tx) ||
        materializr::lengthField(materializr::tr("Tgt Y"), &ty) ||
        materializr::lengthField(materializr::tr("Tgt Z"), &tz)) {
        m_target.SetCoord(tx, ty, tz);
    }

    // Show the computed translation
    double dx = tx - sx, dy = ty - sy, dz = tz - sz;
    ImGui::Text(materializr::tr("Translation: (%.3f, %.3f, %.3f)"), dx, dy, dz);
}

OperationDiff AlignOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string AlignOp::serializeParams() const {
    char buf[220];
    std::snprintf(buf, sizeof(buf),
                  "body=%d;sx=%.9g;sy=%.9g;sz=%.9g;tx=%.9g;ty=%.9g;tz=%.9g",
                  m_bodyId, m_source.X(), m_source.Y(), m_source.Z(),
                  m_target.X(), m_target.Y(), m_target.Z());
    return buf;
}

bool AlignOp::deserializeParams(const std::string& blob) {
    double sx=0, sy=0, sz=0, tx=0, ty=0, tz=0;
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        double d = std::atof(blob.substr(eq + 1, end - eq - 1).c_str());
        if      (key == "body") { m_bodyId = static_cast<int>(d); any = true; }
        else if (key == "sx") { sx = d; any = true; }
        else if (key == "sy") { sy = d; any = true; }
        else if (key == "sz") { sz = d; any = true; }
        else if (key == "tx") { tx = d; any = true; }
        else if (key == "ty") { ty = d; any = true; }
        else if (key == "tz") { tz = d; any = true; }
        pos = end + 1;
    }
    m_source = gp_Pnt(sx, sy, sz);
    m_target = gp_Pnt(tx, ty, tz);
    return any;
}

bool AlignOp::rehydrateFromReload(const ReloadState& state, Document&) {
    if (m_bodyId < 0) return false;
    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    return !m_previousShape.IsNull();
}
