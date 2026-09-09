// Exchange formats round 1 (#41 BREP in/out, #42 OBJ export, #43 DXF sketch
// export, #44 3MF export). BREP is verified by geometric round-trip; the
// mesh/drawing exporters by structural parses of what they wrote - every
// numeric claim is checked against the source document, not just "file
// exists".
#include <gtest/gtest.h>

#include "../src/core/Document.h"
#include "../src/io/BrepIO.h"
#include "../src/io/DxfExport.h"
#include "../src/io/DxfImport.h"
#include "../src/io/IgesIO.h"
#include "../src/io/ObjExport.h"
#include "../src/io/ThreeMfExport.h"
#include "../src/modeling/Sketch.h"

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESControl_Reader.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax2.hxx>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace materializr;

namespace {

std::string tmpPath(const char* name) {
    return std::string(::testing::TempDir()) + name;
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Document twoBodyDoc() {
    Document doc;
    doc.addBody(BRepPrimAPI_MakeBox(20.0, 30.0, 40.0).Shape(), "Box");
    doc.addBody(BRepPrimAPI_MakeCylinder(
                    gp_Ax2(gp_Pnt(100.0, 0.0, 0.0), gp_Dir(0.0, 1.0, 0.0)),
                    10.0, 25.0).Shape(),
                "Cylinder");
    return doc;
}

} // namespace

// ── BREP: exact round-trip ──────────────────────────────────────────────────

TEST(BrepIO, RoundTripsTwoBodiesWithExactVolumes) {
    Document doc = twoBodyDoc();
    const std::string path = tmpPath("exchange.brep");

    auto ex = BrepIO::exportFile(path, doc);
    ASSERT_TRUE(ex.success) << ex.errorMessage;

    Document back;
    auto im = BrepIO::import(path, back);
    ASSERT_TRUE(im.success) << im.errorMessage;
    EXPECT_EQ(im.bodiesImported, 2);
    ASSERT_EQ(back.getAllBodyIds().size(), 2u);

    // Volumes survive exactly (BREP is lossless; the Y/Z-up rotations cancel).
    std::vector<double> want = {20.0 * 30.0 * 40.0, M_PI * 10.0 * 10.0 * 25.0};
    std::vector<double> got;
    for (int id : back.getAllBodyIds()) got.push_back(volumeOf(back.getBody(id)));
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    for (size_t i = 0; i < want.size(); ++i)
        EXPECT_NEAR(got[i], want[i], want[i] * 1e-9);
}

TEST(BrepIO, DiskFileIsZUpPerTheStepConvention) {
    // A self-round-trip can't see a rotation-sign error (the pair inverts
    // itself), so read the exported file RAW - no import rotation - and
    // assert the disk axes: scene box 20(X)×30(Y-up)×40(Z) must land with
    // its height on disk-Z (extent 30) and scene-Z on disk −Y (extent 40,
    // span [−40, 0]) - exactly what StepIO/StlExport write. Caught #45.
    Document doc;
    doc.addBody(BRepPrimAPI_MakeBox(20.0, 30.0, 40.0).Shape(), "Box");
    const std::string path = tmpPath("axes.brep");
    ASSERT_TRUE(BrepIO::exportFile(path, doc).success);

    TopoDS_Shape raw;
    BRep_Builder bb;
    ASSERT_TRUE(BRepTools::Read(raw, path.c_str(), bb));
    Bnd_Box box;
    BRepBndLib::Add(raw, box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x1 - x0, 20.0, 1e-6);
    EXPECT_NEAR(z1 - z0, 30.0, 1e-6);  // scene Y (up) → disk Z (up)
    EXPECT_NEAR(z0, 0.0, 1e-6);        // part sits ON the disk floor
    EXPECT_NEAR(y0, -40.0, 1e-6);      // scene Z → disk −Y
    EXPECT_NEAR(y1, 0.0, 1e-6);
}

TEST(BrepIO, SingleBodyExportsBareAndReimports) {
    Document doc;
    doc.addBody(BRepPrimAPI_MakeBox(5.0, 5.0, 5.0).Shape(), "Cube");
    const std::string path = tmpPath("single.brep");
    ASSERT_TRUE(BrepIO::exportFile(path, doc).success);

    Document back;
    auto im = BrepIO::import(path, back);
    ASSERT_TRUE(im.success) << im.errorMessage;
    EXPECT_EQ(im.bodiesImported, 1);
    EXPECT_NEAR(volumeOf(back.getBody(back.getAllBodyIds().front())), 125.0, 1e-6);
}

TEST(BrepIO, ImportRejectsGarbage) {
    const std::string path = tmpPath("garbage.brep");
    { std::ofstream(path) << "this is not a brep file\n"; }
    Document doc;
    EXPECT_FALSE(BrepIO::import(path, doc).success);
    EXPECT_TRUE(doc.getAllBodyIds().empty());
}

// ── IGES: round-trip + disk convention (axis fix, #46) ──────────────────────

TEST(IgesIO, RoundTripRestoresSceneOrientation) {
    // IGES translates solids as surface collections by default, so volume is
    // not a meaningful invariant - the scene-space BOUNDING BOX is: it proves
    // the export and import rotations compose to identity (the actual #46
    // fix) and the size survives.
    Document doc;
    doc.addBody(BRepPrimAPI_MakeBox(20.0, 30.0, 40.0).Shape(), "Box");
    const std::string path = tmpPath("exchange.iges");
    ASSERT_TRUE(IgesIO::exportFile(path, doc).success);

    Document back;
    auto im = IgesIO::import(path, back);
    ASSERT_TRUE(im.success) << im.errorMessage;
    ASSERT_GE(back.getAllBodyIds().size(), 1u);

    Bnd_Box box;
    for (int id : back.getAllBodyIds()) BRepBndLib::Add(back.getBody(id), box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x1 - x0, 20.0, 1e-4);
    EXPECT_NEAR(y1 - y0, 30.0, 1e-4);  // height back on scene Y
    EXPECT_NEAR(z1 - z0, 40.0, 1e-4);
    EXPECT_NEAR(y0, 0.0, 1e-4);        // and sitting on the scene floor
    EXPECT_NEAR(z0, 0.0, 1e-4);        // (a sign error would put z in [-40,0])
}

TEST(IgesIO, DiskFileIsZUpPerTheStepConvention) {
    // Same raw-read pattern as the BREP twin (#45): read the file with the
    // bare OCCT reader - none of our import rotation - and assert disk axes.
    Document doc;
    doc.addBody(BRepPrimAPI_MakeBox(20.0, 30.0, 40.0).Shape(), "Box");
    const std::string path = tmpPath("axes.iges");
    ASSERT_TRUE(IgesIO::exportFile(path, doc).success);

    IGESControl_Reader reader;
    ASSERT_EQ(reader.ReadFile(path.c_str()), IFSelect_RetDone);
    reader.TransferRoots();
    TopoDS_Shape raw = reader.OneShape();
    ASSERT_FALSE(raw.IsNull());
    Bnd_Box box;
    BRepBndLib::Add(raw, box);
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    EXPECT_NEAR(x1 - x0, 20.0, 1e-4);
    EXPECT_NEAR(z1 - z0, 30.0, 1e-4);  // scene Y (up) → disk Z (up)
    EXPECT_NEAR(y0, -40.0, 1e-4);      // scene Z → disk −Y
}

// ── OBJ: structural parse of the written mesh ───────────────────────────────

TEST(ObjExport, WritesIndexedMeshWithValidReferences) {
    Document doc = twoBodyDoc();
    const std::string path = tmpPath("exchange.obj");
    auto ex = ObjExport::exportFile(path, doc);
    ASSERT_TRUE(ex.success) << ex.errorMessage;

    std::ifstream in(path);
    ASSERT_TRUE(in.good());
    long vCount = 0, fCount = 0, oCount = 0;
    long maxIndex = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("v ", 0) == 0) ++vCount;
        else if (line.rfind("o ", 0) == 0) ++oCount;
        else if (line.rfind("f ", 0) == 0) {
            ++fCount;
            long a = 0, b = 0, c = 0;
            ASSERT_EQ(std::sscanf(line.c_str(), "f %ld %ld %ld", &a, &b, &c), 3)
                << "unparsable face line: " << line;
            EXPECT_GE(a, 1); EXPECT_GE(b, 1); EXPECT_GE(c, 1);
            maxIndex = std::max({maxIndex, a, b, c});
        }
    }
    EXPECT_EQ(oCount, 2);            // one group per body
    EXPECT_GE(vCount, 8);            // at least the box's corners
    EXPECT_GE(fCount, 12);           // at least the box's triangles
    EXPECT_LE(maxIndex, vCount);     // every face references a real vertex
}

TEST(ObjExport, SkipsHiddenBodies) {
    Document doc = twoBodyDoc();
    auto ids = doc.getAllBodyIds();
    doc.setBodyVisible(ids.front(), false);
    const std::string path = tmpPath("hidden.obj");
    ASSERT_TRUE(ObjExport::exportFile(path, doc).success);
    const std::string text = slurp(path);
    long oCount = 0;
    for (size_t p = text.find("\no "); p != std::string::npos;
         p = text.find("\no ", p + 1))
        ++oCount;
    EXPECT_EQ(oCount, 1);
}

// ── DXF: entity-level parse against the source sketch ───────────────────────

TEST(DxfExport, WritesAllEntityKindsWithExactValues) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({50.0f, 0.0f});
    sk.addLine(a, b);
    int c = sk.addPoint({10.0f, 20.0f});
    sk.addCircle(c, 7.5);
    // CCW quarter arc: centre origin-ish, start +X, end +Y.
    int ac = sk.addPoint({-30.0f, 0.0f});
    int as = sk.addPoint({-20.0f, 0.0f});
    int ae = sk.addPoint({-30.0f, 10.0f});
    sk.addArc(ac, as, ae, 10.0);

    const std::string path = tmpPath("sketch.dxf");
    auto ex = DxfExport::exportSketch(path, sk);
    ASSERT_TRUE(ex.success) << ex.errorMessage;
    EXPECT_EQ(ex.entityCount, 3);

    const std::string text = slurp(path);
    EXPECT_NE(text.find("$ACADVER"), std::string::npos);
    EXPECT_NE(text.find("AC1009"), std::string::npos);
    EXPECT_NE(text.find("\nLINE\n"), std::string::npos);
    EXPECT_NE(text.find("\nCIRCLE\n"), std::string::npos);
    EXPECT_NE(text.find("\nARC\n"), std::string::npos);
    EXPECT_NE(text.find("\nEOF\n"), std::string::npos);
    // Circle radius written under group 40.
    EXPECT_NE(text.find("40\n7.500000"), std::string::npos);
    // Arc angles: group 50 (start) ≈ 0°, group 51 (end) ≈ 90°. Parsed, not
    // string-matched - the sketch stores points as float vec2 and addArc
    // re-derives the end point through float trig, so the written angle is
    // 90-ish to ~1e-5° (observed 90.000003), which is exactly right for the
    // exporter to pass through.
    auto groupAfterArc = [&](const char* code) -> double {
        size_t arc = text.find("\nARC\n");
        EXPECT_NE(arc, std::string::npos);
        size_t pos = text.find(std::string("\n") + code + "\n", arc);
        EXPECT_NE(pos, std::string::npos);
        return std::atof(text.c_str() + pos + 1 + std::strlen(code) + 1);
    };
    EXPECT_NEAR(groupAfterArc("50"), 0.0, 1e-3);
    EXPECT_NEAR(groupAfterArc("51"), 90.0, 1e-3);
}

TEST(DxfExport, RejectsSketchWithNoGeometry) {
    Sketch sk;
    sk.addPoint({0.0f, 0.0f}); // a bare point is not a cuttable entity
    const std::string path = tmpPath("empty.dxf");
    EXPECT_FALSE(DxfExport::exportSketch(path, sk).success);
}

// ── DXF import (#47): round-trip, units, bulge arcs, tolerance ──────────────

TEST(DxfImport, RoundTripsOurOwnExport) {
    Sketch src;
    int a = src.addPoint({-25.0f, -10.0f});
    int b = src.addPoint({25.0f, -10.0f});
    src.addLine(a, b);
    int c = src.addPoint({0.0f, 5.0f});
    src.addCircle(c, 7.5);
    int ac = src.addPoint({0.0f, -10.0f});
    int as = src.addPoint({10.0f, -10.0f});
    int ae = src.addPoint({0.0f, 0.0f});
    src.addArc(ac, as, ae, 10.0);

    const std::string path = tmpPath("roundtrip.dxf");
    ASSERT_TRUE(DxfExport::exportSketch(path, src).success);

    Sketch back;
    auto im = DxfImport::importFile(path, back);
    ASSERT_TRUE(im.success) << im.errorMessage;
    EXPECT_EQ(im.entityCount, 3);
    ASSERT_EQ(back.getLines().size(), 1u);
    ASSERT_EQ(back.getCircles().size(), 1u);
    ASSERT_EQ(back.getArcs().size(), 1u);

    // Dimensions survive exactly (import recentres, so compare extents).
    const auto& l = back.getLines().front();
    glm::vec2 la = back.getPoint(l.startPointId)->pos;
    glm::vec2 lb = back.getPoint(l.endPointId)->pos;
    EXPECT_NEAR(glm::length(lb - la), 50.0f, 1e-3f);
    EXPECT_NEAR(back.getCircles().front().radius, 7.5, 1e-6);
    EXPECT_NEAR(back.getArcs().front().radius, 10.0, 1e-4);
}

TEST(DxfImport, HonorsInchUnits) {
    // Hand-authored R12: $INSUNITS=1 (inches), one 1-inch line.
    const std::string path = tmpPath("inches.dxf");
    {
        std::ofstream f(path);
        f << "0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n1\n0\nENDSEC\n"
             "0\nSECTION\n2\nENTITIES\n"
             "0\nLINE\n8\n0\n10\n0.0\n20\n0.0\n11\n1.0\n21\n0.0\n"
             "0\nENDSEC\n0\nEOF\n";
    }
    Sketch sk;
    auto im = DxfImport::importFile(path, sk);
    ASSERT_TRUE(im.success) << im.errorMessage;
    ASSERT_EQ(sk.getLines().size(), 1u);
    const auto& l = sk.getLines().front();
    glm::vec2 a = sk.getPoint(l.startPointId)->pos;
    glm::vec2 b = sk.getPoint(l.endPointId)->pos;
    EXPECT_NEAR(glm::length(b - a), 25.4f, 1e-4f); // 1 in → 25.4 mm
}

TEST(DxfImport, LwpolylineBulgeBecomesArc) {
    // Closed LWPOLYLINE: bottom edge is a straight segment, the return
    // segment carries bulge=1 → a CCW semicircle of radius 10.
    const std::string path = tmpPath("bulge.dxf");
    {
        std::ofstream f(path);
        f << "0\nSECTION\n2\nENTITIES\n"
             "0\nLWPOLYLINE\n8\n0\n90\n2\n70\n1\n"
             "10\n-10.0\n20\n0.0\n"          // vertex 1 (straight to v2)
             "10\n10.0\n20\n0.0\n42\n1.0\n"  // vertex 2, bulge 1 back to v1
             "0\nENDSEC\n0\nEOF\n";
    }
    Sketch sk;
    auto im = DxfImport::importFile(path, sk);
    ASSERT_TRUE(im.success) << im.errorMessage;
    ASSERT_EQ(sk.getLines().size(), 1u);
    ASSERT_EQ(sk.getArcs().size(), 1u);
    EXPECT_NEAR(sk.getArcs().front().radius, 10.0, 1e-4);
}

TEST(DxfImport, SkipsUnsupportedEntitiesWithoutFailing) {
    const std::string path = tmpPath("mixed.dxf");
    {
        std::ofstream f(path);
        f << "0\nSECTION\n2\nENTITIES\n"
             "0\nTEXT\n8\n0\n10\n0\n20\n0\n40\n2.5\n1\nhello\n"
             "0\nCIRCLE\n8\n0\n10\n0.0\n20\n0.0\n40\n5.0\n"
             "0\nENDSEC\n0\nEOF\n";
    }
    Sketch sk;
    auto im = DxfImport::importFile(path, sk);
    ASSERT_TRUE(im.success) << im.errorMessage;
    EXPECT_EQ(im.entityCount, 1);
    EXPECT_EQ(im.skippedCount, 1);
    ASSERT_EQ(sk.getCircles().size(), 1u);
    EXPECT_NEAR(sk.getCircles().front().radius, 5.0, 1e-9);
}

TEST(DxfImport, RejectsEmptyOrGarbage) {
    const std::string path = tmpPath("garbage.dxf");
    { std::ofstream(path) << "not a dxf at all\n"; }
    Sketch sk;
    EXPECT_FALSE(DxfImport::importFile(path, sk).success);
}

// ── 3MF: container + model structure ────────────────────────────────────────

TEST(ThreeMfExport, WritesValidZipContainerWithModel) {
    Document doc = twoBodyDoc();
    const std::string path = tmpPath("exchange.3mf");
    auto ex = ThreeMfExport::exportFile(path, doc);
    ASSERT_TRUE(ex.success) << ex.errorMessage;

    const std::string zip = slurp(path);
    ASSERT_GT(zip.size(), 100u);
    // ZIP magic, end-of-central-directory record, and the three OPC parts.
    EXPECT_EQ(zip.compare(0, 4, "PK\x03\x04"), 0);
    EXPECT_NE(zip.find("PK\x05\x06"), std::string::npos);
    EXPECT_NE(zip.find("[Content_Types].xml"), std::string::npos);
    EXPECT_NE(zip.find("_rels/.rels"), std::string::npos);
    EXPECT_NE(zip.find("3D/3dmodel.model"), std::string::npos);

    // Model payload (stored entries → readable in place).
    EXPECT_NE(zip.find("unit=\"millimeter\""), std::string::npos);
    EXPECT_NE(zip.find("<object id=\"1\""), std::string::npos);
    EXPECT_NE(zip.find("<object id=\"2\""), std::string::npos);
    EXPECT_NE(zip.find("<triangle "), std::string::npos);
    EXPECT_NE(zip.find("<vertex "), std::string::npos);

    // EOCD says exactly 3 entries.
    size_t eocd = zip.rfind("PK\x05\x06");
    ASSERT_NE(eocd, std::string::npos);
    uint16_t entries;
    std::memcpy(&entries, zip.data() + eocd + 10, 2);
    EXPECT_EQ(entries, 3);
}

TEST(ThreeMfExport, FailsCleanlyWithNoBodies) {
    Document doc;
    const std::string path = tmpPath("empty.3mf");
    EXPECT_FALSE(ThreeMfExport::exportFile(path, doc).success);
}
