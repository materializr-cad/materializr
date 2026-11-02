#include "gl_common.h"
#include "BoxSelect.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <vector>
#include <algorithm>

namespace materializr {

// Passthrough vertex shader: positions already in NDC
static const char* s_vertSource = R"(
#version 330 core
layout(location = 0) in vec2 a_position;
void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

// Fragment shader: solid color with alpha
static const char* s_fragSource = R"(
#version 330 core
uniform vec4 u_color;
out vec4 fragColor;
void main() {
    fragColor = u_color;
}
)";

BoxSelect::BoxSelect() {}

BoxSelect::~BoxSelect() {
    if (m_program) glDeleteProgram(m_program);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
}

static bool compileShaderHelper(unsigned int& shader, unsigned int type, const char* source) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "BoxSelect shader error: %s\n", log);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

bool BoxSelect::initialize() {
    unsigned int vert = 0, frag = 0;
    if (!compileShaderHelper(vert, GL_VERTEX_SHADER, s_vertSource)) return false;
    if (!compileShaderHelper(frag, GL_FRAGMENT_SHADER, s_fragSource)) {
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
        std::fprintf(stderr, "BoxSelect link error: %s\n", log);
        return false;
    }

    m_locColor = glGetUniformLocation(m_program, "u_color");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    return true;
}

void BoxSelect::begin(glm::vec2 startPos) {
    m_active = true;
    m_start = startPos;
    m_current = startPos;
}

void BoxSelect::update(glm::vec2 currentPos) {
    m_current = currentPos;
}

void BoxSelect::end() {
    m_active = false;
}

bool BoxSelect::isActive() const { return m_active; }

glm::vec2 BoxSelect::getMin() const {
    return glm::vec2(std::min(m_start.x, m_current.x),
                     std::min(m_start.y, m_current.y));
}

glm::vec2 BoxSelect::getMax() const {
    return glm::vec2(std::max(m_start.x, m_current.x),
                     std::max(m_start.y, m_current.y));
}

void BoxSelect::render(float viewportWidth, float viewportHeight) {
    if (!m_active || !m_program) return;

    // Convert screen coordinates to NDC [-1, 1]
    auto toNDC = [&](glm::vec2 screen) -> glm::vec2 {
        return glm::vec2(
            (screen.x / viewportWidth) * 2.0f - 1.0f,
            1.0f - (screen.y / viewportHeight) * 2.0f // flip Y
        );
    };

    glm::vec2 minScreen = getMin();
    glm::vec2 maxScreen = getMax();

    glm::vec2 bl = toNDC(glm::vec2(minScreen.x, maxScreen.y)); // bottom-left
    glm::vec2 br = toNDC(glm::vec2(maxScreen.x, maxScreen.y)); // bottom-right
    glm::vec2 tr = toNDC(glm::vec2(maxScreen.x, minScreen.y)); // top-right
    glm::vec2 tl = toNDC(glm::vec2(minScreen.x, minScreen.y)); // top-left

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_program);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // Draw filled quad (two triangles) with low alpha
    {
        float fillVerts[] = {
            bl.x, bl.y,  br.x, br.y,  tr.x, tr.y,
            bl.x, bl.y,  tr.x, tr.y,  tl.x, tl.y,
        };
        glBufferData(GL_ARRAY_BUFFER, sizeof(fillVerts), fillVerts, GL_DYNAMIC_DRAW);
        // Blue = window (fully enclosed), green = crossing (touched).
        if (isWindow()) glUniform4f(m_locColor, 0.3f, 0.5f, 1.0f, 0.1f);
        else            glUniform4f(m_locColor, 0.3f, 0.8f, 0.4f, 0.1f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // Draw outline (line loop) with higher alpha
    {
        float outlineVerts[] = {
            bl.x, bl.y,
            br.x, br.y,
            tr.x, tr.y,
            tl.x, tl.y,
        };
        glBufferData(GL_ARRAY_BUFFER, sizeof(outlineVerts), outlineVerts, GL_DYNAMIC_DRAW);
        if (isWindow()) glUniform4f(m_locColor, 0.3f, 0.5f, 1.0f, 0.8f);
        else            glUniform4f(m_locColor, 0.3f, 0.8f, 0.4f, 0.8f);
        glLineWidth(1.5f);
        glDrawArrays(GL_LINE_LOOP, 0, 4);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}

} // namespace materializr
