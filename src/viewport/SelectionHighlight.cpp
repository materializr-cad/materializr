#include "gl_common.h"
#include "SelectionHighlight.h"
#include "core/SelectionManager.h"
#include "core/Document.h"

#include <glm/gtc/type_ptr.hpp>

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <BRepAdaptor_Curve.hxx>
#include <GCPnts_TangentialDeflection.hxx>

#include <vector>
#include <cstdio>
#include <algorithm>

namespace materializr {

static const char* s_vertSrc = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
uniform mat4 u_mvp;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

static const char* s_fragSrc = R"(
#version 330 core
uniform vec4 u_color;
out vec4 fragColor;
void main() {
    fragColor = u_color;
}
)";

// Expands each GL_LINES segment into a screen-space quad `u_halfWidth` pixels
// wide. glLineWidth > 1 is not guaranteed in the 3.3 core profile (the spec
// only requires a [1,1] aliased range), so wide selection lines have to be
// built as geometry. Offsets are applied in NDC then scaled back by w so the
// quad keeps a constant pixel width under perspective.
static const char* s_lineGeomSrc = R"(
#version 330 core
layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;
uniform vec2  u_viewport;   // framebuffer size in pixels
uniform float u_halfWidth;  // half the line width, in pixels
void main() {
    vec4 p0 = gl_in[0].gl_Position;
    vec4 p1 = gl_in[1].gl_Position;
    if (p0.w <= 0.0 || p1.w <= 0.0) return; // behind the camera

    vec2 ndc0 = p0.xy / p0.w;
    vec2 ndc1 = p1.xy / p1.w;

    // Direction in pixel space, then a perpendicular pixel offset.
    vec2 dir = (ndc1 - ndc0) * u_viewport;
    if (dot(dir, dir) < 1e-12) return;
    dir = normalize(dir);
    vec2 normalPx = vec2(-dir.y, dir.x) * u_halfWidth;
    vec2 ndcOffset = normalPx * 2.0 / u_viewport; // pixels -> NDC

    gl_Position = vec4((ndc0 + ndcOffset) * p0.w, p0.z, p0.w); EmitVertex();
    gl_Position = vec4((ndc0 - ndcOffset) * p0.w, p0.z, p0.w); EmitVertex();
    gl_Position = vec4((ndc1 + ndcOffset) * p1.w, p1.z, p1.w); EmitVertex();
    gl_Position = vec4((ndc1 - ndcOffset) * p1.w, p1.z, p1.w); EmitVertex();
    EndPrimitive();
}
)";

SelectionHighlight::SelectionHighlight() {}

void SelectionHighlight::setLineWidth(float w) {
    m_edgeLineWidth = std::clamp(w, 1.0f, 10.0f);
}

SelectionHighlight::~SelectionHighlight() {
    clearCaches(); // frees each entry's persistent VAO/VBO
    if (m_program) glDeleteProgram(m_program);
    if (m_lineProgram) glDeleteProgram(m_lineProgram);
}

bool SelectionHighlight::initialize() {
    unsigned int vert = 0, frag = 0;
    if (!compileShader(vert, GL_VERTEX_SHADER, s_vertSrc)) return false;
    if (!compileShader(frag, GL_FRAGMENT_SHADER, s_fragSrc)) {
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
    if (!success) return false;

    m_locMVP = glGetUniformLocation(m_program, "u_mvp");
    m_locColor = glGetUniformLocation(m_program, "u_color");

    // Second program with a geometry shader for thick lines. GL ES 3.0 has no
    // geometry shaders, so on Android m_lineProgram stays 0 and drawThickLines()
    // becomes a no-op - selected faces still highlight; thick edge outlines are
    // skipped (TODO: emulate with an instanced-quad expansion in the touch pass).
#if !defined(MZ_GLES)
    unsigned int lvert = 0, lgeom = 0, lfrag = 0;
    if (!compileShader(lvert, GL_VERTEX_SHADER, s_vertSrc)) return false;
    if (!compileShader(lgeom, GL_GEOMETRY_SHADER, s_lineGeomSrc)) {
        glDeleteShader(lvert);
        return false;
    }
    if (!compileShader(lfrag, GL_FRAGMENT_SHADER, s_fragSrc)) {
        glDeleteShader(lvert);
        glDeleteShader(lgeom);
        return false;
    }
    m_lineProgram = glCreateProgram();
    glAttachShader(m_lineProgram, lvert);
    glAttachShader(m_lineProgram, lgeom);
    glAttachShader(m_lineProgram, lfrag);
    glLinkProgram(m_lineProgram);
    glDeleteShader(lvert);
    glDeleteShader(lgeom);
    glDeleteShader(lfrag);

    glGetProgramiv(m_lineProgram, GL_LINK_STATUS, &success);
    if (!success) return false;

    m_locLineMVP   = glGetUniformLocation(m_lineProgram, "u_mvp");
    m_locLineColor = glGetUniformLocation(m_lineProgram, "u_color");
    m_locViewport  = glGetUniformLocation(m_lineProgram, "u_viewport");
    m_locHalfWidth = glGetUniformLocation(m_lineProgram, "u_halfWidth");
#endif

    // No shared scratch VAO/VBO any more - each cache entry owns its own
    // persistent buffer (see CacheEntry), uploaded once on build.
    return true;
}

void SelectionHighlight::render(const SelectionManager& sel, const Document& doc,
                                 const glm::mat4& view, const glm::mat4& projection) {
    if (!sel.hasSelection() || !m_program) return;

    glm::mat4 vp = projection * view;
    glm::vec3 faceColor(0.25f, 0.55f, 1.0f);
    glm::vec3 edgeColor(0.2f, 1.0f, 0.4f);
    glm::vec3 bodyOutlineColor(0.3f, 0.6f, 1.0f);

    for (const auto& entry : sel.getSelection()) {
        // Body outlines use the LIVE document shape (not entry.shape).
        // entry.shape is captured at selection time; after a rotate /
        // transform, it's stale and the outline would draw in the
        // pre-transform position while the body itself is elsewhere
        // ("bodies disappear, wireframe stays" report). Looking up
        // doc.getBody(id) gives us the current pose, and the per-body
        // TShape cache below still avoids re-tessellating unless the
        // topology actually changed.
        switch (entry.type) {
            case SelectionType::Face:
                if (!entry.shape.IsNull())
                    renderFace(entry.shape, vp, faceColor);
                break;
            case SelectionType::Edge:
                if (!entry.shape.IsNull())
                    renderEdge(entry.shape, vp, edgeColor);
                break;
            case SelectionType::Body: {
                if (entry.bodyId < 0) break;
                TopoDS_Shape current;
                try { current = doc.getBody(entry.bodyId); } catch (...) {}
                if (!current.IsNull()) renderBody(current, vp, bodyOutlineColor);
                break;
            }
            default:
                break;
        }
    }
}

void SelectionHighlight::highlightShapes(const std::vector<TopoDS_Shape>& shapes,
                                         const glm::mat4& view,
                                         const glm::mat4& projection,
                                         const glm::vec3& color) {
    if (!m_program) return;
    const glm::mat4 vp = projection * view;
    for (const auto& s : shapes) {
        if (s.IsNull()) continue;
        switch (s.ShapeType()) {
            case TopAbs_FACE: renderFace(s, vp, color); break;
            case TopAbs_EDGE: renderEdge(s, vp, color); break;
            default:          renderBody(s, vp, color); break; // SOLID/SHELL/COMPOUND
        }
    }
}

// Free an entry's persistent GPU buffers (rebuild or eviction).
void SelectionHighlight::freeEntryGL(CacheEntry& e) {
    if (e.vao) glDeleteVertexArrays(1, &e.vao);
    if (e.vbo) glDeleteBuffers(1, &e.vbo);
    e.vao = 0; e.vbo = 0; e.count = 0;
}

// Delete every entry's GPU buffers, then drop the map. std::map::clear() alone
// would leak the VAOs/VBOs (it only runs the trivial CacheEntry destructor).
void SelectionHighlight::freeCacheGL(std::map<const void*, CacheEntry>& m) {
    for (auto& [key, e] : m) freeEntryGL(e);
    m.clear();
}

// Upload a freshly-built vertex stream into the entry's own persistent buffer
// (GL_STATIC_DRAW; drawn many frames, uploaded once). Empty stream → count 0,
// no GL objects, so the entry is cached as "nothing to draw" and not rebuilt
// every frame.
void SelectionHighlight::uploadEntry(CacheEntry& e,
                                     const std::vector<float>& verts) {
    freeEntryGL(e);
    if (verts.empty()) return;
    e.count = static_cast<int>(verts.size() / 3);
    glGenVertexArrays(1, &e.vao);
    glGenBuffers(1, &e.vbo);
    glBindVertexArray(e.vao);
    glBindBuffer(GL_ARRAY_BUFFER, e.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void SelectionHighlight::clearCaches() {
    freeCacheGL(m_bodyCache);
    freeCacheGL(m_faceCache);
    freeCacheGL(m_edgeCache);
}

void SelectionHighlight::renderFace(const TopoDS_Shape& faceShape, const glm::mat4& vp,
                                     const glm::vec3& color) {
    // Just a blue tint over the face - no outline, no solid
    TopoDS_Face face = TopoDS::Face(faceShape);

    // Cache the triangulated vertex buffer per face - walking every triangle
    // per frame was 5-50ms on a big NURBS face. See the CacheEntry comment in
    // the header for the key/ownership/revalidation/cap scheme.
    const void* key = faceShape.TShape().get();
    auto it = m_faceCache.find(key);
    if (it == m_faceCache.end() || !(it->second.loc == faceShape.Location())) {
        TopLoc_Location location;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, location);
        if (tri.IsNull()) {
            BRepMesh_IncrementalMesh mesh(faceShape, materializr::meshParams(0.1, 0.5, false));
            tri = BRep_Tool::Triangulation(face, location);
        }

        if (it == m_faceCache.end() && m_faceCache.size() >= kCacheCap)
            freeCacheGL(m_faceCache); // flush orphans; live entries rebuild next frame
        CacheEntry& entry = m_faceCache[key];
        entry.shape = faceShape;
        entry.loc = faceShape.Location();
        std::vector<float> verts;
        // A face the mesher cannot triangulate (a self-intersecting wire, some
        // fused tangent surfaces) gets an EMPTY entry, so the mesher is not
        // re-run on every frame it stays selected; the entry draws nothing.
        if (tri.IsNull()) {
            uploadEntry(entry, verts);
            return;
        }

        const gp_Trsf& trsf = location.Transformation();
        bool hasXform = !location.IsIdentity();
        verts.reserve(tri->NbTriangles() * 9);
        for (int i = 1; i <= tri->NbTriangles(); i++) {
            int n1, n2, n3;
            tri->Triangle(i).Get(n1, n2, n3);
            gp_Pnt p1 = tri->Node(n1), p2 = tri->Node(n2), p3 = tri->Node(n3);
            if (hasXform) { p1.Transform(trsf); p2.Transform(trsf); p3.Transform(trsf); }
            verts.insert(verts.end(), {(float)p1.X(),(float)p1.Y(),(float)p1.Z()});
            verts.insert(verts.end(), {(float)p2.X(),(float)p2.Y(),(float)p2.Z()});
            verts.insert(verts.end(), {(float)p3.X(),(float)p3.Y(),(float)p3.Z()});
        }
        uploadEntry(entry, verts); // persistent GPU buffer; CPU copy dropped here
        it = m_faceCache.find(key);
    }
    if (it == m_faceCache.end() || it->second.count == 0) return;

    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform4f(m_locColor, color.r, color.g, color.b, 0.35f);

    glBindVertexArray(it->second.vao);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);

    glDrawArrays(GL_TRIANGLES, 0, it->second.count);

    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    glBindVertexArray(0);
    glUseProgram(0);
}

void SelectionHighlight::renderEdge(const TopoDS_Shape& edgeShape, const glm::mat4& vp,
                                     const glm::vec3& color) {
    try {
        // Same caching scheme as renderBody/renderFace: the GCPnts curve
        // discretization is the per-frame cost we want to skip. See the
        // CacheEntry comment in the header for the key/ownership/cap scheme.
        const void* key = edgeShape.TShape().get();
        auto it = m_edgeCache.find(key);
        if (it == m_edgeCache.end() || !(it->second.loc == edgeShape.Location())) {
            TopoDS_Edge edge = TopoDS::Edge(edgeShape);
            BRepAdaptor_Curve curve(edge);
            GCPnts_TangentialDeflection discretizer(curve, 0.05, 0.05);
            int nPts = discretizer.NbPoints();
            if (nPts < 2) return;

            if (it == m_edgeCache.end() && m_edgeCache.size() >= kCacheCap)
                freeCacheGL(m_edgeCache);
            CacheEntry& entry = m_edgeCache[key];
            entry.shape = edgeShape;
            entry.loc = edgeShape.Location();
            std::vector<float> verts;
            verts.reserve((nPts - 1) * 6);
            for (int i = 1; i < nPts; i++) {
                gp_Pnt p1 = discretizer.Value(i);
                gp_Pnt p2 = discretizer.Value(i + 1);
                verts.insert(verts.end(), {(float)p1.X(),(float)p1.Y(),(float)p1.Z()});
                verts.insert(verts.end(), {(float)p2.X(),(float)p2.Y(),(float)p2.Z()});
            }
            uploadEntry(entry, verts);
            it = m_edgeCache.find(key);
        }
        if (it == m_edgeCache.end() || it->second.count == 0) return;

        drawThickLines(it->second.vao, it->second.count, vp, color,
                       m_edgeLineWidth * 0.5f);
    } catch (...) {}
}

void SelectionHighlight::renderBody(const TopoDS_Shape& bodyShape, const glm::mat4& vp,
                                     const glm::vec3& color) {
    // Cache key = the body's TShape pointer, revalidated on the shape's
    // location (the verts are world coords; a multi-body gizmo move commit
    // keeps the TShape but moves the body). See the CacheEntry comment in
    // the header for the full ownership/cap scheme. Stored per-body so
    // multiple selected bodies each keep their own cached verts.
    const void* key = bodyShape.TShape().get();
    auto it = m_bodyCache.find(key);
    if (it == m_bodyCache.end() || !(it->second.loc == bodyShape.Location())) {
        if (it == m_bodyCache.end() && m_bodyCache.size() >= kCacheCap)
            freeCacheGL(m_bodyCache);
        CacheEntry& entry = m_bodyCache[key];
        entry.shape = bodyShape;
        entry.loc = bodyShape.Location();
        std::vector<float> verts;
        for (TopExp_Explorer exp(bodyShape, TopAbs_EDGE); exp.More(); exp.Next()) {
            try {
                TopoDS_Edge edge = TopoDS::Edge(exp.Current());
                BRepAdaptor_Curve curve(edge);
                GCPnts_TangentialDeflection discretizer(curve, 0.1, 0.1);
                int nPts = discretizer.NbPoints();
                for (int i = 1; i < nPts; i++) {
                    gp_Pnt p1 = discretizer.Value(i);
                    gp_Pnt p2 = discretizer.Value(i + 1);
                    verts.insert(verts.end(),
                        {(float)p1.X(),(float)p1.Y(),(float)p1.Z()});
                    verts.insert(verts.end(),
                        {(float)p2.X(),(float)p2.Y(),(float)p2.Z()});
                }
            } catch (...) { continue; }
        }
        uploadEntry(entry, verts);
        it = m_bodyCache.find(key);
    }
    if (it == m_bodyCache.end() || it->second.count == 0) return;

    // Body outlines track the configured edge width but stay a touch thinner
    // (the original 2.5:3.0 ratio) so a whole body reads differently from a
    // single picked edge.
    drawThickLines(it->second.vao, it->second.count, vp, color,
                   m_edgeLineWidth * 0.5f * (2.5f / 3.0f));
}

void SelectionHighlight::drawThickLines(unsigned int vao, int count, const glm::mat4& vp,
                                        const glm::vec3& color, float halfWidthPx) {
    if (!vao || count <= 0) return;

    glBindVertexArray(vao); // buffer already resident (uploaded on cache build)

#if defined(MZ_GLES)
    // GL ES 3.0 has no geometry shader, so there's no screen-space line widening
    // (m_lineProgram is 0). Fall back to plain GL_LINES with the basic program -
    // the selection outline renders, just at hardware line width rather than the
    // antialiased ribbon. (void halfWidthPx; line width is fixed.)
    (void)halfWidthPx;
    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform4f(m_locColor, color.r, color.g, color.b, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glLineWidth(2.0f);
    glDrawArrays(GL_LINES, 0, count);
    glEnable(GL_DEPTH_TEST);
#else
    if (!m_lineProgram) { glBindVertexArray(0); return; }
    GLint viewport[4] = {0, 0, 1, 1};
    glGetIntegerv(GL_VIEWPORT, viewport);
    float vw = static_cast<float>(viewport[2] > 0 ? viewport[2] : 1);
    float vh = static_cast<float>(viewport[3] > 0 ? viewport[3] : 1);

    glUseProgram(m_lineProgram);
    glUniformMatrix4fv(m_locLineMVP, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform4f(m_locLineColor, color.r, color.g, color.b, 1.0f);
    glUniform2f(m_locViewport, vw, vh);
    glUniform1f(m_locHalfWidth, std::max(0.5f, halfWidthPx));

    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_LINES, 0, count);
    glEnable(GL_DEPTH_TEST);
#endif

    glBindVertexArray(0);
    glUseProgram(0);
}

bool SelectionHighlight::compileShader(unsigned int& shader, unsigned int type, const char* source) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

} // namespace materializr
