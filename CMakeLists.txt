cmake_minimum_required(VERSION 3.20)
project(Materializr VERSION 1.6.3 LANGUAGES CXX)

# Pre-release suffix appended to the reported version (e.g. "-beta.1"). Empty on
# stable releases. Kept here next to project() so a beta build self-identifies
# as e.g. "1.3.0-beta.1" — that way the in-app update check (which is semver
# pre-release aware) doesn't nag a beta tester to "update" to the build they're
# already running, and hands off cleanly to the final 1.3.0.
set(MATERIALIZR_VERSION_SUFFIX "")

# macOS: deliberately do NOT pin CMAKE_OSX_DEPLOYMENT_TARGET. The bundled Homebrew
# OpenCASCADE/SDL2 dylibs are built for the build host's macOS, so they — not our
# compile flag — set the real floor of a packaged .dmg. Letting the binary inherit
# the host target keeps it consistent with those dylibs (no "built for newer
# version" linker warnings), and packaging/macos/build-dmg.sh derives the true
# LSMinimumSystemVersion from the actual binaries so the plist can't lie. (Override
# with -DCMAKE_OSX_DEPLOYMENT_TARGET=… only alongside dylibs built for that target.)

# macOS: deliberately do NOT pin CMAKE_OSX_DEPLOYMENT_TARGET. The bundled Homebrew
# OpenCASCADE/SDL2 dylibs are built for the build host's macOS, so they — not our
# compile flag — set the real floor of a packaged .dmg. Letting the binary inherit
# the host target keeps it consistent with those dylibs (no "built for newer
# version" linker warnings), and packaging/macos/build-dmg.sh derives the true
# LSMinimumSystemVersion from the actual binaries so the plist can't lie. (Override
# with -DCMAKE_OSX_DEPLOYMENT_TARGET=… only alongside dylibs built for that target.)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── FetchContent dependencies ───────────────────────────────────────────────

include(FetchContent)

# SDL2 — the windowing/input backend on every platform (the Android port
# unified desktop + touch on SDL2; src/app/Window.cpp is SDL-based, replacing
# the old GLFW backend). Prefer the system/vcpkg package; fall back to building
# 2.30.9 from source — the same version android/scripts/setup-deps.sh pins —
# which on Linux needs the X11 dev headers already required for the GL stack.
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "System SDL2 not found - fetching and building 2.30.9 from source")
    FetchContent_Declare(
        sdl2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.30.9
    )
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl2)
endif()
# Normalise the target name across find_package configs and source builds.
if(TARGET SDL2::SDL2)
    set(MZ_SDL2_TARGET SDL2::SDL2)
elseif(TARGET SDL2)
    set(MZ_SDL2_TARGET SDL2)
else()
    message(FATAL_ERROR "SDL2 target not found after find_package/FetchContent")
endif()

# GLM
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# ImGui (docking), pinned to a RELEASE TAG.
#
# GIT_TAG was `docking` — a BRANCH HEAD, so every configure resolved to whatever
# that branch pointed at that day. ImGui is the entire UI and input layer,
# compiled straight into the shipped binary, so an upstream force-push or a
# merged malicious PR would land in the next release with no review step; and
# two builds of the same Materializr tag could ship different ImGui, which makes
# a release impossible to reproduce.
#
# It must be a TAG, not a branch commit. `docking` is rebased onto master
# regularly, and every rebase orphans the commits that were on it: the SHA this
# was first pinned to (81cfcb8c1) was unreachable in a fresh clone within hours
# —
#     $ git clone https://github.com/ocornut/imgui.git
#     $ git cat-file -t 81cfcb8c1fed1e4dac8bcdc059ce811c17b6194c
#     fatal: git cat-file: could not get object info
#
# — which made the whole project unbuildable from scratch. (GitHub's API still
# serves orphaned commits, so a SHA that looks alive there can already be dead
# to `git clone`. Check with a clone, not the API.) The same rebase is why the
# ImGui our own build machine had cached, d460434, no longer exists either.
#
# Tags are immutable, so this is stable ground. Bump it deliberately; keep it in
# step with android/app/jni/src/CMakeLists.txt.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.8-docking # b61e56346a92cfcaf1f43a545ca37b0b32239654
)
FetchContent_MakeAvailable(imgui)

# ─── ImGui static library ────────────────────────────────────────────────────

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC ${MZ_SDL2_TARGET})

# ─── Find system packages ────────────────────────────────────────────────────

# Link the legacy libGL.so.1 entry point rather than GLVND's libOpenGL.so.0 +
# libGLX.so.0. Both work when building and running on the same box, but the
# AppImage deliberately does NOT bundle the GL stack (it must come from the
# host driver - see the exclude list in the Dockerfile), and libOpenGL.so.0
# ships in a separate package (libopengl0 on Debian/Ubuntu) that a stock
# desktop install does not pull in. Linking GLVND therefore produced an
# AppImage that died at startup with
#     error while loading shared libraries: libOpenGL.so.0
# on any clean Ubuntu. libGL.so.1 is present wherever working drivers are, and
# is what SDL2 itself dlopens, so LEGACY is the portable choice here.
set(OpenGL_GL_PREFERENCE LEGACY)
find_package(OpenGL REQUIRED)

# OpenCASCADE
if(WIN32 OR APPLE)
    # Windows (vcpkg) and macOS (Homebrew: `brew install opencascade`) both ship a
    # clean OpenCASCADE CMake config that exports the TK* targets and sets
    # OpenCASCADE_INCLUDE_DIR directly — no path patching needed (unlike the Debian
    # packages handled in the else branch). The Homebrew config lives under the brew
    # prefix; pass -DCMAKE_PREFIX_PATH="$(brew --prefix)" if CMake doesn't find it.
    find_package(OpenCASCADE CONFIG REQUIRED)
elseif(DEFINED MZR_OCCT_PREFIX)
# Linux, source-built OCCT at a custom prefix (the release AppImage path — the
# Dockerfile compiles OCCT 7.9.3 from source so every platform runs the same
# kernel version, rather than whatever the distro's apt ships). A from-source
# build exports a clean CMake config with correct paths and no broken libtbb
# symlinks, so it uses the same clean find_package the Windows/macOS branch
# does — none of the Debian path-patching below applies.
find_package(OpenCASCADE CONFIG REQUIRED
    PATHS "${MZR_OCCT_PREFIX}/lib/cmake/opencascade" NO_DEFAULT_PATH)
else()
# Linux (distro apt OCCT — local dev convenience): work around missing
# libtbb-dev symlinks by patching cmake files locally.
# Auto-detect the OCCT cmake directory (differs between aarch64/x86_64).
file(GLOB_RECURSE _occt_config_search "/usr/lib/*/cmake/opencascade/OpenCASCADEConfig.cmake")
if(_occt_config_search)
    list(GET _occt_config_search 0 _occt_config_file)
    get_filename_component(OCCT_CMAKE_SRC "${_occt_config_file}" DIRECTORY)
else()
    # Fallback: try standard multiarch path
    execute_process(COMMAND dpkg-architecture -qDEB_HOST_MULTIARCH
                    OUTPUT_VARIABLE _multiarch OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(OCCT_CMAKE_SRC "/usr/lib/${_multiarch}/cmake/opencascade")
endif()
message(STATUS "OCCT cmake source: ${OCCT_CMAKE_SRC}")

# Detect the lib directory for patching .so paths
get_filename_component(_occt_lib_dir "${OCCT_CMAKE_SRC}/../.." ABSOLUTE)
set(OCCT_CMAKE_LOCAL "${CMAKE_BINARY_DIR}/cmake_occt_patched")
file(MAKE_DIRECTORY ${OCCT_CMAKE_LOCAL})

file(GLOB OCCT_CMAKE_FILES "${OCCT_CMAKE_SRC}/*.cmake")
foreach(_file ${OCCT_CMAKE_FILES})
    get_filename_component(_fname ${_file} NAME)
    file(READ ${_file} _content)
    # Replace broken .so symlinks with versioned .so files that actually exist
    # Handle both /path/libtbb.so" and /path/libtbb.so; patterns
    string(REGEX REPLACE "/usr/lib/[^/]+/libtbb\\.so([;\"])" "${_occt_lib_dir}/libtbb.so.12\\1" _content "${_content}")
    string(REGEX REPLACE "/usr/lib/[^/]+/libtbbmalloc\\.so([;\"])" "${_occt_lib_dir}/libtbbmalloc.so.2\\1" _content "${_content}")
    string(REGEX REPLACE "/usr/lib/[^/]+/libfreetype\\.so([;\"])" "${_occt_lib_dir}/libfreetype.so.6\\1" _content "${_content}")
    # Fix install prefix in Config: replace the two get_filename_component calls
    string(REGEX REPLACE
        "get_filename_component \\(OpenCASCADE_INSTALL_PREFIX \"\\$\\{CMAKE_CURRENT_LIST_FILE\\}\" PATH\\)[^\n]*\nget_filename_component \\(OpenCASCADE_INSTALL_PREFIX \"\\$\\{OpenCASCADE_INSTALL_PREFIX\\}\" PATH\\)"
        "set(OpenCASCADE_INSTALL_PREFIX \"/usr\")"
        _content "${_content}")
    # Fix _IMPORT_PREFIX in Targets files: replace the prefix computation block
    # that goes up 4 levels from the cmake file location with a hardcoded /usr
    string(REGEX REPLACE
        "get_filename_component\\(_IMPORT_PREFIX \"\\$\\{CMAKE_CURRENT_LIST_FILE\\}\" PATH\\)"
        "set(_IMPORT_PREFIX \"/usr\")\nif(FALSE)  # patched out"
        _content "${_content}")
    string(REGEX REPLACE
        "if\\(_IMPORT_PREFIX STREQUAL \"/\"\\)\n  set\\(_IMPORT_PREFIX \"\"\\)\nendif\\(\\)"
        "endif()  # end patched-out block"
        _content "${_content}")
    file(WRITE "${OCCT_CMAKE_LOCAL}/${_fname}" "${_content}")
endforeach()

find_package(OpenCASCADE REQUIRED
    PATHS ${OCCT_CMAKE_LOCAL}
    NO_DEFAULT_PATH
)
endif()

# ─── Main executable ─────────────────────────────────────────────────────────

add_executable(materializr
    $<$<PLATFORM_ID:Windows>:packaging/windows/app.rc>
    src/main.cpp
    src/app/Application.cpp
    src/app/ui_layout_bridge.cpp
    src/i18n.cpp
    src/app/InteractiveOpController.cpp
    src/app/FaceOpControllers.cpp
    src/app/ExtrudeController.cpp
    src/app/PushPullController.cpp
    src/app/LiveOpPreview.cpp
    src/app/HistoryEditPreview.cpp
    src/app/EdgeOpController.cpp
    src/app/SplitController.cpp
    src/app/Application_Dialogs.cpp
    src/app/Application_InteractiveOps.cpp
    src/app/layout/LayoutCommon.cpp
    src/app/layout/classic/ClassicLayout.cpp
    src/app/layout/modern/ModernLayout.cpp
    src/app/layout/imtouch/ImTouchLayout.cpp
    src/app/Application_Viewport.cpp
    src/app/Window.cpp
    src/plugin/PluginContext.cpp
    src/plugin/InteractiveOp.cpp
    src/app/CylindricalPick.cpp
    src/plugin/PluginRegistry.cpp
    src/plugins/ForceLink.cpp
    src/plugins/CoreCommandsPlugin.cpp
    src/plugins/StepIOPlugin.cpp
    src/plugins/IgesIOPlugin.cpp
    src/plugins/StlImportPlugin.cpp
    src/plugins/StlExportPlugin.cpp
    src/plugins/GltfExportPlugin.cpp
    src/plugins/BrepIOPlugin.cpp
    src/plugins/ObjExportPlugin.cpp
    src/plugins/ThreeMfExportPlugin.cpp
    src/plugins/DxfImportPlugin.cpp
    src/plugins/BooleanPlugin.cpp
    src/plugins/DeletePlugin.cpp
    src/plugins/TransformPlugin.cpp
    src/plugins/MirrorPlugin.cpp
    src/plugins/CopyPlugin.cpp
    src/plugins/PatternPlugin.cpp
    src/plugins/ShellPlugin.cpp
    src/plugins/ConstructionPlanePlugin.cpp
    src/plugins/RefImagePlugin.cpp
    src/plugins/BoundaryFillPlugin.cpp
    src/plugins/PatchPlugin.cpp
    src/plugins/ConstructionAxisPlugin.cpp
    src/plugins/RevolvePlugin.cpp
    src/plugins/FilletPlugin.cpp
    src/plugins/ChamferPlugin.cpp
    src/plugins/ExtrudePlugin.cpp
    src/plugins/PushPullPlugin.cpp
    src/plugins/SketchPlugin.cpp
    src/plugins/GizmoDragPlugin.cpp
    src/plugins/LoftPlugin.cpp
    src/plugins/SvgImportPlugin.cpp
    src/plugins/PrimitivesPlugin.cpp
    src/plugins/TutorialPlugin.cpp
    src/core/Document.cpp
    src/core/History.cpp
    src/core/SelectionManager.cpp
    src/core/MeshGuard.cpp
    src/core/VariableManager.cpp
    src/core/Material.cpp
    src/core/Verbose.cpp
    src/core/UiKeepAlive.cpp
    src/core/ThrowTrace.cpp
    src/core/VersionManager.cpp
    src/modeling/ExtrudeOp.cpp
    src/modeling/PushPullOp.cpp
    src/modeling/MoveFaceOp.cpp
    src/modeling/CombineSketchesOp.cpp
    src/modeling/DuplicateSketchOp.cpp
    src/modeling/MoveHoleOp.cpp
    src/modeling/ReplayOp.cpp
    src/modeling/OperationFactory.cpp
    src/modeling/SubShapeIndex.cpp
    src/modeling/EdgeAnchor.cpp
    src/modeling/FaceAnchor.cpp
    src/modeling/TopoName.cpp
    src/modeling/GenerationLedger.cpp
    src/modeling/FaceLineage.cpp
    src/modeling/ThreadOp.cpp
    src/modeling/FilletOp.cpp
    src/modeling/ChamferOp.cpp
    src/modeling/BlendCut.cpp
    src/modeling/ResizeCylindricalOp.cpp
    src/modeling/BooleanOp.cpp
    src/modeling/TransformOp.cpp
    src/modeling/BatchTransformOp.cpp
    src/modeling/ShellOp.cpp
    src/modeling/TaperOp.cpp
    src/modeling/ScaleFaceOp.cpp
    src/modeling/DefeatureOp.cpp
    src/modeling/MergeFacesOp.cpp
    src/modeling/ProjectSketchOp.cpp
    src/modeling/SplitBodyOp.cpp
    src/modeling/SeparateBodyOp.cpp
    src/modeling/MirrorOp.cpp
    src/modeling/PatternOp.cpp
    src/modeling/RevolveOp.cpp
    src/modeling/SweepOp.cpp
    src/modeling/LoftOp.cpp
    src/modeling/GuidedLoftOp.cpp
    src/modeling/BoundaryFillOp.cpp
    src/modeling/PatchOp.cpp
    src/modeling/SketchTransformOp.cpp
    src/modeling/ConstructionPlaneOp.cpp
    src/modeling/ConstructionAxisOp.cpp
    src/modeling/PlaneTransformOp.cpp
    src/modeling/AxisTransformOp.cpp
    src/modeling/AlignOp.cpp
    src/modeling/CopyOp.cpp
    src/modeling/DeleteOp.cpp
    src/modeling/PrimitiveOp.cpp
    src/modeling/TextSketchOp.cpp
    src/modeling/SvgImport.cpp
    src/modeling/Sketch.cpp
    src/modeling/Unfold.cpp
    src/modeling/SketchSolver.cpp
    src/modeling/AirfoilImport.cpp
    src/modeling/SketchTool.cpp
    src/modeling/SketchOffset.cpp
    src/modeling/SketchEditOp.cpp
    src/viewport/Viewport.cpp
    src/viewport/Camera.cpp
    src/viewport/Grid.cpp
    src/viewport/ShapeRenderer.cpp
    src/viewport/SketchRenderer.cpp
    src/viewport/ViewCube.cpp
    src/viewport/Picker.cpp
    src/viewport/EdgeRenderer.cpp
    src/viewport/PlaneRenderer.cpp
    src/viewport/RefImageRenderer.cpp
    src/viewport/AxisRenderer.cpp
    src/viewport/BackgroundRenderer.cpp
    src/viewport/Gizmo.cpp
    src/viewport/BoxSelect.cpp
    src/viewport/SectionView.cpp
    src/viewport/SectionCap.cpp
    src/viewport/SelectionHighlight.cpp
    src/ui/Toolbar.cpp
    src/ui/TouchWidgets.cpp
    src/ui/HistoryPanel.cpp
    src/ui/ItemsPanel.cpp
    src/ui/StatusBar.cpp
    src/ui/ThemeManager.cpp
    src/ui/PropertiesPanel.cpp
    src/ui/MeasureTool.cpp
    src/ui/VariablePanel.cpp
    src/ui/MaterialPanel.cpp
    src/ui/DrawingView.cpp
    src/ui/VersionPanel.cpp
    src/ui/SectionPanel.cpp
    src/ui/ToastNotification.cpp
    src/ui/ShortcutsPanel.cpp
    src/ui/AboutDialog.cpp
    src/ui/WelcomeScreen.cpp
    src/ui/LandingPage.cpp
    src/app/ProjectSession.cpp
    src/ui/UpdateChecker.cpp
    src/ui/HelpPanel.cpp
    src/io/ProjectIO.cpp
    src/io/ImageDecode.cpp
    src/io/ImageEncode.cpp
    src/io/SketchRecovery.cpp
    src/io/ProjectRecovery.cpp
    src/io/Settings.cpp
    src/io/StepIO.cpp
    src/io/StlIO.cpp
    src/io/MeshDecimate.cpp
    src/io/StlExport.cpp
    src/io/SvgExport.cpp
    src/io/IgesIO.cpp
    src/io/BrepIO.cpp
    src/io/ObjExport.cpp
    src/io/DxfExport.cpp
    src/io/DxfImport.cpp
    src/io/ThreeMfExport.cpp
    src/io/GltfExport.cpp
    src/io/FileDialogs.cpp
)

target_include_directories(materializr PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
# Mark OpenCASCADE as a SYSTEM include so its own deprecated declarations
# (e.g. std::iterator under C++17) don't spam our build with warnings.
target_include_directories(materializr SYSTEM PRIVATE
    ${OpenCASCADE_INCLUDE_DIR}
)

target_link_libraries(materializr PRIVATE
    imgui_lib
    ${MZ_SDL2_TARGET}
    OpenGL::GL
    glm::glm
)

if(WIN32)
    # vcpkg ships OpenCASCADE 7.8+, which renamed several data-exchange toolkits
    # (TKSTEP -> TKDESTEP, etc.). Link via the umbrella variable from OCCT's
    # CMake config so we don't have to track its toolkit reshuffles by hand.
    message(STATUS "OpenCASCADE libraries: ${OpenCASCADE_LIBRARIES}")
    target_link_libraries(materializr PRIVATE ${OpenCASCADE_LIBRARIES})
else()
    # Linux (system OCCT) and macOS (Homebrew) link the explicit set of toolkits
    # the app actually uses. This is deliberately NOT OCCT's umbrella
    # ${OpenCASCADE_LIBRARIES}: that pulls in the Draw/test harnesses (TKDraw,
    # TKViewerTest, TKQADraw, ...), which the app never calls and which bloat the
    # bundled macOS .dmg with ~dozens of extra dylibs. OCCT 7.8 renamed the
    # data-exchange toolkits (TKSTEP -> TKDESTEP, etc.); pick names by version so
    # we link on both pre- and post-refactor OCCT (distros + Homebrew ship 7.9+).
    if(OpenCASCADE_VERSION VERSION_GREATER_EQUAL 7.8)
        set(OCCT_DATAEXCHANGE_LIBS TKDESTEP TKDEIGES TKDESTL)
    else()
        set(OCCT_DATAEXCHANGE_LIBS TKSTEP TKIGES TKSTL)
    endif()
    target_link_libraries(materializr PRIVATE
        TKernel
        TKMath
        TKG3d
        TKBRep
        TKGeomBase
        TKGeomAlgo
        TKTopAlgo
        TKPrim
        TKBool
        TKBO
        TKFillet
        TKOffset
        TKShHealing
        TKMesh
        TKXSBase
        ${OCCT_DATAEXCHANGE_LIBS}
        # TKBin: BinTools::Write / Read (binary BREP, used by the v3
        # .materializr format — smaller + faster than BRepTools' ASCII).
        TKBin
        # TKService + TKV3d: Font_BRepFont / Font_BRepTextBuilder (the
        # sketch Text tool renders TrueType glyphs into outline wires).
        TKService
        TKV3d
    )
endif()

# zlib for the v3 .materializr file format (gzipped output, transparent on
# read). System zlib on Linux; vcpkg ships one on Windows.
find_package(ZLIB REQUIRED)
target_link_libraries(materializr PRIVATE ZLIB::ZLIB)

# ─── Windows specifics ───────────────────────────────────────────────────────
# Everything here is gated on Windows/MSVC, so the Linux build is unaffected.
if(WIN32)
    # GL loader (opengl32.dll only provides GL 1.1). GLEW comes from vcpkg.
    find_package(GLEW REQUIRED)
    target_link_libraries(materializr PRIVATE GLEW::GLEW)
endif()

# -Wswitch ONLY (not -Wall): a switch over an enum class that misses an
# enumerator is a build warning. The interactive-op dispatch (see
# plugin/InteractiveOp.h) relies on exactly this — it deliberately has no
# `default:`, so adding an op and forgetting to handle it is caught here
# instead of shipping a button that silently does nothing. Nothing else is
# turned on: -Wall over 28k lines of existing code would bury the one warning
# that's meant to be load-bearing. MSVC's nearest equivalent (C4062) is left
# off; the Linux/macOS CI legs cover the check for everyone.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(materializr PRIVATE -Wswitch)
endif()

if(MSVC)
    # _USE_MATH_DEFINES: M_PI etc.  NOMINMAX: keep std::min/max usable.
    # /bigobj: OpenCASCADE-heavy translation units exceed the default obj limit.
    target_compile_definitions(materializr PRIVATE
        _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(materializr PRIVATE /bigobj)
    # /EHa (asynchronous exceptions): the OCC_CONVERT_SIGNALS comment below
    # assumed this was already on ("throws from the signal handler natively")
    # but nothing ever actually passed it, so MSVC's default /EHsc only catches
    # synchronous C++ throws — a hardware fault deep in OCCT (e.g. reading a
    # truncated BREP file) surfaces as a raw SEH exception that skips every
    # OCC_CATCH_SIGNALS/try-catch in this codebase entirely (only gtest's own
    # SEH-aware runner caught it, reporting a failed test instead of OCCT's
    # handler converting it to a catchable Standard_Failure). Global, via
    # add_compile_options (not target_compile_options), so it also reaches
    # every test executable in tests/ added below — those hit the same OCCT
    # code paths and need the same fault-to-exception conversion.
    add_compile_options(/EHa)
endif()

if(APPLE)
    # OpenGL was deprecated on macOS 10.14; silence the framework headers app-wide.
    # gl_common.h also defines this, but ImGui's GL3 backend pulls <OpenGL/gl3.h>
    # in on its own, so define it on both targets. Apple still ships/supports the
    # framework, and there's no Metal/MoltenVK port yet.
    target_compile_definitions(materializr PRIVATE GL_SILENCE_DEPRECATION)
    target_compile_definitions(imgui_lib PRIVATE GL_SILENCE_DEPRECATION)
endif()

# Surface the project version to the C++ code so About / update-checker can use
# a single source of truth (the project() VERSION at the top of this file).
target_compile_definitions(materializr PRIVATE
    MATERIALIZR_VERSION="${PROJECT_VERSION}${MATERIALIZR_VERSION_SUFFIX}")

# OCC_CONVERT_SIGNALS turns the OCC_CATCH_SIGNALS macro into a real
# Standard_ErrorHandler so that an OCCT kernel fault (SIGSEGV/SIGFPE deep inside
# the modelling algorithms) longjmps to the nearest op-level try{} instead of
# aborting the whole app. OSD::SetSignal() (in main.cpp) installs the handler;
# this macro is what lets a `catch` actually receive the converted exception.
# OCCT's own cmake only exports this for the RelWithDebInfo config, so our
# Release AppImage was shipping with it OFF — every OCC_CATCH_SIGNALS (ShellOp,
# MoveFaceOp, …) silently expanded to nothing and kernel faults killed the app
# (issue #3: Shell-on-Shell crash). Define it unconditionally on Unix/Linux.
# Not for Windows/MSVC, which throws from the signal handler natively (/EHa).
if(NOT WIN32)
    target_compile_definitions(materializr PRIVATE OCC_CONVERT_SIGNALS)
endif()

# libcurl powers the Help → Check for Updates feature (a small HTTPS GET to
# GitHub's releases API). Required on both platforms.
find_package(CURL REQUIRED)
target_link_libraries(materializr PRIVATE CURL::libcurl)

# ─── Tests ───────────────────────────────────────────────────────────────────

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
