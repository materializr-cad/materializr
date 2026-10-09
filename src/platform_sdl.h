#pragma once

// The ONLY door to SDL for code that is not the window/event layer.
//
// Application, the plugins, the file dialogs and the UI used to call SDL
// directly (SDL_GetTicks, SDL_GetPrefPath, SDL_OpenURL, ...). Funnelling those
// through here keeps the SDL surface down to Window.cpp, the ImGui backend glue
// and the per-platform shims (android_*, ios_*), so a backend change - SDL2 to
// SDL3 - touches a handful of files instead of twenty. This header is
// deliberately SDL-free: callers must not need SDL's headers.

#include <cstdint>
#include <string>

namespace materializr {

// Milliseconds since SDL initialised. 32-bit and WRAPPING (about 49 days), like
// SDL2's old SDL_GetTicks (SDL3's is 64-bit); compare with unsigned subtraction, never `<` on two
// absolute values.
std::uint32_t platformTicksMs();

// Block the calling thread for at least `ms` milliseconds.
void platformSleepMs(std::uint32_t ms);

// Per-user writable data directory for Materializr, created if missing, with a
// trailing path separator ("" if the platform can't provide one). Resolves to
// the right place everywhere, including Android app storage and the iOS
// sandbox.
std::string platformPrefPath();

// Hand `url` to the OS (never via a shell). Returns false and fills *err (if
// given) on failure. Scheme/host validation is the caller's job - see
// url_open.h.
bool platformOpenUrl(const std::string& url, std::string* err = nullptr);

#if defined(__ANDROID__)
// App-specific external storage directory ("" if unavailable).
std::string platformAndroidExternalStoragePath();
#endif

} // namespace materializr
