#include "UiTheme.h"
#include "ui_scale.h"
#include "AboutDialog.h"
#include "url_open.h"
#include <imgui.h>

#include <cstring>
#include <string>
#include "../i18n.h"
#include "../i18n.h"

#ifndef MATERIALIZR_VERSION
#define MATERIALIZR_VERSION "0.0.0"
#endif

namespace materializr {

namespace {

// Open a URL in the user's default browser via the shared shell-free helper
// (see url_open.h). The About links are hardcoded https constants; openUrl
// still validates the scheme and uses SDL_OpenURL rather than a shell.
void openInBrowser(const char* url) {
    materializr::openUrl(url);
}

} // namespace

AboutDialog::AboutDialog() = default;

void AboutDialog::setVisible(bool vis) {
    m_visible = vis;
}

bool AboutDialog::isVisible() const {
    return m_visible;
}

void AboutDialog::render() {
    if (!m_visible) return;

    ImGui::OpenPopup("About Materializr");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(uiSz(420, 400), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("About Materializr", &m_visible,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {

        // App name - slightly larger via font scaling.
        float origScale = ImGui::GetFont()->Scale;
        ImGui::GetFont()->Scale = 2.0f;
        ImGui::PushFont(ImGui::GetFont());
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("Materializr").x) * 0.5f);
        ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Materializr"));
        ImGui::GetFont()->Scale = origScale;
        ImGui::PopFont();

        std::string verLine = std::string("Version ") + MATERIALIZR_VERSION;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(verLine.c_str()).x) * 0.5f);
        ImGui::Text("%s", verLine.c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const char* desc = "Open-source parametric 3D CAD";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(desc).x) * 0.5f);
        ImGui::Text("%s", desc);

        ImGui::Spacing();
        ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Credits"));
        ImGui::BulletText("%s", materializr::tr("R4stl1n - original project"));
        ImGui::BulletText("%s", materializr::tr("stevebushwa - design, testing, direction"));
        ImGui::BulletText("%s", materializr::tr("Claude (Anthropic) - pair-coding collaborator"));

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", materializr::tr("Built with OpenCASCADE, Dear ImGui, SDL2, GLM, libcurl."));
        // GPLv3 since 0.9.8 (the old "MIT" line survived the relicense);
        // section-7 additional permissions live in LICENSE-EXCEPTIONS.md.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::TextWrapped("%s", materializr::tr("License: GNU GPLv3, with additional permissions"));
        ImGui::TextWrapped("%s", materializr::tr("(see LICENSE and LICENSE-EXCEPTIONS.md in the source repository)"));
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const char* repoUrl = "https://github.com/materializr-cad/materializr";
        float btnW = 200.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
        if (ImGui::Button(materializr::tr("Open Project on GitHub"), ImVec2(btnW, 0))) {
            openInBrowser(repoUrl);
        }
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(repoUrl).x) * 0.5f);
        ImGui::TextDisabled("%s", repoUrl);

        ImGui::Spacing();

        // Community Discord - Discord "blurple" so it reads as its own action,
        // the same way the coffee button is branded yellow.
        const char* discordUrl = "https://discord.gg/BRjzbMGZvE";
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.345f, 0.396f, 0.949f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.447f, 0.498f, 1.000f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.275f, 0.318f, 0.796f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
        if (ImGui::Button(materializr::tr("Join our Discord"), ImVec2(btnW, 0))) {
            openInBrowser(discordUrl);
        }
        ImGui::PopStyleColor(4);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(discordUrl).x) * 0.5f);
        ImGui::TextDisabled("%s", discordUrl);

        ImGui::Spacing();

        // Buy Me a Coffee - proceeds split between stevebushwa and R4stl1n
        // (stevebushwa just runs the page). Coloured in the BMC brand yellow
        // so it reads as a separate "support" action rather than another
        // navigation button.
        const char* bmcUrl = "https://www.buymeacoffee.com/stevebushwa";
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.00f, 0.87f, 0.00f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.92f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.85f, 0.74f, 0.00f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
        if (ImGui::Button(materializr::tr("Buy us a Coffee"), ImVec2(btnW, 0))) {
            openInBrowser(bmcUrl);
        }
        ImGui::PopStyleColor(4);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(bmcUrl).x) * 0.5f);
        ImGui::TextDisabled("%s", bmcUrl);

        ImGui::Spacing();
        float closeW = 100.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - closeW) * 0.5f);
        if (ImGui::Button(materializr::tr("Close"), ImVec2(closeW, 0))) {
            m_visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace materializr
