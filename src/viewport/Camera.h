#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace materializr {

/// Arcball/orbit camera for the 3D viewport.
class Camera {
public:
    Camera();

    /// Rotate around target (arcball). Disables orthographic mode.
    void orbit(float deltaX, float deltaY);

    /// Rotate around target by an exact angle in radians. Disables orthographic mode.
    /// Used by the ViewCube rotate buttons.
    void rotateAroundTarget(float yawRadians, float pitchRadians);

    /// Translate camera and target in screen-aligned plane.
    void pan(float deltaX, float deltaY);

    /// Dolly: move closer/further from target.
    void zoom(float delta);

    /// Dolly toward (or away from) a specific world-space focus point - both
    /// camera and target move so the focus stays put on screen. Used by the
    /// mouse-wheel handler with `focus` set to the world point under the
    /// cursor (ray-pick onto geometry, fallback to the target-plane). Solves
    /// the "can't zoom into a small object far from origin" problem because
    /// the zoom centre is wherever you're already looking.
    void zoomToward(const glm::vec3& focus, float delta);

    /// Compute the view matrix from current camera state.
    glm::mat4 getViewMatrix() const;

    /// Compute the perspective projection matrix.
    glm::mat4 getProjectionMatrix() const;

    /// Update the aspect ratio (e.g. on viewport resize).
    void setAspect(float aspect);

    /// Reset to default isometric-like view (looking at origin from (5,5,5)).
    void reset();

    /// Frame the camera to fit a bounding box in view.
    void zoomToFit(glm::vec3 min, glm::vec3 max);

    /// Get the current camera position.
    glm::vec3 getPosition() const { return m_position; }

    /// Get the current target point.
    glm::vec3 getTarget() const { return m_target; }

    /// Vertical field of view in degrees. Picker uses this to convert a
    /// pixel-pickability radius into a depth-scaled world distance for
    /// thin geometry (construction axes).
    float getFov() const { return m_fov; }

    /// Get the current up vector.
    glm::vec3 getUp() const { return m_up; }

    /// Set the camera position.
    void setPosition(glm::vec3 pos);

    /// Set the camera target.
    void setTarget(glm::vec3 target);

    /// Set the camera up vector.
    void setUp(glm::vec3 up);

    /// Get the near clipping plane distance.
    float getNearPlane() const { return m_nearPlane; }

    /// Get the far clipping plane distance.
    float getFarPlane() const { return m_farPlane; }

    /// Toggle orthographic projection. Pan and zoom preserve this mode; orbit clears it.
    void setOrthographic(bool ortho) { m_orthographic = ortho; }
    bool isOrthographic() const { return m_orthographic; }

private:
    /// Turntable orbit around the world up axis (Y). Yaw spins around Y; pitch
    /// changes elevation, clamped short of the poles. The up vector is held level
    /// with the ground (never rolls), so the horizon stays flat.
    void orbitLevel(float yawDelta, float pitchDelta);

public:

    /// Half-height (world units) of the ortho view box. Width is derived from aspect.
    void setOrthoSize(float halfHeight) { m_orthoSize = halfHeight; }
    float getOrthoSize() const { return m_orthoSize; }

    /// When true (default), orbiting is a level turntable around world up (the
    /// horizon never rolls). When false, orbiting is a free trackball.
    void setLevelOrbit(bool level) { m_levelOrbit = level; }
    bool isLevelOrbit() const { return m_levelOrbit; }

    /// Uniform multiplier on orbit / pan / zoom input deltas - one user-facing
    /// sensitivity knob, so a slow trackpad doesn't whip the camera around at
    /// the same time as the desktop mouse cursor stays slow. 1.0 = the
    /// hard-coded baseline (m_orbitSpeed / m_panSpeed / m_zoomSpeed); below 1
    /// is calmer, above 1 is snappier. Persisted via AppSettings.
    void setMouseSensitivity(float s) { m_mouseSensitivity = s; }
    float getMouseSensitivity() const { return m_mouseSensitivity; }

    /// Viewport height in the same units the pan deltas arrive in (ImGui
    /// points). With this set, pan() moves the world EXACTLY one
    /// world-size-of-a-pixel per pixel dragged - the point under the cursor
    /// stays under the cursor at any zoom. Unset (<= 0) keeps the legacy
    /// distance-fraction pan. Refreshed every frame by the app.
    void setViewHeightPx(float px) { m_viewHeightPx = px; }

    /// Depth anchor for perspective pan: distance from the camera to the
    /// content the user is grabbing (the hover pick at drag start). The
    /// camera TARGET distance is a bad proxy on large projects - cursor-zoom
    /// can leave the target metres from (or millimetres in front of) the
    /// geometry on screen, making pan frozen or twitchy. <= 0 falls back to
    /// the target distance. Cleared when the pan gesture ends.
    void setPanRefDist(float d) { m_panRefDist = d; }

private:
    glm::vec3 m_position;
    glm::vec3 m_target;
    glm::vec3 m_up;

    float m_fov;
    float m_nearPlane;
    float m_farPlane;
    float m_aspect;

    bool m_orthographic = false;
    float m_orthoSize = 10.0f; // half-height in world units
    bool m_levelOrbit = true;  // turntable (level) vs free trackball orbit

    // Orbit sensitivity
    float m_orbitSpeed;
    float m_panSpeed;
    float m_zoomSpeed;
    // User-facing sensitivity multiplier (see setMouseSensitivity).
    float m_mouseSensitivity = 1.0f;
    // Exact-pan support (see setViewHeightPx / setPanRefDist).
    float m_viewHeightPx = 0.0f;
    float m_panRefDist = -1.0f;
};

} // namespace materializr
