#include "../gl_common.h"

#include "LandingPage.h"
#include "IconsIconoir.h"
#include "UiTheme.h"
#include "../ui_scale.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include "../i18n.h"
#include "../i18n.h"

#ifndef MATERIALIZR_VERSION
#define MATERIALIZR_VERSION "0.0.0"
#endif

namespace materializr {

LandingPage::~LandingPage() { destroyTextures(); }

void LandingPage::destroyTextures() {
    for (Entry& e : m_entries) {
        if (e.tex) {
            GLuint t = e.tex;
            glDeleteTextures(1, &t);
            e.tex = 0;
        }
    }
}

void LandingPage::setVisible(bool vis) {
    if (vis && !m_visible) m_takeFocus = true;
    m_visible = vis;
}

void LandingPage::setEntries(std::vector<Entry> entries) {
    destroyTextures();
    m_entries = std::move(entries);
}

void LandingPage::setEntryTexture(const std::string& ref, unsigned int tex) {
    if (!tex) return;
    for (auto& e : m_entries) {
        if (e.ref != ref) continue;
        // A stale-cache preview may already be showing; the freshly peeked
        // one replaces it.
        if (e.tex) {
            GLuint old = e.tex;
            glDeleteTextures(1, &old);
        }
        e.tex = tex;
        return;
    }
    // The tile list changed under a peek that was already in flight - its
    // texture has no home, so don't leak it.
    GLuint orphan = tex;
    glDeleteTextures(1, &orphan);
}

namespace {

// Middle-of-nowhere ellipsis helper: trim `s` so it fits in maxW, appending
// "…". Names are short; a linear back-off is plenty.
std::string fitLabel(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string t = s;
    while (t.size() > 1 &&
           ImGui::CalcTextSize((t + "\xe2\x80\xa6").c_str()).x > maxW)
        t.pop_back();
    return t + "\xe2\x80\xa6";
}

// What a tile reported this frame: a plain click, or a context-menu pick.
enum class TileAct { None, Click, CtxOpen, CtxStep, CtxStl, CtxParts };

// One tile: label centred above a square preview area. `tex` = 0 draws the
// placeholder glyph instead of an image. `withContext` adds the right-click
// menu (recent-project tiles only - not New Project).
TileAct tile(const char* id, const std::string& label, unsigned int tex,
             const char* placeholderGlyph, float tileW, float previewH,
             const char* tooltip, bool withContext) {
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float labelH = ImGui::GetTextLineHeight();
    const float pad = uiW(8.0f);
    const ImVec2 size(tileW, labelH + pad * 3.0f + previewH);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();

    TileAct act = ImGui::InvisibleButton("tile", size) ? TileAct::Click
                                                       : TileAct::None;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && tooltip && *tooltip) ImGui::SetTooltip("%s", tooltip);
    if (withContext && ImGui::BeginPopupContextItem("tilectx")) {
        if (ImGui::MenuItem(materializr::tr("Open"))) act = TileAct::CtxOpen;
        ImGui::Separator();
        // Cross-project parts: pick bodies/sketches out of this project into
        // a fresh workspace. Baked copies - no cross-file parametrics.
        if (ImGui::MenuItem(materializr::tr("Import Parts..."))) act = TileAct::CtxParts;
        ImGui::Separator();
        // The cheap, deliberate export model: the file's baked final bodies,
        // no cross-file parametrics (see Application::exportRecentProjectAs).
        if (ImGui::MenuItem(materializr::tr("Export as STEP..."))) act = TileAct::CtxStep;
        if (ImGui::MenuItem(materializr::tr("Export as STL..."))) act = TileAct::CtxStl;
        ImGui::EndPopup();
    }

    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const float rounding = uiW(6.0f);
    ImU32 bg = ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered
                                          : ImGuiCol_FrameBg);
    dl->AddRectFilled(p0, p1, bg, rounding);
    dl->AddRect(p0, p1,
                ImGui::GetColorU32(hovered ? ImGuiCol_ButtonActive
                                           : ImGuiCol_Border),
                rounding, 0, hovered ? 2.0f : 1.0f);

    // Name above the preview.
    const std::string shown = fitLabel(label, size.x - pad * 2.0f);
    const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
    dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + pad),
                ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());

    // Preview: the embedded thumbnail, or a big centred glyph.
    const ImVec2 i0(p0.x + pad, p0.y + pad * 2.0f + labelH);
    const ImVec2 i1(i0.x + (tileW - pad * 2.0f), i0.y + previewH);
    if (tex) {
        dl->AddImageRounded(static_cast<ImTextureID>(static_cast<intptr_t>(tex)),
                            i0, i1, ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, rounding * 0.5f);
    } else {
        dl->AddRectFilled(i0, i1, ImGui::GetColorU32(ImGuiCol_ChildBg),
                          rounding * 0.5f);
        ImFont* font = ImGui::GetFont();
        const float glyphSize = previewH * 0.42f;
        const ImVec2 gs = font->CalcTextSizeA(glyphSize, FLT_MAX, 0.0f,
                                              placeholderGlyph);
        dl->AddText(font, glyphSize,
                    ImVec2(i0.x + (i1.x - i0.x - gs.x) * 0.5f,
                           i0.y + (i1.y - i0.y - gs.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    placeholderGlyph);
    }
    (void)style;
    ImGui::PopID();
    return act;
}

} // namespace

LandingPage::Action LandingPage::render() {
    Action action;
    if (!m_visible) return action;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
    if (m_takeFocus) {
        // One-shot: lift the page above the docked panels it covers. Not
        // every frame - that would fight the menu-bar popups and the modal
        // dialogs that must sit on top.
        ImGui::SetNextWindowFocus();
        m_takeFocus = false;
    }
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(uiW(24.0f), uiW(18.0f)));
    if (!ImGui::Begin("##LandingPage", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return action;
    }
    ImGui::PopStyleVar();

    // ── Header: logo + app identity left, actions right ──────────────────
    if (m_logoTex) {
        const float logoSz = ImGui::GetTextLineHeight() * 1.5f;
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(m_logoTex)),
                     ImVec2(logoSz, logoSz));
        ImGui::SameLine();
    }
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextColored(accentText(), "%s", materializr::tr("Materializr"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    ImGui::TextColored(dimText(), "v" MATERIALIZR_VERSION);

    {
        const char* openLbl = ICON_IC_FOLDER "  Open File...";
        const char* helpLbl = ICON_IC_HELP_CIRCLE "  Help";
        const char* gearLbl = ICON_IC_SETTINGS "  Settings";
        const char* backLbl = ICON_IC_XMARK;
        const ImGuiStyle& style = ImGui::GetStyle();
        int nBtns = m_canDismiss ? 4 : 3;
        float rightW = ImGui::CalcTextSize(openLbl).x +
                       ImGui::CalcTextSize(helpLbl).x +
                       ImGui::CalcTextSize(gearLbl).x +
                       (m_canDismiss ? ImGui::CalcTextSize(backLbl).x : 0.0f) +
                       style.FramePadding.x * 2.0f * nBtns +
                       style.ItemSpacing.x * (nBtns - 1);
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - rightW);
        if (ImGui::Button(openLbl)) action.type = ActionType::OpenFileDialog;
        ImGui::SameLine();
        if (ImGui::Button(helpLbl)) action.type = ActionType::OpenHelp;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", materializr::tr("User guide"));
        ImGui::SameLine();
        if (ImGui::Button(gearLbl)) action.type = ActionType::OpenSettings;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", materializr::tr("Settings"));
        if (m_canDismiss) {
            ImGui::SameLine();
            if (ImGui::Button(backLbl)) action.type = ActionType::Dismiss;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", materializr::tr("Back to the open project"));
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── The grid ─────────────────────────────────────────────────────────
    ImGui::BeginChild("##LandingGrid", ImVec2(0, 0), false);
    const float tileW = uiW(190.0f);
    const float previewH = tileW - uiW(16.0f);   // square-ish preview
    const float cellW = tileW + ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, static_cast<int>(
                            ImGui::GetContentRegionAvail().x / cellW));

    int col = 0;
    auto nextCell = [&]() {
        if (++col < columns) ImGui::SameLine();
        else col = 0;
    };

    if (tile("new", "New Project", 0, ICON_IC_PLUS, tileW, previewH,
             "Start an empty project", false) == TileAct::Click)
        action.type = ActionType::NewProject;
    nextCell();

    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        ImGui::PushID(static_cast<int>(i));
        const TileAct ta = tile("recent", e.name, e.tex, ICON_IC_CUBE, tileW,
                                previewH, e.ref.c_str(), true);
        if (ta != TileAct::None) {
            action.ref = e.ref;
            action.name = e.name;
            switch (ta) {
            case TileAct::Click:
            case TileAct::CtxOpen:  action.type = ActionType::OpenEntry; break;
            case TileAct::CtxStep:  action.type = ActionType::ExportStep; break;
            case TileAct::CtxStl:   action.type = ActionType::ExportStl; break;
            case TileAct::CtxParts: action.type = ActionType::ImportParts; break;
            case TileAct::None:     break;
            }
        }
        ImGui::PopID();
        nextCell();
    }

    if (m_entries.empty()) {
        if (col != 0) ImGui::NewLine();
        ImGui::Spacing();
        ImGui::TextColored(dimText(), "%s", materializr::tr("Projects you open or save appear here."));
    }
    ImGui::EndChild();

    ImGui::End();
    return action;
}

} // namespace materializr
