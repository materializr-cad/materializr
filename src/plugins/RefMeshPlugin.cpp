#include "../plugin/PluginMacro.h"
#include "../plugin/PluginContext.h"
#include "../viewport/RefMeshRenderer.h"
#include "../viewport/RefMeshOverlay.h"
#include "../io/RefMeshImport.h"

#include <memory>

// The 3D counterpart to RefImagePlugin's 2D photo overlay: a single
// translucent reference mesh (an STL loaded purely to model against, never a
// Document body - see RefMeshRenderer and the "stl-recreate-via-ai-agent"
// project memory). Unlike a reference image, there is no construction plane
// or Document entry backing this - it is pure render-pass state, reached
// only through RefMeshOverlay's tiny free-function API (nothing calls that
// API yet; this plugin just makes the mechanism live in the viewport for
// whatever calls it next).
namespace {

struct RefMeshState {
    materializr::RefMeshRenderer renderer;
    bool initialized = false;
};

static std::unique_ptr<RefMeshState> g_state;

} // namespace

namespace materializr { namespace refMeshOverlay {

LoadStatus setFromStl(const std::string& path) {
    LoadStatus status;
    if (!g_state || !g_state->initialized) {
        status.errorMessage = "reference mesh renderer is not initialized yet";
        return status;
    }
    RefMeshLoadResult loaded = RefMeshImport::load(path);
    if (!loaded.success) {
        status.errorMessage = loaded.errorMessage;
        return status;
    }
    g_state->renderer.setMesh(loaded.vertices);
    status.success = true;
    return status;
}

void clear() {
    if (g_state && g_state->initialized) g_state->renderer.setMesh({});
}

bool isLoaded() {
    return g_state && g_state->renderer.hasMesh();
}

} } // namespace materializr::refMeshOverlay

REGISTER_PLUGIN(RefMesh, [](materializr::PluginContext& ctx) {
    materializr::RenderPassContribution pass;
    pass.name = "ReferenceMesh";
    // Same reasoning as RefImagePlugin's 500 (its own comment has the full
    // story): must run AFTER opaque bodies or their depth-mask-off
    // translucent draw loses the depth test to bodies drawn later and
    // vanishes. Placed after the existing 500/501/502 band (photo/planes/
    // axes) rather than reusing one of those values - nothing orders this
    // overlay relative to them, so just avoid colliding.
    pass.priority = 505;
    pass.initialize = []() -> bool {
        if (!g_state) g_state = std::make_unique<RefMeshState>();
        g_state->initialized = g_state->renderer.initialize();
        return g_state->initialized;
    };
    pass.render = [](materializr::PluginContext&,
                     const glm::mat4& view, const glm::mat4& proj) {
        if (!g_state) return;
        g_state->renderer.render(view, proj);
    };
    ctx.registerRenderPass(std::move(pass));
})
