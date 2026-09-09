#include "AxisRenderer.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstdio>

namespace materializr {

static const char* s_axisVertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
uniform mat4 u_mvp;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

static const char* s_axisFragSource = R"(
#version 330 core
uniform vec4 u_color;
out vec4 fragColor;
void main() {
    fragColor = u_color;
}
)";

AxisRenderer::AxisRenderer() = default;

AxisRenderer::~AxisRenderer() {
    if (m_program) { glDeleteProgram(m_program); m_program = 0; }
    if (m_vao)     { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_vbo)     { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
}

bool AxisRenderer::initialize() {
    unsigned int vertShader = 0, fragShader = 0;
    if (!compileShader(vertShader, GL_VERTEX_SHADER, s_axisVertSource))   return false;
    if (!compileShader(fragShader, GL_FRAGMENT_SHADER, s_axisFragSource)) {
        glDeleteShader(vertShader); return false;
    }
    if (!linkProgram(vertShader, fragShader)) {
        glDeleteShader(vertShader); glDeleteShader(fragShader); return false;
    }
    glDeleteShader(vertShader); glDeleteShader(fragShader);

    m_locMVP   = glGetUniformLocation(m_program, "u_mvp");
    m_locColor = glGetUniformLocation(m_program, "u_color");

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

void AxisRenderer::addAxis(const gp_Pnt& origin, const gp_Dir& direction,
                            const std::string& name,
                            glm::vec4 color, float halfLength, bool selected) {
    AxisData a;
    a.origin = origin;
    a.direction = direction;
    a.name = name;
    a.color = color;
    a.halfLength = halfLength;
    a.visible = true;
    a.selected = selected;
    m_axes.push_back(std::move(a));
}

void AxisRenderer::render(const glm::mat4& view, const glm::mat4& projection) {
    if (!m_program || m_axes.empty()) return;

    glm::mat4 vp = projection * view;
    glUseProgram(m_program);

    // Lines mix with the rest of the scene; standard alpha blend so the
    // axis stays visible against bodies without occluding them entirely.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(m_vao);

    for (const auto& ad : m_axes) {
        if (!ad.visible) continue;

        glm::vec3 o((float)ad.origin.X(), (float)ad.origin.Y(), (float)ad.origin.Z());
        glm::vec3 d((float)ad.direction.X(), (float)ad.direction.Y(), (float)ad.direction.Z());
        float h = ad.halfLength;

        glm::vec3 p0 = o - d * h;
        glm::vec3 p1 = o + d * h;

        // Selected: brighter amber + thicker. Unselected: caller's colour.
        glm::vec4 col = ad.selected
                            ? glm::vec4(1.00f, 0.78f, 0.20f, 1.0f)
                            : ad.color;
        float lineW = ad.selected ? 3.0f : 1.8f;

        // Main axis segment.
        float lineVerts[] = {
            p0.x, p0.y, p0.z,
            p1.x, p1.y, p1.z,
        };
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(lineVerts), lineVerts, GL_DYNAMIC_DRAW);
        glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(vp));
        glUniform4fv(m_locColor, 1, glm::value_ptr(col));
        glLineWidth(lineW);
        glDrawArrays(GL_LINES, 0, 2);

        // Origin marker: a small +-cross perpendicular to the axis so the
        // anchor is visible even when the line lies along a body edge.
        // Pick an arbitrary "up" not collinear with d, then orthogonalise.
        glm::vec3 up = (std::abs(d.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 t1 = glm::normalize(glm::cross(d, up));
        glm::vec3 t2 = glm::normalize(glm::cross(d, t1));
        float m = h * 0.06f; // marker size - proportional to the axis length
        float markVerts[] = {
            (o - t1*m).x, (o - t1*m).y, (o - t1*m).z,
            (o + t1*m).x, (o + t1*m).y, (o + t1*m).z,
            (o - t2*m).x, (o - t2*m).y, (o - t2*m).z,
            (o + t2*m).x, (o + t2*m).y, (o + t2*m).z,
        };
        glBufferData(GL_ARRAY_BUFFER, sizeof(markVerts), markVerts, GL_DYNAMIC_DRAW);
        glLineWidth(lineW + 0.5f);
        glDrawArrays(GL_LINES, 0, 4);
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

void AxisRenderer::clear() { m_axes.clear(); }

bool AxisRenderer::compileShader(unsigned int& shader, unsigned int type,
                                  const char* source) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "AxisRenderer shader compile failed: %s\n", log);
        glDeleteShader(shader); shader = 0; return false;
    }
    return true;
}

bool AxisRenderer::linkProgram(unsigned int vertShader, unsigned int fragShader) {
    m_program = glCreateProgram();
    glAttachShader(m_program, vertShader);
    glAttachShader(m_program, fragShader);
    glLinkProgram(m_program);
    int ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(m_program, 512, nullptr, log);
        std::fprintf(stderr, "AxisRenderer link failed: %s\n", log);
        glDeleteProgram(m_program); m_program = 0; return false;
    }
    return true;
}

} // namespace materializr
