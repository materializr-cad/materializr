#include "SweepOp.h"
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <imgui.h>
#include "../i18n.h"
#include "../i18n.h"
#include "ParamParse.h"

SweepOp::SweepOp() = default;

void SweepOp::setProfile(const TopoDS_Shape& profile) {
    m_profile = profile;
}

void SweepOp::setPath(const TopoDS_Wire& path) {
    m_path = path;
}

bool SweepOp::execute(Document& doc) {
    if (m_profile.IsNull() || m_path.IsNull()) {
        return false;
    }

    try {
        BRepOffsetAPI_MakePipe pipe(m_path, m_profile);
        pipe.Build();
        if (!pipe.IsDone()) {
            return false;
        }

        TopoDS_Shape sweptShape = pipe.Shape();
        // Validate-or-refuse (same gate as BooleanOp/FilletOp/ShellOp): a pipe
        // along a self-intersecting or degenerate path can report IsDone() yet
        // produce a null, empty, or topologically invalid shape that would
        // later crash tessellation/boolean/save.
        if (sweptShape.IsNull()) return false;
        GProp_GProps gp;
        BRepGProp::VolumeProperties(sweptShape, gp);
        if (gp.Mass() < 1e-6) return false;
        if (!BRepCheck_Analyzer(sweptShape).IsValid()) return false;
        doc.addOrPutBody(m_createdBodyId, sweptShape, "Sweep");

        return true;
    } catch (...) {
        return false;
    }
}

bool SweepOp::undo(Document& doc) {
    try {
        if (m_createdBodyId >= 0) {
            doc.removeBody(m_createdBodyId);
            // Keep m_createdBodyId — tombstone restore on next execute().
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string SweepOp::description() const {
    return "Sweep profile along path";
}

void SweepOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Sweep"));
    ImGui::Separator();

    if (m_profile.IsNull()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", materializr::tr("No profile selected"));
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", materializr::tr("Profile set"));
    }

    if (m_path.IsNull()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", materializr::tr("No path selected"));
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", materializr::tr("Path set"));
    }

    ImGui::Separator();
    ImGui::TextWrapped("%s", materializr::tr("Select a profile (face or wire) and a path (wire) to sweep along."));
}

OperationDiff SweepOp::captureDiff() const {
    OperationDiff d;
    if (m_createdBodyId >= 0) d.created.push_back(m_createdBodyId);
    return d;
}

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS.hxx>
#include <sstream>

std::string SweepOp::serializeParams() const {
    // Profile + path are raw picked geometry — persist as an ASCII BREP
    // compound [profile, path] embedded in the params (length-prefixed,
    // last; the PARAMS_LEN container is binary-safe).
    std::string blob = "created=" + std::to_string(m_createdBodyId);
    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    bb.Add(comp, m_profile);
    bb.Add(comp, m_path);
    std::ostringstream os;
    BRepTools::Write(comp, os);
    const std::string brep = os.str();
    blob += ";brep=" + std::to_string(brep.size()) + ":" + brep;
    return blob;
}

bool SweepOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = blob.substr(pos, eq - pos);
        if (key == "brep") {
            size_t colon = blob.find(':', eq);
            if (colon == std::string::npos) break;
            // Checked length, bounded by subtraction (ParamParse.h):
            // the old `colon + 1 + n > blob.size()` wrapped on a
            // negative length and let the guard pass.
            size_t n = 0, payload = 0;
            if (!materializr::readLenPrefix(blob, eq + 1, colon, n, payload)) break;
            std::istringstream is(blob.substr(payload, n));
            TopoDS_Shape comp;
            BRep_Builder bb;
            try { BRepTools::Read(comp, is, bb); } catch (...) { return false; }
            TopoDS_Iterator it(comp);
            if (!it.More()) return false;
            m_profile = it.Value();
            it.Next();
            if (!it.More() || it.Value().ShapeType() != TopAbs_WIRE) return false;
            m_path = TopoDS::Wire(it.Value());
            any = true;
            break;
        }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        if (key == "created") {
            m_createdBodyId = std::atoi(blob.substr(eq + 1, end - eq - 1).c_str());
            any = true;
        }
        pos = end + 1;
    }
    return any && !m_profile.IsNull() && !m_path.IsNull();
}

bool SweepOp::rehydrateFromReload(const ReloadState& state, Document&) {
    if (m_profile.IsNull() || m_path.IsNull()) return false;
    if (m_createdBodyId < 0 && !state.created.empty())
        m_createdBodyId = state.created.front();
    return true;
}
