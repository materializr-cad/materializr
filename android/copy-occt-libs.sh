#!/usr/bin/env bash
# Copy the cross-built OpenCASCADE (+ its runtime deps) .so set into the APK's
# jniLibs so they're packaged and loadable at runtime. Run once after the OCCT
# cross-build completes (and re-run if OCCT is rebuilt).
set -euo pipefail

ABI="${1:-arm64-v8a}"
PREFIX="${MATERIALIZR_OCCT_PREFIX:-$HOME/Android/prefix/$ABI}"
DEST="$(cd "$(dirname "$0")" && pwd)/app/src/main/jniLibs/$ABI"

mkdir -p "$DEST"
shopt -s nullglob
libs=("$PREFIX"/lib/libTK*.so)
if [ ${#libs[@]} -eq 0 ]; then
    echo "No OCCT libraries found in $PREFIX/lib - build OCCT first." >&2
    exit 1
fi
cp -v "${libs[@]}" "$DEST"/
echo "Copied ${#libs[@]} OCCT toolkits to $DEST"

# Refresh the bundled OCCT resource tree (units/messages/STEP-IGES config) and
# the extraction manifest the app reads at first launch. Only the dirs OCCT
# actually consults for modeling + data exchange.
RES="$PREFIX/share/opencascade/resources"
ASSETS="$(cd "$(dirname "$0")" && pwd)/app/src/main/assets"
if [ -d "$RES" ]; then
    rm -rf "$ASSETS/occt-resources"
    mkdir -p "$ASSETS/occt-resources"
    for d in UnitsAPI StdResource XSMessage SHMessage XSTEPResource BOPAlgo TObj XmlOcafResource; do
        [ -d "$RES/$d" ] && cp -a "$RES/$d" "$ASSETS/occt-resources/"
    done
    ( cd "$ASSETS/occt-resources" && find . -type f | sed 's#^\./##' | sort ) > "$ASSETS/occt-resources.list"
    echo "Staged $(wc -l < "$ASSETS/occt-resources.list") OCCT resource files into assets"
fi
