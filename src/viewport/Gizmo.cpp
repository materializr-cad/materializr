#include "gl_common.h"
#include "Gizmo.h"
#include "Camera.h"
#include "../touch_mode.h"

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

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
uniform vec3 u_color;
out vec4 fragColor;
void main() {
    fragColor = vec4(u_color, 1.0);
}
)";

static constexpr float kAxisLength = 1.0f;
static constexpr float kConeBaseRadius = 0.08f;
static constexpr float kConeHeight = 0.2f;
static constexpr float kShaftRadius = 0.025f;
static constexpr float kRingRadius = 0.75f;
static constexpr float kRingTubeRadius = 0.025f;
static constexpr float kCubeSize = 0.08f;
static constexpr float kCubeOffset = 1.05f;
static constexpr int kSegments = 16;
static constexpr int kRingSegments = 48;
static constexpr float kPickThreshold = 0.04f;

// World-size factor the gizmo scales by (× camera distance). A fingertip is far
// less precise than a cursor, so grow the whole widget on touch - the visual
// arrows/rings AND their hit-tests both derive from this, so they enlarge
// together (same approach as the ViewCube).
static float gizmoScale() {
    return materializr::touchMode() ? 0.225f : 0.15f;
}

Gizmo::Gizmo() {}

Gizmo::~Gizmo() {
    if (m_program) glDeleteProgram(m_program);
    if (m_arrowVao) glDeleteVertexArrays(1, &m_arrowVao);
    if (m_arrowVbo) glDeleteBuffers(1, &m_arrowVbo);
    if (m_ringVao) glDeleteVertexArrays(1, &m_ringVao);
    if (m_ringVbo) glDeleteBuffers(1, &m_ringVbo);
    if (m_cubeVao) glDeleteVertexArrays(1, &m_cubeVao);
    if (m_cubeVbo) glDeleteBuffers(1, &m_cubeVbo);
}

bool Gizmo::initialize() {
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
        std::fprintf(stderr, "Gizmo link error: %s\n", log);
        return false;
    }
    m_locMVP = glGetUniformLocation(m_program, "u_mvp");
    m_locColor = glGetUniformLocation(m_program, "u_color");

    glGenVertexArrays(1, &m_arrowVao);
    glGenBuffers(1, &m_arrowVbo);
    glGenVertexArrays(1, &m_ringVao);
    glGenBuffers(1, &m_ringVbo);
    glGenVertexArrays(1, &m_cubeVao);
    glGenBuffers(1, &m_cubeVbo);

    buildArrowMesh();
    buildRingMesh();
    buildCubeMesh();
    return true;
}

static void uploadMesh(unsigned int vao, unsigned int vbo,
                       const std::vector<float>& verts, int& vertCount) {
    vertCount = static_cast<int>(verts.size() / 3);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void Gizmo::buildArrowMesh() {
    std::vector<float> verts;
    auto addTri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
        verts.insert(verts.end(), {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z});
    };

    // Shaft cylinder along +Z
    for (int i = 0; i < kSegments; ++i) {
        float a1 = 2.0f * (float)M_PI * i / kSegments;
        float a2 = 2.0f * (float)M_PI * (i + 1) / kSegments;
        float c1 = std::cos(a1), s1 = std::sin(a1);
        float c2 = std::cos(a2), s2 = std::sin(a2);
        glm::vec3 p0(kShaftRadius*c1, kShaftRadius*s1, 0.0f);
        glm::vec3 p1(kShaftRadius*c2, kShaftRadius*s2, 0.0f);
        glm::vec3 p2(kShaftRadius*c2, kShaftRadius*s2, kAxisLength);
        glm::vec3 p3(kShaftRadius*c1, kShaftRadius*s1, kAxisLength);
        addTri(p0, p1, p2);
        addTri(p0, p2, p3);
    }

    // Cone tip
    glm::vec3 tip(0, 0, kAxisLength + kConeHeight);
    for (int i = 0; i < kSegments; ++i) {
        float a1 = 2.0f * (float)M_PI * i / kSegments;
        float a2 = 2.0f * (float)M_PI * (i + 1) / kSegments;
        glm::vec3 b1(kConeBaseRadius*std::cos(a1), kConeBaseRadius*std::sin(a1), kAxisLength);
        glm::vec3 b2(kConeBaseRadius*std::cos(a2), kConeBaseRadius*std::sin(a2), kAxisLength);
        addTri(b1, b2, tip);
        addTri(glm::vec3(0,0,kAxisLength), b2, b1);
    }

    uploadMesh(m_arrowVao, m_arrowVbo, verts, m_arrowVertCount);
}

void Gizmo::buildRingMesh() {
    // A torus arc (270 degrees) in the XY plane centered at origin
    std::vector<float> verts;
    auto addTri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
        verts.insert(verts.end(), {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z});
    };

    float arcAngle = 2.0f * (float)M_PI * 0.75f; // 270 degrees
    int tubeSegs = 8;

    for (int i = 0; i < kRingSegments; ++i) {
        float theta1 = arcAngle * i / kRingSegments;
        float theta2 = arcAngle * (i + 1) / kRingSegments;

        for (int j = 0; j < tubeSegs; ++j) {
            float phi1 = 2.0f * (float)M_PI * j / tubeSegs;
            float phi2 = 2.0f * (float)M_PI * (j + 1) / tubeSegs;

            auto torusPoint = [&](float theta, float phi) -> glm::vec3 {
                float r = kRingRadius + kRingTubeRadius * std::cos(phi);
                return glm::vec3(r * std::cos(theta), r * std::sin(theta),
                                 kRingTubeRadius * std::sin(phi));
            };

            glm::vec3 p00 = torusPoint(theta1, phi1);
            glm::vec3 p10 = torusPoint(theta2, phi1);
            glm::vec3 p11 = torusPoint(theta2, phi2);
            glm::vec3 p01 = torusPoint(theta1, phi2);

            addTri(p00, p10, p11);
            addTri(p00, p11, p01);
        }
    }

    uploadMesh(m_ringVao, m_ringVbo, verts, m_ringVertCount);
}

void Gizmo::buildCubeMesh() {
    // A shaft along +Z with a small cube at the end (mirrors the move arrow's
    // shaft+cone, so scale handles read as bars with a knob).
    std::vector<float> verts;
    auto addTri = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
        verts.insert(verts.end(), {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z});
    };

    // Shaft cylinder from origin to the cube.
    for (int i = 0; i < kSegments; ++i) {
        float a1 = 2.0f * (float)M_PI * i / kSegments;
        float a2 = 2.0f * (float)M_PI * (i + 1) / kSegments;
        float c1 = std::cos(a1), s1 = std::sin(a1);
        float c2 = std::cos(a2), s2 = std::sin(a2);
        glm::vec3 p0(kShaftRadius*c1, kShaftRadius*s1, 0.0f);
        glm::vec3 p1(kShaftRadius*c2, kShaftRadius*s2, 0.0f);
        glm::vec3 p2(kShaftRadius*c2, kShaftRadius*s2, kCubeOffset);
        glm::vec3 p3(kShaftRadius*c1, kShaftRadius*s1, kCubeOffset);
        addTri(p0, p1, p2);
        addTri(p0, p2, p3);
    }

    float s = kCubeSize;
    glm::vec3 o(0, 0, kCubeOffset);
    glm::vec3 corners[8] = {
        o + glm::vec3(-s,-s,-s), o + glm::vec3(+s,-s,-s),
        o + glm::vec3(+s,+s,-s), o + glm::vec3(-s,+s,-s),
        o + glm::vec3(-s,-s,+s), o + glm::vec3(+s,-s,+s),
        o + glm::vec3(+s,+s,+s), o + glm::vec3(-s,+s,+s),
    };

    // 6 faces, 2 triangles each
    int faces[6][4] = {
        {0,1,2,3}, {5,4,7,6}, // front, back
        {4,0,3,7}, {1,5,6,2}, // left, right
        {3,2,6,7}, {4,5,1,0}, // top, bottom
    };
    for (auto& f : faces) {
        addTri(corners[f[0]], corners[f[1]], corners[f[2]]);
        addTri(corners[f[0]], corners[f[2]], corners[f[3]]);
    }

    uploadMesh(m_cubeVao, m_cubeVbo, verts, m_cubeVertCount);
}

void Gizmo::setMode(GizmoMode mode) { m_mode = mode; }
GizmoMode Gizmo::getMode() const { return m_mode; }
void Gizmo::setPosition(glm::vec3 pos) { m_position = pos; }
void Gizmo::setVisible(bool vis) { m_visible = vis; }
bool Gizmo::isVisible() const { return m_visible; }

void Gizmo::cancelDrag() {
    m_draggingAxis = GizmoAxis::None;
    m_hoveredAxis = GizmoAxis::None;
}

glm::vec2 Gizmo::worldToNDC(glm::vec3 pos, const glm::mat4& vp) {
    glm::vec4 clip = vp * glm::vec4(pos, 1.0f);
    if (std::abs(clip.w) < 1e-6f) return glm::vec2(1e6f);
    return glm::vec2(clip.x / clip.w, clip.y / clip.w);
}

float Gizmo::screenDistToSegment(glm::vec2 mouse, glm::vec2 a, glm::vec2 b) {
    glm::vec2 ab = b - a;
    float len2 = glm::dot(ab, ab);
    if (len2 < 1e-12f) return glm::length(mouse - a);
    float t = glm::clamp(glm::dot(mouse - a, ab) / len2, 0.0f, 1.0f);
    return glm::length(mouse - (a + t * ab));
}

void Gizmo::renderArrowAlong(const glm::mat4& view, const glm::mat4& projection,
                            glm::vec3 position, glm::vec3 dir, glm::vec3 color) {
    if (!m_program || !m_arrowVao) return;
    glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    float scale = glm::length(camPos - position) * gizmoScale();
    if (scale < 0.01f) scale = 0.01f;

    // Rotation mapping the +Z canonical arrow onto `dir`.
    glm::vec3 d = glm::normalize(dir);
    glm::vec3 z(0.0f, 0.0f, 1.0f);
    glm::vec3 ax = glm::cross(z, d);
    float axl = glm::length(ax);
    float c = glm::clamp(glm::dot(z, d), -1.0f, 1.0f);
    glm::mat4 rot(1.0f);
    if (axl < 1e-6f) {
        if (c < 0.0f) rot = glm::rotate(glm::mat4(1.0f), glm::radians(180.0f),
                                        glm::vec3(1.0f, 0.0f, 0.0f));
    } else {
        rot = glm::rotate(glm::mat4(1.0f), std::acos(c), ax / axl);
    }
    glm::mat4 model = glm::translate(glm::mat4(1.0f), position)
                    * glm::scale(glm::mat4(1.0f), glm::vec3(scale)) * rot;
    glm::mat4 mvp = projection * view * model;

    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform3fv(m_locColor, 1, glm::value_ptr(color));
    glBindVertexArray(m_arrowVao);
    glDrawArrays(GL_TRIANGLES, 0, m_arrowVertCount);
    glBindVertexArray(0);
}

void Gizmo::renderRingAbout(const glm::mat4& view, const glm::mat4& projection,
                            glm::vec3 position, glm::vec3 axisDir, glm::vec3 color) {
    if (!m_program || !m_ringVao) return;
    glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    float scale = glm::length(camPos - position) * gizmoScale();
    if (scale < 0.01f) scale = 0.01f;

    // The ring mesh lies in the XY plane (axis +Z); rotate +Z onto axisDir so it
    // sits in the plane perpendicular to the tilt axis.
    glm::vec3 d = glm::normalize(axisDir);
    glm::vec3 z(0.0f, 0.0f, 1.0f);
    glm::vec3 ax = glm::cross(z, d);
    float axl = glm::length(ax);
    float c = glm::clamp(glm::dot(z, d), -1.0f, 1.0f);
    glm::mat4 rot(1.0f);
    if (axl < 1e-6f) {
        if (c < 0.0f) rot = glm::rotate(glm::mat4(1.0f), glm::radians(180.0f),
                                        glm::vec3(1.0f, 0.0f, 0.0f));
    } else {
        rot = glm::rotate(glm::mat4(1.0f), std::acos(c), ax / axl);
    }
    glm::mat4 model = glm::translate(glm::mat4(1.0f), position)
                    * glm::scale(glm::mat4(1.0f), glm::vec3(scale)) * rot;
    glm::mat4 mvp = projection * view * model;

    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform3fv(m_locColor, 1, glm::value_ptr(color));
    glBindVertexArray(m_ringVao);
    glDrawArrays(GL_TRIANGLES, 0, m_ringVertCount);
    glBindVertexArray(0);
}

void Gizmo::renderCubeAlong(const glm::mat4& view, const glm::mat4& projection,
                            glm::vec3 position, glm::vec3 dir, glm::vec3 color) {
    if (!m_program || !m_cubeVao) return;
    glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    float scale = glm::length(camPos - position) * gizmoScale();
    if (scale < 0.01f) scale = 0.01f;

    glm::vec3 d = glm::normalize(dir);
    glm::vec3 z(0.0f, 0.0f, 1.0f);
    glm::vec3 ax = glm::cross(z, d);
    float axl = glm::length(ax);
    float c = glm::clamp(glm::dot(z, d), -1.0f, 1.0f);
    glm::mat4 rot(1.0f);
    if (axl < 1e-6f) {
        if (c < 0.0f) rot = glm::rotate(glm::mat4(1.0f), glm::radians(180.0f),
                                        glm::vec3(1.0f, 0.0f, 0.0f));
    } else {
        rot = glm::rotate(glm::mat4(1.0f), std::acos(c), ax / axl);
    }
    glm::mat4 model = glm::translate(glm::mat4(1.0f), position)
                    * glm::scale(glm::mat4(1.0f), glm::vec3(scale)) * rot;
    glm::mat4 mvp = projection * view * model;

    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_program);
    glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniform3fv(m_locColor, 1, glm::value_ptr(color));
    glBindVertexArray(m_cubeVao);
    glDrawArrays(GL_TRIANGLES, 0, m_cubeVertCount);
    glBindVertexArray(0);
}

void Gizmo::render(const glm::mat4& view, const glm::mat4& projection) {
    if (!m_visible || !m_program) return;

    glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);
    float dist = glm::length(camPos - m_position);
    float scale = dist * gizmoScale();
    if (scale < 0.01f) scale = 0.01f;

    glm::mat4 vp = projection * view;

    // Rotation matrices to align +Z mesh with each axis
    glm::mat4 rotX = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0,1,0));
    glm::mat4 rotY = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1,0,0));
    glm::mat4 rotZ = glm::mat4(1.0f);

    // Ring rotations: ring is in XY plane, rotate to align with each axis's rotation plane
    // X rotation ring: rotate ring so it sits in YZ plane
    glm::mat4 ringRotX = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0,1,0));
    // Y rotation ring: rotate ring so it sits in XZ plane
    glm::mat4 ringRotY = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0));
    // Z rotation ring: ring already in XY plane
    glm::mat4 ringRotZ = glm::mat4(1.0f);

    struct DrawInfo {
        GizmoAxis axis;
        GizmoMode mode;
        glm::vec3 baseColor;
        glm::mat4 rotation;
        unsigned int vao;
        int vertCount;
    };

    DrawInfo draws[] = {
        // Translate arrows
        {GizmoAxis::X, GizmoMode::Translate, {0.9f,0.2f,0.2f}, rotX, m_arrowVao, m_arrowVertCount},
        {GizmoAxis::Y, GizmoMode::Translate, {0.2f,0.9f,0.2f}, rotY, m_arrowVao, m_arrowVertCount},
        {GizmoAxis::Z, GizmoMode::Translate, {0.3f,0.4f,0.95f}, rotZ, m_arrowVao, m_arrowVertCount},
        // Rotate rings
        {GizmoAxis::X, GizmoMode::Rotate, {0.9f,0.2f,0.2f}, ringRotX, m_ringVao, m_ringVertCount},
        {GizmoAxis::Y, GizmoMode::Rotate, {0.2f,0.9f,0.2f}, ringRotY, m_ringVao, m_ringVertCount},
        {GizmoAxis::Z, GizmoMode::Rotate, {0.3f,0.4f,0.95f}, ringRotZ, m_ringVao, m_ringVertCount},
        // Scale cubes
        {GizmoAxis::X, GizmoMode::Scale, {0.9f,0.2f,0.2f}, rotX, m_cubeVao, m_cubeVertCount},
        {GizmoAxis::Y, GizmoMode::Scale, {0.2f,0.9f,0.2f}, rotY, m_cubeVao, m_cubeVertCount},
        {GizmoAxis::Z, GizmoMode::Scale, {0.3f,0.4f,0.95f}, rotZ, m_cubeVao, m_cubeVertCount},
    };

    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_program);

    for (const auto& d : draws) {
        if (d.mode != m_mode) continue; // only the active mode's handles
        glm::mat4 model = glm::translate(glm::mat4(1.0f), m_position)
                        * glm::scale(glm::mat4(1.0f), glm::vec3(scale))
                        * d.rotation;
        glm::mat4 mvp = vp * model;

        glm::vec3 color = d.baseColor;
        if (m_draggingAxis == d.axis && m_draggingMode == d.mode)
            color = glm::clamp(color * 1.8f, glm::vec3(0), glm::vec3(1));
        else if (m_hoveredAxis == d.axis && m_hoveredMode == d.mode)
            color = glm::clamp(color * 1.4f, glm::vec3(0), glm::vec3(1));

        glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniform3fv(m_locColor, 1, glm::value_ptr(color));

        glBindVertexArray(d.vao);
        glDrawArrays(GL_TRIANGLES, 0, d.vertCount);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

Gizmo::PickResult Gizmo::pickNearest(float mx, float my, float vpW, float vpH,
                                      const Camera& camera) {
    PickResult best;
    best.axis = GizmoAxis::None;
    best.mode = GizmoMode::Translate;
    // Widen the catch radius on touch - a fingertip can't land on a thin arrow
    // line as precisely as a cursor. (The widget itself is also larger on touch
    // via gizmoScale(), so this is the matching slack around it.)
    best.dist = kPickThreshold * (materializr::touchMode() ? 2.5f : 1.0f);

    glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
    glm::vec3 camPos = camera.getPosition();
    float dist = glm::length(camPos - m_position);
    float scale = dist * gizmoScale();

    float ndcX = (mx / vpW) * 2.0f - 1.0f;
    float ndcY = 1.0f - (my / vpH) * 2.0f;
    glm::vec2 mouse(ndcX, ndcY);

    glm::vec2 originNDC = worldToNDC(m_position, vp);

    glm::vec3 axisDirs[3] = {{1,0,0}, {0,1,0}, {0,0,1}};
    GizmoAxis axisIds[3] = {GizmoAxis::X, GizmoAxis::Y, GizmoAxis::Z};

    // Only the active mode's handles are pickable - otherwise the long translate
    // arrow lines would steal clicks meant for the scale cubes / rotate rings.
    // Test translate arrows (line from origin to tip)
    if (m_mode == GizmoMode::Translate)
    for (int i = 0; i < 3; i++) {
        glm::vec3 tip = m_position + axisDirs[i] * scale * (kAxisLength + kConeHeight);
        glm::vec2 tipNDC = worldToNDC(tip, vp);
        float d = screenDistToSegment(mouse, originNDC, tipNDC);
        if (d < best.dist) {
            best.dist = d;
            best.axis = axisIds[i];
            best.mode = GizmoMode::Translate;
        }
    }

    // Test scale cubes (small region around cube endpoint)
    if (m_mode == GizmoMode::Scale)
    for (int i = 0; i < 3; i++) {
        glm::vec3 cubePos = m_position + axisDirs[i] * scale * kCubeOffset;
        glm::vec2 cubeNDC = worldToNDC(cubePos, vp);
        float d = glm::length(mouse - cubeNDC);
        if (d < best.dist) {
            best.dist = d;
            best.axis = axisIds[i];
            best.mode = GizmoMode::Scale;
        }
    }

    // Test rotate rings (sample points along the arc)
    if (m_mode == GizmoMode::Rotate) {
    glm::mat4 ringRots[3] = {
        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0,1,0)),
        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1,0,0)),
        glm::mat4(1.0f),
    };
    for (int i = 0; i < 3; i++) {
        float arcAngle = 2.0f * (float)M_PI * 0.75f;
        glm::vec2 prevNDC(0);
        for (int s = 0; s <= kRingSegments; s++) {
            float theta = arcAngle * s / kRingSegments;
            glm::vec3 localPt(kRingRadius * std::cos(theta), kRingRadius * std::sin(theta), 0);
            glm::vec3 worldPt = m_position + glm::vec3(
                glm::translate(glm::mat4(1.0f), glm::vec3(0))
                * glm::scale(glm::mat4(1.0f), glm::vec3(scale))
                * ringRots[i] * glm::vec4(localPt, 1.0f));
            // Simpler: transform localPt
            glm::vec4 transformed = ringRots[i] * glm::vec4(localPt, 1.0f);
            worldPt = m_position + glm::vec3(transformed) * scale;

            glm::vec2 ptNDC = worldToNDC(worldPt, vp);
            if (s > 0) {
                float d = screenDistToSegment(mouse, prevNDC, ptNDC);
                if (d < best.dist) {
                    best.dist = d;
                    best.axis = axisIds[i];
                    best.mode = GizmoMode::Rotate;
                }
            }
            prevNDC = ptNDC;
        }
    }
    } // m_mode == Rotate

    return best;
}

glm::vec3 Gizmo::projectOnAxis(float mx, float my, float vpW, float vpH,
                                const Camera& camera, GizmoAxis axis) {
    glm::mat4 invVP = glm::inverse(camera.getProjectionMatrix() * camera.getViewMatrix());

    float ndcX = (mx / vpW) * 2.0f - 1.0f;
    float ndcY = 1.0f - (my / vpH) * 2.0f;

    glm::vec4 nearClip = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farClip  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
    glm::vec3 rayOrigin = glm::vec3(nearClip) / nearClip.w;
    glm::vec3 rayDir = glm::normalize(glm::vec3(farClip) / farClip.w - rayOrigin);

    glm::vec3 axisDir(0);
    if (axis == GizmoAxis::X) axisDir = glm::vec3(1,0,0);
    else if (axis == GizmoAxis::Y) axisDir = glm::vec3(0,1,0);
    else axisDir = glm::vec3(0,0,1);

    glm::vec3 w0 = m_position - rayOrigin;
    float a = 1.0f; // dot(axisDir, axisDir)
    float b = glm::dot(axisDir, rayDir);
    float c = 1.0f; // dot(rayDir, rayDir)
    float d = glm::dot(axisDir, w0);
    float e = glm::dot(rayDir, w0);
    float denom = a * c - b * b;
    float t = (std::abs(denom) > 1e-8f) ? (b * e - c * d) / denom : 0.0f;

    return m_position + t * axisDir;
}

bool Gizmo::ringAngle(float mx, float my, float vpW, float vpH,
                      const Camera& camera, GizmoAxis axis, float& outDeg) {
    glm::mat4 invVP = glm::inverse(camera.getProjectionMatrix() * camera.getViewMatrix());
    float ndcX = (mx / vpW) * 2.0f - 1.0f;
    float ndcY = 1.0f - (my / vpH) * 2.0f;
    glm::vec4 nearClip = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farClip  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
    glm::vec3 rayOrigin = glm::vec3(nearClip) / nearClip.w;
    glm::vec3 rayDir = glm::normalize(glm::vec3(farClip) / farClip.w - rayOrigin);

    // Axis (plane normal) and an in-plane basis (u, v) with u x v = axis.
    glm::vec3 n, u, v;
    if (axis == GizmoAxis::X)      { n = {1,0,0}; u = {0,1,0}; v = {0,0,1}; }
    else if (axis == GizmoAxis::Y) { n = {0,1,0}; u = {0,0,1}; v = {1,0,0}; }
    else                           { n = {0,0,1}; u = {1,0,0}; v = {0,1,0}; }

    float denom = glm::dot(rayDir, n);
    if (std::abs(denom) < 1e-6f) return false; // ray parallel to the ring plane
    float t = glm::dot(m_position - rayOrigin, n) / denom;
    glm::vec3 hit = rayOrigin + rayDir * t;
    glm::vec3 vec = hit - m_position;
    outDeg = glm::degrees(std::atan2(glm::dot(vec, v), glm::dot(vec, u)));
    return true;
}

GizmoResult Gizmo::handleInput(float mouseX, float mouseY,
                                float vpWidth, float vpHeight,
                                bool mouseDown, bool mouseJustPressed,
                                const Camera& camera) {
    GizmoResult result;
    if (!m_visible) return result;

    if (mouseJustPressed) {
        auto pick = pickNearest(mouseX, mouseY, vpWidth, vpHeight, camera);
        if (pick.axis != GizmoAxis::None) {
            m_draggingAxis = pick.axis;
            m_draggingMode = pick.mode;
            if (pick.mode == GizmoMode::Rotate) {
                ringAngle(mouseX, mouseY, vpWidth, vpHeight, camera, pick.axis, m_lastDragAngle);
            } else {
                m_lastDragPos = projectOnAxis(mouseX, mouseY, vpWidth, vpHeight, camera, pick.axis);
            }
        }
    }

    if (mouseDown && m_draggingAxis != GizmoAxis::None) {
        glm::vec3 axisDir = (m_draggingAxis == GizmoAxis::X) ? glm::vec3(1,0,0)
                          : (m_draggingAxis == GizmoAxis::Y) ? glm::vec3(0,1,0)
                                                             : glm::vec3(0,0,1);
        if (m_draggingMode == GizmoMode::Rotate) {
            // Rotation: signed angle change as the mouse goes around the ring,
            // encoded as axisDir * degrees so the app reads it via dot(delta,axis).
            float ang;
            if (ringAngle(mouseX, mouseY, vpWidth, vpHeight, camera, m_draggingAxis, ang)) {
                float dDeg = ang - m_lastDragAngle;
                while (dDeg > 180.0f) dDeg -= 360.0f;   // shortest direction
                while (dDeg < -180.0f) dDeg += 360.0f;
                m_lastDragAngle = ang;
                result.changed = true;
                result.activeAxis = m_draggingAxis;
                result.mode = m_draggingMode;
                result.delta = axisDir * dDeg;
            }
        } else {
            // Translate / Scale: movement along the axis line.
            glm::vec3 currentPos = projectOnAxis(mouseX, mouseY, vpWidth, vpHeight,
                                                  camera, m_draggingAxis);
            glm::vec3 delta = currentPos - m_lastDragPos;
            m_lastDragPos = currentPos;
            result.changed = true;
            result.activeAxis = m_draggingAxis;
            result.mode = m_draggingMode;
            result.delta = delta;
        }
    }

    if (!mouseDown) {
        m_draggingAxis = GizmoAxis::None;
        auto pick = pickNearest(mouseX, mouseY, vpWidth, vpHeight, camera);
        m_hoveredAxis = pick.axis;
        m_hoveredMode = pick.mode;
    }

    if (m_draggingAxis != GizmoAxis::None) {
        result.activeAxis = m_draggingAxis;
    }

    return result;
}

bool Gizmo::compileShader(unsigned int& shader, unsigned int type, const char* source) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "Gizmo shader error: %s\n", log);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

} // namespace materializr
