cmake_minimum_required(VERSION 3.6)

project(GAME)

# 16 KB page support (required by Google Play for Android 15+): every shared
# library - libmain.so, libSDL2.so, and the cross-built OCCT .so (see
# setup-deps.sh) - must have its ELF LOAD segments aligned to 16 KB. NDK r27+
# defaults to this; on r26 we pass the linker flag. add_link_options applies to
# all targets declared after it (SDL and src below).
add_link_options(-Wl,-z,max-page-size=16384)

# armeabi-v7a requires cpufeatures library
# include(AndroidNdkModules)
# android_ndk_import_module_cpufeatures()


# SDL sources are in a subfolder named "SDL"
add_subdirectory(SDL)

# Compilation of companion libraries
#add_subdirectory(SDL_image)
#add_subdirectory(SDL_mixer)
#add_subdirectory(SDL_ttf)

# Your game and its CMakeLists.txt are in a subfolder named "src"
add_subdirectory(src)

