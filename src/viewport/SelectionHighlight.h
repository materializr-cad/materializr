#pragma once
#include "gl_common.h"
#include <glm/glm.hpp>
#include <TopoDS_Shape.hxx>
#include <TopLoc_Location.hxx>
#include <map>
#include <vector>
#include <cstddef>   // size_t

class SelectionManager;
class Document;

namespace materializr {

class SelectionHighlight {
public:
    SelectionHighlight();
    ~SelectionHighlight();

    bool initialize();

    void render(const SelectionManager& sel, const Document& doc,
                const glm::mat4& view, const glm::mat4& projection);

    // Highlight an explicit list of shapes in one colour - the history-step
    // preview (hover/select) knows the exact faces/bodies an op produced and
    // draws them directly, bypassing the SelectionManager. Faces dispatch to
    // the face fill, edges to the edge outline, everything else to the body
    // outline. Same per-TShape tessellation cache as render().
    void highlightShapes(const std::vector<TopoDS_Shape>& shapes,
                         const glm::mat4& view, const glm::mat4& projection,
                         const glm::vec3& color);

    // Width (in pixels) of highlighted edges and body outlines. Clamped to a
    // sane range; what the driver actually honours depends on its max line
    // width, but most support up to ~10.
    void setLineWidth(float w);

    // Drop every cached tessellation. Called on project load - the entries
    // pin their source shapes alive (see CacheEntry), so the outgoing
    // project's topology would otherwise stay resident forever.
    void clearCaches();

private:
    void renderFace(const TopoDS_Shape& face, const glm::mat4& vp, const glm::vec3& color);
    void renderEdge(const TopoDS_Shape& edge, const glm::mat4& vp, const glm::vec3& color);
    void renderBody(const TopoDS_Shape& body, const glm::mat4& vp, const glm::vec3& color);

    bool compileShader(unsigned int& shader, unsigned int type, const char* source);

    // Draw the `count` vertices already resident in `vao` (xyz triplets,
    // GL_LINES order) as quads of `halfWidthPx` pixels using the geometry-
    // shader line program. Used by both edge and body highlighting so
    // thickness is honoured in core-profile GL. No per-frame upload - the
    // buffer was filled once when the cache entry was built.
    void drawThickLines(unsigned int vao, int count, const glm::mat4& vp,
                        const glm::vec3& color, float halfWidthPx);

    // Faces are a translucent triangle tint (no geometry shader).
    unsigned int m_program = 0;
    int m_locMVP = -1;
    int m_locColor = -1;

    // Lines: a separate program whose geometry shader expands each segment into
    // a screen-space quad, since glLineWidth > 1 is not honoured in core profile.
    unsigned int m_lineProgram = 0;
    int m_locLineMVP = -1;
    int m_locLineColor = -1;
    int m_locViewport = -1;
    int m_locHalfWidth = -1;

    // Highlighted-edge line width in pixels. Body outlines render slightly
    // thinner so a whole selected body stays distinguishable from single edges.
    float m_edgeLineWidth = 3.0f;

    // Highlight-tessellation caches. Without these, every frame the user has
    // something selected we re-walk the body's edges / the face's triangles /
    // the edge's curve (GCPnts / triangulation walks - 5-50ms per frame on a
    // complex part), so caching is the difference between smooth and 6-fps
    // orbits with a selection. Keyed on the sub-shape's TShape POINTER, with
    // three safety properties the original raw-pointer/vector maps lacked:
    //
    //  1. OWNERSHIP: each entry stores the TopoDS_Shape it was built from,
    //     pinning the TShape alive - so the key pointer can never be REUSED
    //     by a new allocation while the entry lives (a freed TShape's address
    //     could otherwise false-hit and render the OLD geometry for a NEW
    //     face).
    //  2. LOCATION REVALIDATION: the verts are baked in world coords; a
    //     location-only transform (multi-body gizmo move commit) keeps the
    //     TShape but moves the body, so a pointer-only key kept drawing the
    //     outline at the pre-move position ("wireframe lagging behind").
    //  3. BOUNDED SIZE: entries went stale on every topology rebuild (new
    //     TShape → new entry, old one orphaned forever) - hundreds of edits ×
    //     hundreds of KB per big-face entry leaked real memory over a long
    //     session. When a cache exceeds kCacheCap on insert it is cleared
    //     outright: only the CURRENT selection's entries get rebuilt next
    //     frame, so the flush is invisible. clearCaches() drops everything on
    //     project load (the pinned shapes belong to the outgoing project).
    //
    // Each entry owns a PERSISTENT GPU buffer: the tessellation is uploaded
    // once (GL_STATIC_DRAW) when the entry is built and the CPU copy freed,
    // so every subsequent frame just binds the VAO and draws - no per-frame
    // glBufferData re-upload of the (unchanging) selection geometry.
    struct CacheEntry {
        TopoDS_Shape shape;   // ownership pin (see above)
        TopLoc_Location loc;  // pose the geometry was sampled at
        unsigned int vao = 0; // persistent GPU buffers (uploaded once)
        unsigned int vbo = 0;
        int count = 0;        // vertex count (0 = nothing to draw)
    };
    static constexpr size_t kCacheCap = 32;
    std::map<const void*, CacheEntry> m_bodyCache;
    std::map<const void*, CacheEntry> m_faceCache;
    std::map<const void*, CacheEntry> m_edgeCache;

    // GPU-buffer lifecycle for the caches above (see CacheEntry).
    static void freeEntryGL(CacheEntry& e);
    static void freeCacheGL(std::map<const void*, CacheEntry>& m);
    static void uploadEntry(CacheEntry& e, const std::vector<float>& verts);
};

} // namespace materializr
