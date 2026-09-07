#include "ui/LengthField.h"
#include "CopyOp.h"
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"

CopyOp::CopyOp() = default;

void CopyOp::setSourceBodyId(int id) {
    m_sourceBodyId = id;
}

void CopyOp::setOffset(double dx, double dy, double dz) {
    m_dx = dx;
    m_dy = dy;
    m_dz = dz;
}

bool CopyOp::execute(Document& doc) {
    if (m_sourceBodyId < 0) {
        return false;
    }

    try {
        // Get the source shape
        const TopoDS_Shape& sourceShape = doc.getBody(m_sourceBodyId);

        // Apply translation offset
        gp_Trsf trsf;
        trsf.SetTranslation(gp_Vec(m_dx, m_dy, m_dz));

        BRepBuilderAPI_Transform transform(sourceShape, trsf, true);
        transform.Build();
        if (!transform.IsDone()) {
            return false;
        }

        // Add as a new body with a descriptive name
        std::string srcName = doc.getBodyName(m_sourceBodyId);
        std::string copyName = srcName.empty() ? "Copy" : srcName + " Copy";
        // Reuse the prior id on redo so the body's folder/colour/etc. survive
        // through undo+redo via Document's tombstone restore.
        doc.addOrPutBody(m_createdBodyId, transform.Shape(), copyName);

        return true;
    } catch (...) {
        return false;
    }
}

bool CopyOp::undo(Document& doc) {
    try {
        if (m_createdBodyId >= 0) {
            doc.removeBody(m_createdBodyId);
            // Keep m_createdBodyId for the redo path.
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string CopyOp::description() const {
    return "Duplicate body " + std::to_string(m_sourceBodyId) +
           " offset (" + std::to_string(m_dx) + ", " +
           std::to_string(m_dy) + ", " + std::to_string(m_dz) + ")";
}

void CopyOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Duplicate"));
    ImGui::Separator();

    materializr::inputNumberInt("Source Body ID", &m_sourceBodyId);

    ImGui::Text("%s", materializr::tr("Offset"));
    materializr::lengthField("X", &m_dx);
    materializr::lengthField("Y", &m_dy);
    materializr::lengthField("Z", &m_dz);

    if (m_createdBodyId >= 0) {
        ImGui::Text(materializr::tr("Created body ID: %d"), m_createdBodyId);
    }
}

OperationDiff CopyOp::captureDiff() const {
    OperationDiff d;
    if (m_createdBodyId >= 0) d.created.push_back(m_createdBodyId);
    return d;
}

std::string CopyOp::serializeParams() const {
    char buf[180];
    std::snprintf(buf, sizeof(buf),
                  "src=%d;created=%d;dx=%.9g;dy=%.9g;dz=%.9g",
                  m_sourceBodyId, m_createdBodyId, m_dx, m_dy, m_dz);
    return buf;
}

bool CopyOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        double d = std::atof(blob.substr(eq + 1, end - eq - 1).c_str());
        if      (key == "src")     { m_sourceBodyId  = static_cast<int>(d); any = true; }
        else if (key == "created") { m_createdBodyId = static_cast<int>(d); any = true; }
        else if (key == "dx") { m_dx = d; any = true; }
        else if (key == "dy") { m_dy = d; any = true; }
        else if (key == "dz") { m_dz = d; any = true; }
        pos = end + 1;
    }
    return any;
}

bool CopyOp::rehydrateFromReload(const ReloadState& state, Document&) {
    if (m_sourceBodyId < 0) return false;
    // The created body id came from the params; confirm against the diff when
    // present (older saves without the key fall back to the recorded diff).
    if (m_createdBodyId < 0 && !state.created.empty())
        m_createdBodyId = state.created.front();
    return true;   // execute() re-copies from the live source body
}
