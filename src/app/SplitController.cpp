#include "ui/LengthField.h"
#include "SplitController.h"

#include "../core/Document.h"
#include "../core/SelectionManager.h"
#include "../modeling/SplitBodyOp.h"
#include "../ui/StepperRow.h"
#include "../ui/TouchWidgets.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../i18n.h"

namespace materializr {

namespace {

// The ghost plane's colours, packed 0xAABBGGRR. A translucent fill so the body
// still reads through it, with a solid outline — the fill alone disappears
// against a light face, and the outline alone reads as a stray rectangle.
//
// AMBER, deliberately not blue: a body is selected whenever this tool is open,
// and the selection highlight is blue. Rig-checked with a blue plane first and
// the two were genuinely hard to tell apart on the body they overlap.
constexpr unsigned kFill    = 0x4030A8FF;
constexpr unsigned kOutline = 0xF030A8FF;

// How far past the body the ghost quad extends, so its edges are visibly clear
// of the silhouette rather than coinciding with it.
constexpr float kOverhang = 1.18f;

const char* axisName(int a) { return a == 0 ? "X" : a == 1 ? "Y" : "Z"; }

// What the two halves are called, in the axis labels the user thinks in.
const char* axisSplitDescription(int a) {
    return a == 0 ? "left / right"
         : a == 1 ? "front / back"
                  : "top / bottom";
}

} // namespace

gp_Dir SplitController::worldNormal(int userAxis) {
    // Axis labels follow user / 3D-printer convention (X = left/right,
    // Y = forward/back, Z = up), and Materializr's world is Y-up: user-Y is
    // world Z and user-Z is world Y. Same mapping the three Split plugin
    // buttons carried before this controller replaced them — getting it wrong
    // silently cuts the body along the wrong axis, which still "works".
    switch (userAxis) {
        case 0:  return gp_Dir(1, 0, 0);
        case 1:  return gp_Dir(0, 0, 1);
        default: return gp_Dir(0, 1, 0);
    }
}

int SplitController::onBegin(const IopContext& ctx) {
    m_axis = 0;
    m_offset = 0.0f;
    m_haveBox = false;
    m_centre = glm::vec3(0.0f);
    m_half = glm::vec3(1.0f);

    int body = -1;
    for (const auto& e : ctx.selection.getSelection())
        if (e.bodyId >= 0) { body = e.bodyId; break; }
    if (body < 0) return -1;
    // Imported meshes decline topology edits — core/MeshGuard.h.
    if (ctx.refuseMesh && ctx.refuseMesh("Split")) return -1;

    try {
        const TopoDS_Shape shape = ctx.doc.getBody(body);
        if (shape.IsNull()) return -1;
        Bnd_Box bb;
        BRepBndLib::Add(shape, bb);
        if (!bb.IsVoid()) {
            double x0, y0, z0, x1, y1, z1;
            bb.Get(x0, y0, z0, x1, y1, z1);
            m_centre = glm::vec3(float(x0 + x1) * 0.5f, float(y0 + y1) * 0.5f,
                                 float(z0 + z1) * 0.5f);
            m_half = glm::vec3(float(x1 - x0) * 0.5f, float(y1 - y0) * 0.5f,
                               float(z1 - z0) * 0.5f);
            m_haveBox = true;
        }
    } catch (...) { return -1; }
    // Without a box there is no centre to anchor the plane to, and a plane
    // through the world origin misses any body that isn't sitting on it.
    if (!m_haveBox) return -1;
    return body;
}

float SplitController::axisHalf() const {
    const gp_Dir n = worldNormal(m_axis);
    return std::abs(float(n.X()) * m_half.x) + std::abs(float(n.Y()) * m_half.y) +
           std::abs(float(n.Z()) * m_half.z);
}

glm::vec3 SplitController::planeCentre() const {
    const gp_Dir n = worldNormal(m_axis);
    return m_centre + glm::vec3(float(n.X()), float(n.Y()), float(n.Z())) * m_offset;
}

bool SplitController::planeCorners(glm::vec3 out[4]) const {
    if (!m_haveBox) return false;
    const gp_Dir n = worldNormal(m_axis);
    const glm::vec3 nv(float(n.X()), float(n.Y()), float(n.Z()));
    // The two world axes the plane spans are simply the ones the normal isn't.
    glm::vec3 u(0.0f), v(0.0f);
    if (std::abs(nv.x) > 0.5f)      { u = glm::vec3(0, 1, 0); v = glm::vec3(0, 0, 1); }
    else if (std::abs(nv.y) > 0.5f) { u = glm::vec3(1, 0, 0); v = glm::vec3(0, 0, 1); }
    else                            { u = glm::vec3(1, 0, 0); v = glm::vec3(0, 1, 0); }

    const float eu = (std::abs(u.x) * m_half.x + std::abs(u.y) * m_half.y +
                      std::abs(u.z) * m_half.z) * kOverhang;
    const float ev = (std::abs(v.x) * m_half.x + std::abs(v.y) * m_half.y +
                      std::abs(v.z) * m_half.z) * kOverhang;
    const glm::vec3 c = planeCentre();
    out[0] = c - u * eu - v * ev;
    out[1] = c + u * eu - v * ev;
    out[2] = c + u * eu + v * ev;
    out[3] = c - u * eu + v * ev;
    return true;
}

std::unique_ptr<Operation> SplitController::buildOp(const IopContext&) {
    const glm::vec3 c = planeCentre();
    auto op = std::make_unique<SplitBodyOp>();
    op->setBody(bodyId());
    op->setSplitPlane(gp_Pln(gp_Pnt(c.x, c.y, c.z), worldNormal(m_axis)));
    return op;
}

void SplitController::panelBody(const IopContext& ctx, bool& changed) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f);
    ImGui::TextDisabled("%s", materializr::tr("Cut the body in two with a plane. The ghost rectangle in the viewport is where the cut lands."));
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    ImGui::TextDisabled("%s", materializr::tr("Axis"));
    for (int a = 0; a < 3; ++a) {
        if (a) ImGui::SameLine();
        if (ImGui::RadioButton(axisName(a), m_axis == a) && m_axis != a) {
            m_axis = a;
            // The offset is a distance along the OLD axis; carrying it over
            // would drop the plane somewhere the user never asked for, and on
            // a flat part it can land outside the body entirely.
            m_offset = 0.0f;
            changed = true;
        }
    }
    ImGui::TextDisabled(materializr::tr("%s halves"), axisSplitDescription(m_axis));

    ImGui::Separator();
    const float half = axisHalf();
    // Stop just short of the ends: a plane exactly on the face touches without
    // cutting, and SplitBodyOp then hands back the body unchanged with nothing
    // to show for the step.
    const float lim = std::max(half * 0.98f, 0.0f);
    ImGui::TextDisabled("%s", materializr::trFormat("Offset from centre: %s", materializr::fmtLength(m_offset)).c_str());
    if (materializr::lengthStepperRow("splitOffset", &m_offset, /*allowNegative=*/true,
                                -lim, lim))
        changed = true;
    if (ctx.cornerCommitUi &&
        materializr::amountLengthField("splitOffsetAmt", nullptr, &m_offset, /*allowSign=*/true, -lim, lim))
        changed = true;
    m_offset = std::min(lim, std::max(-lim, m_offset));

    ImGui::TextDisabled("%s", materializr::trFormat("Body spans %s on %s.", materializr::fmtLength(half * 2.0f), axisName(m_axis)).c_str());
}

void SplitController::drawOverlay(const IopOverlay& ov) const {
    if (!active() || !ov.toScreen) return;
    glm::vec3 w[4];
    if (!planeCorners(w)) return;

    glm::vec2 s[4];
    for (int i = 0; i < 4; ++i)
        if (!ov.toScreen(w[i], s[i])) return;   // a corner behind the camera

    if (ov.triangle) {
        ov.triangle(s[0], s[1], s[2], kFill);
        ov.triangle(s[0], s[2], s[3], kFill);
    }
    if (ov.line)
        for (int i = 0; i < 4; ++i)
            ov.line(s[i], s[(i + 1) % 4], kOutline, 2.0f);
}

void SplitController::onCleanup() {
    m_haveBox = false;
    m_offset = 0.0f;
}

} // namespace materializr
