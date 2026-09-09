#pragma once
#include <string>

namespace materializr {

// Tiny GitHub-releases checker. Hits the public releases API for a given owner
// /repo, pulls the latest tag, and exposes it for comparison against the
// running build's MATERIALIZR_VERSION. No persistent state; one shot per call.
class UpdateChecker {
public:
    struct Result {
        bool ok = false;            // network call succeeded and a tag was parsed
        bool updateAvailable = false;
        std::string current;        // version baked into this binary
        std::string latest;         // tag from the GitHub release (sans leading "v")
        std::string releasePageUrl; // human-facing page for downloading the new build
        std::string errorMessage;   // populated when ok == false
    };

    // Blocks for the duration of the HTTPS request (typically <1s). Designed
    // to be called in response to a user clicking "Check for Updates".
    //
    // includePrereleases == false (default): queries /releases/latest, which
    // GitHub defines to EXCLUDE pre-releases - the stable channel.
    // includePrereleases == true: queries /releases (the full list, newest
    // first) and takes the most recent entry, so beta tags like
    // "v1.3.0-beta.1" are offered - the beta channel.
    static Result check(const std::string& githubOwner,
                        const std::string& githubRepo,
                        bool includePrereleases = false);

    // Returns -1 if a < b, 0 if equal, +1 if a > b. Both inputs may carry a
    // leading "v". Understands semver pre-release suffixes: "1.3.0-beta.1" is
    // OLDER than "1.3.0", and "1.3.0-beta.1" < "1.3.0-beta.2".
    static int compareVersions(const std::string& a, const std::string& b);
};

} // namespace materializr
