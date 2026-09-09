# Materializr - iOS (iPad) port

Builds the Materializr CAD app for **iPadOS 15+ (arm64)**, reusing the entire
`core/` + `modeling/` geometry codebase unchanged - the same architecture as
the Android port: SDL2 + OpenGL ES 3.0 + cross-compiled OpenCASCADE, with the
runtime *touch mode*.

> **Status: scaffolding, not yet compiled on a Mac.** The shared-code refactor
> (`MZ_GLES` / `MZ_MOBILE` guards) is build-verified on Linux desktop; the
> files in this directory are written to spec but need their first build on
> macOS. Expect small fix-ups - the "First-build checklist" below lists the
> spots most likely to need attention.

## How it maps onto the Android port

| Concern | Android | iOS | Where |
|---|---|---|---|
| GL API deltas | `MZ_GLES` | same code | `src/platform_defs.h`, `src/gl_common.h` |
| Touch UI / pickers | `MZ_MOBILE` | same code | `src/touch_mode.h`, `src/io/FileDialogs.cpp` |
| Entry point | `android_main.cpp` | `ios_main.cpp` (SDL_main) | `src/` |
| Runtime setup | extract APK assets | none - chdir into the bundle | `src/ios_platform.mm` |
| System pickers | SAF via JNI | `UIDocumentPickerViewController` | `src/ios_files.mm` |
| Recents refs | persistable content:// URIs | security-scoped bookmarks | `src/ios_files.mm` |
| OCCT / deps | shared `.so`, NDK | **static** `.a`, Xcode | `ios/scripts/setup-deps.sh` |
| Background GL | activity pause handles it | hard gate on `SDL_APP_WILLENTERBACKGROUND` | `src/ios_platform.mm` + `Application.cpp` |

## Prerequisites

- macOS with **Xcode 15+** (`xcode-select --install` done, an iOS SDK present)
- **CMake 3.24+** (`brew install cmake`)
- An iPad running iPadOS 15+ and an Apple ID (free tier works for on-device
  dev builds - 7-day provisioning; paid for TestFlight)

## Build

```bash
# 1. Cross-build FreeType + OpenCASCADE + SDL2 as static libs (~30-60 min).
#    Same pinned versions/SHA-256s as the Android build.
cd ios && ./scripts/setup-deps.sh          # -> ~/iOS/prefix/iphoneos-arm64

# 2. Generate the Xcode project (from the repo root).
cmake -S ios -B build-ios -GXcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DMATERIALIZR_IOS_PREFIX=$HOME/iOS/prefix/iphoneos-arm64 \
      -DMATERIALIZR_VERSION_NAME=1.3.0-beta.11 \
      -DMZ_DEVELOPMENT_TEAM=<YOUR_TEAM_ID>       # omit to pick the team in Xcode

# 3. Open, select your iPad as the destination, Run.
open build-ios/MaterializriOS.xcodeproj
```

Free-tier signing note: `com.materializr.cad` must be unique per team - if
signing fails, change `XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER` in
`ios/CMakeLists.txt` (or the Xcode Signing pane) to e.g.
`com.yourname.materializr`.

Simulator (arm64, Apple Silicon Macs): re-run `setup-deps.sh` with
`SIMULATOR=1` (separate prefix), then configure with
`-DCMAKE_OSX_SYSROOT=iphonesimulator` and the simulator prefix.

## First-build checklist (likely fix-up spots)

1. **Bundle resource layout** - after the first build, verify:
   `ls build-ios/Debug-iphoneos/materializr.app/assets/fonts/` and
   `.../materializr.app/occt-resources/StdResource/` exist. If Xcode nested
   them under `Resources/` instead (CMake `MACOSX_PACKAGE_LOCATION` mapping),
   adjust the `chdir`/`CSF_*` paths in `src/ios_platform.mm` accordingly.
   Startup logs the resolved paths to the Xcode console.
2. **SDL2 static target name** - `CMakeLists.txt` prefers `SDL2::SDL2-static`
   and falls back to `SDL2::SDL2`. If configure fails at `find_package(SDL2)`,
   check `~/iOS/prefix/iphoneos-arm64/lib/cmake/SDL2/` for the exported names.
3. **OCCT static link** - if the app fails to link with missing `Standard_*`
   or `TK*` symbols, the archive glob may need `-Wl,-force_load` on specific
   toolkits, or additional system libs. Report the first ~20 error lines.
4. **Settings persistence** - settings go to `$HOME/.config/materializr`
   (container root). If the startup log shows "could not create
   ~/.config/materializr", switch `iosInitRuntime()` to set
   `HOME=<container>/Library` instead (comment in `ios_platform.mm`).
5. **OpenGL ES deprecation warnings** are expected and silenced where we
   control the flags; ignore any that leak through ImGui's backend.

## Known v1 limitations (by design, revisit in bring-up)

- **Save flow is export-shaped**: iOS picks the destination *after* the file
  is written, so a save reports success before the export sheet confirms, and
  a cancelled export sheet loses that save silently (the app still holds the
  document - just Save again). Fixing this properly needs an app-side async
  save result; tracked as TODO(phase-4) in `src/ios_files.mm`.
- **Recents after "Save As"** record a temp path, not the final destination
  (self-heals: Open Recent drops dead entries on first use). Documents opened
  via the picker record correct security-scoped bookmarks.
- **No app icon yet** - asset catalog to be added.
- Split View / Stage Manager is disabled (`UIRequiresFullScreen`); Apple
  Pencil arrives as plain touch; autosave-on-background not yet wired (iOS
  may kill the suspended app; save before switching apps).

## Distribution reminder

GPLv3 conflicts with App Store terms - see `docs/ipad-port-plan.md` (Phase 0).
Personal Xcode builds and sideloading are fine today; TestFlight/App Store
need the §7 additional-permission exception signed off by all copyright
holders first.
