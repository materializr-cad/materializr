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
    , m_fovEff(45.0f)
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
    leaveOrthographic();

    // Capture the view's CURRENT up before we reset it - a Top/Bottom ortho snap
    // stored the screen orientation there, and we need it to recover the azimuth
    // at the pole (below). Then reset up to world-up so a turntable orbit always
    // shows a level horizon. Without the reset, a sketch's chosen up vector (which
    // may be -X, +Z, etc.) sticks around and orbiting looks rotated / twisted. The
    // target is left as-is so an orbit out of a sketch ortho view pivots around
    // the world-grid-aligned anchor alignCameraToActiveSketch set near the face.
    // Mid-blend the entering up is the one the blend started from, not the
    // partly-levelled value this function wrote on the previous call.
    const glm::vec3 enterUp = (m_blendActive && m_blendLevel) ? m_blendUp0 : m_up;
    // Keep the horizon level the way the view is FACING: an upside-down view
    // orbits upside-down rather than rolling through half a turn to get upright.
    // (Near the poles the up vector says nothing about it, so the last sign holds.)
    if (std::abs(enterUp.y) > 0.005f) m_levelSign = enterUp.y < 0.0f ? -1.0f : 1.0f;
    // Everything below is turntable maths about +Y. For an upside-down view run
    // it in the frame rotated 180 degrees about X (its own inverse).
    const float sg = m_levelSign;
    const auto R = [sg](const glm::vec3& v) { return glm::vec3(v.x, sg * v.y, sg * v.z); };
    const glm::vec3 prevUp = R(enterUp);
    m_up = glm::vec3(0.0f, sg, 0.0f);

    glm::vec3 offset = R(m_position - m_target); // target -> camera
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
    m_position = m_target + R(newDir * radius);
    m_up = glm::vec3(0.0f, sg, 0.0f);
    if (m_blendActive) {
        // Level the roll over the blend instead of in one step: a sketch view
        // that was turned sideways (Look At, ViewCube) would otherwise snap
        // upright on the first orbit.
        m_blendLevel = true;
        m_up = blendedUp(R(newDir));
    }
}

void Camera::orbit(float deltaX, float deltaY)
{
    finishAnimation();
    const float s = m_orbitSpeed * m_mouseSensitivity;
    if (m_levelOrbit) {
        orbitLevel(deltaX * s, deltaY * s);
    } else {
        leaveOrthographic();
        trackballRotate(m_position, m_target, m_up,
                        -deltaX * s, -deltaY * s);
    }
}

void Camera::rotateAroundTarget(float yawRadians, float pitchRadians)
{
    finishAnimation();
    if (m_levelOrbit) {
        orbitLevel(yawRadians, pitchRadians);
    } else {
        leaveOrthographic();
        trackballRotate(m_position, m_target, m_up, -yawRadians, -pitchRadians);
    }
}

void Camera::pan(float deltaX, float deltaY)
{
    finishAnimation();
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
            panScale = (2.0f * ref * std::tan(glm::radians(m_fovEff) * 0.5f)) /
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
    finishAnimation();
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
    finishAnimation();
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
    return glm::perspective(glm::radians(m_fovEff), m_aspect, n, f);
}

void Camera::setAspect(float aspect)
{
    m_aspect = aspect;
}

void Camera::reset()
{
    finishAnimation();
    m_blendActive = false;
    m_fovEff = 45.0f;
    m_position = glm::vec3(5.0f, 5.0f, 5.0f);
    m_target = glm::vec3(0.0f, 0.0f, 0.0f);
    m_up = glm::vec3(0.0f, 1.0f, 0.0f);
    m_fov = 45.0f;
    m_orthographic = false;
}

void Camera::zoomToFit(glm::vec3 min, glm::vec3 max)
{
    finishAnimation();
    finishBlend();
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
    finishAnimation();
    finishBlend();
    m_position = pos;
}

void Camera::setTarget(glm::vec3 target)
{
    finishAnimation();
    finishBlend();
    m_target = target;
}

void Camera::setUp(glm::vec3 up)
{
    finishAnimation();
    finishBlend();
    m_up = up;
}


// ---------------------------------------------------------------------------
// Animated view changes (see Camera.h)

namespace {

// Ortho->perspective blend (Camera::leaveOrthographic): starting FOV, near enough
// to orthographic that the switch is invisible, and how long it takes to open up.
constexpr float kBlendStartFov = 4.0f;
constexpr float kBlendSeconds  = 0.3f;

// Distance at which a perspective camera sees a half-height of `orthoSize`:
// the perspective view that frames a flat subject exactly like the ortho box.
float equivalentDistance(float orthoSize, float fovDegrees)
{
    const float halfFov = glm::radians(fovDegrees) * 0.5f;
    return std::max(orthoSize / std::tan(halfFov), 1e-3f);
}

glm::vec3 safeDirection(const glm::vec3& from, const glm::vec3& to, const glm::vec3& fallback)
{
    const glm::vec3 d = from - to;
    const float len = glm::length(d);
    return len > 1e-6f ? d / len : fallback;
}

// Smootherstep: zero velocity and acceleration at both ends, so the move eases
// out of the old view and settles into the new one.
float ease(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

} // namespace

void Camera::animateTo(glm::vec3 position, glm::vec3 target, glm::vec3 up,
                       bool orthographic, float orthoSize, float seconds)
{
    finishAnimation();   // settle any move in flight; start from where we truly are
    finishBlend();

    const glm::vec3 fallbackDir(0.0f, 0.0f, 1.0f);
    const glm::vec3 dir1 = safeDirection(position, target, fallbackDir);
    if (seconds <= 0.0f || glm::length(position - target) < 1e-6f) {
        m_position = position;
        m_target = target;
        m_up = up;
        m_orthographic = orthographic;
        if (orthographic) m_orthoSize = orthoSize;
        return;
    }

    Animation a;
    a.duration = seconds;
    a.t0 = m_target;
    a.u0 = m_up;
    const glm::vec3 dir0 = safeDirection(m_position, m_target, fallbackDir);
    const float d0 = m_orthographic ? equivalentDistance(m_orthoSize, m_fov)
                                    : std::max(glm::length(m_position - m_target), 1e-3f);
    a.p0 = m_target + dir0 * d0;

    a.t1 = target;
    a.u1 = up;
    const float d1 = orthographic ? equivalentDistance(orthoSize, m_fov)
                                  : std::max(glm::length(position - target), 1e-3f);
    a.p1 = target + dir1 * d1;
    a.endPos = position;
    a.endOrtho = orthographic;
    a.endOrthoSize = orthoSize;
    a.active = true;

    // Start from the perspective-equivalent of the current view: from an
    // orthographic one this is the same picture, so there is nothing to jump.
    m_orthographic = false;
    m_position = a.p0;
    m_anim = a;
}

void Camera::leaveOrthographic()
{
    if (!m_orthographic) return;
    const glm::vec3 dir = safeDirection(m_position, m_target, glm::vec3(0.0f, 0.0f, 1.0f));
    m_blendOrthoSize = m_orthoSize;
    m_blendT = 0.0f;
    m_blendActive = true;
    m_blendLevel = false;
    m_blendRollSet = false;
    m_blendUp0 = m_up;
    m_fovEff = kBlendStartFov;
    m_position = m_target + dir * equivalentDistance(m_orthoSize, m_fovEff);
    m_orthographic = false;
}

void Camera::finishBlend()
{
    if (!m_blendActive) return;
    m_blendActive = false;
    m_fovEff = m_fov;
    const glm::vec3 dir = safeDirection(m_position, m_target, glm::vec3(0.0f, 0.0f, 1.0f));
    m_position = m_target + dir * equivalentDistance(m_blendOrthoSize, m_fov);
    if (m_blendLevel) {
        m_blendT = 1.0f;
        m_up = blendedUp(dir);
    }
}

glm::vec3 Camera::blendedUp(const glm::vec3& dir)
{
    // Roll from the blend's starting up to world-up (level horizon) as a real
    // rotation about the view axis, eased by the blend's progress. A straight
    // mix of the two vectors passes through zero when they are opposite - an
    // upside-down view (X reversed) flipped 180 degrees in one frame.
    const glm::vec3 worldUp(0.0f, m_levelSign, 0.0f);   // the level up this view keeps
    glm::vec3 a = m_blendUp0 - dir * glm::dot(m_blendUp0, dir);
    glm::vec3 b = worldUp - dir * glm::dot(worldUp, dir);
    if (glm::length(a) < 1e-4f || glm::length(b) < 1e-4f) return m_up;   // along the pole
    a = glm::normalize(a);
    b = glm::normalize(b);
    float ang = std::atan2(glm::dot(dir, glm::cross(a, b)), glm::dot(a, b));
    // Near 180 degrees the sign is a coin toss that can change as the camera
    // moves; stay on the side the roll already took.
    const float pi = 3.14159265f;
    if (m_blendRollSet && std::abs(ang - m_blendRollAngle) > pi)
        ang += (m_blendRollAngle > ang) ? 2.0f * pi : -2.0f * pi;
    m_blendRollAngle = ang;
    m_blendRollSet = true;
    const float r = ang * ease(m_blendT);
    return glm::normalize(a * std::cos(r) + glm::cross(dir, a) * std::sin(r));
}

bool Camera::tick(float dtSeconds)
{
    bool moved = false;
    if (m_blendActive) {
        // Dolly-zoom: tan(fov/2) eases from the near-ortho start to the resting
        // FOV, and the distance follows (d = halfHeight / tan(fov/2)) so the
        // plane the ortho box framed keeps its size. Re-seated along the CURRENT
        // view ray, so an orbit in progress just carries on.
        m_blendT = std::min(m_blendT + dtSeconds / kBlendSeconds, 1.0f);
        const float e = ease(m_blendT);
        const float t0 = std::tan(glm::radians(kBlendStartFov) * 0.5f);
        const float t1 = std::tan(glm::radians(m_fov) * 0.5f);
        const float th = glm::mix(t0, t1, e);
        m_fovEff = m_blendT >= 1.0f ? m_fov : glm::degrees(2.0f * std::atan(th));
        const glm::vec3 dir = safeDirection(m_position, m_target, glm::vec3(0.0f, 0.0f, 1.0f));
        m_position = m_target + dir * std::max(m_blendOrthoSize / th, 1e-3f);
        if (m_blendLevel) m_up = blendedUp(dir);
        if (m_blendT >= 1.0f) m_blendActive = false;
        moved = true;
    }
    if (!m_anim.active) return moved;
    Animation& a = m_anim;
    a.t += dtSeconds / std::max(a.duration, 1e-3f);
    if (a.t >= 1.0f) {
        finishAnimation();
        return true;   // the last step changed the view too
    }
    const float e = ease(a.t);

    // View direction: slerp. Opposite directions have no unique arc; pick any
    // axis perpendicular to the start so the camera swings around, not through.
    const glm::vec3 da = glm::normalize(a.p0 - a.t0);
    const glm::vec3 db = glm::normalize(a.p1 - a.t1);
    const float c = std::clamp(glm::dot(da, db), -1.0f, 1.0f);
    glm::vec3 dir;
    if (c > 0.9999f) {
        dir = glm::normalize(glm::mix(da, db, e));
    } else if (c < -0.9999f) {
        glm::vec3 axis = glm::cross(da, std::abs(da.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
        axis = glm::normalize(axis);
        const float ang = 3.14159265f * e;
        dir = da * std::cos(ang) + glm::cross(axis, da) * std::sin(ang);
    } else {
        const float th = std::acos(c);
        dir = (std::sin((1.0f - e) * th) * da + std::sin(e * th) * db) / std::sin(th);
    }
    dir = glm::normalize(dir);

    // Distance eases geometrically: a zoom looks even, not rushed at the end.
    const float dist0 = glm::length(a.p0 - a.t0);
    const float dist1 = glm::length(a.p1 - a.t1);
    const float dist = std::exp(glm::mix(std::log(dist0), std::log(dist1), e));

    m_target = glm::mix(a.t0, a.t1, e);
    m_position = m_target + dir * dist;

    // Up: blend, then make it perpendicular to the view so it never skews.
    glm::vec3 u = glm::mix(a.u0, a.u1, e);
    u -= dir * glm::dot(u, dir);
    if (glm::length(u) < 1e-4f) {
        u = a.u1 - dir * glm::dot(a.u1, dir);
        if (glm::length(u) < 1e-4f) u = m_up;
    }
    m_up = glm::normalize(u);
    return true;
}

void Camera::finishAnimation()
{
    if (!m_anim.active) return;
    const Animation a = m_anim;
    m_anim.active = false;
    m_position = a.endPos;
    m_target = a.t1;
    m_up = a.u1;
    m_orthographic = a.endOrtho;
    if (a.endOrtho) m_orthoSize = a.endOrthoSize;
}

} // namespace materializr
