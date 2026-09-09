#pragma once

#include "gl_common.h"

#include "Camera.h"

#include <cstdio>

#include <glm/glm.hpp>

namespace materializr {

/// Manages the 3D viewport FBO and input within an ImGui window.
class Viewport {
public:
    Viewport();
    ~Viewport();

    /// Recreate the framebuffer at a new size.
    void resize(int width, int height);

    /// Bind the viewport FBO for off-screen rendering.
    void bind();

    /// Unbind the viewport FBO (restore default framebuffer). When MSAA is on,
    /// this also resolves the multisampled buffer into the displayed texture.
    void unbind();

    /// Set the MSAA sample count (0 = off; clamped to the GL maximum). Recreates
    /// the framebuffer when the count changes.
    void setSamples(int samples);

    /// Get the color texture ID for ImGui::Image().
    unsigned int getTextureID() const { return m_colorTexture; }

    /// Re-assert THIS framebuffer's aspect on the camera. Tab switches copy a
    /// whole Camera in (carrying the aspect it had when stashed - or a fresh
    /// session's default), and resize()'s same-size early-return would never
    /// correct it, leaving the scene stretched until a real resize happens.
    void syncCameraAspect() {
        if (m_width > 0 && m_height > 0)
            m_camera.setAspect(static_cast<float>(m_width) /
                               static_cast<float>(m_height));
    }

    /// Access the camera.
    Camera& getCamera() { return m_camera; }
    const Camera& getCamera() const { return m_camera; }

private:
    void createFramebuffer();
    void destroyFramebuffer();

    Camera m_camera;

    unsigned int m_fbo = 0;
    unsigned int m_colorTexture = 0;
    unsigned int m_depthRenderbuffer = 0;

    // Multisampled render target (only created when m_samples > 0). The scene is
    // drawn here, then blitted ("resolved") into m_fbo/m_colorTexture for display.
    unsigned int m_msaaFbo = 0;
    unsigned int m_msaaColor = 0;
    unsigned int m_msaaDepth = 0;
    int m_samples = 0;

    int m_width = 1280;
    int m_height = 720;
};

} // namespace materializr
