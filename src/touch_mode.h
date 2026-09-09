#pragma once
#include "platform_defs.h"
#include "i18n.h"
#include <cstdlib>

// Runtime "touch mode" flag. When ON, the UI scales up for fingers and input is
// interpreted as touch gestures (long-press context menus, press-drag-release
// drawing, on-screen Multi-Select / Move toggles, trackpad-style navigation,
// SVG-import without hover). When OFF, the desktop mouse/keyboard model applies.
//
// It defaults to the platform (on for Android, off elsewhere) but is a saved
// user setting, so a tablet with a mouse/keyboard/trackpad attached can run the
// full desktop interaction model - the touch adaptations are a mode, not baked
// into the build.
//
// The value is fixed for a run: Application sets it from the saved setting at
// startup (before fonts/scale are baked), and everything reads it live via
// touchMode(). Changing it in Settings persists the choice and takes full effect
// on the next launch. Header-only (inline function-local static) so it's a
// single shared instance across translation units with no extra .cpp / CMake
// entry - same pattern as ui_scale.h.
namespace materializr {

inline bool& touchModeRef() {
    static bool t =
#if defined(MZ_MOBILE)
        true;
#else
        false;
#endif
    return t;
}
inline bool touchMode() { return touchModeRef(); }
inline void setTouchMode(bool on) { touchModeRef() = on; }

// Desktop touch gestures: opt-in, via MATERIALIZR_DESKTOP_TOUCH=1.
//
// The gesture machine was written and tuned against Android and has never run
// on a desktop touchscreen, so it stays off by default until it has real
// mileage -- turning it on unconditionally would change input behaviour for
// every 2-in-1 and touch-monitor user on the strength of an untested code
// path. What they get today is SDL's own touch->mouse synthesis, which is
// crude (no pinch, no long-press) but predictable.
//
// Read once and cached: the SDL hint that disables that synthesis is set when
// the window is built, so the answer must not change underneath a running
// session. Env var rather than a saved setting for the same reason -- it is a
// launch-time decision, and it keeps an unproven path out of the Settings UI
// until it earns a place there.
inline bool desktopTouchEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("MATERIALIZR_DESKTOP_TOUCH");
        return e && *e && *e != '0';
    }();
    return on;
}

// Should finger events drive the gesture pipeline this run? Always on a
// touch-first platform; on desktop only when opted in above.
inline bool touchInputActive() {
#if defined(MZ_MOBILE)
    return true;
#else
    return desktopTouchEnabled();
#endif
}

// Commit/cancel/create button labels. In touch mode drop the keyboard hint -
// there are no Enter/Esc keys, and "(Enter)" just eats space and confuses. So
// "Confirm (Enter)" -> "Confirm", "Cancel (Esc)" -> "Cancel", etc.
inline const char* btnConfirm() {
    return tr(touchMode() ? "Confirm" : "Confirm (Enter)");
}
inline const char* btnCancel()  {
    return tr(touchMode() ? "Cancel"  : "Cancel (Esc)");
}
inline const char* btnCreate()  { return touchMode() ? "Create"  : "Create (Enter)"; }

} // namespace materializr
