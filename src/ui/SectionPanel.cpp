#include "ui/LengthField.h"
#include "SectionPanel.h"
#include "../viewport/SectionView.h"

#include <imgui.h>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

SectionPanel::SectionPanel() = default;

void SectionPanel::setSectionView(SectionView* sv) {
    m_sectionView = sv;
}

bool SectionPanel::render() {
    if (!m_sectionView) return false;

    bool needsUpdate = false;

    ImGui::Begin("Section View");

    // Enable/disable toggle
    bool enabled = m_sectionView->isEnabled();
    if (ImGui::Checkbox(materializr::tr("Enable Section"), &enabled)) {
        m_sectionView->setEnabled(enabled);
        needsUpdate = true;
    }

    ImGui::Separator();

    // Plane axis selection
    const char* axisItems[] = { "YZ (X normal)", "XZ (Y normal)", "XY (Z normal)" };
    if (ImGui::Combo(materializr::tr("Cutting Plane"), &m_planeAxis, axisItems, 3)) {
        gp_Pnt origin(0, 0, 0);
        gp_Pln plane;
        switch (m_planeAxis) {
            case 0: plane = gp_Pln(origin, gp_Dir(1, 0, 0)); break; // YZ plane, X normal
            case 1: plane = gp_Pln(origin, gp_Dir(0, 1, 0)); break; // XZ plane, Y normal
            case 2: plane = gp_Pln(origin, gp_Dir(0, 0, 1)); break; // XY plane, Z normal
        }
        m_sectionView->setPlane(plane);
        needsUpdate = true;
    }

    // Offset slider
    // The label carries the unit, like the equivalent control in
    // Application_Dialogs. The value converted correctly; only the label
    // failed to say which unit the number was in.
    if (materializr::lengthSlider(
            materializr::trFormat("Offset (%s)", materializr::unitSuffix()).c_str(),
            &m_offset, -100.0f, 100.0f)) {
        m_sectionView->setOffset(m_offset);
        needsUpdate = true;
    }

    // Reset offset button
    if (ImGui::Button(materializr::tr("Reset Offset"))) {
        m_offset = 0.0f;
        m_sectionView->setOffset(m_offset);
        needsUpdate = true;
    }

    ImGui::End();

    return needsUpdate;
}

} // namespace materializr
