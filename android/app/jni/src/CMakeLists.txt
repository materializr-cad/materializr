cmake_minimum_required(VERSION 3.22)
project(materializr_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (this file lives at android/app/jni/src/).
get_filename_component(MZ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(MZ_SRC "${MZ_ROOT}/src")

# Cross-built OpenCASCADE prefix (FreeType is statically baked into TKService).
if(NOT DEFINED MATERIALIZR_OCCT_PREFIX)
    message(FATAL_ERROR "MATERIALIZR_OCCT_PREFIX not set (path to cross-built OCCT)")
endif()
# Accept EITHER a single-ABI prefix (…/prefix/arm64-v8a - the per-APK build) or
# a BASE prefix (…/prefix) that needs the ABI appended. The base form lets one
# build compile every ABI in an app bundle (AAB), where CMake runs once per ABI
# with ANDROID_ABI set, each linking its own OCCT.
if(EXISTS "${MATERIALIZR_OCCT_PREFIX}/include/opencascade")
    set(OCCT_ROOT "${MATERIALIZR_OCCT_PREFIX}")
elseif(EXISTS "${MATERIALIZR_OCCT_PREFIX}/${ANDROID_ABI}/include/opencascade")
    set(OCCT_ROOT "${MATERIALIZR_OCCT_PREFIX}/${ANDROID_ABI}")
else()
    message(FATAL_ERROR
        "OCCT not found for ${ANDROID_ABI} under ${MATERIALIZR_OCCT_PREFIX} "
        "(need include/opencascade there or in an ${ANDROID_ABI}/ subdir)")
endif()
set(OCCT_INC "${OCCT_ROOT}/include/opencascade")
set(OCCT_LIB "${OCCT_ROOT}/lib")

# ── ImGui (docking) + GLM, same versions as the desktop build ────────────────
include(FetchContent)
# Pinned to a release TAG, not the `docking` branch head and not a commit on it
# - see the rationale in the top-level CMakeLists.txt (docking is rebased, which
# orphans branch commits and breaks clean builds). Keep this in step with that
# one so desktop and Android build the same ImGui.
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git
                     GIT_TAG v1.92.8-docking) # b61e56346a92cfcaf1f43a545ca37b0b32239654
FetchContent_MakeAvailable(imgui)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_MakeAvailable(glm)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# imgui_impl_opengl3 auto-selects the GLES3 loader when it sees __ANDROID__.
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(imgui_lib PUBLIC SDL2)

# ── App sources ──────────────────────────────────────────────────────────────
# Glob the whole tree, then drop translation units that are desktop-only.
file(GLOB_RECURSE MZ_SOURCES CONFIGURE_DEPENDS "${MZ_SRC}/*.cpp")
list(FILTER MZ_SOURCES EXCLUDE REGEX "/main\\.cpp$")            # desktop CLI entry
list(FILTER MZ_SOURCES EXCLUDE REGEX "/UpdateChecker\\.cpp$")   # libcurl (desktop-only)
# AI Assistant: every backend is a network call, and Android gates loopback
# traffic behind INTERNET the same as any other host - there is no way to
# reach even a local Ollama/LM Studio server without it. Desktop only; see
# docs/superpowers/specs/2026-09-12-ai-assistant-design.md's "Platform Scope"
# section.
list(FILTER MZ_SOURCES EXCLUDE REGEX "/src/ai/")
list(FILTER MZ_SOURCES EXCLUDE REGEX "/AiAssistantPlugin\\.cpp$")
# FileDialogs.cpp IS built on Android: it compiles the in-app ImGui browser
# (pfd is #ifdef'd out). android_main.cpp (SDL_main) + android_shims.cpp
# (update-check stub) replace the remaining desktop-only entry points.

add_library(main SHARED ${MZ_SOURCES})

target_include_directories(main PRIVATE "${MZ_SRC}")
target_include_directories(main SYSTEM PRIVATE "${OCCT_INC}")

# SDL2 (built by the sibling add_subdirectory(SDL)) provides the headers/target.
target_link_libraries(main PRIVATE SDL2 SDL2main imgui_lib glm::glm)

# OpenCASCADE. The desktop build lists ~21 toolkits and relies on the system
# OCCT's DT_NEEDED to pull the rest transitively (TKG2d, etc.). Under Android's
# --no-undefined link, every transitively-needed toolkit must be on the line, so
# we link the whole cross-built set. --no-as-needed (in LINK_FLAGS, before the
# libs) keeps them all regardless of the cyclic dependency order.
file(GLOB OCCT_SO_PATHS "${OCCT_LIB}/libTK*.so")
list(LENGTH OCCT_SO_PATHS _occt_count)
message(STATUS "Linking ${_occt_count} OpenCASCADE toolkits from ${OCCT_LIB}")
target_link_options(main PRIVATE "-Wl,--no-as-needed")
target_link_libraries(main PRIVATE ${OCCT_SO_PATHS})

# Android system libs: GL ES 3, EGL, zlib (NDK-provided), Android logging.
find_library(EGL_LIB EGL)
find_library(GLES3_LIB GLESv3)
find_library(LOG_LIB log)
find_library(ANDROID_LIB android)
find_library(Z_LIB z)
target_link_libraries(main PRIVATE ${GLES3_LIB} ${EGL_LIB} ${LOG_LIB} ${ANDROID_LIB} ${Z_LIB})

# Version is passed in from build.gradle (-DMATERIALIZR_VERSION_NAME=<versionName>)
# so the splash/about version can't drift from the APK version - it silently did
# (0.9.7.1 through 1.0.x, then 1.2.0 while the app shipped 1.2.2). The fallback
# only applies to a direct CMake build outside gradle.
if(NOT DEFINED MATERIALIZR_VERSION_NAME OR MATERIALIZR_VERSION_NAME STREQUAL "")
    set(MATERIALIZR_VERSION_NAME "0.0.0")
endif()
target_compile_definitions(main PRIVATE MATERIALIZR_VERSION="${MATERIALIZR_VERSION_NAME}")

# Make OCC_CATCH_SIGNALS a real Standard_ErrorHandler (see the matching note in
# the top-level CMakeLists.txt): without OCC_CONVERT_SIGNALS the macro is a
# no-op and an OCCT kernel fault aborts the app instead of being caught by an
# op's try{}. Android is always Unix, so define it unconditionally.
target_compile_definitions(main PRIVATE OCC_CONVERT_SIGNALS)
