#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="$PROJECT_DIR/dist"
ARCH=$(uname -m)
# Optional: put the version in the file name (Materializr-<version>-<arch>.AppImage).
APP_VERSION="${APP_VERSION:-}"
NAME="Materializr-${APP_VERSION:+${APP_VERSION}-}${ARCH}"

echo "=== Building Materializr AppImage (${ARCH}) ==="

cd "$PROJECT_DIR"

# Build in Docker and extract the AppImage. --network=host: the default bridge
# network lost DNS on the GitHub runners (apt could not resolve the mirrors).
DOCKER_BUILDKIT=1 docker build --network=host --build-arg APP_VERSION="$APP_VERSION" --output type=local,dest="$OUTPUT_DIR" .

echo ""
echo "=== Build complete ==="
echo "AppImage: $OUTPUT_DIR/${NAME}.AppImage"
echo ""
echo "To run:"
echo "  chmod +x $OUTPUT_DIR/${NAME}.AppImage"
echo "  $OUTPUT_DIR/${NAME}.AppImage"
