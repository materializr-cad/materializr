#pragma once
#include <string>

namespace materializr {

// Thin, deliberately small seam onto RefMeshPlugin's render-pass state, so a
// caller (a future AI tool that loads a reference STL to model against - see
// AiToolDispatcher and the "stl-recreate-via-ai-agent" project memory) never
// needs direct access to RefMeshRenderer or the plugin's internals, only
// this header. No trigger calls this yet - RefMeshPlugin wires the renderer
// into the live viewport, this is where the NEXT piece (the actual load
// trigger, tool or UI) will reach in.
namespace refMeshOverlay {

struct LoadStatus {
    bool success = false;
    std::string errorMessage;
};

// Loads `path` via RefMeshImport and replaces whatever reference mesh was
// showing. A no-op with success=false before the render pass has
// initialized (there is no live app moment where that can actually happen -
// nothing can call this before the viewport exists).
LoadStatus setFromStl(const std::string& path);

void clear();
bool isLoaded();

} // namespace refMeshOverlay
} // namespace materializr
