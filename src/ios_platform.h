#pragma once
#include "platform_defs.h"

// iOS runtime services (implemented in ios_platform.mm). Safe to include
// everywhere: on non-iOS platforms the query is an inline constant so callers
// need no guards.
namespace materializr {

#if defined(MZ_IOS)

// Point the process at the app bundle + sandbox before Application starts:
// chdir to the bundle (so cwd-relative "assets/fonts/<name>" resolves), set the
// CSF_* env vars at the bundled OCCT resources, ensure $HOME/.config exists for
// settings, and install the UIKit lifecycle watch backing iosInBackground().
void iosInitRuntime();

// True between SDL_APP_WILLENTERBACKGROUND and SDL_APP_DIDENTERFOREGROUND.
// iOS terminates apps that touch the GL context while backgrounded, so the
// render loop must stop drawing whenever this is set (see Application::run).
bool iosInBackground();

// Screen safe-area insets in POINTS (same space as SDL window coords / ImGui):
// status bar + rounded corners at the top/sides, home indicator at the bottom.
// The UI's root work rect shrinks by these - see Application::beginFrame().
// Must be called from the main thread (the SDL loop is one on iOS).
void iosSafeAreaInsets(float& top, float& left, float& bottom, float& right);

#else

inline bool iosInBackground() { return false; }
inline void iosSafeAreaInsets(float& top, float& left, float& bottom, float& right) {
    top = left = bottom = right = 0.0f;
}

#endif

} // namespace materializr
