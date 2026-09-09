FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Build deps. OCCT is NOT taken from apt (Ubuntu 24.04 ships 7.6.3) - it is
# compiled from source below so every platform runs the SAME kernel (7.9.3):
# matching Android/macOS and pulling Windows off vcpkg's 8.0 (which hangs long-
# rod thread generation). fontconfig + the X11 dev headers are OCCT build-deps
# (Font_FontMgr includes fontconfig unconditionally).
RUN apt-get update && apt-get install -y \
    build-essential cmake git wget ca-certificates \
    libfreetype-dev libfontconfig1-dev \
    libgl-dev libglu1-mesa-dev \
    libx11-dev libxext-dev libxmu-dev libxt-dev \
    libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libxkbcommon-dev libwayland-dev pkg-config \
    libsdl2-dev \
    libcurl4-openssl-dev \
    zlib1g-dev \
    file patchelf fuse libfuse2 \
    imagemagick \
    zsync \
    && rm -rf /var/lib/apt/lists/*

# ─── OpenCASCADE 7.9.3 from source (shared) -> /usr/local ────────────────────
# Pinned tag + sha256 (identical to android/scripts/setup-deps.sh) for supply-
# chain integrity. Modules mirror the old apt set minus Draw (Tcl/Tk); FreeType
# ON for the Text tool's Font_BRepFont; heavy optional 3rd-parties OFF to stay
# lean. Adds ~30 min to a cold Linux build (no distro OCCT anymore).
ARG OCCT_TAG=V7_9_3
ARG OCCT_SHA256=5ecf094ec6b12d5413dfb851d8c3590c354058aee556e32e408bdfbf8c357d57
RUN wget -q "https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/${OCCT_TAG}.tar.gz" -O /tmp/occt.tar.gz \
    && echo "${OCCT_SHA256}  /tmp/occt.tar.gz" | sha256sum -c - \
    && mkdir -p /tmp/occt && tar -xzf /tmp/occt.tar.gz -C /tmp/occt --strip-components=1 \
    && cmake -S /tmp/occt -B /tmp/occt-build \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_LIBRARY_TYPE=Shared \
        -DBUILD_MODULE_Draw=OFF -DBUILD_DOC_Overview=OFF -DBUILD_SAMPLES_QT=OFF \
        -DUSE_FREETYPE=ON -DUSE_TK=OFF -DUSE_TCL=OFF \
        -DUSE_FREEIMAGE=OFF -DUSE_TBB=OFF -DUSE_VTK=OFF -DUSE_RAPIDJSON=OFF \
        -DUSE_OPENVR=OFF -DUSE_DRACO=OFF -DUSE_FFMPEG=OFF \
    && cmake --build /tmp/occt-build --target install -j"$(nproc)" \
    && ldconfig \
    && rm -rf /tmp/occt /tmp/occt-build /tmp/occt.tar.gz

WORKDIR /src
COPY . .

# Build the project against the source-built OCCT in /usr/local (MZR_OCCT_PREFIX
# routes CMake to the clean from-source config, skipping the Debian apt path).
RUN mkdir -p build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
        -DMZR_OCCT_PREFIX=/usr/local \
    && make -j$(nproc)

# appimagetool - pinned tag + per-arch sha256, same treatment as OCCT above.
#
# This was `continuous`: a rolling tag upstream force-updates, downloaded with
# no integrity check and then EXECUTED to package every shipped Linux release.
# Anyone able to publish to that tag (or interpose on the download) could inject
# into the AppImage users install, and nothing in the pipeline would notice.
#
# 1.9.1 is the current stable release, and is byte-identical to what
# `continuous` serves today - so this pins the behaviour we already have rather
# than changing versions. Bumping is then a deliberate, reviewable diff.
ARG APPIMAGETOOL_VERSION=1.9.1
ARG APPIMAGETOOL_SHA256_X86_64=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
ARG APPIMAGETOOL_SHA256_AARCH64=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
RUN ARCH=$(uname -m) \
    && case "$ARCH" in \
         x86_64)  AIT_SHA="$APPIMAGETOOL_SHA256_X86_64" ;; \
         aarch64) AIT_SHA="$APPIMAGETOOL_SHA256_AARCH64" ;; \
         *) echo "no pinned appimagetool checksum for arch $ARCH" >&2; exit 1 ;; \
       esac \
    && wget -q "https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-${ARCH}.AppImage" \
        -O /usr/local/bin/appimagetool \
    && echo "${AIT_SHA}  /usr/local/bin/appimagetool" | sha256sum -c - \
    && chmod +x /usr/local/bin/appimagetool

# ─── Create AppDir structure ────────────────────────────────────────────────

RUN mkdir -p /AppDir/usr/bin /AppDir/usr/lib \
    /AppDir/usr/share/icons/hicolor/256x256/apps \
    /AppDir/usr/share/icons/hicolor/512x512/apps \
    /AppDir/usr/share/materializr/fonts

# Copy binary
RUN cp /src/build/materializr /AppDir/usr/bin/materializr

# Bundle every TTF from assets/fonts: JetBrains Mono for the ImGui UI font,
# plus DejaVu Sans/Serif for the sketch Text tool's font picker. Resolved at
# runtime via the `<exe>/../share/materializr/fonts/` candidate. ~1.4 MB.
RUN cp /src/assets/fonts/*.ttf /AppDir/usr/share/materializr/fonts/ 2>/dev/null || true

# Copy OCCT + TBB + Freetype shared libs (follow symlinks, any arch). OCCT now
# lives in /usr/local/lib (source build); freetype/fontconfig stay in /usr/lib.
RUN find /usr/lib /usr/local/lib -name "libTK*.so*" -o -name "libtbb*.so*" -o -name "libfreetype.so*" \
    | while read lib; do cp -L "$lib" /AppDir/usr/lib/ 2>/dev/null || true; done

# Bundle the binary's FULL shared-lib closure, minus the system layer that
# must come from the host (glibc, GL stack, X11/xcb, fontconfig, wayland).
# The hand-list above stopped sufficing when TKService arrived (Text tool's
# Font_BRepFont): it drags in FreeImage and its whole codec tree - jpeg,
# png, tiff, webp, OpenEXR, raw - which no hand-list should chase.
RUN ldd /AppDir/usr/bin/materializr | awk '/=> \//{print $3}' | sort -u \
    | grep -vE '/(libc|libm|libdl|libpthread|librt|libresolv|libgcc_s|libstdc\+\+|ld-linux|libGL|libGLX|libGLdispatch|libOpenGL|libEGL|libX11|libxcb|libXau|libXdmcp|libXext|libXrender|libXi|libXfixes|libXcursor|libXrandr|libXinerama|libXxf86vm|libfontconfig|libexpat|libdbus|libdrm|libwayland)[.-]' \
    | while read lib; do cp -L "$lib" /AppDir/usr/lib/ 2>/dev/null || true; done

# Set RPATH
RUN patchelf --set-rpath '$ORIGIN/../lib' /AppDir/usr/bin/materializr || true

# Create .desktop file. StartupWMClass must match the WM_CLASS / Wayland
# app-id the running window reports (set in Window.cpp via SDL)
# so taskbar extensions like Dash-to-Panel can tie the window to its icon.
RUN printf '[Desktop Entry]\nName=Materializr\nExec=materializr\nIcon=materializr\nType=Application\nCategories=Graphics;3DGraphics;Engineering;\nComment=Open-source parametric 3D CAD\nStartupWMClass=Materializr\n' \
    > /AppDir/materializr.desktop

# AppStream metainfo - lets Gear Lever / AppImagePool / software centres
# auto-populate the description, screenshot, links and release notes instead
# of falling back to just the .desktop Name/Comment.
RUN mkdir -p /AppDir/usr/share/metainfo \
    && cp /src/com.materializr.app.metainfo.xml \
        /AppDir/usr/share/metainfo/com.materializr.app.metainfo.xml

# Use the project's icon.png if present at the repo root, resized to the
# canonical 256x256 + 512x512 hicolor sizes so desktop environments pick
# them up cleanly. Falls back to a tiny generated SVG placeholder during
# early bring-up if no icon.png is committed yet. `-background none` keeps
# transparency intact; `-resize` preserves aspect and pads with transparent
# pixels so non-square sources land centred in a square frame.
RUN if [ -f /src/icon.png ]; then \
        convert /src/icon.png -background none -resize 256x256 \
            -gravity center -extent 256x256 /AppDir/materializr.png && \
        cp /AppDir/materializr.png \
            /AppDir/usr/share/icons/hicolor/256x256/apps/materializr.png && \
        convert /src/icon.png -background none -resize 512x512 \
            -gravity center -extent 512x512 \
            /AppDir/usr/share/icons/hicolor/512x512/apps/materializr.png ; \
    else \
        printf '<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256">\n<rect width="256" height="256" rx="32" fill="#2a2a3a"/>\n<text x="128" y="160" font-size="120" font-family="sans-serif" font-weight="bold" fill="#4a9eff" text-anchor="middle">C</text>\n</svg>\n' \
            > /AppDir/materializr.svg && \
        cp /AppDir/materializr.svg /AppDir/usr/share/icons/hicolor/256x256/apps/materializr.svg ; \
    fi

# Create AppRun script
RUN printf '#!/bin/bash\nHERE="$(dirname "$(readlink -f "$0")")"\nexport APPIMAGE_ORIG_LD_LIBRARY_PATH="$LD_LIBRARY_PATH"\nexport LD_LIBRARY_PATH="$HERE/usr/lib:$LD_LIBRARY_PATH"\nexec "$HERE/usr/bin/materializr" "$@"\n' \
    > /AppDir/AppRun \
    && chmod +x /AppDir/AppRun

# Build the AppImage (--appimage-extract-and-run avoids FUSE requirement inside Docker).
# -u embeds gh-releases-zsync update info AND emits a .zsync control file alongside
# the AppImage, so AppImageUpdate / Gear Lever can do delta auto-updates from the
# GitHub "latest" release. cd /output so the generated .zsync lands beside it.
RUN mkdir -p /output \
    && ARCH=$(uname -m) \
    && cd /output \
    && appimagetool --appimage-extract-and-run \
        -u "gh-releases-zsync|materializr-cad|materializr|latest|Materializr-*${ARCH}.AppImage.zsync" \
        /AppDir Materializr-${ARCH}.AppImage

# ─── Export stage ────────────────────────────────────────────────────────────

FROM scratch AS export
COPY --from=builder /output/*.AppImage /
COPY --from=builder /output/*.zsync /
