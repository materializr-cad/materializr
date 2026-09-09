#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/Events.h"
#include "../core/EventBus.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../modeling/ConstructionAxisOp.h"
#include "../viewport/AxisRenderer.h"
#include "../viewport/Camera.h"

#include <memory>

// Construction-axis feature owned by this plugin, exactly parallel to
// ConstructionPlanePlugin's structure:
//   - toolbar button + command that opens the host's popup
//   - the AxisRenderer that draws each visible axis
//   - the document-event subscriptions that mark the renderer dirty
//   - the render pass that flushes the renderer each frame
//
// Application drives the popup (m_axisOpActive et al. - TODO scaffolding)
// because the popup pushes a real ConstructionAxisOp onto history with
// live preview; that needs deep document access. The visuals + plumbing
// live here.
namespace {

struct AxisRenderState {
    materializr::AxisRenderer renderer;
    bool dirty = true;
};

// Plugin-local singleton - needed so the render-pass lambda and the event
// subscriber lambdas share the same renderer instance across init.
static std::unique_ptr<AxisRenderState> g_state;

} // namespace

REGISTER_PLUGIN(ConstructionAxis, [](materializr::PluginContext& ctx) {
    // Toolbar button + Command Palette entry - both hand the workflow over
    // to Application's interactive-op popup (same routing the plane plugin
    // uses, with a different op name so the host can dispatch).
    auto action = [](materializr::PluginContext& c) {
        c.requestInteractiveOp(materializr::InteractiveOp::ConstructionAxis);
    };
    ctx.registerToolbarButton({"Construction Axis", "Create",
        materializr::SelectionContext::Always, 51,
        action, nullptr,
        "Open the Construction Axis popup. Pick World-X / Y / Z through an "
        "origin point, or two world points / a face normal. The axis lands "
        "in history; later Revolve uses it as a rotation axis."});
    ctx.registerCommand({"New Construction Axis", "", action});

    // Document → renderer sync.
    ctx.events().subscribe<materializr::AxisAddedEvent>(
        [](const materializr::AxisAddedEvent&) {
            if (g_state) g_state->dirty = true;
        });
    ctx.events().subscribe<materializr::AxisRemovedEvent>(
        [](const materializr::AxisRemovedEvent&) {
            if (g_state) g_state->dirty = true;
        });
    ctx.events().subscribe<materializr::AxisChangedEvent>(
        [](const materializr::AxisChangedEvent&) {
            if (g_state) g_state->dirty = true;
        });
    // Tab switch: this draw list belongs to the outgoing document (see the
    // same subscription in ConstructionPlanePlugin).
    ctx.events().subscribe<materializr::ActiveDocumentChangedEvent>(
        [](const materializr::ActiveDocumentChangedEvent&) {
            if (g_state) g_state->dirty = true;
        });

    // Render pass - Application iterates registered passes once per frame.
    materializr::RenderPassContribution pass;
    pass.name = "ConstructionAxes";
    pass.priority = 502; // just after planes (501) so axes draw on top
    pass.initialize = []() -> bool {
        if (!g_state) g_state = std::make_unique<AxisRenderState>();
        return g_state->renderer.initialize();
    };
    pass.render = [](materializr::PluginContext& c,
                     const glm::mat4& view, const glm::mat4& proj) {
        if (!g_state) return;
        // Same hide-during-ortho-sketch rule as planes - clean canvas for
        // drawing. Axes are decorative anchors, not sketch geometry.
        if (c.isInSketchMode() && c.camera().isOrthographic()) return;

        // Resolve selection so the renderer can highlight the selected axis.
        int selectedAxisId = -1;
        for (const auto& sel : c.selection().getSelection()) {
            if (sel.type == SelectionType::Axis && sel.axisId >= 0) {
                selectedAxisId = sel.axisId; break;
            }
        }
        static int s_lastSelected = -2;
        if (g_state->dirty || selectedAxisId != s_lastSelected) {
            g_state->renderer.clear();
            auto& doc = c.document();
            for (int aid : doc.getAllAxisIds()) {
                const auto* entry = doc.getAxis(aid);
                if (!entry || !entry->visible) continue;
                // Default axis colour: a warm orange so it reads against
                // the steel/copper body palette.
                glm::vec4 col(0.95f, 0.55f, 0.20f, 1.0f);
                g_state->renderer.addAxis(entry->origin, entry->direction,
                                          entry->name, col,
                                          static_cast<float>(entry->halfLength),
                                          aid == selectedAxisId);
            }
            g_state->dirty = false;
            s_lastSelected = selectedAxisId;
        }
        g_state->renderer.render(view, proj);
    };
    ctx.registerRenderPass(std::move(pass));
})
