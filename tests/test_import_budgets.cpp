// Regression tests for the untrusted-input budgets on the DXF and image import
// paths. Both are reachable from a file the user was merely sent: a DXF from the
// import dialog, an image from the REFIMG section of any shared .materializr.
//
// Before these budgets existed neither path had a ceiling of any kind — unlike
// SvgImport (32 MB + a 500k point budget) and IgesIO (kMaxEntities), which this
// codebase already got right.

#include "io/DxfImport.h"
#include "io/ImageDecode.h"
#include "modeling/Sketch.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_tmp_path.h"

namespace {

std::string writeTemp(const char* name, const std::string& text) {
    const std::string path = mzrtest::tmpPath(name);
    std::ofstream f(path, std::ios::trunc | std::ios::binary);
    f << text;
    f.close();
    return path;
}

// Minimal well-formed DXF carrying one LINE, as a positive control.
std::string oneLineDxf() {
    return "0\nSECTION\n2\nENTITIES\n"
           "0\nLINE\n10\n0.0\n20\n0.0\n11\n10.0\n21\n10.0\n"
           "0\nENDSEC\n0\nEOF\n";
}

} // namespace

// ── DXF ─────────────────────────────────────────────────────────────────────

TEST(DxfBudgets, WellFormedFileStillImports) {
    const std::string path = writeTemp("mz_budget_ok.dxf", oneLineDxf());
    materializr::Sketch sk;
    const auto r = materializr::DxfImport::importFile(path, sk);
    EXPECT_TRUE(r.success) << r.errorMessage;
}

TEST(DxfBudgets, OverlongLineAfterValidGeometryIsRefusedNotTruncated) {
    // THE discriminating case. An earlier version of this test put the long
    // line first, so nothing was ever emitted and the failure actually came
    // from "no profile entities found" — it passed without exercising the cap
    // at all. Here a complete LINE lands FIRST, so if the budget breach is
    // mistaken for clean EOF the import SUCCEEDS with the drawing silently
    // truncated, which is the bug.
    std::string evil = "0\nSECTION\n2\nENTITIES\n"
                       "0\nLINE\n10\n0.0\n20\n0.0\n11\n10.0\n21\n10.0\n"
                       "0\nLINE\n10\n";
    evil += std::string(8u * 1024 * 1024, '9');   // one 8 MB group value
    evil += "\n0\nENDSEC\n0\nEOF\n";
    const std::string path = writeTemp("mz_budget_trunc.dxf", evil);
    materializr::Sketch sk;
    const auto r = materializr::DxfImport::importFile(path, sk);
    EXPECT_FALSE(r.success)
        << "over-budget input must be refused, not silently truncated";
}

TEST(DxfBudgets, AbsurdSplineDegreeDoesNotOverflow) {
    // Group code 71 = 2147483647. `n + degree + 1` was computed BEFORE the
    // guard that would have rejected it — signed overflow, i.e. UB. The degree
    // is now bounded first, so this parses to "no usable entities" instead.
    std::string dxf = "0\nSECTION\n2\nENTITIES\n0\nSPLINE\n"
                      "71\n2147483647\n"
                      "10\n0.0\n20\n0.0\n10\n1.0\n20\n1.0\n"
                      "0\nENDSEC\n0\nEOF\n";
    const std::string path = writeTemp("mz_budget_degree.dxf", dxf);
    materializr::Sketch sk;
    // Must return (not trap, not hang); either outcome is acceptable, but it
    // must not be UB. Under UBSan this is the test that catches a regression.
    const auto r = materializr::DxfImport::importFile(path, sk);
    (void)r;
    SUCCEED();
}

TEST(DxfBudgets, MissingFileIsRefusedNotOpened) {
    materializr::Sketch sk;
    const auto r = materializr::DxfImport::importFile(mzrtest::tmpPath("mz_does_not_exist.dxf"), sk);
    EXPECT_FALSE(r.success);
}

// ── Images ──────────────────────────────────────────────────────────────────

namespace {

// A 1x1 PNG, with its IHDR width/height patchable. Small enough to inline, and
// the point is precisely that a tiny file can DECLARE enormous dimensions.
std::vector<uint8_t> tinyPng() {
    static const uint8_t bytes[] = {
        0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A,
        0,0,0,0x0D,'I','H','D','R',
        0,0,0,1, 0,0,0,1, 8,6,0,0,0, 0x1F,0x15,0xC4,0x89,
        0,0,0,0x0A,'I','D','A','T',
        0x78,0x9C,0x63,0x00,0x01,0x00,0x00,0x05,0x00,0x01,
        0x0D,0x0A,0x2D,0xB4,
        0,0,0,0,'I','E','N','D',0xAE,0x42,0x60,0x82
    };
    return std::vector<uint8_t>(std::begin(bytes), std::end(bytes));
}

void patchDims(std::vector<uint8_t>& png, uint32_t w, uint32_t h) {
    // IHDR width is at offset 16, height at 20 (big-endian).
    for (int i = 0; i < 4; ++i) {
        png[16 + i] = static_cast<uint8_t>((w >> (24 - 8 * i)) & 0xFF);
        png[20 + i] = static_cast<uint8_t>((h >> (24 - 8 * i)) & 0xFF);
    }
}

} // namespace

TEST(ImageBudgets, RejectsAbsurdDeclaredDimensions) {
    // ~90 bytes on disk, declares 30000x30000 — about 3.6 GB decoded. The check
    // has to happen on the PROBE, before stbi_load_from_memory allocates.
    std::vector<uint8_t> png = tinyPng();
    patchDims(png, 30000, 30000);

    int w = -1, h = -1;
    EXPECT_FALSE(materializr::probeImageSize(png.data(), png.size(), w, h));
    // Out-params must be untouched on failure.
    EXPECT_EQ(w, -1);
    EXPECT_EQ(h, -1);

    materializr::DecodedImage img;
    EXPECT_FALSE(materializr::decodeImage(png.data(), png.size(), img));
    EXPECT_TRUE(img.rgba.empty());
}

TEST(ImageBudgets, RejectsDimensionsThatOverflowTheProduct) {
    // Each axis is under the per-axis cap on its own; only w*h exceeds the
    // pixel budget. Catches a bounds check written per-axis instead of on the
    // product.
    std::vector<uint8_t> png = tinyPng();
    patchDims(png, 16000, 16000);   // 256 MP
    int w = -1, h = -1;
    EXPECT_FALSE(materializr::probeImageSize(png.data(), png.size(), w, h));
}

TEST(ImageBudgets, EmptyAndNullInputAreRejected) {
    int w = 0, h = 0;
    EXPECT_FALSE(materializr::probeImageSize(nullptr, 0, w, h));
    materializr::DecodedImage img;
    EXPECT_FALSE(materializr::decodeImage(nullptr, 0, img));
    const uint8_t junk[4] = {1, 2, 3, 4};
    EXPECT_FALSE(materializr::decodeImage(junk, sizeof(junk), img));
}

TEST(ImageBudgets, DecodeClearsOutputOnFailure) {
    // A caller reusing a DecodedImage must not be handed the previous image
    // when the new decode fails.
    materializr::DecodedImage img;
    img.width = 7;
    img.height = 9;
    img.rgba.assign(64, 0xAB);
    const uint8_t junk[4] = {1, 2, 3, 4};
    EXPECT_FALSE(materializr::decodeImage(junk, sizeof(junk), img));
    EXPECT_TRUE(img.rgba.empty());
    EXPECT_EQ(img.width, 0);
    EXPECT_EQ(img.height, 0);
}
