#pragma once

namespace materializr {

// Android-only startup hook (no-op on desktop). Run ONCE before constructing
// Application. It:
//   * points $HOME at app-private internal storage so SettingsIO::defaultPath()
//     writes a readable/writable settings file (no code change there);
//   * chdir()s there and extracts the bundled TTFs so resolveBundledFont()'s
//     cwd-relative "assets/fonts/" lookup succeeds (no code change there);
//   * extracts the bundled OpenCASCADE resource tree and sets the CSF_* env vars
//     OCCT consults for unit/message/STEP/IGES data.
void androidInitRuntime();

// Pixels at the top of the window covered by system UI (status bar / freeform
// caption bar / cutout), in the same units as ImGui coordinates. 0 off Android
// and wherever the window is already laid out below the bar. Cheap to call every
// frame: the JNI query is throttled and the last value reused.
int androidTopInsetPx();

// True in the Android build. Lets settings UI for Android-only options be
// compiled (and type-checked) on every platform instead of hiding behind #ifdef.
#ifdef __ANDROID__
inline constexpr bool kIsAndroid = true;
#else
inline constexpr bool kIsAndroid = false;
#endif

// Hide (true) or keep (false) the status bar, on top of the navigation bar the
// app always hides. Remembers the last value and only calls into Java when it
// changes, so it is safe to call every frame. No-op off Android.
void androidSetFullscreen(bool on);

} // namespace materializr
