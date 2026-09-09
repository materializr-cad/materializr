// Audit-hardening regression tests: pins the fixes for the residual findings
// from docs/CODE-AUDIT.md / docs/CODE-AUDIT-DEEPSCAN.md that survived the
// first hardening pass.
//
//  - Settings: array-indexing / enum ints are clamped on load (orbitButton=99
//    would otherwise index past ImGui's MouseDown[5] every drag frame), and
//    string values with embedded control characters cannot inject key=value
//    lines into the .cfg on the save -> load round trip.
//  - ProjectIO::load is transactional-on-failure: a corrupt file never leaves
//    a partially-mutated (or stale) Document behind - failure means empty.
//  - SvgImport: a file whose paths flatten into more vertices than the global
//    budget is refused instead of freezing buildWires/regions downstream.

#include "core/Document.h"
#include "io/ProjectIO.h"
#include "io/Settings.h"
#include "modeling/SvgImport.h"

#include <gtest/gtest.h>

#include <BRepPrimAPI_MakeBox.hxx>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include "test_tmp_path.h"

using materializr::AppSettings;
using materializr::SvgImport;
using materializr::SvgPaths;

namespace {

std::string tempPath(const char* name) {
    // temp_directory_path() honours TMPDIR on POSIX and TEMP/TMP on Windows,
    // where the old "/tmp" fallback was an unwritable path.
    return mzrtest::tmpPath(name);
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::trunc | std::ios::binary);
    ASSERT_TRUE(f.is_open()) << "cannot write " << path;
    f << text;
}

} // namespace

// ─── Settings clamping ───────────────────────────────────────────────────────

TEST(SettingsHardening, OutOfRangeIntsAreClampedOnLoad) {
    const std::string path = tempPath("mz_test_settings_clamp.cfg");
    writeFile(path,
              "orbitButton = 99\n"
              "panButton = -7\n"
              "theme = 12\n"
              "msaaSamples = 1024\n"
              "meshQuality = 42\n"
              "inferenceLevel = -3\n"
              "angleSnapDeg = 4000\n");

    AppSettings s = materializr::SettingsIO::load(path);
    std::remove(path.c_str());

    EXPECT_GE(s.orbitButton, 0);
    EXPECT_LE(s.orbitButton, 2);
    EXPECT_GE(s.panButton, 0);
    EXPECT_LE(s.panButton, 2);
    EXPECT_GE(s.theme, 0);
    EXPECT_LE(s.theme, 1);
    EXPECT_LE(s.msaaSamples, 8);
    EXPECT_GE(s.meshQuality, 0);
    EXPECT_LE(s.meshQuality, 3);
    EXPECT_GE(s.inferenceLevel, 0);
    EXPECT_LE(s.inferenceLevel, 3);
    EXPECT_GE(s.angleSnapDeg, 1);
    EXPECT_LE(s.angleSnapDeg, 90);
}

TEST(SettingsHardening, EmbeddedNewlineCannotInjectKeys) {
    // A POSIX filename may legally contain '\n'. Before the fix, save() wrote
    // it verbatim and the next load() re-parsed the payload as its own
    // key=value line - flipping arbitrary settings (here: orbitButton).
    const std::string path = tempPath("mz_test_settings_inject.cfg");

    AppSettings s;                      // defaults: orbitButton is 0 or 2
    const int cleanOrbit = s.orbitButton;
    s.lastProjectPath = "/tmp/evil\norbitButton = 99\n.mzr";
    ASSERT_TRUE(materializr::SettingsIO::save(path, s));

    AppSettings back = materializr::SettingsIO::load(path);
    std::remove(path.c_str());

    EXPECT_EQ(back.orbitButton, cleanOrbit)
        << "control chars in a written string value injected a key";
    // The path survives minus the control characters.
    EXPECT_EQ(back.lastProjectPath.find('\n'), std::string::npos);
}

// ─── ProjectIO transactional load ────────────────────────────────────────────

TEST(ProjectIOHardening, FailedLoadLeavesDocumentEmpty) {
    // Pre-populate the document, then feed load() a corrupt file. The old
    // behavior left whatever the parser got through (or, on an early header
    // failure, the stale old content). The contract now: failure => empty.
    Document doc;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    doc.addBody(box, "stale");
    ASSERT_EQ(doc.getAllBodyIds().size(), 1u);

    const std::string bad = tempPath("mz_test_corrupt.materializr");

    // Early failure: bad header.
    writeFile(bad, "NOT_A_PROJECT_FILE\n");
    auto r1 = materializr::ProjectIO::load(bad, doc, nullptr);
    EXPECT_FALSE(r1.success);
    EXPECT_TRUE(doc.getAllBodyIds().empty())
        << "failed load left stale bodies behind";

    // Mid-parse failure: valid header, then garbage where BODY_COUNT belongs.
    doc.addBody(box, "stale2");
    writeFile(bad, "MATERIALIZR_PROJECT v2\nBODY_COUNT nonsense\n");
    auto r2 = materializr::ProjectIO::load(bad, doc, nullptr);
    EXPECT_FALSE(r2.success);
    EXPECT_TRUE(doc.getAllBodyIds().empty())
        << "failed load left a partially-parsed document behind";

    std::remove(bad.c_str());
}

// ─── SVG global vertex budget ────────────────────────────────────────────────

TEST(SvgHardening, VertexBudgetRefusesPathologicalFile) {
    // ~6000 full-size circles: nanosvg turns each into 4 high-turn cubics that
    // sample to hundreds of points, comfortably past the 500k global budget. A
    // real drawing never looks like this; a DoS file does.
    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" "
           "height=\"100\" viewBox=\"0 0 100 100\">\n";
    for (int i = 0; i < 6000; ++i)
        svg << "<circle cx=\"50\" cy=\"50\" r=\"" << (10.0 + (i % 800) * 0.05)
            << "\" fill=\"black\"/>\n";
    svg << "</svg>\n";

    const std::string path = tempPath("mz_test_vertex_bomb.svg");
    writeFile(path, svg.str());

    SvgPaths out;
    EXPECT_FALSE(SvgImport::load(path, out))
        << "a past-budget SVG should be refused, not imported";
    std::remove(path.c_str());
}

TEST(SvgHardening, NormalFileStillImports) {
    // Sanity guard on the budget: a plain small file is untouched by the cap.
    const std::string path = tempPath("mz_test_small.svg");
    writeFile(path,
              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" "
              "height=\"100\" viewBox=\"0 0 100 100\">"
              "<rect x=\"10\" y=\"10\" width=\"50\" height=\"40\" "
              "fill=\"black\"/>"
              "<circle cx=\"70\" cy=\"70\" r=\"15\" fill=\"black\"/>"
              "</svg>\n");

    SvgPaths out;
    EXPECT_TRUE(SvgImport::load(path, out));
    EXPECT_FALSE(out.empty());
    std::remove(path.c_str());
}
