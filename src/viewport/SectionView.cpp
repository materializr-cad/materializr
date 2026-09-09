#include "gl_common.h"
#include "SectionView.h"
#include "SectionCap.h"

#include <glm/gtc/type_ptr.hpp>

#include <gp_Dir.hxx>


#include <cstdio>

namespace materializr {


static const char* s_sectionVertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
uniform mat4 u_mvp;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

static const char* s_sectionFragSource = R"(
#version 330 core
uniform vec3 u_color;
out vec4 fragColor;
void main() {
    fragColor = vec4(u_color, 1.0);
}
)";

// Cap fill: the cross-section is planar (one normal for the whole cap), so a
// flat shade off that normal is enough to read as solid cut material.
static const char* s_capVertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
uniform mat4 u_mvp;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

static const char* s_capFragSource = R"(
#version 330 core
uniform vec3 u_color;
uniform vec3 u_normal; // cut-plane normal (world)
out vec4 fragColor;
void main() {
    vec3 L = normalize(vec3(0.4, 0.7, 0.6));
    float d = 0.45 + 0.55 * abs(dot(normalize(u_normal), L));
    fragColor = vec4(u_color * d, 1.0);
}
)";

SectionView::SectionView() {}

SectionView::~SectionView() {
    if (m_program) glDeleteProgram(m_program);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_capProgram) glDeleteProgram(m_capProgram);
    if (m_capVao) glDeleteVertexArrays(1, &m_capVao);
    if (m_capVbo) glDeleteBuffers(1, &m_capVbo);
}

bool SectionView::initialize() {
    unsigned int vert = 0, frag = 0;
    if (!compileShader(vert, GL_VERTEX_SHADER, s_sectionVertSource)) return false;
    if (!compileShader(frag, GL_FRAGMENT_SHADER, s_sectionFragSource)) {
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
        std::fprintf(stderr, "SectionView link error: %s\n", log);
        glDeleteProgram(m_program);
        m_program = 0;
        return false;
    }

    m_locMVP = glGetUniformLocation(m_program, "u_mvp");
    m_locColor = glGetUniformLocation(m_program, "u_color");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    // Cap fill program
    unsigned int cvert = 0, cfrag = 0;
    if (!compileShader(cvert, GL_VERTEX_SHADER, s_capVertSource)) return false;
    if (!compileShader(cfrag, GL_FRAGMENT_SHADER, s_capFragSource)) {
        glDeleteShader(cvert);
        return false;
    }
    m_capProgram = glCreateProgram();
    glAttachShader(m_capProgram, cvert);
    glAttachShader(m_capProgram, cfrag);
    glLinkProgram(m_capProgram);
    glDeleteShader(cvert);
    glDeleteShader(cfrag);

    int capOk = 0;
    glGetProgramiv(m_capProgram, GL_LINK_STATUS, &capOk);
    if (!capOk) {
        char log[512];
        glGetProgramInfoLog(m_capProgram, 512, nullptr, log);
        std::fprintf(stderr, "SectionView cap link error: %s\n", log);
        glDeleteProgram(m_capProgram);
        m_capProgram = 0;
        return false;
    }

    m_capLocMVP = glGetUniformLocation(m_capProgram, "u_mvp");
    m_capLocColor = glGetUniformLocation(m_capProgram, "u_color");
    m_capLocNormal = glGetUniformLocation(m_capProgram, "u_normal");

    glGenVertexArrays(1, &m_capVao);
    glGenBuffers(1, &m_capVbo);

    return true;
}

void SectionView::setDocument(const Document* doc) {
    m_document = doc;
}

void SectionView::setPlane(const gp_Pln& plane) {
    m_plane = plane;
}

void SectionView::setOffset(float offset) {
    m_offset = offset;
}

void SectionView::setEnabled(bool enabled) {
    m_enabled = enabled;
}

bool SectionView::isEnabled() const {
    return m_enabled;
}

SectionView::Result SectionView::compute(const std::vector<BodyMesh>& bodies,
                                         const gp_Pln& cuttingPlane,
                                         const std::atomic<bool>* cancel) {
    Result out;
    {
        gp_Dir cn = cuttingPlane.Axis().Direction();
        out.capNormal = glm::vec3(static_cast<float>(cn.X()),
                                  static_cast<float>(cn.Y()),
                                  static_cast<float>(cn.Z()));
    }
    for (const BodyMesh& body : bodies) {
        if (cancel && cancel->load()) return out;
        SectionSlice slice;
        if (!sliceSection(body.faces, cuttingPlane, slice)) continue;
        for (size_t i = 0; i + 6 <= slice.lines.size(); i += 6) {
            out.lines.push_back({glm::vec3(slice.lines[i], slice.lines[i + 1], slice.lines[i + 2]),
                                 glm::vec3(slice.lines[i + 3], slice.lines[i + 4], slice.lines[i + 5])});
        }
        if (!slice.cap.empty()) {
            CapMesh cap;
            cap.color = body.color;
            cap.positions = std::move(slice.cap);
            out.caps.push_back(std::move(cap));
        }
    }
    return out;
}

void SectionView::apply(Result&& r) {
    m_lines = std::move(r.lines);
    m_caps = std::move(r.caps);
    m_capNormal = r.capNormal;
}

void SectionView::render(const glm::mat4& view, const glm::mat4& projection) {
    if (!m_enabled) return;

    glm::mat4 mvpCap = projection * view;

    // --- Filled caps first (depth-tested so walls in front still occlude) ---
    if (m_capProgram && !m_caps.empty()) {
        glUseProgram(m_capProgram);
        glUniformMatrix4fv(m_capLocMVP, 1, GL_FALSE, glm::value_ptr(mvpCap));
        glUniform3fv(m_capLocNormal, 1, glm::value_ptr(m_capNormal));

        const GLboolean cullWas = glIsEnabled(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE); // cap winding is not guaranteed toward camera

        glBindVertexArray(m_capVao);
        glBindBuffer(GL_ARRAY_BUFFER, m_capVbo);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        glEnableVertexAttribArray(0);
        for (const auto& cap : m_caps) {
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(cap.positions.size() * sizeof(float)),
                         cap.positions.data(), GL_DYNAMIC_DRAW);
            glUniform3fv(m_capLocColor, 1, glm::value_ptr(cap.color));
            glDrawArrays(GL_TRIANGLES, 0,
                         static_cast<GLsizei>(cap.positions.size() / 3));
        }
        glBindVertexArray(0);
        if (cullWas) glEnable(GL_CULL_FACE); // restore; do not leak state
    }

    if (m_lines.empty() || !m_program) {
        glUseProgram(0);
        return;
    }

    // Upload line data to VBO
    std::vector<float> vertices;
    vertices.reserve(m_lines.size() * 6);
    for (const auto& line : m_lines) {
        vertices.push_back(line.start.x);
        vertices.push_back(line.start.y);
        vertices.push_back(line.start.z);
        vertices.push_back(line.end.x);
        vertices.push_back(line.end.y);
        vertices.push_back(line.end.z);
    }

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(float)),
                 vertices.data(), GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);

    glm::mat4 mvp = projection * view;

    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(mvp));

    // Orange/red section line color
    glm::vec3 sectionColor(0.95f, 0.4f, 0.1f);
    glUniform3fv(m_locColor, 1, glm::value_ptr(sectionColor));

    // Disable depth test so section lines are always visible
    glDisable(GL_DEPTH_TEST);
    glLineWidth(2.0f);

    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices.size() / 3));

    glEnable(GL_DEPTH_TEST);
    glLineWidth(1.0f);

    glBindVertexArray(0);
    glUseProgram(0);
}

bool SectionView::compileShader(unsigned int& shader, unsigned int type, const char* source) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "SectionView shader error: %s\n", log);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

} // namespace materializr
