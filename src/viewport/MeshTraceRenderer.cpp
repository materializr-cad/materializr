#include "MeshTraceRenderer.h"
#include "SectionCap.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstdio>

namespace materializr {

// Flat-colored triangles (the filled cap) and flat-colored lines (the
// outline) - same two-program shape as RefImageRenderer's photo+border pass,
// just without a texture sampler.
static const char* s_vertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
uniform mat4 u_mvp;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

static const char* s_fragSource = R"(
#version 330 core
uniform vec4 u_color;
out vec4 fragColor;
void main() {
    fragColor = u_color;
}
)";

MeshTraceRenderer::MeshTraceRenderer() = default;

MeshTraceRenderer::~MeshTraceRenderer() {
    if (m_capProgram) glDeleteProgram(m_capProgram);
    if (m_lineProgram) glDeleteProgram(m_lineProgram);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
}

bool MeshTraceRenderer::initialize() {
    auto build = [&](unsigned int& prog, int& locMVP, int& locColor) -> bool {
        unsigned int vs = 0, fs = 0;
        if (!compileShader(vs, GL_VERTEX_SHADER, s_vertSource)) return false;
        if (!compileShader(fs, GL_FRAGMENT_SHADER, s_fragSource)) {
            glDeleteShader(vs);
            return false;
        }
        prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs); glDeleteShader(fs);
        int ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            std::fprintf(stderr, "MeshTraceRenderer: program link failed\n");
            glDeleteProgram(prog);
            prog = 0;
            return false;
        }
        locMVP = glGetUniformLocation(prog, "u_mvp");
        locColor = glGetUniformLocation(prog, "u_color");
        return true;
    };
    if (!build(m_capProgram, m_capLocMVP, m_capLocColor)) return false;
    if (!build(m_lineProgram, m_lineLocMVP, m_lineLocColor)) return false;

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void MeshTraceRenderer::sync(const std::vector<Item>& items) {
    std::map<int, Slice> next;
    for (const auto& it : items) {
        if (!it.bodyShape || it.bodyShape->IsNull()) continue;
        Slice s;
        s.opacity = it.opacity;
        s.selected = it.selected;
        if (it.shadowMode) {
            // Just the filled blob, no outline - every triangle flattened
            // and drawn raw (see SectionCap.h), cheap enough to redo every
            // sync() and immune to the loop-chaining noise a real outline
            // extraction risks on a dense/curved mesh. Real closed-loop
            // geometry for sketch insertion is a separate, one-shot call
            // (computeMeshShadowOutline) triggered by the Insert button, not
            // this live overlay.
            computeMeshShadow(*it.bodyShape, it.plane, s.cap);
        } else {
            SectionSlice slice;
            if (sliceSection(faceMeshes(*it.bodyShape), it.plane, slice)) {
                s.lines = std::move(slice.lines);
                s.cap = std::move(slice.cap);
            }
        }
        next[it.planeId] = std::move(s);
    }
    m_slices = std::move(next);
}

void MeshTraceRenderer::render(const glm::mat4& view, const glm::mat4& projection) {
    if (!m_capProgram || !m_lineProgram || m_slices.empty()) return;

    glm::mat4 vp = projection * view;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Same no-depth-write treatment as the photo underlay - depth TEST alone
    // keeps a trace that's genuinely behind a body correctly hidden, without
    // the translucent cap losing to a body drawn after it in submission
    // order (see RefImagePlugin's priority-500 comment for the history).
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    for (const auto& [planeId, s] : m_slices) {
        (void)planeId;
        if (!s.cap.empty()) {
            glUseProgram(m_capProgram);
            glUniformMatrix4fv(m_capLocMVP, 1, GL_FALSE, glm::value_ptr(vp));
            glm::vec4 c = s.selected
                ? glm::vec4(1.00f, 0.78f, 0.20f, s.opacity)
                : glm::vec4(0.35f, 0.62f, 0.90f, s.opacity);
            glUniform4fv(m_capLocColor, 1, glm::value_ptr(c));
            glBufferData(GL_ARRAY_BUFFER,
                        static_cast<GLsizeiptr>(s.cap.size() * sizeof(float)),
                        s.cap.data(), GL_DYNAMIC_DRAW);
            glDrawArrays(GL_TRIANGLES, 0,
                        static_cast<GLsizei>(s.cap.size() / 3));
        }
        if (!s.lines.empty()) {
            glUseProgram(m_lineProgram);
            glUniformMatrix4fv(m_lineLocMVP, 1, GL_FALSE, glm::value_ptr(vp));
            glm::vec4 c = s.selected
                ? glm::vec4(1.00f, 0.78f, 0.20f, 1.00f)
                : glm::vec4(0.20f, 0.45f, 0.75f, 0.90f);
            glUniform4fv(m_lineLocColor, 1, glm::value_ptr(c));
            glBufferData(GL_ARRAY_BUFFER,
                        static_cast<GLsizeiptr>(s.lines.size() * sizeof(float)),
                        s.lines.data(), GL_DYNAMIC_DRAW);
            glLineWidth(s.selected ? 2.5f : 1.5f);
            glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(s.lines.size() / 3));
        }
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

bool MeshTraceRenderer::compileShader(unsigned int& shader, unsigned int type,
                                      const char* src) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "MeshTraceRenderer shader compile failed: %s\n", log);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

} // namespace materializr
