// Verifies StlIO::import turns a triangle-soup STL into a clean solid:
//  - a 12-triangle binary cube imports as one body,
//  - sewing closes it into a SOLID,
//  - ShapeUpgrade_UnifySameDomain merges the 12 facets back to 6 planar faces,
//  - winding correction yields a positive volume.
// (The STL import menu entry is parked/disabled - see StlImportPlugin - but the
//  importer itself is exercised here so the parked code keeps building/working.)
#include <gtest/gtest.h>

#include "io/StlIO.h"
#include "io/ProjectIO.h"
#include "core/Document.h"

#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Shape.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cstdio>
#include <string>

using namespace materializr;

namespace {

// Append one ASCII-STL facet (the normal is ignored on read, so we emit zeros).
void writeTri(std::string& s,
              float ax, float ay, float az,
              float bx, float by, float bz,
              float cx, float cy, float cz) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        " facet normal 0 0 0\n"
        "  outer loop\n"
        "   vertex %g %g %g\n"
        "   vertex %g %g %g\n"
        "   vertex %g %g %g\n"
        "  endloop\n"
        " endfacet\n",
        ax, ay, az, bx, by, bz, cx, cy, cz);
    s += buf;
}

// Write an ASCII STL of an axis-aligned cube [0,s]^3 (12 triangles) to a temp file.
std::string writeCubeStl(float s) {
    const float o = 0.0f;
    std::string stl = "solid cube\n";
    writeTri(stl, o,o,o, s,o,o, s,s,o); writeTri(stl, o,o,o, s,s,o, o,s,o); // z=o
    writeTri(stl, o,o,s, s,s,s, s,o,s); writeTri(stl, o,o,s, o,s,s, s,s,s); // z=s
    writeTri(stl, o,o,o, s,o,s, s,o,o); writeTri(stl, o,o,o, o,o,s, s,o,s); // y=o
    writeTri(stl, o,s,o, s,s,o, s,s,s); writeTri(stl, o,s,o, s,s,s, o,s,s); // y=s
    writeTri(stl, o,o,o, o,s,o, o,s,s); writeTri(stl, o,o,o, o,s,s, o,o,s); // x=o
    writeTri(stl, s,o,o, s,s,s, s,s,o); writeTri(stl, s,o,o, s,o,s, s,s,s); // x=s
    stl += "endsolid cube\n";

    std::string path = std::string(std::tmpnam(nullptr)) + ".stl";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(stl.data(), 1, stl.size(), f);
    std::fclose(f);
    return path;
}

// Emit a planar quad (origin o, spanning vectors u,v over the full edge),
// subdivided into an n×n grid of triangles, into an ASCII-STL string.
void writeGridFace(std::string& s, int n,
                   float ox, float oy, float oz,
                   float ux, float uy, float uz,
                   float vx, float vy, float vz) {
    auto P = [&](float a, float b, float& x, float& y, float& z) {
        x = ox + a*ux + b*vx; y = oy + a*uy + b*vy; z = oz + a*uz + b*vz;
    };
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const float a0=float(i)/n, a1=float(i+1)/n, b0=float(j)/n, b1=float(j+1)/n;
            float x00,y00,z00, x10,y10,z10, x01,y01,z01, x11,y11,z11;
            P(a0,b0,x00,y00,z00); P(a1,b0,x10,y10,z10);
            P(a0,b1,x01,y01,z01); P(a1,b1,x11,y11,z11);
            writeTri(s, x00,y00,z00, x10,y10,z10, x11,y11,z11);
            writeTri(s, x00,y00,z00, x11,y11,z11, x01,y01,z01);
        }
}

// A cube [0,s]^3 whose six faces are each subdivided into an n×n grid.
std::string writeSubdividedCubeStl(float s, int n) {
    std::string stl = "solid cube\n";
    writeGridFace(stl, n, 0,0,0,  s,0,0,  0,s,0); // z=0
    writeGridFace(stl, n, 0,0,s,  s,0,0,  0,s,0); // z=s
    writeGridFace(stl, n, 0,0,0,  s,0,0,  0,0,s); // y=0
    writeGridFace(stl, n, 0,s,0,  s,0,0,  0,0,s); // y=s
    writeGridFace(stl, n, 0,0,0,  0,s,0,  0,0,s); // x=0
    writeGridFace(stl, n, s,0,0,  0,s,0,  0,0,s); // x=s
    stl += "endsolid cube\n";
    std::string path = std::string(std::tmpnam(nullptr)) + ".stl";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(stl.data(), 1, stl.size(), f);
    std::fclose(f);
    return path;
}

int countSubShapes(const TopoDS_Shape& s, TopAbs_ShapeEnum kind) {
    int n = 0;
    for (TopExp_Explorer ex(s, kind); ex.More(); ex.Next()) ++n;
    return n;
}

// A cube with INCONSISTENT winding: the +X face's two triangles are wound the
// opposite way from the rest. The importer's orientation pass must reconcile
// this into a valid solid.
std::string writeCubeStlMixedWinding(float s) {
    const float o = 0.0f;
    std::string stl = "solid cube\n";
    writeTri(stl, o,o,o, s,o,o, s,s,o); writeTri(stl, o,o,o, s,s,o, o,s,o); // z=o
    writeTri(stl, o,o,s, s,s,s, s,o,s); writeTri(stl, o,o,s, o,s,s, s,s,s); // z=s
    writeTri(stl, o,o,o, s,o,s, s,o,o); writeTri(stl, o,o,o, o,o,s, s,o,s); // y=o
    writeTri(stl, o,s,o, s,s,o, s,s,s); writeTri(stl, o,s,o, s,s,s, o,s,s); // y=s
    writeTri(stl, o,o,o, o,s,o, o,s,s); writeTri(stl, o,o,o, o,s,s, o,o,s); // x=o
    // x=s, winding reversed vs the others (swap last two verts of each tri):
    writeTri(stl, s,o,o, s,s,o, s,s,s); writeTri(stl, s,o,o, s,s,s, s,o,s);
    stl += "endsolid cube\n";
    std::string path = std::string(std::tmpnam(nullptr)) + ".stl";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(stl.data(), 1, stl.size(), f);
    std::fclose(f);
    return path;
}

// An open box: the cube with its top (z=s) face omitted - not watertight.
std::string writeOpenBoxStl(float s) {
    const float o = 0.0f;
    std::string stl = "solid open\n";
    writeTri(stl, o,o,o, s,o,o, s,s,o); writeTri(stl, o,o,o, s,s,o, o,s,o); // z=o
    writeTri(stl, o,o,o, s,o,s, s,o,o); writeTri(stl, o,o,o, o,o,s, s,o,s); // y=o
    writeTri(stl, o,s,o, s,s,o, s,s,s); writeTri(stl, o,s,o, s,s,s, o,s,s); // y=s
    writeTri(stl, o,o,o, o,s,o, o,s,s); writeTri(stl, o,o,o, o,s,s, o,o,s); // x=o
    writeTri(stl, s,o,o, s,s,s, s,s,o); writeTri(stl, s,o,o, s,o,s, s,s,s); // x=s
    stl += "endsolid open\n";                                              // (no z=s)
    std::string path = std::string(std::tmpnam(nullptr)) + ".stl";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(stl.data(), 1, stl.size(), f);
    std::fclose(f);
    return path;
}

} // namespace

TEST(StlImport, CubeBecomesSolidWithSixFaces) {
    const float s = 10.0f;
    std::string path = writeCubeStl(s);

    Document doc;
    ImportResult res = StlIO::import(path, doc);
    std::remove(path.c_str());

    ASSERT_TRUE(res.success) << res.errorMessage;
    EXPECT_EQ(res.bodiesImported, 1);

    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 1u);

    const TopoDS_Shape& body = doc.getBody(ids[0]);
    ASSERT_FALSE(body.IsNull());

    // Sewing should have produced exactly one solid.
    EXPECT_EQ(countSubShapes(body, TopAbs_SOLID), 1);
    // UnifySameDomain should collapse the 12 coplanar facets to 6 cube faces.
    EXPECT_EQ(countSubShapes(body, TopAbs_FACE), 6);

    // Volume should be s^3 (winding correction makes it positive).
    GProp_GProps props;
    BRepGProp::VolumeProperties(body, props);
    EXPECT_NEAR(props.Mass(), static_cast<double>(s) * s * s, 1e-3);

    // The imported body is tagged as a mesh.
    EXPECT_TRUE(doc.isBodyMesh(ids[0]));
}

TEST(StlImport, AccuracyMergesFlatRegions) {
    // A subdivided cube is piecewise-planar: at the default accuracy the angular
    // merge collapses every face's grid back to ONE planar face, while the
    // direct topology builder keeps it a valid solid with the right volume.
    const float s = 10.0f;
    const int n = 10; // 6*2*100 = 1200 triangles
    std::string path = writeSubdividedCubeStl(s, n);

    Document doc;
    ImportResult res = StlIO::import(path, doc, /*accuracy=*/0.5);
    std::remove(path.c_str());

    ASSERT_TRUE(res.success) << res.errorMessage;
    EXPECT_EQ(res.trianglesBefore, 6 * 2 * n * n);

    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 1u);
    const TopoDS_Shape& body = doc.getBody(ids[0]);
    EXPECT_EQ(countSubShapes(body, TopAbs_SOLID), 1);
    EXPECT_EQ(countSubShapes(body, TopAbs_FACE), 6);  // grid merged back to 6 faces
    GProp_GProps props;
    BRepGProp::VolumeProperties(body, props);
    EXPECT_NEAR(props.Mass(), static_cast<double>(s) * s * s, 1e-3);
}

TEST(StlImport, InconsistentWindingReconciledToSolid) {
    const float s = 10.0f;
    std::string path = writeCubeStlMixedWinding(s);
    Document doc;
    ImportResult res = StlIO::import(path, doc, /*accuracy=*/0.5);
    std::remove(path.c_str());

    ASSERT_TRUE(res.success) << res.errorMessage;
    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 1u);
    const TopoDS_Shape& body = doc.getBody(ids[0]);
    // Orientation pass makes the flipped face agree → a valid closed solid.
    EXPECT_EQ(countSubShapes(body, TopAbs_SOLID), 1);
    EXPECT_EQ(countSubShapes(body, TopAbs_FACE), 6);
    GProp_GProps props;
    BRepGProp::VolumeProperties(body, props);
    EXPECT_NEAR(props.Mass(), static_cast<double>(s) * s * s, 1e-3);
}

TEST(StlImport, OpenMeshImportsAsShellNotSolid) {
    const float s = 10.0f;
    std::string path = writeOpenBoxStl(s);
    Document doc;
    ImportResult res = StlIO::import(path, doc, /*accuracy=*/0.5);
    std::remove(path.c_str());

    ASSERT_TRUE(res.success) << res.errorMessage; // imports, does not crash
    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 1u);
    const TopoDS_Shape& body = doc.getBody(ids[0]);
    ASSERT_FALSE(body.IsNull());
    // Not watertight → kept as a shell (5 merged faces), NOT wrapped as a solid.
    EXPECT_EQ(countSubShapes(body, TopAbs_SOLID), 0);
    EXPECT_EQ(countSubShapes(body, TopAbs_FACE), 5);
}

TEST(StlImport, MeshFlagSurvivesSaveLoad) {
    const float s = 10.0f;
    std::string stl = writeCubeStl(s);
    Document doc;
    ASSERT_TRUE(StlIO::import(stl, doc).success);
    std::remove(stl.c_str());
    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 1u);
    ASSERT_TRUE(doc.isBodyMesh(ids[0]));

    std::string proj = std::string(std::tmpnam(nullptr)) + ".mzr";
    ASSERT_TRUE(ProjectIO::save(proj, doc).success);

    Document loaded;
    ASSERT_TRUE(ProjectIO::load(proj, loaded).success);
    std::remove(proj.c_str());

    auto lids = loaded.getAllBodyIds();
    ASSERT_EQ(lids.size(), 1u);
    EXPECT_TRUE(loaded.isBodyMesh(lids[0])); // flag round-tripped via BODY_MESH
}
