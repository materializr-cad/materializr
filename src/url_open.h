#pragma once

// Safe URL opener. Hands the URL to the OS via platformOpenUrl (SDL_OpenURL), which does NOT run a
// shell - unlike the old `std::system("xdg-open \"" + url + "\" ...")` path, where
// a server-controlled release URL (the GitHub API's html_url) was an OS
// command-injection vector ($(...), backticks, and quote-breakout all executed).
//
// Callers opening an UNTRUSTED url (anything derived from a network response)
// MUST pass the expected host prefix, e.g. openUrl(url, "https://github.com/"),
// so a tampered response can't redirect the user to an arbitrary destination.

#include "platform_sdl.h"
#include <string>
#include <cstdio>

namespace materializr {

inline bool openUrl(const std::string& url, const char* requiredPrefix = nullptr) {
    // Scheme: https only - every link the app opens is an https resource.
    if (url.rfind("https://", 0) != 0) {
        std::fprintf(stderr, "openUrl: refusing non-https URL\n");
        return false;
    }
    // Host pinning for untrusted (server-supplied) URLs.
    if (requiredPrefix && url.rfind(requiredPrefix, 0) != 0) {
        std::fprintf(stderr, "openUrl: URL did not match required prefix\n");
        return false;
    }
    // Defense in depth: a valid URI never contains these unencoded (RFC 3986
    // controls + "unsafe" set), and they are exactly what an injection payload
    // needs. platformOpenUrl never invokes a shell, so this is belt-and-suspenders.
    for (unsigned char c : url) {
        if (c < 0x20 || c >= 0x7f || c == ' ' || c == '"' || c == '<' ||
            c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' ||
            c == '|' || c == '}') {
            std::fprintf(stderr, "openUrl: refusing malformed URL\n");
            return false;
        }
    }
    std::string err;
    if (!platformOpenUrl(url, &err)) {
        std::fprintf(stderr, "openUrl: failed to open URL: %s\n", err.c_str());
        return false;
    }
    return true;
}

} // namespace materializr
