#pragma once
//
// Portable scratch paths for tests.
//
// Several suites hardcoded "/tmp/...", which is fine on Linux and macOS and
// simply does not exist on Windows - the file open fails and the surrounding
// ASSERT_TRUE(res.success) fires, so the test reports a product bug that is
// really just an unwritable path. std::filesystem::temp_directory_path() picks
// the right directory on all three (honouring TMPDIR on POSIX and
// TEMP/TMP/USERPROFILE on Windows), so tests use these helpers instead.

#include <filesystem>
#include <string>

namespace mzrtest {

// Temp directory as a string, WITHOUT a trailing separator.
inline std::string tmpDir() {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    if (ec || p.empty()) p = std::filesystem::current_path(ec); // last resort
    return p.string();
}

// A path to `name` inside the temp directory, using the platform's separator.
inline std::string tmpPath(const std::string& name) {
    return (std::filesystem::path(tmpDir()) / name).string();
}

// A subdirectory inside the temp directory, created if absent. Returns its path
// without a trailing separator. Replaces `std::system("mkdir -p /tmp/...")`,
// which relied on a POSIX shell as well as a POSIX path.
inline std::string tmpSubdir(const std::string& name) {
    std::filesystem::path p = std::filesystem::path(tmpDir()) / name;
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}

} // namespace mzrtest
