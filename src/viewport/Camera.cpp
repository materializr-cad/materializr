#include "Camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace materializr {

Camera::Camera()
    : m_position(5.0f, 5.0f, 5.0f)
    , m_target(0.0f, 0.0f, 0.0f)
    , m_up(0.0f, 1.0f, 0.0f)
    , m_fov(45.0f)
    , m_nearPlane(0.01f)
    , m_farPlane(1000.0f)
    , m_aspect(16.0f / 9.0f)
    , m_orbitSpeed(0.005f)
    , m_panSpeed(0.01f)
    , m_zoomSpeed(0.1f)
{
}

// Free trackball rotation: yaw around the camera's CURRENT up vector, pitch
// around its CURRENT right vector. Allows full 3D tumbling (the view can roll).
// Used when level-orbit is turned off in Settings.
static void trackballRotate(glm::vec3& position, const glm::vec3& target, glm::vec3& upInOut,
                            float yaw, float pitch)
{
    glm::vec3 offset = position - target;
    if (glm::length(offset) < 1e-6f) return;

    glm::vec3 forward = -glm::normalize(offset);
    glm::vec3 right = glm::cross(forward, upInOut);
    if (glm::length(right) < 1e-6f) {
        right = glm::cross(forward, glm::vec3(0, 1, 0));
        if (glm::length(right) < 1e-6f) right = glm::cross(forward, glm::vec3(1, 0, 0));
    }
    right = glm::normalize(right);
    glm::vec3 up = glm::normalize(glm::cross(right, forward));

    glm::mat4 rotYaw = glm::rotate(glm::mat4(1.0f), yaw, up);
    glm::mat4 rotPitch = glm::rotate(glm::mat4(1.0f), pitch, right);
    glm::mat4 rot = rotPitch * rotYaw;

    position = target + glm::vec3(rot * glm::vec4(offset, 0.0f));
    upInOut = glm::normalize(glm::vec3(rot * glm::vec4(up, 0.0f)));
}

void Camera::orbitLevel(float yawDelta, float pitchDelta)
{
    // Orbiting implies free 3D rotation, so drop the ortho lock if it was set
    // by entering a sketch. Pan and zoom intentionally keep ortho mode on.
    m_orthographic = false;

    // Capture the view's CURRENT up before we reset it - a Top/Bottom ortho snap
    // stored the screen orientation there, and we need it to recover the azimuth
    // at the pole (below). Then reset up to world-up so a turntable orbit always
    // shows a level horizon. Without the reset, a sketch's chosen up vector (which
    // may be -X, +Z, etc.) sticks around and orbiting looks rotated / twisted. The
    // target is left as-is so an orbit out of a sketch ortho view pivots around
    // the world-grid-aligned anchor alignCameraToActiveSketch set near the face.
    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    const glm::vec3 prevUp = m_up;
    m_up = worldUp;

    glm::vec3 offset = m_position - m_target; // target -> camera
    float radius = glm::length(offset);
    if (radius < 1e-6f) return;
    glm::vec3 dir = offset / radius;

    // Decompose into yaw (around world Y) and pitch (elevation above the ground
    // plane). Rebuilding the offset from these keeps the view level - there is
    // no roll term, so the horizon stays flat no matter how far we orbit.
    float pitch = std::asin(glm::clamp(dir.y, -1.0f, 1.0f));
    float yaw;
    const glm::vec2 upH(prevUp.x, prevUp.z); // horizontal part of the entering up
    if (std::abs(dir.y) > 0.9999f && glm::length(upH) > 1e-4f) {
        // Looking straight down/up (an ortho Top/Bottom view): yaw is indeterminate
        // from `dir` alone - atan2(0,0) collapses to 0, snapping the first orbit to
        // the +X "Right" side. Recover the azimuth from the view's up vector, which
        // the ortho snap set to a horizontal direction, so the orbit tips off the
        // pole the way the screen is already oriented instead of jumping to Right.
        const glm::vec2 h = glm::normalize(upH);
        yaw = (dir.y > 0.0f) ? std::atan2(-h.y, -h.x)   // top:    screen-up = -(cosY,sinY)
                             : std::atan2( h.y,  h.x);   // bottom: screen-up =  (cosY,sinY)
    } else {
        yaw = std::atan2(dir.z, dir.x);
    }

    yaw += yawDelta;
    pitch += pitchDelta;

    // Stop just short of straight up/down so the up vector never becomes
    // parallel to the view direction (which would make lookAt degenerate).
    const float lim = glm::radians(89.0f);
    pitch = glm::clamp(pitch, -lim, lim);

    glm::vec3 newDir(std::cos(pitch) * std::cos(yaw),
                     std::sin(pitch),
                     std::cos(pitch) * std::sin(yaw));
    m_position = m_target + newDir * radius;
    m_up = worldUp;
}

void Camera::orbit(float deltaX, float deltaY)
{
    const float s = m_orbitSpeed * m_mouseSensitivity;
    if (m_levelOrbit) {
        orbitLevel(deltaX * s, deltaY * s);
    } else {
        m_orthographic = false;
        trackballRotate(m_position, m_target, m_up,
                        -deltaX * s, -deltaY * s);
    }
}

void Camera::rotateAroundTarget(float yawRadians, float pitchRadians)
{
    if (m_levelOrbit) {
        orbitLevel(yawRadians, pitchRadians);
    } else {
        m_orthographic = false;
        trackballRotate(m_position, m_target, m_up, -yawRadians, -pitchRadians);
    }
}

void Camera::pan(float deltaX, float deltaY)
{
    glm::vec3 forward = glm::normalize(m_target - m_position);
    glm::vec3 right = glm::normalize(glm::cross(forward, m_up));
    glm::vec3 up = glm::normalize(glm::cross(right, forward));

    float panScale;
    if (m_viewHeightPx > 1.0f) {
        // Exact screen-space pan: move the world by precisely one pixel's
        // world size per pixel of mouse motion, so the point you "grab"
        // stays under the cursor at every zoom level. In perspective the
        // pixel size depends on depth - use the pan anchor (the content
        // under the cursor at drag start) when the app provided one; the
        // target distance is only the fallback, because on large projects
        // it can sit metres from the geometry on screen (stale after
        // cursor-zoom), which used to make pan twitchy up close and frozen
        // far away.
        if (m_orthographic) {
            panScale = (2.0f * m_orthoSize) / m_viewHeightPx;
        } else {
            float ref = (m_panRefDist > 0.0f)
                            ? m_panRefDist
                            : glm::length(m_target - m_position);
            panScale = (2.0f * ref * std::tan(glm::radians(m_fov) * 0.5f)) /
                       m_viewHeightPx;
        }
        panScale *= m_mouseSensitivity;
    } else {
        // Legacy distance-fraction pan (no viewport height known). In ortho,
        // visible size depends on m_orthoSize (not distance) - otherwise
        // panning at distance 0.1 looks frozen while panning at distance 100
        // throws the model off-screen.
        float scaleRef =
            m_orthographic ? m_orthoSize : glm::length(m_target - m_position);
        panScale = scaleRef * m_panSpeed * m_mouseSensitivity;
    }

    glm::vec3 offset = -right * deltaX * panScale + up * deltaY * panScale;
    m_position += offset;
    m_target += offset;
}

void Camera::zoom(float delta)
{
    const float s = m_zoomSpeed * m_mouseSensitivity;
    if (m_orthographic) {
        // In ortho, "zoom" scales the visible extents rather than moving the camera.
        // Multiplicative step keeps the feel consistent across scales.
        float factor = 1.0f - delta * s;
        factor = glm::clamp(factor, 0.1f, 10.0f);
        m_orthoSize = std::max(0.01f, m_orthoSize * factor);
        return;
    }

    glm::vec3 direction = m_position - m_target;
    float distance = glm::length(direction);

    // Scale zoom by current distance for consistent feel
    float zoomAmount = delta * s * distance;

    // Prevent zooming through the target
    float newDistance = distance - zoomAmount;
    newDistance = std::max(newDistance, 0.1f);

    m_position = m_target + glm::normalize(direction) * newDistance;
}

void Camera::zoomToward(const glm::vec3& focus, float delta)
{
    // Scale-around-focus formulation: a single uniform factor scales both the
    // camera→focus and target→focus vectors. The focus point stays put on
    // screen, position and target both slide toward (or away from) it, and
    // subsequent orbits pivot around the new target - which is now near the
    // thing the user was zooming on. This eliminates the "I have to pan to
    // re-aim before zoom feels right" dance for off-origin parts.
    float factor = 1.0f - delta * m_zoomSpeed * m_mouseSensitivity;
    factor = glm::clamp(factor, 0.1f, 10.0f);
    if (m_orthographic) {
        // In ortho, both the camera/target slide AND the view extent scale -
        // otherwise the focus would shift on screen as we narrow the frustum.
        m_orthoSize = std::max(0.01f, m_orthoSize * factor);
        m_position  = focus + (m_position - focus) * factor;
        m_target    = focus + (m_target   - focus) * factor;
        return;
    }
    glm::vec3 newPos    = focus + (m_position - focus) * factor;
    glm::vec3 newTarget = focus + (m_target   - focus) * factor;
    // Don't let the camera pass through (or onto) the focus point - keep at
    // least 0.1 mm of standoff so the view doesn't degenerate.
    if (glm::length(newPos - newTarget) < 0.1f) return;
    if (glm::length(newPos - focus)     < 0.1f) return;
    m_position = newPos;
    m_target   = newTarget;
}

glm::mat4 Camera::getViewMatrix() const
{
    return glm::lookAt(m_position, m_target, m_up);
}

glm::mat4 Camera::getProjectionMatrix() const
{
    // Scale clip planes with the camera distance to the target so far-zoomed
    // views of large models (e.g. a 2 m part viewed from across the room) keep
    // showing geometry instead of getting clipped at the legacy 1000 mm far
    // plane, while close-up sketch work still gets fine depth precision.
    float ref = m_orthographic ? m_orthoSize
                               : glm::length(m_position - m_target);
    float n = std::max(m_nearPlane, ref * 0.001f);
    float f = std::max(m_farPlane,  ref * 200.0f);
    if (m_orthographic) {
        float h = m_orthoSize;
        float w = h * m_aspect;
        return glm::ortho(-w, w, -h, h, n, f);
    }
    return glm::perspective(glm::radians(m_fov), m_aspect, n, f);
}

void Camera::setAspect(float aspect)
{
    m_aspect = aspect;
}

void Camera::reset()
{
    m_position = glm::vec3(5.0f, 5.0f, 5.0f);
    m_target = glm::vec3(0.0f, 0.0f, 0.0f);
    m_up = glm::vec3(0.0f, 1.0f, 0.0f);
    m_fov = 45.0f;
    m_orthographic = false;
}

void Camera::zoomToFit(glm::vec3 min, glm::vec3 max)
{
    glm::vec3 center = (min + max) * 0.5f;
    glm::vec3 extents = max - min;
    float radius = glm::length(extents) * 0.5f;

    // Maintain current view direction
    glm::vec3 direction = glm::normalize(m_position - m_target);
    m_target = center;

    if (m_orthographic) {
        // In ortho, fit = adjust the visible extents. Add a margin so geometry
        // doesn't sit right on the edge of the viewport.
        m_orthoSize = std::max(radius * 1.1f, 0.01f);
        // Position is kept aligned along the current direction with a safe distance
        // so geometry stays between the near/far planes.
        float dist = std::max(glm::length(m_position - m_target), radius * 2.0f);
        m_position = center + direction * dist;
        return;
    }

    // Compute required distance to fit the bounding sphere in view
    float halfFov = glm::radians(m_fov) * 0.5f;
    float distance = radius / std::sin(halfFov);
    m_position = center + direction * distance;
}

void Camera::setPosition(glm::vec3 pos)
{
    m_position = pos;
}

void Camera::setTarget(glm::vec3 target)
{
    m_target = target;
}

void Camera::setUp(glm::vec3 up)
{
    m_up = up;
}

} // namespace materializr
