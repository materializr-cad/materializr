// Android entry point. SDL2 owns the real platform main on Android (it runs an
// activity that loads this shared library and calls SDL_main); including
// SDL_main.h renames the function below to SDL_main. The body mirrors the
// desktop main() minus the CLI parsing - a phone passes no arguments.
#include "app/Application.h"
#include "core/Verbose.h"
#include "android_platform.h"

#include <OSD.hxx>
#include <SDL_main.h>

#include <iostream>
#include <dlfcn.h>
#include <cstdint>

int main(int /*argc*/, char* /*argv*/[]) {
    // OpenCASCADE's OSD_File / Resource_Manager closes a resource-file descriptor
    // in a way Android's fd-sanitizer flags as illegal (double-close / wrong
    // ownership tag), which aborts the whole process with SIGABRT. It's hit when
    // a STEP import (and other OCCT readers) load a shape-healing resource - so
    // importing a complex STEP killed the app instantly. OCCT's fd use is
    // otherwise functional, so downgrade fdsan from FATAL to warn-only. The
    // sanitizer is only fatal on API 29+ and android_fdsan_set_error_level is
    // API 29+, so dlsym it to stay compatible with minSdk 24 (older = no-op).
    if (auto* set_fdsan_level = reinterpret_cast<std::uint32_t (*)(std::uint32_t)>(
            dlsym(RTLD_DEFAULT, "android_fdsan_set_error_level"))) {
        set_fdsan_level(/*ANDROID_FDSAN_ERROR_LEVEL_WARN_ONCE=*/1u);
    }

    // Prepare writable storage, extract bundled fonts + OCCT resources, and set
    // HOME/CWD/CSF_* so settings, fonts and OpenCASCADE find their data. Must
    // run before constructing Application (which loads settings and touches OCCT).
    materializr::androidInitRuntime();

    // Convert OCCT internal faults (SIGSEGV/SIGFPE inside the kernel) into
    // catchable Standard_Failure exceptions, matching the desktop build.
    OSD::SetSignal(Standard_False);

    try {
        materializr::Application app(/*safeMode=*/false);
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
