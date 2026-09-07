#include "ui/LengthField.h"
#include "UiTheme.h"
#include "ui_scale.h"
#include "MeasureTool.h"
#include "../core/Document.h"
#include "../core/SelectionManager.h"

#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS.hxx>

#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <set>
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

// OCCT/world space is Y-up; the UI presents the user's Z-up axes
// (user X = world X, user Y = world Z, user Z = world Y). The Measure panel
// reports in the user's axes, so every world coordinate/extent it displays
// goes through this swap — mirrors the Scale panel's userToWorld[]={0,2,1}
// (Application_Dialogs.cpp). Without it a body's height (user Z) was reported
// under Y (issue #2). A pure axis swap, so it works for points and deltas
// alike and never flips a box's min/max.
static inline glm::vec3 worldToUser(const glm::vec3& w) {
    return glm::vec3(w.x, w.z, w.y);
}

MeasureTool::MeasureTool() = default;

void MeasureTool::setDocument(const Document* doc)        { m_document  = doc; }
void MeasureTool::setSelectionManager(const SelectionManager* sel) { m_selection = sel; }

void MeasureTool::setMode(MeasureMode m) {
    m_mode = m;
    m_pointsCaptured = 0;
    m_results.clear();
}

void MeasureTool::capturePoint(glm::vec3 p) {
    if (m_mode != MeasureMode::Line) return;
    if (m_pointsCaptured >= 2) {
        // Third click after a completed measurement: start over with this as
        // point 1, so the user can keep measuring without resetting manually.
        m_point1 = p;
        m_point2 = glm::vec3(0.0f);
        m_pointsCaptured = 1;
        m_results.clear();
        return;
    }
    if (m_pointsCaptured == 0) { m_point1 = p; m_pointsCaptured = 1; }
    else                       { m_point2 = p; m_pointsCaptured = 2; }
}

void MeasureTool::resetPointCapture() {
    m_pointsCaptured = 0;
    m_point1 = m_point2 = glm::vec3(0.0f);
    if (m_mode == MeasureMode::Line) m_results.clear();
}

void MeasureTool::update() {
    m_results.clear();
    switch (m_mode) {
        case MeasureMode::Object:        measureObjects();      break;
        case MeasureMode::Edge:          measureEdges();        break;
        case MeasureMode::Line:  measureLine(); break;
        default: break;
    }
}

void MeasureTool::measureObjects() {
    if (!m_document || !m_selection) return;
    // Combined bbox of every body referenced by the selection. A single click
    // on a body in the viewport selects its FACE — for the user this still
    // intuitively means "I picked that body", so we deduplicate body ids
    // across any selection type (Body / Face / Edge / Vertex) and bbox each.
    std::set<int> uniqueBodyIds;
    for (const auto& e : m_selection->getSelection()) {
        if (e.bodyId >= 0) uniqueBodyIds.insert(e.bodyId);
    }
    Bnd_Box bb;
    int count = 0;
    for (int bodyId : uniqueBodyIds) {
        try {
            const TopoDS_Shape& shape = m_document->getBody(bodyId);
            if (shape.IsNull()) continue;
            // Analytic bounds, no tolerance padding — same reasoning as the
            // Properties panel dim readout (avoids ~5–10 µm of slop on
            // cylinders/cones and STEP-imported faces).
            BRepBndLib::AddOptimal(shape, bb, Standard_False, Standard_False);
            ++count;
        } catch (...) {}
    }
    if (count == 0 || bb.IsVoid()) return;

    double x0,y0,z0,x1,y1,z1;
    bb.Get(x0,y0,z0,x1,y1,z1);

    MeasureResult r;
    r.type   = MeasureResult::BoundingBox;
    r.label  = (count == 1) ? "Bounding Box"
                            : "Bounding Box (" + std::to_string(count) + " bodies)";
    // Report extents and corners in the user's Z-up axes (see worldToUser).
    r.dimX   = x1 - x0;          // user X = world X
    r.dimY   = z1 - z0;          // user Y = world Z
    r.dimZ   = y1 - y0;          // user Z = world Y
    r.pointA = worldToUser(glm::vec3((float)x0,(float)y0,(float)z0));
    r.pointB = worldToUser(glm::vec3((float)x1,(float)y1,(float)z1));
    m_results.push_back(r);
}

void MeasureTool::measureEdges() {
    if (!m_selection) return;
    double total = 0.0;
    int count = 0;
    for (const auto& e : m_selection->getSelection()) {
        if (e.type != SelectionType::Edge || e.shape.IsNull()) continue;
        try {
            BRepAdaptor_Curve curve(TopoDS::Edge(e.shape));
            total += GCPnts_AbscissaPoint::Length(curve);
            ++count;
        } catch (...) {}
    }
    if (count == 0) return;

    MeasureResult r;
    r.type  = MeasureResult::EdgeLength;
    r.value = total;
    r.label = (count == 1) ? "Edge Length"
                           : "Total Edge Length (" + std::to_string(count) + " edges)";
    m_results.push_back(r);
}

void MeasureTool::measureLine() {
    if (m_pointsCaptured < 2) return;
    glm::vec3 d = m_point2 - m_point1;
    MeasureResult r;
    r.type   = MeasureResult::Distance;
    r.value  = static_cast<double>(glm::length(d)); // length is axis-invariant
    r.label  = "Distance";
    // Captured picks are world-space; show the panel's From/To and per-axis
    // deltas in the user's Z-up axes (see worldToUser).
    r.pointA = worldToUser(m_point1);
    r.pointB = worldToUser(m_point2);
    glm::vec3 du = worldToUser(d);
    r.dimX   = std::abs(du.x);
    r.dimY   = std::abs(du.y);
    r.dimZ   = std::abs(du.z);
    m_results.push_back(r);
}

void MeasureTool::renderPanel() {
    if (m_mode == MeasureMode::Inactive) return;

    bool open = true;
    ImGui::SetNextWindowSize(uiSz(320, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Measure", &open)) { ImGui::End(); return; }

    // Mode selector — three buttons at the top, current mode highlighted.
    auto modeButton = [&](const char* label, MeasureMode m) {
        bool isCurrent = (m_mode == m);
        if (isCurrent)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
        if (ImGui::Button(label, materializr::uiSz(95, 28))) setMode(m);
        if (isCurrent) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    modeButton("Object",     MeasureMode::Object);
    modeButton("Edge",       MeasureMode::Edge);
    modeButton("Line",   MeasureMode::Line);
    ImGui::NewLine();
    ImGui::Separator();
    ImGui::Spacing();

    // Live selection counts so the user can see the tool is actually noticing
    // what they pick.
    int bodyIds = 0, edges = 0;
    if (m_selection) {
        std::set<int> b;
        for (const auto& e : m_selection->getSelection()) {
            if (e.bodyId >= 0) b.insert(e.bodyId);
            if (e.type == SelectionType::Edge && !e.shape.IsNull()) ++edges;
        }
        bodyIds = static_cast<int>(b.size());
    }

    // Prompt + results per mode.
    switch (m_mode) {
        case MeasureMode::PickMode:
            ImGui::TextWrapped("%s", materializr::tr("Pick a measurement mode above."));
            break;
        case MeasureMode::Object:
            ImGui::TextWrapped("%s", materializr::tr("Click a body in the viewport — clicking a face counts. Ctrl+click to add more bodies, or use box-select."));
            ImGui::Spacing();
            ImGui::TextColored(materializr::accentText(),
                               materializr::tr("Selected: %d %s"), bodyIds, bodyIds == 1 ? "body" : "bodies");
            break;
        case MeasureMode::Edge:
            ImGui::TextWrapped("%s", materializr::tr("Click within ~8 px of an edge to pick it. Ctrl+click to add more edges to the sum."));
            ImGui::Spacing();
            ImGui::TextColored(materializr::accentText(),
                               materializr::tr("Selected: %d %s"), edges, edges == 1 ? "edge" : "edges");
            break;
        case MeasureMode::Line:
            if (m_pointsCaptured == 0)
                ImGui::TextDisabled("%s", materializr::tr("Click the first point in the viewport…"));
            else if (m_pointsCaptured == 1)
                ImGui::TextDisabled("%s", materializr::tr("…now click the second point."));
            else
                ImGui::TextDisabled("%s", materializr::tr("Click again to start a new measurement."));
            break;
        default: break;
    }

    if (m_results.empty()) {
        ImGui::End();
        if (!open) m_mode = MeasureMode::Inactive;
        return;
    }

    ImGui::Spacing();
    for (const auto& r : m_results) {
        ImGui::Separator();
        switch (r.type) {
            case MeasureResult::Distance:
                ImGui::TextUnformatted(materializr::trFormat("Distance: %s", materializr::fmtLength(r.value)).c_str());
                ImGui::TextUnformatted(materializr::trFormat("  \xCE\x94 %s", materializr::fmtVec3(r.dimX, r.dimY, r.dimZ)).c_str());
                ImGui::TextUnformatted(materializr::trFormat("  From: %s", materializr::fmtVec3(r.pointA.x, r.pointA.y, r.pointA.z)).c_str());
                ImGui::TextUnformatted(materializr::trFormat("  To:   %s", materializr::fmtVec3(r.pointB.x, r.pointB.y, r.pointB.z)).c_str());
                break;
            case MeasureResult::EdgeLength:
                ImGui::TextUnformatted(materializr::trFormat("%s: %s", r.label.c_str(), materializr::fmtLength(r.value)).c_str());
                break;
            case MeasureResult::FaceArea:
                ImGui::TextUnformatted(materializr::trFormat("Area: %s", materializr::fmtArea(r.value)).c_str());
                break;
            case MeasureResult::BoundingBox:
                ImGui::Text("%s", r.label.c_str());
                ImGui::TextUnformatted(materializr::trFormat("  X: %s", materializr::fmtLength(r.dimX)).c_str());
                ImGui::TextUnformatted(materializr::trFormat("  Y: %s", materializr::fmtLength(r.dimY)).c_str());
                ImGui::TextUnformatted(materializr::trFormat("  Z: %s", materializr::fmtLength(r.dimZ)).c_str());
                ImGui::TextUnformatted(materializr::trFormat("  Min: %s", materializr::fmtVec3(r.pointA.x, r.pointA.y, r.pointA.z)).c_str());
                ImGui::TextUnformatted(materializr::trFormat("  Max: %s", materializr::fmtVec3(r.pointB.x, r.pointB.y, r.pointB.z)).c_str());
                break;
            default: break;
        }
    }

    ImGui::End();
    if (!open) m_mode = MeasureMode::Inactive;
}

const std::vector<MeasureResult>& MeasureTool::getResults() const { return m_results; }

void MeasureTool::clear() {
    m_results.clear();
    m_pointsCaptured = 0;
}

} // namespace materializr
