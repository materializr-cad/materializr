#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/Events.h"
#include "../core/EventBus.h"
#include "../core/SelectionManager.h"
#include "../viewport/RefImageRenderer.h"
#include "../viewport/Camera.h"

#include <memory>

// Reference-image canvas, owned end-to-end by this plugin (same architecture
// as ConstructionPlanePlugin):
//   - the toolbar button + command that kick Application's import flow
//     (file dialog + document mutation need deep Application access)
//   - the RefImageRenderer that draws each photo as a textured quad at its
//     host construction plane's pose
//   - the render pass that re-syncs the draw list when planes change
//
// A reference image IS a construction plane (plus a raster payload keyed by
// the plane's id in Document), so selection, the move/rotate gizmo,
// visibility, the Items panel, and sketch-on-plane all come from the existing
// plane plumbing. ConstructionPlanePlugin skips image-host planes so the blue
// quad doesn't draw over the photo.
namespace {

struct RefImageState {
    materializr::RefImageRenderer renderer;
    bool dirty = true;
};

static std::unique_ptr<RefImageState> g_state;

} // namespace

REGISTER_PLUGIN(RefImage, [](materializr::PluginContext& ctx) {
    auto action = [](materializr::PluginContext& c) {
        c.requestInteractiveOp(materializr::InteractiveOp::ImportRefImage);
    };
    // NO toolbar button. A reference image is a construction plane carrying a
    // picture (RefImageEntry is keyed by planeId), and it is now reachable
    // where planes are made: the New Plane dialog has an "Add a reference
    // image" option that works with EVERY plane type, and any existing plane
    // can take one from its properties. A separate rail button would be a
    // third route to a worse version of the same thing -- it could only ever
    // make a ground plane at the origin.
    //
    // The command stays registered so the action is still scriptable and
    // keyboard-reachable, and so old muscle memory finds it in the command
    // list rather than hitting nothing.
    ctx.registerCommand({"Import Reference Image", "", action});

    // Same three plane events as the plane renderer - pose moves, renames,
    // visibility, AND every image-side change (opacity/size/add/remove) ride
    // PlaneChangedEvent, so one dirty flag covers everything.
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
    // Tab switch: these textured quads belong to the outgoing document (see
    // the same subscription in ConstructionPlanePlugin).
    ctx.events().subscribe<materializr::ActiveDocumentChangedEvent>(
        [](const materializr::ActiveDocumentChangedEvent&) {
            if (g_state) g_state->dirty = true;
        });

    materializr::RenderPassContribution pass;
    pass.name = "ReferenceImages";
    // 500 = Application::kBodyPassPriority, the threshold that decides whether
    // a pass runs BEFORE the bodies or after. At 490 the photo ran before them
    // and every body painted straight over it - the renderer uses
    // glDepthMask(GL_FALSE), correct for a translucent overlay but it leaves no
    // depth behind, so a body drawn afterwards passes the depth test and wins.
    // Construction planes had exactly this bug and were moved across the line;
    // the photo is the same kind of thing (a tracing aid you look AT the model
    // through) and belongs on the same side. Depth TEST is still on, so a photo
    // genuinely behind a body stays correctly hidden.
    //
    // Planes (501) and axes (502) moved up one to keep the old relative order:
    // photo first, so a plane's translucent fill can still overlay it.
    pass.priority = 500;
    pass.initialize = []() -> bool {
        if (!g_state) g_state = std::make_unique<RefImageState>();
        return g_state->renderer.initialize();
    };
    pass.render = [](materializr::PluginContext& c,
                     const glm::mat4& view, const glm::mat4& proj) {
        if (!g_state) return;
        // Unlike construction planes, the image STAYS visible while sketching
        // in ortho - tracing over the photo is the whole point of the feature.
        int selectedPlaneId = -1;
        for (const auto& sel : c.selection().getSelection()) {
            if (sel.type == SelectionType::Plane && sel.planeId >= 0) {
                selectedPlaneId = sel.planeId; break;
            }
        }
        static int s_lastSelected = -2;
        if (g_state->dirty || selectedPlaneId != s_lastSelected) {
            auto& doc = c.document();
            std::vector<materializr::RefImageRenderer::Item> items;
            for (int pid : doc.getAllRefImagePlaneIds()) {
                const auto* img = doc.getRefImage(pid);
                const auto* plane = doc.getPlane(pid);
                if (!img || !plane || !plane->visible) continue;
                materializr::RefImageRenderer::Item it;
                it.planeId = pid;
                it.plane = plane->plane;
                it.widthMM = img->widthMM;
                it.heightMM = (img->pixW > 0)
                    ? img->widthMM * static_cast<double>(img->pixH) / img->pixW
                    : img->widthMM;
                it.opacity = img->opacity;
                it.selected = (pid == selectedPlaneId);
                it.fileBytes = &img->fileBytes;
                items.push_back(it);
            }
            g_state->renderer.sync(items);
            g_state->dirty = false;
            s_lastSelected = selectedPlaneId;
        }
        g_state->renderer.render(view, proj);
    };
    ctx.registerRenderPass(std::move(pass));
})
