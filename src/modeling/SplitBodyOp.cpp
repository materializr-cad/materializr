#include "SplitBodyOp.h"
#include <cstdio>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopTools_ListOfShape.hxx>
#include <imgui.h>
#include "../i18n.h"

SplitBodyOp::SplitBodyOp() = default;

void SplitBodyOp::setBody(int id) {
    m_bodyId = id;
}

void SplitBodyOp::setSplitPlane(const gp_Pln& plane) {
    m_splitPlane = plane;
}

bool SplitBodyOp::execute(Document& doc) {
    if (m_bodyId < 0) {
        return false;
    }

    try {
        // Store previous shape for undo
        m_previousShape = doc.getBody(m_bodyId);

        // Create a large planar face from the split plane to act as the
        // splitting tool. The size should be large enough to fully
        // intersect the body.
        BRepBuilderAPI_MakeFace faceMaker(m_splitPlane, -1000.0, 1000.0, -1000.0, 1000.0);
        faceMaker.Build();
        if (!faceMaker.IsDone()) {
            std::fprintf(stderr, "[Split] face maker FAILED\n");
            return false;
        }
        {
            gp_Pnt o = m_splitPlane.Location();
            gp_Dir n = m_splitPlane.Axis().Direction();
            std::fprintf(stderr, "[Split] plane o=(%.2f,%.2f,%.2f) n=(%.2f,%.2f,%.2f)\n",
                         o.X(), o.Y(), o.Z(), n.X(), n.Y(), n.Z());
        }
        TopoDS_Shape splittingFace = faceMaker.Shape();

        // Set up the splitter
        TopTools_ListOfShape arguments;
        arguments.Append(m_previousShape);

        TopTools_ListOfShape tools;
        tools.Append(splittingFace);

        BRepAlgoAPI_Splitter splitter;
        splitter.SetArguments(arguments);
        splitter.SetTools(tools);
        // Thread-cut bodies carry healed spline faces whose tolerances the
        // splitter rejects without slack ("produced 1 solid" on a plane
        // straight through the body). Same remedy as the thread cut itself.
        splitter.SetFuzzyValue(1.0e-3);
        splitter.Build();
        if (!splitter.IsDone()) {
            std::fprintf(stderr, "[Split] splitter FAILED\n");
            return false;
        }

        // Extract resulting solids
        std::vector<TopoDS_Shape> solids;
        for (TopExp_Explorer exp(splitter.Shape(), TopAbs_SOLID); exp.More(); exp.Next()) {
            solids.push_back(exp.Current());
        }

        std::fprintf(stderr, "[Split] splitter produced %d solid(s)\n",
                     static_cast<int>(solids.size()));
        if (solids.size() != 2) {
            // <2: the plane missed the body, nothing to split.
            // >2: a multi-lobed/disjoint body cut into 3+ pieces - this op
            // only ever keeps solids[0] and solids[1], so anything past
            // that would be silently discarded (#114). Fail cleanly instead
            // of dropping geometry; a proper N-way split is future work.
            return false;
        }

        // Update the original body with the first solid
        doc.updateBody(m_bodyId, solids[0]);

        // Add the second solid as a new body
        m_secondBodyId = doc.addBody(solids[1], "Split Body");

        return true;
    } catch (...) {
        return false;
    }
}

bool SplitBodyOp::undo(Document& doc) {
    try {
        // Remove the second body that was created
        if (m_secondBodyId >= 0) {
            doc.removeBody(m_secondBodyId);
            m_secondBodyId = -1;
        }

        // Restore the original body
        if (m_bodyId >= 0 && !m_previousShape.IsNull()) {
            doc.updateBody(m_bodyId, m_previousShape);
        }

        return true;
    } catch (...) {
        return false;
    }
}

std::string SplitBodyOp::description() const {
    return "Split Body " + std::to_string(m_bodyId) + " by plane";
}

void SplitBodyOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Split Body"));
    ImGui::Separator();

    ImGui::Text(materializr::tr("Body ID: %d"), m_bodyId);

    // Display plane parameters (read-only for now, set programmatically)
    gp_Pnt loc = m_splitPlane.Location();
    gp_Dir dir = m_splitPlane.Axis().Direction();
    ImGui::Text(materializr::tr("Plane origin: (%.1f, %.1f, %.1f)"), loc.X(), loc.Y(), loc.Z());
    ImGui::Text(materializr::tr("Plane normal: (%.2f, %.2f, %.2f)"), dir.X(), dir.Y(), dir.Z());

    if (m_secondBodyId >= 0) {
        ImGui::Text(materializr::tr("Second body ID: %d"), m_secondBodyId);
    }
}

OperationDiff SplitBodyOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    if (m_secondBodyId >= 0) d.created.push_back(m_secondBodyId);
    return d;
}

std::string SplitBodyOp::serializeParams() const {
    const gp_Ax3 a = m_splitPlane.Position();
    char buf[360];
    std::snprintf(buf, sizeof(buf),
                  "body=%d;second=%d;"
                  "ox=%.9g;oy=%.9g;oz=%.9g;nx=%.9g;ny=%.9g;nz=%.9g;"
                  "xx=%.9g;xy=%.9g;xz=%.9g",
                  m_bodyId, m_secondBodyId,
                  a.Location().X(), a.Location().Y(), a.Location().Z(),
                  a.Direction().X(), a.Direction().Y(), a.Direction().Z(),
                  a.XDirection().X(), a.XDirection().Y(), a.XDirection().Z());
    return buf;
}

bool SplitBodyOp::deserializeParams(const std::string& blob) {
    double v[9] = {0, 0, 0, 0, 0, 1, 1, 0, 0};
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        double d = std::atof(blob.substr(eq + 1, end - eq - 1).c_str());
        if      (key == "body")   { m_bodyId = static_cast<int>(d); any = true; }
        else if (key == "second") { m_secondBodyId = static_cast<int>(d); any = true; }
        else if (key == "ox") v[0] = d; else if (key == "oy") v[1] = d;
        else if (key == "oz") v[2] = d; else if (key == "nx") v[3] = d;
        else if (key == "ny") v[4] = d; else if (key == "nz") v[5] = d;
        else if (key == "xx") v[6] = d; else if (key == "xy") v[7] = d;
        else if (key == "xz") v[8] = d;
        pos = end + 1;
    }
    try {
        m_splitPlane = gp_Pln(gp_Ax3(gp_Pnt(v[0], v[1], v[2]),
                                     gp_Dir(v[3], v[4], v[5]),
                                     gp_Dir(v[6], v[7], v[8])));
    } catch (...) {}
    return any;
}

bool SplitBodyOp::rehydrateFromReload(const ReloadState& state, Document&) {
    if (m_bodyId < 0) return false;
    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    if (m_secondBodyId < 0 && !state.created.empty())
        m_secondBodyId = state.created.front();
    return !m_previousShape.IsNull();
}
