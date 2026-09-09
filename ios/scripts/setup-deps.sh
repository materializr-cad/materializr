#!/usr/bin/env bash
# Cross-builds every native prerequisite of the iOS app into a static prefix:
#   * FreeType 2.13.3 (static)
#   * OpenCASCADE 7.9.3 (static - no dylib embedding, and static sidesteps the
#     empty-Standard_EXPORT inline-symbol problem documented for the Android
#     shared build in android/scripts/setup-deps.sh)
#   * SDL2 2.30.9 (static)
#
# Same pinned versions and SHA-256s as the Android build - one supply chain,
# two mobile targets. Run on macOS with Xcode 15+ and CMake 3.24+ installed.
#
# Env overrides:
#   MATERIALIZR_IOS_WORK  work dir (default ~/iOS): downloads, sources, builds, prefix
#   MIN_IOS               deployment target (default 15.0)
#   SIMULATOR=1           build for the arm64 iPhoneSimulator instead of devices
#   JOBS                  parallel build jobs (default: hw.ncpu)
set -euo pipefail

MIN_IOS="${MIN_IOS:-15.0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
SDL_VER="2.30.9"
FT_VER="2.13.3"
OCCT_TAG="V7_9_3"

if [ "${SIMULATOR:-0}" = "1" ]; then
    SYSROOT="iphonesimulator"
else
    SYSROOT="iphoneos"
fi
PLATFORM="$SYSROOT-arm64"

WORK="${MATERIALIZR_IOS_WORK:-$HOME/iOS}"
PREFIX="$WORK/prefix/$PLATFORM"
DL="$WORK/dl"; SRC="$WORK/src"; BUILD="$WORK/build"
mkdir -p "$DL" "$SRC" "$BUILD" "$PREFIX"

command -v cmake >/dev/null || { echo "cmake not found (brew install cmake)"; exit 1; }
xcrun --sdk "$SYSROOT" --show-sdk-path >/dev/null || { echo "Xcode SDK '$SYSROOT' not found"; exit 1; }

echo "SDK:    $SYSROOT (min iOS $MIN_IOS)"
echo "PREFIX: $PREFIX"

# Expected SHA-256 of each pinned source tarball - verified after download so a
# corrupted mirror or tampered upstream can't slip in. Identical pins to
# android/scripts/setup-deps.sh.
SDL2_SHA256="24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4"
FT_SHA256="5c3a8e78f7b24c20b25b54ee575d6daa40007a5f4eea2845861c3409b3021747"
OCCT_SHA256="5ecf094ec6b12d5413dfb851d8c3590c354058aee556e32e408bdfbf8c357d57"

fetch() { # url dest sha256
    [ -f "$2" ] || curl -L --fail --retry 3 -o "$2" "$1"
    if [ -n "$3" ]; then
        echo "$3  $2" | shasum -a 256 -c - || {
            echo "ERROR: checksum mismatch for $2 - refusing to build." >&2
            rm -f "$2"; exit 1
        }
    fi
}

# Shared iOS cross-compile settings. CMake supports iOS natively - no
# third-party toolchain file. TRY_COMPILE=STATIC_LIBRARY lets configure-time
# feature checks link without an iOS code-signing identity.
IOS_CMAKE_FLAGS=(
    -DCMAKE_SYSTEM_NAME=iOS
    -DCMAKE_OSX_SYSROOT="$SYSROOT"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS"
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_FIND_ROOT_PATH="$PREFIX"
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH
)

# ── FreeType (static) ────────────────────────────────────────────────────────
fetch "https://download.savannah.gnu.org/releases/freetype/freetype-$FT_VER.tar.gz" "$DL/freetype.tar.gz" "$FT_SHA256"
[ -d "$SRC/freetype-$FT_VER" ] || tar -xzf "$DL/freetype.tar.gz" -C "$SRC"
rm -rf "$BUILD/freetype-$PLATFORM"
cmake -S "$SRC/freetype-$FT_VER" -B "$BUILD/freetype-$PLATFORM" \
    "${IOS_CMAKE_FLAGS[@]}" -DBUILD_SHARED_LIBS=OFF \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON
cmake --build "$BUILD/freetype-$PLATFORM" --target install -j"$JOBS"

# ── OpenCASCADE (static) ─────────────────────────────────────────────────────
# Same module set as Android. Keep 7.9.x - do not bump to 8.0.x without
# re-reading the version note in android/scripts/setup-deps.sh; the known
# failure is shared-lib-specific, but stay in lockstep until verified.
fetch "https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/$OCCT_TAG.tar.gz" "$DL/occt.tar.gz" "$OCCT_SHA256"
OCCT_DIR="$SRC/OCCT-${OCCT_TAG#V}"
[ -d "$OCCT_DIR" ] || tar -xzf "$DL/occt.tar.gz" -C "$SRC"

rm -rf "$BUILD/occt-$PLATFORM"
cmake -S "$OCCT_DIR" -B "$BUILD/occt-$PLATFORM" \
    "${IOS_CMAKE_FLAGS[@]}" \
    -DBUILD_LIBRARY_TYPE=Static \
    -DBUILD_MODULE_Draw=OFF -DBUILD_DOC_Overview=OFF -DBUILD_SAMPLES_QT=OFF \
    -DUSE_FREETYPE=ON -D3RDPARTY_FREETYPE_DIR="$PREFIX" \
    -D3RDPARTY_FREETYPE_INCLUDE_DIR_freetype2="$PREFIX/include/freetype2" \
    -D3RDPARTY_FREETYPE_INCLUDE_DIR_ft2build="$PREFIX/include/freetype2" \
    -D3RDPARTY_FREETYPE_LIBRARY="$PREFIX/lib/libfreetype.a" \
    -D3RDPARTY_FREETYPE_LIBRARY_DIR="$PREFIX/lib" \
    -DUSE_TK=OFF -DUSE_TCL=OFF -DUSE_FREEIMAGE=OFF -DUSE_TBB=OFF -DUSE_VTK=OFF \
    -DUSE_RAPIDJSON=OFF -DUSE_OPENVR=OFF -DUSE_DRACO=OFF -DUSE_FFMPEG=OFF
cmake --build "$BUILD/occt-$PLATFORM" --target install -j"$JOBS"

# ── SDL2 (static) ────────────────────────────────────────────────────────────
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" "$DL/sdl2.tar.gz" "$SDL2_SHA256"
[ -d "$SRC/SDL2-$SDL_VER" ] || tar -xzf "$DL/sdl2.tar.gz" -C "$SRC"
rm -rf "$BUILD/sdl2-$PLATFORM"
# Joystick/haptic/sensor/hidapi OFF: a CAD app uses none of them, and their
# iOS backends reference CoreBluetooth/CoreMotion/CoreHaptics/GameController -
# App Store validation (ITMS-90683) demands purpose strings for APIs the
# binary merely references. SDL keeps its public API as stubs, so callers
# (e.g. ImGui's gamepad path) still link; the subsystems just report absent.
cmake -S "$SRC/SDL2-$SDL_VER" -B "$BUILD/sdl2-$PLATFORM" \
    "${IOS_CMAKE_FLAGS[@]}" \
    -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TEST=OFF \
    -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_SENSOR=OFF -DSDL_HIDAPI=OFF
cmake --build "$BUILD/sdl2-$PLATFORM" --target install -j"$JOBS"

echo
echo "Done. Static prerequisites installed to:"
echo "  $PREFIX"
echo "Now configure the app (from the repo root):"
echo "  cmake -S ios -B build-ios -GXcode -DCMAKE_SYSTEM_NAME=iOS \\"
echo "        -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN_IOS -DCMAKE_OSX_ARCHITECTURES=arm64 \\"
echo "        -DMATERIALIZR_IOS_PREFIX=$PREFIX"
echo "  open build-ios/MaterializriOS.xcodeproj"
