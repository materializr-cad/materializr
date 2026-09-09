#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/Events.h"
#include "../core/EventBus.h"
#include "../core/History.h"
#include "../core/SelectionManager.h"
#include "../modeling/ConstructionPlaneOp.h"
#include "../viewport/PlaneRenderer.h"
#include "../viewport/Camera.h"

#include <memory>

// Construction-plane feature owned end-to-end by this plugin:
//   - the toolbar button + command that opens Application's popup
//   - the PlaneRenderer that draws translucent quads for every visible plane
//   - the document-event subscriptions that mark the renderer dirty
//   - the render pass that flushes the renderer each frame
//
// Application drives the popup (m_planeOpActive et al.) because the popup
// pushes a real ConstructionPlaneOp onto history with live preview, and the
// preview-undo mechanic needs deep document access. Everything visual /
// renderer-shaped, however, lives here.
namespace {

struct PlaneRenderState {
    materializr::PlaneRenderer renderer;
    bool dirty = true;
};

// Plugin-local singleton: needed because the render-pass lambda + the event
// subscriber lambdas have to share the same renderer instance, and the
// plugin's init function is called once at startup (no `this`).
static std::unique_ptr<PlaneRenderState> g_state;

} // namespace

REGISTER_PLUGIN(ConstructionPlane, [](materializr::PluginContext& ctx) {
    // Toolbar button + Command Palette entry - both hand the actual workflow
    // over to Application's interactive-op popup. The popup pushes a
    // ConstructionPlaneOp onto history; doc.addPlane fires PlaneAddedEvent
    // (subscribed below) which flips this plugin's dirty flag so the next
    // frame's render pass picks up the new plane.
    auto action = [](materializr::PluginContext& c) {
        c.requestInteractiveOp(materializr::InteractiveOp::ConstructionPlane);
    };
    ctx.registerToolbarButton({"Construction Plane", "Create",
        materializr::SelectionContext::Always, 50,
        action, nullptr,
        "Open the Construction Plane popup. Pick XY / XZ / YZ or Parallel-to-"
        "Face (when a planar face is selected) with a live-previewed offset. "
        "The plane lands in history; sketch on it like any face."});
    ctx.registerCommand({"New Construction Plane", "", action});

    // Document → renderer sync. Three events because rename / visibility
    // toggles need a refresh too (PlaneChangedEvent), not just add/remove.
    ctx.events().subscribe<materializr::PlaneAddedEvent>(
        [](const materializr::PlaneAddedEvent&) {
            if (g_state) g_state->dirty = true;
        });
    ctx.events().subscribe<materializr::PlaneRemovedEvent>(
        [](const materializr::PlaneRemovedEvent&) {
            if (g_state) g_state->dirty = true;
        });
    ctx.events().subscribe<materializr::PlaneChangedEvent>(
        [](const materializr::PlaneChangedEvent&) {
            if (g_state) g_state->dirty = true;
        });
    // A different TAB is now in front: this cache belongs to the old
    // document. No Plane event fires for that (neither document changed -
    // the active one did), so without this the previous project's planes
    // keep drawing over the new one.
    ctx.events().subscribe<materializr::ActiveDocumentChangedEvent>(
        [](const materializr::ActiveDocumentChangedEvent&) {
            if (g_state) g_state->dirty = true;
        });

    // Render pass - Application iterates registered passes once per frame
    // after the body / edge / grid layer but before the gizmo overlay.
    // initialize() runs once on the GL thread before the first render.
    materializr::RenderPassContribution pass;
    pass.name = "ConstructionPlanes";
    pass.priority = 501; // above body (>= kBodyPassPriority), below gizmo
                         // overlay; one above the reference photo (500)
    pass.initialize = []() -> bool {
        if (!g_state) g_state = std::make_unique<PlaneRenderState>();
        return g_state->renderer.initialize();
    };
    pass.render = [](materializr::PluginContext& c,
                     const glm::mat4& view, const glm::mat4& proj) {
        if (!g_state) return;
        // Hide planes entirely while the user is sketching in ortho -
        // the canvas should be clean for drawing, the construction plane
        // they're sketching on is implied by the camera framing. Outside
        // ortho the plane stays visible so the user can find their way
        // back if they orbit away.
        if (c.isInSketchMode() && c.camera().isOrthographic()) {
            return;
        }
        // Resolve the currently-selected plane id once per frame; the
        // renderer marks that one with a brighter highlight. Selection is
        // cheap to query, so we don't bother dirty-flagging on it - just
        // resync the render list every frame the selection changes.
        int selectedPlaneId = -1;
        for (const auto& sel : c.selection().getSelection()) {
            if (sel.type == SelectionType::Plane && sel.planeId >= 0) {
                selectedPlaneId = sel.planeId; break;
            }
        }
        // Rebuild on either the document-side dirty flag OR a selection
        // change (so the highlight follows clicks). Track last-rendered
        // selected id in plugin state to detect changes.
        static int s_lastSelected = -2;
        if (g_state->dirty || selectedPlaneId != s_lastSelected) {
            g_state->renderer.clear();
            auto& doc = c.document();
            for (int pid : doc.getAllPlaneIds()) {
                const auto* entry = doc.getPlane(pid);
                if (!entry || !entry->visible) continue;
                // Planes hosting a reference image are drawn by
                // RefImagePlugin as textured quads (with their own selection
                // border) - the translucent blue fill would wash the photo out.
                if (doc.getRefImage(pid)) continue;
                glm::vec4 col(0.30f, 0.55f, 0.95f, 0.30f);
                g_state->renderer.addPlane(entry->plane, entry->name, col,
                                           static_cast<float>(entry->halfSize),
                                           pid == selectedPlaneId);
            }
            g_state->dirty = false;
            s_lastSelected = selectedPlaneId;
        }
        g_state->renderer.render(view, proj);
    };
    ctx.registerRenderPass(std::move(pass));
})
