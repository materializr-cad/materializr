# Building Materializr

One repo, four targets. The windowing/input backend is SDL2 on every platform;
the touch interface is a **runtime setting** (Settings ▸ General ▸ Touch mode,
default on for Android, off on desktop) — not a separate build.

## Linux (desktop)

```sh
sudo apt install build-essential cmake git libsdl2-dev libgl-dev \
    libocct-data-exchange-dev libocct-draw-dev libocct-foundation-dev \
    libocct-modeling-algorithms-dev libocct-modeling-data-dev \
    libocct-visualization-dev libcurl4-openssl-dev zlib1g-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/materializr
```

If `libsdl2-dev` is absent, CMake builds SDL 2.30.9 from source (needs the X11
dev headers). GLM and Dear ImGui are always fetched by CMake.

The release AppImage is built in Docker: `./scripts/build-appimage.sh`
(see `Dockerfile`; CI runs this on x86_64 and aarch64 via
`.github/workflows/linux.yml`).

## Windows

CI (`.github/workflows/windows.yml`) is the reference: vcpkg provides
`opencascade glew curl sdl2` (x64-windows), then a standard CMake/MSVC build
with `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`.

## macOS (Apple Silicon)

Do **not** `brew install sdl2`: Homebrew's `sdl2` formula is now an alias for
[`sdl2-compat`](https://github.com/libsdl-org/sdl2-compat), an SDL3-backed shim
whose dylib initializer aborts before `main()` when bundled into the `.app`
(issue #12). Build real SDL2 from source instead, the same way CI does:

```sh
brew install cmake opencascade

# SDL 2.30.9 from source (matches .github/workflows/macos.yml and the Android
# pin). MACOSX_DEPLOYMENT_TARGET=14.0 keeps a packaged .dmg loadable on
# macOS 14+ while still compiling against the current SDK.
curl -L --fail -o /tmp/sdl2.tar.gz \
  https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
echo "24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4  /tmp/sdl2.tar.gz" | shasum -a 256 -c -
tar -xzf /tmp/sdl2.tar.gz -C /tmp
cmake -S /tmp/SDL2-2.30.9 -B /tmp/sdl2-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_INSTALL_PREFIX="$HOME/sdl2-prefix"
cmake --build /tmp/sdl2-build -j$(sysctl -n hw.ncpu)
cmake --install /tmp/sdl2-build

cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/sdl2-prefix;$(brew --prefix)"
cmake --build build -j$(sysctl -n hw.ncpu)
./build/materializr
```

Needs the Xcode Command Line Tools (`xcode-select --install`) for AppleClang.
GLM and Dear ImGui are fetched by CMake; OpenCASCADE comes from Homebrew,
and curl + zlib from the macOS SDK. The GL backend uses the system OpenGL
framework (`<OpenGL/gl3.h>`) — no GLEW loader — with a forward-compatible **3.3
Core** context running the same GLSL 330 shaders as the other desktop targets.

Tested on arm64 (Apple Silicon), including HiDPI/Retina — the offscreen 3D
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
locally built `.dmg` requires that macOS or newer — the script writes the true
floor into `LSMinimumSystemVersion`. CI builds on the latest macOS runner with
SDL2 source-built at `MACOSX_DEPLOYMENT_TARGET=14.0`, so the released `.dmg`
targets **macOS 14+**; it is built, the bundle is launch-tested,
and the artifact uploaded on pushes to `main` (`.github/workflows/macos.yml`).
Not yet wired up: Intel/universal binaries and Developer-ID signing/notarization.

## FreeBSD (desktop, unofficial)

Not part of CI or the release matrix — no official FreeBSD binaries are
published, just a community build path. Builds clean on FreeBSD 15 with
system packages:

```sh
pkg install cmake sdl2 opencascade curl git
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
  CONFIG REQUIRED)` path on FreeBSD — previously only Windows/macOS did;
  Linux assumed a Debian `/usr/lib/<multiarch>` layout that doesn't exist here,
  since the port installs its CMake config under `/usr/local` like Homebrew.
- `std::thread` needs `Threads::Threads` linked explicitly — implicit via
  glibc on Linux, not on FreeBSD's libthr.
- Bundled-font path resolution (`Application::resolveBundledFont`) gained a
  `sysctl(KERN_PROC_PATHNAME)` branch alongside the Linux `/proc/self/exe`
  read: FreeBSD doesn't mount `/proc` by default, so the icon font (and every
  other bundled font) silently failed to resolve without it.

## Android (arm64-v8a)

Prerequisites: JDK 17, Android SDK + NDK r26.x, cmake, curl on the host.

```sh
# one-time: fetch + cross-compile SDL2 / FreeType / OpenCASCADE 7.8.1
# (sources are SHA-256 verified; ~30+ min for OCCT)
ANDROID_HOME=~/Android/Sdk ./android/scripts/setup-deps.sh

cd android && ./gradlew assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Native prerequisites land under `$MATERIALIZR_WORK` (default `~/Android`);
the OCCT `.so` set is staged into `android/app/src/main/jniLibs/` (not
committed — everything builds from pinned upstream source).

## Layout notes

- `src/` is shared by all targets. Platform code is guarded with
  `#if defined(__ANDROID__)`; touch *behaviour* gates on
  `materializr::touchMode()` (see `src/touch_mode.h`) so a tablet with a
  mouse — or a desktop touchscreen — can switch interaction models at runtime.
- `src/main.cpp` is the desktop entry; `src/android_main.cpp` (SDL_main) is
  Android's. Each build includes only its own.
- `android/` is self-contained (Gradle project, vendored SDL Java glue with a
  one-line soft-keyboard patch, dependency scripts).
