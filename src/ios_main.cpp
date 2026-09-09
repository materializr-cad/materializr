// iOS entry point. SDL2 owns the real platform main on iOS (SDL2main's
// SDL_UIKitRunApp starts the UIApplication and calls SDL_main); including
// SDL_main.h renames the function below to SDL_main. The body mirrors
// android_main.cpp minus the Android-only fdsan workaround.
//
// Guarded by MZ_IOS (not a build-system exclusion) so the Android build - which
// globs the whole src/ tree - compiles this file to nothing, the same pattern
// as android_files.cpp on other platforms. The desktop build's explicit source
// list never includes it.
#include "platform_defs.h"
#if defined(MZ_IOS)

#include "app/Application.h"
#include "core/Verbose.h"
#include "ios_platform.h"

#include <OSD.hxx>
#include <SDL_main.h>

#include <iostream>

int main(int /*argc*/, char* /*argv*/[]) {
    // Point cwd/CSF_* at the app bundle, ensure the settings dir exists, and
    // install the background-lifecycle watch. Must run before constructing
    // Application (which loads settings and touches OCCT).
    materializr::iosInitRuntime();

    // Convert OCCT internal faults (SIGSEGV/SIGFPE inside the kernel) into
    // catchable Standard_Failure exceptions, matching the other platforms.
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

#endif // MZ_IOS
