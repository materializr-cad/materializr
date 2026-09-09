// Single-shot shell probe: ONE (thickness, join) attempt against a project's
// body, printed with timing, then exit. Run under `timeout` in a shell loop so
// a hang in one combo can't poison the rest - this maps the failure band and
// tells us whether the intersection (sharp-corner) join is viable near the
// fillet radius or hangs there.
//
// Usage: probe_shell <project> <thickness> <arc|inter>

#include "core/Document.h"
#include "io/ProjectIO.h"

#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffset_Mode.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Vec.hxx>
#include <Standard_ErrorHandler.hxx>
#include <OSD.hxx>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>

using materializr::ProjectIO;
using materializr::ProjectHistory;

static TopoDS_Face bottomFace(const TopoDS_Shape& body) {
    TopoDS_Face best; double bestDot = 1e9;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        try {
            BRepGProp_Face gf(f);
            Standard_Real u0, u1, v0, v1; gf.Bounds(u0, u1, v0, v1);
            gp_Pnt p; gp_Vec n; gf.Normal((u0+u1)/2, (v0+v1)/2, p, n);
            if (n.Magnitude() < 1e-9) continue;
            n.Normalize();
            if (n.Y() < bestDot) { bestDot = n.Y(); best = f; }
        } catch (...) {}
    }
    return best;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s <project> <thickness> <arc|inter> [tol]\n", argv[0]); return 2; }
    OSD::SetSignal(Standard_False);
    double t = std::atof(argv[2]);
    bool inter = std::strcmp(argv[3], "inter") == 0;
    double tol = argc >= 5 ? std::atof(argv[4]) : 1.0e-3;
    GeomAbs_JoinType join = inter ? GeomAbs_Intersection : GeomAbs_Arc;

    Document doc;
    ProjectHistory hist;
    if (!ProjectIO::load(argv[1], doc, &hist).success) { std::fprintf(stderr, "load failed\n"); return 1; }
    int id = doc.getAllBodyIds().front();
    TopoDS_Shape body = doc.getBody(id);

    // radii mode: histogram of rounded-face radii - a doubled count of the same
    // radius exposes a fillet stacked on itself by a bad re-derivation.
    if (std::strcmp(argv[2], "radii") == 0) {
        int total = 0, plane = 0, cyl = 0, tor = 0;
        std::map<int,int> buckets; // radius*10 -> count
        for (TopExp_Explorer e(body, TopAbs_FACE); e.More(); e.Next(), ++total) {
            try {
                BRepAdaptor_Surface s(TopoDS::Face(e.Current()));
                if (s.GetType() == GeomAbs_Cylinder) { ++cyl; buckets[(int)(s.Cylinder().Radius()*10+0.5)]++; }
                else if (s.GetType() == GeomAbs_Torus) { ++tor; buckets[(int)(s.Torus().MinorRadius()*10+0.5)]++; }
                else if (s.GetType() == GeomAbs_Plane) ++plane;
            } catch (...) {}
        }
        GProp_GProps gv; BRepGProp::VolumeProperties(body, gv);
        Bnd_Box bb; BRepBndLib::Add(body, bb);
        double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
        double bbvol=(x1-x0)*(y1-y0)*(z1-z0);
        std::printf("total=%d plane=%d cyl=%d torus=%d  volume=%.0f bbox=%.0f fill=%.1f%%\n", total, plane, cyl, tor, gv.Mass(), bbvol, 100.0*gv.Mass()/bbvol);
        for (auto& kv : buckets) std::printf("   radius %.1fmm : %d faces\n", kv.first/10.0, kv.second);
        return 0;
    }

    // scan mode: try EVERY face as the open face at this thickness/join.
    if (std::strcmp(argv[2], "scan") == 0) {
        double st = std::atof(argv[3]);
        bool si = std::strcmp(argv[4], "inter") == 0;
        GeomAbs_JoinType sj = si ? GeomAbs_Intersection : GeomAbs_Arc;
        int fi = 0, okCount = 0;
        for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next(), ++fi) {
            TopTools_ListOfShape fl; fl.Append(TopoDS::Face(ex.Current()));
            const char* verdict = "??";
            try {
                OCC_CATCH_SIGNALS
                BRepOffsetAPI_MakeThickSolid mk;
                mk.MakeThickSolidByJoin(body, fl, -st, 1.0e-3, BRepOffset_Skin,
                                        si ? Standard_True : Standard_False,
                                        Standard_False, sj);
                mk.Build();
                if (!mk.IsDone()) verdict = "NOTDONE";
                else if (mk.Shape().IsNull()) verdict = "NULL";
                else if (!BRepCheck_Analyzer(mk.Shape()).IsValid()) verdict = "INVALID";
                else { verdict = "OK"; ++okCount; }
            } catch (...) { verdict = "THREW"; }
            if (std::strcmp(verdict, "OK") == 0)
                std::printf("  face %d: OK\n", fi);
        }
        std::printf("scan t=%.2f %s: %d/%d faces yield a valid shell\n",
                    st, si ? "inter" : "arc", okCount, fi);
        return 0;
    }

    TopoDS_Face bf = bottomFace(body);
    TopTools_ListOfShape faces; faces.Append(bf);

    auto t0 = std::chrono::steady_clock::now();
    const char* verdict = "??";
    double v = 0;
    try {
        OCC_CATCH_SIGNALS
        BRepOffsetAPI_MakeThickSolid mk;
        mk.MakeThickSolidByJoin(body, faces, -t, tol, BRepOffset_Skin,
                                inter ? Standard_True : Standard_False,
                                Standard_False, join);
        mk.Build();
        if (!mk.IsDone()) verdict = "NOTDONE";
        else if (mk.Shape().IsNull()) verdict = "NULL";
        else if (!BRepCheck_Analyzer(mk.Shape()).IsValid()) verdict = "INVALID";
        else { verdict = "OK"; GProp_GProps g; BRepGProp::VolumeProperties(mk.Shape(), g); v = g.Mass(); }
    } catch (Standard_Failure&) { verdict = "THREW"; }
      catch (...) { verdict = "THREW?"; }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("t=%.2f %-5s tol=%.0e : %-8s %8.1f ms  vol=%.1f\n",
                t, inter ? "inter" : "arc", tol, verdict, ms, v);
    return 0;
}
