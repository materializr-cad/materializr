#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/MeshGuard.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../core/EventBus.h"
#include "../core/Events.h"
#include "../modeling/BooleanOp.h"
#include <imgui.h>
#include <cstdio>
#include <string>
#include <vector>
#include <set>
#include "../i18n.h"

namespace {

// Distinct body ids in the current selection, in selection order.
std::vector<int> distinctSelectedBodies(materializr::PluginContext& ctx) {
    std::vector<int> bodies;
    for (const auto& s : ctx.selection().getSelection()) {
        if (s.bodyId < 0) continue;
        bool found = false;
        for (int b : bodies) if (b == s.bodyId) { found = true; break; }
        if (!found) bodies.push_back(s.bodyId);
    }
    return bodies;
}

// Reference bodies (imported meshes) are not boolean operands. An STL is a
// tessellation - a 4,881-face solid where a modelled part has a dozen analytic
// faces - so a fuse has to intersect thousands of facet pairs. It is not
// invalid: measured on a real import it completed in 101 seconds and produced
// an 8,745-face result. That is the problem. It runs on the main thread, so the
// window stops painting and the app looks hung (Steve, 2026-08-03), and what
// comes out the far side is another mesh, not a modelled body - no analytic
// faces to fillet, sketch on, or edit afterwards.
//
// So refuse rather than offer a slow path to a useless result. An imported mesh
// is there to measure and trace against; model the replacement alongside it.
//
// Gate the COMMANDS, not BooleanOp itself: a project saved before this could
// contain a mesh boolean that already succeeded, and history replay has to
// reproduce it faithfully or the file changes shape on load.
bool refuseMeshOperands(materializr::PluginContext& ctx,
                        const std::vector<int>& bodies) {
    auto meshes = materializr::meshBodiesAmong(ctx.document(), bodies);
    if (meshes.empty()) return false;
    ctx.events().publish(materializr::ToastEvent{
        materializr::meshRefusalMessage("A boolean", meshes.size(), bodies.size()),
        6.0});
    return true;
}

// Fold every body after the first into bodies[0] (target = the surviving first
// body, each other a tool, consumed). Union/Intersect combine all; Subtract
// cuts all the rest out of the first.
void runChainedBoolean(materializr::PluginContext& ctx,
                       const std::vector<int>& bodies, BooleanMode mode) {
    bool allOk = true;
    for (size_t i = 1; i < bodies.size() && allOk; ++i) {
        auto op = std::make_unique<BooleanOp>();
        op->setTargetBodyId(bodies[0]);
        op->setToolBodyId(bodies[i]);
        op->setMode(mode);
        allOk = ctx.history().pushOperation(std::move(op), ctx.document());
    }
    if (allOk) { ctx.markMeshesDirty(); ctx.selection().clear(); }
    else {
        std::fprintf(stderr, "Boolean failed (%zu bodies)\n", bodies.size());
        // Say it on screen too. This was stderr-only, so a Union that failed
        // was indistinguishable from a dead button: no toast, no history step,
        // no selection change, nothing at all. Subtract has always reported its
        // failures here; Union and Intersect run the same path and said nothing.
        ctx.events().publish(materializr::ToastEvent{
            std::string(mode == BooleanMode::Union ? "Union" : "Intersect") +
            " couldn't make a valid solid from these bodies - they "
            "may not overlap, share a coincident face, or the geometry is too "
            "degenerate.", 5.0});
    }
}

// Subtract is order-dependent, so unlike Union/Intersect we put up a modal: the
// user ticks which bodies are CUTTERS (subtracted) - everything unticked is kept
// and has the cutters cut out of it. State persists while the modal is open.
std::vector<int> g_subtractBodies;     // all selected, in selection order
std::set<int>    g_subtractCutters;    // ticked = cutter (subtracts)
bool g_subtractKeepCutters = false;    // keep cutter bodies after cutting
bool g_subtractOpenRequested = false;

// Subtract every cutter from every kept body. A cutter is consumed on its LAST
// use (so a cutter shared across several targets survives until the last one),
// unless the user asked to keep the cutters - then it's never consumed.
void runSubtractMulti(materializr::PluginContext& ctx,
                      const std::vector<int>& targets,
                      const std::vector<int>& cutters, bool keepCutters) {
    bool allOk = true;
    for (int cutter : cutters) {
        for (size_t i = 0; i < targets.size() && allOk; ++i) {
            bool lastUse = (i + 1 == targets.size());
            auto op = std::make_unique<BooleanOp>();
            op->setTargetBodyId(targets[i]);
            op->setToolBodyId(cutter);
            op->setMode(BooleanMode::Subtract);
            op->setKeepTool(keepCutters || !lastUse);
            allOk = ctx.history().pushOperation(std::move(op), ctx.document());
        }
        if (!allOk) break;
    }
    if (allOk) { ctx.markMeshesDirty(); ctx.selection().clear(); }
    else {
        std::fprintf(stderr, "Subtract failed\n");
        ctx.events().publish(materializr::ToastEvent{
            "Subtract couldn't make a valid solid from these bodies - "
            "they may not overlap, share a coincident face, or the geometry is "
            "too degenerate.", 5.0});
    }
}

} // anonymous namespace

REGISTER_PLUGIN(Boolean, [](materializr::PluginContext& ctx) {
    ctx.registerToolbarButton({"Union", "Boolean",
        materializr::SelectionContext::MultipleBodies, 100,
        [](materializr::PluginContext& ctx) {
            auto b = distinctSelectedBodies(ctx);
            if (refuseMeshOperands(ctx, b)) return;
            if (b.size() >= 2) runChainedBoolean(ctx, b, BooleanMode::Union);
        },
        nullptr,
        "Merge the selected bodies into one (A ∪ B). Overlapping volumes fuse."});

    ctx.registerToolbarButton({"Subtract", "Boolean",
        materializr::SelectionContext::MultipleBodies, 101,
        [](materializr::PluginContext& ctx) {
            // Order matters - open the modal to pick cutters vs kept bodies.
            auto b = distinctSelectedBodies(ctx);
            if (refuseMeshOperands(ctx, b)) return;
            if (b.size() >= 2) {
                g_subtractBodies = b;
                g_subtractCutters.clear();
                // Default: keep the first selected, cut the rest out of it.
                for (size_t i = 1; i < b.size(); ++i) g_subtractCutters.insert(b[i]);
                g_subtractKeepCutters = false;
                g_subtractOpenRequested = true;
            }
        },
        nullptr,
        "Cut bodies out of others. Pick which are cutters in the popup."});

    ctx.registerToolbarButton({"Intersect", "Boolean",
        materializr::SelectionContext::MultipleBodies, 102,
        [](materializr::PluginContext& ctx) {
            auto b = distinctSelectedBodies(ctx);
            if (refuseMeshOperands(ctx, b)) return;
            if (b.size() >= 2) runChainedBoolean(ctx, b, BooleanMode::Intersect);
        },
        nullptr,
        "Keep only the volume the selected bodies share (A ∩ B)."});

    // Cutters-vs-kept picker for Subtract, rendered each frame.
    materializr::OverlayContribution overlay;
    overlay.name = "BooleanSubtractPicker";
    overlay.render = [](materializr::PluginContext& ctx) {
        if (g_subtractBodies.empty()) return;
        if (g_subtractOpenRequested) {
            ImGui::OpenPopup("Subtract##boolpick");
            g_subtractOpenRequested = false;
        }
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Subtract##boolpick", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(materializr::tr("Tick the cutters - they're subtracted"));
            ImGui::TextUnformatted(materializr::tr("from the unticked bodies, which remain."));
            ImGui::Separator();
            for (int id : g_subtractBodies) {
                std::string label = ctx.document().getBodyName(id);
                if (label.empty()) label = "Body " + std::to_string(id);
                bool cutter = g_subtractCutters.count(id) > 0;
                ImGui::PushID(id);
                if (ImGui::Checkbox(label.c_str(), &cutter)) {
                    if (cutter) g_subtractCutters.insert(id);
                    else        g_subtractCutters.erase(id);
                }
                ImGui::SameLine();
                ImGui::TextDisabled(cutter ? "(cutter)" : "(keep)");
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::Checkbox(materializr::tr("Keep the cutter bodies after cutting"),
                            &g_subtractKeepCutters);
            ImGui::Separator();

            std::vector<int> cutters;
            std::vector<int> targets;
            for (int id : g_subtractBodies) {
                if (g_subtractCutters.count(id)) cutters.push_back(id);
                else                             targets.push_back(id);
            }
            bool canApply = !cutters.empty() && !targets.empty();
            if (!canApply)
                ImGui::TextDisabled("%s", materializr::tr("Need at least one cutter (ticked) and one body to keep."));

            bool apply = false;
            ImGui::BeginDisabled(!canApply);
            if (ImGui::Button(materializr::tr("Apply"))) apply = true;
            ImGui::EndDisabled();
            ImGui::SameLine();
            bool cancel = ImGui::Button(materializr::tr("Cancel"));
            if (apply || cancel) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();

            if (apply) {
                bool keep = g_subtractKeepCutters;
                g_subtractBodies.clear();
                g_subtractCutters.clear();
                runSubtractMulti(ctx, targets, cutters, keep);
            } else if (cancel) {
                g_subtractBodies.clear();
                g_subtractCutters.clear();
            }
        }
    };
    ctx.registerOverlay(std::move(overlay));
})
