#include "gl_common.h"

#include "Viewport.h"

#include <imgui.h>

namespace materializr {

Viewport::Viewport()
{
    createFramebuffer();
}

Viewport::~Viewport()
{
    destroyFramebuffer();
}

void Viewport::resize(int width, int height)
{
    if (width == m_width && height == m_height) return;
    if (width <= 0 || height <= 0) return;

    m_width = width;
    m_height = height;
    m_camera.setAspect(static_cast<float>(width) / static_cast<float>(height));

    destroyFramebuffer();
    createFramebuffer();
}

void Viewport::bind()
{
    // NOTE: glBindFramebuffer requires glad or GL 3.0+ function pointer
    if (m_samples > 0 && m_msaaFbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFbo);
#if !defined(MZ_GLES)
        // GL ES has no GL_MULTISAMPLE toggle - MSAA is implied by the
        // multisampled renderbuffer/EGL config and is always active.
        glEnable(GL_MULTISAMPLE);
#endif
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    }
    glViewport(0, 0, m_width, m_height);
}

void Viewport::unbind()
{
    // Resolve the multisampled buffer into the single-sample texture ImGui shows.
    if (m_samples > 0 && m_msaaFbo) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
        glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_windowFramebuffer);
}

void Viewport::setSamples(int samples)
{
    // Clamp to what the driver supports (and to sane values).
    if (samples < 0) samples = 0;
    if (samples > 0) {
        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        if (samples > maxSamples) samples = maxSamples;
    }
    if (samples == m_samples) return;

    m_samples = samples;
    destroyFramebuffer();
    createFramebuffer();
}

void Viewport::createFramebuffer()
{
    // NOTE: These calls require glad or GL 3.0+ extension loading.

    // Create framebuffer
    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    // Create color attachment texture
    glGenTextures(1, &m_colorTexture);
    glBindTexture(GL_TEXTURE_2D, m_colorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_colorTexture, 0);

    // Create depth+stencil renderbuffer
    glGenRenderbuffers(1, &m_depthRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthRenderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_width, m_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, m_depthRenderbuffer);

    // Check completeness
    // GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    // assert(status == GL_FRAMEBUFFER_COMPLETE);

    // Multisampled render target. The scene draws here (color + depth/stencil as
    // multisampled renderbuffers) and is resolved into m_fbo on unbind().
    if (m_samples > 0) {
        glGenFramebuffers(1, &m_msaaFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFbo);

        glGenRenderbuffers(1, &m_msaaColor);
        glBindRenderbuffer(GL_RENDERBUFFER, m_msaaColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_RGBA8,
                                         m_width, m_height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, m_msaaColor);

        glGenRenderbuffers(1, &m_msaaDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, m_msaaDepth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples,
                                         GL_DEPTH24_STENCIL8, m_width, m_height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, m_msaaDepth);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g_windowFramebuffer);
}

void Viewport::destroyFramebuffer()
{
    if (m_colorTexture) {
        glDeleteTextures(1, &m_colorTexture);
        m_colorTexture = 0;
    }
    if (m_depthRenderbuffer) {
        glDeleteRenderbuffers(1, &m_depthRenderbuffer);
        m_depthRenderbuffer = 0;
    }
    if (m_fbo) {
        glDeleteFramebuffers(1, &m_fbo);
        m_fbo = 0;
    }
    if (m_msaaColor) {
        glDeleteRenderbuffers(1, &m_msaaColor);
        m_msaaColor = 0;
    }
    if (m_msaaDepth) {
        glDeleteRenderbuffers(1, &m_msaaDepth);
        m_msaaDepth = 0;
    }
    if (m_msaaFbo) {
        glDeleteFramebuffers(1, &m_msaaFbo);
        m_msaaFbo = 0;
    }
}

} // namespace materializr
