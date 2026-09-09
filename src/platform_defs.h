#pragma once

// Platform *meaning* macros, so guards say what they mean instead of naming an
// OS. The Android port keyed everything on __ANDROID__; iOS needs the same GL
// and touch behaviour but different OS services, so the guards are split:
//
//   MZ_GLES   - rendering on OpenGL ES 3.0 (Android + iOS). Covers every GL
//               API delta: '#version 300 es' shaders, no glGetTexImage /
//               GL_MULTISAMPLE / GL_POLYGON_OFFSET_LINE / geometry shaders /
//               glPointSize.
//   MZ_MOBILE - touch-first platform (Android + iOS): touch-mode defaults,
//               finger gestures, system document pickers (mobile_files.h),
//               mobile app lifecycle.
//   MZ_IOS    - iOS/iPadOS specifically (UIKit services live in src/ios_*).
//
// __ANDROID__ remains in use only for genuinely Android-only code (JNI, SAF,
// logcat, fdsan). NOTE: on iOS __APPLE__ is also defined - any __APPLE__ guard
// means "macOS desktop" ONLY if the MZ_IOS/MZ_GLES case is handled first
// (see gl_common.h).
//
// Keep this header dependency-free: it's included from gl_common.h and from
// headers with inline platform defaults (touch_mode.h, io/Settings.h).

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define MZ_IOS 1
#endif
#endif

#if defined(__ANDROID__) || defined(MZ_IOS)
#define MZ_GLES 1
#define MZ_MOBILE 1
#endif

//   MZ_TOUCH_INPUT - the SDL finger-gesture pipeline is COMPILED IN. Defined
//               everywhere: SDL2 reports SDL_FINGER* on desktop X11/Wayland
//               just as it does on Android, and a Surface/2-in-1 running the
//               desktop build has a real touchscreen. This is deliberately
//               separate from MZ_MOBILE, which stays "is a touch-FIRST
//               platform" and still gates the on-screen keyboard and the
//               mobile defaults.
//
// Compiling the pipeline in is not the same as running it: on desktop it stays
// dormant until opted into at runtime (materializr::desktopTouchEnabled() in
// touch_mode.h). Before this split the gestures were inside MZ_MOBILE, so on
// desktop handleFingerEvent() was an empty stub and touch mode delivered its
// larger UI with none of its input model.
#define MZ_TOUCH_INPUT 1
