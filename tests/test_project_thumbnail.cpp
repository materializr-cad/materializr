// THUMB_PNG round-trip: the embedded-thumbnail section must survive
// save → peekThumbnail byte-identically, must not disturb a normal load
// (older loaders skip it as an unknown section - same dispatch path), and
// peek must fail cleanly on thumb-less and legacy files.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "io/ProjectIO.h"

#include <BRepPrimAPI_MakeBox.hxx>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using materializr::ProjectIO;

namespace {

std::string tmpPath(const char* name) {
#ifdef _WIN32
    const char* base = std::getenv("TEMP");
    return std::string(base ? base : ".") + "\\" + name;
#else
    return std::string("/tmp/") + name;
#endif
}

// Not a real PNG - ProjectIO treats the thumbnail as an opaque byte blob, so
// the test exercises binary fidelity (every byte value, including newlines
// and nulls, which is what base64 exists to protect in the line parser).
std::vector<uint8_t> syntheticBytes(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    return v;
}

Document makeDoc() {
    Document doc;
    doc.addBody(BRepPrimAPI_MakeBox(10, 10, 10).Shape(), "Box");
    doc.addBody(BRepPrimAPI_MakeBox(4, 4, 12).Shape(), "Post");
    return doc;
}

} // namespace

TEST(ProjectThumbnail, RoundTripsByteIdentical) {
    const std::string path = tmpPath("mzr_thumb_roundtrip.materializr");
    Document doc = makeDoc();
    // Sizes chosen to hit every base64 padding case (len % 3 == 0, 1, 2).
    for (size_t len : {3000u * 3u, 3001u * 3u + 1u, 3002u * 3u + 2u}) {
        std::vector<uint8_t> thumb = syntheticBytes(len);
        auto save = ProjectIO::save(path, doc, nullptr, &thumb);
        ASSERT_TRUE(save.success) << save.errorMessage;

        std::vector<uint8_t> out;
        ASSERT_TRUE(ProjectIO::peekThumbnail(path, out));
        EXPECT_EQ(out, thumb);
    }
    std::remove(path.c_str());
}

TEST(ProjectThumbnail, LoadIgnoresThumbSection) {
    const std::string path = tmpPath("mzr_thumb_load.materializr");
    Document doc = makeDoc();
    std::vector<uint8_t> thumb = syntheticBytes(10000);
    ASSERT_TRUE(ProjectIO::save(path, doc, nullptr, &thumb).success);

    Document loaded;
    auto res = ProjectIO::load(path, loaded);
    ASSERT_TRUE(res.success) << res.errorMessage;
    EXPECT_EQ(res.bodiesLoaded, 2);
    EXPECT_EQ(loaded.bodyCount(), 2);
    std::remove(path.c_str());
}

TEST(ProjectThumbnail, PeekFailsWithoutThumb) {
    const std::string path = tmpPath("mzr_thumb_none.materializr");
    Document doc = makeDoc();
    ASSERT_TRUE(ProjectIO::save(path, doc).success);

    std::vector<uint8_t> out;
    EXPECT_FALSE(ProjectIO::peekThumbnail(path, out));
    std::remove(path.c_str());
}

TEST(ProjectThumbnail, PeekFailsOnGarbageAndMissingFiles) {
    std::vector<uint8_t> out;
    EXPECT_FALSE(ProjectIO::peekThumbnail(tmpPath("mzr_thumb_missing.materializr"), out));

    const std::string path = tmpPath("mzr_thumb_garbage.materializr");
    {
        std::ofstream f(path, std::ios::binary);
        f << "definitely not a project file\n\x01\x02\x03";
    }
    EXPECT_FALSE(ProjectIO::peekThumbnail(path, out));
    std::remove(path.c_str());
}

TEST(ProjectThumbnail, PeekHandlesEmptyProject) {
    // Zero bodies: the body-skip loop runs zero times and the tail begins
    // immediately - both with and without a thumbnail.
    const std::string path = tmpPath("mzr_thumb_empty.materializr");
    Document doc;
    std::vector<uint8_t> thumb = syntheticBytes(500);
    ASSERT_TRUE(ProjectIO::save(path, doc, nullptr, &thumb).success);
    std::vector<uint8_t> out;
    ASSERT_TRUE(ProjectIO::peekThumbnail(path, out));
    EXPECT_EQ(out, thumb);

    ASSERT_TRUE(ProjectIO::save(path, doc).success);
    EXPECT_FALSE(ProjectIO::peekThumbnail(path, out));
    std::remove(path.c_str());
}
