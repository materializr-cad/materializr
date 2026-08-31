#include "TaperOp.h"
#include "SubShapeIndex.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Shape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "ParamParse.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TaperOp::TaperOp() = default;

void TaperOp::setBody(int id) { m_bodyId = id; }
void TaperOp::addFace(const TopoDS_Face& f) { m_faces.Append(f); }
void TaperOp::clearFaces() { m_faces.Clear(); }

void TaperOp::setDirection(double x, double y, double z) {
    m_dirX = x; m_dirY = y; m_dirZ = z;
}

void TaperOp::setNeutralPoint(double x, double y, double z) {
    m_nX = x; m_nY = y; m_nZ = z;
}

void TaperOp::setAngleDeg(double a) { m_angleDeg = a; }

bool TaperOp::execute(Document& doc) {
    if (m_bodyId < 0 || m_faces.IsEmpty() || std::abs(m_angleDeg) < 1e-3 ||
        std::abs(m_angleDeg) > 80.0) {
        return false;
    }
    try {
        m_previousShape = doc.getBody(m_bodyId);

        // Re-resolve the drafted faces against the (possibly rebuilt) body.
        // Mint topo names on the first run, while the stored handles are
        // valid; on later runs, if any handle is no longer a sub-shape of the
        // current body (an upstream edit rebuilt it), resolve the names
        // instead — otherwise DraftAngle::Add just fails on the stale face.
        {
            auto isLive = [&](const TopoDS_Shape& f) {
                for (TopExp_Explorer ex(m_previousShape, TopAbs_FACE);
                     ex.More(); ex.Next())
                    if (ex.Current().IsSame(f)) return true;
                return false;
            };
            bool allLive = !m_faces.IsEmpty();
            for (const TopoDS_Shape& f : m_faces)
                if (!isLive(f)) { allLive = false; break; }
            if (allLive && m_faceRefs.empty()) {
                materializr::topo::Context ctx;
                ctx.doc = &doc; ctx.shape = m_previousShape;
                ctx.type = TopAbs_FACE;
                for (const TopoDS_Shape& f : m_faces)
                    m_faceRefs.push_back(materializr::topo::mint(f, ctx));
            } else if (!allLive) {
                bool named = !m_faceRefs.empty();
                for (const auto& r : m_faceRefs)
                    if (r.empty()) { named = false; break; }
                std::vector<TopoDS_Shape> out;
                materializr::topo::Context rc;
                rc.doc = &doc; rc.shape = m_previousShape;
                rc.type = TopAbs_FACE;
                rc.crossRebuild = true;
                if (!named ||
                    !materializr::topo::resolveSet(m_faceRefs, rc, out) ||
                    out.size() != static_cast<size_t>(m_faces.Size())) {
                    std::fprintf(stderr, "[Taper] could not re-find the "
                                 "drafted face(s) on the rebuilt body\n");
                    return false;
                }
                m_faces.Clear();
                for (const auto& f : out) m_faces.Append(TopoDS::Face(f));
            }
        }

        gp_Dir dir(m_dirX, m_dirY, m_dirZ);
        gp_Pln neutral(gp_Pnt(m_nX, m_nY, m_nZ), dir);
        double angleRad = m_angleDeg * M_PI / 180.0;

        BRepOffsetAPI_DraftAngle draft(m_previousShape);
        for (const TopoDS_Shape& f : m_faces) {
            draft.Add(TopoDS::Face(f), dir, angleRad, neutral);
            if (!draft.AddDone()) {
                std::fprintf(stderr, "[Taper] face cannot be drafted at "
                                     "%.1f deg (geometry constraint)\n",
                             m_angleDeg);
                return false;
            }
        }
        draft.Build();
        if (!draft.IsDone()) {
            std::fprintf(stderr, "[Taper] draft build failed\n");
            return false;
        }
        TopoDS_Shape result = draft.Shape();
        if (result.IsNull()) return false;
        if (!BRepCheck_Analyzer(result).IsValid()) {
            ShapeFix_Shape fixer(result);
            fixer.Perform();
            result = fixer.Shape();
        }
        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        std::fprintf(stderr, "[Taper] execute threw\n");
        return false;
    }
}

bool TaperOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) { return false; }
}

std::string TaperOp::description() const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Draft %.1f deg (%d face(s))",
                  m_angleDeg, m_faces.Size());
    return buf;
}

void TaperOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Draft"));
    ImGui::Separator();
    materializr::inputNumber(materializr::tr("Angle (deg)"), &m_angleDeg, 0.5, 5.0, "%.1f");
    ImGui::Text(materializr::tr("Faces: %d"), m_faces.Size());
    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);
}

std::string TaperOp::serializeParams() const {
    std::string blob;
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "body=%d;angle=%.6f;dir=%.9f,%.9f,%.9f;np=%.6f,%.6f,%.6f",
                  m_bodyId, m_angleDeg, m_dirX, m_dirY, m_dirZ,
                  m_nX, m_nY, m_nZ);
    blob += buf;
    if (!m_previousShape.IsNull() && !m_faces.IsEmpty()) {
        std::vector<TopoDS_Shape> faces;
        for (const TopoDS_Shape& f : m_faces) faces.push_back(f);
        std::string idx = SubShapeIndex::serialize(m_previousShape, faces,
                                                   TopAbs_FACE);
        if (!idx.empty()) blob += ";faces=" + idx;
    }
    // Topological face names (see ShellOp): only when every face is nameable;
    // length-prefixed opaque blobs, written LAST (read to end-of-string).
    if (!m_faceRefs.empty()) {
        bool allNamed = true;
        for (const auto& r : m_faceRefs) if (r.empty()) { allNamed = false; break; }
        if (allNamed) {
            std::string rb;
            for (const auto& r : m_faceRefs) {
                std::string b = r.serialize();
                rb += std::to_string(b.size()) + ":" + b;
            }
            blob += ";facerefs=" + rb;
        }
    }
    return blob;
}

bool TaperOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        // facerefs is a length-prefixed list written last — read to the end.
        if (key == "facerefs") {
            std::string rest = blob.substr(eq + 1);
            m_faceRefs.clear();
            materializr::topo::parseRefList(rest, m_faceRefs);
            any = true;
            break;
        }
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "body")  { m_bodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "angle") { m_angleDeg = std::atof(val.c_str()); any = true; }
        else if (key == "dir") {
            std::sscanf(val.c_str(), "%lf,%lf,%lf", &m_dirX, &m_dirY, &m_dirZ);
            any = true;
        }
        else if (key == "np") {
            std::sscanf(val.c_str(), "%lf,%lf,%lf", &m_nX, &m_nY, &m_nZ);
            any = true;
        }
        else if (key == "faces") {
            m_faceIndices = SubShapeIndex::parse(val);
            any = true;
        }
        pos = end + 1;
    }
    return any;
}

bool TaperOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0 || m_faceIndices.empty()) return false;

    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    if (m_previousShape.IsNull()) return false;

    m_faces.Clear();
    std::vector<TopoDS_Shape> resolved;
    if (!SubShapeIndex::resolveAll(m_previousShape, m_faceIndices,
                                   TopAbs_FACE, resolved)) {
        return false;
    }
    for (const auto& f : resolved) m_faces.Append(TopoDS::Face(f));
    return true;
}

OperationDiff TaperOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}
