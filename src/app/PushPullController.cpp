#include "ui/LengthField.h"
#include "PushPullController.h"
#include "../core/Document.h"
#include "../core/History.h"
#include "../core/NumParse.h"
#include "../core/SelectionManager.h"
#include "../modeling/PushPullOp.h"
#include "../modeling/Sketch.h"
#include "../ui/NumField.h"      // btnConfirm / btnCancel
#include "../ui/OpDialogGrip.h"
#include "../ui/StepperRow.h"
#include "../ui/TouchWidgets.h"  // im-touch number-pad amount field
#include "../ui/UiTheme.h"       // viewportBanner
#include "../touch_mode.h"
#include <imgui.h>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SurfaceOfRevolution.hxx>
#include <Geom_Surface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <GeomLib_IsPlanarSurface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../i18n.h"

namespace materializr {

namespace {
// A plane, or close enough that OCCT's planarity check accepts it (a face can
// be planar without a Geom_Plane). Same test as Application::faceIsPlanar and
// FaceOpControllers' local copy.
bool faceIsPlanar(const TopoDS_Face& face) {
    Handle(Geom_Surface) s = BRep_Tool::Surface(face);
    if (s.IsNull()) return false;
    if (s->IsKind(STANDARD_TYPE(Geom_Plane))) return true;
    GeomLib_IsPlanarSurface tester(s, 1.0e-7);
    return tester.IsPlanar();
}

// A body dense enough that a real boolean per drag frame is unaffordable.
constexpr int kDenseFaceCount = 250;
} // namespace

bool PushPullController::beginPushPull(const IopContext& ctx) {
    if (ctx.refuseMesh && ctx.refuseMesh("Push/Pull")) return false;
    m_st = PushPullState{};
    return begin(ctx);
}

// Scan the selection into targets, then work out the arrow frame and whether
// this gesture has to fall back to the ghost preview.
int PushPullController::onBegin(const IopContext& ctx) {
    bool curvedFaceSkipped = false;   // a rounded/fillet face was picked (#28)
    for (const auto& e : ctx.selection.getSelection()) {
        if (e.type == SelectionType::SketchRegion) {
            // For sketches loaded from a previous session, the in-memory
            // sourceFace may not have been bound yet (it isn't serialised).
            // Refresh it now so buildRegions correctly subtracts any existing
            // hole / inner wire the user sketched around.
            if (ctx.ensureSketchSourceFace) ctx.ensureSketchSourceFace(e.sketchId);
            auto sketch = ctx.doc.getSketch(e.sketchId);
            if (!sketch) continue;
            auto regions = sketch->buildRegions();
            if (e.subShapeIndex < 0 ||
                e.subShapeIndex >= static_cast<int>(regions.size())) continue;
            PushPullState::Target t;
            t.sketchId = e.sketchId;
            t.regionIndex = e.subShapeIndex;
            // A DETACHED sketch has been deliberately broken away from its
            // former host (moved independently in 3D). Keeping the stale
            // source-body id fused the new prism into a body that can be
            // hundreds of mm away — a push/pull on an unlinked sketch must
            // behave like a free-floating sketch and make its own body.
            t.sourceBodyId = sketch->isDetachedFromBody()
                                 ? -1
                                 : sketch->getSourceBody();
            t.profile = regions[e.subShapeIndex].face;
            if (t.profile.IsNull()) continue;
            // PUSH/PULL adopts the body the sketch sits flat ON. A sketch with
            // no body link (e.g. drawn on a construction plane and used to cut
            // a hole) that lies coplanar-and-over a visible body's face should
            // fuse/cut that body in place — not spawn a separate solid that
            // overlaps and z-fights it. (Extrude From keeps its always-new-
            // body semantics; a new body from this sketch is one Extrude
            // away.) A DETACHED sketch was deliberately unlinked, so it stays
            // free-floating; genuine free-space sketches (over no face) return
            // -1 and are unaffected.
            if (t.sourceBodyId < 0 && !sketch->isDetachedFromBody() &&
                ctx.findBodyUnderRegion) {
                int host = ctx.findBodyUnderRegion(t.profile, sketch->getPlane());
                if (host >= 0) t.sourceBodyId = host;
            }
            m_st.targets.push_back(t);
        } else if (e.type == SelectionType::Face && !e.shape.IsNull()) {
            // Push/Pull on a body face: face is the profile, the owning body is
            // the source. Positive distance extrudes outward (Fuse), negative
            // cuts inward (Cut).
            PushPullState::Target t;
            t.sketchId = -1;
            t.regionIndex = -1;
            t.sourceBodyId = e.bodyId;
            t.profile = TopoDS::Face(e.shape);
            if (t.profile.IsNull()) continue;
            // Push/Pull only works on FLAT faces. Sweeping a prism from a
            // curved face (fillet / round / cylinder wall) and booling it
            // produces self-intersecting garbage — skip it (and toast below).
            // Same planarity test as "Sketch on face"; a flat face bounded by
            // fillets still pushes fine — this only rejects the rounded face
            // itself. #28
            if (!faceIsPlanar(t.profile)) { curvedFaceSkipped = true; continue; }
            m_st.targets.push_back(t);
        } else if (e.type == SelectionType::Sketch && e.sketchId >= 0) {
            // Whole-sketch push/pull (selected from the Items panel, no
            // specific region): push/pull EVERY region of the sketch. Mirrors
            // the SketchRegion branch above, per region — so a body-attached
            // sketch picked from the panel edits its host body, matching
            // Extrude's whole-sketch behaviour. The rail only offers Push here
            // for an attached sketch, so sourceBodyId resolves to the real host.
            if (ctx.ensureSketchSourceFace) ctx.ensureSketchSourceFace(e.sketchId);
            auto sketch = ctx.doc.getSketch(e.sketchId);
            if (!sketch) continue;
            auto regions = sketch->buildRegions();
            for (int ri = 0; ri < static_cast<int>(regions.size()); ++ri) {
                if (regions[ri].face.IsNull()) continue;
                PushPullState::Target t;
                t.sketchId = e.sketchId;
                t.regionIndex = ri;
                t.sourceBodyId = sketch->isDetachedFromBody()
                                     ? -1
                                     : sketch->getSourceBody();
                t.profile = regions[ri].face;
                if (t.sourceBodyId < 0 && !sketch->isDetachedFromBody() &&
                    ctx.findBodyUnderRegion) {
                    int host = ctx.findBodyUnderRegion(t.profile,
                                                       sketch->getPlane());
                    if (host >= 0) t.sourceBodyId = host;
                }
                m_st.targets.push_back(t);
            }
        }
    }

    // A rounded/fillet face was picked — tell the user why it was ignored (#28).
    if (curvedFaceSkipped && ctx.toast)
        ctx.toast("Push/Pull works on flat faces \xE2\x80\x94 not curved or "
                  "fillet faces.");
    if (m_st.targets.empty()) {
        if (!curvedFaceSkipped)
            std::fprintf(stderr,
                         "Push/Pull: select a sketch region or a body face first\n");
        return -1;
    }

    // Threaded bodies are no longer refused here: a threaded rod always
    // exceeds the dense-face threshold below, so the drag shows the GHOST tool
    // volume (no per-frame boolean over helicoid faces) and the commit runs
    // once through History::pushOperation, which reflows the op beneath the
    // Thread step and re-cuts the thread in background.

    // Arrow direction at the first target's centre.
    //
    // For a flat face the UV-midpoint surface normal IS the face normal, but
    // for a CURVED face (chamfer cone, fillet torus, side of a cylinder, etc.)
    // that normal is the surface tangent perpendicular at one specific point —
    // sloped for a cone, twisted for a torus. The push/pull arrow then looks
    // like it "follows the polygon clicked" instead of a stable axis.
    //
    // Fix: if the face's underlying surface has a natural rotation axis (cone,
    // torus, cylinder, surface of revolution), use that axis as the push/pull
    // direction. Sign-correct it so positive distance still points outward
    // (positive dot product with the UV-midpoint normal preserves the
    // "fuse for +, cut for −" convention the user expects).
    m_st.hasArrow = false;
    try {
        const auto& tgt0 = m_st.targets.front();
        const TopoDS_Face& f = tgt0.profile;
        if (!f.IsNull()) {
            BRepGProp_Face prop(f);
            double u1, u2, v1, v2;
            prop.Bounds(u1, u2, v1, v2);
            gp_Pnt c; gp_Vec n;
            prop.Normal((u1 + u2) * 0.5, (v1 + v2) * 0.5, c, n);
            if (n.Magnitude() > 1e-10) {
                // NO outward correction — BRepGProp_Face::Normal() already
                // applies face orientation; this IS outward. MUST mirror
                // PushPullOp::execute (see the war-story comment there).
                gp_Vec dir = n;
                Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
                gp_Dir axis;
                bool hasAxis = false;
                if (auto cone =
                        Handle(Geom_ConicalSurface)::DownCast(surf); !cone.IsNull()) {
                    axis = cone->Axis().Direction(); hasAxis = true;
                } else if (auto tor =
                        Handle(Geom_ToroidalSurface)::DownCast(surf); !tor.IsNull()) {
                    axis = tor->Axis().Direction(); hasAxis = true;
                } else if (auto cyl =
                        Handle(Geom_CylindricalSurface)::DownCast(surf); !cyl.IsNull()) {
                    axis = cyl->Axis().Direction(); hasAxis = true;
                } else if (auto rev =
                        Handle(Geom_SurfaceOfRevolution)::DownCast(surf); !rev.IsNull()) {
                    axis = rev->Axis().Direction(); hasAxis = true;
                }
                if (hasAxis) {
                    dir = gp_Vec(axis);
                    if (dir.Dot(n) < 0) dir.Reverse();
                } else if (tgt0.sourceBodyId >= 0) {
                    // Mirror PushPullOp::execute: correct a genuinely-inverted
                    // planar normal so the live arrow agrees with the executed
                    // direction (bug #5). Untouched for curved/axis faces and
                    // for every reading that isn't an unambiguous inverted pair.
                    dir = correctedOutwardNormal(
                        ctx.doc.getBody(tgt0.sourceBodyId), f, c, dir);
                }
                m_st.normal = glm::normalize(glm::vec3(dir.X(), dir.Y(), dir.Z()));
                m_st.origin = glm::vec3(c.X(), c.Y(), c.Z());
                m_st.hasArrow = true;
            }
        }
    } catch (...) {}

    m_st.distance = 0.0f; // start at no change; drag the arrow or type a value
    m_st.distanceRaw = 0.0f;
    materializr::formatLengthDigits(m_st.inputBuf, sizeof(m_st.inputBuf), m_st.distance);
    m_st.inputFocus = true;

    // Dense bodies (a threaded rod has hundreds of helical faces) cannot
    // afford a real boolean per drag frame — and since push/pull now
    // triggers the thread-last reflow, each preview frame would re-thread
    // the whole rod (Steve: drag "a no go, non-responsive ~10s"). Those
    // bodies get a GHOST preview (tinted tool volume) and run the real
    // boolean once, on commit.
    m_st.heavyPreview = false;
    for (const auto& t : m_st.targets) {
        if (t.sourceBodyId < 0) continue;
        try {
            int nf = 0;
            for (TopExp_Explorer fx(ctx.doc.getBody(t.sourceBodyId), TopAbs_FACE);
                 fx.More() && nf <= kDenseFaceCount; fx.Next()) ++nf;
            if (nf > kDenseFaceCount) { m_st.heavyPreview = true; break; }
        } catch (...) {}
    }
    // Cut-intersecting push/pulls (a free-space sketch, or any drag that
    // goes negative mid-gesture) boolean into EVERY visible body in the
    // tool's path — the source-body face count above never sees those. If
    // any visible body is threaded, the light path would run that boolean
    // over the thread's helicoid faces per preview frame ("stacked discs"
    // + not-responding, 2026-07-21). Ghost preview + one real boolean at
    // commit, where the thread reflow handles it once.
    if (!m_st.heavyPreview) {
        for (int id : ctx.doc.getAllBodyIds()) {
            if (!ctx.doc.isBodyVisible(id)) continue;
            if (ctx.history.isBodyThreaded(id)) { m_st.heavyPreview = true; break; }
        }
    }

    // Push/Pull may edit several bodies at once, and a free-space one CREATES
    // its body — there is no single body to snapshot. The live instance's own
    // undo() is the restore path.
    return kNoTargetBody;
}

bool PushPullController::allFreeSketchTargets() const {
    if (m_st.targets.empty()) return false;
    for (const auto& t : m_st.targets)
        if (!(t.sourceBodyId < 0 && t.sketchId >= 0)) return false;
    return true;
}

std::unique_ptr<PushPullOp> PushPullController::makeOp() const {
    auto op = std::make_unique<PushPullOp>();
    std::vector<PushPullOp::Target> targets;
    for (const auto& t : m_st.targets) {
        PushPullOp::Target ot;
        ot.profile = t.profile;
        ot.sourceBodyId = t.sourceBodyId;
        targets.push_back(ot);
    }
    op->setTargets(std::move(targets));
    op->setDistance(static_cast<double>(m_st.distance));
    op->setSymmetric(m_st.symmetric);
    // Cut-intersecting: a free-space sketch (cut-or-new-body) OR any cut-
    // direction push/pull (also cut the other visible bodies in the path). An
    // extrude (positive) on a source/face body keeps it OFF → fuses its source
    // only, exactly as before.
    op->setCutIntersecting(allFreeSketchTargets() || m_st.distance < 0.0f);
    // Cascade plumbing: stamp the originating sketch+region on every target.
    // setTargets() above pre-sizes the source arrays to all -1, so this
    // upgrades them where we actually have a sketch source. Free-face
    // pushpulls (sourceBodyId-driven, no sketch) keep -1.
    for (size_t i = 0; i < m_st.targets.size(); ++i) {
        const auto& t = m_st.targets[i];
        if (t.sketchId >= 0)
            op->setSketchSource(static_cast<int>(i), t.sketchId, t.regionIndex);
    }
    return op;
}

std::unique_ptr<Operation> PushPullController::buildOp(const IopContext&) {
    return makeOp();
}

bool PushPullController::syncLiveOp(Operation& op) {
    auto& pp = static_cast<PushPullOp&>(op);
    pp.setDistance(static_cast<double>(m_st.distance));
    pp.setSymmetric(m_st.symmetric);
    // A zero-distance gesture is a no-op: leave the document un-previewed
    // rather than running a degenerate prism through the booleans.
    return std::abs(m_st.distance) > 1e-6;
}

// Tinted, renderer-only tool volume for the heavy path — the swept prism, with
// no boolean and no Document body behind it.
void PushPullController::updateGhost(const IopContext& ctx) const {
    if (!ctx.showGhost || !ctx.clearGhost) return;
    bool any = false;
    TopoDS_Compound comp;
    BRep_Builder bb;
    bb.MakeCompound(comp);
    if (std::abs(m_st.distance) > 1e-6) {
        gp_Vec pv(m_st.normal.x, m_st.normal.y, m_st.normal.z);
        pv *= static_cast<double>(m_st.distance);
        for (const auto& t : m_st.targets) {
            if (t.profile.IsNull()) continue;
            try {
                BRepPrimAPI_MakePrism mk(t.profile, pv);
                mk.Build();
                if (mk.IsDone()) { bb.Add(comp, mk.Shape()); any = true; }
            } catch (...) {}
        }
    }
    if (any) ctx.showGhost(comp, m_st.distance < 0.0f);
    else     ctx.clearGhost();
}

void PushPullController::updatePushPull(const IopContext& ctx, bool applySnap) {
    if (!active()) return;
    if (!std::isfinite(m_st.distance)) { m_st.distance = 0.0f; return; }

    // Snap the live distance to the corner-widget grid step before applying.
    // Mutating m_st.distance itself (rather than just the value handed to
    // setDistance) means the dim-arrow readout, the InputText field, and the
    // stepper all reflect the snapped value — there's no "type 5.3, see 5.3 in
    // the field, body extrudes to 5.0" discrepancy. Toggling snap off mid-drag
    // immediately frees the distance to fine values on the next frame.
    if (applySnap && ctx.snapToGrid && ctx.gridStep > 0.0f) {
        m_st.distance = std::round(m_st.distance / ctx.gridStep) * ctx.gridStep;
        materializr::formatLengthDigits(m_st.inputBuf, sizeof(m_st.inputBuf), m_st.distance);
    }

    if (m_st.heavyPreview) {
        // Ghost only. Deliberately does NOT run the base update(): with
        // wantsLivePreview() false it would do nothing except mark the meshes
        // dirty, and that full rebuild clear()s the ghost slot straight back
        // off the screen.
        updateGhost(ctx);
        return;
    }
    update(ctx);
}

// Mark only what the push/pull actually touched. On a 100+ body project this
// turns each preview frame from "re-tessellate every visible body" into
// "re-tessellate 1-2 bodies", which is the difference between unusable and
// smooth — and it keeps the full rebuild (which clears the renderer) away from
// the ghost slot.
void PushPullController::markPreviewDirty(const IopContext& ctx) const {
    if (m_st.heavyPreview) return;   // nothing was previewed; the ghost stands
    if (!ctx.markBodyDirty) { ctx.markMeshesDirty(); return; }
    for (const auto& t : m_st.targets)
        if (t.sourceBodyId >= 0) ctx.markBodyDirty(t.sourceBodyId);
    // Free-floating push/pull creates new bodies — mark them too so they
    // appear / refresh. Restrict to VISIBLE bodies: invisible ones never get a
    // renderer slot (full-rebuild skips them), so without the visibility check
    // they'd be marked dirty every preview frame forever — pure waste.
    if (!ctx.bodyHasRenderSlot) return;
    for (int id : ctx.doc.getAllBodyIds()) {
        if (!ctx.doc.isBodyVisible(id)) continue;
        if (!ctx.bodyHasRenderSlot(id)) ctx.markBodyDirty(id);
    }
}

std::unique_ptr<Operation> PushPullController::buildCommitOp(const IopContext& ctx) {
    // First, whatever happens: the ghost is renderer-only and nothing else
    // clears it.
    if (ctx.clearGhost) ctx.clearGhost();

    const bool moved = std::abs(m_st.distance) > 1e-6;
    if (!moved) return nullptr;   // nothing applied, nothing to record

    if (m_st.heavyPreview) {
        // Ghost path: the document was never previewed, so hand the base a
        // real op to push. This is where the thread reflow runs for dense
        // bodies — a single synchronous recompute instead of one per frame.
        std::fprintf(stdout, "Push/Pull committed at %.2f mm\n", m_st.distance);
        return makeOp();
    }

    // Smart cut: a free-space sketch push/pull that runs into visible bodies
    // subtracts from them (each separately) instead of making an overlapping
    // new body; so does ANY cut-direction push/pull (it cuts the source body
    // AND every other visible body in its path). The preview always showed the
    // new-body extrusion, so hand back a fresh cut-enabled op — the base undoes
    // the preview before pushing it. The op itself falls back to a new body if
    // it hits nothing, so "no intersection" is still today's behaviour.
    if (allFreeSketchTargets() || m_st.distance < 0.0f) {
        std::fprintf(stdout, "Push/Pull (smart cut) committed at %.2f mm\n",
                     m_st.distance);
        return makeOp();
    }

    // Plain extrude onto a host body: the preview IS the result. Returning null
    // records the applied instance via pushExecuted, without re-running it.
    std::fprintf(stdout, "Push/Pull committed at %.2f mm\n", m_st.distance);
    return nullptr;
}

void PushPullController::cancel(const IopContext& ctx) {
    if (ctx.clearGhost) ctx.clearGhost();   // ghost only — nothing was pushed
    InteractiveOpController::cancel(ctx);
}

void PushPullController::onCleanup() {
    m_st = PushPullState{};
}

void PushPullController::applyDrag(const IopViewport& vp) {
    m_st.distanceRaw += vp.dragAlongAxis(m_st.origin, m_st.normal, vp.mouseDelta);
    m_st.distance = m_st.distanceRaw;   // snapped in updatePushPull
    materializr::formatLengthDigits(m_st.inputBuf, sizeof(m_st.inputBuf), m_st.distance);
}

// Drag the arrow: a one-finger drag in the viewport (touch — orbit is
// suppressed while push/pull is active) or a mouse left-drag. No handle to
// latch — the whole viewport is the drag surface — so draggingHandle() stays
// false and the camera keeps its own claim (the dispatch loop skips this while
// the camera is dragging).
void PushPullController::onViewportInput(const IopViewport& vp,
                                         const IopContext& ctx) {
    if (!active() || !m_st.hasArrow) return;

    if (vp.dragging) {
        applyDrag(vp);
        updatePushPull(ctx);
    }

    // Trackpad-mode click→click sticky drag (orbit and pan both on LMB). A
    // single click in the viewport flips the sticky flag; while sticky, every
    // frame's mouse delta feeds the arrow with no button held. Clicks consumed
    // by ImGui widgets don't count. Sticky is a DESKTOP-trackpad model (move
    // the cursor with no button held) — vp.trackpadInput is already false under
    // touch, where a tap would toggle it on and then feed BOTH it and the
    // direct drag above (double distance).
    // Toggle on a click that never became a drag -- the press frame is too
    // early now that an empty-canvas LEFT-drag orbits in trackpad mode: the
    // orbit's own press would flip sticky on, and after the orbit ended the
    // cursor would keep feeding the value.
    if (vp.trackpadInput && !vp.uiCaptured) {
        if (vp.clicked) m_st.pressWasDrag = false;
        if (vp.dragging) m_st.pressWasDrag = true;
        if (vp.released && !m_st.pressWasDrag) m_st.sticky = !m_st.sticky;
    }
    if (m_st.sticky && (vp.mouseDelta.x != 0.0f || vp.mouseDelta.y != 0.0f)) {
        applyDrag(vp);
        updatePushPull(ctx);
    }
}

void PushPullController::renderPushPullPanel(const IopContext& ctx) {
    if (!active()) return;
    const float s = ctx.panel.uiScale;
    const bool imTouch = ctx.panel.imTouch;

    materializr::viewportBanner(
        ImVec4(0.3f, 0.85f, 1.0f, 1.0f),
        materializr::touchMode()
            ? "PUSH/PULL - Positive = extrude, Negative = cut. Drag the arrow, then Confirm / Cancel."
            : "PUSH/PULL - Positive = extrude, Negative = cut. Enter to confirm, Escape to cancel.");

    // im-touch: anchor the well just off the push/pull arrow's tip, like the
    // sketch bubbles; other layouts (or a tip behind the camera) keep the fixed
    // top-right spot.
    bool ppAnchored = false;
    if (imTouch && ctx.panel.anchorValid) {
        const ImVec2 vwp = ImGui::GetWindowPos();
        const float vww = ImGui::GetWindowWidth();
        float ax = std::min(std::max(ctx.panel.anchorX + 24.0f * s, vwp.x + 8.0f),
                            vwp.x + vww - 250.0f * s);
        float ay = std::max(ctx.panel.anchorY + 12.0f * s, vwp.y + 8.0f);
        ImGui::SetNextWindowPos(ImVec2(ax, ay), ImGuiCond_Appearing);
        ppAnchored = true;
    }
    if (!ppAnchored)
        ImGui::SetNextWindowPos(ImVec2(
            std::max(ImGui::GetWindowPos().x + 6.0f,
                     ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 250.0f * s),
            ImGui::GetWindowPos().y + 50), ImGuiCond_Appearing);
    // Pin the width (min == max) so moving the panel can't feed back into
    // the value field's content-avail width and ratchet the window wider.
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.0f * s, 0.0f),
                                        ImVec2(240.0f * s, 100000.0f));
    ImGui::Begin("##PushPullInput", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_AlwaysAutoResize);
    opDialogDragGrip(s);

    if (!imTouch) {   // im-touch: just the value well below
        ImGui::TextUnformatted(materializr::trFormat(m_st.symmetric ? "Distance per side (%s)"
                                   : "Distance (%s) - signed", materializr::unitSuffix()).c_str());
        ImGui::Separator();
    }

    if (m_st.inputFocus) {
        if (!materializr::touchMode())
            ImGui::SetKeyboardFocusHere();  // touch: drag to set distance, or tap the field to type
        m_st.inputFocus = false;
    }

    bool doCommit = false, doCancel = false;
    if (imTouch) {
        // im-touch: the panel is the value well (+ the Symmetric toggle below
        // when it applies) — no header, hint or steppers.
        if (materializr::amountLengthField("ppAmt", m_st.symmetric ? "Per side" : "Distance", &m_st.distance, /*allowSign=*/!m_st.symmetric)) {
            m_st.distanceRaw = m_st.distance;
            materializr::formatLengthDigits(m_st.inputBuf, sizeof(m_st.inputBuf), m_st.distance);
            updatePushPull(ctx, /*applySnap=*/false);
        }
        // touch: raise the keyboard on TAP, not on open (see the Extrude
        // field, issue #22).
        if (materializr::touchMode() && ImGui::IsItemClicked())
            ImGui::SetKeyboardFocusHere(-1);
    } else {
        // The member is the truth; the buffer follows it unless being typed in.
        materializr::reseedLengthBufferIfIdle("##ppdist", m_st.inputBuf, sizeof(m_st.inputBuf), m_st.distance);
        if (ImGui::InputText("##ppdist", m_st.inputBuf, sizeof(m_st.inputBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            (void)materializr::parseLength(m_st.inputBuf, m_st.distance);
            m_st.distanceRaw = m_st.distance;
            updatePushPull(ctx);
            doCommit = true;
        } else if (materializr::lengthBufferIsActive("##ppdist")) {
            // Only while typing — see the shell controller for why.
            float parsed = m_st.distance;
            if (materializr::parseLength(m_st.inputBuf, parsed) &&
                std::abs(parsed - m_st.distance) > 0.01f) {
                m_st.distance = parsed;
                m_st.distanceRaw = parsed;
                updatePushPull(ctx);
            }
        }
        ImGui::SameLine();
        ImGui::Text("%s", materializr::unitSuffix());
    }

    // Quick-nudge stepper (replaces the slider). Symmetric sweeps both ways, so
    // a negative distance is meaningless there — drop the minus buttons and
    // clamp positive while ticked. 0 clears the change. Desktop only —
    // im-touch stays a single well.
    if (!imTouch &&
        materializr::lengthStepperRow("ppStep", &m_st.distance,
                                /*allowNegative=*/!m_st.symmetric,
                                m_st.symmetric ? 0.1f : -50.0f, 50.0f)) {
        m_st.distanceRaw = m_st.distance;
        materializr::formatLengthDigits(m_st.inputBuf, sizeof(m_st.inputBuf), m_st.distance);
        updatePushPull(ctx, /*applySnap=*/false);   // steppers override the grid
    }

    // Symmetric: one prism swept the distance to BOTH sides of the sketch plane
    // (plane sketches only — on a body face it would push into and out of the
    // body at once). Single body, no mid-plane seam.
    {
        bool allFree = !m_st.targets.empty();
        for (const auto& t : m_st.targets)
            if (t.sourceBodyId >= 0) { allFree = false; break; }
        if (allFree && ImGui::Checkbox(materializr::tr("Symmetric (both sides)"), &m_st.symmetric)) {
            if (m_st.symmetric && m_st.distance < 0.1f) {
                m_st.distance = std::abs(m_st.distance);
                if (m_st.distance < 0.1f) m_st.distance = 0.1f;
                materializr::formatLengthDigits(m_st.inputBuf, sizeof(m_st.inputBuf), m_st.distance);
            }
            m_st.distanceRaw = m_st.distance;
            updatePushPull(ctx);
        }
        if (allFree && m_st.symmetric)
            ImGui::TextUnformatted(materializr::trFormat("Total width: %s", materializr::fmtLength(m_st.distance * 2.0f)).c_str());
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
    // the window has to be closed first. (The hand-written version called them
    // from inside; same fix Extrude got on extraction.)
    if (doCommit) commit(ctx);
    else if (doCancel) cancel(ctx);
}

void PushPullController::confirmFromKey(const IopContext& ctx) {
    if (!active()) return;
    (void)materializr::parseLength(m_st.inputBuf, m_st.distance);
    m_st.distanceRaw = m_st.distance;
    updatePushPull(ctx);
    commit(ctx);
}

void PushPullController::panelBody(const IopContext&, bool&) {
    // Unused: renderPushPullPanel draws the whole thing (renderPanel is
    // overridden silent) because the value well anchors to the viewport.
}

} // namespace materializr
