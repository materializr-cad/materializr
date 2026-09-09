#pragma once

#include "gl_common.h"

#include <glm/glm.hpp>

namespace materializr {

/// Renders an infinite grid on an arbitrary plane using a full-screen quad
/// shader. Defaults to the y=0 ground plane but can be retargeted to any plane
/// (e.g. the active sketch plane) so a from-scratch sketch on XY/XZ/YZ shows the
/// same grid face-on.
class Grid {
public:
    Grid();
    ~Grid();

    /// The plane the grid lies on: an origin, two orthonormal in-plane basis
    /// vectors (grid X/Y), and the plane normal. Defaults to the XZ ground.
    struct Plane {
        glm::vec3 origin{0.0f, 0.0f, 0.0f};
        glm::vec3 u{1.0f, 0.0f, 0.0f};
        glm::vec3 v{0.0f, 0.0f, 1.0f};
        glm::vec3 normal{0.0f, 1.0f, 0.0f};
    };

    /// Initialize the grid (compile shaders, create VAO).
    /// Call once after OpenGL context is ready.
    bool initialize();

    /// Render the grid on `plane`. `minorStep` is the minor line spacing in world
    /// units (major lines every 10×). The grid fades with distance from
    /// `fadeCenter` over `fadeDistance` world units - distance-based so it stays
    /// visible under orthographic projection (unlike the old depth-based fade).
    /// `minorAlpha` (0..1) scales the 1× tier opacity - set to 0 to hide minor
    /// lines on big projects where they read as clutter. `globalAlpha` (0..1)
    /// scales the final grid opacity so geometry below stays visible.
    /// Every tier carries a screen-space density fade in the shader, so a tier
    /// dissolves smoothly as it gets sub-pixel dense (no moiré). `sketchGrid`
    /// (0/1) picks the look: 0 = the tiered world/ground grid (minor + 10× major
    /// + 100× mega, brighter coarser lines on top); 1 = the face-on sketch grid,
    /// a SINGLE uniform tier (every line the same colour and weight, no "plaid")
    /// that dims as one even sheet under `globalAlpha`.
    /// `depthBias` is a signed fraction of the view-ray length nudging the grid's
    /// depth: positive draws it ON a coplanar face (the sketch face), negative
    /// lets a coplanar body face occlude it (the ground grid under a body). The
    /// grid never writes depth, so it blends over geometry rather than punching
    /// through it.
    /// `lightBg` (0/1): 1 switches the grid to a dark-on-light line palette for
    /// the light-theme viewport (lines stay readable against a light background).
    /// `sketchThickness` multiplies the sketch grid's line width (1 = default);
    /// it has no effect on the tiered world/ground grid.
    void render(const glm::mat4& view, const glm::mat4& projection,
                const glm::vec3& fadeCenter, float fadeDistance,
                const Plane& plane, float minorStep,
                float minorAlpha = 1.0f, float globalAlpha = 0.55f,
                float sketchGrid = 0.0f, float depthBias = 0.0005f,
                float lightBg = 0.0f, float sketchThickness = 1.0f);

private:
    bool compileShader(unsigned int& shader, unsigned int type, const char* source);
    bool linkProgram(unsigned int vertShader, unsigned int fragShader);

    unsigned int m_shaderProgram = 0;
    unsigned int m_vao = 0; // Dummy VAO for the full-screen quad trick

    // Uniform locations
    int m_locViewProjection = -1;
    int m_locInvViewProjection = -1;
    int m_locFadeCenter = -1;
    int m_locFadeDistance = -1;
    int m_locPlaneOrigin = -1;
    int m_locPlaneU = -1;
    int m_locPlaneV = -1;
    int m_locPlaneNormal = -1;
    int m_locScale = -1;
    int m_locMinorAlpha = -1;
    int m_locGlobalAlpha = -1;
    int m_locSketchGrid = -1;
    int m_locDepthBias = -1;
    int m_locLightBg = -1;
    int m_locSketchThickness = -1;
};

} // namespace materializr
