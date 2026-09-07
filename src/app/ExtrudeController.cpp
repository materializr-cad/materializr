#include "ui/LengthField.h"
#include "ExtrudeController.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/Operation.h"
#include "../core/SelectionManager.h"
#include "../modeling/CutTargetPick.h"
#include "../core/NumParse.h"
#include "../ui/UiTheme.h"       // viewportBanner
#include "../ui/NumField.h"      // btnConfirm / btnCancel
#include "../ui/StepperRow.h"
#include "../ui/TouchWidgets.h"  // im-touch number-pad amount field
#include "../ui/TouchIcons.h"    // MZ_ICON_BODY — the all-bodies pill
#include "../ui/OpDialogGrip.h"
#include "../touch_mode.h"
#include <imgui.h>
#include <BRep_Tool.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp_Face.hxx>
#include <Bnd_Box.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

double ExtrudeController::opDistance() const {
    return m_sweepSign * static_cast<double>(m_distance);
}

// Every visible, non-mesh body except the preview's own tool volume. Imported
// meshes decline modelling ops elsewhere and must not become a cut target here
// either.
static std::vector<std::pair<int, TopoDS_Shape>> cutCandidates(
        const IopContext& ctx, int excludeBody) {
    std::vector<std::pair<int, TopoDS_Shape>> out;
    for (int id : ctx.doc.getAllBodyIds()) {
        if (id == excludeBody) continue;
        if (!ctx.doc.isBodyVisible(id) || ctx.doc.isBodyMesh(id)) continue;
        try {
            const TopoDS_Shape& s = ctx.doc.getBody(id);
            if (!s.IsNull()) out.push_back({id, s});
        } catch (...) {}
    }
    return out;
}

int ExtrudeController::resolveCutTarget(const IopContext& ctx) const {
    // The live preview IS the tool volume — the exact solid the user is
    // watching — so ask which bodies it overlaps rather than re-deriving it.
    const int previewId = previewBodyId();
    if (previewId < 0) {
        std::fprintf(stderr, "[Subtract] no tool volume to cut with "
                             "(preview not applied)\n");
        return -1;
    }
    TopoDS_Shape tool;
    try { tool = ctx.doc.getBody(previewId); } catch (...) { return -1; }
    if (tool.IsNull()) return -1;
    const auto cands = cutCandidates(ctx, previewId);
    const int hit = cutpick::pickCutTarget(cands, tool, m_targetBody);
    if (hit < 0) {
        // Say what was actually measured — "nothing to cut" is a claim about
        // geometry, and a wrong one is invisible without the numbers (this
        // dump is what turned "but it CLEARLY overlaps" into "the tool spans
        // x -18..-6 and every body starts at 0"). Re-running the booleans to
        // report them is fine: only the refusal path gets here, and the user
        // is already stopped.
        std::fprintf(stderr, "[Subtract] tool body %d (dist %.3f) reaches no "
                     "body; preferred=%d, checked %zu:\n",
                     previewId, opDistance(), m_targetBody, cands.size());
        auto bbox = [](const TopoDS_Shape& s, double* v) {
            Bnd_Box b; BRepBndLib::Add(s, b);
            if (b.IsVoid()) { for (int i = 0; i < 6; ++i) v[i] = 0; return; }
            b.Get(v[0], v[1], v[2], v[3], v[4], v[5]);
        };
        double t[6]; bbox(tool, t);
        std::fprintf(stderr, "  tool bbox [%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n",
                     t[0], t[1], t[2], t[3], t[4], t[5]);
        for (const auto& [id, shape] : cands) {
            double b[6]; bbox(shape, b);
            std::fprintf(stderr, "  body %d overlap %.6g  bbox [%.2f %.2f %.2f]"
                         "..[%.2f %.2f %.2f]\n", id,
                         cutpick::removedVolume(shape, tool),
                         b[0], b[1], b[2], b[3], b[4], b[5]);
        }
    }
    return hit;
}

bool ExtrudeController::beginExtrude(const IopContext& ctx,
                                     const TopoDS_Shape& profile,
                                     ExtrudeMode mode, int targetBody,
                                     int sourceSketchId) {
    // Extrude sweeps a profile along its normal — only meaningful for a FLAT
    // profile. A single curved body face (cylinder / sphere / fillet) has no
    // single normal, so extruding it produced garbage geometry; refuse with
    // guidance instead (mirrors Sketch-on-Face). Checked BEFORE anything is
    // disturbed so a bad attempt leaves an in-progress op alone. Sketch
    // profiles are planar by construction; wire / compound profiles aren't a
    // single face and skip this check.
    if (profile.ShapeType() == TopAbs_FACE) {
        Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(profile));
        if (s.IsNull() || !s->IsKind(STANDARD_TYPE(Geom_Plane))) {
            if (ctx.toast)
                ctx.toast("Can't extrude a curved face \xE2\x80\x94 extrude "
                          "works on flat faces only.");
            return false;
        }
    }
    m_profile = profile;
    m_mode = mode;
    m_targetBody = targetBody;
    m_sketchId = sourceSketchId;
    return begin(ctx);
}

int ExtrudeController::onBegin(const IopContext& ctx) {
    m_distance = 5.0f;
    materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_distance);
    m_inputFocus = true;

    // Face normal and centre. A compound profile (multi-region extrude —
    // several letters at once) uses its first face: all regions of one sketch
    // are coplanar, so any face gives the right normal.
    TopoDS_Shape normShape = m_profile;
    if (m_profile.ShapeType() != TopAbs_FACE) {
        TopExp_Explorer fx(m_profile, TopAbs_FACE);
        if (fx.More()) normShape = fx.Current();
    }
    if (normShape.ShapeType() == TopAbs_FACE) {
        BRepGProp_Face prop(TopoDS::Face(normShape));
        gp_Pnt center;
        gp_Vec norm;
        double u1, u2, v1, v2;
        prop.Bounds(u1, u2, v1, v2);
        prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, center, norm);
        if (norm.Magnitude() > 1e-10)
            m_normal = glm::normalize(glm::vec3(norm.X(), norm.Y(), norm.Z()));
        m_origin = glm::vec3(center.X(), center.Y(), center.Z());
    }
    // Point the on-screen arrow INTO the material for a Subtract, so dragging
    // toward it deepens the cut. A face sketch gets that for free — its normal
    // points OUT of the host body, so the cut runs the other way. A sketch on a
    // construction or origin plane has no host and no such convention: its
    // normal points wherever the plane faces, which half the time is away from
    // every body, so aim at the nearest one instead.
    m_sweepSign = 1.0;
    if (m_mode == ExtrudeMode::Subtract) {
        m_sweepSign = -1.0;
        if (m_targetBody < 0) {
            std::vector<TopoDS_Shape> bodies;
            for (const auto& [id, shape] : cutCandidates(ctx, -1))
                bodies.push_back(shape);
            m_sweepSign = cutpick::cutSweepSign(
                gp_Pnt(m_origin.x, m_origin.y, m_origin.z),
                gp_Dir(m_normal.x, m_normal.y, m_normal.z), bodies);
        }
        m_normal *= static_cast<float>(m_sweepSign);
    }

    // Threaded target bodies are fine: the preview is always a NewBody tool
    // volume (never a per-frame boolean against the target), and the real
    // Subtract runs once at commit through History::pushOperation, which
    // reflows the cut beneath the Thread step and re-cuts in background.
    return kNoTargetBody;   // the preview body doesn't exist yet
}

std::unique_ptr<Operation> ExtrudeController::buildOp(const IopContext& ctx) {
    (void)ctx;
    // The live instance. Always NewBody — the user watches the tool volume
    // being swept; Subtract's real boolean is buildCommitOp's job.
    auto op = std::make_unique<ExtrudeOp>();
    op->setProfile(m_profile);
    op->setDistance(opDistance());
    op->setMode(ExtrudeMode::NewBody);
    op->setSketchSource(m_sketchId);
    return op;
}

bool ExtrudeController::syncLiveOp(Operation& op) {
    static_cast<ExtrudeOp&>(op).setDistance(opDistance());
    return true;
}

// Resolve the cut target from the swept volume before the base records
// anything. Without this the two ways a Subtract can quietly do nothing both
// end in a History step: no target at all (the base would record the preview,
// leaving the tool volume behind as a stray body), or a target the sweep never
// reaches (BRepAlgoAPI_Cut hands the body straight back, valid and unchanged).
// Refusing leaves the op OPEN so the distance can be pushed further or reversed.
void ExtrudeController::commit(const IopContext& ctx) {
    if (active() && m_mode == ExtrudeMode::Subtract) {
        if (m_cutAllBodies) {
            const std::vector<int> targets = resolveAllCutTargets(ctx);
            if (targets.empty()) {
                if (ctx.toast)
                    ctx.toast("Subtract: this profile doesn't reach any body \xE2\x80\x94 "
                              "nothing to cut. Extrude it further, or drag the other way.");
                return;
            }
            commitCutAll(ctx, targets);
            return;
        }
        const int target = resolveCutTarget(ctx);
        if (target < 0) {
            if (ctx.toast)
                ctx.toast("Subtract: this profile doesn't reach any body \xE2\x80\x94 "
                          "nothing to cut. Extrude it further, or drag the other way.");
            return;
        }
        m_targetBody = target;
    }
    InteractiveOpController::commit(ctx);
}

std::vector<int> ExtrudeController::resolveAllCutTargets(
        const IopContext& ctx) const {
    const int previewId = previewBodyId();
    if (previewId < 0) return {};
    TopoDS_Shape tool;
    try { tool = ctx.doc.getBody(previewId); } catch (...) { return {}; }
    if (tool.IsNull()) return {};
    return cutpick::pickAllCutTargets(cutCandidates(ctx, previewId), tool);
}

// One body, one step. The base's commit() records at most a single alternative
// op, so this takes over the whole tail of the gesture: drop the preview volume
// (it is a NewBody tool that must not survive), then push a real Subtract per
// target. Each carries the same profile and distance, so each cuts its own body
// exactly as a single-target Subtract would — including its face lineage, which
// is per-body and would have to be reinvented to pack them into one op.
void ExtrudeController::commitCutAll(const IopContext& ctx,
                                     const std::vector<int>& targets) {
    if (livePreviewApplied() && liveOp()) {
        try { liveOp()->undo(ctx.doc); } catch (...) {}
    }
    int done = 0;
    for (int id : targets) {
        auto op = std::make_unique<ExtrudeOp>();
        op->setProfile(m_profile);
        op->setDistance(opDistance());
        op->setMode(ExtrudeMode::Subtract);
        op->setTargetBody(id);
        op->setSketchSource(m_sketchId);
        if (ctx.history.pushOperation(std::move(op), ctx.doc)) ++done;
    }
    std::fprintf(stdout, "Subtracted %.1f mm from %d of %zu bodies\n",
                 std::abs(m_distance), done, targets.size());
    // A body whose cut FAILED is not a silent loss — pushOperation refuses an
    // op that can't produce a valid solid, so that body is simply unchanged,
    // and the ones that worked still landed.
    if (done < static_cast<int>(targets.size()) && ctx.toast) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "Cut %d of %zu bodies \xE2\x80\x94 the rest couldn't make a "
                      "valid solid and were left alone.",
                      done, targets.size());
        ctx.toast(msg);
    }
    ctx.selection.clear();
    ctx.markMeshesDirty();
    teardown();
}

std::unique_ptr<Operation> ExtrudeController::buildCommitOp(const IopContext& ctx) {
    (void)ctx;
    // NewBody: the previewed instance IS the result — let the base record it
    // as-is. Subtract: the preview was only a tool volume, so hand back the
    // real boolean cut against the body the sketch was drawn on.
    if (m_mode != ExtrudeMode::Subtract || m_targetBody < 0) {
        std::fprintf(stdout, "Extruded %.1f mm\n", m_distance);
        return nullptr;
    }
    auto op = std::make_unique<ExtrudeOp>();
    op->setProfile(m_profile);
    op->setDistance(opDistance());
    op->setMode(ExtrudeMode::Subtract);
    op->setTargetBody(m_targetBody);
    op->setSketchSource(m_sketchId);
    std::fprintf(stdout, "Subtracted %.1f mm from body %d\n",
                 std::abs(m_distance), m_targetBody);
    return op;
}

void ExtrudeController::updateExtrude(const IopContext& ctx, bool applySnap) {
    if (!active()) return;
    if (!std::isfinite(m_distance)) { m_distance = 0.0f; return; }
    // Snap the live distance to the corner-widget grid step before applying
    // (issue #24). Drag/commit paths snap; live typing and the steppers pass
    // applySnap=false so a typed value stays exact.
    if (applySnap && ctx.snapToGrid && ctx.gridStep > 0.0f) {
        m_distance = std::round(m_distance / ctx.gridStep) * ctx.gridStep;
        materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_distance);
    }
    update(ctx);
}

int ExtrudeController::previewBodyId() const {
    const auto* op = static_cast<const ExtrudeOp*>(liveOp());
    return (op && livePreviewApplied()) ? op->createdBodyId() : -1;
}

// Left-drag anywhere in the viewport moves the distance along the arrow's
// normal. No handle to latch — the whole viewport is the drag surface — so
// draggingHandle() stays false and the camera keeps its own claim (the
// dispatch loop skips this while the camera is dragging).
void ExtrudeController::onViewportInput(const IopViewport& vp,
                                        const IopContext& ctx) {
    if (!active()) return;
    if (vp.dragging) {
        m_distance += vp.dragAlongAxis(m_origin, m_normal, vp.mouseDelta);
        materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_distance);
        updateExtrude(ctx);
    }
    // Trackpad-mode click-move-click, same model as Push/Pull: with Left
    // now free to orbit during the op, a whole-viewport drag surface needs
    // a buttonless way to drive the value. Click (without dragging) arms
    // sticky, cursor motion feeds the distance, click again to release.
    if (vp.trackpadInput && !vp.uiCaptured) {
        if (vp.clicked) m_stickyPressWasDrag = false;
        if (vp.dragging) m_stickyPressWasDrag = true;
        if (vp.released && !m_stickyPressWasDrag) m_sticky = !m_sticky;
    }
    if (m_sticky && (vp.mouseDelta.x != 0.0f || vp.mouseDelta.y != 0.0f)) {
        m_distance += vp.dragAlongAxis(m_origin, m_normal, vp.mouseDelta);
        materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_distance);
        updateExtrude(ctx);
    }
}

void ExtrudeController::renderExtrudePanel(const IopContext& ctx) {
    if (!active()) return;
    const float s = ctx.panel.uiScale;
    const bool imTouch = ctx.panel.imTouch;

    materializr::viewportBanner(
        ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
        materializr::touchMode()
            ? "EXTRUDE - Drag in viewport or type distance, then Confirm / Cancel."
            : "EXTRUDE - Drag in viewport or type distance. Enter to confirm, Escape to cancel.");

    // Floating distance input panel. im-touch: anchored just off the extrude
    // arrow's tip, like the sketch bubbles; other layouts (or a tip behind
    // the camera) keep the fixed top-right spot.
    bool extAnchored = false;
    if (imTouch && ctx.panel.anchorValid) {
        const ImVec2 vwp = ImGui::GetWindowPos();
        const float vww = ImGui::GetWindowWidth();
        float ax = std::min(std::max(ctx.panel.anchorX + 24.0f * s, vwp.x + 8.0f),
                            vwp.x + vww - 250.0f * s);
        float ay = std::max(ctx.panel.anchorY + 12.0f * s, vwp.y + 8.0f);
        ImGui::SetNextWindowPos(ImVec2(ax, ay), ImGuiCond_Appearing);
        extAnchored = true;
    }
    if (!extAnchored)
        ImGui::SetNextWindowPos(ImVec2(
            std::max(ImGui::GetWindowPos().x + 6.0f,
                     ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 250.0f * s),
            ImGui::GetWindowPos().y + 50), ImGuiCond_Appearing);
    // Pin the width (min == max) so moving the panel can't feed back into
    // the value field's content-avail width and ratchet the window wider.
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f * s, 0.0f),
                                        ImVec2(240.0f * s, 100000.0f));
    ImGui::Begin("##ExtrudeInput", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_AlwaysAutoResize);
    opDialogDragGrip(s);

    if (!imTouch) {   // im-touch: just the value well below
        ImGui::Text("%s", materializr::trFormat("Extrude Distance (%s)", materializr::unitSuffix()).c_str());
        ImGui::Separator();
    }

    if (m_inputFocus) {
        if (!materializr::touchMode())
            ImGui::SetKeyboardFocusHere();  // touch: drag to set distance, or tap the field to type
        m_inputFocus = false;
    }

    bool doCommit = false, doCancel = false;
    if (imTouch) {
        // im-touch: the WHOLE panel is this one tappable value well — no
        // header, hint or steppers (Steve: the full "distance dialog" kept
        // showing up; drag for coarse, pad for exact).
        if (materializr::amountLengthField("extAmt", materializr::tr("Distance"), &m_distance, /*allowSign=*/true)) {
            materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_distance);
            updateExtrude(ctx, /*applySnap=*/false);  // typed = exact
        }
        // touch: raise the soft keyboard only when the field is TAPPED (not
        // on open, which would cover the drag handle). ImGui's own
        // click-activation doesn't focus the field in this transient overlay
        // popup, so re-assert focus on the tap (issue #22).
        if (materializr::touchMode() && ImGui::IsItemClicked())
            ImGui::SetKeyboardFocusHere(-1);
    } else {
        // The member is the truth; the buffer follows it unless being typed in.
        materializr::reseedLengthBufferIfIdle("##dist", m_inputBuf, sizeof(m_inputBuf), m_distance);
        if (ImGui::InputText("##dist", m_inputBuf, sizeof(m_inputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            // Enter pressed — commit (parseFinite: keep last on garbage)
            (void)materializr::parseLength(m_inputBuf, m_distance);
            updateExtrude(ctx);
            doCommit = true;
        } else if (materializr::lengthBufferIsActive("##dist")) {
            // Only while typing — an idle re-parse truncated the member to the
            // buffer's decimals and reinterpreted stale text after a unit switch.
            float parsed = m_distance;
            if (materializr::parseLength(m_inputBuf, parsed) &&
                std::abs(parsed - m_distance) > 0.01f && std::abs(parsed) > 0.01f) {
                m_distance = parsed;
                updateExtrude(ctx, /*applySnap=*/false);  // live typing = exact
            }
        }
        ImGui::SameLine();
        ImGui::Text("%s", materializr::unitSuffix());
    }

    // Quick-nudge stepper (replaces the slider): ±10/1/0.1, and 0 to clear
    // the extrusion mid-preview. Desktop only — im-touch stays a single well.
    if (!imTouch &&
        materializr::lengthStepperRow("extrudeStep", &m_distance,
                                /*allowNegative=*/true, -50.0f, 50.0f)) {
        materializr::formatLengthDigits(m_inputBuf, sizeof(m_inputBuf), m_distance);
        updateExtrude(ctx, /*applySnap=*/false);  // steppers override the grid
    }

    // Subtract only: cut everything the sweep passes through, not just the one
    // body. Off by default — a sketch on a face means that face's body, and
    // carving a neighbour it merely overlaps would be a surprise. The checkbox
    // is here (not a setting) because it is a property of THIS cut: a profile
    // meant to pass through a stack and one meant to pocket a single plate are
    // the same gesture until you say which.
    if (m_mode == ExtrudeMode::Subtract) {
        ImGui::Spacing();
        // Nothing to re-preview either way: the preview IS the tool volume, and
        // which bodies it cuts is decided at commit. The panel is pinned to
        // 240*s, so the label has to stay short or it clips — the tooltip
        // carries the detail. im-touch gets a pill instead of a checkbox: a
        // checkbox tickbox is a fingertip-hostile tap target.
        if (imTouch) {
            if (touchui::pillButton("cutall", MZ_ICON_BODY, materializr::tr("All bodies"),
                                    m_cutAllBodies))
                m_cutAllBodies = !m_cutAllBodies;
        } else {
            ImGui::Checkbox(materializr::tr("Cut every body it reaches"), &m_cutAllBodies);
        }
        ImGui::SetItemTooltip("%s", materializr::tr("Off: cut ONE body \xE2\x80\x94 the one the sketch sits on when it has a host, otherwise whichever the sweep reaches most of.\nOn: cut every body the swept profile reaches, each as its own undoable step."));
    }

    if (!ctx.cornerCommitUi) {   // im-touch: corner ✓/✗ FABs instead
        ImGui::Spacing();
        if (ImGui::Button(materializr::btnConfirm(), ImVec2(110, 0)))
            doCommit = true;
        ImGui::SameLine();
        if (ImGui::Button(materializr::btnCancel(), ImVec2(110, 0)))
            doCancel = true;
    }
    ImGui::End();
    // Commit/cancel AFTER End() — they tear the controller's state down, and
    // the window has to be closed first.
    if (doCommit) commit(ctx);
    else if (doCancel) cancel(ctx);
}

void ExtrudeController::confirmFromKey(const IopContext& ctx) {
    if (!active()) return;
    (void)materializr::parseLength(m_inputBuf, m_distance);
    updateExtrude(ctx);
    commit(ctx);
}

void ExtrudeController::panelBody(const IopContext&, bool&) {
    // Unused: renderExtrudePanel draws the whole thing (renderPanel is
    // overridden silent) because the value well anchors to the viewport.
}

void ExtrudeController::onCleanup() {
    m_sticky = false;
    m_stickyPressWasDrag = false;
    m_profile.Nullify();
    m_mode = ExtrudeMode::NewBody;
    m_targetBody = -1;
    m_sweepSign = 1.0;
    m_cutAllBodies = false;
    m_sketchId = -1;
}

} // namespace materializr
