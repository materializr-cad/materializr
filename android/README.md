# Materializr - Android port

Cross-compiles the Materializr CAD app for **Android arm64-v8a**, reusing the
entire `core/` + `modeling/` geometry codebase unchanged. The desktop-only
layers (windowing, GL version, file dialogs) are abstracted so one source tree
builds both targets.

## What changed vs. desktop (all in the shared `src/` tree)

| Concern | Desktop | Android | Where |
|---|---|---|---|
| Windowing / input | GLFW | **SDL2** (unified - both platforms now use SDL2) | `src/app/Window.{h,cpp}` |
| ImGui backend | `imgui_impl_glfw` | `imgui_impl_sdl2` | `src/app/Application*.cpp` |
| GL | OpenGL 3.3 Core | **OpenGL ES 3.0** | `src/gl_common.h` |
| Shaders | GLSL 330 core | rewritten to GLSL ES 3.00 at upload | `src/gl_shader.cpp` |
| Entry point | `main()` + CLI | `SDL_main` | `src/android_main.cpp` |
| File dialogs | portable-file-dialogs | SAF stub (TODO) | `src/android_shims.cpp` |
| Update check | libcurl | disabled | `src/android_shims.cpp` |

The shader and window changes are **no-ops on desktop** - they're guarded by
`#if defined(__ANDROID__)`, so the desktop build is unaffected.

## Prerequisites (already set up under `~/Android/` on this machine)

- JDK 17 - `~/Android/jdk`
- Android SDK + NDK r26.3.11579264 + cmake 3.22.1 - `~/Android/Sdk`
- **Cross-compiled OpenCASCADE 7.8.1 + FreeType 2.13.3 (arm64-v8a)** -
  `~/Android/prefix/arm64-v8a/` (built by `~/Android/build-occt.sh` /
  `build-freetype.sh`)

## Build from scratch

One command reproduces every native prerequisite (SDL2 source, cross-compiled
FreeType + OpenCASCADE, the OCCT `.so` set + resources staged into the app):

```bash
export ANDROID_HOME=~/Android/Sdk        # SDK with NDK r26.x + cmake installed
cd android
./scripts/setup-deps.sh                  # ~40 min: cross-builds FreeType + OCCT
./gradlew assembleDebug                  # -> app/build/outputs/apk/debug/app-debug.apk
```

If the native prefix already exists, just stage + build:

```bash
./copy-occt-libs.sh && ./gradlew assembleDebug
```

`MATERIALIZR_OCCT_PREFIX` is passed to CMake from `app/build.gradle`; it defaults
to `~/Android/prefix/arm64-v8a` (override via the env var).

## Install & test on a device

The debug APK is arm64-v8a only - install on a 64-bit ARM phone/tablet (any
recent device). Either sideload the file via a file manager (enable “install
unknown apps”), or with adb:

```bash
~/Android/Sdk/platform-tools/adb install -r \
    app/build/outputs/apk/debug/app-debug.apk

# First-launch diagnostics - all startup logging uses the "Materializr" tag,
# plus SDL routes stdout/stderr (the app's verbose traces) to logcat:
~/Android/Sdk/platform-tools/adb logcat -c   # clear
~/Android/Sdk/platform-tools/adb logcat | grep -iE "Materializr|SDL|libc|DEBUG|tombstone"
```

On first launch the app extracts bundled fonts + OCCT resources to internal
storage and sets `HOME`/`CSF_*` accordingly (see `src/android_platform.cpp`).

## How the native build is wired

- `app/jni/CMakeLists.txt` → `add_subdirectory(SDL)` (symlink to the SDL2
  source) + `add_subdirectory(src)`.
- `app/jni/src/CMakeLists.txt` globs the whole `src/` tree (excluding the
  desktop-only `main.cpp`, `UpdateChecker.cpp`, `FileDialogs.cpp`), fetches
  ImGui (docking) + GLM, and links the cross-built OCCT toolkits as imported
  `.so` targets plus the NDK's GLESv3/EGL/z/log.
- `MaterializrActivity` (extends `SDLActivity`) loads `libSDL2.so` + `libmain.so`;
  the OCCT toolkits load transitively via `DT_NEEDED`.

## Remaining work (tracked)

1. **OCCT resource files (CSF_*)** - some kernel operations (units, text,
   shape-healing messages) read resource files. Bundle OCCT's `resources/` into
   the APK assets and set the `CSF_*` env vars at startup before any OCCT call.
2. **SAF file I/O** - replace the `android_shims.cpp` FileDialogs stub with
   `ACTION_OPEN_DOCUMENT` / `ACTION_CREATE_DOCUMENT` bridged through JNI, and
   make `ProjectIO` read/write via content URIs (or copy to app storage first).
3. **Font/asset extraction** - fonts in `assets/fonts/` are packaged but the
   path resolver (`Application.cpp`) uses exe-relative paths; on Android copy
   assets to internal storage at first launch and point the resolver there.
   (Until then ImGui falls back to its built-in font - the app still boots.)
4. **Touch UI pass** - multi-select toggle (replacing Ctrl), soft-keyboard for
   numeric fields, larger ImGui hit targets / touch-tuned style.
5. **Multi-ABI** - currently arm64-v8a only; add armeabi-v7a/x86_64 by
   re-running the OCCT/FreeType cross-build per ABI if needed.
