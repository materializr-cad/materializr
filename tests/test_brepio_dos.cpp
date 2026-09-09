// Regression test for the BREP length-prefix DoS: a tiny .brep file whose
// header declares an impossible section count (e.g. "Curves 999999999") made
// OCCT's BRepTools::Read spin/OOM for many seconds before the pre-scan guard
// in BrepIO::import was added. The guard rejects any section count larger than
// the file's own byte size (physically impossible for a real file) up front.
//
// If the guard regresses, ImpossibleCountIsRejectedFast doesn't just fail - it
// hangs, and CTest's per-test timeout turns that into a red build, which is the
// intended signal.

#include "io/BrepIO.h"
#include "core/Document.h"

#include <gtest/gtest.h>

#include <BRepPrimAPI_MakeBox.hxx>

#include <cstdio>
#include <fstream>
#include <string>
#include "test_tmp_path.h"

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

TEST(BrepIoDos, ImpossibleCountIsRejectedFast) {
    const std::string path = tempPath("mz_test_brep_hugecount.brep");
    // ~90 bytes, but the header claims ~1e9 curves - no real file can.
    writeFile(path,
              "DBRep_DrawableShape\n\n"
              "CASCADE Topology V3, (c) Open Cascade\n"
              "Locations 0\nCurve2ds 0\nCurves 999999999\n");

    Document doc;
    auto r = materializr::BrepIO::import(path, doc);
    std::remove(path.c_str());

    EXPECT_FALSE(r.success);
    EXPECT_TRUE(doc.getAllBodyIds().empty());
    // The message names the guard so a future refactor that drops it and lets
    // the file fail elsewhere (or hang) is visible here.
    EXPECT_NE(r.errorMessage.find("impossible"), std::string::npos)
        << "expected the count-sanity guard to fire, got: " << r.errorMessage;
}

TEST(BrepIoDos, ValidFileStillRoundTrips) {
    // Guard must not reject a legitimate file: export a real box, re-import it.
    const std::string path = tempPath("mz_test_brep_valid.brep");
    Document out;
    out.addBody(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), "box");
    ASSERT_TRUE(materializr::BrepIO::exportFile(path, out).success);

    Document in;
    auto r = materializr::BrepIO::import(path, in);
    std::remove(path.c_str());

    EXPECT_TRUE(r.success) << r.errorMessage;
    EXPECT_EQ(in.getAllBodyIds().size(), 1u);
}

TEST(BrepIoDos, TruncatedFileFailsCleanly) {
    // A believable-but-truncated file (counts within the byte size) sails past
    // the guard and is rejected by the reader itself - still no crash/hang.
    const std::string path = tempPath("mz_test_brep_truncated.brep");
    writeFile(path,
              "DBRep_DrawableShape\n\n"
              "CASCADE Topology V3, (c) Open Cascade\n"
              "Locations 0\nCurve2ds 0\nCurves 2\n1 0 0 0 0 -1 0\n");

    Document doc;
    auto r = materializr::BrepIO::import(path, doc);
    std::remove(path.c_str());

    EXPECT_FALSE(r.success);
    EXPECT_TRUE(doc.getAllBodyIds().empty());
}

// ── Truncated-section cases (the Windows SEH crash, #74-adjacent) ───────────
//
// A count that is small and plausible but larger than the data actually behind
// it is as dangerous as an absurd one: OCCT reads a table it never populated
// and dereferences the garbage. On Linux OSD's signal translation catches it;
// on Windows it is an SEH access violation that KILLS THE PROCESS, verified on
// CI with the app's own OSD::SetSignal installed. It also faults only
// sometimes, so a passing run proves nothing without the guard.
//
// Each of these must be refused by brepHeaderCountsSane before the kernel
// reader ever opens the file.
namespace {
const char* kHdr =
    "DBRep_DrawableShape\n\n"
    "CASCADE Topology V3, (c) Open Cascade\n";
}

TEST(BrepIoDos, TruncatedSectionsAreRefused) {
    // `preScan` marks the files whose declared counts outrun the data behind
    // them - those MUST be refused before the kernel reader opens them, because
    // that is the path that faults. The others are merely malformed: the reader
    // rejects them cleanly on both platforms, which is a fine outcome and not
    // something the guard needs to duplicate.
    struct Case { const char* name; const char* body; bool preScan; };
    const Case cases[] = {
        // Declares 2 curves with 1 line left. This is the file that fails CI.
        {"curves2_one_partial", "Locations 0\nCurve2ds 0\nCurves 2\n1 0 0 0 0 -1 0\n", true},
        // Declares 1 curve with nothing after it.
        {"curves1_none",        "Locations 0\nCurve2ds 0\nCurves 1\n", true},
        // Declares 1 curve and has 1 (partial) line for it - the count is
        // satisfiable, so the guard lets it through and the reader says no.
        {"curves1_half_record", "Locations 0\nCurve2ds 0\nCurves 1\n1 0 0 0 0 ", false},
        // Declares 3 shapes with an empty table. THIS is the one that killed
        // the process on Windows with an SEH access violation.
        {"tshapes_empty",
         "Locations 0\nCurve2ds 0\nCurves 0\nPolygon3D 0\n"
         "PolygonOnTriangulations 0\nSurfaces 0\nTriangulations 0\nTShapes 3\n", true},
    };

    for (const Case& c : cases) {
        const std::string path = tempPath("mz_test_brep_trunc.brep");
        writeFile(path, std::string(kHdr) + c.body);

        Document doc;
        auto r = materializr::BrepIO::import(path, doc);
        std::remove(path.c_str());

        EXPECT_FALSE(r.success) << c.name << " was accepted";
        EXPECT_TRUE(doc.getAllBodyIds().empty()) << c.name << " added bodies";
        if (c.preScan) {
            // Refused by the pre-scan, not merely survived by the reader - on
            // Windows "survived" is a coin flip.
            EXPECT_NE(r.errorMessage.find("truncated"), std::string::npos)
                << c.name << " reached the kernel reader: " << r.errorMessage;
        }
    }
}

// The guard must not cost a legitimate file. A real export, with every section
// count matching real records, still round-trips - this is the check that
// would catch an over-eager line bound.
TEST(BrepIoDos, RealExportSurvivesTheTruncationGuard) {
    const std::string path = tempPath("mz_test_brep_guard_roundtrip.brep");
    Document out;
    out.addBody(BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), "box");
    ASSERT_TRUE(materializr::BrepIO::exportFile(path, out).success);

    Document back;
    auto r = materializr::BrepIO::import(path, back);
    std::remove(path.c_str());

    EXPECT_TRUE(r.success) << "the guard rejected a valid file: " << r.errorMessage;
    EXPECT_EQ(back.getAllBodyIds().size(), 1u);
}
