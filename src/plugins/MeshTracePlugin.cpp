#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../core/Document.h"
#include "../core/Events.h"
#include "../core/EventBus.h"
#include "../core/SelectionManager.h"
#include "../viewport/MeshTraceRenderer.h"

#include <memory>

// Mesh trace overlay, same architecture as RefImagePlugin: a construction
// plane hosts a MeshTraceEntry (Document.h) that points at an imported STL
// body; this plugin owns the render pass that slices that body against the
// plane's CURRENT pose (SectionCap.h, via MeshTraceRenderer) and draws the
// cross-section as a tracing underlay. Setup (creating the 3 planes + hiding
// the source mesh) is Application's job - see beginMeshTraceSetup in
// Application_Dialogs.cpp, reached from the Items panel's mesh-body context
// menu - because it needs deep Application access (bbox math, undo-free
// document mutation) the same way ref-image import does.
namespace {

struct MeshTraceState {
    materializr::MeshTraceRenderer renderer;
    bool dirty = true;
};

static std::unique_ptr<MeshTraceState> g_state;

} // namespace

REGISTER_PLUGIN(MeshTrace, [](materializr::PluginContext& ctx) {
    // No toolbar button or command: a mesh trace is set up from an imported
    // mesh body's context menu (see Application_Dialogs::beginMeshTraceSetup),
    // there's nothing else to invoke it with.
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
    // Tab switch: these slices belong to the outgoing document.
    ctx.events().subscribe<materializr::ActiveDocumentChangedEvent>(
        [](const materializr::ActiveDocumentChangedEvent&) {
            if (g_state) g_state->dirty = true;
        });

    materializr::RenderPassContribution pass;
    pass.name = "MeshTraces";
    // Same priority band as ReferenceImages (500 = kBodyPassPriority): the
    // cap is a translucent overlay drawn with depth-write off, so it needs to
    // run on the "in front" side of that threshold to keep its depth TEST
    // correct against real bodies (see RefImagePlugin's long comment on the
    // 490-vs-500 history). The source STL body itself is hidden by
    // beginMeshTraceSetup, so draw order relative to it doesn't matter.
    pass.priority = 500;
    pass.initialize = []() -> bool {
        if (!g_state) g_state = std::make_unique<MeshTraceState>();
        return g_state->renderer.initialize();
    };
    pass.render = [](materializr::PluginContext& c,
                     const glm::mat4& view, const glm::mat4& proj) {
        if (!g_state) return;
        int selectedPlaneId = -1;
        for (const auto& sel : c.selection().getSelection()) {
            if (sel.type == SelectionType::Plane && sel.planeId >= 0) {
                selectedPlaneId = sel.planeId; break;
            }
        }
        static int s_lastSelected = -2;
        if (g_state->dirty || selectedPlaneId != s_lastSelected) {
            auto& doc = c.document();
            std::vector<materializr::MeshTraceRenderer::Item> items;
            // Shapes referenced by Item::bodyShape must outlive sync() below
            // (it only reads them during that call, same rule as RefImage's
            // fileBytes), so keep them alive in this local vector.
            std::vector<TopoDS_Shape> shapes;
            std::vector<int> traceIds = doc.getAllMeshTracePlaneIds();
            shapes.reserve(traceIds.size());
            for (int pid : traceIds) {
                const auto* trace = doc.getMeshTrace(pid);
                const auto* plane = doc.getPlane(pid);
                if (!trace || !plane || !plane->visible) continue;
                TopoDS_Shape shape;
                try { shape = doc.getBody(trace->bodyId); } catch (...) { continue; }
                if (shape.IsNull()) continue;
                shapes.push_back(shape);
                materializr::MeshTraceRenderer::Item it;
                it.planeId = pid;
                it.plane = plane->plane;
                it.opacity = trace->opacity;
                it.selected = (pid == selectedPlaneId);
                it.shadowMode = (trace->mode == MeshTraceMode::Shadow);
                it.bodyShape = &shapes.back();
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
