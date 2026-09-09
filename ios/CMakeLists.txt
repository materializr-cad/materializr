# Materializr iOS app. Build with the Xcode generator (code signing + device
# deploy need it):
#
#   cmake -S ios -B build-ios -GXcode -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DMATERIALIZR_IOS_PREFIX=$HOME/iOS/prefix/iphoneos-arm64 \
#         [-DMZ_DEVELOPMENT_TEAM=YOURTEAMID]
#   open build-ios/MaterializriOS.xcodeproj
#
# Mirrors android/app/jni/src/CMakeLists.txt: glob the shared src/ tree, drop
# the desktop/Android entry points, link the cross-built static OCCT/FreeType/
# SDL2 prefix produced by ios/scripts/setup-deps.sh.
cmake_minimum_required(VERSION 3.24)
project(MaterializriOS CXX OBJCXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "Configure with -DCMAKE_SYSTEM_NAME=iOS (see header comment)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)

get_filename_component(MZ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(MZ_SRC "${MZ_ROOT}/src")

# Cross-built static prefix (FreeType + OCCT + SDL2).
if(NOT DEFINED MATERIALIZR_IOS_PREFIX)
    set(MATERIALIZR_IOS_PREFIX "$ENV{HOME}/iOS/prefix/iphoneos-arm64")
endif()
if(NOT EXISTS "${MATERIALIZR_IOS_PREFIX}/include/opencascade")
    message(FATAL_ERROR "No cross-built OCCT at ${MATERIALIZR_IOS_PREFIX} - run ios/scripts/setup-deps.sh first")
endif()
set(OCCT_INC "${MATERIALIZR_IOS_PREFIX}/include/opencascade")
set(OCCT_LIB "${MATERIALIZR_IOS_PREFIX}/lib")

# ── SDL2 (static, from the prefix) ───────────────────────────────────────────
# CMake's iOS platform sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY, re-rooting
# every find_package() path under the SDK sysroot; the prefix must be a find
# root itself or its SDL2Config.cmake is invisible.
list(APPEND CMAKE_FIND_ROOT_PATH "${MATERIALIZR_IOS_PREFIX}")
find_package(SDL2 CONFIG REQUIRED PATHS "${MATERIALIZR_IOS_PREFIX}" NO_DEFAULT_PATH)
if(TARGET SDL2::SDL2-static)
    set(MZ_SDL2_TARGET SDL2::SDL2-static)
else()
    set(MZ_SDL2_TARGET SDL2::SDL2)
endif()

# ── ImGui (docking) + GLM, same versions as the desktop/Android builds ───────
include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG docking)
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
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(imgui_lib PUBLIC ${MZ_SDL2_TARGET})

# ── App sources ──────────────────────────────────────────────────────────────
# Glob the shared tree, drop foreign entry points. ios_main.cpp is picked up by
# the glob (it compiles empty off-iOS); the Objective-C++ glue is listed
# explicitly. android_shims.cpp stays IN: it is the MZ_MOBILE UpdateChecker
# stub. android_files.cpp / android_platform.cpp compile to (near) nothing.
file(GLOB_RECURSE MZ_SOURCES CONFIGURE_DEPENDS "${MZ_SRC}/*.cpp")
list(FILTER MZ_SOURCES EXCLUDE REGEX "/main\\.cpp$")            # desktop CLI entry
list(FILTER MZ_SOURCES EXCLUDE REGEX "/android_main\\.cpp$")    # Android SDL_main (unguarded)
list(FILTER MZ_SOURCES EXCLUDE REGEX "/UpdateChecker\\.cpp$")   # libcurl (desktop-only)

set(MZ_OBJCXX_SOURCES
    "${MZ_SRC}/ios_platform.mm"
    "${MZ_SRC}/ios_files.mm"
    "${MZ_SRC}/ios_storekit.mm")
set_source_files_properties(${MZ_OBJCXX_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

add_executable(materializr MACOSX_BUNDLE ${MZ_SOURCES} ${MZ_OBJCXX_SOURCES})

target_include_directories(materializr PRIVATE "${MZ_SRC}")
target_include_directories(materializr SYSTEM PRIVATE "${OCCT_INC}")
target_link_libraries(materializr PRIVATE ${MZ_SDL2_TARGET} SDL2::SDL2main imgui_lib glm::glm)

# OpenCASCADE static toolkits. ld64 re-searches archives, so the glob order is
# fine despite OCCT's internal cycles. FreeType resolves TKService's font refs.
file(GLOB OCCT_A_PATHS "${OCCT_LIB}/libTK*.a")
list(LENGTH OCCT_A_PATHS _occt_count)
message(STATUS "Linking ${_occt_count} static OpenCASCADE toolkits from ${OCCT_LIB}")
target_link_libraries(materializr PRIVATE ${OCCT_A_PATHS} "${MATERIALIZR_IOS_PREFIX}/lib/libfreetype.a")

# System libs + frameworks. The SDL2 static config carries its own framework
# deps; the explicit list below covers our direct uses (GLES, pickers, share
# sheet) and doubles as insurance if the config's interface is incomplete.
# CoreBluetooth/CoreMotion/CoreHaptics are deliberately absent: they were only
# referenced by SDL's joystick/haptic/sensor subsystems, which
# ios/scripts/setup-deps.sh now builds OFF (App Store validation demands
# purpose strings for APIs the binary references - ITMS-90683). A prefix built
# before that change needs them back to link. GameController stays: SDL's
# UIKit event backend uses GCKeyboard/GCMouse for iPad hardware keyboard and
# mouse support (independent of the joystick subsystem), and that framework
# needs no purpose string.
target_link_libraries(materializr PRIVATE z iconv
    "-framework UIKit" "-framework Foundation" "-framework CoreGraphics"
    "-framework QuartzCore" "-framework OpenGLES" "-framework Metal"
    "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio"
    "-framework GameController" "-framework CoreVideo" "-framework ImageIO"
    "-framework UniformTypeIdentifiers" "-framework StoreKit")

# Version: pass -DMATERIALIZR_VERSION_NAME=<x.y.z[-suffix]> to override;
# otherwise default to the desktop project's version + suffix (the single
# source of truth in the root CMakeLists), so archives self-identify
# without remembering a configure flag.
if(NOT DEFINED MATERIALIZR_VERSION_NAME OR MATERIALIZR_VERSION_NAME STREQUAL "")
    file(READ "${MZ_ROOT}/CMakeLists.txt" _root_cml)
    string(REGEX MATCH "project\\(Materializr VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" _ "${_root_cml}")
    if(CMAKE_MATCH_1)
        set(MATERIALIZR_VERSION_NAME "${CMAKE_MATCH_1}")
        string(REGEX MATCH "set\\(MATERIALIZR_VERSION_SUFFIX \"([^\"]*)\"" _ "${_root_cml}")
        string(APPEND MATERIALIZR_VERSION_NAME "${CMAKE_MATCH_1}")
    else()
        set(MATERIALIZR_VERSION_NAME "0.0.0") # root parse failed - ad-hoc build
    endif()
    unset(_root_cml)
endif()
target_compile_definitions(materializr PRIVATE
    MATERIALIZR_VERSION="${MATERIALIZR_VERSION_NAME}"
    OCC_CONVERT_SIGNALS)   # OCCT kernel faults -> catchable exceptions (Unix)

# ── Bundle ───────────────────────────────────────────────────────────────────
# CFBundleVersion must be numeric - strip any -beta suffix for the plist.
string(REGEX REPLACE "[-+].*$" "" MZ_PLIST_VERSION "${MATERIALIZR_VERSION_NAME}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
               "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

set_target_properties(materializr PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.materializr.cad"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2"     # iPad-only
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_ENABLE_BITCODE "NO"
    XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING "YES"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    # Product → Archive marks the result an "iOS App Archive" (uploadable to
    # App Store Connect) only if the .app is installed into the archive at
    # Applications/. CMake-generated targets default to SKIP_INSTALL=YES,
    # which yields a useless "Generic Xcode Archive" instead.
    XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
    XCODE_ATTRIBUTE_INSTALL_PATH "$(LOCAL_APPS_DIR)"
    # CMake's Xcode generator defaults to plain "dwarf", which leaves the
    # archive without a dSYM - App Store crash reports would come back
    # unsymbolicated. Release only; Debug keeps the faster format.
    "XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT[variant=Release]" "dwarf-with-dsym")
if(DEFINED MZ_DEVELOPMENT_TEAM)
    set_target_properties(materializr PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${MZ_DEVELOPMENT_TEAM}")
endif()

# Keep the StoreKit test configuration as a project member across regens, so
# the scheme's StoreKit Configuration picker always finds it - a reference
# dragged into the Xcode navigator by hand dies with the next pbxproj
# regeneration. HEADER_FILE_ONLY: listed, never compiled or bundled.
target_sources(materializr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Materializr.storekit")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Materializr.storekit"
    PROPERTIES HEADER_FILE_ONLY TRUE)

# ── Bundle resources ─────────────────────────────────────────────────────────
# Staged through Xcode's Copy Bundle Resources phase (runs before code signing;
# a POST_BUILD copy would land after signing and break device installs).
# On iOS "Resources/<sub>" maps to "<bundle root>/<sub>" - matching the
# cwd-relative lookups after ios_platform.mm chdirs to the bundle.

# Launch storyboard (required for native fullscreen resolution).
target_sources(materializr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/LaunchScreen.storyboard")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/LaunchScreen.storyboard"
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

# App icon: single-size (1024x1024, opaque) asset catalog. Xcode compiles it
# with actool (ASSETCATALOG_COMPILER_APPICON_NAME above) and derives every
# launcher/Settings/Spotlight size; AppIcon.png is regenerated from the
# repo-root icon.png (cover-crop to square, alpha flattened - Apple rejects
# icons with an alpha channel).
target_sources(materializr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets"
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

# Fonts -> <bundle>/assets/fonts (same layout the Android build extracts to).
file(GLOB MZ_FONT_FILES "${MZ_ROOT}/assets/fonts/*.ttf")
foreach(f ${MZ_FONT_FILES})
    target_sources(materializr PRIVATE "${f}")
    set_source_files_properties("${f}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/assets/fonts")
endforeach()

# OCCT resources -> <bundle>/occt-resources/<dir>/... (the CSF_* env vars set
# in ios_platform.mm point here). Same directory set android/copy-occt-libs.sh
# stages into the APK.
set(MZ_OCCT_RES "${MATERIALIZR_IOS_PREFIX}/share/opencascade/resources")
foreach(d UnitsAPI StdResource XSMessage SHMessage XSTEPResource BOPAlgo TObj XmlOcafResource)
    file(GLOB_RECURSE _res_files "${MZ_OCCT_RES}/${d}/*")
    foreach(f ${_res_files})
        file(RELATIVE_PATH _rel "${MZ_OCCT_RES}" "${f}")
        get_filename_component(_reldir "${_rel}" DIRECTORY)
        target_sources(materializr PRIVATE "${f}")
        set_source_files_properties("${f}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/occt-resources/${_reldir}")
    endforeach()
endforeach()
