#pragma once
#include "gl_common.h"
#include <glm/glm.hpp>
#include <gp_Pln.hxx>
#include <TopoDS_Shape.hxx>
#include <map>
#include <vector>

namespace materializr {

// Draws mesh traces (STL cross-section underlays hosted on construction
// planes, see Document.h's MeshTraceEntry) as a filled cap + outline at each
// host plane's CURRENT pose. Modeled on RefImageRenderer, but there is no
// texture to upload: SectionCap.h's sliceSection() already returns WORLD-
// space line/triangle positions, so sync() just re-slices and caches CPU
// buffers per plane id; render() uploads and draws them, same blend / no-
// depth-write treatment as the photo underlay so sketch lines stay legible
// on top.
class MeshTraceRenderer {
public:
    MeshTraceRenderer();
    ~MeshTraceRenderer();

    bool initialize();

    struct Item {
        int planeId = -1;
        gp_Pln plane;
        float opacity = 0.5f;
        bool selected = false;
        // false = CrossSection (sliceSection: exact intersection with the
        // plane's current pose), true = Shadow (computeMeshShadow: every
        // triangle flattened onto the plane, the mesh's overall silhouette
        // blob along the plane's normal). Mirrors Document.h's MeshTraceMode
        // as a bool so this header doesn't need to include Document.h - the
        // plugin translates the enum when building items.
        bool shadowMode = false;
        // Only READ during this sync() call (aliases Document storage, like
        // RefImageRenderer::Item::fileBytes) - never held past it.
        const TopoDS_Shape* bodyShape = nullptr;
    };

    // Re-slice every item's body against its plane's current pose and cache
    // the result. Call from the GL thread (render pass) whenever a hosted
    // plane moved or the trace list changed. Re-slicing every dirty item is
    // synchronous - fine for the tessellation sizes StlIO produces (capped
    // at 60k triangles), but a plane dragged every frame during a move-gizmo
    // drag will re-slice every frame too; if that turns out laggy on a dense
    // import, follow SectionView's async-worker pattern (Application_
    // Viewport.cpp) instead of doing this on the GL thread.
    void sync(const std::vector<Item>& items);

    void render(const glm::mat4& view, const glm::mat4& projection);

private:
    bool compileShader(unsigned int& shader, unsigned int type, const char* src);

    struct Slice {
        std::vector<float> lines; // x,y,z pairs, GL_LINES
        std::vector<float> cap;   // x,y,z triples, GL_TRIANGLES
        float opacity = 0.5f;
        bool selected = false;
    };
    std::map<int, Slice> m_slices; // planeId -> cached slice

    unsigned int m_capProgram = 0;
    int m_capLocMVP = -1;
    int m_capLocColor = -1;
    unsigned int m_lineProgram = 0;
    int m_lineLocMVP = -1;
    int m_lineLocColor = -1;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
};

} // namespace materializr
