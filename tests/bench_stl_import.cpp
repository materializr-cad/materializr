// Not a correctness test - a stopwatch. Generates a dense curved STL (UV
// sphere, so decimation can't just collapse coplanar facets) and times import
// at several accuracy values, with per-stage breakdown (MZR_STL_TIMING), so we
// can choose the accuracy->triangle mapping and confirm import never hangs.
// Run manually: ./bench_stl_import
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

#include "io/StlIO.h"
#include "core/Document.h"

using namespace materializr;
using clk = std::chrono::steady_clock;
static double ms(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Write a UV-sphere ASCII STL with nLat×nLong cells (~2·nLat·nLong triangles).
static std::string writeSphereStl(double r, int nLat, int nLong) {
    std::string s = "solid sphere\n";
    char buf[512];
    auto vert = [&](double lat, double lon, double& x, double& y, double& z) {
        x = r * std::sin(lat) * std::cos(lon);
        y = r * std::cos(lat);
        z = r * std::sin(lat) * std::sin(lon);
    };
    auto tri = [&](double x0,double y0,double z0,double x1,double y1,double z1,
                   double x2,double y2,double z2) {
        std::snprintf(buf, sizeof(buf),
            " facet normal 0 0 0\n  outer loop\n"
            "   vertex %g %g %g\n   vertex %g %g %g\n   vertex %g %g %g\n"
            "  endloop\n endfacet\n", x0,y0,z0, x1,y1,z1, x2,y2,z2);
        s += buf;
    };
    const double PI = 3.14159265358979323846;
    for (int i = 0; i < nLat; ++i) {
        double la0 = PI * i / nLat, la1 = PI * (i + 1) / nLat;
        for (int j = 0; j < nLong; ++j) {
            double lo0 = 2*PI * j / nLong, lo1 = 2*PI * (j + 1) / nLong;
            double a[3],b[3],c[3],d[3];
            vert(la0,lo0,a[0],a[1],a[2]); vert(la1,lo0,b[0],b[1],b[2]);
            vert(la1,lo1,c[0],c[1],c[2]); vert(la0,lo1,d[0],d[1],d[2]);
            tri(a[0],a[1],a[2], b[0],b[1],b[2], c[0],c[1],c[2]);
            tri(a[0],a[1],a[2], c[0],c[1],c[2], d[0],d[1],d[2]);
        }
    }
    s += "endsolid sphere\n";
    std::string path = std::string(std::tmpnam(nullptr)) + ".stl";
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return path;
}

int main() {
#ifdef _WIN32
    _putenv_s("MZR_STL_TIMING", "1"); // per-stage breakdown to stderr
#else
    setenv("MZR_STL_TIMING", "1", 1); // per-stage breakdown to stderr
#endif
    const int nLat = 320, nLong = 320; // ~204k triangles (a heavy real-world STL)
    std::string path = writeSphereStl(10.0, nLat, nLong);
    std::printf("Generated ~%d-triangle sphere STL\n\n", 2 * nLat * nLong);

    for (double acc : {0.0, 0.3, 0.5, 0.7, 1.0}) {
        Document doc;
        std::fprintf(stderr, "--- accuracy %.1f ---\n", acc);
        auto a = clk::now();
        ImportResult r = StlIO::import(path, doc, acc);
        auto b = clk::now();
        std::printf("accuracy %.1f : total %8.0f ms   tris %d -> %d   faces %d   %s\n",
                    acc, ms(a, b), r.trianglesBefore, r.trianglesAfter, r.faceCount,
                    r.success ? "ok" : ("FAIL: " + r.errorMessage).c_str());
        std::fflush(stdout);
    }
    std::remove(path.c_str());
    return 0;
}
