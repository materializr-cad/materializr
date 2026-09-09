# iPad port - implementation plan

Goal: Materializr running natively on iPad, reusing the entire `core/` +
`modeling/` geometry codebase the same way the Android port does - SDL2 +
OpenGL ES 3.0 + cross-compiled OpenCASCADE, with the existing runtime *touch
mode*. The Android port already solved every hard architectural problem; this
plan is mostly "do the Android port again with Apple toolchains," plus one
non-code workstream (licensing/distribution).

**Prerequisites:** a Mac with Xcode 15+ (the macOS release build already
requires one; CI has `macos.yml`), a physical iPad for testing, and an Apple
Developer account - the free tier suffices for on-device development builds
(7-day provisioning), $99/yr for TestFlight/App Store.

**Targets:** iPadOS 15+, arm64 only, iPad-only device family (the README
already says phones are cramped; that goes double under iOS review). Simulator
support (arm64 sim slice) is optional but worth having for fast iteration on
Apple Silicon Macs.

---

## Phase 0 - Decisions & licensing (start immediately, runs in parallel)

1. **Distribution decision.** GPLv3 conflicts with App Store terms (§6
   anti-tivoization, §10 no-further-restrictions) regardless of price. The
   options, cheapest first:
   - *Sideload-only* (personal Xcode builds, AltStore/SideStore, EU alt
     marketplaces): no license change needed. CI can publish an unsigned
     `.ipa` next to the APK.
   - *App Store / TestFlight*: add a **GPLv3 §7 "additional permission"**
     (App Store exception - Nextcloud's `COPYING.iOS` is a proven template).
     Requires written consent from all three copyright holders to date
     (stevebushwa, r4stl1n, TechHQUSA). Record consent in a tracked GitHub
     issue; add the exception text to the repo so future contributions
     inherit it.
2. **Do the §7 exception now even if shipping sideload-first** - the
   contributor list only grows.
3. Pick the bundle ID (`com.materializr.app` matches Android) and register it.

No code depends on this phase; Phases 1–4 proceed regardless.

## Phase 1 - Split the `__ANDROID__` guards by meaning (lands on `main`, no-op everywhere)

There are ~53 `#if defined(__ANDROID__)` sites in `src/`, but they mean three
different things. iOS needs two of the three, so introduce real macros first
(e.g. in a new `src/platform_defs.h`, included from `gl_common.h`):

```c
MZ_GLES    - "rendering on OpenGL ES 3.0"   (Android + iOS)
MZ_MOBILE  - "touch-first mobile platform"  (Android + iOS)
__ANDROID__ / MZ_IOS - genuinely OS-specific services
```

Reclassify each site:

- **`MZ_GLES`** (GL API differences - identical on iOS):
  - `gl_common.h` GLES3 include + `glShaderSourceAdapt` interception, and
    `gl_shader.cpp` (`#version 330 core` → `300 es` rewrite)
  - `Application.cpp:574` - ImGui init string `"#version 300 es"`
  - `Viewport.cpp:37` - no `GL_MULTISAMPLE` toggle on ES
  - `EdgeRenderer.cpp:198,220` - no `GL_POLYGON_OFFSET_LINE` on ES
  - `SketchRenderer.cpp:156,281` - no `glPointSize` on ES
  - `SelectionHighlight.cpp:114,384`
  - `ImageExport.cpp:106` - no `glGetTexImage`; FBO readback path
  - Android/iOS CMake: `IMGUI_IMPL_OPENGL_ES3` for `imgui_lib`

  ⚠️ **Trap:** `gl_common.h` tests `__APPLE__` to mean *macOS desktop GL*.
  On iOS `__APPLE__` is also defined. The GLES branch must come first and be
  keyed on `MZ_GLES` (defined for `__ANDROID__` or `TARGET_OS_IPHONE` via
  `<TargetConditionals.h>`), or an iOS build silently includes
  `<OpenGL/gl3.h>` and fails. Audit every `__APPLE__` guard in the tree for
  the same ambiguity (`Window.cpp` core-profile setup, CMake).

- **`MZ_MOBILE`** (touch/lifecycle behavior - wanted on iOS as-is):
  - `touch_mode.h` and `Settings.h:15,20` - touch-mode/trackpad defaults
  - `Window.cpp` - `SDL_HINT_TOUCH_MOUSE_EVENTS=0`, finger-event handling,
    long-press/box-select/synthetic right-click, ES context request
    (this one is both; the context bit is `MZ_GLES`)
  - `Window.cpp:122` - no desktop "start maximized" fallback
  - `Application.cpp:5101` - foreground/idle gating exemption
  - `Application.cpp:2834,2851,3379,...` - picker-named files, recents via
    platform URIs (see file API below)
  - `FileDialogs.{h,cpp}` - in-app ImGui browser + system-picker bridge

- **Stays Android-only:** `android_platform.cpp` (logcat pipe, asset
  extraction), `android_main.cpp` (fdsan workaround), `android_files.cpp`
  (JNI/SAF).

Also rename the `android_files.h` API to a platform-neutral mobile-files
interface (same signatures - `mobileStartOpenDocument`, `mobilePollFileResult`,
`mobileCommitSave`, `mobileShareFile`, `mobileLastDocUri/Name`, `mobileOpenUri`,
`mobileShow/HideTextInput`). The async poll-per-frame design maps 1:1 onto
iOS's `UIDocumentPickerViewController` delegate model, so **only the
implementation is per-OS, not the call sites**. Keep `android_files.cpp` as the
Android impl behind the new names.

**Exit criteria:** desktop (Linux/Windows/macOS) and Android builds are
bit-for-bit behaviorally unchanged; all four CI workflows green. This phase is
pure refactor and can merge long before any iOS code exists.

## Phase 2 - Cross-build dependencies for iOS (`ios/scripts/setup-deps.sh`)

Mirror `android/scripts/setup-deps.sh` (same pinned versions + SHA-256
verification):

- **Toolchain:** no NDK-style toolchain file needed - CMake supports iOS
  natively:
  `-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_OSX_SYSROOT=iphoneos`.
  Prefix at e.g. `~/iOS/prefix/iphoneos-arm64/` (and optionally a second
  `iphonesimulator-arm64` prefix for the simulator).
- **FreeType 2.13.3** - static, as on Android.
- **OpenCASCADE 7.9.3** - OCCT officially supports iOS as a target. Key
  difference from Android: **`-DBUILD_LIBRARY_TYPE=Static`**. Static is the
  natural iOS packaging (no 40-dylib framework embedding, no `DT_NEEDED`
  ordering games) and sidesteps the empty-`Standard_EXPORT` inline-symbol
  problem documented in the Android script. Same module flags:
  `USE_TK/TCL/FREEIMAGE/TBB/VTK/RAPIDJSON/OPENVR/DRACO/FFMPEG=OFF`,
  `BUILD_MODULE_Draw=OFF`, FreeType wired to the static lib. Keep the
  "do NOT bump to 8.0.x" note in force until re-evaluated for static linking
  (the failure mode was shared-lib-specific, but verify before diverging
  versions across platforms).
- **SDL2 2.30.9** - build as a static library with the same CMake invocation
  (SDL2's CMake handles iOS; it pulls in the UIKit/Metal/EAGL frameworks via
  usage requirements).

Whole-set link on iOS: the app links `libTK*.a` inside
`-Wl,-force_load` is *not* needed (unlike `--no-as-needed`); a normal static
link resolves OCCT's cyclic deps if the toolkits are listed once - Apple's
`ld` re-scans archives. Expect a large final binary (~60–120 MB before
dead-strip); enable `-dead_strip` and LTO in Release.

**Exit criteria:** prefix directory with static OCCT + FreeType + SDL2 for
`iphoneos-arm64`; a trivial `main()` linking `TKernel` runs on device.

## Phase 3 - App shell + iOS glue (`ios/` + ~4 small files in `src/`)

**Project structure** - follow the Android layout:

```
ios/
  CMakeLists.txt          # app bundle target, -GXcode
  scripts/setup-deps.sh   # Phase 2
  Info.plist.in
  LaunchScreen.storyboard # required for full-screen; trivial
  Assets.xcassets/        # icon (reuse icon.png pipeline)
```

Use CMake's Xcode generator to produce the `.app` directly
(`MACOSX_BUNDLE`, `XCODE_ATTRIBUTE_CODE_SIGN_*`, `XCODE_EMBED_FRAMEWORKS` not
needed with static libs). Source list = the same glob as
`android/app/jni/src/CMakeLists.txt` (exclude `main.cpp`, `UpdateChecker.cpp`,
`android_*.cpp`; include the new `ios_*.mm`). ImGui + GLM via FetchContent as
on Android, `IMGUI_IMPL_OPENGL_ES3` defined, `OCC_CONVERT_SIGNALS` defined
(iOS is Unix; the `OSD::SetSignal` path works as on Android/Linux).

**New source files** (Objective-C++ where UIKit is touched):

1. `src/ios_main.cpp` - `SDL_main` entry, mirroring `android_main.cpp` minus
   the fdsan workaround (Android-only) and with `iosInitRuntime()` instead.
   (Alternatively fold both into one `mobile_main.cpp` with two tiny
   platform hooks - reviewer's choice.)
2. `src/ios_platform.mm` (~60 lines, *simpler* than Android's 144):
   - **No asset extraction step.** The `.app` bundle is a real directory -
     `chdir()` into `[NSBundle mainBundle].resourcePath` so the existing
     cwd-relative `assets/fonts/<name>` lookup resolves, and point every
     `CSF_*` env var straight at `<bundle>/occt-resources/...`. The Android
     manifest/extract machinery (needed because APK assets aren't files)
     disappears entirely.
   - `HOME` is already set to the sandbox container on iOS, so
     `SettingsIO::defaultPath()`'s `$HOME/.config/materializr` works
     unmodified - but the settings then live in the container root, which
     iCloud-backs-up by default. Acceptable for v1; note it.
   - stdout/stderr: no logcat equivalent needed - Xcode and Console.app
     capture them. Optionally mirror to `os_log` later.
3. `src/ios_files.mm` (~200 lines) - implement the mobile-files API:
   - Open/Save: `UIDocumentPickerViewController`
     (`forOpeningContentTypes:` / `forExportingURLs:`), presented from
     SDL's root view controller
     (`SDL_GetWindowWMInfo` → `info.uikit.window.rootViewController`).
     Delegate stores the result; `mobilePollFileResult()` reads it - same
     copy-to-temp contract as Android.
   - Recents: security-scoped bookmarks (`NSURL bookmarkData…`) are the iOS
     analog of SAF persistable URIs; `mobileOpenUri()` resolves a bookmark
     (base64 string in the recents list), `startAccessingSecurityScopedResource`,
     copy to temp, stop access.
   - Share: `UIActivityViewController` (needs the popover anchor set on
     iPad or UIKit throws).
   - Keyboard: try plain `SDL_StartTextInput()` first - SDL's iOS backend
     raises the system keyboard itself, so the Android IME workaround is
     likely unnecessary; keep the hooks as no-ops if so.
4. `Info.plist` keys:
   - `UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace` - projects
     and exports visible in the Files app *even before* the picker code
     works (day-one escape hatch).
   - Exported UTI for `.materializr` + document-type declarations for
     STL/STEP/SVG import (enables "Open in Materializr" from Files/Mail).
   - `UIDeviceFamily = 2` (iPad-only), all four orientations,
     `UIRequiresFullScreen = YES` for v1 (defer Split View/Stage Manager
     resize handling).

**Rendering/window specifics to set in the `MZ_MOBILE`/`MZ_GLES` paths:**
`SDL_WINDOW_ALLOW_HIGHDPI` + drive `ui_scale` from
`SDL_GL_GetDrawableSize / SDL_GetWindowSize` ratio (verify against however the
Android build derives density); ES 3.0 context request is already the shared
mobile branch. MSAA via multisampled renderbuffer works on iOS GLES the same
as Android (`Viewport` already handles the ES path).

**Exit criteria:** app launches on a physical iPad, splash → full UI, can
sketch/extrude/fillet, and a saved project is visible in the Files app.

## Phase 4 - On-device bring-up and iPad polish

Bring-up checklist (in rough order of expected friction):

- [ ] Touch gestures end-to-end: tap-select, one-finger orbit, two-finger
      pan/pinch, long-press context menu, press-drag-release drawing -
      should "just work" via the shared `Window.cpp` finger path, but this
      is the main verification surface.
- [ ] **Background/foreground lifecycle - the one genuinely new problem.**
      iOS *terminates* apps that touch the GL context while backgrounded.
      Handle `SDL_APP_WILLENTERBACKGROUND` / `SDL_APP_DIDENTERFOREGROUND`
      (SDL delivers these via an event filter *during* the callback - they
      cannot be polled later) and hard-stop rendering. Also autosave the
      project on `WILLENTERBACKGROUND`: iOS may silently kill the app any
      time after. The `Application.cpp:5101` foreground-gate exemption gets
      an iOS-correct implementation here rather than the Android
      "always foreground" shortcut.
- [ ] Memory: OCCT + big STEP imports under iPadOS jetsam limits. Respond to
      `SDL_APP_LOWMEMORY` (drop undo history / cached tessellations if
      needed). Test on a base-model iPad, not just an M-series Pro.
- [ ] Safe areas: inset ImGui's root layout from the rounded corners /
      home-indicator (SDL2: `SDL_GetDisplayUsableBounds`; or read
      `safeAreaInsets` in `ios_platform.mm` and expose it).
- [ ] Apple Pencil arrives as ordinary touch - verify pressure doesn't
      confuse the finger path. Hardware keyboard/trackpad: the saved
      `touchMode=false` setting already covers this; verify SDL delivers
      mouse events on iPadOS.
- [ ] Performance pass with per-frame profiling on device (A-series GPU vs
      the Android GLES path - expect fine, verify edge/AA rendering).
- [ ] Screenshot/image export (`ImageExport.cpp` ES readback path) and
      share-sheet export of STL/STEP/SVG.

## Phase 5 - CI, packaging, distribution

- **CI (`.github/workflows/ios.yml`)**: macOS runner; cache the Phase-2
  prefix keyed on `setup-deps.sh` hash (the OCCT cross-build is ~40 min);
  build the Xcode project with `CODE_SIGNING_ALLOWED=NO`; zip the `.app`
  into an unsigned `.ipa` artifact. That artifact is directly usable by
  AltStore/SideStore users - publish it in releases alongside the APK from
  day one, license-clean.
- **Fastlane**: the repo already has `fastlane/` for Android metadata; add an
  iOS lane later if/when App Store distribution happens (screenshots,
  TestFlight upload).
- **Distribution ladder** (each step gated on Phase 0):
  1. Unsigned `.ipa` in GitHub releases (sideload) - no blockers.
  2. TestFlight - needs paid account + §7 exception advisable (same terms
     conflict as the store).
  3. App Store - needs the signed-off §7 exception, privacy "nutrition
     label" (easy: the app is offline; update check is already stubbed on
     mobile and should stay disabled - the store handles updates),
     App Review pass.
  4. EU alternative marketplaces - fallback if App Store review or
     licensing stalls.

---

## Risks / open questions

| Risk | Assessment | Mitigation |
|---|---|---|
| OpenGL ES deprecated on iOS since iOS 12 | Still ships and works on all current iPadOS versions; same posture as GL-on-macOS already accepted in `gl_common.h` | If Apple ever removes it: ANGLE (GLES-on-Metal) is a drop-in static lib - bounded, known fallback, not a rewrite |
| OCCT 7.9.3 static build for iOS has an unknown quirk | OCCT officially supports iOS; static sidesteps the known Android shared-lib symbol issue | Budget slack in Phase 2; the Android script's patch-note history shows this is where surprises live |
| GPLv3 vs App Store | Certain conflict for store distribution | §7 exception - only 3 copyright holders today; do it now |
| iOS background-GL termination | Certain, must be handled | Phase 4 lifecycle work; autosave on background |
| Jetsam memory kills on big models | Possible on low-RAM iPads | `SDL_APP_LOWMEMORY` handling; document limits |
| `__APPLE__` guards silently meaning "macOS" | Certain if unaudited | Phase 1 audit is explicitly scoped for it |
| SDL2 keyboard/picker view-controller integration | Low - SDL iOS backend is mature | Fallbacks documented in Phase 3 |

## Effort estimate (single developer familiar with the codebase)

| Phase | Effort |
|---|---|
| 0 - Licensing/consents | days of calendar time, hours of work |
| 1 - Guard refactor | 1–2 days (mechanical, CI-verified) |
| 2 - Dependency cross-build | 2–4 days |
| 3 - App shell + glue | 3–5 days |
| 4 - Bring-up + polish | 1–2 weeks (iterative, device in hand) |
| 5 - CI + sideload release | 2–3 days |

**Total: roughly 3–4 working weeks to a sideloadable release**, with App
Store submission as a separate, mostly non-engineering tail. Phases 1 and 0
are pure wins that can start today and carry zero risk to existing platforms.
