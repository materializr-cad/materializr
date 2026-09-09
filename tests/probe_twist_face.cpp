// De-risk probe for a "Twist Face" op: does BRepOffsetAPI_ThruSections (the
// same ruled loft Move Face already uses) produce a genuine TWIST when the top
// wire is a copy of the base rotated about the face normal - or does OCCT
// re-align the wires to minimise twist and hand back an untwisted solid?
//
// Mirrors MoveFaceOp::execute's loft exactly: reversed base outer wire + the
// transformed top outer wire → ThruSections(solid, ruled). For each angle we
// report validity, volume, face count, and the MEASURED twist (median angular
// delta between each side face's base-edge and top-edge about the axis). A
// square has 90° symmetry that can mask a re-align, so we also run a 10x6
// rectangle where any correspondence error is unambiguous.
//
// Usage: probe_twist_face

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepTools.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRep_Tool.hxx>
#include <gp_Trsf.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>

#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double H  = 10.0; // prism height

double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}
int faceCount(const TopoDS_Shape& s) {
    TopTools_IndexedMapOfShape m; TopExp::MapShapes(s, TopAbs_FACE, m);
    return m.Extent();
}

// A rectangular loop in the z=zPlane plane, centred on (cx,cy). `perEdge`
// intermediate points are inserted along each side (perEdge=1 = plain corners),
// so the top copy can be densely matched to the base for large-angle twists.
TopoDS_Wire rectWire(double cx, double cy, double hw, double hh, double z,
                     int perEdge = 1) {
    gp_Pnt corner[4] = {
        gp_Pnt(cx - hw, cy - hh, z), gp_Pnt(cx + hw, cy - hh, z),
        gp_Pnt(cx + hw, cy + hh, z), gp_Pnt(cx - hw, cy + hh, z)};
    BRepBuilderAPI_MakePolygon p;
    for (int e = 0; e < 4; ++e) {
        const gp_Pnt& a = corner[e];
        const gp_Pnt& b = corner[(e + 1) % 4];
        for (int k = 0; k < perEdge; ++k) {   // a inclusive, b handled by next edge
            double t = double(k) / perEdge;
            p.Add(gp_Pnt(a.X() + (b.X() - a.X()) * t,
                         a.Y() + (b.Y() - a.Y()) * t, z));
        }
    }
    p.Close();
    return p.Wire();
}

// Median signed angular delta (deg) between each side face's base-edge midpoint
// and top-edge midpoint about the vertical axis through (cx,cy). ~= the twist
// actually realised by the loft. Returns 999 if it couldn't measure.
double measuredTwistDeg(const TopoDS_Shape& solid, double cx, double cy) {
    auto ang = [&](const gp_Pnt& p) {
        return std::atan2(p.Y() - cy, p.X() - cx);
    };
    std::vector<double> deltas;
    for (TopExp_Explorer fe(solid, TopAbs_FACE); fe.More(); fe.Next()) {
        TopoDS_Face f = TopoDS::Face(fe.Current());
        // Collect this face's vertices, split base (z~0) vs top (z~H).
        std::vector<gp_Pnt> base, top;
        TopTools_IndexedMapOfShape vm;
        TopExp::MapShapes(f, TopAbs_VERTEX, vm);
        for (int i = 1; i <= vm.Extent(); ++i) {
            gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vm(i)));
            if (std::fabs(p.Z()) < 1e-6) base.push_back(p);
            else if (std::fabs(p.Z() - H) < 1e-6) top.push_back(p);
        }
        // Side faces straddle both planes; caps sit in one.
        if (base.empty() || top.empty()) continue;
        gp_Pnt bm(0,0,0), tm(0,0,0);
        for (auto& p : base) bm.SetXYZ(bm.XYZ() + p.XYZ());
        for (auto& p : top)  tm.SetXYZ(tm.XYZ() + p.XYZ());
        bm.SetXYZ(bm.XYZ() / base.size());
        tm.SetXYZ(tm.XYZ() / top.size());
        double d = (ang(tm) - ang(bm)) * 180.0 / PI;
        while (d > 180.0)  d -= 360.0;
        while (d < -180.0) d += 360.0;
        deltas.push_back(d);
    }
    if (deltas.empty()) return 999.0;
    std::sort(deltas.begin(), deltas.end());
    return deltas[deltas.size() / 2];
}

// One twist attempt: rotate the top wire about the vertical axis through the
// centroid by `deg`, loft base(reversed)+top exactly like MoveFaceOp.
void tryTwist(const char* label, const TopoDS_Wire& baseWire,
              const TopoDS_Wire& topWire0, double cx, double cy, double deg) {
    gp_Trsf t;
    t.SetRotation(gp_Ax1(gp_Pnt(cx, cy, H), gp_Dir(0, 0, 1)), deg * PI / 180.0);
    TopoDS_Wire topWire =
        TopoDS::Wire(BRepBuilderAPI_Transform(topWire0, t, Standard_True).Shape());

    TopoDS_Wire bRev = baseWire; bRev.Reverse();
    BRepOffsetAPI_ThruSections ts(Standard_True /*solid*/, Standard_True /*ruled*/);
    ts.AddWire(bRev);
    ts.AddWire(topWire);
    try { ts.Build(); } catch (...) {
        std::printf("  %-10s %5.0f deg : THREW\n", label, deg); return;
    }
    if (!ts.IsDone() || ts.Shape().IsNull()) {
        std::printf("  %-10s %5.0f deg : not done\n", label, deg); return;
    }
    TopoDS_Shape s = ts.Shape();
    bool valid = BRepCheck_Analyzer(s).IsValid();
    double v = vol(s);
    int nf = faceCount(s);
    double tw = measuredTwistDeg(s, cx, cy);
    std::printf("  %-10s %5.0f deg : valid=%d  vol=%7.2f  faces=%d  measuredTwist=%6.1f%s\n",
                label, deg, valid ? 1 : 0, v, nf, tw,
                (std::fabs(tw - deg) < 2.0) ? "  <-- twist OK"
                : (std::fabs(tw) < 2.0)     ? "  <-- UNTWISTED (re-aligned)"
                                            : "  <-- other");
}
} // namespace

int main() {
    std::printf("=== Twist-face feasibility: ruled ThruSections base->rotated-top ===\n");

    // SQUARE 10x10 (90-deg symmetric - a re-align to nearest corner shows up as
    // the twist wrapping toward 0 past 45 deg).
    std::printf("\nSquare 10x10 (centroid 5,5):\n");
    TopoDS_Wire sqBase = rectWire(5, 5, 5, 5, 0);
    TopoDS_Wire sqTop  = rectWire(5, 5, 5, 5, H);
    for (double d : {5.0, 15.0, 30.0, 45.0, 60.0, 80.0})
        tryTwist("square", sqBase, sqTop, 5, 5, d);

    // RECTANGLE 10x6 (no 90-deg symmetry - correspondence errors are obvious;
    // only 180-deg symmetric, so meaningful twist range is < ~90 deg anyway).
    std::printf("\nRectangle 10x6 (centroid 5,3):\n");
    TopoDS_Wire rcBase = rectWire(5, 3, 5, 3, 0);
    TopoDS_Wire rcTop  = rectWire(5, 3, 5, 3, H);
    for (double d : {5.0, 15.0, 30.0, 45.0, 60.0, 80.0})
        tryTwist("rect", rcBase, rcTop, 5, 3, d);

    // LAYERED twist: feed ONE ThruSections a stack of intermediate wires, each
    // a small twist increment (total/steps per step). Each step stays well
    // under the ~45 re-align threshold, so correspondence holds and the whole
    // loft twists the full amount as a single valid solid. This is the real
    // implementation path (Move Face's loft, but with intermediate sections).
    auto layeredTwist = [](const char* label, double cx, double cy,
                           double hw, double hh, double total, int steps) {
        BRepOffsetAPI_ThruSections ts(Standard_True, Standard_True);
        for (int i = 0; i <= steps; ++i) {
            double z = H * double(i) / steps;
            double deg = total * double(i) / steps;
            TopoDS_Wire w = rectWire(cx, cy, hw, hh, z);   // plain 4-corner
            gp_Trsf t; t.SetRotation(gp_Ax1(gp_Pnt(cx, cy, z), gp_Dir(0,0,1)),
                                     deg * PI / 180.0);
            w = TopoDS::Wire(BRepBuilderAPI_Transform(w, t, Standard_True).Shape());
            if (i == 0) w.Reverse(); // match MoveFaceOp base orientation
            ts.AddWire(w);
        }
        try { ts.Build(); } catch (...) {
            std::printf("  %-12s %5.0f deg /%2d steps : THREW\n", label, total, steps);
            return;
        }
        if (!ts.IsDone() || ts.Shape().IsNull()) {
            std::printf("  %-12s %5.0f deg /%2d steps : not done\n", label, total, steps);
            return;
        }
        TopoDS_Shape s = ts.Shape();
        // Top-cap footprint (z~H): a real twist rotates it; for the rectangle a
        // 90 twist swaps 10x6 -> 6x10, which "valid + face count" can't show.
        double xmn=1e9, xmx=-1e9, ymn=1e9, ymx=-1e9;
        TopTools_IndexedMapOfShape vm; TopExp::MapShapes(s, TopAbs_VERTEX, vm);
        for (int i = 1; i <= vm.Extent(); ++i) {
            gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vm(i)));
            if (std::fabs(p.Z() - H) < 1e-6) {
                xmn = std::min(xmn, p.X()); xmx = std::max(xmx, p.X());
                ymn = std::min(ymn, p.Y()); ymx = std::max(ymx, p.Y());
            }
        }
        std::printf("  %-12s %5.0f deg /%2d steps : valid=%d  vol=%7.2f  faces=%d  topCap=%.1fx%.1f\n",
                    label, total, steps, BRepCheck_Analyzer(s).IsValid() ? 1 : 0,
                    vol(s), faceCount(s), xmx - xmn, ymx - ymn);
    };
    std::printf("\nLayered twist (one ThruSections, N intermediate wires):\n");
    layeredTwist("sq 90",  5, 5, 5, 5, 90.0,  8);
    layeredTwist("sq 90",  5, 5, 5, 5, 90.0, 16);
    layeredTwist("sq 180", 5, 5, 5, 5, 180.0, 16);
    layeredTwist("sq 360", 5, 5, 5, 5, 360.0, 32);
    layeredTwist("rect 90", 5, 3, 5, 3, 90.0, 16);

    // DENSIFIED square: 8 points per edge (32-pt loop). If dense matching
    // points defeat the corner-correspondence wrap, big twists (90+) should
    // read true instead of folding back past 45.
    std::printf("\nSquare 10x10 densified (8 pts/edge):\n");
    TopoDS_Wire dsBase = rectWire(5, 5, 5, 5, 0, 8);
    TopoDS_Wire dsTop  = rectWire(5, 5, 5, 5, H, 8);
    for (double d : {30.0, 45.0, 60.0, 90.0, 120.0, 160.0})
        tryTwist("sq-dense", dsBase, dsTop, 5, 5, d);

    std::printf("\nReading: measuredTwist ~= input angle => genuine twist (loft "
                "keeps corner correspondence). ~=0 => OCCT re-aligned the wires "
                "and there is no twist. valid=0 => OCCT refuses at that angle.\n");
    return 0;
}
