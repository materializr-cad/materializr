# Building Materializr

One repo, four targets. The windowing/input backend is SDL3 (pinned to 3.4.18, `MZ_SDL3_VERSION` in
`CMakeLists.txt`) on every platform;
the touch interface is a **runtime setting** (Settings ▸ General ▸ Touch mode,
default on for Android, off on desktop) - not a separate build.

## Linux (desktop)

```sh
sudo apt install build-essential cmake git libgl-dev \
    libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxfixes-dev \
    libwayland-dev libxkbcommon-dev libdecor-0-dev libegl-dev \
    libocct-data-exchange-dev libocct-draw-dev libocct-foundation-dev \
    libocct-modeling-algorithms-dev libocct-modeling-data-dev \
    libocct-visualization-dev libcurl4-openssl-dev zlib1g-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/materializr
```

CMake uses a system SDL3 only if it is at least 3.4.18; otherwise it downloads
and builds the pinned release (the X11/Wayland headers above are what it needs;
Wayland needs `wayland-scanner` too). `libdecor` is loaded at runtime and is
optional on the machine that runs the app. GLM and Dear ImGui are always fetched
by CMake.

On a Wayland session the app runs natively on Wayland. `--x11` (or
`SDL_VIDEO_DRIVER=x11`) forces X11/XWayland instead; on GNOME without `libdecor`
installed it falls back to XWayland by itself so the window keeps a title bar.

The release AppImage is built in Docker: `./scripts/build-appimage.sh`
(see `Dockerfile`; CI runs this on x86_64 and aarch64 via
`.github/workflows/linux.yml`).

## Windows

CI (`.github/workflows/windows.yml`) is the reference: vcpkg provides
`opencascade glew curl` (x64-windows), then a standard CMake/MSVC build
with `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`. SDL3 is not a
vcpkg dependency: CMake downloads the pinned release and links it statically.

## macOS (Apple Silicon)

Do **not** `brew install sdl3` for packaging: a Homebrew bottle is compiled for the
runner's own macOS with no deployment-target control, which is what broke
issue #12 (bottle initializer aborting on newer macOS, or a minos too high for
older ones). Build SDL3 from source, the same way CI does:

```sh
brew install cmake opencascade

# SDL 3.4.18 from source (matches .github/workflows/macos.yml and MZ_SDL3_VERSION).
# MACOSX_DEPLOYMENT_TARGET=14.0 keeps a packaged .dmg loadable on macOS 14+
# while still compiling against the current SDK.
curl -L --fail -o /tmp/sdl3.tar.gz \
  https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
echo "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3  /tmp/sdl3.tar.gz" | shasum -a 256 -c -
tar -xzf /tmp/sdl3.tar.gz -C /tmp
cmake -S /tmp/SDL3-3.4.18 -B /tmp/sdl3-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX="$HOME/sdl3-prefix"
cmake --build /tmp/sdl3-build -j$(sysctl -n hw.ncpu)
cmake --install /tmp/sdl3-build

cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/sdl3-prefix;$(brew --prefix)"
cmake --build build -j$(sysctl -n hw.ncpu)
./build/materializr
```

Needs the Xcode Command Line Tools (`xcode-select --install`) for AppleClang.
GLM and Dear ImGui are fetched by CMake; OpenCASCADE comes from Homebrew,
and curl + zlib from the macOS SDK. The GL backend uses the system OpenGL
framework (`<OpenGL/gl3.h>`) - no GLEW loader - with a forward-compatible **3.3
Core** context running the same GLSL 330 shaders as the other desktop targets.

Tested on arm64 (Apple Silicon), including HiDPI/Retina - the offscreen 3D
viewport renders at the display's pixel resolution.

A self-contained `Materializr.app` + `.dmg` is built by
`./packaging/macos/build-dmg.sh` (run after the build above; needs
`brew install dylibbundler`). It copies every Homebrew/OpenCASCADE dylib into
the bundle and rewrites install names, so the app runs on a Mac that has never
seen Homebrew. It is ad-hoc signed (not notarized): a downloaded copy is
quarantined, so the first launch needs **System Settings ▸ Privacy & Security ▸
"Open Anyway"** (macOS 15 removed the old right-click ▸ Open bypass), or
`xattr -dr com.apple.quarantine Materializr.app`.

The bundled dylibs are built for the macOS they were compiled on, so a
locally built `.dmg` requires that macOS or newer - the script writes the true
floor into `LSMinimumSystemVersion`. CI builds on the latest macOS runner with
SDL2 source-built at `MACOSX_DEPLOYMENT_TARGET=14.0`, so the released `.dmg`
targets **macOS 14+**; it is built, the bundle is launch-tested,
and the artifact uploaded on pushes to `main` (`.github/workflows/macos.yml`).
Not yet wired up: Intel/universal binaries and Developer-ID signing/notarization.

## FreeBSD (desktop, unofficial)

Not part of CI or the release matrix - no official FreeBSD binaries are
published, just a community build path. Builds clean on FreeBSD 15 with
system packages:

```sh
pkg install cmake sdl3 opencascade curl git
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/materializr
```

`opencascade` pulls in a large dependency tree (Qt5, VTK) via the port's
default build options; there's no slimmer flavor at time of writing. GLM and
Dear ImGui are fetched by CMake as usual, same as every other desktop target.

Three portability gaps had to be closed to get a clean build here, all fixed
in-tree (nothing FreeBSD-specific needed at the command line beyond the
`pkg install` above):
- OpenCASCADE detection now also takes the clean `find_package(OpenCASCADE
  CONFIG REQUIRED)` path on FreeBSD - previously only Windows/macOS did;
  Linux assumed a Debian `/usr/lib/<multiarch>` layout that doesn't exist here,
  since the port installs its CMake config under `/usr/local` like Homebrew.
- `std::thread` needs `Threads::Threads` linked explicitly - implicit via
  glibc on Linux, not on FreeBSD's libthr.
- Bundled-font path resolution (`Application::resolveBundledFont`) gained a
  `sysctl(KERN_PROC_PATHNAME)` branch alongside the Linux `/proc/self/exe`
  read: FreeBSD doesn't mount `/proc` by default, so the icon font (and every
  other bundled font) silently failed to resolve without it.

## Android (arm64-v8a)

Prerequisites: JDK 17, Android SDK + NDK r26.x, cmake, curl on the host.

```sh
# one-time: fetch + cross-compile SDL3 / FreeType / OpenCASCADE 7.8.1
# (sources are SHA-256 verified; ~30+ min for OCCT)
ANDROID_HOME=~/Android/Sdk ./android/scripts/setup-deps.sh

cd android && ./gradlew assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Native prerequisites land under `$MATERIALIZR_WORK` (default `~/Android`);
the OCCT `.so` set is staged into `android/app/src/main/jniLibs/` (not
committed - everything builds from pinned upstream source).

## Layout notes

- `src/` is shared by all targets. Platform code is guarded with
  `#if defined(__ANDROID__)`; touch *behaviour* gates on
  `materializr::touchMode()` (see `src/touch_mode.h`) so a tablet with a
  mouse - or a desktop touchscreen - can switch interaction models at runtime.
- `src/main.cpp` is the desktop entry; `src/android_main.cpp` (SDL_main) is
  Android's. Each build includes only its own.
- `android/` is self-contained (Gradle project, vendored SDL Java glue with a
  one-line soft-keyboard patch, dependency scripts).
