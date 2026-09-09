#include "RefImageRenderer.h"
#include "../io/ImageDecode.h"

#include <glm/gtc/type_ptr.hpp>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <cstdio>

namespace materializr {

// Textured quad. UVs flip V because stb_image decodes top-left-origin rows
// while GL samples bottom-left - the flip happens in the corner/UV pairing
// (see render()), not in the shader.
static const char* s_vertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
uniform mat4 u_mvp;
out vec2 v_uv;
void main() {
    v_uv = a_uv;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

static const char* s_fragSource = R"(
#version 330 core
uniform sampler2D u_tex;
uniform float u_opacity;
in vec2 v_uv;
out vec4 fragColor;
void main() {
    vec4 c = texture(u_tex, v_uv);
    fragColor = vec4(c.rgb, c.a * u_opacity);
}
)";

// Border line-loop (selection highlight) - plain color, same as PlaneRenderer.
static const char* s_lineVertSource = R"(
#version 330 core
layout(location = 0) in vec3 a_position;
uniform mat4 u_mvp;
void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

static const char* s_lineFragSource = R"(
#version 330 core
uniform vec4 u_color;
out vec4 fragColor;
void main() {
    fragColor = u_color;
}
)";

RefImageRenderer::RefImageRenderer() = default;

RefImageRenderer::~RefImageRenderer() {
    clearTextures();
    if (m_program) glDeleteProgram(m_program);
    if (m_lineProgram) glDeleteProgram(m_lineProgram);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
}

bool RefImageRenderer::initialize() {
    // Textured program.
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
    m_locTex     = glGetUniformLocation(m_program, "u_tex");

    // Border program (separate tiny program; reusing m_program's slot layout
    // but without UV/tex requirements).
    {
        unsigned int lvs = 0, lfs = 0;
        if (!compileShader(lvs, GL_VERTEX_SHADER, s_lineVertSource)) return false;
        if (!compileShader(lfs, GL_FRAGMENT_SHADER, s_lineFragSource)) {
            glDeleteShader(lvs);
            return false;
        }
        m_lineProgram = glCreateProgram();
        glAttachShader(m_lineProgram, lvs);
        glAttachShader(m_lineProgram, lfs);
        glLinkProgram(m_lineProgram);
        glDeleteShader(lvs); glDeleteShader(lfs);
        int ok = 0;
        glGetProgramiv(m_lineProgram, GL_LINK_STATUS, &ok);
        if (!ok) {
            std::fprintf(stderr, "RefImageRenderer: border program link failed\n");
            glDeleteProgram(m_lineProgram);
            m_lineProgram = 0;
            return false;
        }
        m_lineLocMVP   = glGetUniformLocation(m_lineProgram, "u_mvp");
        m_lineLocColor = glGetUniformLocation(m_lineProgram, "u_color");
    }

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // Interleaved x,y,z,u,v - the border pass just strides past the UVs.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void RefImageRenderer::sync(const std::vector<Item>& items) {
    m_items = items;

    // Upload textures for ids we haven't seen. Decode failures leave no cache
    // entry, so the item simply doesn't draw (and we retry next sync - cheap,
    // since a bad file keeps failing the same probe the panel already ran).
    for (const auto& it : m_items) {
        if (m_textures.count(it.planeId)) continue;
        if (!it.fileBytes || it.fileBytes->empty()) continue;
        DecodedImage img;
        if (!decodeImage(it.fileBytes->data(), it.fileBytes->size(), img)) {
            std::fprintf(stderr,
                "[RefImage] decode failed for plane %d (%zu bytes)\n",
                it.planeId, it.fileBytes->size());
            continue;
        }
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Pin unpack state so a prior upload's stray GL_UNPACK_ROW_LENGTH /
        // alignment can't garble this tightly-packed RGBA upload (same trap the
        // logo and calibration-preview textures hit). Save/pin/restore.
        GLint prevRowLen = 0, prevAlign = 4;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prevRowLen);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, prevRowLen);
        glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_textures[it.planeId] = tex;
    }

    // The byte pointers alias Document storage and are only valid during this
    // call - null them so a stale dereference is impossible between syncs.
    for (auto& it : m_items) it.fileBytes = nullptr;

    // Evict textures whose planes left the document (image or plane deleted).
    for (auto it = m_textures.begin(); it != m_textures.end();) {
        bool live = false;
        for (const auto& item : m_items)
            if (item.planeId == it->first) { live = true; break; }
        if (!live) {
            glDeleteTextures(1, &it->second);
            it = m_textures.erase(it);
        } else {
            ++it;
        }
    }
}

void RefImageRenderer::render(const glm::mat4& view, const glm::mat4& projection) {
    if (!m_program || m_items.empty()) return;

    glm::mat4 vp = projection * view;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // NO depth write: the world grid now renders BEFORE this pass (so the
    // photo blends over it - real transparency), and the sketch grid renders
    // AFTER it with a small bias that must never lose to the quad - writing
    // depth here made the sketch grid vanish over the photo past a certain
    // zoom (bias shrinks with depth precision, the 0.05 mm lift doesn't).
    glDepthMask(GL_FALSE);
    // Photos read from both sides (mirrored from behind - physically honest).
    glDisable(GL_CULL_FACE);

    glBindVertexArray(m_vao);

    for (const auto& it : m_items) {
        auto texIt = m_textures.find(it.planeId);
        if (texIt == m_textures.end()) continue;

        const gp_Ax1& axis = it.plane.Axis();
        gp_Pnt origin = axis.Location();
        gp_Dir normal = axis.Direction();
        gp_Dir xDir = it.plane.Position().XDirection();
        gp_Dir yDir = it.plane.Position().YDirection();

        // Lift the quad 0.05 mm off its plane: the ground grid renders BEFORE
        // this pass with a depth bias, so an exactly-coplanar quad loses the
        // depth test and vanishes (the default import pose is the ground
        // plane - the common case). 0.05 mm is invisible for tracing and the
        // SKETCH still lands on the true plane, not the lifted quad.
        glm::vec3 o(static_cast<float>(origin.X() + normal.X() * 0.05),
                    static_cast<float>(origin.Y() + normal.Y() * 0.05),
                    static_cast<float>(origin.Z() + normal.Z() * 0.05));
        glm::vec3 dx(static_cast<float>(xDir.X()),
                     static_cast<float>(xDir.Y()),
                     static_cast<float>(xDir.Z()));
        glm::vec3 dy(static_cast<float>(yDir.X()),
                     static_cast<float>(yDir.Y()),
                     static_cast<float>(yDir.Z()));

        const float hw = static_cast<float>(it.widthMM * 0.5);
        const float hh = static_cast<float>(it.heightMM * 0.5);

        glm::vec3 c0 = o - hw * dx - hh * dy;   // bottom-left  (uv 0,1)
        glm::vec3 c1 = o + hw * dx - hh * dy;   // bottom-right (uv 1,1)
        glm::vec3 c2 = o + hw * dx + hh * dy;   // top-right    (uv 1,0)
        glm::vec3 c3 = o - hw * dx + hh * dy;   // top-left     (uv 0,0)
        // V flips (1 at the quad's -Y edge) because stb rows are top-first.

        const float verts[] = {
            c0.x, c0.y, c0.z, 0.0f, 1.0f,
            c1.x, c1.y, c1.z, 1.0f, 1.0f,
            c2.x, c2.y, c2.z, 1.0f, 0.0f,

            c0.x, c0.y, c0.z, 0.0f, 1.0f,
            c2.x, c2.y, c2.z, 1.0f, 0.0f,
            c3.x, c3.y, c3.z, 0.0f, 0.0f,
        };

        glUseProgram(m_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texIt->second);
        glUniform1i(m_locTex, 0);
        glUniformMatrix4fv(m_locMVP, 1, GL_FALSE, glm::value_ptr(vp));
        glUniform1f(m_locOpacity, it.opacity);

        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Border: amber when selected (matches the construction-plane
        // highlight); a faint neutral frame otherwise so a fully transparent
        // photo edge is still findable.
        if (m_lineProgram) {
            const float border[] = {
                c0.x, c0.y, c0.z, 0.0f, 0.0f,
                c1.x, c1.y, c1.z, 0.0f, 0.0f,
                c2.x, c2.y, c2.z, 0.0f, 0.0f,
                c3.x, c3.y, c3.z, 0.0f, 0.0f,
            };
            glUseProgram(m_lineProgram);
            glUniformMatrix4fv(m_lineLocMVP, 1, GL_FALSE, glm::value_ptr(vp));
            glm::vec4 bc = it.selected
                ? glm::vec4(1.00f, 0.78f, 0.20f, 1.00f)
                : glm::vec4(0.55f, 0.58f, 0.62f, 0.35f);
            glUniform4fv(m_lineLocColor, 1, glm::value_ptr(bc));
            glBufferData(GL_ARRAY_BUFFER, sizeof(border), border, GL_DYNAMIC_DRAW);
            glLineWidth(it.selected ? 3.0f : 1.5f);
            glDrawArrays(GL_LINE_LOOP, 0, 4);
        }
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

void RefImageRenderer::clearTextures() {
    for (auto& [id, tex] : m_textures)
        glDeleteTextures(1, &tex);
    m_textures.clear();
}

bool RefImageRenderer::compileShader(unsigned int& shader, unsigned int type,
                                     const char* src) {
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        std::fprintf(stderr, "RefImageRenderer shader compile failed: %s\n", log);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

bool RefImageRenderer::linkProgram(unsigned int vertShader, unsigned int fragShader) {
    m_program = glCreateProgram();
    glAttachShader(m_program, vertShader);
    glAttachShader(m_program, fragShader);
    glLinkProgram(m_program);
    int ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_program, 512, nullptr, log);
        std::fprintf(stderr, "RefImageRenderer link failed: %s\n", log);
        glDeleteProgram(m_program);
        m_program = 0;
        return false;
    }
    return true;
}

} // namespace materializr
