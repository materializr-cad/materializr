// Project files save at a compression level that is worth waiting for.
//
// gzipDeflate used Z_BEST_COMPRESSION for every save, on the reasoning that a
// project "saves once and loads many times". Measured on a real 17 MB project
// that trade turned out not to exist:
//
//     level 9 : 5.29 s -> 8.7 MB
//     level 6 : 1.01 s -> 8.7 MB      <- same bytes, a fifth of the time
//     level 1 : 0.71 s -> 9.1 MB
//
// So user saves dropped to Balanced (6) and the crash-recovery sidecar - which
// is rewritten every few seconds and read only after a crash - to Fastest (1).
//
// What has to hold, and is what these tests pin: BOTH levels produce a file
// that loads back identically. A gzip level is not supposed to be visible in
// the data, and it would be a nasty way to lose a project if it were.

#include "core/Document.h"
#include "core/History.h"
#include "io/ProjectIO.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <cstdio>
#include <string>
#include "test_tmp_path.h"

using materializr::ProjectIO;

namespace {

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

int faceCount(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) ++n;
    return n;
}

// A document with enough shape variety that a compression difference would
// have something to mangle.
void fill(Document& doc) {
    doc.addBody(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), "box");
    doc.addBody(BRepPrimAPI_MakeCylinder(4.0, 15.0).Shape(), "cyl");
}

void checkRoundTrip(ProjectIO::Compression comp, const char* label) {
    Document src;
    fill(src);
    const std::string path = mzrtest::tmpPath("mz_test_compression.mzr");

    auto sr = ProjectIO::save(path, src, nullptr, nullptr, comp);
    ASSERT_TRUE(sr.success) << label << ": save failed";

    Document back;
    auto lr = ProjectIO::load(path, back);
    std::remove(path.c_str());
    ASSERT_TRUE(lr.success) << label << ": load failed: " << lr.errorMessage;

    auto srcIds = src.getAllBodyIds();
    auto backIds = back.getAllBodyIds();
    ASSERT_EQ(srcIds.size(), backIds.size()) << label;
    for (size_t i = 0; i < srcIds.size(); ++i) {
        const TopoDS_Shape& a = src.getBody(srcIds[i]);
        const TopoDS_Shape& b = back.getBody(backIds[i]);
        EXPECT_EQ(faceCount(a), faceCount(b)) << label << " body " << i;
        EXPECT_NEAR(volumeOf(a), volumeOf(b), 1e-6) << label << " body " << i;
    }
}

} // namespace

TEST(ProjectCompression, BalancedRoundTrips) {
    checkRoundTrip(ProjectIO::Compression::Balanced, "Balanced");
}

TEST(ProjectCompression, FastestRoundTrips) {
    checkRoundTrip(ProjectIO::Compression::Fastest, "Fastest");
}

// Either level must produce a real gzip file - the loader sniffs the magic, so
// a level that somehow emitted raw deflate would still "save" and then fail to
// open on the next launch.
TEST(ProjectCompression, BothLevelsWriteGzip) {
    for (auto comp : {ProjectIO::Compression::Balanced,
                      ProjectIO::Compression::Fastest}) {
        Document doc;
        fill(doc);
        const std::string path = mzrtest::tmpPath("mz_test_compression_magic.mzr");
        ASSERT_TRUE(ProjectIO::save(path, doc, nullptr, nullptr, comp).success);

        std::FILE* f = std::fopen(path.c_str(), "rb");
        ASSERT_NE(f, nullptr);
        unsigned char magic[2] = {0, 0};
        ASSERT_EQ(std::fread(magic, 1, 2, f), 2u);
        std::fclose(f);
        std::remove(path.c_str());
        EXPECT_EQ(magic[0], 0x1f);
        EXPECT_EQ(magic[1], 0x8b);
    }
}
