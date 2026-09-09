# Building from Source

The released binaries (AppImage / Windows zip / installer) are produced by CI
from this same source - see `.github/workflows/windows.yml` and
`scripts/build-appimage.sh`. The instructions below cover building locally
when you want to hack on the code.

## Linux (native)

### Prerequisites (Ubuntu / Debian)

```bash
sudo apt-get install -y \
  build-essential cmake git \
  libocct-data-exchange-dev \
  libocct-draw-dev \
  libocct-foundation-dev \
  libocct-modeling-algorithms-dev \
  libocct-modeling-data-dev \
  libocct-visualization-dev \
  libgl-dev libxrandr-dev libxinerama-dev \
  libxcursor-dev libxi-dev libxkbcommon-dev \
  libwayland-dev pkg-config \
  libcurl4-openssl-dev
```

### Build

```bash
git clone https://github.com/materializr-cad/materializr.git
cd materializr
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
./materializr
```

SDL2, GLM, and Dear ImGui are pulled in via CMake `FetchContent`, so they
don't need to be installed system-wide. (A system SDL2 is used when present;
otherwise 2.30.9 is built from source, which needs the X11 dev headers the GL
stack already requires.)

### Tests

```bash
cd build
ctest --output-on-failure
```

## Linux AppImage (Docker)

`scripts/build-appimage.sh` runs the whole pipeline in a Docker container,
producing a portable single-file binary that bundles OCCT, TBB, and Freetype.

```bash
./scripts/build-appimage.sh
# Output: dist/Materializr-<arch>.AppImage
```

Requires Docker with BuildKit enabled. The container builds OCCT-link-ready
release binaries and emits the AppImage via `appimagetool`.

## Windows

The Windows build uses **MSVC** (Visual Studio 2022) and **vcpkg** for
OpenCASCADE, GLEW, and libcurl. Set `VCPKG_ROOT` to your vcpkg checkout
before configuring.

```powershell
git clone https://github.com/materializr-cad/materializr.git
cd materializr
& "$env:VCPKG_ROOT\vcpkg.exe" install `
    opencascade:x64-windows glew:x64-windows curl:x64-windows
cmake -B build -S . `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DBUILD_TESTS=OFF
cmake --build build --config Release --parallel
```

The resulting executable lives under `build/Release/`, with the required DLLs
copied next to it by vcpkg's applocal-deps step.

### Packaging (portable zip + NSIS installer)

The GitHub Actions workflow (`.github/workflows/windows.yml`) does the
packaging step automatically. To reproduce locally, copy
`build/Release/materializr.exe` plus every `.dll` in that directory (and
anything missed from `$VCPKG_ROOT/installed/x64-windows/bin/`) into a staging
folder, then run NSIS against `packaging/windows/installer.nsi`.

## CMake options

| Option | Default | Effect |
|--------|---------|--------|
| `BUILD_TESTS` | `ON` | Build the Google Test unit suite under `tests/`. |
| `CMAKE_BUILD_TYPE` | (unset) | Standard; pass `Release` for production builds. |

The project's version is set in the top-level `project(... VERSION ...)`
call and forwarded to the C++ code as `MATERIALIZR_VERSION` (used by the
About dialog and the update checker).
