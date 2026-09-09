#pragma once

#include "gl_common.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <cstddef>   // size_t

namespace materializr {

class Sketch;
class SketchTool;
class SketchSolver;

class SketchRenderer {
public:
    SketchRenderer();
    ~SketchRenderer();

    bool initialize();

    // Width (px) for committed sketch geometry - user setting (Sketch line
    // width). Point markers scale with it too. See uploadAndDraw / drawLines.
    void setLineWidth(float w) { m_lineWidth = w; }

    void render(const Sketch* sketch, const SketchTool* tool,
                const glm::mat4& view, const glm::mat4& projection,
                const SketchSolver* solver = nullptr);

    // Drop every cached static-sketch GPU buffer (project load). Safe any
    // time a GL context is current; caches rebuild lazily on next render.
    void clearCache();

    // Highlight a single region of a sketch (outline only, in given color).
    void renderRegionBoundary(const Sketch* sketch, int regionIndex,
                              const glm::vec3& color, float lineWidth,
                              const glm::mat4& view, const glm::mat4& projection);

    // Translucent fill of a region's face (triangulated). Drawn under the
    // boundary so selected/hovered regions read as surfaces, not outlines.
    void renderRegionFill(const Sketch* sketch, int regionIndex,
                          const glm::vec3& color, float alpha,
                          const glm::mat4& view, const glm::mat4& projection);

    // Highlight every primitive in a sketch (lines, circles, arcs, splines,
    // polygon edges) in a single colour at the given line width - used when
    // the whole sketch is in the selection, including open profiles that
    // have no closed region for renderRegionBoundary to outline.
    void renderSketchHighlight(const Sketch* sketch,
                               const glm::vec3& color, float lineWidth,
                               const glm::mat4& view, const glm::mat4& projection);

    // Highlight only specific elements (by id) of a sketch - used to show which
    // line / circle / arc a selected history step edits, even when that sketch
    // isn't the one being actively drawn.
    void renderElementsHighlight(const Sketch* sketch,
                                 const std::set<int>& lineIds,
                                 const std::set<int>& circleIds,
                                 const std::set<int>& arcIds,
                                 const glm::vec3& color, float lineWidth,
                                 const glm::mat4& view, const glm::mat4& projection);

    // Draw a face-local measurement grid covering the active sketch face.
    // `faceExtent` is the half-width of the grid (in sketch units) around the sketch origin.
    void renderFaceGrid(const Sketch* sketch, float faceExtent, float gridStep,
                        const glm::mat4& view, const glm::mat4& projection);

private:
    bool compileShader(unsigned int& shader, unsigned int type, const char* source);
    bool linkProgram();

    void drawLines(const Sketch* sketch, const glm::mat4& vp);
    void drawCircles(const Sketch* sketch, const glm::mat4& vp);
    void drawArcs(const Sketch* sketch, const glm::mat4& vp);
    void drawPoints(const Sketch* sketch, const glm::mat4& vp);
    void drawSplines(const Sketch* sketch, const glm::mat4& vp);
    void drawPolygons(const Sketch* sketch, const glm::mat4& vp);
    void drawPreview(const Sketch* sketch, const SketchTool* tool, const glm::mat4& vp);
    void drawSvgGhost(const Sketch* sketch, const SketchTool* tool, const glm::mat4& vp);
    void drawAirfoilGhost(const Sketch* sketch, const SketchTool* tool, const glm::mat4& vp);
    void drawTrimHover(const Sketch* sketch, const SketchTool* tool, const glm::mat4& vp);
    void drawOffsetPreview(const Sketch* sketch, const SketchTool* tool, const glm::mat4& vp);
    void drawMidpointDots(const Sketch* sketch, const glm::mat4& vp);
    void drawConstraints(const Sketch* sketch, const SketchSolver* solver, const glm::mat4& vp);

    void uploadAndDraw(const std::vector<float>& verts, GLenum mode, const glm::vec3& color,
                       const glm::mat4& vp, float lineWidth = 2.0f);

    // Convert sketch 2D coordinate to 3D world position using the sketch's plane
    glm::vec3 toWorld(const Sketch* sketch, glm::vec2 pt2d) const;

    // ── Static-sketch geometry cache ────────────────────────────────────
    // Every VISIBLE sketch used to regenerate its full CPU vertex stream
    // (64-segment circles, spline resampling, …) AND re-upload it through
    // glBufferData every rendered frame - the dominant per-frame cost when
    // sketches are on screen in a complex project. Sketches rendered with
    // no tool/solver (everything except the one being actively edited) are
    // pure functions of their geometry + plane, so their draw passes are
    // captured ONCE into persistent GPU buffers and revalidated with a
    // content signature: an FNV-1a hash of the plane, points, elements and
    // pass inputs. Hash validation (instead of a mutation counter on
    // Sketch) makes the cache immune to EVERY mutation route - ops, the
    // solver, whole-object snapshot restores - at O(content bytes) per
    // frame, orders of magnitude cheaper than retessellating. The ACTIVE
    // sketch keeps the untouched live path (it legitimately changes every
    // frame while drawing).
    struct PassBuf {
        unsigned int vao = 0;
        unsigned int vbo = 0;
        int count = 0;      // vertices
        GLenum mode = GL_LINES;
        glm::vec3 color{1.0f};
        float width = 1.0f;
    };
    struct CapturedPass {
        std::vector<float> verts;
        GLenum mode;
        glm::vec3 color;
        float width;
    };
    struct SketchCacheEntry {
        std::uint64_t sig = 0;
        std::vector<PassBuf> passes;
    };
    static constexpr size_t kSketchCacheCap = 64;

    void renderCachedStatic(const Sketch* sketch, const glm::mat4& vp);
    void drawBuffer(const PassBuf& p, const glm::mat4& vp);
    void freeEntry(SketchCacheEntry& e);
    std::uint64_t contentSignature(const Sketch* sketch) const;

    // Frame-local point lookup: Sketch::getPoint is a linear scan, and the
    // draw passes called it 1-3× per element - O(elements × points) per
    // sketch per frame. Built once per render() from getPoints(); pointers
    // are valid for the duration of the call only.
    void buildPointLut(const Sketch* sketch);
    const struct SketchPoint* lutPoint(const Sketch* sketch, int id) const;

    std::map<const void*, SketchCacheEntry> m_sketchCache;
    std::vector<CapturedPass>* m_capture = nullptr; // uploadAndDraw redirect
    std::unordered_map<int, const struct SketchPoint*> m_pointLut;
    const void* m_lutSketch = nullptr; // sketch the LUT was built from

    unsigned int m_program = 0;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    float m_lineWidth = 2.5f; // committed-geometry width (Sketch line width setting)
    int m_locMVP = -1;
    int m_locColor = -1;
    int m_locAlpha = -1;
    int m_locPointSize = -1;
};

} // namespace materializr
