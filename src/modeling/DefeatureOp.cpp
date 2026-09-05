#include "DefeatureOp.h"
#include "SubShapeIndex.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopoDS.hxx>
#include <imgui.h>
#include "../i18n.h"

DefeatureOp::DefeatureOp() = default;

void DefeatureOp::setBody(int id) { m_bodyId = id; }
void DefeatureOp::addFace(const TopoDS_Face& face) { m_faces.Append(face); }
void DefeatureOp::clearFaces() { m_faces.Clear(); }

bool DefeatureOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_faces.IsEmpty()) return false;

    try {
        m_previousShape = doc.getBody(m_bodyId);
        if (m_previousShape.IsNull()) return false;

        BRepAlgoAPI_Defeaturing df;
        df.SetShape(m_previousShape);
        df.AddFacesToRemove(m_faces);
        df.SetRunParallel(Standard_False);
        df.Build();

        if (df.HasErrors() || df.Shape().IsNull()) {
            std::fprintf(stderr,
                "[Defeature] couldn't remove %d face(s) — the surrounding faces "
                "can't be extended to close the gap.\n", m_faces.Size());
            return false;
        }
        TopoDS_Shape result = df.Shape();
        if (!BRepCheck_Analyzer(result).IsValid()) {
            std::fprintf(stderr, "[Defeature] result was invalid; rejecting.\n");
            return false;
        }

        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        return false;
    }
}

bool DefeatureOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) {
        return false;
    }
}

std::string DefeatureOp::description() const {
    return "Remove " + std::to_string(m_faces.Size()) + " face(s)";
}

void DefeatureOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Remove Feature"));
    ImGui::Separator();
    ImGui::Text(materializr::tr("Faces removed: %d"), m_faces.Size());
    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);
}

std::string DefeatureOp::serializeParams() const {
    // Removed faces persist as ordinal indices into the INPUT shape's canonical
    // face map (see SubShapeIndex.h) — same scheme as Shell/Fillet/Chamfer.
    std::string blob;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "body=%d", m_bodyId);
    blob += buf;
    if (!m_previousShape.IsNull() && !m_faces.IsEmpty()) {
        std::vector<TopoDS_Shape> faces;
        for (const TopoDS_Shape& f : m_faces) faces.push_back(f);
        std::string idx = SubShapeIndex::serialize(m_previousShape, faces,
                                                   TopAbs_FACE);
        if (!idx.empty()) blob += ";faces=" + idx;
    }
    return blob;
}

bool DefeatureOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "body")  { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "faces") { m_faceIndices = SubShapeIndex::parse(val); any = true; }
        pos = end + 1;
    }
    return any;
}

bool DefeatureOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0) return false;

    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    // Re-resolve the removed faces against the reloaded input shape. No saved
    // indices means nothing to remove — decline so it falls back to a baked op
    // rather than silently doing nothing.
    m_faces.Clear();
    if (m_faceIndices.empty()) return false;
    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_faceIndices,
                                   TopAbs_FACE, resolved)) {
        return false;
    }
    for (const auto& f : resolved) m_faces.Append(f);
    return true;
}

OperationDiff DefeatureOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}
