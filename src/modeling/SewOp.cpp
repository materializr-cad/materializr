#include "SewOp.h"

#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <Standard_ErrorHandler.hxx> // OCC_CATCH_SIGNALS
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <sstream>

#include "../i18n.h"
#include "ParamParse.h"

namespace {

// A blob-supplied body list sizes a vector before anything checks it.
constexpr int kMaxSewBodies = 4096;

bool isSolid(const TopoDS_Shape& s) {
    return !s.IsNull() && TopExp_Explorer(s, TopAbs_SOLID).More();
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

// Edges carried by exactly one face. A degenerate edge is a parametric artefact
// — a cone apex, a sphere pole — with no free side, so it is never a gap.
int freeEdgeCount(const TopoDS_Shape& s) {
    if (s.IsNull()) return 0;
    TopTools_IndexedDataMapOfShapeListOfShape anc;
    TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, anc);
    int n = 0;
    for (int i = 1; i <= anc.Extent(); ++i) {
        if (BRep_Tool::Degenerated(TopoDS::Edge(anc.FindKey(i)))) continue;
        if (anc.FindFromIndex(i).Extent() == 1) ++n;
    }
    return n;
}

} // namespace

SewOp::SewOp() = default;

bool SewOp::execute(Document& doc) {
    if (m_bodyIds.empty()) return false;

    try {
        OCC_CATCH_SIGNALS

        // Snapshot everything first: undo has to put back bodies that execute()
        // is about to delete, and the shapes are only reachable while they are
        // still in the document.
        const int keepId = m_bodyIds.front();
        const TopoDS_Shape keepShape = doc.getBody(keepId);
        if (keepShape.IsNull()) return false;

        std::vector<Consumed> consumed;
        std::vector<TopoDS_Shape> inputs{keepShape};
        for (size_t i = 1; i < m_bodyIds.size(); ++i) {
            const int id = m_bodyIds[i];
            const TopoDS_Shape s = doc.getBody(id);
            if (s.IsNull()) continue;
            consumed.push_back({id, s, doc.getBodyName(id), doc.isBodyVisible(id)});
            inputs.push_back(s);
        }

        int faces = 0;
        for (const auto& s : inputs)
            for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) ++faces;
        if (faces == 0) return false;

        // Tightest tolerance that closes — see the header for why this is a
        // ladder and not a slider.
        TopoDS_Shape best;
        int bestFree = -1;
        double bestTol = 0.0;
        for (double tol : {Precision::Confusion(), 1e-4, 1e-3, 1e-2}) {
            BRepBuilderAPI_Sewing sew(tol);
            for (const auto& s : inputs)
                for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next())
                    sew.Add(ex.Current());
            sew.Perform();
            const TopoDS_Shape sewn = sew.SewedShape();
            if (sewn.IsNull()) continue;
            const int free = freeEdgeCount(sewn);
            // Keep the best result seen, so a run that never closes still hands
            // back the tightest sew rather than the loosest.
            if (bestFree < 0 || free < bestFree) {
                best = sewn;
                bestFree = free;
                bestTol = tol;
            }
            if (free == 0) break;
        }
        if (best.IsNull() || bestFree < 0) return false;

        TopoDS_Shape result = best;
        m_madeSolid = false;
        if (bestFree == 0) {
            for (TopExp_Explorer ex(best, TopAbs_SHELL); ex.More(); ex.Next()) {
                TopoDS_Shell shell = TopoDS::Shell(ex.Current());
                BRepBuilderAPI_MakeSolid ms(shell);
                if (!ms.IsDone()) break;
                TopoDS_Solid solid = ms.Solid();
                // A negative volume is an inside-out shell; reversing it is the
                // whole fix. Same check the face tweak makes after ITS sew.
                if (volumeOf(solid) < 0.0) {
                    shell.Reverse();
                    BRepBuilderAPI_MakeSolid ms2(shell);
                    if (!ms2.IsDone()) break;
                    solid = ms2.Solid();
                }
                if (BRepCheck_Analyzer(solid).IsValid() &&
                    std::abs(volumeOf(solid)) > 1e-9) {
                    result = solid;
                    m_madeSolid = true;
                }
                break;
            }
        }

        // Refuse a no-op rather than spend a history step on it. With nothing
        // to join, sewing a lone body can only hand back what went in — either
        // it was already a solid, or it is a shell that stayed exactly as open
        // as it started. Checking !madeSolid alone missed the first case, which
        // is the common one: an ordinary solid re-solidified into itself and
        // reported success.
        if (consumed.empty() &&
            (isSolid(keepShape) ||
             (!m_madeSolid && bestFree == freeEdgeCount(keepShape)))) {
            std::fprintf(stderr, "[Sew] nothing to join - one body, already as "
                                 "sewn as it gets.\n");
            return false;
        }

        m_previousShape = keepShape;
        m_consumed = std::move(consumed);
        m_freeEdges = bestFree;
        m_facesSewn = faces;
        m_tolUsed = bestTol;

        for (const auto& c : m_consumed) doc.removeBody(c.id);
        doc.updateBody(keepId, result);
        return true;
    } catch (...) {
        return false;
    }
}

bool SewOp::undo(Document& doc) {
    try {
        if (!m_bodyIds.empty() && !m_previousShape.IsNull())
            doc.updateBody(m_bodyIds.front(), m_previousShape);
        // Back under their ORIGINAL ids, so any later step that names one still
        // resolves it — the DeleteOp discipline.
        for (const auto& c : m_consumed) {
            doc.putBody(c.id, c.shape, c.name);
            doc.setBodyVisible(c.id, c.visible);
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string SewOp::description() const {
    char buf[128];
    if (m_madeSolid)
        std::snprintf(buf, sizeof buf, "Sew %d surfaces into a solid",
                      static_cast<int>(m_bodyIds.size()));
    else
        std::snprintf(buf, sizeof buf, "Sew %d surfaces (%d edge%s still open)",
                      static_cast<int>(m_bodyIds.size()), m_freeEdges,
                      m_freeEdges == 1 ? "" : "s");
    return buf;
}

void SewOp::renderProperties() {
    ImGui::Text(materializr::tr("Faces sewn: %d"), m_facesSewn);
    if (m_madeSolid)
        ImGui::Text("%s", materializr::tr("Closed - this is a solid."));
    else
        ImGui::Text(materializr::tr("%d edge(s) still open."), m_freeEdges);
    ImGui::TextDisabled(materializr::tr("Joined at %.4f mm."), m_tolUsed);
}

OperationDiff SewOp::captureDiff() const {
    OperationDiff d;
    if (!m_bodyIds.empty() && !m_previousShape.IsNull())
        d.modifiedBefore.emplace_back(m_bodyIds.front(), m_previousShape);
    for (const auto& c : m_consumed) d.deletedBefore.emplace_back(c.id, c.shape);
    return d;
}

std::string SewOp::serializeParams() const {
    std::ostringstream os;
    os << "n=" << m_bodyIds.size() << ";ids=";
    for (size_t i = 0; i < m_bodyIds.size(); ++i)
        os << m_bodyIds[i] << (i + 1 < m_bodyIds.size() ? "," : "");
    os << ";solid=" << (m_madeSolid ? 1 : 0) << ";free=" << m_freeEdges;
    return os.str();
}

bool SewOp::deserializeParams(const std::string& blob) {
    m_bodyIds.clear();
    int n = -1;
    bool any = false, gotIds = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        const size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        const std::string key = blob.substr(pos, eq - pos);
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        const std::string val = blob.substr(eq + 1, end - eq - 1);

        if (key == "n") {
            int v = 0;
            // n SIZES the list below, so it is bounded before anything reserves
            // on it (ParamParse.h).
            if (!materializr::parseWholeInt(val, v) || v < 0 || v > kMaxSewBodies)
                return false;
            n = v;
            any = true;
        } else if (key == "ids") {
            std::istringstream is(val);
            std::string tok;
            while (std::getline(is, tok, ',')) {
                if (tok.empty()) continue;
                int v = 0;
                if (!materializr::parseWholeInt(tok, v) || v < 0) return false;
                if (static_cast<int>(m_bodyIds.size()) >= kMaxSewBodies) return false;
                m_bodyIds.push_back(v);
            }
            gotIds = true;
            any = true;
        } else if (key == "solid") {
            int v = 0;
            m_madeSolid = materializr::parseWholeInt(val, v) && v != 0;
        } else if (key == "free") {
            int v = 0;
            if (materializr::parseWholeInt(val, v) && v >= 0) m_freeEdges = v;
        }
        pos = end + 1;
    }
    return any && gotIds && !m_bodyIds.empty() &&
           (n < 0 || static_cast<int>(m_bodyIds.size()) == n);
}

bool SewOp::rehydrateFromReload(const ReloadState& state, Document&) {
    if (m_bodyIds.empty()) return false;
    for (const auto& [id, shape] : state.modifiedBefore)
        if (id == m_bodyIds.front()) m_previousShape = shape;
    // The bodies this step consumed, so undo can put them back after a reload.
    m_consumed.clear();
    for (const auto& [id, shape] : state.deletedBefore)
        m_consumed.push_back({id, shape, std::string(), true});
    return !m_previousShape.IsNull();
}
