#include "RefMeshRenderer.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstdio>

namespace materializr {

// Flat single-light diffuse + ambient - deliberately simpler than
// ShapeRenderer's Blinn-Phong (no fill light, no specular, no section
// clipping, no outline pass): this is a tracing aid, not a modeled body, so
// it only needs to read as a recognizable translucent shape.
static const char* s_vertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
uniform mat4 u_mvp;
out vec3 v_normal;
void main() {
    v_normal = a_normal;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

static const char* s_fragSource = R"(
#version 330 core
uniform float u_opacity;
uniform vec3 u_color;
in vec3 v_normal;
out vec4 fragColor;
void main() {
    vec3 n = normalize(v_normal);
    vec3 lightDir = normalize(vec3(0.4, 0.6, 0.7));
    float diffuse = abs(dot(n, lightDir)); // abs: light both faces, mesh may be unsewn/open
    float shade = 0.55 + 0.45 * diffuse;
    fragColor = vec4(u_color * shade, u_opacity);
}
)";

RefMeshRenderer::RefMeshRenderer() = default;

RefMeshRenderer::~RefMeshRenderer() {
    if (m_program) glDeleteProgram(m_program);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
}

bool RefMeshRenderer::initialize() {
    unsigned int vs = 0, fs = 0;
    if (!compileShader(vs, GL_VERTEX_SHADER, s_vertSource)) return false;
    if (!compileShader(fs, GL_FRAGMENT_SHADER, s_fragSource)) {
        glDeleteShader(vs);
        return false;
    }
    if (!linkProgram(vs, fs)) {
        glDeleteShader(vs); glDeleteShader(fs);
        return false;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    m_locMVP     = glGetUniformLocation(m_program, "u_mvp");
    m_locOpacity = glGetUniformLocation(m_program, "u_opacity");
    m_locColor   = glGetUniformLocation(m_program, "u_color");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void RefMeshRenderer::setMesh(const std::vector<float>& vertices) {
    m_vertexCount = static_cast<int>(vertices.size() / 6);
    if (!m_vao) return; // not initialized yet (e.g. headless test build)
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    if (vertices.empty()) {
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STATIC_DRAW);
    } else {
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                    vertices.data(), GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RefMeshRenderer::render(const glm::mat4& view, const glm::mat4& projection) {
    if (!m_program || !m_visible || m_vertexCount == 0) return;

    glm::mat4 vp = projection * view;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // No depth write, same reasoning as RefImageRenderer: this overlay must
    // never make the real modeling result underneath it harder to see, and
    // an unsorted translucent solid has no correct depth order to write
    // anyway.
    glDepthMask(GL_FALSE);
    // Reference meshes from a scan/print are routinely unsewn or have mixed
    // winding - cull nothing, the fragment shader lights both sides instead.
    glDisable(GL_CULL_FACE);

    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform1f(m_locOpacity, m_opacity);
    // Cool blue, deliberately distinct from every body's default grey/custom
    // palette (ShapeRenderer::bodyColor) so a reference is never mistaken for
    // real geometry.
    glUniform3f(m_locColor, 0.30f, 0.55f, 0.85f);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, m_vertexCount);
    glBindVertexArray(0);

    glUseProgram(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

bool RefMeshRenderer::compileShader(unsigned int& shader, unsigned int type,
                                    const char* src) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "RefMeshRenderer shader compile failed: %s\n", log);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

bool RefMeshRenderer::linkProgram(unsigned int vertShader, unsigned int fragShader) {
    m_program = glCreateProgram();
    glAttachShader(m_program, vertShader);
    glAttachShader(m_program, fragShader);
    glLinkProgram(m_program);
    int ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_program, 512, nullptr, log);
        std::fprintf(stderr, "RefMeshRenderer link failed: %s\n", log);
        glDeleteProgram(m_program);
        m_program = 0;
        return false;
    }
    return true;
}

} // namespace materializr
