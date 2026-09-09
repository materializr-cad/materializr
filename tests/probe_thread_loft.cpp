// PROBE v2: threaded rod via BRepOffsetAPI_MakePipeShell - the notched
// cross-section swept along the STRAIGHT axis while an auxiliary HELIX spine
// drives its rotation ("twist"). No boolean; a handful of smooth helicoid
// faces; validity by construction. Timed against Steve's real case
// (14mm x 70mm rod, 2mm pitch = 35 turns) that took minutes via the cut path.
//
// Profile (notch centred at phi=0):
//   root flat  arc r=R-depth, phi in [-45,+45]     (0.25 P of the period)
//   flank      line up to crest edge               (0.25 P)
//   crest flat arc r=R,      phi in [135,225]      (0.25 P)
//   flank      line back down                      (0.25 P)
//
// Usage: probe_thread_loft [radius len pitch depth]

#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepLib.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom2d_Line.hxx>
#include <Geom_Circle.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Dir2d.hxx>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static const double PI = 3.14159265358979323846;

static gp_Pnt polar(double r, double phiDeg) {
    double a = phiDeg * PI / 180.0;
    return gp_Pnt(r * std::cos(a), r * std::sin(a), 0.0);
}

int main(int argc, char** argv) {
    double R = 7.0, len = 70.0, pitch = 2.0, depth = 0.8;
    if (argc >= 5) {
        R = std::atof(argv[1]); len = std::atof(argv[2]);
        pitch = std::atof(argv[3]); depth = std::atof(argv[4]);
    }
    std::printf("rod R=%.1f len=%.1f pitch=%.1f depth=%.1f (%.1f turns)\n",
                R, len, pitch, depth, len / pitch);
    auto t0 = std::chrono::steady_clock::now();

    // ── Profile wire in the XY plane (spine start) ──
    const double rr = R - depth;
    // root arc [-45, +45] at r=rr; flank line to (R,135); crest arc [135,225]
    // at r=R; flank line back to (rr,-45)==(rr,315).
    gp_Pnt rootA = polar(rr, -45), rootM = polar(rr, 0),  rootB = polar(rr, 45);
    gp_Pnt crestA = polar(R, 135), crestM = polar(R, 180), crestB = polar(R, 225);
    TopoDS_Edge eRoot  = BRepBuilderAPI_MakeEdge(
        GC_MakeArcOfCircle(rootA, rootM, rootB).Value()).Edge();
    // Flanks as 3-point arcs staying in the thread band - a straight chord
    // across 90 deg of arc sags to ~0.7R and gouges the rod.
    TopoDS_Edge eUp    = BRepBuilderAPI_MakeEdge(
        GC_MakeArcOfCircle(rootB, polar(0.5 * (rr + R), 90), crestA).Value()).Edge();
    TopoDS_Edge eCrest = BRepBuilderAPI_MakeEdge(
        GC_MakeArcOfCircle(crestA, crestM, crestB).Value()).Edge();
    TopoDS_Edge eDown  = BRepBuilderAPI_MakeEdge(
        GC_MakeArcOfCircle(crestB, polar(0.5 * (rr + R), 270), rootA).Value()).Edge();
    BRepBuilderAPI_MakeWire mkProfile(eRoot, eUp, eCrest, eDown);
    if (!mkProfile.IsDone()) { std::printf("PROFILE FAILED\n"); return 1; }
    TopoDS_Wire profile = mkProfile.Wire();

    // ── Spine: straight axis line 0..len along +Z ──
    TopoDS_Edge eSpine = BRepBuilderAPI_MakeEdge(
        gp_Pnt(0, 0, 0), gp_Pnt(0, 0, len)).Edge();
    TopoDS_Wire spine = BRepBuilderAPI_MakeWire(eSpine).Wire();

    // ── Auxiliary spine: helix on the cylinder, same span, pitch P ──
    // (2D line in the cylinder's UV space; BuildCurves3d gives the 3D helix -
    // the same trick ThreadOp's cutter uses.)
    Handle(Geom_CylindricalSurface) cyl =
        new Geom_CylindricalSurface(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), R);
    const double turns = len / pitch;
    gp_Pnt2d origin2d(0.0, 0.0);
    gp_Dir2d dir2d(2.0 * PI, pitch);                     // du per dv
    Handle(Geom2d_Line) l2d = new Geom2d_Line(origin2d, dir2d);
    const double segLen = std::sqrt(4.0 * PI * PI + pitch * pitch) * turns;
    TopoDS_Edge eHelix = BRepBuilderAPI_MakeEdge(l2d, cyl, 0.0, segLen).Edge();
    BRepLib::BuildCurves3d(eHelix);
    TopoDS_Wire helix = BRepBuilderAPI_MakeWire(eHelix).Wire();

    // ── Sweep: profile along the axis, rotation driven by the helix ──
    BRepOffsetAPI_MakePipeShell pipe(spine);
    pipe.SetMode(helix, Standard_True);   // curvilinear equivalence = twist
    pipe.Add(profile);
    pipe.Build();
    if (!pipe.IsDone()) { std::printf("SWEEP FAILED\n"); return 1; }
    if (!pipe.MakeSolid()) { std::printf("MAKESOLID FAILED\n"); return 1; }
    TopoDS_Shape rod = pipe.Shape();
    auto t1 = std::chrono::steady_clock::now();

    int nf = 0;
    for (TopExp_Explorer ex(rod, TopAbs_FACE); ex.More(); ex.Next()) ++nf;
    GProp_GProps g; BRepGProp::VolumeProperties(rod, g);
    const double plain = PI * R * R * len;
    auto t2 = std::chrono::steady_clock::now();
    bool valid = BRepCheck_Analyzer(rod).IsValid();
    auto t3 = std::chrono::steady_clock::now();
    BRepMesh_IncrementalMesh mesh(rod, 0.1);
    auto t4 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    std::printf("sweep %lldms  vol %lldms  check %lldms  mesh %lldms\n",
                ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4));
    std::printf("faces %d  volume %.1f (plain %.1f, ratio %.3f)  valid %s\n",
                nf, g.Mass(), plain, g.Mass() / plain, valid ? "YES" : "NO");
    // Sanity: compare against the intended cross-section area (numerically
    // integrated from the same polar profile: r=rr 90deg, r=R 90deg, arcs
    // between whose radius ~ linear in angle), within 5%.
    double A = 0.0;
    const int NI = 3600;
    for (int i = 0; i < NI; ++i) {
        double phi = 360.0 * i / NI;   // degrees, notch at 0
        double r;
        if (phi >= 315 || phi < 45) r = rr;
        else if (phi < 135) r = rr + (R - rr) * (phi - 45) / 90.0;
        else if (phi < 225) r = R;
        else r = R - (R - rr) * (phi - 225) / 90.0;
        A += 0.5 * r * r * (2.0 * PI / NI);
    }
    const double expect = A * len;
    bool volOk = std::abs(g.Mass() - expect) < 0.05 * expect;
    std::printf("expected %.1f, got %.1f (%.1f%%): %s\n", expect, g.Mass(),
                100.0 * g.Mass() / expect, volOk ? "OK" : "OUT OF BAND");
    return (valid && volOk) ? 0 : 1;
}
