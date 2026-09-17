// RefMeshImport reads a raw STL triangulation for display only - no sewing,
// no solid-building, no Document. Verifies it produces the vertex layout
// RefMeshRenderer::setMesh expects, applies the same Z-up->Y-up rotation as
// StlIO::import (so a reference mesh and real geometry line up), and fails
// cleanly on a missing file or a mesh with nothing usable in it.
#include <gtest/gtest.h>

#include "io/RefMeshImport.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace materializr;

namespace {

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

std::string writeStl(const std::string& body) {
    std::string stl = "solid t\n" + body + "endsolid t\n";
    std::string path = std::string(std::tmpnam(nullptr)) + ".stl";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(stl.data(), 1, stl.size(), f);
    std::fclose(f);
    return path;
}

// Axis-aligned cube [0,s]^3, 12 triangles.
std::string writeCubeStl(float s) {
    const float o = 0.0f;
    std::string body;
    writeTri(body, o,o,o, s,o,o, s,s,o); writeTri(body, o,o,o, s,s,o, o,s,o);
    writeTri(body, o,o,s, s,s,s, s,o,s); writeTri(body, o,o,s, o,s,s, s,s,s);
    writeTri(body, o,o,o, s,o,s, s,o,o); writeTri(body, o,o,o, o,o,s, s,o,s);
    writeTri(body, o,s,o, s,s,o, s,s,s); writeTri(body, o,s,o, s,s,s, o,s,s);
    writeTri(body, o,o,o, o,s,o, o,s,s); writeTri(body, o,o,o, o,s,s, o,o,s);
    writeTri(body, s,o,o, s,s,s, s,s,o); writeTri(body, s,o,o, s,o,s, s,s,s);
    return writeStl(body);
}

} // namespace

TEST(RefMeshImport, LoadsATwelveTriangleCube) {
    std::string path = writeCubeStl(10.0f);
    RefMeshLoadResult r = RefMeshImport::load(path);
    std::remove(path.c_str());

    ASSERT_TRUE(r.success) << r.errorMessage;
    EXPECT_EQ(r.triangleCount, 12);
    EXPECT_EQ(r.vertices.size(), 12u * 3u * 6u);
}

TEST(RefMeshImport, AppliesTheSameZUpToYUpRotationAsStlImport) {
    // A single facet with one vertex at raw STL (0,0,10) - Z-up "straight
    // up" - must land on the app's Y-up "straight up": (0,10,0).
    std::string body;
    writeTri(body, 0,0,10, 1,0,10, 0,1,10);
    std::string path = writeStl(body);
    RefMeshLoadResult r = RefMeshImport::load(path);
    std::remove(path.c_str());

    ASSERT_TRUE(r.success) << r.errorMessage;
    ASSERT_EQ(r.triangleCount, 1);
    // First vertex of the first (only) triangle.
    EXPECT_NEAR(r.vertices[0], 0.0f, 1e-4f);
    EXPECT_NEAR(r.vertices[1], 10.0f, 1e-4f);
    EXPECT_NEAR(r.vertices[2], 0.0f, 1e-4f);
}

TEST(RefMeshImport, FailsCleanlyOnAMissingFile) {
    RefMeshLoadResult r = RefMeshImport::load("/nonexistent/path/does_not_exist.stl");
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.errorMessage.empty());
    EXPECT_TRUE(r.vertices.empty());
}

TEST(RefMeshImport, FailsCleanlyWhenEveryTriangleIsDegenerate) {
    std::string body;
    writeTri(body, 0,0,0, 0,0,0, 0,0,0); // zero area
    std::string path = writeStl(body);
    RefMeshLoadResult r = RefMeshImport::load(path);
    std::remove(path.c_str());

    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.triangleCount, 0);
}
