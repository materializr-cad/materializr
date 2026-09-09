#!/usr/bin/env bash
# Build Materializr.app and a distributable .dmg on macOS (Apple Silicon).
#
# Prereqs:
#   - a Release build in $BUILD_DIR (default: build/) - see BUILD.md (macOS)
#   - brew install dylibbundler
#
# The result is self-contained: every Homebrew/OpenCASCADE dylib the binary
# needs is copied into the .app and its install name rewritten, so the app runs
# on a Mac that has never seen Homebrew. It is ad-hoc signed (not notarized): a
# downloaded copy is quarantined, so the first launch needs
#   System Settings > Privacy & Security > "Open Anyway"
# (the old right-click > Open Gatekeeper bypass was removed in macOS 15), or
#   xattr -dr com.apple.quarantine Materializr.app
#
# NOTE: the bundled Homebrew dylibs are built for the host's macOS, so the .dmg's
# real minimum is the macOS it was built on. This script writes that true floor
# into LSMinimumSystemVersion (derived from the binaries) rather than guessing.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
BIN="$BUILD_DIR/materializr"
APP="$BUILD_DIR/Materializr.app"
VERSION="$(grep -m1 'project(Materializr VERSION' CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/')"
DMG="$BUILD_DIR/Materializr-${VERSION}-arm64.dmg"
BREW="$(brew --prefix)"

[ -x "$BIN" ] || { echo "error: $BIN not found - build first: cmake --build $BUILD_DIR"; exit 1; }
[ -n "$VERSION" ] || { echo "error: could not parse version from CMakeLists.txt"; exit 1; }
command -v dylibbundler >/dev/null || { echo "error: dylibbundler not found - brew install dylibbundler"; exit 1; }

# One scratch dir for the iconset + dmg staging, cleaned up on any exit.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> Assembling $APP (v$VERSION)"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/assets" "$APP/Contents/Frameworks"

cp "$BIN" "$APP/Contents/MacOS/materializr"

# Bundled fonts - resolveBundledFont() looks in <exe>/../Resources/assets/fonts.
cp -R assets/fonts "$APP/Contents/Resources/assets/"

# Icon: icon.png -> Materializr.icns (build a full iconset so Finder/Dock scale).
ICONSET="$WORK/Materializr.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
  sips -z "$s"        "$s"        icon.png --out "$ICONSET/icon_${s}x${s}.png"    >/dev/null
  sips -z "$((s*2))"  "$((s*2))"  icon.png --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/Materializr.icns"

# Copy every non-system dylib into Frameworks/ and rewrite install names to
# @executable_path/../Frameworks. Search paths cover the Homebrew + OCCT kegs so
# dylibbundler never needs to prompt interactively.
echo "==> Bundling dylibs"
dylibbundler -b -cd -of \
  -x "$APP/Contents/MacOS/materializr" \
  -d "$APP/Contents/Frameworks/" \
  -p "@executable_path/../Frameworks/" \
  -s "${SDL2_PREFIX:-$BREW}/lib" \
  -s "$BREW/lib" \
  -s "$BREW/opt/opencascade/lib" </dev/null

# dylibbundler rewrites each original rpath to the same @executable_path/../
# Frameworks, leaving duplicate LC_RPATH entries that dyld refuses to load
# ("duplicate LC_RPATH"). This happens in the main binary AND in any bundled OCCT
# dylib that shipped with more than one rpath, so collapse duplicates everywhere.
# -delete_rpath removes one matching entry per call.
RP='@executable_path/../Frameworks/'
dedupe_rpath() {
  while [ "$(otool -l "$1" | grep -c "path $RP")" -gt 1 ]; do
    install_name_tool -delete_rpath "$RP" "$1"
  done
}
dedupe_rpath "$APP/Contents/MacOS/materializr"
for dylib in "$APP/Contents/Frameworks/"*.dylib; do
  dedupe_rpath "$dylib"
done

# Effective minimum macOS = the highest LC_BUILD_VERSION 'minos' across the main
# binary and every bundled dylib - a load fails if the OS is older than ANY of
# them. Homebrew bottles are built for the host's macOS, so this reports what the
# .dmg actually requires instead of a fictional floor. Falls back to 11.0 only if
# nothing carries a build-version load command (very old toolchains).
minos_of() { otool -l "$1" | awk '/LC_BUILD_VERSION/{v=1} v&&/minos/{print $2; v=0}'; }
MINOS="$(minos_of "$APP/Contents/MacOS/materializr")"
for dylib in "$APP/Contents/Frameworks/"*.dylib; do
  m="$(minos_of "$dylib")" || true
  [ -n "$m" ] || continue
  if [ "$(printf '%s\n%s\n' "${MINOS:-0}" "$m" | sort -V | tail -1)" = "$m" ]; then MINOS="$m"; fi
done
: "${MINOS:=11.0}"
echo "==> Bundle minimum macOS: $MINOS"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Materializr</string>
  <key>CFBundleDisplayName</key><string>Materializr</string>
  <key>CFBundleExecutable</key><string>materializr</string>
  <key>CFBundleIdentifier</key><string>com.materializr.app</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleIconFile</key><string>Materializr</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>LSMinimumSystemVersion</key><string>${MINOS}</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.graphics-design</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>Materializr contributors - GNU GPL v3.</string>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Materializr Project</string>
      <key>CFBundleTypeExtensions</key><array><string>materializr</string></array>
      <key>CFBundleTypeRole</key><string>Editor</string>
      <key>LSHandlerRank</key><string>Owner</string>
    </dict>
  </array>
</dict>
</plist>
PLIST

# Ad-hoc sign inside-out: every nested dylib first, then the bundle. Apple
# deprecated --deep and signs nested code in an unspecified order with it; signing
# leaf-first is the supported way. Not notarized - see the header note.
echo "==> Ad-hoc signing"
find "$APP/Contents/Frameworks" -name '*.dylib' -exec codesign --force --sign - {} +
codesign --force --sign - "$APP"
# Gate on verification - on its own line so `set -e` aborts a broken seal. (The
# previous `... && echo OK` form let a failed verify slide straight into shipping,
# because the left side of && is exempt from errexit.)
codesign --verify --strict --verbose=2 "$APP"
echo "    signature OK"

# .dmg with an /Applications symlink for drag-to-install.
echo "==> Building $DMG"
STAGE="$WORK/dmg"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "Materializr ${VERSION}" -srcfolder "$STAGE" \
  -ov -format UDZO "$DMG" >/dev/null

echo "==> Done"
echo "    $APP"
echo "    $DMG  ($(du -h "$DMG" | cut -f1))"
