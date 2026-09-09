#include "gl_common.h"
#include "EdgeRenderer.h"

#include <glm/gtc/type_ptr.hpp>

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <BRep_Tool.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <cstdio>
#include <vector>

namespace materializr {

static const char* s_edgeVertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
// Same three matrices as the SURFACE shader, multiplied in the SAME order, and
// gl_Position declared invariant in both - so an edge vertex produces the
// bit-identical depth value its face produced. The old path premultiplied one
// MVP on the CPU while faces multiplied P*V*M on the GPU; float32 rounds the
// two differently, and the divergence is view-dependent. With no working line
// bias (POLYGON_OFFSET_LINE never applies to GL_LINES), edges landed a hair in
// front of or behind their own surface depending on camera angle - "sinking"
// into bodies, and popping through thin walls when the error exceeded the
// wall's depth footprint (the corvus see-through bug). Invariance kills the
// mismatch at the root; the edge pass then draws with GL_LEQUAL so equal
// depth = edge wins on its own face, greater = hidden behind a wall stays hidden.
invariant gl_Position;
uniform mat4 u_model;
uniform mat4 u_view;
uniform mat4 u_projection;
out vec3 v_worldPos;
void main() {
    // Edge buffers are world-space; u_model is identity except during live
    // previews. Applying it here also fixes the section clip ignoring the
    // preview transform (v_worldPos used to be the untransformed position).
    vec4 worldPos = u_model * vec4(a_position, 1.0);
    v_worldPos = worldPos.xyz;
    gl_Position = u_projection * u_view * worldPos;
}
)";

static const char* s_edgeFragSource = R"(
#version 330 core
in vec3 v_worldPos;
uniform vec3 u_color;
uniform bool u_sectionEnabled; // section view: clip like the body shader,
uniform vec3 u_sectionPoint;   // or the full wireframe ghosts through the
uniform vec3 u_sectionNormal;  // clipped half
out vec4 fragColor;
void main() {
    if (u_sectionEnabled &&
        dot(v_worldPos - u_sectionPoint, u_sectionNormal) > 0.0) discard;
    fragColor = vec4(u_color, 1.0);
}
)";

EdgeRenderer::EdgeRenderer() {}

EdgeRenderer::~EdgeRenderer() {
    clear();
    if (m_program) glDeleteProgram(m_program);
}

bool EdgeRenderer::initialize() {
    unsigned int vert = 0, frag = 0;
    if (!compileShader(vert, GL_VERTEX_SHADER, s_edgeVertSource)) return false;
    if (!compileShader(frag, GL_FRAGMENT_SHADER, s_edgeFragSource)) {
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
        std::fprintf(stderr, "EdgeRenderer link error: %s\n", log);
        return false;
    }

    m_locModel = glGetUniformLocation(m_program, "u_model");
    m_locView = glGetUniformLocation(m_program, "u_view");
    m_locProjection = glGetUniformLocation(m_program, "u_projection");
    m_locColor = glGetUniformLocation(m_program, "u_color");
    m_locSectionEnabled = glGetUniformLocation(m_program, "u_sectionEnabled");
    m_locSectionPoint = glGetUniformLocation(m_program, "u_sectionPoint");
    m_locSectionNormal = glGetUniformLocation(m_program, "u_sectionNormal");

    return true;
}

int EdgeRenderer::addShape(const TopoDS_Shape& shape, float deflection) {
    if (shape.IsNull()) return -1;

    std::vector<float> vertices;

    for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(explorer.Current());

        // Skip degenerated edges
        if (BRep_Tool::Degenerated(edge)) continue;

        try {
            BRepAdaptor_Curve curve(edge);
            GCPnts_TangentialDeflection discretizer(curve, deflection, 0.1);

            int nbPoints = discretizer.NbPoints();
            if (nbPoints < 2) continue;

            for (int i = 1; i < nbPoints; ++i) {
                gp_Pnt p1 = discretizer.Value(i);
                gp_Pnt p2 = discretizer.Value(i + 1);

                vertices.push_back(static_cast<float>(p1.X()));
                vertices.push_back(static_cast<float>(p1.Y()));
                vertices.push_back(static_cast<float>(p1.Z()));

                vertices.push_back(static_cast<float>(p2.X()));
                vertices.push_back(static_cast<float>(p2.Y()));
                vertices.push_back(static_cast<float>(p2.Z()));
            }
        } catch (...) {
            // Skip edges that cannot be discretized
            continue;
        }
    }

    if (vertices.empty()) return -1;

    EdgeMesh mesh;
    mesh.vertexCount = static_cast<int>(vertices.size() / 3);
    mesh.shape = shape;
    mesh.deflection = deflection;

    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);

    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);

    int index = static_cast<int>(m_meshes.size());
    m_meshes.push_back(mesh);
    return index;
}

void EdgeRenderer::setBodyEdges(int bodyId, const TopoDS_Shape& shape,
                                 float deflection) {
    // Reclaim the slot retireAll() kept for this exact shape, if any, before
    // paying for a fresh discretization + upload.
    int appendedSlot = -1;
    for (size_t i = 0; i < m_retired.size(); ++i) {
        EdgeMesh& r = m_retired[i];
        if (r.vao && r.deflection == deflection && r.shape.IsSame(shape)) {
            r.modelMatrix = glm::mat4(1.0f);
            appendedSlot = static_cast<int>(m_meshes.size());
            m_meshes.push_back(r);
            m_retired[i] = m_retired.back();
            m_retired.pop_back();
            break;
        }
    }
    if (appendedSlot < 0) appendedSlot = addShape(shape, deflection);
    if (appendedSlot < 0) return; // tessellation produced no edges
    auto it = m_bodyToSlot.find(bodyId);
    if (it == m_bodyToSlot.end()) {
        m_meshes[appendedSlot].bodyId = bodyId;
        m_bodyToSlot[bodyId] = appendedSlot;
        return;
    }
    int oldSlot = it->second;
    if (oldSlot < 0 || oldSlot >= static_cast<int>(m_meshes.size())) {
        m_meshes[appendedSlot].bodyId = bodyId;
        m_bodyToSlot[bodyId] = appendedSlot;
        return;
    }
    EdgeMesh& old = m_meshes[oldSlot];
    if (old.vao) glDeleteVertexArrays(1, &old.vao);
    if (old.vbo) glDeleteBuffers(1, &old.vbo);
    old = m_meshes[appendedSlot];
    old.bodyId = bodyId;
    // Drop the now-stale tail slot.
    m_meshes.pop_back();
}

void EdgeRenderer::removeBody(int bodyId) {
    auto it = m_bodyToSlot.find(bodyId);
    if (it == m_bodyToSlot.end()) return;
    int slot = it->second;
    m_bodyToSlot.erase(it);
    if (slot < 0 || slot >= static_cast<int>(m_meshes.size())) return;
    EdgeMesh& m = m_meshes[slot];
    if (m.vao) glDeleteVertexArrays(1, &m.vao);
    if (m.vbo) glDeleteBuffers(1, &m.vbo);
    m.vao = 0; m.vbo = 0; m.vertexCount = 0; m.bodyId = -1;
    m.shape.Nullify(); // do not pin a deleted body's geometry through m_retired
}

void EdgeRenderer::render(const glm::mat4& view, const glm::mat4& projection) {
    if (m_meshes.empty() || !m_program) return;

    glUseProgram(m_program);
    glUniformMatrix4fv(m_locView, 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(m_locProjection, 1, GL_FALSE, glm::value_ptr(projection));

    // Dark gray edge color
    glm::vec3 edgeColor(0.15f, 0.15f, 0.18f);
    glUniform3fv(m_locColor, 1, glm::value_ptr(edgeColor));
    glUniform1i(m_locSectionEnabled, m_sectionEnabled ? 1 : 0);
    glUniform3fv(m_locSectionPoint, 1, glm::value_ptr(m_sectionPoint));
    glUniform3fv(m_locSectionNormal, 1, glm::value_ptr(m_sectionNormal));

    // Depth: edges compute the exact same depth as their faces (invariant
    // shader above), so LEQUAL lets an edge win the tie against its own
    // surface while anything genuinely behind a wall still loses. No bias:
    // the old glPolygonOffset(GL_POLYGON_OFFSET_LINE) was a no-op here -
    // polygon offset applies to polygons rasterized in line MODE, never to
    // GL_LINES primitives.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);

    glLineWidth(1.0f);

    for (const auto& mesh : m_meshes) {
        if (mesh.vertexCount == 0) continue;
        // Per-mesh MVP folds the body's own model matrix in. Identity
        // for nearly all bodies; non-identity only during a live preview
        // (revolve, future move-preview, etc.) where the geometry stays
        // put but the rendering shifts via the GPU.
        glUniformMatrix4fv(m_locModel, 1, GL_FALSE,
                           glm::value_ptr(mesh.modelMatrix));

        glBindVertexArray(mesh.vao);
        glDrawArrays(GL_LINES, 0, mesh.vertexCount);
    }

    glDepthFunc(GL_LESS);   // restore the default for every later pass
    glBindVertexArray(0);
    glUseProgram(0);
}

void EdgeRenderer::retireAll() {
    // Keep the buffers: the full rebuild that follows may hand the same
    // shapes straight back (see setBodyEdges). Anything retired last time
    // and never reclaimed dies here.
    freeRetired();
    m_retired.swap(m_meshes);
    m_meshes.clear();
    m_bodyToSlot.clear();
}

void EdgeRenderer::clear() {
    retireAll();
    freeRetired();
}

void EdgeRenderer::freeRetired() {
    for (auto& mesh : m_retired) {
        if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
        if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
    }
    m_retired.clear();
}

bool EdgeRenderer::reclaimStale(int bodyId) {
    for (size_t i = 0; i < m_retired.size(); ++i) {
        if (m_retired[i].bodyId != bodyId || !m_retired[i].vao) continue;
        const int slot = static_cast<int>(m_meshes.size());
        m_meshes.push_back(m_retired[i]);
        m_retired[i] = m_retired.back();
        m_retired.pop_back();
        m_bodyToSlot[bodyId] = slot;
        return true;
    }
    return false;
}

int EdgeRenderer::findSlotByBody(int bodyId) const {
    auto it = m_bodyToSlot.find(bodyId);
    return (it == m_bodyToSlot.end()) ? -1 : it->second;
}

void EdgeRenderer::setModelMatrix(int slot, const glm::mat4& model) {
    if (slot < 0 || slot >= static_cast<int>(m_meshes.size())) return;
    m_meshes[slot].modelMatrix = model;
}

bool EdgeRenderer::compileShader(unsigned int& shader, unsigned int type, const char* source) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "EdgeRenderer shader error: %s\n", log);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

} // namespace materializr
