#include "StatusBar.h"
#include "../touch_mode.h"
#include "../core/Document.h"
#include "../core/SelectionManager.h"
#include <imgui.h>
#include <cstdio>
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

StatusBar::StatusBar() = default;

void StatusBar::setDocument(const Document* doc) {
    m_document = doc;
}

void StatusBar::setSelectionManager(const SelectionManager* sel) {
    m_selection = sel;
}

void StatusBar::setCurrentTool(const std::string& tool) {
    m_currentTool = tool;
}

void StatusBar::setSketchMode(bool active) {
    m_sketchMode = active;
}

void StatusBar::setMessage(const std::string& msg) {
    m_message = msg;
}

void StatusBar::render() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    float height = 24.0f;
    if (materializr::touchMode()) {
        // The touch UI renders the font at ~2x, but a fixed 24 px bar clipped the
        // bottom of those glyphs. Size the bar to the actual font so descenders fit.
        height = ImGui::GetFontSize() + 12.0f;
    }
    ImVec2 pos(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - height);
    ImVec2 size(viewport->WorkSize.x, height);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (ImGui::Begin("##StatusBar", nullptr, flags)) {
        // Bodies count
        int bodyCount = 0;
        if (m_document) {
            bodyCount = m_document->bodyCount();
        }

        // Project name first - the thing people most want to confirm.
        ImGui::Text(materializr::tr("Project: %s"),
                    m_projectName.empty() ? "New project"
                                          : m_projectName.c_str());
        ImGui::SameLine(); ImGui::Text("|"); ImGui::SameLine();

        char bodiesText[64];
        std::snprintf(bodiesText, sizeof(bodiesText), "Bodies: %d", bodyCount);
        ImGui::Text("%s", bodiesText);

        // Selection info
        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        if (m_selection && m_selection->hasSelection()) {
            const char* typeName = "None";
            int count = 0;

            switch (m_selection->primaryType()) {
                case SelectionType::Body:
                    typeName = "Body";
                    count = m_selection->selectedBodyCount();
                    break;
                case SelectionType::Face:
                    typeName = "Face";
                    count = m_selection->selectedFaceCount();
                    break;
                case SelectionType::Edge:
                    typeName = "Edge";
                    count = m_selection->selectedEdgeCount();
                    break;
                case SelectionType::Vertex:
                    typeName = "Vertex";
                    count = static_cast<int>(m_selection->getSelection().size());
                    break;
                case SelectionType::Sketch:
                    typeName = "Sketch";
                    count = static_cast<int>(m_selection->getSelection().size());
                    break;
                case SelectionType::Plane:
                    typeName = "Plane";
                    count = static_cast<int>(m_selection->getSelection().size());
                    break;
                default:
                    break;
            }

            char selText[128];
            std::snprintf(selText, sizeof(selText), "Selection: %s (%d)", typeName, count);
            ImGui::Text("%s", selText);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", materializr::tr("Selection: None"));
        }

        // Current tool
        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        char toolText[128];
        std::snprintf(toolText, sizeof(toolText), "Tool: %s", m_currentTool.c_str());
        ImGui::Text("%s", toolText);

        // Sketch mode indicator
        if (m_sketchMode) {
            ImGui::SameLine();
            ImGui::Text("|");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), "%s", materializr::tr("[SKETCH MODE]"));
        }

        // Transient message (right-aligned)
        if (!m_message.empty()) {
            float msgWidth = ImGui::CalcTextSize(m_message.c_str()).x;
            float availWidth = ImGui::GetContentRegionAvail().x;
            if (availWidth > msgWidth + 10.0f) {
                ImGui::SameLine(ImGui::GetWindowWidth() - msgWidth - 12.0f);
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f), "%s", m_message.c_str());
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
}

} // namespace materializr
