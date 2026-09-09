#include "UiTheme.h"
#include "ui_scale.h"
#include "WelcomeScreen.h"
#include "ios_storekit.h"
#include "url_open.h"
#include <imgui.h>

#include <string>
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

#ifndef MATERIALIZR_VERSION
#define MATERIALIZR_VERSION "0.0.0"
#endif

namespace materializr {

WelcomeScreen::Action WelcomeScreen::render() {
    if (!m_visible) return Action::None;

    Action action = Action::None;

    // Guarded reopen: a raw OpenPopup-every-frame stomps any other popup at
    // the same stack level (see the Application render site: Welcome also
    // yields to the startup modals for the same reason).
    if (!ImGui::IsPopupOpen("Welcome")) ImGui::OpenPopup("Welcome");

    // Cond_Always, not Appearing: on iOS the window can first appear on a
    // frame with a degenerate viewport (splash → UI handoff), and an
    // Appearing-only position lands wrong ONCE and NoMove pins it there -
    // a 45px sliver at ImGui's (60,60) cascade default while the modal dim
    // blocks all input (the "second launch locks up" report). Re-asserting
    // the centre every frame makes a bad first frame self-heal.
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(uiSz(440, 0).x, 0.0f), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Welcome", &m_visible,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize)) {

        // App name - slightly larger via font scaling, same as the About dialog.
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

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + uiSz(400, 0).x);
        ImGui::TextWrapped("%s", materializr::tr("Materializr is free and open source, and always will be. If it has earned a place in your workflow, please consider supporting development - it keeps the project moving."));
        ImGui::PopTextWrapPos();

        ImGui::Spacing();

        float btnW = uiSz(220, 0).x;

#if defined(MZ_IOS)
        // App Store build: the ask goes through StoreKit (Apple guideline 3.1.1
        // forbids external payment links for developer-directed donations). One
        // non-consumable Supporter product; Application's main loop persists the
        // flag when iosStoreConsumeEntitled() fires.
        const bool working = iosStorePhase() == TipPhase::Working;
        ImGui::BeginDisabled(working);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.00f, 0.87f, 0.00f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.92f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.85f, 0.74f, 0.00f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
        if (ImGui::Button(working ? "Contacting the App Store..." : "Support Materializr",
                          ImVec2(btnW, 0))) {
            iosStoreBuySupporter();
        }
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();

        // Status / error line from the last StoreKit attempt.
        std::string storeMsg = iosStoreMessage();
        if (!storeMsg.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + uiSz(400, 0).x);
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", storeMsg.c_str());
            ImGui::PopTextWrapPos();
        }
#else
        // Desktop (and, until Play Billing is wired, Android side-loads): Buy Me
        // a Coffee, brand-yellow like the About dialog's copy of this button.
        // Google Play submissions must swap this for Play Billing first.
        const char* bmcUrl = "https://www.buymeacoffee.com/stevebushwa";
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.00f, 0.87f, 0.00f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.92f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.85f, 0.74f, 0.00f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnW) * 0.5f);
        if (ImGui::Button(materializr::tr("Support us - Buy us a Coffee"), ImVec2(btnW, 0))) {
            materializr::openUrl(bmcUrl);
        }
        ImGui::PopStyleColor(4);
#endif

        ImGui::Spacing();

        float contW = uiSz(120, 0).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - contW) * 0.5f);
        if (ImGui::Button(materializr::tr("Continue"), ImVec2(contW, 0))) {
            m_visible = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::Spacing();

#if defined(MZ_IOS)
        // Restore path Apple requires for non-consumables: re-delivers the
        // Supporter purchase after a reinstall or on a new device.
        const char* restore = "Already a supporter? Restore purchase";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(restore).x) * 0.5f);
        ImGui::TextDisabled("%s", restore);
        if (ImGui::IsItemClicked()) iosStoreRestore();
#else
        // Honor-system Supporter switch: silences the prompt permanently.
        const char* already = "I already support - don't show this again";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(already).x) * 0.5f);
        ImGui::TextDisabled("%s", already);
        if (ImGui::IsItemClicked()) {
            action = Action::MarkSupporter;
            m_visible = false;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
#endif

        ImGui::EndPopup();
    }

    return action;
}

void WelcomeScreen::setVisible(bool vis) { m_visible = vis; }

bool WelcomeScreen::isVisible() const { return m_visible; }

} // namespace materializr
