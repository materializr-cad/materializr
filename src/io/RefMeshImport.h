#pragma once
#include <string>
#include <vector>

namespace materializr {

struct RefMeshLoadResult {
    bool success = false;
    std::string errorMessage;
    // Position+normal interleaved triangles (6 floats/vertex, 3 vertices/
    // triangle) - hand straight to RefMeshRenderer::setMesh. Already rotated
    // into the app's Y-up world the same way StlIO::import rotates a real
    // import (STL is conventionally Z-up), so a reference mesh and any real
    // geometry line up without the caller doing anything extra.
    std::vector<float> vertices;
    int triangleCount = 0;
};

// Reads raw STL triangulation for display ONLY - no decimation, sewing, or
// solid-building, and the result never touches a Document (see
// RefMeshRenderer and the "stl-recreate-via-ai-agent" project memory for
// why: this mesh must never be pickable, exportable, or booleanable, unlike
// a real StlIO::import). Unlike StlIO::import, a malformed or non-manifold
// mesh still renders whatever triangles it can read - there is no modeling
// correctness at stake here, only a visual trace to model against.
class RefMeshImport {
public:
    static RefMeshLoadResult load(const std::string& filePath);
};

} // namespace materializr
