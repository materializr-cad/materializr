#pragma once

#include <string>
#include <cstdint>
#include <vector>

struct SDL_Window;

namespace materializr {

// Windowing/GL-context wrapper. Backed by SDL2 on every platform: a GL 3.3 core
// context on desktop, a GL ES 3.0 context on Android. SDL gives us one input and
// windowing path for both, and maps touch events to mouse so the click-and-drag
// interaction model works on a phone unchanged.
class Window {
public:
    // uiScaleHint: the --ui-scale CLI value, or 0 to detect. Passed in because
    // the initial window SIZE must be scaled at creation time, and
    // setUiScaleOverride() lands after the constructor has already run.
    Window(int width, int height, const std::string& title,
           float uiScaleHint = 0.0f);
    ~Window();

    // Non-copyable, non-movable
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const { return m_shouldClose; }
    void requestClose(bool close = true) { m_shouldClose = close; }
    void swapBuffers();
    // Pump SDL events and forward them to the ImGui backend. If waitMs > 0,
    // blocks up to that many milliseconds for the first event before draining
    // (use this when idle to sleep instead of busy-spinning).
    // Returns: 0 = no events, 1 = trivial only (mouse motion, expose),
    //          2 = significant (click, key, scroll, resize, focus, touch).
    int pollEvents(int waitMs = 0);

    // True when this is the active foreground window (has input focus and is not
    // minimized/hidden). The main loop suspends rendering when false so a
    // backgrounded app uses ~no GPU (and stops contending with the compositor).
    bool isForeground() const;

    // Drawable (framebuffer) size in pixels - may exceed window size on HiDPI.
    void framebufferSize(int& w, int& h) const;

    // Hardware Ctrl state, polled directly so it works even while ImGui owns
    // keyboard focus (used by the undo/redo shortcut). Always false on Android.
    static bool isCtrlDown();

    // UI scale factor for HiDPI / touch. 1.0 on desktop; on Android it's derived
    // from the display DPI so fonts and widgets are finger-sized on a tablet.
    float uiScale() const;

    // Explicit desktop UI-scale preference (Settings → Appearance, Linux only).
    // > 0 overrides the platform default on Linux desktop; 0 = follow default.
    // Set before the font atlas is built (a change takes effect on restart).
    void setUiScaleOverride(float s) { m_uiScaleOverride = (s > 0.0f ? s : 0.0f); }
    // Linux/X11: size the X theme cursors to match uiScale(). Must be called
    // once the scale is final and BEFORE ImGui creates its system cursors -
    // Xcursor reads XCURSOR_SIZE as it loads each one, and never again.
    void applyCursorScale();

    // Raise/lower the soft keyboard to match ImGui's WantTextInput. The
    // SDL2 backend no longer calls SDL_StartTextInput itself, which is what shows
    // the keyboard on Android - so we drive it each frame. No-op on desktop.
    //
    // retapPulse: the user tapped this frame and a text field is STILL focused
    // afterwards (i.e. the tap landed in a field). The raise below is edge-
    // triggered, and the OS can dismiss the keyboard behind our back (Android
    // back gesture, iOS dismiss key) with the field still focused - no falling
    // edge, so re-tapping the field did nothing. The pulse re-raises even when
    // the latch says the keyboard is already up.
    void updateTextInput(bool wantTextInput, bool retapPulse = false);

    SDL_Window* handle() const { return m_window; }
    void* glContext() const { return m_glContext; }   // SDL_GLContext (opaque)
    int width() const { return m_width; }
    int height() const { return m_height; }

    // Touch-gesture output (Android). pollEvents() recognises two-finger
    // gestures; the viewport camera consumes the accumulated deltas each frame.
    // Returns true and writes the pending delta (then clears it) if any.
    bool consumeTouchPan(float& dx, float& dy);   // centroid movement, pixels
    bool consumeTouchZoom(float& dz);             // pinch delta, wheel-equivalent
    bool consumeDoubleTap();                      // true once after two quick taps (touch "double-click")
    // true once (with the tap position) after a GENUINE single tap lifts - not a
    // hold, drag, or 2-finger gesture. Selection commits off THIS instead of the
    // press frame so a following nav gesture (one-finger orbit / two-finger
    // pan-zoom) can't re-pick or clear it before the drag is recognized (#68).
    bool consumeSingleTap(float& x, float& y);
    bool consumeUndoTap();                        // true once after a two-finger tap (mobile undo gesture)
    bool consumeRedoTap();                        // true once after a three-finger tap (mobile redo gesture)

    // True if the most recent left-button release was NOT a genuine single-finger
    // lift but a two-finger gesture taking over (the second finger landing forces
    // a release). Lets press-drag-release placement ignore spurious releases.
    bool lastLeftReleaseWasGesture() const { return m_leftReleaseWasGesture; }

    // True while a two-finger pan/pinch gesture is in progress. The viewport
    // keeps its input latch alive through the gesture: the takeover parks the
    // cursor off-screen (see handleFingerEvent), which would otherwise drop
    // IsItemHovered and starve the pan/zoom consumption plus the release
    // handlers that used to see the frozen press position.
    bool twoFingerActive() const { return m_twoFinger; }

    // True once a one-finger press has been held stationary past the hold
    // threshold AND then dragged. The viewport uses this to start a box/drag-
    // select instead of orbiting - the touch equivalent of the desktop empty-
    // space left-drag, which trackpad mode otherwise reserves for orbit. A hold
    // that never drags is a long-press (context menu) instead, not a box-select.
    bool isTouchHoldSelect() const { return m_holdSelect && m_movedBeyondHold; }

    // Long-press progress for an on-screen feedback ring, 0 when not pressing.
    // Writes the press point (pixels) and returns 0..1 toward the hold threshold
    // (1.0 once armed and still held). Drawn by the app each frame on Android.
    float holdProgress(float& x, float& y) const;

    // The app reports each frame whether the current touch is over the 3D
    // viewport canvas (vs a panel/slider/overlay). The long-press only arms over
    // the canvas - otherwise slowly dragging a slider popped the context-menu
    // ring + a stray right-click.
    void setTouchOverViewport(bool v) { m_touchOverViewport = v; }
    // Strictly the 3D canvas (NOT the Items panel, unlike setTouchOverViewport
    // which also covers Items so long-press works there). Used to gate touch
    // drag-to-scroll: a vertical drag over any panel scrolls it, but a drag over
    // the canvas must stay an orbit.
    void setTouchOnCanvas(bool v) { m_touchOnCanvas = v; }

private:
    SDL_Window* m_window = nullptr;
    float m_uiScaleOverride = 0.0f;  // desktop UI-scale pref (Linux); 0 = default
    void* m_glContext = nullptr;
    bool m_shouldClose = false;
    int m_width;
    int m_height;
    // iOS only: SDL's color renderbuffer for the window - swapBuffers()
    // re-binds it before presenting (see Window.cpp). Stays 0 elsewhere.
    unsigned int m_windowRenderbuffer = 0;

    // Active touch points and the running two-finger gesture state.
    struct Finger { std::int64_t id; float x, y; };
    std::vector<Finger> m_fingers;
    bool  m_leftDown = false;            // is the synthetic left button held?
    bool  m_twoFinger = false;           // a two-finger gesture is active
    bool  m_suppressLeft = false;        // ignore a leftover finger after 2-finger
    float m_lastPinchDist = 0.0f;
    float m_lastCentroidX = 0.0f, m_lastCentroidY = 0.0f;
    float m_panAccX = 0.0f, m_panAccY = 0.0f, m_zoomAcc = 0.0f;
    // Two-finger gesture lock: a gesture commits to EITHER pan OR zoom once one
    // clearly dominates, so they don't fight mid-gesture (0 undecided/1 pan/2 zoom).
    int   m_twoFingerMode = 0;
    // Reference centroid/spacing captured when the two-finger gesture begins.
    // pan/zoom intent is judged from NET change vs these (not summed per-frame
    // deltas), so a slow pan's finger wobble can't accumulate into a false zoom.
    float m_startCentroidX = 0.0f, m_startCentroidY = 0.0f, m_startPinchDist = 0.0f;

    // Multi-finger tap gestures (two-finger tap = undo, three-finger = redo).
    // A "touch session" spans first-finger-down to all-fingers-up; the tap
    // fires on full lift-off when it was short, peaked at 2 or 3 fingers, and
    // the two-finger tracker never committed to pan/zoom.
    std::uint32_t m_sessionStartTicks = 0;
    int   m_sessionMaxFingers = 0;
    float m_sessionPanNet = 0.0f;   // peak net centroid travel while undecided
    float m_sessionZoomNet = 0.0f;  // peak net spacing change while undecided
    bool  m_undoTapPending = false;
    bool  m_redoTapPending = false;

    // One-finger press-and-hold tracking (-> box/drag-select).
    std::uint32_t m_downTicks = 0;        // SDL_GetTicks at single-finger down
    float m_downX = 0.0f, m_downY = 0.0f; // where it went down
    // Genuine double-tap: two quick taps (each a fast down-UP, not a hold) at the
    // same spot within the double-click time. Fires on the SECOND release, so a
    // tap-then-long-press can't trigger it (unlike ImGui's down-based double-click).
    std::uint32_t m_lastTapTick = 0;
    float m_lastTapX = 0.0f, m_lastTapY = 0.0f;
    bool  m_doubleTapPending = false;
    bool  m_singleTapPending = false;     // a genuine single tap lifted (see consumeSingleTap)
    float m_singleTapX = 0.0f, m_singleTapY = 0.0f;
    bool  m_movedBeyondHold = false;      // moved too far -> it's a drag, not a hold
    bool  m_holdSelect = false;           // hold threshold passed; select-drag mode
    bool  m_textInputActive = false;      // soft keyboard currently raised
    bool  m_leftReleaseWasGesture = false; // last left-up was a 2-finger takeover
    bool  m_touchOverViewport = false;    // current touch is over the canvas OR Items panel
    bool  m_touchOnCanvas = false;        // current touch is strictly over the 3D canvas
    // One-finger drag-to-scroll over a panel (touch mode). A vertical-dominant
    // drag converts the press into mobile-style flick scrolling of the hovered
    // window; a horizontal drag is left to widgets (sliders) untouched.
    bool  m_panelScroll = false;          // this gesture became a panel scroll
    bool  m_scrollArmed = false;          // crossed the scroll threshold once; commit next frame
                                          // (gives ImGui a frame to claim a tab/window drag first)
    float m_lastScrollY = 0.0f;           // finger Y at the last scroll step

    // Synthetic right-click queued by a long-press (touch context menu). Played
    // back over two frames (button down, then up) at the held point so ImGui's
    // right-click popups (Items/History tree items, viewport face/sketch menus)
    // open without a physical mouse. 0 = idle, 1 = press pending, 2 = release.
    int   m_rightClickPhase = 0;
    float m_rightClickX = 0.0f, m_rightClickY = 0.0f;

    void handleFingerEvent(unsigned type, std::int64_t id, float nx, float ny);
    void updateHoldSelect();              // per-frame hold check (Android)
    void pumpSyntheticRightClick();       // play back a queued long-press click
};

} // namespace materializr
