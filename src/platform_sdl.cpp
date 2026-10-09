#include "platform_sdl.h"

#include <SDL.h>

namespace materializr {

std::uint32_t platformTicksMs() {
    return static_cast<std::uint32_t>(SDL_GetTicks());
}

void platformSleepMs(std::uint32_t ms) {
    SDL_Delay(ms);
}

std::string platformPrefPath() {
    char* base = SDL_GetPrefPath("Materializr", "Materializr");
    if (!base) return {};
    std::string out(base);
    SDL_free(base);
    return out;
}

bool platformOpenUrl(const std::string& url, std::string* err) {
    if (SDL_OpenURL(url.c_str()) != 0) {
        if (err) *err = SDL_GetError();
        return false;
    }
    return true;
}

#if defined(__ANDROID__)
std::string platformAndroidExternalStoragePath() {
    const char* p = SDL_AndroidGetExternalStoragePath();
    return p ? p : std::string();
}
#endif

} // namespace materializr
