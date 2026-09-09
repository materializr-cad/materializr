// PROBE: >40-turn rods as CHUNKED sweeps - MakePipeShell is exact through ~40
// turns and degrades beyond, so build N phase-aligned <=40-turn segments and
// fuse them at their planar interfaces (cheap boolean: two solids touching on
// a disc). Usage: probe_thread_chunk [R len pitch depth]
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BOPAlgo_GlueEnum.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepLib.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom2d_Line.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Trsf.hxx>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
static const double PI = 3.14159265358979323846;
static gp_Pnt polar(double r, double phiDeg) {
    double a = phiDeg * PI / 180.0;
    return gp_Pnt(r * std::cos(a), r * std::sin(a), 0.0);
}
// One <=40-turn swept segment starting at z0 with the notch phase already at
// theta0 = 2*pi*z0/pitch (so segments join seamlessly).
static TopoDS_Shape segment(double R, double depth, double pitch,
                            double z0, double segLenZ) {
    const double rr = R - depth;
    const double th0 = 360.0 * z0 / pitch;   // phase at the segment start, deg
    gp_Pnt rootA = polar(rr, th0 - 45), rootM = polar(rr, th0), rootB = polar(rr, th0 + 45);
    gp_Pnt crA = polar(R, th0 + 135), crM = polar(R, th0 + 180), crB = polar(R, th0 + 225);
    TopoDS_Edge eRoot = BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(rootA, rootM, rootB).Value()).Edge();
    TopoDS_Edge eUp = BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(rootB, polar(0.5*(rr+R), th0+90), crA).Value()).Edge();
    TopoDS_Edge eCrest = BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(crA, crM, crB).Value()).Edge();
    TopoDS_Edge eDown = BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(crB, polar(0.5*(rr+R), th0+270), rootA).Value()).Edge();
    BRepBuilderAPI_MakeWire mw(eRoot, eUp, eCrest, eDown);
    if (!mw.IsDone()) return {};
    TopoDS_Edge eSp = BRepBuilderAPI_MakeEdge(gp_Pnt(0,0,0), gp_Pnt(0,0,segLenZ)).Edge();
    TopoDS_Wire spine = BRepBuilderAPI_MakeWire(eSp).Wire();
    Handle(Geom_CylindricalSurface) cyl = new Geom_CylindricalSurface(
        gp_Ax3(gp_Pnt(0,0,0), gp_Dir(0,0,1)), R);
    Handle(Geom2d_Line) l2d = new Geom2d_Line(
        gp_Pnt2d(th0 * PI / 180.0, 0.0), gp_Dir2d(2.0 * PI, pitch));
    double sl = std::sqrt(4.0*PI*PI + pitch*pitch) * (segLenZ / pitch);
    TopoDS_Edge eH = BRepBuilderAPI_MakeEdge(l2d, cyl, 0.0, sl).Edge();
    BRepLib::BuildCurves3d(eH);
    BRepOffsetAPI_MakePipeShell pipe(spine);
    pipe.SetMode(BRepBuilderAPI_MakeWire(eH).Wire(), Standard_True);
    pipe.Add(mw.Wire());
    pipe.Build();
    if (!pipe.IsDone() || !pipe.MakeSolid()) return {};
    gp_Trsf t; t.SetTranslation(gp_Vec(0, 0, z0));
    return BRepBuilderAPI_Transform(pipe.Shape(), t, Standard_True).Shape();
}
int main(int argc, char** argv) {
    double R = 5.0, len = 150.0, pitch = 2.0, depth = 0.8;
    if (argc >= 5) { R=std::atof(argv[1]); len=std::atof(argv[2]);
                     pitch=std::atof(argv[3]); depth=std::atof(argv[4]); }
    const double turns = len / pitch;
    const int nSeg = std::max(1, (int)std::ceil(turns / 35.0)); // <=35 turns/seg
    // Chunk boundaries on whole-pitch multiples so phases align exactly.
    std::printf("rod R=%.1f len=%.1f pitch=%.1f (%.1f turns) -> %d segments\n",
                R, len, pitch, turns, nSeg);
    auto t0 = std::chrono::steady_clock::now();
    long long segMs = 0, fuseMs = 0;
    TopoDS_Shape rod;
    double z = 0.0;
    for (int i = 0; i < nSeg; ++i) {
        double zEnd = (i == nSeg - 1) ? len
                    : pitch * std::floor((len * (i + 1) / nSeg) / pitch);
        auto s0 = std::chrono::steady_clock::now();
        TopoDS_Shape seg = segment(R, depth, pitch, z, zEnd - z);
        auto s1 = std::chrono::steady_clock::now();
        segMs += std::chrono::duration_cast<std::chrono::milliseconds>(s1 - s0).count();
        if (seg.IsNull()) { std::printf("SEGMENT %d FAILED\n", i); return 1; }
        if (rod.IsNull()) rod = seg;
        else {
            BRepAlgoAPI_Fuse f;
            TopTools_ListOfShape args, tools;
            args.Append(rod); tools.Append(seg);
            f.SetArguments(args); f.SetTools(tools);
            f.SetFuzzyValue(1e-5);
            // The segments touch ONLY on coincident planar discs (identical
            // notched profiles at whole-pitch boundaries) - glue mode skips
            // the face-face intersection machinery entirely.
            f.SetGlue(BOPAlgo_GlueFull);
            auto f0 = std::chrono::steady_clock::now();
            f.Build();
            auto f1 = std::chrono::steady_clock::now();
            fuseMs += std::chrono::duration_cast<std::chrono::milliseconds>(f1 - f0).count();
            if (!f.IsDone()) { std::printf("FUSE %d FAILED\n", i); return 1; }
            rod = f.Shape();
        }
        z = zEnd;
    }
    auto tu = std::chrono::steady_clock::now();
    auto t1 = std::chrono::steady_clock::now();
    std::printf("segments %lldms  fuse %lldms  (no unify)\n", segMs, fuseMs);
    (void)tu;
    int nf = 0;
    for (TopExp_Explorer ex(rod, TopAbs_FACE); ex.More(); ex.Next()) ++nf;
    GProp_GProps g; BRepGProp::VolumeProperties(rod, g);
    bool valid = BRepCheck_Analyzer(rod).IsValid();
    // expected volume from the polar profile
    const double rr = R - depth;
    double A = 0.0; const int NI = 720;
    for (int i = 0; i < NI; ++i) {
        double phi = 360.0 * i / NI; double r;
        if (phi >= 315 || phi < 45) r = rr;
        else if (phi < 135) r = rr + depth * (phi - 45) / 90.0;
        else if (phi < 225) r = R;
        else r = R - depth * (phi - 225) / 90.0;
        A += 0.5 * r * r * (2.0 * PI / NI);
    }
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::printf("build %lldms  faces %d  volume %.1f expected %.1f (%.1f%%)  valid %s\n",
                ms, nf, g.Mass(), A * len, 100.0 * g.Mass() / (A * len),
                valid ? "YES" : "NO");
    bool ok = valid && std::abs(g.Mass() - A * len) < 0.06 * A * len;
    std::printf("%s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
