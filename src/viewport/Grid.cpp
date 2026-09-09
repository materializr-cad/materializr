#include "gl_common.h"

#include "Grid.h"

#include <glm/gtc/type_ptr.hpp>
#include <cstdio>

namespace materializr {

// Embedded grid shader sources (matching shaders/grid.vert and shaders/grid.frag)
static const char* s_gridVertSource = R"(
#version 330 core

uniform mat4 u_viewProjection;
uniform mat4 u_invViewProjection;

out vec3 v_nearPoint;
out vec3 v_farPoint;

vec3 gridPlane[6] = vec3[](
    vec3( 1,  1, 0), vec3(-1, -1, 0), vec3(-1,  1, 0),
    vec3(-1, -1, 0), vec3( 1,  1, 0), vec3( 1, -1, 0)
);

vec3 unprojectPoint(float x, float y, float z) {
    vec4 unprojected = u_invViewProjection * vec4(x, y, z, 1.0);
    return unprojected.xyz / unprojected.w;
}

void main() {
    vec3 p = gridPlane[gl_VertexID];
    // Use the true near plane (NDC z = -1), not z = 0. Under orthographic
    // projection depth is linear, so z = 0 is the mid-depth point, which can sit
    // behind the target plane and make the ray parameter negative - discarding
    // the whole grid. The near plane is always in front of the camera.
    v_nearPoint = unprojectPoint(p.x, p.y, -1.0);
    v_farPoint  = unprojectPoint(p.x, p.y,  1.0);
    gl_Position = vec4(p, 1.0);
}
)";

static const char* s_gridFragSource = R"(
#version 330 core

in vec3 v_nearPoint;
in vec3 v_farPoint;

uniform mat4 u_viewProjection;
uniform vec3 u_fadeCenter;     // grid fades out around this world point
uniform float u_fadeDistance;  // world-space fade radius
uniform vec3 u_planeOrigin;
uniform vec3 u_planeU;         // in-plane basis (grid X), unit length
uniform vec3 u_planeV;         // in-plane basis (grid Y), unit length
uniform vec3 u_planeNormal;
uniform float u_scale;         // lines every 1/u_scale plane units (minor)
uniform float u_minorAlpha;    // 0..1 multiplier on the minor (1×) tier; 0 hides it
uniform float u_globalAlpha;   // 0..1 multiplier on the final grid alpha
uniform float u_sketchGrid;    // 1 = sketch grid: one uniform tier (no plaid)
uniform float u_depthBias;     // signed depth nudge: + toward camera (draw the
                               // grid ON a coplanar face, e.g. the sketch face),
                               // - away (let a coplanar body face occlude it)
uniform float u_lightBg;       // 1 = light viewport: use dark-on-light line palette
uniform float u_sketchThickness; // sketch grid line-width multiplier (1 = default)

out vec4 fragColor;

float computeDepth(vec3 pos) {
    vec4 clipPos = u_viewProjection * vec4(pos, 1.0);
    return (clipPos.z / clipPos.w) * 0.5 + 0.5;
}

// Grid line coverage with a CONSTANT pixel width and a flat (box) profile, so a
// line keeps the same thickness at every opacity and simply dims uniformly when
// the alpha is lowered - instead of a bright-cored triangle whose sides get
// eaten as you fade, which reads as lines changing thickness ("plaid"). `coord`
// is in cell units (one integer step = one grid cell); `widthPx` is the line
// width in pixels. When cells go sub-pixel the line just widens to fill and the
// grid greys out evenly, so there is no moiré beat pattern either.
float gridCoverage(vec2 coord, float widthPx) {
    vec2 deriv = fwidth(coord);                       // cell units per pixel
    vec2 toLine = abs(fract(coord - 0.5) - 0.5);      // 0 at a line, 0.5 mid-cell
    vec2 halfW = deriv * (widthPx * 0.5);             // half line width, cell units
    vec2 aa = deriv;                                  // one-pixel anti-alias band
    // Flat top out to `halfW`, then a one-pixel linear ramp to 0. Constant width,
    // uniform brightness across the line - dims cleanly under a global alpha.
    vec2 cov = clamp((halfW + aa - toLine) / aa, 0.0, 1.0);
    return max(cov.x, cov.y);
}

void main() {
    vec3 dir = v_farPoint - v_nearPoint;
    float denom = dot(dir, u_planeNormal);
    if (abs(denom) < 1e-6) discard;
    float t = dot(u_planeOrigin - v_nearPoint, u_planeNormal) / denom;
    if (t < 0.0) discard; // plane behind the camera

    vec3 fragPos3D = v_nearPoint + t * dir;
    // Bias the grid's depth slightly toward the camera so it doesn't z-fight
    // with geometry that lies on the plane (the common "sketch on a face"
    // case). The bias is computed in world space - a fixed NDC offset would
    // drift with depth and make the grid appear to lift off the plane as you
    // zoom out. We push the world position toward the near point by a fraction
    // of the ray length, so the bias stays visually tight at any scale.
    vec3 toNear = v_nearPoint - fragPos3D;
    float rayLen = length(toNear);
    vec3 biasedPos = fragPos3D + (toNear / max(rayLen, 1e-6)) * (rayLen * u_depthBias);
    gl_FragDepth = computeDepth(biasedPos);

    // In-plane coordinates relative to the plane origin.
    vec3 rel = fragPos3D - u_planeOrigin;
    vec2 uv = vec2(dot(rel, u_planeU), dot(rel, u_planeV));

    // Distance-based fade (works in both perspective and orthographic).
    float dist = length(fragPos3D - u_fadeCenter);
    float fade = clamp(1.0 - dist / max(u_fadeDistance, 1e-3), 0.0, 1.0);

    // The finest (1×) tier, drawn with the anti-moiré pristine-grid coverage.
    // The thickness multiplier scales every tier's line width (sketch AND the
    // world/ground grid) so the slider reads the same way the opacity one does.
    float covMinor = gridCoverage(uv * u_scale, 1.3 * u_sketchThickness);

    // Line palettes. On the dark viewport the lines are light and coarser tiers
    // get BRIGHTER (more prominent); on the light viewport they flip to dark, and
    // coarser tiers get DARKER, so the grid reads the same way against either
    // background instead of washing out.
    bool lightBg     = u_lightBg > 0.5;
    // Sketch grid: a fixed mid-grey fine line with a noticeably DARKER every-10th
    // so the decade lines read clearly while sketching. (Was a user "shade"
    // slider - dropped: it only affected this grid and read as doing nothing in
    // the normal 3D view.)
    vec3 sketchCol   = vec3(0.50);
    vec3 sketchMajor = vec3(0.26);
    // World/ground grid: the minor (every-1) lines are dim, and the every-10th
    // (major) + every-100th (mega) get progressively stronger so the decade lines
    // stand out clearly against the rest.
    vec3 minorCol    = lightBg ? vec3(0.62, 0.64, 0.70) : vec3(0.30, 0.30, 0.34);
    vec3 majorCol    = lightBg ? vec3(0.32, 0.34, 0.41) : vec3(0.90, 0.92, 1.00);
    vec3 megaCol     = lightBg ? vec3(0.16, 0.18, 0.25) : vec3(1.00, 1.00, 1.00);

    vec3  rgb;
    float a;
    if (u_sketchGrid > 0.5) {
        // SKETCH GRID: a uniform fine tier PLUS a heavier every-10th line so the
        // user can gauge scale (count decades) while sketching. The every-10th
        // tier is just a stronger shade of the SAME colour drawn a touch wider -
        // not the bright/dim "plaid" of the old tiered grid - and since the grid
        // now blends over geometry (no depth punch-through) it reads cleanly. The
        // pristine-grid coverage still greys each tier out evenly when it gets
        // dense, so no moiré; the opacity slider dims the whole sheet together.
        float covMajor = gridCoverage(uv * u_scale * 0.1, 1.6 * u_sketchThickness);
        rgb = sketchCol;
        a   = covMinor * u_minorAlpha;
        rgb = mix(rgb, sketchMajor, covMajor);
        a   = max(a, covMajor);
    } else {
        // WORLD / GROUND GRID: three tiers - minor (every 1), major (every 10),
        // mega (every 100). The coarser 10- and 100-unit lines read on top;
        // zooming reveals finer tiers. Keeps the tiered look (the "10 mm /
        // 100 mm lines") the user wants on the ground grid.
        float covMajor = gridCoverage(uv * u_scale * 0.1,  1.3 * u_sketchThickness);
        float covMega  = gridCoverage(uv * u_scale * 0.01, 1.3 * u_sketchThickness);
        rgb = minorCol;
        a   = covMinor * u_minorAlpha;
        rgb = mix(rgb, majorCol, covMajor);
        a   = max(a, covMajor);
        rgb = mix(rgb, megaCol, covMega);
        a   = max(a, covMega);
    }

    // Coloured axes through the grid origin (red = U, blue = V), anti-aliased
    // over one pixel and always drawn on top so the origin reads at any zoom.
    vec2 acoord = uv * u_scale;
    vec2 aderiv = fwidth(acoord);
    float axisU = 1.0 - min(abs(acoord.y) / max(aderiv.y, 1e-6), 1.0); // V≈0 → U axis
    float axisV = 1.0 - min(abs(acoord.x) / max(aderiv.x, 1e-6), 1.0); // U≈0 → V axis
    if (axisU > 0.0) { rgb = mix(rgb, vec3(0.80, 0.20, 0.20), axisU); a = max(a, axisU); }
    if (axisV > 0.0) { rgb = mix(rgb, vec3(0.20, 0.20, 0.80), axisV); a = max(a, axisV); }

    // Distance fade, then the global opacity slider - both linear multipliers so
    // the whole grid dims uniformly instead of culling lines one by one.
    float alpha = a * fade * u_globalAlpha;
    if (alpha < 0.001) discard;

    fragColor = vec4(rgb, alpha);
}
)";

Grid::Grid() {}

Grid::~Grid()
{
    if (m_shaderProgram) {
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
    }
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
}

bool Grid::initialize()
{
    // Compile shaders
    unsigned int vertShader = 0, fragShader = 0;
    if (!compileShader(vertShader, GL_VERTEX_SHADER, s_gridVertSource)) {
        return false;
    }
    if (!compileShader(fragShader, GL_FRAGMENT_SHADER, s_gridFragSource)) {
        glDeleteShader(vertShader);
        return false;
    }
    if (!linkProgram(vertShader, fragShader)) {
        glDeleteShader(vertShader);
        glDeleteShader(fragShader);
        return false;
    }
    glDeleteShader(vertShader);
    glDeleteShader(fragShader);

    // Cache uniform locations
    m_locViewProjection = glGetUniformLocation(m_shaderProgram, "u_viewProjection");
    m_locInvViewProjection = glGetUniformLocation(m_shaderProgram, "u_invViewProjection");
    m_locFadeCenter = glGetUniformLocation(m_shaderProgram, "u_fadeCenter");
    m_locFadeDistance = glGetUniformLocation(m_shaderProgram, "u_fadeDistance");
    m_locPlaneOrigin = glGetUniformLocation(m_shaderProgram, "u_planeOrigin");
    m_locPlaneU = glGetUniformLocation(m_shaderProgram, "u_planeU");
    m_locPlaneV = glGetUniformLocation(m_shaderProgram, "u_planeV");
    m_locPlaneNormal = glGetUniformLocation(m_shaderProgram, "u_planeNormal");
    m_locScale = glGetUniformLocation(m_shaderProgram, "u_scale");
    m_locMinorAlpha = glGetUniformLocation(m_shaderProgram, "u_minorAlpha");
    m_locGlobalAlpha = glGetUniformLocation(m_shaderProgram, "u_globalAlpha");
    m_locSketchGrid = glGetUniformLocation(m_shaderProgram, "u_sketchGrid");
    m_locDepthBias = glGetUniformLocation(m_shaderProgram, "u_depthBias");
    m_locLightBg = glGetUniformLocation(m_shaderProgram, "u_lightBg");
    m_locSketchThickness = glGetUniformLocation(m_shaderProgram, "u_sketchThickness");

    // Create a dummy VAO (required for core profile, even with no vertex attributes)
    glGenVertexArrays(1, &m_vao);

    return true;
}

void Grid::render(const glm::mat4& view, const glm::mat4& projection,
                  const glm::vec3& fadeCenter, float fadeDistance,
                  const Plane& plane, float minorStep,
                  float minorAlpha, float globalAlpha, float sketchGrid,
                  float depthBias, float lightBg,
                  float sketchThickness)
{
    if (!m_shaderProgram) return;

    glm::mat4 vp = projection * view;
    glm::mat4 invVP = glm::inverse(vp);
    float scale = (minorStep > 1e-4f) ? (1.0f / minorStep) : 1.0f;

    glUseProgram(m_shaderProgram);
    glUniformMatrix4fv(m_locViewProjection, 1, GL_FALSE, glm::value_ptr(vp));
    glUniformMatrix4fv(m_locInvViewProjection, 1, GL_FALSE, glm::value_ptr(invVP));
    glUniform3fv(m_locFadeCenter, 1, glm::value_ptr(fadeCenter));
    glUniform1f(m_locFadeDistance, fadeDistance);
    glUniform3fv(m_locPlaneOrigin, 1, glm::value_ptr(plane.origin));
    glUniform3fv(m_locPlaneU, 1, glm::value_ptr(plane.u));
    glUniform3fv(m_locPlaneV, 1, glm::value_ptr(plane.v));
    glUniform3fv(m_locPlaneNormal, 1, glm::value_ptr(plane.normal));
    glUniform1f(m_locScale, scale);
    glUniform1f(m_locMinorAlpha, minorAlpha);
    glUniform1f(m_locGlobalAlpha, globalAlpha);
    glUniform1f(m_locSketchGrid, sketchGrid);
    glUniform1f(m_locDepthBias, depthBias);
    glUniform1f(m_locLightBg, lightBg);
    glUniform1f(m_locSketchThickness, sketchThickness);

    // Enable blending for grid transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Depth: TEST against the scene (so geometry in front of the grid plane
    // hides it) but do NOT WRITE depth. Writing depth on the grid's line pixels
    // made the lines "win" the depth fight against any coplanar face (a body
    // sitting on the ground, or the very face being sketched on) and punch a
    // hole through it to the background - a grey grid baked into the face that
    // opacity couldn't dim. With writes off the grid simply blends over whatever
    // is behind it and fades cleanly. u_depthBias decides whether it sits on top
    // of (sketch) or behind (ground) a coplanar surface.
    glDepthMask(GL_FALSE);

    // Draw the full-screen quad (6 vertices from gl_VertexID)
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

bool Grid::compileShader(unsigned int& shader, unsigned int type, const char* source)
{
    shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::fprintf(stderr, "Grid shader compilation failed: %s\n", infoLog);
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

bool Grid::linkProgram(unsigned int vertShader, unsigned int fragShader)
{
    m_shaderProgram = glCreateProgram();
    glAttachShader(m_shaderProgram, vertShader);
    glAttachShader(m_shaderProgram, fragShader);
    glLinkProgram(m_shaderProgram);

    int success = 0;
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_shaderProgram, 512, nullptr, infoLog);
        std::fprintf(stderr, "Grid shader linking failed: %s\n", infoLog);
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
        return false;
    }
    return true;
}

} // namespace materializr
