#include "core/Units.h"
#include "DrawingView.h"
#include "../core/Document.h"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <gp_Pnt.hxx>

#include <fstream>
#include <cstdio>
#include <cmath>
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

static const char* projectionDirName(ProjectionDir dir) {
    switch (dir) {
        case ProjectionDir::Front:     return "Front";
        case ProjectionDir::Back:      return "Back";
        case ProjectionDir::Left:      return "Left";
        case ProjectionDir::Right:     return "Right";
        case ProjectionDir::Top:       return "Top";
        case ProjectionDir::Bottom:    return "Bottom";
        case ProjectionDir::Isometric: return "Isometric";
    }
    return "Unknown";
}

DrawingView::DrawingView() = default;

void DrawingView::setDocument(const Document* doc) {
    m_document = doc;
}

glm::vec2 DrawingView::projectPoint(const gp_Pnt& pt, ProjectionDir dir) {
    double x = pt.X();
    double y = pt.Y();
    double z = pt.Z();

    switch (dir) {
        case ProjectionDir::Front:
            // Looking along -Y: project onto XZ plane
            return glm::vec2(static_cast<float>(x), static_cast<float>(z));
        case ProjectionDir::Back:
            // Looking along +Y: project onto XZ plane, X flipped
            return glm::vec2(static_cast<float>(-x), static_cast<float>(z));
        case ProjectionDir::Right:
            // Looking along -X: project onto YZ plane
            return glm::vec2(static_cast<float>(y), static_cast<float>(z));
        case ProjectionDir::Left:
            // Looking along +X: project onto YZ plane, Y flipped
            return glm::vec2(static_cast<float>(-y), static_cast<float>(z));
        case ProjectionDir::Top:
            // Looking along -Z: project onto XY plane
            return glm::vec2(static_cast<float>(x), static_cast<float>(-y));
        case ProjectionDir::Bottom:
            // Looking along +Z: project onto XY plane, Y not flipped
            return glm::vec2(static_cast<float>(x), static_cast<float>(y));
        case ProjectionDir::Isometric: {
            // Standard isometric: rotate 45 degrees around Z, then ~35.264 around X
            // Using the standard isometric projection matrix
            const float a = static_cast<float>(std::cos(glm::radians(30.0f)));
            const float b = static_cast<float>(std::sin(glm::radians(30.0f)));
            float px = a * static_cast<float>(x) - a * static_cast<float>(y);
            float py = b * static_cast<float>(x) + b * static_cast<float>(y) + static_cast<float>(z);
            return glm::vec2(px, py);
        }
    }
    return glm::vec2(static_cast<float>(x), static_cast<float>(z));
}

void DrawingView::projectShape(const TopoDS_Shape& shape, ProjectionDir dir,
                               std::vector<DrawingLine>& visible,
                               std::vector<DrawingLine>& hidden) {
    // MVP approach: iterate all edges and discretize them into line segments.
    // All lines go into visible; hidden-line detection is not implemented yet.
    (void)hidden;

    for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(explorer.Current());

        try {
            BRepAdaptor_Curve curve(edge);
            // Discretize with angular deflection of 5 degrees and curvature deflection of 0.1
            GCPnts_TangentialDeflection discretizer(curve, 0.1, glm::radians(5.0f));

            int nbPoints = discretizer.NbPoints();
            if (nbPoints < 2) continue;

            for (int i = 1; i < nbPoints; ++i) {
                gp_Pnt p1 = discretizer.Value(i);
                gp_Pnt p2 = discretizer.Value(i + 1);

                DrawingLine line;
                line.start = projectPoint(p1, dir);
                line.end = projectPoint(p2, dir);
                line.isHidden = false;
                line.weight = 1.0f;
                visible.push_back(line);
            }
        } catch (...) {
            // Skip edges that cannot be adapted (e.g., degenerate edges)
            continue;
        }
    }
}

void DrawingView::addView(ProjectionDir dir, glm::vec2 sheetPos, float scale) {
    DrawingViewData view;
    view.direction = dir;
    view.name = projectionDirName(dir);
    view.position = sheetPos;
    view.scale = scale;
    m_views.push_back(view);
}

void DrawingView::generateViews() {
    m_views.clear();

    if (!m_document) return;

    // Create default views: Front, Top, Right, Isometric
    // Auto-layout: divide the sheet into a 2x2 grid with margins
    float marginX = 30.0f;
    float marginY = 30.0f;
    float cellW = (m_sheetWidth - 3.0f * marginX) / 2.0f;
    float cellH = (m_sheetHeight - 3.0f * marginY) / 2.0f;

    // Top-left: Front view
    addView(ProjectionDir::Front,
            glm::vec2(marginX + cellW * 0.5f, marginY + cellH * 0.5f));

    // Top-right: Right view
    addView(ProjectionDir::Right,
            glm::vec2(marginX * 2.0f + cellW * 1.5f, marginY + cellH * 0.5f));

    // Bottom-left: Top view
    addView(ProjectionDir::Top,
            glm::vec2(marginX + cellW * 0.5f, marginY * 2.0f + cellH * 1.5f));

    // Bottom-right: Isometric view
    addView(ProjectionDir::Isometric,
            glm::vec2(marginX * 2.0f + cellW * 1.5f, marginY * 2.0f + cellH * 1.5f));

    // Project all bodies into each view
    std::vector<int> bodyIds = m_document->getAllBodyIds();
    for (auto& view : m_views) {
        view.visibleLines.clear();
        view.hiddenLines.clear();
        for (int id : bodyIds) {
            if (!m_document->isBodyVisible(id)) continue;
            try {
                const TopoDS_Shape& shape = m_document->getBody(id);
                if (shape.IsNull()) continue;
                projectShape(shape, view.direction, view.visibleLines, view.hiddenLines);
            } catch (...) {
                continue;
            }
        }
    }
}

void DrawingView::renderSheet() {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetCursorScreenPos();
    ImVec2 winSize = ImGui::GetContentRegionAvail();

    // Calculate scale to fit the sheet in the available window space
    float scaleX = winSize.x / m_sheetWidth;
    float scaleY = winSize.y / m_sheetHeight;
    float displayScale = (scaleX < scaleY) ? scaleX : scaleY;
    displayScale *= 0.95f; // small margin

    float sheetW = m_sheetWidth * displayScale;
    float sheetH = m_sheetHeight * displayScale;

    // Center the sheet
    float offsetX = winPos.x + (winSize.x - sheetW) * 0.5f;
    float offsetY = winPos.y + (winSize.y - sheetH) * 0.5f;

    // Sheet background (white)
    drawList->AddRectFilled(
        ImVec2(offsetX, offsetY),
        ImVec2(offsetX + sheetW, offsetY + sheetH),
        IM_COL32(255, 255, 255, 255));

    // Sheet border (black)
    drawList->AddRect(
        ImVec2(offsetX, offsetY),
        ImVec2(offsetX + sheetW, offsetY + sheetH),
        IM_COL32(0, 0, 0, 255), 0.0f, 0, 2.0f);

    // Title block area (bottom-right corner)
    float titleBlockW = 80.0f * displayScale;
    float titleBlockH = 30.0f * displayScale;
    float tbX = offsetX + sheetW - titleBlockW - 5.0f * displayScale;
    float tbY = offsetY + sheetH - titleBlockH - 5.0f * displayScale;

    drawList->AddRect(
        ImVec2(tbX, tbY),
        ImVec2(tbX + titleBlockW, tbY + titleBlockH),
        IM_COL32(0, 0, 0, 200), 0.0f, 0, 1.0f);

    // Title block text
    drawList->AddText(
        ImVec2(tbX + 4.0f, tbY + 4.0f),
        IM_COL32(0, 0, 0, 200),
        "Materializr Drawing");

    // Draw each view
    for (const auto& view : m_views) {
        // View label
        float labelX = offsetX + view.position.x * displayScale - 20.0f;
        float labelY = offsetY + view.position.y * displayScale - 40.0f * displayScale;
        drawList->AddText(
            ImVec2(labelX, labelY),
            IM_COL32(80, 80, 80, 200),
            view.name.c_str());

        // Visible lines (black, solid)
        for (const auto& line : view.visibleLines) {
            float x1 = offsetX + (view.position.x + line.start.x * view.scale) * displayScale;
            float y1 = offsetY + (view.position.y - line.start.y * view.scale) * displayScale;
            float x2 = offsetX + (view.position.x + line.end.x * view.scale) * displayScale;
            float y2 = offsetY + (view.position.y - line.end.y * view.scale) * displayScale;
            drawList->AddLine(
                ImVec2(x1, y1), ImVec2(x2, y2),
                IM_COL32(0, 0, 0, 255),
                line.weight * displayScale * 0.5f);
        }

        // Hidden lines (gray, dashed approximation using short segments)
        for (const auto& line : view.hiddenLines) {
            float x1 = offsetX + (view.position.x + line.start.x * view.scale) * displayScale;
            float y1 = offsetY + (view.position.y - line.start.y * view.scale) * displayScale;
            float x2 = offsetX + (view.position.x + line.end.x * view.scale) * displayScale;
            float y2 = offsetY + (view.position.y - line.end.y * view.scale) * displayScale;

            // Approximate dashed line: draw in dash-gap pattern
            float dx = x2 - x1;
            float dy = y2 - y1;
            float length = std::sqrt(dx * dx + dy * dy);
            if (length < 0.001f) continue;

            float dashLen = 4.0f * displayScale;
            float gapLen = 3.0f * displayScale;
            float nx = dx / length;
            float ny = dy / length;
            float drawn = 0.0f;
            bool drawing = true;

            while (drawn < length) {
                float segLen = drawing ? dashLen : gapLen;
                if (drawn + segLen > length) segLen = length - drawn;

                if (drawing) {
                    float sx = x1 + nx * drawn;
                    float sy = y1 + ny * drawn;
                    float ex = x1 + nx * (drawn + segLen);
                    float ey = y1 + ny * (drawn + segLen);
                    drawList->AddLine(
                        ImVec2(sx, sy), ImVec2(ex, ey),
                        IM_COL32(150, 150, 150, 200),
                        line.weight * displayScale * 0.3f);
                }

                drawn += segLen;
                drawing = !drawing;
            }
        }
    }

    // Reserve the space so ImGui knows how much area was used
    ImGui::Dummy(winSize);
}

void DrawingView::renderView(const DrawingViewData& view) {
    // Individual view rendering is handled inside renderSheet()
    (void)view;
}

void DrawingView::render() {
    ImGui::Begin("2D Drawing");

    if (!m_document) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", materializr::tr("No document loaded."));
        ImGui::End();
        return;
    }

    // Toolbar
    if (ImGui::Button(materializr::tr("Generate Views"))) {
        generateViews();
    }
    ImGui::SameLine();
    if (ImGui::Button(materializr::tr("Export DXF"))) {
        exportDXF("drawing.dxf");
    }
    ImGui::SameLine();
    if (ImGui::Button(materializr::tr("Export SVG"))) {
        exportSVG("drawing.svg");
    }

    ImGui::Separator();

    // View count info
    char info[128];
    std::snprintf(info, sizeof(info), "Views: %d | Sheet: %s x %s", static_cast<int>(m_views.size()), materializr::fmtLength(m_sheetWidth).c_str(), materializr::fmtLength(m_sheetHeight).c_str());
    ImGui::Text("%s", info);

    ImGui::Separator();

    // Draw the sheet with all projected views
    renderSheet();

    ImGui::End();
}

bool DrawingView::exportDXF(const std::string& filePath) {
    std::ofstream ofs(filePath, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) return false;

    // DXF header
    ofs << "0\n";
    ofs << "SECTION\n";
    ofs << "2\n";
    ofs << "ENTITIES\n";

    for (const auto& view : m_views) {
        // Visible lines
        for (const auto& line : view.visibleLines) {
            float x1 = view.position.x + line.start.x * view.scale;
            float y1 = view.position.y + line.start.y * view.scale;
            float x2 = view.position.x + line.end.x * view.scale;
            float y2 = view.position.y + line.end.y * view.scale;

            ofs << "0\n";
            ofs << "LINE\n";
            ofs << "8\n";       // Layer group code
            ofs << view.name << "_visible\n";
            ofs << "10\n";      // Start X
            ofs << x1 << "\n";
            ofs << "20\n";      // Start Y
            ofs << y1 << "\n";
            ofs << "11\n";      // End X
            ofs << x2 << "\n";
            ofs << "21\n";      // End Y
            ofs << y2 << "\n";
        }

        // Hidden lines
        for (const auto& line : view.hiddenLines) {
            float x1 = view.position.x + line.start.x * view.scale;
            float y1 = view.position.y + line.start.y * view.scale;
            float x2 = view.position.x + line.end.x * view.scale;
            float y2 = view.position.y + line.end.y * view.scale;

            ofs << "0\n";
            ofs << "LINE\n";
            ofs << "8\n";
            ofs << view.name << "_hidden\n";
            ofs << "6\n";       // Linetype name
            ofs << "DASHED\n";
            ofs << "10\n";
            ofs << x1 << "\n";
            ofs << "20\n";
            ofs << y1 << "\n";
            ofs << "11\n";
            ofs << x2 << "\n";
            ofs << "21\n";
            ofs << y2 << "\n";
        }
    }

    ofs << "0\n";
    ofs << "ENDSEC\n";
    ofs << "0\n";
    ofs << "EOF\n";

    return ofs.good();
}

bool DrawingView::exportSVG(const std::string& filePath) {
    std::ofstream ofs(filePath, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) return false;

    char buf[256];

    ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    std::snprintf(buf, sizeof(buf),
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.1f\" height=\"%.1f\" "
        "viewBox=\"0 0 %.1f %.1f\">\n",
        m_sheetWidth, m_sheetHeight, m_sheetWidth, m_sheetHeight);
    ofs << buf;

    // White background
    std::snprintf(buf, sizeof(buf),
        "  <rect fill=\"white\" width=\"%.1f\" height=\"%.1f\"/>\n",
        m_sheetWidth, m_sheetHeight);
    ofs << buf;

    // Sheet border
    std::snprintf(buf, sizeof(buf),
        "  <rect x=\"0\" y=\"0\" width=\"%.1f\" height=\"%.1f\" "
        "fill=\"none\" stroke=\"black\" stroke-width=\"0.5\"/>\n",
        m_sheetWidth, m_sheetHeight);
    ofs << buf;

    for (const auto& view : m_views) {
        // View label
        std::snprintf(buf, sizeof(buf),
            "  <text x=\"%.2f\" y=\"%.2f\" font-size=\"4\" fill=\"gray\">%s</text>\n",
            view.position.x - 10.0f,
            view.position.y - 15.0f,
            view.name.c_str());
        ofs << buf;

        // Visible lines (black, solid)
        for (const auto& line : view.visibleLines) {
            float x1 = view.position.x + line.start.x * view.scale;
            // SVG Y is inverted compared to our coordinate system
            float y1 = view.position.y - line.start.y * view.scale;
            float x2 = view.position.x + line.end.x * view.scale;
            float y2 = view.position.y - line.end.y * view.scale;

            std::snprintf(buf, sizeof(buf),
                "  <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" "
                "stroke=\"black\" stroke-width=\"%.2f\"/>\n",
                x1, y1, x2, y2, line.weight * 0.35f);
            ofs << buf;
        }

        // Hidden lines (gray, dashed)
        for (const auto& line : view.hiddenLines) {
            float x1 = view.position.x + line.start.x * view.scale;
            float y1 = view.position.y - line.start.y * view.scale;
            float x2 = view.position.x + line.end.x * view.scale;
            float y2 = view.position.y - line.end.y * view.scale;

            std::snprintf(buf, sizeof(buf),
                "  <line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" "
                "stroke=\"gray\" stroke-width=\"%.2f\" stroke-dasharray=\"2,1.5\"/>\n",
                x1, y1, x2, y2, line.weight * 0.25f);
            ofs << buf;
        }
    }

    ofs << "</svg>\n";

    return ofs.good();
}

} // namespace materializr
