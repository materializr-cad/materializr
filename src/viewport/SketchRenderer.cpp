#include "gl_common.h"
#include "SketchRenderer.h"
#include "modeling/AirfoilImport.h"
#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchConstraints.h"

#include <glm/gtc/type_ptr.hpp>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <Geom_Curve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <cstdio>
#include <cmath>

namespace materializr {

static const char* s_vertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
uniform mat4 u_mvp;
uniform float u_pointSize;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
    gl_PointSize = u_pointSize;   // GL ES honours this; desktop uses glPointSize
}
)";

static const char* s_fragSource = R"(
#version 330 core
uniform vec3 u_color;
uniform float u_alpha;
out vec4 fragColor;
void main() {
    fragColor = vec4(u_color, u_alpha);
}
)";

SketchRenderer::SketchRenderer() {}

SketchRenderer::~SketchRenderer() {
    clearCache();
    if (m_program) glDeleteProgram(m_program);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
}

void SketchRenderer::freeEntry(SketchCacheEntry& e) {
    for (auto& p : e.passes) {
        if (p.vao) glDeleteVertexArrays(1, &p.vao);
        if (p.vbo) glDeleteBuffers(1, &p.vbo);
    }
    e.passes.clear();
    e.sig = 0;
}

void SketchRenderer::clearCache() {
    for (auto& [key, e] : m_sketchCache) freeEntry(e);
    m_sketchCache.clear();
}

void SketchRenderer::buildPointLut(const Sketch* sketch) {
    m_pointLut.clear();
    m_lutSketch = sketch;
    const auto& pts = sketch->getPoints();
    m_pointLut.reserve(pts.size());
    for (const auto& p : pts) m_pointLut.emplace(p.id, &p);
}

const SketchPoint* SketchRenderer::lutPoint(const Sketch* sketch, int id) const {
    // Only trust the LUT for the sketch it was built from - the highlight /
    // region entry points can arrive with a DIFFERENT sketch while the LUT
    // still holds the last render()'s table, and sketch point ids are small
    // ints that would false-hit across sketches. Everything else falls back
    // to the authoritative linear lookup.
    if (m_lutSketch == sketch) {
        auto it = m_pointLut.find(id);
        return it != m_pointLut.end() ? it->second : nullptr;
    }
    // LUT belongs to another sketch - authoritative linear lookup. (This was
    // `return lutPoint(sketch, id);`: infinite self-recursion, tail-call
    // compiled into a 100% CPU spin. Reached whenever a highlight draws a
    // sketch the LUT wasn't built from - selecting a sketch from the im-touch
    // lite tree, or the first frame of a brand-new sketch.)
    for (const auto& p : sketch->getPoints())
        if (p.id == id) return &p;
    return nullptr;
}

std::uint64_t SketchRenderer::contentSignature(const Sketch* sketch) const {
    // FNV-1a over everything the standard passes read: plane basis, points,
    // element tables, spline control ids, the source-face centroid (midpoint
    // dots) and the line-width setting (pass widths). Any change - from ops,
    // the solver, or a whole-object snapshot restore - changes the hash.
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    auto mixD = [&](double v) { mix(&v, sizeof v); };
    auto mixF = [&](float v)  { mix(&v, sizeof v); };
    auto mixI = [&](int v)    { mix(&v, sizeof v); };

    const gp_Ax3& ax = sketch->getPlane().Position();
    mixD(ax.Location().X());   mixD(ax.Location().Y());   mixD(ax.Location().Z());
    mixD(ax.XDirection().X()); mixD(ax.XDirection().Y()); mixD(ax.XDirection().Z());
    mixD(ax.YDirection().X()); mixD(ax.YDirection().Y()); mixD(ax.YDirection().Z());
    mixD(ax.Direction().X());  mixD(ax.Direction().Y());  mixD(ax.Direction().Z());
    mixF(m_lineWidth);

    glm::vec2 centroid;
    const bool hasCentroid = sketch->getSourceFaceCentroid(centroid);
    mixI(hasCentroid ? 1 : 0);
    if (hasCentroid) { mixF(centroid.x); mixF(centroid.y); }
    glm::vec2 trueCenter;
    const bool hasCenter = sketch->getCenterPoint(trueCenter);
    mixI(hasCenter ? 1 : 0);
    if (hasCenter) { mixF(trueCenter.x); mixF(trueCenter.y); }

    for (const auto& p : sketch->getPoints()) {
        mixI(p.id); mixF(p.pos.x); mixF(p.pos.y);
        mixI(p.fromText ? 1 : 0); mixI(p.isConstruction ? 1 : 0);
    }
    for (const auto& l : sketch->getLines()) {
        mixI(l.id); mixI(l.startPointId); mixI(l.endPointId);
        mixI(l.fromText ? 1 : 0);
    }
    for (const auto& c : sketch->getCircles()) {
        mixI(c.id); mixI(c.centerPointId); mixD(c.radius);
    }
    for (const auto& a : sketch->getArcs()) {
        mixI(a.id); mixI(a.centerPointId); mixI(a.startPointId);
        mixI(a.endPointId); mixD(a.radius);
    }
    for (const auto& s : sketch->getSplines()) {
        mixI(s.id);
        for (int cp : s.controlPointIds) mixI(cp);
    }
    for (const auto& pg : sketch->getPolygons()) {
        mixI(pg.id); mixI(pg.centerPointId);
    }
    return h;
}

void SketchRenderer::drawBuffer(const PassBuf& p, const glm::mat4& vp) {
    if (!p.count || !p.vao) return;
    // Mirrors uploadAndDraw's state setup, minus the CPU regen + upload.
    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform3fv(m_locColor, 1, glm::value_ptr(p.color));
    glUniform1f(m_locAlpha, 1.0f);
    glBindVertexArray(p.vao);
    glLineWidth(p.width);
    if (m_locPointSize >= 0)
        glUniform1f(m_locPointSize, p.mode == GL_POINTS ? p.width : 1.0f);
#if !defined(MZ_GLES)
    if (p.mode == GL_POINTS) glPointSize(p.width);
#endif
    glDrawArrays(p.mode, 0, p.count);
    glBindVertexArray(0);
    glUseProgram(0);
}

void SketchRenderer::renderCachedStatic(const Sketch* sketch, const glm::mat4& vp) {
    const void* key = sketch;
    const std::uint64_t sig = contentSignature(sketch);
    auto it = m_sketchCache.find(key);
    if (it == m_sketchCache.end() || it->second.sig != sig) {
        if (it == m_sketchCache.end() && m_sketchCache.size() >= kSketchCacheCap)
            clearCache(); // flush; visible sketches rebuild lazily - cheap
        SketchCacheEntry& e = m_sketchCache[key];
        freeEntry(e);

        // Re-run the standard passes with uploadAndDraw redirected into a
        // capture list, then upload each captured pass into its own
        // persistent buffer.
        std::vector<CapturedPass> caps;
        m_capture = &caps;
        drawLines(sketch, vp);
        drawCircles(sketch, vp);
        drawArcs(sketch, vp);
        drawSplines(sketch, vp);
        drawPolygons(sketch, vp);
        drawPoints(sketch, vp);
        drawMidpointDots(sketch, vp);
        m_capture = nullptr;

        e.passes.reserve(caps.size());
        for (const auto& c : caps) {
            if (c.verts.empty()) continue;
            PassBuf p;
            p.mode = c.mode;
            p.color = c.color;
            p.width = c.width;
            p.count = static_cast<int>(c.verts.size() / 3);
            glGenVertexArrays(1, &p.vao);
            glGenBuffers(1, &p.vbo);
            glBindVertexArray(p.vao);
            glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(c.verts.size() * sizeof(float)),
                         c.verts.data(), GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                                  nullptr);
            glEnableVertexAttribArray(0);
            glBindVertexArray(0);
            e.passes.push_back(p);
        }
        e.sig = sig;
        it = m_sketchCache.find(key);
    }
    for (const auto& p : it->second.passes) drawBuffer(p, vp);
}

bool SketchRenderer::initialize() {
    unsigned int vert = 0, frag = 0;
    if (!compileShader(vert, GL_VERTEX_SHADER, s_vertSource)) return false;
    if (!compileShader(frag, GL_FRAGMENT_SHADER, s_fragSource)) {
        glDeleteShader(vert);
        return false;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vert);
    glAttachShader(m_program, frag);
    glLinkProgram(m_program);
    glDeleteShader(vert);
    glDeleteShader(frag);

    int success = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(m_program, 512, nullptr, log);
        std::fprintf(stderr, "SketchRenderer link error: %s\n", log);
        return false;
    }

    m_locMVP = glGetUniformLocation(m_program, "u_mvp");
    m_locColor = glGetUniformLocation(m_program, "u_color");
    m_locAlpha = glGetUniformLocation(m_program, "u_alpha");
    m_locPointSize = glGetUniformLocation(m_program, "u_pointSize");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    return true;
}

void SketchRenderer::uploadAndDraw(const std::vector<float>& verts, GLenum mode,
                                    const glm::vec3& color, const glm::mat4& vp,
                                    float lineWidth) {
    if (verts.empty()) return;

    // Static-sketch cache rebuild in progress: capture the pass instead of
    // drawing - renderCachedStatic uploads it into a persistent buffer once.
    if (m_capture) {
        m_capture->push_back({verts, mode, color, lineWidth});
        return;
    }

    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform3fv(m_locColor, 1, glm::value_ptr(color));
    glUniform1f(m_locAlpha, 1.0f);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);

    glLineWidth(lineWidth);
    // For GL_POINTS the same `lineWidth` arg doubles as the marker diameter.
    // GL ES has no glPointSize, so the vertex shader sets gl_PointSize from this
    // uniform; desktop honours glPointSize directly.
    if (m_locPointSize >= 0) glUniform1f(m_locPointSize, mode == GL_POINTS ? lineWidth : 1.0f);
#if !defined(MZ_GLES)
    if (mode == GL_POINTS) glPointSize(lineWidth);
#endif

    glDrawArrays(mode, 0, static_cast<int>(verts.size() / 3));

    glBindVertexArray(0);
    glUseProgram(0);
}

void SketchRenderer::render(const Sketch* sketch, const SketchTool* tool,
                             const glm::mat4& view, const glm::mat4& projection,
                             const SketchSolver* solver) {
    if (!sketch || !m_program) return;

    glm::mat4 vp = projection * view;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Frame-local id→point lookup for the passes below (getPoint is a linear
    // scan; per-element lookups made each pass O(elements × points)).
    buildPointLut(sketch);

    // A sketch rendered with no tool/solver is STATIC (not being edited):
    // draw it from cached GPU buffers instead of regenerating + re-uploading
    // its whole vertex stream every frame. The active sketch (tool/solver
    // present) keeps the live path below - it changes every frame anyway,
    // and its overlays depend on transient tool state.
    if (!tool && !solver) {
        renderCachedStatic(sketch, vp);
        m_pointLut.clear();
        m_lutSketch = nullptr;
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        return;
    }

    drawLines(sketch, vp);
    drawCircles(sketch, vp);
    drawArcs(sketch, vp);
    drawSplines(sketch, vp);
    drawPolygons(sketch, vp);
    drawPoints(sketch, vp);
    drawMidpointDots(sketch, vp);

    // Selection highlight overlay: re-draw selected lines/points in a bright
    // colour so the user can see what Mirror/Copy/Rotate will act on.
    if (tool && tool->hasElementSelection()) {
        const auto& selLines = tool->getSelectedLines();
        const auto& selPoints = tool->getSelectedPoints();
        if (!selLines.empty()) {
            std::vector<float> lv;
            for (const auto& l : sketch->getLines()) {
                if (!selLines.count(l.id)) continue;
                const SketchPoint* a = lutPoint(sketch, l.startPointId);
                const SketchPoint* b = lutPoint(sketch, l.endPointId);
                if (!a || !b) continue;
                glm::vec3 wa = toWorld(sketch, a->pos);
                glm::vec3 wb = toWorld(sketch, b->pos);
                lv.push_back(wa.x); lv.push_back(wa.y); lv.push_back(wa.z);
                lv.push_back(wb.x); lv.push_back(wb.y); lv.push_back(wb.z);
            }
            if (!lv.empty())
                uploadAndDraw(lv, GL_LINES, glm::vec3(1.0f, 0.85f, 0.1f), vp, 3.5f);
        }
        const auto& selCircles = tool->getSelectedCircles();
        if (!selCircles.empty()) {
            const int segments = 64;
            std::vector<float> cv;
            for (const auto& c : sketch->getCircles()) {
                if (!selCircles.count(c.id)) continue;
                const SketchPoint* center = lutPoint(sketch, c.centerPointId);
                if (!center) continue;
                float r = static_cast<float>(c.radius);
                for (int i = 0; i < segments; i++) {
                    float a1 = 2.0f * M_PI * i / segments;
                    float a2 = 2.0f * M_PI * (i + 1) / segments;
                    glm::vec3 w1 = toWorld(sketch, glm::vec2(center->pos.x + r * std::cos(a1),
                                                             center->pos.y + r * std::sin(a1)));
                    glm::vec3 w2 = toWorld(sketch, glm::vec2(center->pos.x + r * std::cos(a2),
                                                             center->pos.y + r * std::sin(a2)));
                    cv.push_back(w1.x); cv.push_back(w1.y); cv.push_back(w1.z);
                    cv.push_back(w2.x); cv.push_back(w2.y); cv.push_back(w2.z);
                }
            }
            if (!cv.empty())
                uploadAndDraw(cv, GL_LINES, glm::vec3(1.0f, 0.85f, 0.1f), vp, 3.5f);
        }
        const auto& selArcs = tool->getSelectedArcs();
        if (!selArcs.empty()) {
            const int segments = 32;
            std::vector<float> av;
            for (const auto& arc : sketch->getArcs()) {
                if (!selArcs.count(arc.id)) continue;
                const SketchPoint* center = lutPoint(sketch, arc.centerPointId);
                const SketchPoint* start = lutPoint(sketch, arc.startPointId);
                const SketchPoint* end = lutPoint(sketch, arc.endPointId);
                if (!center || !start || !end) continue;
                float startAngle = std::atan2(start->pos.y - center->pos.y,
                                              start->pos.x - center->pos.x);
                float endAngle = std::atan2(end->pos.y - center->pos.y,
                                            end->pos.x - center->pos.x);
                if (endAngle < startAngle) endAngle += 2.0f * M_PI;
                float r = static_cast<float>(arc.radius);
                for (int i = 0; i < segments; i++) {
                    float a1 = startAngle + (static_cast<float>(i) / segments) * (endAngle - startAngle);
                    float a2 = startAngle + (static_cast<float>(i + 1) / segments) * (endAngle - startAngle);
                    glm::vec3 w1 = toWorld(sketch, glm::vec2(center->pos.x + r * std::cos(a1),
                                                             center->pos.y + r * std::sin(a1)));
                    glm::vec3 w2 = toWorld(sketch, glm::vec2(center->pos.x + r * std::cos(a2),
                                                             center->pos.y + r * std::sin(a2)));
                    av.push_back(w1.x); av.push_back(w1.y); av.push_back(w1.z);
                    av.push_back(w2.x); av.push_back(w2.y); av.push_back(w2.z);
                }
            }
            if (!av.empty())
                uploadAndDraw(av, GL_LINES, glm::vec3(1.0f, 0.85f, 0.1f), vp, 3.5f);
        }
        const auto& selSplines = tool->getSelectedSplines();
        if (!selSplines.empty()) {
            std::vector<float> sv;
            for (const auto& sp : sketch->getSplines()) {
                if (!selSplines.count(sp.id)) continue;
                std::vector<glm::vec2> pts = sketch->sampleSpline2D(sp, 12);
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    glm::vec3 w1 = toWorld(sketch, pts[i]);
                    glm::vec3 w2 = toWorld(sketch, pts[i + 1]);
                    sv.push_back(w1.x); sv.push_back(w1.y); sv.push_back(w1.z);
                    sv.push_back(w2.x); sv.push_back(w2.y); sv.push_back(w2.z);
                }
            }
            if (!sv.empty())
                uploadAndDraw(sv, GL_LINES, glm::vec3(1.0f, 0.85f, 0.1f), vp, 3.5f);
        }
        if (!selPoints.empty()) {
            std::vector<float> pv;
            for (const auto& p : sketch->getPoints()) {
                if (!selPoints.count(p.id)) continue;
                glm::vec3 w = toWorld(sketch, p.pos);
                pv.push_back(w.x); pv.push_back(w.y); pv.push_back(w.z);
            }
            if (!pv.empty())
                uploadAndDraw(pv, GL_POINTS, glm::vec3(1.0f, 0.85f, 0.1f), vp, 8.0f);
        }
    }

    if (solver) drawConstraints(sketch, solver, vp);
    if (tool) {
        drawPreview(sketch, tool, vp);
        drawTrimHover(sketch, tool, vp);
        drawOffsetPreview(sketch, tool, vp);
        drawSvgGhost(sketch, tool, vp);
        drawAirfoilGhost(sketch, tool, vp);
    }

    // The LUT holds pointers into the sketch's point vector - valid only for
    // this call. Clear AND un-own it so a later entry point can never
    // dereference pointers a between-frames mutation invalidated.
    m_pointLut.clear();
    m_lutSketch = nullptr;

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

glm::vec3 SketchRenderer::toWorld(const Sketch* sketch, glm::vec2 pt2d) const {
    const gp_Pln& pln = sketch->getPlane();
    const gp_Ax3& ax = pln.Position();
    gp_Pnt origin = ax.Location();
    gp_Dir xd = ax.XDirection();
    gp_Dir yd = ax.YDirection();
    gp_Dir nd = ax.Direction();
    return glm::vec3(
        origin.X() + pt2d.x * xd.X() + pt2d.y * yd.X() + 0.001 * nd.X(),
        origin.Y() + pt2d.x * xd.Y() + pt2d.y * yd.Y() + 0.001 * nd.Y(),
        origin.Z() + pt2d.x * xd.Z() + pt2d.y * yd.Z() + 0.001 * nd.Z()
    );
}

void SketchRenderer::drawLines(const Sketch* sketch, const glm::mat4& vp) {
    std::vector<float> verts;
    for (const auto& line : sketch->getLines()) {
        const SketchPoint* p1 = lutPoint(sketch, line.startPointId);
        const SketchPoint* p2 = lutPoint(sketch, line.endPointId);
        if (!p1 || !p2) continue;

        glm::vec3 w1 = toWorld(sketch, p1->pos);
        glm::vec3 w2 = toWorld(sketch, p2->pos);
        verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
        verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
    }

    // Deep cobalt - saturated enough to pop against the light-blue sketch face
    // tint, while keeping the "blue = sketch" convention.
    glm::vec3 color = glm::vec3(0.10f, 0.35f, 0.95f);
    uploadAndDraw(verts, GL_LINES, color, vp, m_lineWidth);
}

void SketchRenderer::drawCircles(const Sketch* sketch, const glm::mat4& vp) {
    const int segments = 64;
    std::vector<float> verts;

    for (const auto& circle : sketch->getCircles()) {
        const SketchPoint* center = lutPoint(sketch, circle.centerPointId);
        if (!center) continue;

        for (int i = 0; i < segments; i++) {
            float a1 = 2.0f * M_PI * i / segments;
            float a2 = 2.0f * M_PI * (i + 1) / segments;
            float r = static_cast<float>(circle.radius);

            glm::vec2 s1(center->pos.x + r * std::cos(a1), center->pos.y + r * std::sin(a1));
            glm::vec2 s2(center->pos.x + r * std::cos(a2), center->pos.y + r * std::sin(a2));
            glm::vec3 w1 = toWorld(sketch, s1);
            glm::vec3 w2 = toWorld(sketch, s2);
            verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
            verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
        }
    }

    glm::vec3 color = glm::vec3(0.10f, 0.35f, 0.95f); // deep cobalt
    uploadAndDraw(verts, GL_LINES, color, vp, m_lineWidth);
}

void SketchRenderer::drawArcs(const Sketch* sketch, const glm::mat4& vp) {
    const int segments = 32;
    std::vector<float> verts;

    for (const auto& arc : sketch->getArcs()) {
        const SketchPoint* center = lutPoint(sketch, arc.centerPointId);
        const SketchPoint* start = lutPoint(sketch, arc.startPointId);
        const SketchPoint* end = lutPoint(sketch, arc.endPointId);
        if (!center || !start || !end) continue;

        float startAngle = std::atan2(start->pos.y - center->pos.y, start->pos.x - center->pos.x);
        float endAngle = std::atan2(end->pos.y - center->pos.y, end->pos.x - center->pos.x);

        if (endAngle < startAngle) endAngle += 2.0f * M_PI;

        for (int i = 0; i < segments; i++) {
            float t1 = static_cast<float>(i) / segments;
            float t2 = static_cast<float>(i + 1) / segments;
            float a1 = startAngle + t1 * (endAngle - startAngle);
            float a2 = startAngle + t2 * (endAngle - startAngle);

            float r = static_cast<float>(arc.radius);
            glm::vec2 s1(center->pos.x + r * std::cos(a1), center->pos.y + r * std::sin(a1));
            glm::vec2 s2(center->pos.x + r * std::cos(a2), center->pos.y + r * std::sin(a2));
            glm::vec3 w1 = toWorld(sketch, s1);
            glm::vec3 w2 = toWorld(sketch, s2);
            verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
            verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
        }
    }

    glm::vec3 color = glm::vec3(0.10f, 0.35f, 0.95f); // deep cobalt
    uploadAndDraw(verts, GL_LINES, color, vp, m_lineWidth);
}

void SketchRenderer::drawSplines(const Sketch* sketch, const glm::mat4& vp) {
    std::vector<float> verts;

    for (const auto& spline : sketch->getSplines()) {
        // Draw the SAME smooth interpolated curve the profile builder
        // emits - what you see is what extrudes. (This used to draw the
        // raw control polyline, back when splines were display-only.)
        std::vector<glm::vec2> pts = sketch->sampleSpline2D(spline, 12);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            glm::vec3 w1 = toWorld(sketch, pts[i]);
            glm::vec3 w2 = toWorld(sketch, pts[i + 1]);
            verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
            verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
        }
    }

    glm::vec3 color = glm::vec3(0.10f, 0.35f, 0.95f); // deep cobalt - match all sketch geometry
    uploadAndDraw(verts, GL_LINES, color, vp, m_lineWidth);
}

void SketchRenderer::drawPolygons(const Sketch* sketch, const glm::mat4& vp) {
    // Polygons are already made of lines, so they render automatically through drawLines().
    // Optionally highlight the center point of each polygon.
    std::vector<float> verts;

    for (const auto& polygon : sketch->getPolygons()) {
        const SketchPoint* center = lutPoint(sketch, polygon.centerPointId);
        if (!center) continue;

        const float s = 0.1f;
        glm::vec3 w1 = toWorld(sketch, glm::vec2(center->pos.x - s, center->pos.y));
        glm::vec3 w2 = toWorld(sketch, glm::vec2(center->pos.x + s, center->pos.y));
        glm::vec3 w3 = toWorld(sketch, glm::vec2(center->pos.x, center->pos.y - s));
        glm::vec3 w4 = toWorld(sketch, glm::vec2(center->pos.x, center->pos.y + s));
        verts.push_back(w1.x); verts.push_back(w1.y); verts.push_back(w1.z);
        verts.push_back(w2.x); verts.push_back(w2.y); verts.push_back(w2.z);
        verts.push_back(w3.x); verts.push_back(w3.y); verts.push_back(w3.z);
        verts.push_back(w4.x); verts.push_back(w4.y); verts.push_back(w4.z);
    }

    glm::vec3 color = glm::vec3(0.8f, 0.5f, 1.0f); // purple for polygon center markers
    uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
}

void SketchRenderer::drawPoints(const Sketch* sketch, const glm::mat4& vp) {
    std::vector<float> verts;
    for (const auto& pt : sketch->getPoints()) {
        // Glyph vertices stay invisible - hundreds per word, pure noise.
        if (pt.fromText) continue;
        glm::vec3 w = toWorld(sketch, pt.pos);
        verts.push_back(w.x);
        verts.push_back(w.y);
        verts.push_back(w.z);
    }

    glm::vec3 color = glm::vec3(1.0f, 0.8f, 0.2f); // yellow dots for points
    // Marker diameter scales with the sketch line width so vertices stay
    // visible (and don't vanish on GL ES, where 1px points are invisible).
    uploadAndDraw(verts, GL_POINTS, color, vp, m_lineWidth * 2.6f);
}

void SketchRenderer::drawTrimHover(const Sketch* sketch, const SketchTool* tool,
                                   const glm::mat4& vp) {
    if (!sketch || !tool) return;
    const auto& pts = tool->getTrimHoverPoints();
    if (pts.size() < 2) return;

    // Convert the densified 2D points into a GL_LINES vertex stream (each pair
    // adjacent in `pts` becomes one segment).
    std::vector<float> verts;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        glm::vec3 a = toWorld(sketch, pts[i]);
        glm::vec3 b = toWorld(sketch, pts[i + 1]);
        verts.push_back(a.x); verts.push_back(a.y); verts.push_back(a.z);
        verts.push_back(b.x); verts.push_back(b.y); verts.push_back(b.z);
    }
    glm::vec3 red(1.0f, 0.25f, 0.25f);
    uploadAndDraw(verts, GL_LINES, red, vp, 4.0f);
}

void SketchRenderer::drawOffsetPreview(const Sketch* sketch, const SketchTool* tool,
                                       const glm::mat4& vp) {
    if (!sketch || !tool || tool->getMode() != SketchToolMode::Offset) return;

    auto stream = [&](const std::vector<std::vector<glm::vec2>>& polys,
                      const glm::vec3& colour, float width) {
        std::vector<float> verts;
        for (const auto& poly : polys) {
            for (size_t i = 0; i + 1 < poly.size(); ++i) {
                glm::vec3 a = toWorld(sketch, poly[i]);
                glm::vec3 b = toWorld(sketch, poly[i + 1]);
                verts.push_back(a.x); verts.push_back(a.y); verts.push_back(a.z);
                verts.push_back(b.x); verts.push_back(b.y); verts.push_back(b.z);
            }
        }
        if (!verts.empty()) uploadAndDraw(verts, GL_LINES, colour, vp, width);
    };

    // Pick phase: cyan over the chain the click would capture - the whole
    // connected run, so it is obvious the tool takes more than one edge.
    stream(tool->getOffsetChainHover(), glm::vec3(0.35f, 0.85f, 1.0f), 4.0f);
    // Distance phase: the result ghost, in the same orange the Mirror ghost
    // uses for "this is what you are about to create".
    stream(tool->getOffsetPreview(), glm::vec3(1.0f, 0.67f, 0.24f), 3.0f);
}

void SketchRenderer::drawMidpointDots(const Sketch* sketch, const glm::mat4& vp) {
    if (!sketch) return;

    std::vector<float> verts;
    auto pushWorld = [&](glm::vec3 w) {
        verts.push_back(w.x); verts.push_back(w.y); verts.push_back(w.z);
    };

    // Midpoint of each line. Glyph edges excluded - no dot confetti on text.
    for (const auto& line : sketch->getLines()) {
        if (line.fromText) continue;
        const SketchPoint* p1 = lutPoint(sketch, line.startPointId);
        const SketchPoint* p2 = lutPoint(sketch, line.endPointId);
        if (!p1 || !p2) continue;
        pushWorld(toWorld(sketch, 0.5f * (p1->pos + p2->pos)));
    }
    // Midpoint of each arc (angular midpoint between start and end going CCW).
    for (const auto& arc : sketch->getArcs()) {
        const SketchPoint* c = lutPoint(sketch, arc.centerPointId);
        const SketchPoint* s = lutPoint(sketch, arc.startPointId);
        const SketchPoint* e = lutPoint(sketch, arc.endPointId);
        if (!c || !s || !e) continue;
        float startA = std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
        float endA = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x);
        float sweep = endA - startA;
        const float TWO_PI = 2.0f * static_cast<float>(M_PI);
        while (sweep < 0.0f) sweep += TWO_PI;
        while (sweep >= TWO_PI) sweep -= TWO_PI;
        float midA = startA + sweep * 0.5f;
        glm::vec2 mid = c->pos + glm::vec2(std::cos(midA), std::sin(midA))
                                  * static_cast<float>(arc.radius);
        pushWorld(toWorld(sketch, mid));
    }

    // Centre dot: the host body's TRUE centre when known (thread axis -
    // mirrors the snap, which suppresses the centroid then), else the face
    // centroid (skipped for freestanding-plane sketches). Drawing BOTH put
    // two green dots 0.3mm apart on a threaded cap and the wrong one was
    // the one being aimed at.
    glm::vec2 centroid;
    if (sketch->getCenterPoint(centroid)) {
        pushWorld(toWorld(sketch, centroid));
    } else if (sketch->getSourceFaceCentroid(centroid)) {
        pushWorld(toWorld(sketch, centroid));
    }

    if (verts.empty()) return;
    glm::vec3 green(0.2f, 1.0f, 0.4f);
    uploadAndDraw(verts, GL_POINTS, green, vp, 4.0f);
}

// Live ghost of the SVG artwork at the cursor - the actual paths, scaled /
// rotated / Y-flipped exactly as SvgImport::place will stamp them, so the
// user sees the real thing before clicking (the dashed box alone made
// placement a guess for detailed art).
void SketchRenderer::drawSvgGhost(const Sketch* sketch, const SketchTool* tool,
                                  const glm::mat4& vp) {
    if (!sketch || !tool || tool->getMode() != SketchToolMode::Svg) return;
    const SvgPaths& svg = tool->getSvgPaths();
    if (svg.empty()) return;
    glm::vec2 size = svg.size();
    float rawW = (size.x > 1e-6f) ? size.x : size.y;
    if (rawW <= 1e-6f) return;

    const glm::vec2 pos = tool->getCurrentPos();
    const float scale = tool->getSvgWidth() / rawW;
    const glm::vec2 center = 0.5f * (svg.bbMin + svg.bbMax);
    const float a =
        glm::radians(static_cast<float>(tool->getTextAngle()));
    const float ca = std::cos(a), sa = std::sin(a);
    auto map = [&](glm::vec2 p) {
        glm::vec2 l = (p - center) * scale;
        l.y = -l.y;
        return pos + glm::vec2(l.x * ca - l.y * sa, l.x * sa + l.y * ca);
    };

    std::vector<float> verts;
    auto push = [&](glm::vec2 q) {
        glm::vec3 w = toWorld(sketch, map(q));
        verts.push_back(w.x); verts.push_back(w.y); verts.push_back(w.z);
    };
    for (size_t li = 0; li < svg.loops.size(); ++li) {
        const auto& L = svg.loops[li];
        for (size_t i = 0; i + 1 < L.size(); ++i) { push(L[i]); push(L[i + 1]); }
        if (svg.closed[li] && L.size() >= 3) { push(L.back()); push(L.front()); }
    }
    uploadAndDraw(verts, GL_LINES, glm::vec3(1.0f, 0.85f, 0.2f), vp, 1.5f);
}

void SketchRenderer::drawAirfoilGhost(const Sketch* sketch, const SketchTool* tool,
                                     const glm::mat4& vp) {
    if (!sketch || !tool || tool->getMode() != SketchToolMode::Airfoil) return;
    const AirfoilProfile& prof = tool->getAirfoil();
    if (prof.empty()) return;

    // Same mapping AirfoilImport::place uses: chord-normalised -> mm, rotated
    // about the LEADING EDGE, which is the cursor. Drawing it any other way
    // would put the ghost somewhere the geometry will not land.
    const glm::vec2 pos = tool->getCurrentPos();
    const float chord = tool->getAirfoilChord();
    const float a = glm::radians(static_cast<float>(tool->getTextAngle()));
    const float ca = std::cos(a), sa = std::sin(a);
    // Same anchor the stamp uses -- one function, so the ghost cannot drift
    // from where the geometry actually lands.
    const glm::vec2 a0 = AirfoilImport::anchorPoint(prof, tool->getAirfoilAnchor());
    auto map = [&](glm::vec2 n) {
        const glm::vec2 l((n.x - a0.x) * chord, (n.y - a0.y) * chord);
        return pos + glm::vec2(l.x * ca - l.y * sa, l.x * sa + l.y * ca);
    };

    std::vector<float> verts;
    auto push = [&](glm::vec2 q) {
        const glm::vec3 w = toWorld(sketch, map(q));
        verts.push_back(w.x); verts.push_back(w.y); verts.push_back(w.z);
    };
    auto strip = [&](const std::vector<glm::vec2>& surf) {
        for (std::size_t i = 0; i + 1 < surf.size(); ++i) { push(surf[i]); push(surf[i + 1]); }
    };
    strip(prof.upper);
    strip(prof.lower);
    // Blunt sections get their trailing-edge closing segment drawn too, so the
    // ghost is the outline that will actually be created.
    if (prof.bluntTrailingEdge && !prof.upper.empty() && !prof.lower.empty()) {
        push(prof.upper.back());
        push(prof.lower.back());
    }
    uploadAndDraw(verts, GL_LINES, glm::vec3(1.0f, 0.85f, 0.2f), vp, 1.5f);
}

void SketchRenderer::drawPreview(const Sketch* sketch, const SketchTool* tool,
                                  const glm::mat4& vp) {
    if (!tool->hasPreview() || !sketch) return;

    glm::vec2 start = tool->getPreviewStart();
    glm::vec2 end = tool->getPreviewEnd();
    SketchToolMode mode = tool->getPreviewType();

    auto pw = [&](glm::vec2 p) -> glm::vec3 { return toWorld(sketch, p); };
    auto pushPt = [](std::vector<float>& v, glm::vec3 p) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
    };

    std::vector<float> verts;
    // Bright yellow - distinct from both committed sketch lines (cobalt) and
    // the dimension overlay (light grey-white). Matches the existing "yellow =
    // active / being placed" convention used elsewhere (point markers).
    glm::vec3 color(1.0f, 0.85f, 0.2f);

    if (mode == SketchToolMode::Spline) {
        // Live smooth preview: the curve through every placed control
        // point plus the cursor, interpolated the same way the committed
        // spline (and its extrudable geometry) will be.
        std::vector<glm::vec2> ctrl;
        for (int id : tool->splinePointsInProgress())
            if (const SketchPoint* p = lutPoint(sketch, id))
                ctrl.push_back(p->pos);
        ctrl.push_back(end);
        std::vector<glm::vec2> pts = Sketch::interpolate2D(ctrl, 12);
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            pushPt(verts, pw(pts[i]));
            pushPt(verts, pw(pts[i + 1]));
        }
        uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
    } else if (mode == SketchToolMode::Line) {
        pushPt(verts, pw(start));
        pushPt(verts, pw(end));
        uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
    } else if (mode == SketchToolMode::Circle) {
        float radius = glm::length(end - start);
        const int segs = 64;
        for (int i = 0; i < segs; i++) {
            float a1 = 2.0f * (float)M_PI * i / segs;
            float a2 = 2.0f * (float)M_PI * (i + 1) / segs;
            pushPt(verts, pw(glm::vec2(start.x + radius*std::cos(a1), start.y + radius*std::sin(a1))));
            pushPt(verts, pw(glm::vec2(start.x + radius*std::cos(a2), start.y + radius*std::sin(a2))));
        }
        uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
    } else if (mode == SketchToolMode::Rectangle) {
        glm::vec2 corners[4] = { start, {end.x, start.y}, end, {start.x, end.y} };
        for (int i = 0; i < 4; i++) {
            pushPt(verts, pw(corners[i]));
            pushPt(verts, pw(corners[(i+1)%4]));
        }
        uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
    } else if (mode == SketchToolMode::Polygon) {
        glm::vec2 delta = end - start;
        float radius = glm::length(delta);
        const int sides = tool ? tool->getPolygonSides() : 6;
        // Rotation = direction from centre toward cursor. Matches the actual
        // placement so vertex 0 sits exactly on the (grid-snapped) cursor.
        float rotation = (radius > 1e-6f) ? std::atan2(delta.y, delta.x) : 0.0f;
        for (int i = 0; i < sides; i++) {
            float a1 = rotation + 2.0f * (float)M_PI * i / sides;
            float a2 = rotation + 2.0f * (float)M_PI * (i + 1) / sides;
            pushPt(verts, pw(glm::vec2(start.x + radius*std::cos(a1), start.y + radius*std::sin(a1))));
            pushPt(verts, pw(glm::vec2(start.x + radius*std::cos(a2), start.y + radius*std::sin(a2))));
        }
        uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
    } else if (mode == SketchToolMode::Arc) {
        // Arc needs 3 clicks. After click 1: the chord (start->cursor) PLUS a
        // curved hint so it doesn't read as a plain line. After click 2: the
        // live arc through (start, cursor, second).
        int clicks = tool->getClickCount();
        // Append the circumcircle arc s -> through m -> e to `v` (chord fallback
        // if the three points are collinear).
        auto pushArc = [&](std::vector<float>& v, glm::vec2 s, glm::vec2 m, glm::vec2 e) {
            float ax = s.x, ay = s.y, bx = m.x, by = m.y, cx = e.x, cy = e.y;
            float D = 2.0f * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
            if (std::abs(D) < 1e-10f) { pushPt(v, pw(s)); pushPt(v, pw(e)); return; }
            float ux = ((ax*ax+ay*ay)*(by-cy) + (bx*bx+by*by)*(cy-ay) + (cx*cx+cy*cy)*(ay-by)) / D;
            float uy = ((ax*ax+ay*ay)*(cx-bx) + (bx*bx+by*by)*(ax-cx) + (cx*cx+cy*cy)*(bx-ax)) / D;
            glm::vec2 c(ux, uy);
            float r = glm::length(s - c);
            float sA = std::atan2(s.y - c.y, s.x - c.x);
            float mA = std::atan2(m.y - c.y, m.x - c.x);
            float eA = std::atan2(e.y - c.y, e.x - c.x);
            auto norm = [](float a){ const float T = 2.0f * (float)M_PI;
                                     while (a < 0) a += T; while (a >= T) a -= T; return a; };
            float ds = norm(eA - sA), dm = norm(mA - sA);
            float sweep = (dm < ds) ? ds : (ds - 2.0f * (float)M_PI);
            const int segs = 48;
            for (int i = 0; i < segs; ++i) {
                float a1 = sA + sweep * float(i) / segs;
                float a2 = sA + sweep * float(i + 1) / segs;
                pushPt(v, pw(glm::vec2(c.x + r*std::cos(a1), c.y + r*std::sin(a1))));
                pushPt(v, pw(glm::vec2(c.x + r*std::cos(a2), c.y + r*std::sin(a2))));
            }
        };
        if (clicks == 1) {
            // Solid yellow chord: the endpoints + the live chord-length readout.
            pushPt(verts, pw(start));
            pushPt(verts, pw(end));
            uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
            // Curved cyan hint bulging off the chord - signals "this is an arc,
            // not a line." It's only a placeholder default; the next tap sets the
            // real bulge. Sagitta = 28% of the chord, perpendicular to it.
            glm::vec2 chord = end - start;
            float clen = glm::length(chord);
            if (clen > 1e-4f) {
                glm::vec2 d = chord / clen;
                glm::vec2 nrm(-d.y, d.x);
                glm::vec2 hintMid = 0.5f * (start + end) + nrm * (clen * 0.28f);
                std::vector<float> hint;
                pushArc(hint, start, hintMid, end);
                uploadAndDraw(hint, GL_LINES, glm::vec3(0.35f, 0.80f, 0.85f), vp, 1.2f);
            }
        } else if (clicks == 2) {
            // start, cursor (= the bulge being chosen), second (= committed end).
            std::vector<float> v;
            pushArc(v, start, end, tool->getSecondClick());
            uploadAndDraw(v, GL_LINES, color, vp, 1.5f);
        }
    }
}

void SketchRenderer::drawConstraints(const Sketch* sketch, const SketchSolver* solver,
                                      const glm::mat4& vp) {
    if (!solver) return;

    const auto& constraints = solver->getConstraints();
    if (constraints.empty()) return;

    const float markerSize = 0.08f;

    // Helper: convert 2D sketch coord to 3D with slight offset along normal
    auto tw = [&](float x, float y) -> glm::vec3 {
        return toWorld(sketch, glm::vec2(x, y));
    };
    auto pushV = [](std::vector<float>& v, glm::vec3 p) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
    };
    // Compatibility shim: replaces the old "push x, push y, push z" pattern
    // by transforming (x,y) through the sketch plane
    auto push2d = [&](std::vector<float>& v, float x, float y) {
        glm::vec3 w = tw(x, y);
        v.push_back(w.x); v.push_back(w.y); v.push_back(w.z);
    };

    for (const auto& c : constraints) {
        glm::vec3 color = c.isSatisfied ? glm::vec3(0.0f, 0.85f, 0.3f)   // green for satisfied
                                        : glm::vec3(0.95f, 0.15f, 0.15f); // red for unsatisfied

        std::vector<float> verts;

        if (c.type == ConstraintType::Horizontal || c.type == ConstraintType::Vertical) {
            // Draw a small marker at the midpoint of the constrained line
            // entityA refers to the line id
            glm::vec2 midpoint(0.0f);
            bool found = false;
            for (const auto& line : sketch->getLines()) {
                if (line.id == c.entityA) {
                    const SketchPoint* p1 = lutPoint(sketch, line.startPointId);
                    const SketchPoint* p2 = lutPoint(sketch, line.endPointId);
                    if (p1 && p2) {
                        midpoint = (p1->pos + p2->pos) * 0.5f;
                        found = true;
                    }
                    break;
                }
            }
            if (!found) continue;

            // Offset the marker slightly above the line
            glm::vec2 offset(0.0f, markerSize * 1.5f);

            if (c.type == ConstraintType::Horizontal) {
                // Draw "H" using 3 line segments
                float s = markerSize * 0.5f;
                glm::vec2 base = midpoint + offset;
                // Left vertical
                push2d(verts, base.x - s, base.y - s);
                push2d(verts, base.x - s, base.y + s);
                // Right vertical
                push2d(verts, base.x + s, base.y - s);
                push2d(verts, base.x + s, base.y + s);
                // Horizontal bar
                push2d(verts, base.x - s, base.y);
                push2d(verts, base.x + s, base.y);
            } else {
                // Draw "V" using 2 line segments
                float s = markerSize * 0.5f;
                glm::vec2 base = midpoint + offset;
                // Left arm of V
                push2d(verts, base.x - s, base.y + s);
                push2d(verts, base.x, base.y - s);
                // Right arm of V
                push2d(verts, base.x + s, base.y + s);
                push2d(verts, base.x, base.y - s);
            }

            uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);

        } else if (c.type == ConstraintType::Coincident) {
            // Draw a larger yellow/green dot at the coincident point
            // entityA is one of the coincident points
            const SketchPoint* pt = lutPoint(sketch, c.entityA);
            if (!pt) continue;

            // Draw a small diamond shape around the point
            float s = markerSize * 0.6f;
            glm::vec2 p = pt->pos;
            push2d(verts, p.x, p.y + s);
            push2d(verts, p.x + s, p.y);

            push2d(verts, p.x + s, p.y);
            push2d(verts, p.x, p.y - s);

            push2d(verts, p.x, p.y - s);
            push2d(verts, p.x - s, p.y);

            push2d(verts, p.x - s, p.y);
            push2d(verts, p.x, p.y + s);

            // Use yellow for coincident markers
            glm::vec3 coincColor = c.isSatisfied ? glm::vec3(1.0f, 0.9f, 0.2f)
                                                 : glm::vec3(0.95f, 0.15f, 0.15f);
            uploadAndDraw(verts, GL_LINES, coincColor, vp, 2.0f);

        } else if (c.type == ConstraintType::Distance) {
            // Draw dimension line between two points with perpendicular end caps
            const SketchPoint* p1 = lutPoint(sketch, c.entityA);
            const SketchPoint* p2 = lutPoint(sketch, c.entityB);
            if (!p1 || !p2) continue;

            glm::vec2 a = p1->pos;
            glm::vec2 b = p2->pos;
            glm::vec2 dir = b - a;
            float len = glm::length(dir);
            if (len < 1e-6f) continue;

            glm::vec2 unit = dir / len;
            glm::vec2 perp(-unit.y, unit.x);

            // Offset the dimension line
            float offsetDist = markerSize * 3.0f;
            glm::vec2 a_off = a + perp * offsetDist;
            glm::vec2 b_off = b + perp * offsetDist;

            // Main dimension line
            push2d(verts, a_off.x, a_off.y);
            push2d(verts, b_off.x, b_off.y);

            // Extension lines from points to dimension line
            float capLen = markerSize * 0.8f;
            glm::vec2 a_ext = a + perp * (offsetDist - capLen);
            glm::vec2 b_ext = b + perp * (offsetDist - capLen);
            glm::vec2 a_ext2 = a + perp * (offsetDist + capLen);
            glm::vec2 b_ext2 = b + perp * (offsetDist + capLen);

            // Left extension
            push2d(verts, a_ext.x, a_ext.y);
            push2d(verts, a_ext2.x, a_ext2.y);
            // Right extension
            push2d(verts, b_ext.x, b_ext.y);
            push2d(verts, b_ext2.x, b_ext2.y);

            // Leader lines from actual points to dimension line
            push2d(verts, a.x, a.y);
            push2d(verts, a_off.x, a_off.y);

            push2d(verts, b.x, b.y);
            push2d(verts, b_off.x, b_off.y);

            uploadAndDraw(verts, GL_LINES, color, vp, 1.0f);

        } else if (c.type == ConstraintType::Fixed) {
            // Draw an X marker at the fixed point
            const SketchPoint* pt = lutPoint(sketch, c.entityA);
            if (!pt) continue;

            float s = markerSize * 0.6f;
            glm::vec2 p = pt->pos;
            // Diagonal 1
            push2d(verts, p.x - s, p.y - s);
            push2d(verts, p.x + s, p.y + s);
            // Diagonal 2
            push2d(verts, p.x - s, p.y + s);
            push2d(verts, p.x + s, p.y - s);

            // Use a specific red-orange for fixed constraints
            glm::vec3 fixedColor = c.isSatisfied ? glm::vec3(0.9f, 0.4f, 0.1f)
                                                 : glm::vec3(0.95f, 0.15f, 0.15f);
            uploadAndDraw(verts, GL_LINES, fixedColor, vp, 2.0f);

        } else if (c.type == ConstraintType::Radius) {
            // Draw a small circle indicator near the constrained circle/arc center
            // entityA refers to the circle/arc id; look for matching circle center
            glm::vec2 center(0.0f);
            bool found = false;
            for (const auto& circle : sketch->getCircles()) {
                if (circle.id == c.entityA) {
                    const SketchPoint* cp = lutPoint(sketch, circle.centerPointId);
                    if (cp) {
                        center = cp->pos;
                        found = true;
                    }
                    break;
                }
            }
            if (!found) {
                for (const auto& arc : sketch->getArcs()) {
                    if (arc.id == c.entityA) {
                        const SketchPoint* cp = lutPoint(sketch, arc.centerPointId);
                        if (cp) {
                            center = cp->pos;
                            found = true;
                        }
                        break;
                    }
                }
            }
            if (!found) continue;

            // Draw small "R" indicator: vertical line + small bump
            float s = markerSize * 0.5f;
            glm::vec2 base = center + glm::vec2(markerSize * 1.5f, markerSize * 1.5f);
            // Vertical stroke
            push2d(verts, base.x, base.y - s);
            push2d(verts, base.x, base.y + s);
            // Top horizontal
            push2d(verts, base.x, base.y + s);
            push2d(verts, base.x + s, base.y + s);
            // Diagonal kick
            push2d(verts, base.x, base.y);
            push2d(verts, base.x + s, base.y - s);

            uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);

        } else if (c.type == ConstraintType::Equal) {
            // Two short "=" tick pairs near the midpoint of each constrained line.
            const SketchLine* lA = nullptr;
            const SketchLine* lB = nullptr;
            for (const auto& line : sketch->getLines()) {
                if (line.id == c.entityA) lA = &line;
                if (line.id == c.entityB) lB = &line;
            }
            if (!lA || !lB) continue;
            auto drawTicks = [&](const SketchLine* l) {
                const SketchPoint* p1 = lutPoint(sketch, l->startPointId);
                const SketchPoint* p2 = lutPoint(sketch, l->endPointId);
                if (!p1 || !p2) return;
                glm::vec2 mid = (p1->pos + p2->pos) * 0.5f;
                glm::vec2 dir = p2->pos - p1->pos;
                float ll = glm::length(dir);
                if (ll < 1e-6f) return;
                dir /= ll;
                glm::vec2 perp(-dir.y, dir.x);
                float s = markerSize * 0.4f;
                // Two short ticks perpendicular to the line, separated along it.
                glm::vec2 t1 = mid - dir * (s * 0.4f);
                glm::vec2 t2 = mid + dir * (s * 0.4f);
                push2d(verts, t1.x - perp.x * s, t1.y - perp.y * s);
                push2d(verts, t1.x + perp.x * s, t1.y + perp.y * s);
                push2d(verts, t2.x - perp.x * s, t2.y - perp.y * s);
                push2d(verts, t2.x + perp.x * s, t2.y + perp.y * s);
            };
            drawTicks(lA);
            drawTicks(lB);
            uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);

        } else if (c.type == ConstraintType::Parallel || c.type == ConstraintType::Perpendicular) {
            // Draw a small marker at the midpoint of the first line
            glm::vec2 midpoint(0.0f);
            bool found = false;
            for (const auto& line : sketch->getLines()) {
                if (line.id == c.entityA) {
                    const SketchPoint* p1 = lutPoint(sketch, line.startPointId);
                    const SketchPoint* p2 = lutPoint(sketch, line.endPointId);
                    if (p1 && p2) {
                        midpoint = (p1->pos + p2->pos) * 0.5f;
                        found = true;
                    }
                    break;
                }
            }
            if (!found) continue;

            float s = markerSize * 0.5f;
            glm::vec2 base = midpoint + glm::vec2(0.0f, markerSize * 1.5f);

            if (c.type == ConstraintType::Parallel) {
                // Two parallel short lines
                push2d(verts, base.x - s * 0.3f, base.y - s);
                push2d(verts, base.x - s * 0.3f, base.y + s);
                push2d(verts, base.x + s * 0.3f, base.y - s);
                push2d(verts, base.x + s * 0.3f, base.y + s);
            } else {
                // Small right angle symbol
                push2d(verts, base.x - s, base.y - s);
                push2d(verts, base.x - s, base.y + s);
                push2d(verts, base.x - s, base.y - s);
                push2d(verts, base.x + s, base.y - s);
            }

            uploadAndDraw(verts, GL_LINES, color, vp, 1.5f);
        }
    }

    // (Removed: a solver-state indicator used to be drawn here as a small
    // square at the fixed sketch coordinate (-5, +5), coloured green / cobalt /
    // red for Fully / Under / Over-constrained. Cobalt is also the sketch LINE
    // colour, so on an under-constrained sketch - i.e. most of the time while
    // drawing - it read as a stray blue dot sitting in the model near the
    // origin, with nothing to explain it. Steve hit it as "a random little blue
    // dot" and it had been mis-filed in my notes as orphan vertices for two
    // months. The same three states are already reported in words by the sketch
    // toolbar's status badge (Toolbar::setSketchSolverState), so this was a
    // second, less legible channel for information the user already had.)
}

void SketchRenderer::renderRegionBoundary(const Sketch* sketch, int regionIndex,
                                          const glm::vec3& color, float lineWidth,
                                          const glm::mat4& view, const glm::mat4& projection) {
    if (!sketch || !m_program) return;
    auto regions = sketch->buildRegions();
    if (regionIndex < 0 || regionIndex >= static_cast<int>(regions.size())) return;

    glm::mat4 vp = projection * view;
    const auto& region = regions[regionIndex];

    auto wireToSegments = [&](const TopoDS_Wire& wire, std::vector<float>& verts) {
        for (BRepTools_WireExplorer ex(wire); ex.More(); ex.Next()) {
            TopoDS_Edge edge = TopoDS::Edge(ex.Current());
            double f, l;
            Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, f, l);
            if (curve.IsNull()) continue;
            const int samples = 64;
            gp_Pnt prev;
            for (int i = 0; i <= samples; ++i) {
                double t = f + (l - f) * (double(i) / samples);
                gp_Pnt p;
                curve->D0(t, p);
                if (i > 0) {
                    verts.push_back(static_cast<float>(prev.X()));
                    verts.push_back(static_cast<float>(prev.Y()));
                    verts.push_back(static_cast<float>(prev.Z()));
                    verts.push_back(static_cast<float>(p.X()));
                    verts.push_back(static_cast<float>(p.Y()));
                    verts.push_back(static_cast<float>(p.Z()));
                }
                prev = p;
            }
        }
    };

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    std::vector<float> verts;
    wireToSegments(region.outerWire, verts);
    for (const auto& h : region.holeWires) wireToSegments(h, verts);
    uploadAndDraw(verts, GL_LINES, color, vp, lineWidth);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

void SketchRenderer::renderRegionFill(const Sketch* sketch, int regionIndex,
                                      const glm::vec3& color, float alpha,
                                      const glm::mat4& view,
                                      const glm::mat4& projection) {
    if (!sketch || !m_program) return;
    auto regions = sketch->buildRegions();
    if (regionIndex < 0 || regionIndex >= static_cast<int>(regions.size()))
        return;
    const TopoDS_Face& face = regions[regionIndex].face;
    if (face.IsNull()) return;

    // Triangulate (cheap for planar faces; the mesher caches on the shape,
    // and buildRegions itself is cached, so repeated frames cost ~nothing).
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) {
        try {
            BRepMesh_IncrementalMesh mesher(face, materializr::meshParams(0.2, 0.5, false));
            tri = BRep_Tool::Triangulation(face, loc);
        } catch (...) {}
        if (tri.IsNull()) return;
    }

    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(tri->NbTriangles()) * 9);
    const gp_Trsf& trsf = loc.Transformation();
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
        Poly_Triangle t = tri->Triangle(i);
        int a, b, c;
        t.Get(a, b, c);
        for (int n : {a, b, c}) {
            gp_Pnt p = tri->Node(n).Transformed(trsf);
            verts.push_back(static_cast<float>(p.X()));
            verts.push_back(static_cast<float>(p.Y()));
            verts.push_back(static_cast<float>(p.Z()));
        }
    }
    if (verts.empty()) return;

    glm::mat4 vp = projection * view;
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform3fv(m_locColor, 1, glm::value_ptr(color));
    glUniform1f(m_locAlpha, alpha);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(),
                 GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<int>(verts.size() / 3));
    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

void SketchRenderer::renderSketchHighlight(const Sketch* sketch,
                                           const glm::vec3& color, float lineWidth,
                                           const glm::mat4& view, const glm::mat4& projection) {
    if (!sketch || !m_program) return;
    glm::mat4 vp = projection * view;

    // Stuff every primitive into one buffer so a single GL draw covers the
    // whole sketch - line endpoints, sampled circles / arcs, spline control
    // polylines, polygon edges. Polygons share their underlying SketchLines
    // with getLines(), so we don't iterate getPolygons() separately.
    std::vector<float> verts;
    auto push = [&](glm::vec3 a, glm::vec3 b) {
        verts.push_back(a.x); verts.push_back(a.y); verts.push_back(a.z);
        verts.push_back(b.x); verts.push_back(b.y); verts.push_back(b.z);
    };

    for (const auto& ln : sketch->getLines()) {
        const SketchPoint* p1 = lutPoint(sketch, ln.startPointId);
        const SketchPoint* p2 = lutPoint(sketch, ln.endPointId);
        if (!p1 || !p2) continue;
        push(toWorld(sketch, p1->pos), toWorld(sketch, p2->pos));
    }

    const int circSegs = 64;
    for (const auto& c : sketch->getCircles()) {
        const SketchPoint* ctr = lutPoint(sketch, c.centerPointId);
        if (!ctr) continue;
        float r = static_cast<float>(c.radius);
        for (int i = 0; i < circSegs; ++i) {
            float a1 = 2.0f * static_cast<float>(M_PI) * i / circSegs;
            float a2 = 2.0f * static_cast<float>(M_PI) * (i + 1) / circSegs;
            glm::vec2 s1(ctr->pos.x + r * std::cos(a1), ctr->pos.y + r * std::sin(a1));
            glm::vec2 s2(ctr->pos.x + r * std::cos(a2), ctr->pos.y + r * std::sin(a2));
            push(toWorld(sketch, s1), toWorld(sketch, s2));
        }
    }

    const int arcSegs = 32;
    for (const auto& a : sketch->getArcs()) {
        const SketchPoint* ctr = lutPoint(sketch, a.centerPointId);
        const SketchPoint* s   = lutPoint(sketch, a.startPointId);
        const SketchPoint* e   = lutPoint(sketch, a.endPointId);
        if (!ctr || !s || !e) continue;
        float a0 = std::atan2(s->pos.y - ctr->pos.y, s->pos.x - ctr->pos.x);
        float a1 = std::atan2(e->pos.y - ctr->pos.y, e->pos.x - ctr->pos.x);
        if (a1 < a0) a1 += 2.0f * static_cast<float>(M_PI);
        float r = static_cast<float>(a.radius);
        for (int i = 0; i < arcSegs; ++i) {
            float t1 = static_cast<float>(i) / arcSegs;
            float t2 = static_cast<float>(i + 1) / arcSegs;
            float ang1 = a0 + t1 * (a1 - a0);
            float ang2 = a0 + t2 * (a1 - a0);
            glm::vec2 q1(ctr->pos.x + r * std::cos(ang1), ctr->pos.y + r * std::sin(ang1));
            glm::vec2 q2(ctr->pos.x + r * std::cos(ang2), ctr->pos.y + r * std::sin(ang2));
            push(toWorld(sketch, q1), toWorld(sketch, q2));
        }
    }

    for (const auto& sp : sketch->getSplines()) {
        for (size_t i = 0; i + 1 < sp.controlPointIds.size(); ++i) {
            const SketchPoint* p1 = lutPoint(sketch, sp.controlPointIds[i]);
            const SketchPoint* p2 = lutPoint(sketch, sp.controlPointIds[i + 1]);
            if (!p1 || !p2) continue;
            push(toWorld(sketch, p1->pos), toWorld(sketch, p2->pos));
        }
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    uploadAndDraw(verts, GL_LINES, color, vp, lineWidth);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

void SketchRenderer::renderElementsHighlight(const Sketch* sketch,
                                             const std::set<int>& lineIds,
                                             const std::set<int>& circleIds,
                                             const std::set<int>& arcIds,
                                             const glm::vec3& color, float lineWidth,
                                             const glm::mat4& view,
                                             const glm::mat4& projection) {
    if (!sketch || !m_program) return;
    if (lineIds.empty() && circleIds.empty() && arcIds.empty()) return;
    glm::mat4 vp = projection * view;

    std::vector<float> verts;
    auto push = [&](glm::vec3 a, glm::vec3 b) {
        verts.push_back(a.x); verts.push_back(a.y); verts.push_back(a.z);
        verts.push_back(b.x); verts.push_back(b.y); verts.push_back(b.z);
    };

    for (const auto& ln : sketch->getLines()) {
        if (!lineIds.count(ln.id)) continue;
        const SketchPoint* p1 = lutPoint(sketch, ln.startPointId);
        const SketchPoint* p2 = lutPoint(sketch, ln.endPointId);
        if (!p1 || !p2) continue;
        push(toWorld(sketch, p1->pos), toWorld(sketch, p2->pos));
    }

    const int circSegs = 64;
    for (const auto& c : sketch->getCircles()) {
        if (!circleIds.count(c.id)) continue;
        const SketchPoint* ctr = lutPoint(sketch, c.centerPointId);
        if (!ctr) continue;
        float r = static_cast<float>(c.radius);
        for (int i = 0; i < circSegs; ++i) {
            float a1 = 2.0f * static_cast<float>(M_PI) * i / circSegs;
            float a2 = 2.0f * static_cast<float>(M_PI) * (i + 1) / circSegs;
            glm::vec2 s1(ctr->pos.x + r * std::cos(a1), ctr->pos.y + r * std::sin(a1));
            glm::vec2 s2(ctr->pos.x + r * std::cos(a2), ctr->pos.y + r * std::sin(a2));
            push(toWorld(sketch, s1), toWorld(sketch, s2));
        }
    }

    const int arcSegs = 32;
    for (const auto& a : sketch->getArcs()) {
        if (!arcIds.count(a.id)) continue;
        const SketchPoint* ctr = lutPoint(sketch, a.centerPointId);
        const SketchPoint* s   = lutPoint(sketch, a.startPointId);
        const SketchPoint* e   = lutPoint(sketch, a.endPointId);
        if (!ctr || !s || !e) continue;
        float a0 = std::atan2(s->pos.y - ctr->pos.y, s->pos.x - ctr->pos.x);
        float a1 = std::atan2(e->pos.y - ctr->pos.y, e->pos.x - ctr->pos.x);
        if (a1 < a0) a1 += 2.0f * static_cast<float>(M_PI);
        float r = static_cast<float>(a.radius);
        for (int i = 0; i < arcSegs; ++i) {
            float t1 = static_cast<float>(i) / arcSegs;
            float t2 = static_cast<float>(i + 1) / arcSegs;
            float ang1 = a0 + t1 * (a1 - a0);
            float ang2 = a0 + t2 * (a1 - a0);
            glm::vec2 q1(ctr->pos.x + r * std::cos(ang1), ctr->pos.y + r * std::sin(ang1));
            glm::vec2 q2(ctr->pos.x + r * std::cos(ang2), ctr->pos.y + r * std::sin(ang2));
            push(toWorld(sketch, q1), toWorld(sketch, q2));
        }
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    uploadAndDraw(verts, GL_LINES, color, vp, lineWidth);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

void SketchRenderer::renderFaceGrid(const Sketch* sketch, float faceExtent, float gridStep,
                                    const glm::mat4& view, const glm::mat4& projection) {
    if (!sketch || !m_program) return;
    if (faceExtent <= 0.0f || gridStep <= 0.0f) return;

    glm::mat4 vp = projection * view;

    // Centre the grid over the host face (its centroid), but QUANTIZED onto
    // the snap lattice: snap-to-grid rounds sketch coordinates to multiples
    // of the step from the PLANE ORIGIN, so the drawn lines must sit on that
    // same lattice. Anchoring at the raw centroid drew a grid offset by
    // (centroid mod step) from where clicks actually land - glaring on a
    // threaded rod's cap, whose centroid shifts off-axis (the groove-runout
    // bite makes the face asymmetric).
    glm::vec2 c{0.0f};
    sketch->getSourceFaceCentroid(c);
    c.x = std::round(c.x / gridStep) * gridStep;
    c.y = std::round(c.y / gridStep) * gridStep;

    // Split lines into minor and every-10th major so the major lines read clearly.
    std::vector<float> minorVerts, majorVerts;
    int steps = static_cast<int>(std::floor(faceExtent / gridStep));
    auto pushLine = [](std::vector<float>& v, glm::vec3 a, glm::vec3 b) {
        v.push_back(a.x); v.push_back(a.y); v.push_back(a.z);
        v.push_back(b.x); v.push_back(b.y); v.push_back(b.z);
    };
    for (int i = -steps; i <= steps; ++i) {
        float t = i * gridStep;
        std::vector<float>& dst = (i % 10 == 0) ? majorVerts : minorVerts;
        pushLine(dst, toWorld(sketch, glm::vec2(c.x + t, c.y - faceExtent)),
                      toWorld(sketch, glm::vec2(c.x + t, c.y + faceExtent)));
        pushLine(dst, toWorld(sketch, glm::vec2(c.x - faceExtent, c.y + t)),
                      toWorld(sketch, glm::vec2(c.x + faceExtent, c.y + t)));
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Faint minor lines, then brighter/thicker major (every 10th) on top.
    uploadAndDraw(minorVerts, GL_LINES, glm::vec3(0.33f, 0.37f, 0.42f), vp, 0.7f);
    uploadAndDraw(majorVerts, GL_LINES, glm::vec3(0.62f, 0.68f, 0.78f), vp, 1.6f);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

bool SketchRenderer::compileShader(unsigned int& shader, unsigned int type, const char* source) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "SketchRenderer shader error: %s\n", log);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

} // namespace materializr
