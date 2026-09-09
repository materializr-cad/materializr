#pragma once

#include <imgui.h>
#include <glm/glm.hpp>

namespace materializr {

class Camera;

enum class ViewCubeAction {
    None, Front, Back, Left, Right, Top, Bottom,
    FrontTopRight, FrontTopLeft, BackTopRight, BackTopLeft,
    FrontBottomRight, FrontBottomLeft, BackBottomRight, BackBottomLeft,
    // Edge (two-face) views - clicking the seam between two faces looks down
    // that edge so both faces are seen at once. One zero component each.
    TopFront, TopBack, TopLeft, TopRight,
    BottomFront, BottomBack, BottomLeft, BottomRight,
    FrontLeft, FrontRight, BackLeft, BackRight,
    RotateLeft, RotateRight, RotateUp, RotateDown,
    // Roll the camera 90° around the view axis (CCW / CW). Lets the user
    // re-orient a snapped ortho view (e.g. Top) without un-snapping.
    RollLeft, RollRight,
    // Snap to the default 3/4 isometric view (FrontTopRight equivalent).
    Home
};

class ViewCube {
public:
    ViewCube();

    // Render the view cube overlay. Call inside an ImGui window. Returns the
    // action if a face was clicked. `invertDrag` flips the orbit sign when the
    // user drags the cube body (configurable in Settings). `lightMode` flips the
    // "ink" (labels/borders/arrows/home) from light to dark for the light theme;
    // the blue/grey cube faces and the yellow hover are unchanged.
    // `releaseIsGesture`: the current left-release was synthesized by a
    // two-finger gesture takeover (Window::lastLeftReleaseWasGesture), not a
    // deliberate lift - treat it as a CANCEL of any armed click instead of a
    // commit, so a pinch whose first finger lands on the cube can't snap the
    // view (issue #38).
    ViewCubeAction render(Camera& camera, bool invertDrag = false,
                          bool lightMode = false, bool releaseIsGesture = false);

    // True while the mouse is over the cube/ring widget - the viewport uses
    // this to suppress its own selection logic so cube clicks don't pass through.
    bool wasHovered() const { return m_lastHovered; }

    // Extra offset (px) nudging the cube off its default top-right anchor:
    // leftPx moves it left (toward the viewport), topPx moves it down (positive)
    // or up (negative). The im-touch shells use it to clear their floating top
    // chrome / centre the Home glyph in the corner. 0,0 everywhere else.
    void setExtraOffset(float leftPx, float topPx) {
        m_extraLeft = leftPx; m_extraTop = topPx;
    }

    // Screen-space anchor of the last-rendered widget (valid after render()):
    // the horizontal centre and the y just below the lowest accessory. The snap
    // widget tucks under these so it tracks the cube through touch scaling AND
    // the im-touch vertical offsets, instead of overlapping it.
    float widgetCenterX() const { return m_widgetCenterX; }
    float widgetBottomY() const { return m_widgetBottomY; }

private:
    float m_size = 120.0f;
    float m_extraLeft = 0.0f;
    float m_extraTop = 0.0f;
    float m_widgetCenterX = 0.0f;
    float m_widgetBottomY = 0.0f;
    bool  m_lastHovered = false;
    // Cube drag-to-orbit state: a press on a face arms a pending snap; if the
    // user drags before releasing, treat it as a free orbit instead and
    // suppress the snap on release.
    bool           m_cubeDragging = false;
    ViewCubeAction m_pendingClick = ViewCubeAction::None;
};

} // namespace materializr
