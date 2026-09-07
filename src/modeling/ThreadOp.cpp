#include "ui/LengthField.h"
#include "ThreadOp.h"
#include <Standard_ErrorHandler.hxx>  // OCC_CATCH_SIGNALS (MSVC needs it explicit)
#include "../core/Verbose.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <algorithm>
#include <vector>
#include <Geom_CylindricalSurface.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Ax3.hxx>
#include <gp_Vec.hxx>
#include <Geom2d_Line.hxx>
#include <Geom_Plane.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <gp_Circ.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepLib.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <Geom2d_Ellipse.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <GCE2d_MakeSegment.hxx>
#include <gp_Ax2d.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <Bnd_Box.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BOPAlgo_GlueEnum.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <TopExp_Explorer.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopoDS_Face.hxx>
#include <Geom_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <ShapeFix_Shape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Dir2d.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressRange.hxx>
#include <Message_ProgressScope.hxx>
#include <imgui.h>
#include "../ui/NumField.h"
#include "../i18n.h"
#include "../i18n.h"
#include "../i18n.h"

namespace {
// Turns the per-job cancel token into an OCCT user-break, so a Cancel click
// aborts a boolean MID-CUT (the O(N²) compound cut is one long boolean —
// between-turn checks alone would never see the flag).
class ThreadCancelBreak : public Message_ProgressIndicator {
public:
    DEFINE_STANDARD_RTTI_INLINE(ThreadCancelBreak, Message_ProgressIndicator)
    explicit ThreadCancelBreak(std::shared_ptr<std::atomic<bool>> f)
        : m_f(std::move(f)) {}
    Standard_Boolean UserBreak() override { return m_f && m_f->load(); }
protected:
    void Show(const Message_ProgressScope&, const Standard_Boolean) override {}
private:
    std::shared_ptr<std::atomic<bool>> m_f;
};
} // namespace

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─── Generalized cross-section spec ─────────────────────────────────────────
// One notch's angular budget as fractions of its period, plus flank style.
// Standard is NOT described here — it keeps its own exact arc code. These
// drive the new maker/printing profiles swept by the same helix machinery.
namespace {
struct NotchSpec {
    double fRoot, fUp, fCrest, fDown;  // fractions of the period (sum ~= 1)
    bool arcFlank;                     // arc flanks (smooth) vs straight ramps
};
NotchSpec notchSpec(ThreadProfile p) {
    switch (p) {
        // ACME/leadscrew: wide flats, moderate straight flanks.
        case ThreadProfile::Trapezoidal: return {0.30, 0.20, 0.30, 0.20, false};
        // Near-vertical walls, equal land and groove.
        case ThreadProfile::Square:      return {0.47, 0.03, 0.47, 0.03, false};
        // Asymmetric: steep load flank (up), long shallow back (down).
        case ThreadProfile::Buttress:    return {0.28, 0.04, 0.30, 0.38, false};
        // Sinusoidal-ish: tiny flats, big arc flanks — best for printing.
        case ThreadProfile::Rounded:     return {0.10, 0.40, 0.10, 0.40, true};
        default:                         return {0.25, 0.25, 0.25, 0.25, true};
    }
}
// Radius of the general notch at angle phi within one period [0,period),
// root centred at 0. Linear flanks (arc flanks integrate close enough for
// the volume guard). rr = root radius, R = crest radius.
double notchRadius(const NotchSpec& s, double period, double phi,
                   double rr, double R) {
    while (phi < 0) phi += period;
    while (phi >= period) phi -= period;
    const double a1 = s.fRoot * period * 0.5;             // root half-width
    const double b0 = a1 + s.fUp * period;                // crest start
    const double b1 = b0 + s.fCrest * period;             // crest end
    const double a0 = period - a1;                        // root resumes
    if (phi <= a1 || phi >= a0) return rr;
    if (phi < b0) return rr + (R - rr) * (phi - a1) / (b0 - a1);
    if (phi <= b1) return R;
    return R - (R - rr) * (phi - b1) / (a0 - b1);
}

// ─── Boolean groove-cutter profile ──────────────────────────────────────────
// The boolean path cuts a groove whose cross-section is a stack of loft bands
// from the mouth (at the surface) to the apex (depth-deep). Each band gives
// the groove's axial half-widths below/above the helix centreline, as a
// FRACTION of the mouth half-width. `openFrac` is the mouth opening / pitch.
// Standard reproduces the shipped 2-band trapezoid exactly (openFrac 0.875,
// mouth 1.0 → apex 0.25), so the Standard boolean path is bit-identical.
struct GrooveBand { double rFrac, offLo, offUp; }; // rFrac 0=mouth .. 1=apex
struct GrooveSpec { double openFrac; std::vector<GrooveBand> bands; };
GrooveSpec grooveSpec(ThreadProfile p) {
    switch (p) {
        case ThreadProfile::Trapezoidal:
            return {0.60, {{0,1,1}, {1,0.35,0.35}}};        // wide-crest, straight
        case ThreadProfile::Square:
            return {0.50, {{0,1,1}, {1,1,1}}};              // vertical walls
        // Near-vertical load flank on the UP side; the back flank tapers
        // DOWNWARD (offLo → apex). Flipped from the first pass per Steve; a
        // direction toggle can follow if the load side needs to be chosen.
        case ThreadProfile::Buttress:
            return {0.60, {{0,1,1}, {1,1.0,0.12}}};
        // TRUE rounded: multi-band so the flanks CURVE (a 2-band loft is
        // always a straight-flanked trapezoid). Built as fused radial slabs,
        // so the tool build is heavier on a long thread — acceptable behind
        // the progress bar; short/coarse prints (the common case) stay quick.
        case ThreadProfile::Rounded:
            return {0.60, {{0,1,1}, {0.5,0.82,0.82}, {1,0.3,0.3}}};
        default: // Standard
            return {0.875, {{0,1,1}, {1,0.25,0.25}}};
    }
}
// Groove cross-section area (avg total width × depth), integrated over the
// band table — replaces the trapezoid-only analytic formula so the volume
// gate is correct for any profile.
double grooveArea(const GrooveSpec& s, double mouthHalf, double depth) {
    double a = 0.0;
    for (size_t i = 1; i < s.bands.size(); ++i) {
        const auto& b0 = s.bands[i-1]; const auto& b1 = s.bands[i];
        double w0 = mouthHalf * (b0.offLo + b0.offUp);
        double w1 = mouthHalf * (b1.offLo + b1.offUp);
        a += 0.5 * (w0 + w1) * (b1.rFrac - b0.rFrac);   // trapezoid rule
    }
    return a * depth;   // rFrac spans the full depth
}
// Groove half-widths below/above the centreline at a given depth fraction —
// for the width-probe gate. Asymmetric (buttress) profiles differ per side,
// so a symmetric probe on the narrow flank falsely reads solid.
void grooveHalfAt(const GrooveSpec& s, double mouthHalf, double df,
                  double& outLo, double& outUp) {
    for (size_t i = 1; i < s.bands.size(); ++i) {
        const auto& b0 = s.bands[i-1]; const auto& b1 = s.bands[i];
        if (df <= b1.rFrac + 1e-9) {
            double t = (df - b0.rFrac) / std::max(1e-9, b1.rFrac - b0.rFrac);
            outLo = mouthHalf * (b0.offLo + (b1.offLo - b0.offLo) * t);
            outUp = mouthHalf * (b0.offUp + (b1.offUp - b0.offUp) * t);
            return;
        }
    }
    outLo = mouthHalf * s.bands.back().offLo;
    outUp = mouthHalf * s.bands.back().offUp;
}
} // namespace

// ─── Swept-rod fast path ("the twist idea") ─────────────────────────────────
// For the most common case — an EXTERNAL thread covering a PLAIN full
// cylinder (sketch circle → extrude → thread) — don't cut grooves with a
// boolean at all. Build the threaded rod NATIVELY: sweep the notched
// cross-section along the axis while an auxiliary helix spine twists it
// (BRepOffsetAPI_MakePipeShell curvilinear equivalence). One solid, ~6 smooth
// helicoid faces, validity by construction, and ~200ms where the boolean
// compound/per-turn path took MINUTES on a 35-turn rod (Steve's 14x70).
// Anything else (holes, bolts with heads, partial spans, interrupted
// cylinders) returns null here and takes the proven boolean paths below.
//
// Cross-section (notch centred at phi=0, radius band [R-depth .. R]):
//   root flat  arc  phi in [-45, +45]  at R-depth   (0.25 of the period)
//   flank      arc  rising to the crest edge        (0.25)
//   crest flat arc  phi in [135, 225]  at R         (0.25)
//   flank      arc  falling back to the root        (0.25)
// Flanks are 3-point arcs staying in the band — a straight chord across 90°
// of arc sags to ~0.7R and gouges the rod (probe-proven).
static TopoDS_Shape sweptRodThread(const gp_Ax3& ax3, double Rin, double len,
                                   double pitch, double depth,
                                   bool rightHanded,
                                   ThreadProfile profile = ThreadProfile::Standard,
                                   double clearance = 0.0) {
    try {
        OCC_CATCH_SIGNALS
        // Clearance pulls the crest IN so a printed external thread fits a
        // nominal internal one (nozzle over-extrusion binds an exact fit).
        const double R = Rin - std::max(0.0, clearance);
        if (R <= depth) return {};
        const double rr = R - depth;
        const double uSign = rightHanded ? 1.0 : -1.0;
        // Flank clearance: besides pulling the crest in RADIALLY (R, above),
        // thin the tooth AXIALLY by `clearance` on each flank so a printed
        // thread clears its mate on the SIDES too, not just the crest/root.
        // One pitch spans 360°, so `clearance` mm ↦ clearance/pitch·360°; the
        // crest is narrowed by that on each side (the groove widens to match).
        // Clamped to keep a real crest land at fine pitches (where a full
        // clearance would otherwise eat the whole tooth).
        const double flankDeg = std::max(0.0, clearance) / pitch * 360.0;
        const bool general = (profile != ThreadProfile::Standard);
        const NotchSpec spec = notchSpec(profile);
        auto polar = [&](double r, double phiDeg) {
            double a = phiDeg * M_PI / 180.0;
            return gp_Pnt(r * std::cos(a), r * std::sin(a), 0.0);
        };

        // One <=35-turn swept segment spanning z0..z0+lenZ, its notch PHASE
        // already rotated to th0 = uSign*360*z0/pitch so consecutive segments
        // continue the same helix seamlessly.
        auto oneSegment = [&](double z0, double lenZ) -> TopoDS_Shape {
            const double th0 = uSign * 360.0 * z0 / pitch;
            BRepBuilderAPI_MakeWire mkProfile;
            if (!general) {
                // Standard: the exact shipped 4-arc notch (25/25/25/25).
                gp_Pnt rootA = polar(rr, th0 - 45), rootM = polar(rr, th0),
                       rootB = polar(rr, th0 + 45);
                // Crest nominally spans [135,225] (90°); narrow it by the flank
                // clearance on each side (keep >=18° of crest land).
                const double cd = std::min(flankDeg, 36.0);
                gp_Pnt crA = polar(R, th0 + 135 + cd), crM = polar(R, th0 + 180),
                       crB = polar(R, th0 + 225 - cd);
                mkProfile.Add(BRepBuilderAPI_MakeEdge(
                    GC_MakeArcOfCircle(rootA, rootM, rootB).Value()).Edge());
                mkProfile.Add(BRepBuilderAPI_MakeEdge(
                    GC_MakeArcOfCircle(rootB, polar(0.5 * (rr + R), th0 + 90),
                                       crA).Value()).Edge());
                mkProfile.Add(BRepBuilderAPI_MakeEdge(
                    GC_MakeArcOfCircle(crA, crM, crB).Value()).Edge());
                mkProfile.Add(BRepBuilderAPI_MakeEdge(
                    GC_MakeArcOfCircle(crB, polar(0.5 * (rr + R), th0 + 270),
                                       rootA).Value()).Edge());
            } else {
                // General notch: root arc at rr, crest arc at R, flanks arc
                // or straight per the profile spec. Angular boundaries around
                // th0 (period 360, single start; root centred at th0).
                const double aR = spec.fRoot * 180.0;          // root half-width
                const double aU = spec.fUp * 360.0;            // up flank
                const double aC = spec.fCrest * 360.0;         // crest
                // Narrow the crest by the flank clearance on each side (keep
                // >=20% of the crest as land); the groove widens to match.
                const double cd = std::min(flankDeg, 0.4 * aC);
                const double b0 = th0 + aR, b1 = b0 + aU + cd;
                const double c1 = b1 + aC - 2.0 * cd;
                gp_Pnt rootA = polar(rr, th0 - aR), rootM = polar(rr, th0),
                       rootB = polar(rr, th0 + aR);
                gp_Pnt crA = polar(R, b1), crM = polar(R, 0.5 * (b1 + c1)),
                       crB = polar(R, c1);
                mkProfile.Add(BRepBuilderAPI_MakeEdge(
                    GC_MakeArcOfCircle(rootA, rootM, rootB).Value()).Edge());
                if (spec.arcFlank) {
                    mkProfile.Add(BRepBuilderAPI_MakeEdge(
                        GC_MakeArcOfCircle(rootB, polar(0.5*(rr+R), 0.5*(b0+b1)),
                                           crA).Value()).Edge());
                } else {
                    mkProfile.Add(BRepBuilderAPI_MakeEdge(rootB, crA).Edge());
                }
                mkProfile.Add(BRepBuilderAPI_MakeEdge(
                    GC_MakeArcOfCircle(crA, crM, crB).Value()).Edge());
                const double d1 = th0 + 360.0 - aR;            // down flank end
                if (spec.arcFlank) {
                    mkProfile.Add(BRepBuilderAPI_MakeEdge(
                        GC_MakeArcOfCircle(crB, polar(0.5*(rr+R), 0.5*(c1+d1)),
                                           rootA).Value()).Edge());
                } else {
                    mkProfile.Add(BRepBuilderAPI_MakeEdge(crB, rootA).Edge());
                }
            }
            if (!mkProfile.IsDone()) return {};
            TopoDS_Edge eSpine = BRepBuilderAPI_MakeEdge(
                gp_Pnt(0, 0, 0), gp_Pnt(0, 0, lenZ)).Edge();
            TopoDS_Wire spine = BRepBuilderAPI_MakeWire(eSpine).Wire();
            Handle(Geom_CylindricalSurface) cylSurf =
                new Geom_CylindricalSurface(
                    gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), R);
            Handle(Geom2d_Line) l2d = new Geom2d_Line(
                gp_Pnt2d(th0 * M_PI / 180.0, 0.0),
                gp_Dir2d(uSign * 2.0 * M_PI, pitch));
            const double segLen =
                std::sqrt(4.0 * M_PI * M_PI + pitch * pitch) * (lenZ / pitch);
            TopoDS_Edge eHelix =
                BRepBuilderAPI_MakeEdge(l2d, cylSurf, 0.0, segLen).Edge();
            BRepLib::BuildCurves3d(eHelix);
            BRepOffsetAPI_MakePipeShell pipe(spine);
            pipe.SetMode(BRepBuilderAPI_MakeWire(eHelix).Wire(),
                         Standard_True);   // twist follows the helix
            pipe.Add(mkProfile.Wire());
            if (general) {
                // Coarse-pitch sweeps come out as rippled B-splines (dense,
                // jagged mesh). Force a single C1 approximation and give the
                // approximator room, so the helicoid is smooth instead of
                // wavy. Standard keeps its shipped (fine-pitch) behaviour.
                pipe.SetForceApproxC1(Standard_True);
            }
            pipe.Build();
            if (!pipe.IsDone() || !pipe.MakeSolid()) return {};
            gp_Trsf up; up.SetTranslation(gp_Vec(0, 0, z0));
            return BRepBuilderAPI_Transform(pipe.Shape(), up,
                                            Standard_True).Shape();
        };

        // MakePipeShell is exact through ~40 turns and degrades beyond, so
        // long rods build as phase-aligned <=35-turn segments GLUED at their
        // coincident planar interfaces (whole-pitch boundaries -> identical
        // notched discs; BOPAlgo_GlueFull skips the face-face intersection
        // machinery, so a 150-turn rod lands in ~1.6s where a plain fuse took
        // 90s and the old boolean cut minutes).
        const double turns = len / pitch;
        // General profiles chunk finer: a shorter per-segment helical span is
        // a simpler surface for the approximator, so each face stays smooth
        // (the boolean path's per-turn simplicity is what made buttress clean).
        const double turnsPerSeg = general ? 6.0 : 35.0;
        const int nSeg = std::max(1, (int)std::ceil(turns / turnsPerSeg));
        TopoDS_Shape rod;
        double z = 0.0;
        for (int i = 0; i < nSeg; ++i) {
            double zEnd = (i == nSeg - 1)
                              ? len
                              : pitch * std::floor((len * (i + 1) / nSeg) / pitch);
            if (zEnd <= z) return {};
            TopoDS_Shape seg = oneSegment(z, zEnd - z);
            if (seg.IsNull()) return {};
            if (rod.IsNull()) {
                rod = seg;
            } else {
                BRepAlgoAPI_Fuse f;
                TopTools_ListOfShape args, tools;
                args.Append(rod);
                tools.Append(seg);
                f.SetArguments(args);
                f.SetTools(tools);
                f.SetFuzzyValue(1e-5);
                f.SetGlue(BOPAlgo_GlueFull);
                f.Build();
                if (!f.IsDone() || f.Shape().IsNull()) return {};
                rod = f.Shape();
            }
            z = zEnd;
        }

        // Move the canonical (origin, +Z) rod onto the op's axis frame.
        gp_Trsf tr;
        tr.SetDisplacement(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1),
                                  gp_Dir(1, 0, 0)), ax3);
        rod = BRepBuilderAPI_Transform(rod, tr, Standard_True).Shape();

        // Guards: valid solid, volume matching the intended profile. Area is
        // the cross-section integral 0.5·r(phi)²·dphi — Standard uses its exact
        // 25/25/25/25 ramps, general profiles use the spec's r(phi). Arc flanks
        // deviate a hair from the linear integral, so the tolerance is looser
        // for the general path.
        if (rod.IsNull() || !BRepCheck_Analyzer(rod).IsValid()) return {};
        double A = 0.0;
        const int NI = 1440;
        for (int i = 0; i < NI; ++i) {
            double phi = 360.0 * i / NI;
            double r;
            if (!general) {
                if (phi >= 315 || phi < 45) r = rr;
                else if (phi < 135) r = rr + depth * (phi - 45) / 90.0;
                else if (phi < 225) r = R;
                else r = R - depth * (phi - 225) / 90.0;
            } else {
                r = notchRadius(spec, 360.0, phi, rr, R);
            }
            A += 0.5 * r * r * (2.0 * M_PI / NI);
        }
        GProp_GProps g;
        BRepGProp::VolumeProperties(rod, g);
        const double tol = general ? 0.10 : 0.06;
        if (std::abs(g.Mass() - A * len) > tol * A * len) return {};
        return rod;
    } catch (...) {
        return {};
    }
}

// Extract the thread frame from a cylindrical face: axis at the face's V_min
// end (origin = surface location + v0·axis, direction along the cylinder), and
// the radius. Length is deliberately NOT taken from the face — the user's
// chosen thread span is kept; only the cylinder's position and diameter follow
// an edit. Returns false if the face isn't a plain cylinder.
static bool cylFaceToThread(const TopoDS_Face& face, gp_Ax2& ax2, double& radius) {
    Handle(Geom_Surface) gs = BRep_Tool::Surface(face);
    Handle(Geom_CylindricalSurface) cs =
        Handle(Geom_CylindricalSurface)::DownCast(gs);
    if (cs.IsNull()) return false;
    const gp_Cylinder cyl = cs->Cylinder();
    radius = cyl.Radius();
    Standard_Real u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const gp_Ax3 pos = cyl.Position();
    const gp_Dir axisDir = pos.Direction();
    const gp_Pnt origin =
        pos.Location().Translated(gp_Vec(axisDir) * std::min(v0, v1));
    ax2 = gp_Ax2(origin, axisDir, pos.XDirection());
    return true;
}

ThreadOp::ThreadOp() = default;

void ThreadOp::setAxis(const gp_Ax2& axis) {
    m_axis = axis;
    m_axOX = axis.Location().X();
    m_axOY = axis.Location().Y();
    m_axOZ = axis.Location().Z();
    m_axDX = axis.Direction().X();
    m_axDY = axis.Direction().Y();
    m_axDZ = axis.Direction().Z();
    m_axXX = axis.XDirection().X();
    m_axXY = axis.XDirection().Y();
    m_axXZ = axis.XDirection().Z();
}

TopoDS_Shape ThreadOp::buildResult(const TopoDS_Shape& body) const {
    if (body.IsNull() || m_pitch <= 0.05 || m_depth <= 0.0 ||
        m_length <= 0.0 || m_radius <= m_depth) {
        return {};
    }
    double turns = m_length / m_pitch;
    // Runaway guard: a 0.1 mm pitch over a long rod would sweep thousands of
    // turns and lock the compute for minutes. 300 turns is far beyond any
    // sane model at this app's scale (+2 below for the runout extensions).
    if (turns > 300.0) return {};

    // Geometric sanity: a depth beyond ~0.65·pitch merges adjacent grooves
    // and leaves paper-thin helical fins instead of crests (ISO depth is
    // 0.6134·P); beyond ~45% of the radius it eats the core. Clamp rather
    // than fail — the UI clamps too, but reloaded files / old params must
    // never produce garbage solids. Multi-start Rounded always cuts with
    // the rope tool, whose radius IS the depth and caps at 0.45·pitch (a
    // land must survive between the interleaved grooves) — fold that cap in
    // here so the analytic volume window and the shape probes measure the
    // SAME groove the tool cuts, instead of a deeper one it silently won't.
    const bool ropeDepthCap =
        m_profile == ThreadProfile::Rounded && m_starts > 1;
    const double depth = std::min({m_depth,
                                   (ropeDepthCap ? 0.45 : 0.65) * m_pitch,
                                   0.45 * m_radius});
    // Multi-start: each of the N interleaved helixes advances N·P per turn
    // (the LEAD) while the groove FORM and the crest spacing stay keyed to
    // the user's pitch P. At any fixed angle the grooves still pass every P
    // axially — which is why the crest probes, the per-P volume windows and
    // the whole-P glue boundaries all hold unchanged for any N. Only the
    // helix PHASE formulas below use the lead.
    const double lead = m_pitch * std::max(1, m_starts);
    if (depth <= 0.0) return {};

    // Boolean groove-cutter cross-section for this profile (Standard = the
    // shipped trapezoid). Used by buildCutterEx, the width-probe gate, and the
    // analytic-volume gate below.
    const GrooveSpec gSpec = grooveSpec(m_profile);
    // Groove half-width at the surface. Normally the profile's own fraction
    // of the pitch; an explicit m_grooveWidth decouples the two so a coarse
    // pitch can carry a NARROW groove (a wire seat, a grip spiral). Clamped
    // to 0.9·pitch so a crest land always survives between turns. This single
    // value feeds the cutter, the analytic volume gate and the width probes,
    // which is why they all stay consistent for free.
    const double baseMouthHalf =
        (m_grooveWidth > 0.0 && profileTakesGrooveWidth(m_profile))
            ? 0.5 * std::min(m_grooveWidth, 0.9 * m_pitch)
            : 0.5 * gSpec.openFrac * m_pitch;
    // Flank clearance (mm per side): widen the groove — and thus every gate
    // that measures it — so it stays consistent between the cutter and the
    // volume/width checks. Clamped to keep a crest land (mouth < 0.9·pitch).
    const double flankClear = std::min(std::max(0.0, m_clearance),
                                       std::max(0.0, 0.45 * m_pitch - baseMouthHalf));

    if (materializr::isVerbose())
        std::fprintf(stderr, "[Thread] buildResult: pitch=%.3f depth=%.3f r=%.3f "
                             "len=%.3f hole=%d\n",
                     m_pitch, m_depth, m_radius, m_length, m_isHole ? 1 : 0);

    // PRE-FLIGHT STRATEGY CHECK. The single compound band tool only cuts
    // reliably against FULL, CLOSED cylinders — after ~40 harness
    // experiments, any boolean between it and a PARTIAL or INTERRUPTED
    // cylinder (the half of a lengthwise split, a rod with a cross-hole)
    // coin-flips between no-ops, inverted removal, and plausible-volume cuts
    // of the WRONG material (the "stacked poker chips" body), and those
    // garbage solids go on to crash the tessellator. Such bodies take the
    // PER-TURN SEQUENTIAL path below instead: one boolean per turn, each
    // validated by removed volume, failed turns retried through a variant
    // ladder (micro-jitters that break the surface coincidences that make
    // the kernel misclassify). Proven in the volume harness: lengthwise
    // halves thread 21/21 turns, holed rods ~20/21.
    bool fullCylinder = false;
    {
        gp_Pnt axLoc(m_axOX, m_axOY, m_axOZ);
        gp_Dir axDir(m_axDX, m_axDY, m_axDZ);
        for (TopExp_Explorer fx(body, TopAbs_FACE);
             fx.More() && !fullCylinder; fx.Next()) {
            TopoDS_Face f = TopoDS::Face(fx.Current());
            Handle(Geom_CylindricalSurface) cs =
                Handle(Geom_CylindricalSurface)::DownCast(BRep_Tool::Surface(f));
            if (cs.IsNull()) continue;
            gp_Cylinder cyl = cs->Cylinder();
            if (std::abs(cyl.Radius() - m_radius) > 1e-4) continue;
            if (std::abs(cyl.Position().Direction().Dot(axDir)) < 0.9999)
                continue;
            // axis line coincidence: location must lie on our axis
            gp_Vec d(axLoc, cyl.Position().Location());
            if (d.Magnitude() > 1e-6 &&
                d.Crossed(gp_Vec(axDir)).Magnitude() > 1e-3) continue;
            double u1, u2, v1, v2;
            BRepTools::UVBounds(f, u1, u2, v1, v2);
            // A face with an inner wire (cross-hole mouth) keeps full outer
            // UV bounds, so "full 2π wrap" here really means "uninterrupted
            // enough for the compound tool to have a chance" — the volume
            // guard in tryCut still arbitrates, and per-turn is the net.
            if (std::abs((u2 - u1) - 2.0 * M_PI) < 1e-3) fullCylinder = true;
        }
    }
    try {
        // Rebuild the axis from the serialisable components (identical for
        // fresh and reloaded ops).
        gp_Pnt loc(m_axOX, m_axOY, m_axOZ);
        gp_Dir zd(m_axDX, m_axDY, m_axDZ);
        gp_Dir xd(m_axXX, m_axXY, m_axXZ);
        gp_Ax3 ax3(loc, zd, xd);

        // ---- Swept-rod fast path: an EXTERNAL thread covering a PLAIN
        // full-cylinder body end to end builds natively (no boolean, ~200ms
        // vs minutes; see sweptRodThread). Detection is strict — exactly one
        // matching full-2pi cylinder + two planar caps perpendicular to the
        // axis, and the thread span covering the body's whole axial extent.
        // Anything else falls through to the proven boolean paths.
        // Standard + Rounded take the fast SWEEP: their SMOOTH arc profiles
        // (no sharp flanks) sweep cleanly, and the sweep gives Rounded the
        // gentle continuous sine wave a boolean can't (the rope cutter made
        // deep discrete scoops, the band loft made facets). The angular
        // profiles (trapezoidal/square/buttress) still take the boolean —
        // their sharp flanks ripple under the sweep.
        if (!m_isHole && fullCylinder && m_starts <= 1 &&
            (m_profile == ThreadProfile::Standard ||
             m_profile == ThreadProfile::Rounded)) {
            int nFaces = 0;
            bool shapeOk = true;
            for (TopExp_Explorer fx(body, TopAbs_FACE); fx.More(); fx.Next()) {
                ++nFaces;
                TopoDS_Face f = TopoDS::Face(fx.Current());
                Handle(Geom_Surface) gs = BRep_Tool::Surface(f);
                Handle(Geom_CylindricalSurface) cs =
                    Handle(Geom_CylindricalSurface)::DownCast(gs);
                Handle(Geom_Plane) pl = Handle(Geom_Plane)::DownCast(gs);
                if (!cs.IsNull()) {
                    if (std::abs(cs->Cylinder().Radius() - m_radius) > 1e-4 ||
                        std::abs(cs->Cylinder().Position().Direction()
                                     .Dot(zd)) < 0.9999)
                        shapeOk = false;
                } else if (!pl.IsNull()) {
                    if (std::abs(pl->Pln().Axis().Direction().Dot(zd)) < 0.9999)
                        shapeOk = false;
                } else {
                    shapeOk = false;
                }
            }
            // Axial extent from the body's vertices.
            double hMin = 1e300, hMax = -1e300;
            for (TopExp_Explorer vx(body, TopAbs_VERTEX); vx.More(); vx.Next()) {
                gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
                double h = gp_Vec(loc, p).Dot(gp_Vec(zd));
                hMin = std::min(hMin, h);
                hMax = std::max(hMax, h);
            }
            const bool fullSpan = hMin > -0.55 * m_pitch &&
                                  std::abs(hMax - m_length) < 0.55 * m_pitch;
            // Long rods chunk into <=35-turn glued segments inside
            // sweptRodThread; the global 300-turn runaway guard still applies.
            if (nFaces == 3 && shapeOk && fullSpan && hMax > hMin) {
                gp_Ax3 base(gp_Pnt(loc.X() + zd.X() * hMin,
                                   loc.Y() + zd.Y() * hMin,
                                   loc.Z() + zd.Z() * hMin), zd, xd);
                TopoDS_Shape rod = sweptRodThread(base, m_radius, hMax - hMin,
                                                  m_pitch, depth,
                                                  m_rightHanded, m_profile,
                                                  m_clearance);
                if (!rod.IsNull()) {
                    if (materializr::isVerbose())
                        std::fprintf(stderr, "[Thread] swept-rod fast path\n");
                    return rod;
                }
                std::fprintf(stderr, "[Thread] swept-rod declined — falling "
                                     "back to boolean cut\n");
            }
        }

        auto pt = [&](double rad, double dz) {
            return gp_Pnt(loc.X() + zd.X() * dz + xd.X() * rad,
                          loc.Y() + zd.Y() * dz + xd.Y() * rad,
                          loc.Z() + zd.Z() * dz + xd.Z() * rad);
        };

        // ---- Thread runout: extend the helix one turn past each FREE end so
        // the groove runs off the cylinder instead of stopping in a blunt
        // wall. An end is free when a probe point just beyond it (at
        // mid-groove radius) lies outside the body — a rod tip or a hole
        // mouth extends; a boss rooted in a plate or a blind hole bottom
        // stays exact so the cutter can't gouge surrounding material.
        double probeR = m_isHole ? (m_radius + 0.5 * depth)
                                 : (m_radius - 0.5 * depth);
        auto endIsFree = [&](double vBeyond) {
            try {
                BRepClass3d_SolidClassifier cls(body, pt(probeR, vBeyond), 1e-6);
                return cls.State() == TopAbs_OUT;
            } catch (...) { return false; }
        };
        double vLo = endIsFree(-0.6 * m_pitch) ? -0.5 * m_pitch : 0.0;
        double vHi = m_length +
                     (endIsFree(m_length + 0.6 * m_pitch) ? 0.5 * m_pitch : 0.0);

        // ---- Thread THROUGH a chamfer/taper. If a coaxial CONE adjoins an
        // end (a bevelled tip), run the thread over it: the groove tool keeps
        // cutting at the cylinder radius and the taper truncates the crests,
        // so the thread runs OUT along the chamfer like a real bolt lead-in
        // instead of stopping dead at the cylinder edge and leaving a smooth
        // bevel (Steve). Detected geometrically — past the end the body's
        // boundary radius drops smoothly from ~R (a taper), vs vanishing
        // (flat cap) or growing (a bigger coaxial neighbour). External only;
        // a hole's chamfer is a countersink, handled by its own runout.
        if (!m_isHole) {
            auto solidVR = [&](double dz, double rad) {
                try {
                    BRepClass3d_SolidClassifier c(body, pt(rad, dz), 1e-6);
                    return c.State() == TopAbs_IN;
                } catch (...) { return false; }
            };
            auto boundaryR = [&](double dz) -> double {   // max solid r, or -1
                if (!solidVR(dz, 0.05)) return -1.0;
                double lo = 0.0, hi = m_radius + 0.5;
                for (int it = 0; it < 18; ++it) {
                    double mid = 0.5 * (lo + hi);
                    if (solidVR(dz, mid)) lo = mid; else hi = mid;
                }
                return lo;
            };
            auto chamferRun = [&](bool topEnd) -> double {
                const double dir = topEnd ? 1.0 : -1.0;
                const double v0  = topEnd ? m_length : 0.0;
                const double step = 0.2 * m_pitch, maxRun = 4.0 * m_pitch;
                // Must START as a taper: reduced radius just past the end.
                const double brNear = boundaryR(v0 + dir * step);
                if (brNear < 0.0 || brNear > m_radius - 0.02) return 0.0;
                double prevR = m_radius, run = step;
                for (double d = 2.0 * step; d <= maxRun; d += step) {
                    const double br = boundaryR(v0 + dir * d);
                    if (br < 0.0) break;               // past the tip
                    if (br > m_radius + 0.02) break;   // bigger neighbour
                    if (br > prevR + 1e-3) break;      // radius grew
                    run = d; prevR = br;
                    if (br < m_radius - depth) break;  // no thread material past
                }
                return run;
            };
            const double topRun = chamferRun(true);
            const double botRun = chamferRun(false);
            if (topRun > 0.0) vHi = std::max(vHi, m_length + topRun + 0.3 * m_pitch);
            if (botRun > 0.0) vLo = std::min(vLo, -(botRun + 0.3 * m_pitch));
            if ((topRun > 0.0 || botRun > 0.0) && materializr::isVerbose())
                std::fprintf(stderr, "[Thread] chamfer run-through: top=%.2f "
                                     "bot=%.2f (vLo=%.2f vHi=%.2f)\n",
                             topRun, botRun, vLo, vHi);
        }

        turns = (vHi - vLo) / m_pitch;
        if (materializr::isVerbose())
            std::fprintf(stderr, "[Thread] runout: vLo=%.3f vHi=%.3f turns=%.1f\n",
                         vLo, vHi, turns);

        // Build the groove cutter for a helix span [lo, hi] along the axis
        // using the canonical OCCT threading construction (the "bottle
        // tutorial"): ThruSections between two half-ellipse wires drawn in
        // UV space on coaxial cylindrical surfaces. Unlike a MakePipeShell
        // sweep (whose helical solids the boolean classified erratically —
        // four variants, four different wrong answers), these tools cut
        // consistently, and the ellipse tips taper to nothing, giving
        // natural thread runout at both ends.
        // Closed UV band wire on a cylindrical surface, SYMMETRIC about the
        // helix line: tip → taper to the lower offset line (−off) → chunked
        // run parallel to the helix → taper to the far tip → taper up to the
        // upper line (+off) → chunked run back → taper to the tip. Both
        // flanks of the lofted groove slant equally (a seam ON the helix
        // lofts into a flat radial wall — Steve's "flat top, tapered
        // bottom"). All edges are straight 2D lines on the surface; long
        // runs are CHUNKED per turn — the tame-patch segmentation that makes
        // the boolean cut reliably (a single 30-turn lofted spline removes
        // nothing or worse; proven via the headless volume harness).
        // `vRef` anchors the helix PHASE: u = 0 at v = vRef (shifted by whole
        // turns so u stays near 0 within [lo, hi] — the surface is periodic
        // and phase mod 2π is what aligns the groove). The compound path
        // passes vRef = lo (the historical anchor); the per-turn path passes
        // the same vRef for every turn so all its tools lie on ONE helix.
        // `tlLo`/`tlHi` are the taper lengths at each end of the band; a
        // value <= 0 produces a SQUARE end (the band stops on a straight
        // axial edge instead of tapering to a tip). Interior per-turn tools
        // use square ends — their end walls sit INSIDE the neighbouring
        // void, away from every existing surface. The interlocking 0.5P
        // tapers were inherited from the compound recipe's runout, and they
        // are exactly the surfaces that made every neighbouring-void
        // boolean coin-flip (the whole shrink/debt/repair saga, all of
        // whose repairs OCCT inverted). Tapers remain only at the thread's
        // real ends, where runout belongs.
        // Asymmetric offsets (offLo below the helix centreline, offUp above)
        // let a profile's two flanks differ (buttress). The symmetric callers
        // pass offLo == offUp.
        auto bandWire = [&](const Handle(Geom_CylindricalSurface)& surf,
                            double uSign, double vRef, double lo, double hi,
                            double offLo, double offUp, int nSeg, double tlLo,
                            double tlHi) -> TopoDS_Wire {
            double kTurn = std::floor((lo - vRef) / lead + 1e-9);
            auto onHelix = [&](double v) {
                return gp_Pnt2d(
                    uSign * 2.0 * M_PI * ((v - vRef) / lead - kTurn), v);
            };
            auto offHelix = [&](double v, double dv) {
                gp_Pnt2d p = onHelix(v);
                return gp_Pnt2d(p.X(), p.Y() + dv);
            };
            bool sqLo = tlLo <= 0.0, sqHi = tlHi <= 0.0;
            gp_Pnt2d L1 = offHelix(lo + std::max(tlLo, 0.0), -offLo);
            gp_Pnt2d L2 = offHelix(hi - std::max(tlHi, 0.0), -offLo);
            gp_Pnt2d U2 = offHelix(hi - std::max(tlHi, 0.0), offUp);
            gp_Pnt2d U1 = offHelix(lo + std::max(tlLo, 0.0), offUp);
            BRepBuilderAPI_MakeWire mw;
            auto addChunked = [&](gp_Pnt2d p, gp_Pnt2d q, int n) {
                for (int i = 0; i < n; ++i) {
                    gp_Pnt2d a(p.X() + (q.X() - p.X()) * i / n,
                               p.Y() + (q.Y() - p.Y()) * i / n);
                    gp_Pnt2d b(p.X() + (q.X() - p.X()) * (i + 1) / n,
                               p.Y() + (q.Y() - p.Y()) * (i + 1) / n);
                    Handle(Geom2d_TrimmedCurve) sg =
                        GCE2d_MakeSegment(a, b).Value();
                    TopoDS_Edge e = BRepBuilderAPI_MakeEdge(sg, surf).Edge();
                    BRepLib::BuildCurves3d(e);
                    mw.Add(e);
                }
            };
            addChunked(L1, L2, nSeg);
            if (sqHi) {
                addChunked(L2, U2, 1); // square top: straight axial edge
            } else {
                gp_Pnt2d D = onHelix(hi);
                addChunked(L2, D, 1);
                addChunked(D, U2, 1);
            }
            addChunked(U2, U1, nSeg);
            if (sqLo) {
                addChunked(U1, L1, 1); // square bottom
            } else {
                gp_Pnt2d A = onHelix(lo);
                addChunked(U1, A, 1);
                addChunked(A, L1, 1);
            }
            return mw.Wire();
        };

        // Full-parameter cutter builder. `outClear`/`wFac`/`dJit` are the
        // per-turn variant ladder's micro-jitters (clearance, groove width
        // factor, extra depth) — they break the exact surface coincidences
        // between a turn's tool and the surfaces the previous turn's cut
        // created, which is what makes the kernel misclassify. `nSeg` is the
        // chunk count of the long runs; one chunk PER TURN, exactly.
        auto buildCutterOneStart = [&](double lo, double hi, double vRef,
                                       double outClear, double wFac, double dJit,
                                       int nSeg, double tlLo,
                                       double tlHi) -> TopoDS_Shape {
            try {
                double uSignH = m_rightHanded ? 1.0 : -1.0;
                // ROPE/KNUCKLE cutter for Rounded: sweep a CIRCLE along the
                // helix (centred at the surface radius). Cutting the in-material
                // half gives a genuinely SEMICIRCULAR groove — a band loft can
                // only make faceted straight flanks. One sweep + one cut, so
                // it's fast too. Groove radius rG sets both depth and half-
                // opening (a rope thread is inherently semicircular); capped so
                // a crest land survives between turns.
                if (m_profile == ThreadProfile::Rounded) {
                    double rG = std::min(depth + std::max(0.0, m_clearance),
                                         0.45 * m_pitch);
                    if (rG <= 1e-4) return {};
                    Handle(Geom_CylindricalSurface) cyl =
                        new Geom_CylindricalSurface(ax3, m_radius);
                    double u0 = uSignH * 2.0 * M_PI * (lo - vRef) / lead;
                    Handle(Geom2d_Line) l2d = new Geom2d_Line(
                        gp_Pnt2d(u0, lo),
                        gp_Dir2d(uSignH * 2.0 * M_PI, lead));
                    double segLen = std::sqrt(4.0 * M_PI * M_PI +
                                              lead * lead) *
                                    ((hi - lo) / lead);
                    TopoDS_Edge eHelix =
                        BRepBuilderAPI_MakeEdge(l2d, cyl, 0.0, segLen).Edge();
                    BRepLib::BuildCurves3d(eHelix);
                    TopoDS_Wire spine =
                        BRepBuilderAPI_MakeWire(eHelix).Wire();
                    BRepAdaptor_Curve hc(eHelix);
                    gp_Pnt p0; gp_Vec t0;
                    hc.D1(hc.FirstParameter(), p0, t0);
                    if (t0.Magnitude() < 1e-9) return {};
                    gp_Circ circ(gp_Ax2(p0, gp_Dir(t0)), rG);
                    TopoDS_Wire circW = BRepBuilderAPI_MakeWire(
                        BRepBuilderAPI_MakeEdge(circ).Edge()).Wire();
                    // MakePipeShell + MakeSolid caps the tube ends into a solid
                    // (plain MakePipe returns only the swept SHELL, which the
                    // boolean can't cut with).
                    BRepOffsetAPI_MakePipeShell pipe(spine);
                    pipe.Add(circW);
                    pipe.Build();
                    if (!pipe.IsDone() || !pipe.MakeSolid()) return {};
                    return pipe.Shape();
                }

                // Outer surface on the material-free side of the face (clean
                // detachment without near-tangency); inner surface at the
                // groove apex. (For a hole, "outer" is inside the void.)
                // m_clearance pulls the whole groove DEEPER on the material
                // side so a printed thread fits its mate (crest of the
                // complementary thread clears by that gap).
                double d   = depth + dJit + std::max(0.0, m_clearance);
                double rOut = m_isHole ? (m_radius - outClear)
                                       : (m_radius + outClear);
                double rIn  = m_isHole ? (m_radius + d) : (m_radius - d);

                // u+ is a right-handed screw for any face axis orientation
                // (chirality is invariant under axis flip).
                double uSign = m_rightHanded ? 1.0 : -1.0;
                // Mouth half-width from the profile's opening fraction (Standard
                // = 0.4375·pitch, the shipped 7/8-pitch opening).
                double mouthHalf = baseMouthHalf * wFac;
                // Flank clearance: widen the groove by `clearance` on EACH
                // flank (thins the ridge) so a printed thread clears its mate
                // on the sides, not just radially (m_clearance already deepens
                // the groove above). Same widening for external AND internal —
                // a thinner ridge clears a nominal mate either way; the radial
                // inversion for a hole is handled by rIn/rOut. `flankClear` is
                // clamped at buildResult scope so the cutter and the volume /
                // width gates all measure the SAME widened groove.
                const double fc = flankClear;

                // Build the groove as a stack of 2-band loft SLABS between
                // consecutive bands, fused. A single multi-section ThruSections
                // twists into a non-intersecting tool ("removed nothing"); each
                // 2-band slab is the proven shipped loft, so N-1 of them fused
                // give the (piecewise-linear, arc-approximating) profile
                // robustly. Two-band profiles are a single slab = shipped path.
                auto slab = [&](const GrooveBand& a,
                                const GrooveBand& b) -> TopoDS_Shape {
                    double ra = rOut + (rIn - rOut) * a.rFrac;
                    double rb = rOut + (rIn - rOut) * b.rFrac;
                    Handle(Geom_CylindricalSurface) sa =
                        new Geom_CylindricalSurface(ax3, ra);
                    Handle(Geom_CylindricalSurface) sb =
                        new Geom_CylindricalSurface(ax3, rb);
                    BRepOffsetAPI_ThruSections ts(Standard_True);
                    ts.AddWire(bandWire(sa, uSign, vRef, lo, hi,
                                        mouthHalf * a.offLo + fc,
                                        mouthHalf * a.offUp + fc,
                                        nSeg, tlLo, tlHi));
                    ts.AddWire(bandWire(sb, uSign, vRef, lo, hi,
                                        mouthHalf * b.offLo + fc,
                                        mouthHalf * b.offUp + fc,
                                        nSeg, tlLo, tlHi));
                    ts.CheckCompatibility(Standard_False);
                    ts.Build();
                    return ts.IsDone() ? ts.Shape() : TopoDS_Shape();
                };
                TopoDS_Shape toolShape;
                for (size_t i = 1; i < gSpec.bands.size(); ++i) {
                    TopoDS_Shape s = slab(gSpec.bands[i-1], gSpec.bands[i]);
                    if (s.IsNull()) return {};
                    if (toolShape.IsNull()) { toolShape = s; continue; }
                    BRepAlgoAPI_Fuse f;
                    TopTools_ListOfShape A, B; A.Append(toolShape); B.Append(s);
                    f.SetArguments(A); f.SetTools(B);
                    f.SetFuzzyValue(1e-5);
                    f.Build();
                    if (!f.IsDone() || f.Shape().IsNull()) return {};
                    toolShape = f.Shape();
                }
                if (toolShape.IsNull()) return {};
                // NOTE: do NOT "fix" the orientation even if GProp reports a
                // negative volume — the integrator mis-reads the helical
                // seam, but the boolean classifies this solid correctly.
                return toolShape;
            } catch (...) { return {}; }
        };
        // N-start wrapper: the same tool built once per start, phase-shifted
        // one PITCH of vRef each (with the helix lead = N·P, +P in vRef is
        // exactly +2π/N of phase). The starts' grooves share the crest land
        // a single-start P-thread would have, so they never touch — a
        // lightweight COMPOUND (no fuse) feeds the boolean as one tool.
        // Single start returns the shipped tool bit-identically.
        auto buildCutterEx = [&](double lo, double hi, double vRef,
                                 double outClear, double wFac, double dJit,
                                 int nSeg, double tlLo,
                                 double tlHi) -> TopoDS_Shape {
            const int nStarts = std::max(1, m_starts);
            if (nStarts == 1)
                return buildCutterOneStart(lo, hi, vRef, outClear, wFac, dJit,
                                           nSeg, tlLo, tlHi);
            TopoDS_Compound comp;
            BRep_Builder bb;
            bb.MakeCompound(comp);
            for (int s = 0; s < nStarts; ++s) {
                TopoDS_Shape one = buildCutterOneStart(
                    lo, hi, vRef + s * m_pitch, outClear, wFac, dJit, nSeg,
                    tlLo, tlHi);
                if (one.IsNull()) return {};
                bb.Add(comp, one);
            }
            return comp;
        };
        // The historical compound-tool builder (full-cylinder fast path) —
        // parameters bit-identical to every shipped release. Chunks are one
        // per HELIX turn, so the lead sets the count.
        auto buildCutter = [&](double lo, double hi) -> TopoDS_Shape {
            double t = (hi - lo) / lead;
            int nSeg = std::max(4, static_cast<int>(std::ceil(t)));
            return buildCutterEx(lo, hi, lo, 0.10, 1.0, 0.0, nSeg,
                                 0.5 * m_pitch, 0.5 * m_pitch);
        };

        auto shapeVol = [](const TopoDS_Shape& s) -> double {
            try {
                GProp_GProps p;
                BRepGProp::VolumeProperties(s, p);
                return p.Mass();
            } catch (...) { return 0.0; }
        };
        const double bodyVol = shapeVol(body);

        auto cutOn = [&](const TopoDS_Shape& target, const TopoDS_Shape& tool,
                         double fuzz) -> TopoDS_Shape {
            try {
                BRepAlgoAPI_Cut cut;
                TopTools_ListOfShape args, tools;
                args.Append(target);
                tools.Append(tool);
                cut.SetArguments(args);
                cut.SetTools(tools);
                cut.SetFuzzyValue(fuzz);
                // The per-turn path runs dozens of these sequentially on
                // the worker thread; let each boolean use the machine
                // (Steve watched a 27-turn rod pin ONE of 16 cores for a
                // minute).
                cut.SetRunParallel(Standard_True);
                if (m_cancelTok) {
                    Handle(ThreadCancelBreak) brk =
                        new ThreadCancelBreak(m_cancelTok);
                    cut.Build(brk->Start());
                } else {
                    cut.Build();
                }
                if (!cut.IsDone()) return {};
                return cut.Shape();
            } catch (...) { return {}; }
        };
        auto cancelRequested = [&]() {
            return m_cancelTok && m_cancelTok->load();
        };
        auto cutOnce = [&](const TopoDS_Shape& tool) -> TopoDS_Shape {
            return cutOn(body, tool, 1.0e-3);
        };

        // ---- Shape-aware cut validation. Volume bands alone cannot tell a
        // real groove from a WRONG-MATERIAL cut at similar volume (Steve's
        // stacked-disc bodies: the boolean removes a full-circumference slab
        // between grooves instead of the groove — comparable volume, garbage
        // shape, tessellator crash). Classifier probes can: after a turn's
        // cut, points in the GROOVE band must be void and points in the
        // CREST band must still be solid. A disc cut eats the crest; an
        // imprint no-op leaves the groove solid. Both fail instantly.
        gp_Vec ydv = gp_Vec(zd).Crossed(gp_Vec(xd));
        auto cylPt = [&](double rad, double theta, double dz) {
            double c = std::cos(theta), s = std::sin(theta);
            return gp_Pnt(
                loc.X() + zd.X() * dz + (xd.X() * c + ydv.X() * s) * rad,
                loc.Y() + zd.Y() * dz + (xd.Y() * c + ydv.Y() * s) * rad,
                loc.Z() + zd.Z() * dz + (xd.Z() * c + ydv.Z() * s) * rad);
        };
        auto isIn = [&](const TopoDS_Shape& s, const gp_Pnt& p) {
            try {
                BRepClass3d_SolidClassifier c(s, p, 1e-7);
                return c.State() == TopAbs_IN;
            } catch (...) { return false; }
        };
        // Score the helix span [lo, hi] of `post` against pre-cut state
        // `pre`: along the groove helix, the groove point must have gone
        // OUT and the crest point (half a pitch up, same angle) stayed IN.
        // The probe angle is derived from v directly (θ = uSign·2π·(v−vLo)
        // /P), so the probes are GRID-INDEPENDENT — they follow the helix
        // wherever the zone boundaries sit. Only samples where BOTH points
        // were solid in `pre` count (split-away regions and cross-holes
        // are legitimately void); taper/runout spans at the very ends are
        // skipped. Returns {good, considered}.
        struct ProbeScore { int good = 0, considered = 0; };
        auto turnProbes = [&](const TopoDS_Shape& pre, const TopoDS_Shape& post,
                              double lo, double hi) -> ProbeScore {
            const int K = 16;
            double uSign = m_rightHanded ? 1.0 : -1.0;
            double rG = m_isHole ? (m_radius + 0.5 * depth)
                                 : (m_radius - 0.5 * depth);
            double rC = m_isHole ? (m_radius + 0.25 * depth)
                                 : (m_radius - 0.25 * depth);
            ProbeScore sc;
            for (int k = 0; k < K; ++k) {
                double vG = lo + (hi - lo) * (k + 0.5) / K;
                if (vG < vLo + 0.75 * m_pitch || vG > vHi - 0.75 * m_pitch)
                    continue; // taper/runout — groove intentionally shallow
                double vC = vG + 0.5 * m_pitch;
                // Phase follows start 0's helix (lead-pitched); the crest
                // offset vC and the width probes stay P-form — at a fixed
                // angle, grooves pass every P for any start count.
                double th = uSign * 2.0 * M_PI * (vG - vLo) / lead;
                gp_Pnt pg = cylPt(rG, th, vG), pc = cylPt(rC, th, vC);
                if (!isIn(pre, pg) || !isIn(pre, pc)) continue;
                ++sc.considered;
                bool okSample = !isIn(post, pg) && isIn(post, pc);
                // WIDTH check, mid-span samples only (so the probes stay
                // inside this cut's own territory): a full-width groove is
                // void a short way off the centreline at this depth; a
                // TAPER-NARROWED section is still solid. The probe offset
                // tracks the PROFILE's half-opening at quarter depth (0.85 of
                // it, inside for any profile) instead of a fixed 0.30P tuned to
                // the old trapezoid — else narrow-opening profiles (square,
                // rounded) fail the gate spuriously.
                if (okSample && k * 10 >= 3 * K && k * 10 <= 7 * K) {
                    double hLo, hUp;
                    grooveHalfAt(gSpec, baseMouthHalf, 0.25, hLo, hUp);
                    gp_Pnt w1 = cylPt(rC, th, vG - 0.85 * hLo);
                    gp_Pnt w2 = cylPt(rC, th, vG + 0.85 * hUp);
                    if (isIn(pre, w1) && isIn(post, w1)) okSample = false;
                    if (isIn(pre, w2) && isIn(post, w2)) okSample = false;
                }
                if (okSample) ++sc.good;
            }
            return sc;
        };

        // Validate a cut RESULT (post-cut body) against the pre-cut `body`:
        // volume band + per-turn shape probes + heal. Shared by the single
        // compound cut and the chunked cut.
        auto validateResult = [&](TopoDS_Shape res) -> TopoDS_Shape {
            if (res.IsNull()) return {};
            double v = shapeVol(res);
            // A real groove removes meaningful material. Accept only
            // 0 < v < body − ε: rejects growth (inverted classification) AND
            // no-op cuts that merely imprint edges (volume unchanged).
            double minRemoval = std::max(1e-2, bodyVol * 1e-4);
            if (v > bodyVol - minRemoval || v < 0.0) {
                std::fprintf(stderr, "[Thread] cut removed nothing or grew "
                                     "(%.2f vs body %.2f) — rejecting\n",
                             v, bodyVol);
                return {};
            }
            // Shape-aware gate: wrong-material cuts with plausible volume reach
            // the tessellator and SEGFAULT; classifier probes catch them.
            // Standard stays STRICT (all probes perfect); generalized profiles'
            // probe offsets are approximate, so their gate only catches GROSS
            // errors (a majority wrong) — demoting them to per-turn is what
            // makes them slow, and volume + validity already backstop.
            int nTurns = static_cast<int>(std::ceil((vHi - vLo) / m_pitch));
            for (int t = 0; t < nTurns; ++t) {
                ProbeScore sc = turnProbes(body, res, vLo + t * m_pitch,
                                           vLo + (t + 1) * m_pitch);
                const bool gross = m_profile == ThreadProfile::Standard
                                       ? (sc.good != sc.considered)
                                       : (sc.good * 2 < sc.considered);
                if (sc.considered >= 5 && gross) {
                    std::fprintf(stderr, "[Thread] cut imperfect at turn %d "
                                         "(%d/%d probes) — demoting\n",
                                 t, sc.good, sc.considered);
                    return {};
                }
            }
            if (!BRepCheck_Analyzer(res).IsValid()) {
                ShapeFix_Shape fixer(res);
                fixer.Perform();
                res = fixer.Shape();
            }
            return res;
        };

        auto tryCut = [&](const TopoDS_Shape& tool) -> TopoDS_Shape {
            if (tool.IsNull()) return {};
            TopoDS_Shape res = cutOnce(tool);
            // A CUT must shrink the body; a helical tool's classification can
            // invert on a partial body — retry with the reversed tool.
            if (!res.IsNull() && shapeVol(res) > bodyVol + 1e-3) {
                std::fprintf(stderr, "[Thread] cut inverted (vol grew) — "
                                     "retrying with reversed tool\n");
                TopoDS_Shape rev = tool;
                rev.Reverse();
                res = cutOnce(rev);
            }
            return validateResult(res);
        };

        // SEGMENT-AND-GLUE (the sweep path's O(N) pattern applied to the
        // boolean). Cutting all N turns as one tool — or chunking one GROWING
        // body — is O(N²): every boolean scans the whole accumulating shape.
        // Instead, thread SHORT fresh cylinder segments independently (each cut
        // is O(1) — a short rod × a short tool) and GLUE them at whole-pitch
        // boundaries, where the notched cross-sections are identical (same
        // phase), so BOPAlgo_GlueFull skips the face-intersection machinery.
        // Total is O(N) and the seams are clean by construction. Only the true
        // rod ends taper (runout). Full external plain cylinders only (the
        // generalized-profile case that can't take the fast sweep).
        auto segmentAndGlue = [&]() -> TopoDS_Shape {
            // Rod axial extent (project body vertices on the axis).
            double zMin = 1e300, zMax = -1e300;
            for (TopExp_Explorer vx(body, TopAbs_VERTEX); vx.More(); vx.Next()) {
                gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
                double h = gp_Vec(loc, p).Dot(gp_Vec(zd));
                zMin = std::min(zMin, h);
                zMax = std::max(zMax, h);
            }
            if (zMax <= zMin + 1e-6) return {};
            const double segLen = 3.0 * m_pitch;   // ~3 turns / segment
            // Build every threaded segment first, then fuse them ALL in ONE
            // GlueFull call. Fusing sequentially onto a growing rod re-scans
            // the whole accumulating shape each time (O(N²), 258s at 20 turns);
            // one fuse of the disjoint segments touches only the coincident
            // interfaces once (O(N)).
            std::vector<TopoDS_Shape> segs;
            double z = zMin;
            int guard = 0;
            while (z < zMax - 1e-6 && ++guard < 500) {
                double zEnd = std::min(z + segLen, zMax);
                if (zEnd < zMax - 1e-6) {
                    double nP = std::round((zEnd - zMin) / m_pitch);
                    zEnd = zMin + std::max(1.0, nP) * m_pitch;
                    zEnd = std::min(zEnd, zMax);
                }
                if (zEnd <= z + 1e-6) break;
                const bool firstSeg = z <= zMin + 1e-6;
                const bool lastSeg  = zEnd >= zMax - 1e-6;

                gp_Pnt segO = loc.Translated(gp_Vec(zd) * z);
                TopoDS_Shape segCyl = BRepPrimAPI_MakeCylinder(
                    gp_Ax2(segO, zd, xd), m_radius, zEnd - z).Shape();

                // Local groove tool: same helix phase (vRef = vLo); runout
                // taper ONLY at the true rod ends, square (continuous) at
                // interior boundaries so neighbours meet cleanly.
                double lo = firstSeg ? vLo : z;
                double hi = lastSeg  ? vHi : zEnd;
                double tlLo = firstSeg ? 0.5 * m_pitch : 0.0;
                double tlHi = lastSeg  ? 0.5 * m_pitch : 0.0;
                int ns = std::max(4, (int)std::ceil((hi - lo) / lead));
                TopoDS_Shape tool =
                    buildCutterEx(lo, hi, vLo, 0.10, 1.0, 0.0, ns, tlLo, tlHi);
                if (tool.IsNull()) return {};
                TopoDS_Shape seg = cutOn(segCyl, tool, 1.0e-3);
                if (seg.IsNull()) return {};
                segs.push_back(seg);
                z = zEnd;
            }
            if (segs.empty()) return {};
            if (segs.size() == 1) return validateResult(segs[0]);
            BRepAlgoAPI_Fuse f;
            TopTools_ListOfShape A, B;
            A.Append(segs[0]);
            for (size_t i = 1; i < segs.size(); ++i) B.Append(segs[i]);
            f.SetArguments(A);
            f.SetTools(B);
            f.SetFuzzyValue(1e-5);
            f.SetGlue(BOPAlgo_GlueFull);
            f.Build();
            if (!f.IsDone() || f.Shape().IsNull()) return {};
            return validateResult(f.Shape());
        };

        // PER-TURN SEQUENTIAL fallback: one boolean per turn, each validated
        // by removed volume against the analytic per-turn groove volume,
        // failed turns retried through a variant ladder of micro-jitters.
        // This is how partial cylinders (lengthwise split halves) and
        // interrupted cylinders (cross-holes) get threaded — the compound
        // tool coin-flips on those, but single-turn cuts with validation
        // land 20-21/21 turns in the harness, and a turn that fails every
        // variant is SKIPPED (a short groove gap), never garbage.
        auto perTurnCut = [&]() -> TopoDS_Shape {
            int nT = static_cast<int>(std::ceil((vHi - vLo) / m_pitch));
            if (nT < 1 || nT > 120) return {};
            // Analytic groove volume of one turn: the PROFILE's cross-section
            // area (integrated from its band table) swept at mid-groove radius.
            double area = grooveArea(gSpec, baseMouthHalf, depth)
                        + 2.0 * flankClear * depth;   // flank clearance widens it
            double rMid = m_isHole ? (m_radius + 0.5 * depth)
                                   : (m_radius - 0.5 * depth);
            double analytic = area * 2.0 * M_PI * rMid;
            if (analytic <= 1e-9) return {};

            struct Variant {
                double fuzz, clear_, wFac, dJit, ovl;
                bool rev, taper;
            };
            // TWO complementary tool families (matrix-proven):
            //   SQUARE (taper=false): square-ended band segments, `ovl`
            //     reaching into the previous void. Perfect + natively
            //     VALID on full and cross-holed rods; no-ops/inverts on
            //     lengthwise halves.
            //   TAPER (taper=true): classic [zone-0.5P, zone+0.5P] tools
            //     with 0.5P tapers. Perfect on halves; coin-flips every
            //     3rd turn on full rods.
            // The engine starts square-first and flips its preference for
            // the rest of the body the first time the taper family wins a
            // turn (bodies are homogeneous in which family works).
            const Variant variants[] = {
                // square family
                {1e-3, 0.10, 1.000, 0.000, 0.06, false, false},
                {1e-3, 0.10, 0.995, 0.000, 0.06, false, false},
                {1e-3, 0.10, 1.000, 0.005, 0.06, false, false},
                {1e-3, 0.10, 1.000, 0.000, 0.11, false, false},
                {1e-3, 0.10, 1.000, 0.000, 0.06, true,  false},
                {2e-3, 0.15, 0.990, 0.010, 0.13, false, false},
                // taper family
                {1e-3, 0.10, 1.000, 0.000, 0.00, false, true},
                {1e-3, 0.10, 0.995, 0.000, 0.00, false, true},
                {1e-3, 0.10, 1.000, 0.005, 0.00, false, true},
                {1e-3, 0.10, 1.000, 0.000, 0.00, true,  true},
                {5e-4, 0.10, 0.995, 0.005, 0.00, false, true},
            };
            const int nV = sizeof(variants) / sizeof(variants[0]);
            bool taperFirst = false;

            TopoDS_Shape cur = body;
            double curVol = bodyVol;
            int failed = 0, skipped = 0;
            // STRICTLY bottom-up: each square tool's blunt top wall (cut
            // into material at zoneHi) is erased by the NEXT turn's
            // overlapping bottom.
            //
            // CREST-ALIGNED ZONE GRID: grooves sit at v = vLo + n·P (the
            // helix anchor), so zone boundaries go at vLo + (n±0.5)·P —
            // mid-crest — and each square tool owns ONE whole groove. The
            // original grid put boundaries exactly ON the grooves: every
            // square end wall sliced through a groove, stacking half-open
            // groove stubs at each boundary until the booleans collapsed
            // outright (Steve's 16mm rod: turns 0-8 OK, everything after
            // failed). nT+1 zones cover [vLo, vHi] with clipped ends.
            // End zones: when a thread end is FREE (vLo/vHi already carry
            // the runout extension), the end zone may reach 0.5P further
            // into the air so its runout taper has somewhere to live.
            // Clamping the first zone to [vLo, vLo+0.5P] made it exactly
            // as long as its own taper — a degenerate tool that cut
            // nothing, deleting the first half-turn of groove (Steve's
            // "weird non-tapered end").
            bool botFree = vLo < -1e-9;
            bool topFree = vHi > m_length + 1e-9;
            double gridLo = botFree ? vLo - 0.5 * m_pitch : vLo;
            double gridHi = topFree ? vHi + 0.5 * m_pitch : vHi;
            // Build the zone list, then MERGE face-crossing end zones into
            // their material-anchored neighbours. An end zone whose groove
            // centre sits beyond the rod face is void-dominated — OCCT
            // no-ops the cut and the face-exit flank sliver never gets
            // removed (the blunt triangular pocket below the rim in
            // Steve's screenshot). Merged, the sliver rides along on a
            // tool that owns a full in-material groove.
            std::vector<std::pair<double, double>> zones;
            for (int i = 0; i <= nT; ++i) {
                double zlo = std::max(gridLo, vLo + (i - 0.5) * m_pitch);
                double zhi = std::min(gridHi, vLo + (i + 0.5) * m_pitch);
                if (zhi - zlo < 0.05 * m_pitch) continue;
                zones.push_back({zlo, zhi});
            }
            while (zones.size() > 1 &&
                   zones[0].second <= 0.5 * m_pitch + 1e-9) {
                zones[1].first = zones[0].first;
                zones.erase(zones.begin());
            }
            while (zones.size() > 1 &&
                   zones.back().first >= m_length - 0.5 * m_pitch - 1e-9) {
                zones[zones.size() - 2].second = zones.back().second;
                zones.pop_back();
            }
            for (size_t zi = 0; zi < zones.size(); ++zi) {
                if (cancelRequested()) {
                    std::fprintf(stderr, "[Thread] per-turn: cancelled at "
                                         "turn %zu\n", zi);
                    return {};
                }
                int i = static_cast<int>(zi);
                double zoneLo = zones[zi].first;
                double zoneHi = zones[zi].second;
                bool first = (zi == 0);
                bool last = (zi + 1 == zones.size());
                bool ok = false;
                bool noMaterial = false;
                // Per-variant failure accounting — printed when a whole
                // turn fails so a field log is diagnosable without a
                // rebuild (finding the last regression cost Steve a CPU
                // fan and me a blind guess).
                int rjNull = 0, rjGrew = 0, rjBig = 0, rjProbe = 0;
                int lastGood = -1, lastDen = -1;
                // Best imperfect candidate across the ladder: a variant
                // that cleared ≥75% of probes but left an arc of the groove
                // filled (Steve's slotted rod: "one of the half grooves got
                // filled in"). Prefer ANY perfect variant; adopt the best
                // imperfect one only after the whole ladder has had a shot.
                TopoDS_Shape bestRes;
                double bestVol = 0.0;
                int bestScore = -1, bestDen = 1, bestVar = -1;
                for (int pass = 0; pass < 2 && !ok; ++pass) {
                    // pass 0 = preferred family, pass 1 = the other
                    bool wantTaper = (pass == 0) ? taperFirst : !taperFirst;
                for (int v = 0; v < nV && !ok; ++v) {
                    const Variant& va = variants[v];
                    if (va.taper != wantTaper) continue;
                    double toolLo, toolHi, tlLo, tlHi;
                    if (va.taper) {
                        toolLo = zoneLo - 0.5 * m_pitch;
                        toolHi = zoneHi + 0.5 * m_pitch;
                        tlLo = tlHi = 0.5 * m_pitch;
                    } else {
                        // Square ends, bottom overlapping the previous
                        // void. Real thread ends keep runout tapers —
                        // shortened when the zone is clamped (non-free
                        // end) so the tool never degenerates to nothing.
                        toolLo = first ? zoneLo : zoneLo - va.ovl * m_pitch;
                        toolHi = zoneHi;
                        double span = toolHi - toolLo;
                        tlLo = first ? std::min(0.5 * m_pitch, 0.6 * span)
                                     : 0.0;
                        tlHi = last ? std::min(0.5 * m_pitch,
                                               0.6 * span - tlLo * 0.5)
                                    : 0.0;
                        if (tlHi < 0.0) tlHi = 0.0;
                    }
                    int nSeg = std::max(1, static_cast<int>(std::lround(
                                               (toolHi - toolLo) / m_pitch)));
                    TopoDS_Shape tool = buildCutterEx(
                        toolLo, toolHi, vLo, va.clear_, va.wFac, va.dJit,
                        nSeg, tlLo, tlHi);
                    if (tool.IsNull()) continue;
                    if (va.rev) tool.Reverse();
                    TopoDS_Shape res = cutOn(cur, tool, va.fuzz);
                    if (res.IsNull()) { ++rjNull; continue; }
                    double after = shapeVol(res);
                    if (after <= 0.0) { ++rjNull; continue; }
                    double removed = curVol - after;
                    // Inverted / wrong-material cuts remove far more than
                    // one groove turn ever could (band hi proven at 2.5×:
                    // a turn after a skipped neighbour legitimately
                    // catches up its missed groove too)...
                    if (removed > 2.5 * analytic) { ++rjBig; continue; }
                    // ...and a GROWN body is an inverted classification.
                    if (removed < -1e-6) { ++rjGrew; continue; }
                    // FACE-EXIT zones legitimately remove a tiny sliver —
                    // the runout flank wedge where the helix crosses the
                    // rod's end face (NOT necessarily the grid's first/
                    // last zone). The normal 2% floor swallowed it ("no
                    // material") and left the groove ending in a blunt
                    // triangular pocket below the rim (Steve's screenshot).
                    bool faceExit = zoneLo < 0.75 * m_pitch ||
                                    zoneHi > m_length - 0.75 * m_pitch;
                    double matFloor = faceExit ? 0.001 * analytic
                                               : 0.02 * analytic;
                    if (removed < matFloor) {
                        // A clean boolean that removed (almost) nothing.
                        // Either this turn's groove isn't in this body
                        // (split-away region) — or the cut no-op'd the way
                        // awkward bodies do and a LATER VARIANT will land
                        // it. Remember the benign outcome but keep trying;
                        // do NOT adopt `res` (imprint edges pollute).
                        noMaterial = true;
                        continue;
                    }
                    // Volume is in band — but at single-turn scale a
                    // wrong-material disc cut removes a volume comparable
                    // to a real groove (Steve's stacked-disc rod). Shape
                    // probes arbitrate.
                    ProbeScore sc = turnProbes(cur, res, zoneLo, zoneHi);
                    if (sc.considered == 0) {
                        // No probeable material yet volume moved: distrust.
                        noMaterial = true;
                        continue;
                    }
                    // < 5 measurable samples = partial end turn, mostly
                    // runout: probes are INCONCLUSIVE — trust the volume
                    // band that already passed.
                    if (sc.good == sc.considered || sc.considered < 5) {
                        cur = res;
                        curVol = after;
                        ok = true;
                        // Adapt: if the non-preferred family won, prefer it
                        // for the rest of this body.
                        if (va.taper != taperFirst) taperFirst = va.taper;
                    } else if (sc.good * 4 >= sc.considered * 3 &&
                               sc.good * bestDen > bestScore * sc.considered) {
                        bestRes = res;
                        bestVol = after;
                        bestScore = sc.good;
                        bestDen = sc.considered;
                        bestVar = v;
                    } else {
                        ++rjProbe;
                        lastGood = sc.good;
                        lastDen = sc.considered;
                    }
                }
                }
                if (!ok && !bestRes.IsNull()) {
                    std::fprintf(stderr, "[Thread] turn %d: best variant %d "
                                         "imperfect (%d/%d probes) — "
                                         "adopting\n",
                                 i, bestVar, bestScore, bestDen);
                    cur = bestRes;
                    curVol = bestVol;
                    ok = true;
                }
                if (!ok) {
                    if (noMaterial) ++skipped;
                    else {
                        ++failed;
                        std::fprintf(stderr, "[Thread] turn %d failed: "
                                             "null=%d grew=%d big=%d "
                                             "probe=%d (last %d/%d)\n",
                                     i, rjNull, rjGrew, rjBig, rjProbe,
                                     lastGood, lastDen);
                    }
                }
                // Bail as soon as the failure budget is blown — each failed
                // turn burns the FULL variant ladder (9 booleans), and a
                // body that fails this often isn't going to be saved by the
                // remaining turns. This runs synchronously during reflow
                // replays; keep the doomed case bounded.
                if (failed > std::max(2, nT / 5)) {
                    std::fprintf(stderr, "[Thread] per-turn: aborting at "
                                         "turn %d (%d failures)\n", i, failed);
                    return {};
                }
            }
            if (materializr::isVerbose())
                std::fprintf(stderr, "[Thread] per-turn: %d turns, %d skipped "
                                     "(no material), %d failed\n",
                             nT, skipped, failed);
            // The whole pass must have removed SOMETHING — a body that no
            // groove intersects is a no-op, and no-ops suspend (same rule
            // as the compound path's volume guard).
            double minRemoval = std::max(1e-2, bodyVol * 1e-4);
            if (curVol > bodyVol - minRemoval) return {};
            if (!BRepCheck_Analyzer(cur).IsValid()) {
                ShapeFix_Shape fixer(cur);
                fixer.Perform();
                cur = fixer.Shape();
                // Still broken after healing = garbage (a stale-radius cut
                // into a resized rod landed an invalid protruding body that
                // passed the volume gates). Fail honestly — the recut path
                // suspends the step instead of shipping the junk.
                if (!BRepCheck_Analyzer(cur).IsValid()) {
                    std::fprintf(stderr, "[Thread] per-turn: result invalid "
                                         "after heal — rejecting\n");
                    return {};
                }
            }
            return cur;
        };

        TopoDS_Shape result;
        // INTERNAL ROUNDED — SLAB-WISE RING SPLICE. Probe-mapped OCCT
        // reality (probe_ring_splice): the flush ring-splice unit works up
        // to ~4-5 turns and FAILS beyond (6+ turns: the seam fuse loses the
        // body / fills the bore / inverts, in every fuse mode) — and a ring
        // ending MID-bore breaks the fuse at any length. So slice the
        // CLEARED body into <=4-turn slabs at whole-pitch boundaries
        // (planar Common, plain tool), splice each slab's SHORT FLUSH ring
        // (the proven unit), and glue the threaded slabs back at their
        // identical planar interfaces (the swept-rod chunking trick at
        // body level). Silhouette-verified continuous sine at 10 and 20
        // turns, exact volumes. A fuse can still coin-flip on some slab
        // splits (a 3-turn tail did once), so the whole construction
        // retries at 4, 3, then 5 turns per slab.
        if (m_isHole && m_starts <= 1 &&
            m_profile == ThreadProfile::Rounded) {
            const char* why = "no slab attempt succeeded";
            try {
                const double minor = m_radius + std::max(0.0, m_clearance);
                const double major = minor + depth;
                double hMin = 1e300, hMax = -1e300;
                for (TopExp_Explorer vx(body, TopAbs_VERTEX); vx.More();
                     vx.Next()) {
                    gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
                    double h = gp_Vec(loc, p).Dot(gp_Vec(zd));
                    hMin = std::min(hMin, h);
                    hMax = std::max(hMax, h);
                }
                const double sLo = std::max(vLo, hMin);
                const double sHi = std::min(vHi, hMax);
                auto axAt = [&](double v) {
                    return gp_Ax2(gp_Pnt(loc.X() + zd.X() * v,
                                         loc.Y() + zd.Y() * v,
                                         loc.Z() + zd.Z() * v), zd, xd);
                };
                auto fuseGlued = [&](const TopoDS_Shape& a,
                                     const TopoDS_Shape& b,
                                     BOPAlgo_GlueEnum glue,
                                     double fuzz) -> TopoDS_Shape {
                    BRepAlgoAPI_Fuse f;
                    TopTools_ListOfShape fa, ft;
                    fa.Append(a);
                    ft.Append(b);
                    f.SetArguments(fa);
                    f.SetTools(ft);
                    if (fuzz > 0.0) f.SetFuzzyValue(fuzz);
                    f.SetGlue(glue);
                    f.SetRunParallel(Standard_True);
                    if (m_cancelTok) {
                        Handle(ThreadCancelBreak) brk =
                            new ThreadCancelBreak(m_cancelTok);
                        f.Build(brk->Start());
                    } else {
                        f.Build();
                    }
                    return f.IsDone() ? f.Shape() : TopoDS_Shape();
                };
                // Rough radial bound of the body for the slab-slicing tool.
                double rBig = major * 2.0 + 10.0;
                {
                    Bnd_Box bb;
                    BRepBndLib::Add(body, bb);
                    if (!bb.IsVoid()) {
                        double x1, y1, z1, x2, y2, z2;
                        bb.Get(x1, y1, z1, x2, y2, z2);
                        rBig = std::hypot(std::hypot(x2 - x1, y2 - y1),
                                          z2 - z1) + major;
                    }
                }
                if (sHi - sLo > 0.1 * m_pitch) {
                    // Bore the whole body to major once (padded plain tool).
                    TopoDS_Shape cleared = cutOn(
                        BRepBuilderAPI_Copy(body).Shape(),
                        BRepPrimAPI_MakeCylinder(axAt(sLo - 1.0), major,
                                                 (sHi - sLo) + 2.0).Shape(),
                        1.0e-3);
                    if (!cleared.IsNull()) {
                        for (double slabTurns : {4.0, 3.0, 5.0}) {
                            if (cancelRequested()) break;
                            const double slabLen = slabTurns * m_pitch;
                            bool ok = true;
                            TopoDS_Shape acc;
                            double partsVol = 0.0;
                            for (double z0 = sLo;
                                 z0 < sHi - 1e-9 && ok;) {
                                if (cancelRequested()) { ok = false; break; }
                                double z1 = std::min(sHi, z0 + slabLen);
                                if (z1 < sHi - 1e-9)
                                    z1 = sLo + m_pitch *
                                         std::floor((z1 - sLo) / m_pitch +
                                                    1e-9);
                                if (z1 - z0 < 0.1 * m_pitch) break;
                                // Cleared slab (plain tool Common).
                                BRepAlgoAPI_Common com;
                                TopTools_ListOfShape ca, ct;
                                ca.Append(cleared);
                                ct.Append(BRepPrimAPI_MakeCylinder(
                                              axAt(z0), rBig,
                                              z1 - z0).Shape());
                                com.SetArguments(ca);
                                com.SetTools(ct);
                                com.SetRunParallel(Standard_True);
                                com.Build();
                                TopoDS_Shape slab =
                                    com.IsDone() ? com.Shape()
                                                 : TopoDS_Shape();
                                if (slab.IsNull() || shapeVol(slab) < 1e-6) {
                                    ok = false;
                                    break;
                                }
                                // Short flush ring for this slab: plain
                                // cylinder ARGUMENT minus the segment's
                                // swept void rod (phase 0 at each whole-
                                // pitch base = identical interfaces).
                                gp_Ax3 segBase(axAt(z0).Location(), zd, xd);
                                TopoDS_Shape rod = sweptRodThread(
                                    segBase, major, z1 - z0, m_pitch, depth,
                                    m_rightHanded, m_profile, 0.0);
                                TopoDS_Shape ring =
                                    rod.IsNull()
                                        ? TopoDS_Shape()
                                        : cutOn(BRepPrimAPI_MakeCylinder(
                                                    axAt(z0), major,
                                                    z1 - z0).Shape(),
                                                rod, 1.0e-3);
                                if (ring.IsNull() ||
                                    shapeVol(ring) < 1e-6 ||
                                    !BRepCheck_Analyzer(ring).IsValid()) {
                                    ok = false;
                                    break;
                                }
                                // Splice, with a per-slab fuse ladder.
                                const double expect =
                                    shapeVol(slab) + shapeVol(ring);
                                TopoDS_Shape ts;
                                for (int mode = 0; mode < 2; ++mode) {
                                    TopoDS_Shape f =
                                        mode == 0
                                            ? fuseGlued(slab, ring,
                                                        BOPAlgo_GlueShift,
                                                        0.0)
                                            : fuseGlued(slab, ring,
                                                        BOPAlgo_GlueOff,
                                                        1.0e-3);
                                    if (!f.IsNull() &&
                                        std::abs(shapeVol(f) - expect) <=
                                            0.01 * expect &&
                                        BRepCheck_Analyzer(f).IsValid()) {
                                        ts = f;
                                        break;
                                    }
                                }
                                if (ts.IsNull()) { ok = false; break; }
                                partsVol += shapeVol(ts);
                                acc = acc.IsNull()
                                          ? ts
                                          : fuseGlued(acc, ts,
                                                      BOPAlgo_GlueFull, 0.0);
                                if (acc.IsNull()) { ok = false; break; }
                                z0 = z1;
                            }
                            if (ok && !acc.IsNull()) {
                                why = "result volume off";
                                if (std::abs(shapeVol(acc) - partsVol) <=
                                    std::max(0.01 * partsVol, 1e-2)) {
                                    why = "result invalid";
                                    if (!BRepCheck_Analyzer(acc).IsValid()) {
                                        ShapeFix_Shape fixer(acc);
                                        fixer.Perform();
                                        acc = fixer.Shape();
                                    }
                                    if (BRepCheck_Analyzer(acc).IsValid()) {
                                        result = acc;
                                        break;
                                    }
                                }
                            }
                            std::fprintf(stderr, "[Thread] slab splice at "
                                                 "%.0f turns/slab declined "
                                                 "— retrying\n", slabTurns);
                        }
                    }
                }
            } catch (...) { result.Nullify(); }
            if (result.IsNull())
                std::fprintf(stderr, "[Thread] internal ring splice declined "
                                     "(%s) — falling back to groove tools\n",
                             why);
            else if (materializr::isVerbose())
                std::fprintf(stderr, "[Thread] internal ring splice OK\n");
        }
        // ROUNDED on a body that couldn't take the direct sweep (a cross-
        // holed / slotted rod — the reflow re-cut case): intersect the body
        // with the SWEPT threaded rod instead of falling into the rope
        // groove, whose deep discrete scoops are exactly the "stacked
        // discs" look the sweep was chosen to avoid (2026-07-21: a hole
        // cut through a Rounded-threaded rod re-threaded as poker chips).
        // body ∩ sweptRod ≡ cutting the groove ribbon out of the body, but
        // the tool is a fat simple solid — a thin helical ribbon's in/out
        // classification inverts (it removed 5× its own volume in the
        // probe), the same OCCT failure that killed swept cutters in the
        // per-turn work. Probe matrix (probe_derived_cutter): Rounded +
        // exact radius + fuzzy 1e-3 is the ONLY working cell — Standard
        // collapses to empty (sharp roots), radius bumps and larger fuzz
        // collapse too. Standard doesn't need this path anyway: the groove-
        // band tools were built for V profiles and are matrix-proven.
        // Guards: the body must LIE WITHIN the source cylinder (Common
        // TRUNCATES anything outside — bbox pre-gate), the removal must sit
        // inside the analytic groove-volume band, and the result must pass
        // the analyzer. Any miss falls through to the proven paths below.
        if (!m_isHole && m_starts <= 1 &&
            m_profile == ThreadProfile::Rounded) {
            const char* why = "swept-rod build failed";
            try {
                gp_Ax3 segBase(gp_Pnt(loc.X() + zd.X() * vLo,
                                      loc.Y() + zd.Y() * vLo,
                                      loc.Z() + zd.Z() * vLo), zd, xd);
                const double span = vHi - vLo;
                TopoDS_Shape srcCyl =
                    BRepPrimAPI_MakeCylinder(
                        gp_Ax2(segBase.Location(), zd, xd),
                        m_radius, span).Shape();
                Bnd_Box cylBox, bodyBox;
                BRepBndLib::Add(srcCyl, cylBox);
                BRepBndLib::Add(body, bodyBox);
                cylBox.Enlarge(1e-3);
                double cx1, cy1, cz1, cx2, cy2, cz2;
                double bx1, by1, bz1, bx2, by2, bz2;
                cylBox.Get(cx1, cy1, cz1, cx2, cy2, cz2);
                bodyBox.Get(bx1, by1, bz1, bx2, by2, bz2);
                why = "body extends outside the thread cylinder";
                if (bx1 >= cx1 && by1 >= cy1 && bz1 >= cz1 &&
                    bx2 <= cx2 && by2 <= cy2 && bz2 <= cz2) {
                    why = "swept-rod build failed";
                    TopoDS_Shape swept = sweptRodThread(segBase, m_radius,
                                                        span, m_pitch, depth,
                                                        m_rightHanded,
                                                        m_profile,
                                                        m_clearance);
                    if (!swept.IsNull()) {
                        why = "common boolean failed";
                        const double grooveMax =
                            M_PI * m_radius * m_radius * span -
                            shapeVol(swept);
        // Fuzzy booleans BUMP TOLERANCES on their input
                        // TShapes — a failed attempt must not pollute `body`
                        // for the groove-tool fallbacks (it measurably
                        // changed their cut results). Operate on a deep copy.
                        TopoDS_Shape bodyCopy =
                            BRepBuilderAPI_Copy(body).Shape();
                        // COMPLEMENT formulation: body ∩ sweptRod, computed
                        // as sweptRod − (srcCyl − body). The direct Common
                        // works for a transverse hole but INVERTS on a
                        // coaxial bore (it filled the bore — the recurring
                        // helical-classification curse — so the volume gate
                        // declined and Steve's tube got the rope grooves).
                        // Phrased as subtraction, every boolean is "smooth
                        // solid vs SIMPLE solid": the complement is exactly
                        // the material missing from a plain rod (the bore,
                        // the slot), never anything helical.
                        why = "complement boolean failed";
                        TopoDS_Shape res;
                        {
                            BRepAlgoAPI_Cut compCut(
                                BRepPrimAPI_MakeCylinder(
                                    gp_Ax2(segBase.Location(),
                                           segBase.Direction(),
                                           segBase.XDirection()),
                                    m_radius, span).Shape(),
                                bodyCopy);
                            compCut.SetFuzzyValue(1.0e-3);
                            compCut.Build();
                            if (compCut.IsDone() &&
                                !compCut.Shape().IsNull()) {
                                TopoDS_Shape missing = compCut.Shape();
                                if (shapeVol(missing) < 1e-6) {
                                    // Body IS the plain rod (nothing
                                    // missing): the swept rod is the result
                                    // outright.
                                    res = swept;
                                } else {
                                    BRepAlgoAPI_Cut fin;
                                    TopTools_ListOfShape fa, ft;
                                    fa.Append(swept);
                                    ft.Append(missing);
                                    fin.SetArguments(fa);
                                    fin.SetTools(ft);
                                    fin.SetFuzzyValue(1.0e-3);
                                    fin.SetRunParallel(Standard_True);
                                    if (m_cancelTok) {
                                        Handle(ThreadCancelBreak) brk =
                                            new ThreadCancelBreak(
                                                m_cancelTok);
                                        fin.Build(brk->Start());
                                    } else {
                                        fin.Build();
                                    }
                                    if (fin.IsDone()) res = fin.Shape();
                                }
                            }
                        }
                        if (!res.IsNull()) {
                            const double removed = bodyVol - shapeVol(res);
                            const double minRemoval =
                                std::max(1e-2, bodyVol * 1e-4);
                            why = "removed volume out of band";
                            if (removed >= minRemoval &&
                                removed <= grooveMax * 1.001 + 1e-6) {
                                why = "result invalid";
                                if (!BRepCheck_Analyzer(res).IsValid()) {
                                    ShapeFix_Shape fixer(res);
                                    fixer.Perform();
                                    res = fixer.Shape();
                                }
                                if (BRepCheck_Analyzer(res).IsValid())
                                    result = res;
                            }
                        }
                    }
                }
            } catch (...) { result.Nullify(); }
            if (result.IsNull())
                std::fprintf(stderr, "[Thread] swept-common re-cut declined "
                                     "(%s) — falling back to groove tools\n",
                             why);
            else if (materializr::isVerbose())
                std::fprintf(stderr, "[Thread] swept-common re-cut OK\n");
        }
        if (result.IsNull() && fullCylinder) {
            if (materializr::isVerbose())
                std::fprintf(stderr, "[Thread] cutting (span %.2f..%.2f)...\n",
                             vLo, vHi);
            // Single compound cut for all profiles. The boolean path is
            // O(N²) here no matter how it's sliced (confirmed: chunked cut,
            // sequential glue, and single fuse of segments all stay O(N²) —
            // OCCT won't fast-path the coincident seams of independently-built
            // pieces). So the plain single-tool cut, which is the FASTEST for
            // the short coarse threads that are the common printed case, wins.
            // (void) the unused segment builder.
            (void)segmentAndGlue;
            result = tryCut(buildCutter(vLo, vHi));
            // NO exact-span compound retry here. When the extended compound
            // cut inverts, the exact-span retry historically "succeeded"
            // with plausible-volume WRONG-MATERIAL removals (the poker-chip
            // bodies) that pass the coarse whole-body volume guard and then
            // SEGFAULT the tessellator. The per-turn fallback below retries
            // with per-turn validation instead — garbage can't pass it.
        }
        if (result.IsNull()) {
            std::fprintf(stderr, "[Thread] %s — per-turn sequential cut\n",
                         fullCylinder ? "compound cut failed"
                                      : "partial/interrupted cylinder");
            result = perTurnCut();
        }
        // GRAFT (external thread on a COMPLEX body). The direct helical cut
        // inverts against a body rebuilt by an upstream boolean (e.g. a union
        // of a lofted head onto the shank) even though the body is boolean-
        // healthy for plain cuts and the SAME cylinder threads fine
        // standalone — OCCT misclassifies the helical cut against the rebuilt
        // shape, and no heal (copy / unify / shapefix) recovers it. So thread
        // a CLEAN synthetic segment and splice it in at the shoulder:
        //   bodyMinus = body − C(plain segment);  result = bodyMinus ∪ T.
        // The helical faces never meet a boolean vs the complex body. A ROOTED
        // end gets a collar that OVERLAPS the parent, so the fuse is a robust
        // volumetric union (not a fragile coincident-face seam); a FREE end
        // keeps its runout (a real threaded bolt tip). The threaded segment
        // becomes a clean cylinder — any tip chamfer on it is flattened.
        if (m_forceGraft) result.Nullify();   // test hook
        if (result.IsNull() && !m_isHole && m_allowGraft && m_length > 1e-3) {
            std::fprintf(stderr, "[Thread] direct cut failed — GRAFT "
                                 "(clean segment spliced at the shoulder)\n");
            try {
                const bool botFree = vLo < -1e-9;
                const bool topFree = vHi > m_length + 1e-9;
                const double collar = std::max(1.0 * m_pitch, 1.0);
                auto axPt = [&](double v) {
                    return gp_Pnt(loc.X() + zd.X() * v, loc.Y() + zd.Y() * v,
                                  loc.Z() + zd.Z() * v);
                };
                auto plainCyl = [&](double lo, double hi) {
                    return BRepPrimAPI_MakeCylinder(
                        gp_Ax2(axPt(lo), zd, xd), m_radius, hi - lo).Shape();
                };
                // Axial span to thread+replace, extended over a FREE-end
                // chamfer (vLo/vHi already carry the chamfer run-through), so
                // the taper is threaded THROUGH, not flattened. Rooted ends
                // get a collar so the thread roots to a flat mating cap.
                const double tLo = botFree ? std::min(0.0, vLo) : 0.0;
                const double tHi = topFree ? std::max(m_length, vHi) : m_length;
                const double eLo = botFree ? tLo : -collar;
                const double eHi = topFree ? tHi : m_length + collar;
                // EXTRACT the real segment (cylinder + any end chamfer +
                // shoulder collar) from the body — a Common with a plain
                // cylinder, so its ACTUAL shape (incl. the taper) gets
                // threaded, not a flat-ended stand-in.
                TopoDS_Shape srcSeg;
                {
                    BRepAlgoAPI_Common com;
                    TopTools_ListOfShape ca, ct;
                    ca.Append(BRepBuilderAPI_Copy(body).Shape());
                    ct.Append(plainCyl(eLo, eHi));
                    com.SetArguments(ca);
                    com.SetTools(ct);
                    com.SetFuzzyValue(1.0e-3);
                    com.SetRunParallel(Standard_True);
                    com.Build();
                    if (com.IsDone()) srcSeg = com.Shape();
                }
                // Fall back to a plain cylinder if the extract failed.
                if (srcSeg.IsNull() || shapeVol(srcSeg) < 1e-6)
                    srcSeg = plainCyl(eLo, eHi);
                // Thread the segment [0, m_length] on the extracted source —
                // direct cut (no graft recursion); the nested op runs the
                // thread through the chamfer via its own run-through.
                ThreadOp seg(*this);
                seg.setAllowGraft(false);
                seg.setForceGraft(false);   // nested op cuts directly
                seg.setTargetFaceRef(materializr::topo::Ref{});
                seg.setAxis(gp_Ax2(loc, zd, xd));
                seg.setLength(m_length);
                TopoDS_Shape T = seg.buildResult(srcSeg);
                if (!T.IsNull()) {
                    // Remove the segment core (cylinder + chamfer, NOT the
                    // shoulder collar) so the fuse re-adds the THREADED taper.
                    TopoDS_Shape C = plainCyl(tLo, tHi);
                    TopoDS_Shape bodyMinus =
                        cutOn(BRepBuilderAPI_Copy(body).Shape(), C, 1.0e-3);
                    if (!bodyMinus.IsNull() && shapeVol(bodyMinus) > 1e-6) {
                        BRepAlgoAPI_Fuse fuse;
                        TopTools_ListOfShape fa, ft;
                        fa.Append(bodyMinus);
                        ft.Append(T);
                        fuse.SetArguments(fa);
                        fuse.SetTools(ft);
                        fuse.SetFuzzyValue(1.0e-3);
                        fuse.SetRunParallel(Standard_True);
                        if (m_cancelTok) {
                            Handle(ThreadCancelBreak) brk =
                                new ThreadCancelBreak(m_cancelTok);
                            fuse.Build(brk->Start());
                        } else {
                            fuse.Build();
                        }
                        TopoDS_Shape g =
                            fuse.IsDone() ? fuse.Shape() : TopoDS_Shape();
                        // Gate: valid single solid whose volume is sane (a
                        // fuse inversion balloons it). The threaded segment
                        // removed grooves but a flattened tip can add a
                        // little, so allow a generous band around the body.
                        if (!g.IsNull()) {
                            if (!BRepCheck_Analyzer(g).IsValid()) {
                                ShapeFix_Shape fx(g);
                                fx.Perform();
                                g = fx.Shape();
                            }
                            int ns = 0;
                            for (TopExp_Explorer sx(g, TopAbs_SOLID);
                                 sx.More(); sx.Next()) ++ns;
                            const double gv = shapeVol(g);
                            if (ns == 1 && BRepCheck_Analyzer(g).IsValid() &&
                                gv > 0.7 * bodyVol && gv < 1.15 * bodyVol) {
                                result = g;
                                if (materializr::isVerbose())
                                    std::fprintf(stderr,
                                                 "[Thread] graft OK\n");
                            } else {
                                std::fprintf(stderr, "[Thread] graft "
                                             "rejected (solids=%d vol=%.1f "
                                             "body=%.1f)\n", ns, gv, bodyVol);
                            }
                        }
                    }
                }
            } catch (...) {
                std::fprintf(stderr, "[Thread] graft threw\n");
            }
        }
        if (result.IsNull()) {
            std::fprintf(stderr, "[Thread] boolean cut FAILED\n");
            return {};
        }
        if (materializr::isVerbose())
            std::fprintf(stderr, "[Thread] buildResult OK\n");
        return result;
    } catch (...) {
        std::fprintf(stderr, "[Thread] buildResult threw\n");
        return {};
    }
}

namespace {
std::function<bool(ThreadOp&, Document&)> s_asyncRecut;
} // namespace

void ThreadOp::setAsyncRecutHook(std::function<bool(ThreadOp&, Document&)> h) {
    s_asyncRecut = std::move(h);
}

bool ThreadOp::execute(Document& doc) {
    if (m_bodyId < 0) return false;
    try {
        m_previousShape = doc.getBody(m_bodyId);
        if (m_previousShape.IsNull()) return false;

        // Follow an upstream edit: on the recompute path (editStep / redo, i.e.
        // no worker-precomputed result), re-resolve the target cylinder face
        // against the current body and adopt its new axis + radius. The stored
        // absolute params are the fallback when there's no ref or it can't
        // resolve — today's behaviour, so nothing regresses for old files.
        if (m_precomputed.IsNull() && !m_faceRef.empty()) {
            materializr::topo::Context ctx;
            ctx.doc = &doc;
            ctx.shape = m_previousShape;
            ctx.type = TopAbs_FACE;
            ctx.crossRebuild = true;   // body may have been rebuilt upstream
            TopoDS_Shape f;
            bool adopted = false;
            if (materializr::topo::resolve(m_faceRef, ctx, f) &&
                !f.IsNull() && f.ShapeType() == TopAbs_FACE) {
                gp_Ax2 ax2; double r = 0.0;
                if (cylFaceToThread(TopoDS::Face(f), ax2, r) && r > 1e-6) {
                    setAxis(ax2);   // updates m_axis + serialized components
                    m_radius = r;
                    adopted = true;
                    if (materializr::isVerbose())
                        std::fprintf(stderr, "[Thread] face ref resolved: "
                                             "r=%.4f\n", r);
                }
            }
            if (!adopted) {
                // GEOMETRIC FALLBACK: the ref dies when an op that doesn't
                // publish lineage rebuilds the body (Resize's ring-fuse —
                // the r8 thread then per-turn-cut GARBAGE into the r10 rod,
                // and it passed the volume gates). The thread's AXIS almost
                // always survives such edits — only the radius changed — so
                // adopt the coaxial cylindrical face whose radius is CLOSEST
                // to the stored one (closest disambiguates the outer wall
                // from a coaxial bore, for external and internal threads
                // alike). No-op whenever the stored-radius face still
                // exists: closest is then the stored radius itself.
                gp_Pnt axLoc(m_axOX, m_axOY, m_axOZ);
                gp_Dir axDir(m_axDX, m_axDY, m_axDZ);
                double bestR = -1.0, bestD = 1e300;
                for (TopExp_Explorer fx(m_previousShape, TopAbs_FACE);
                     fx.More(); fx.Next()) {
                    Handle(Geom_CylindricalSurface) cs =
                        Handle(Geom_CylindricalSurface)::DownCast(
                            BRep_Tool::Surface(TopoDS::Face(fx.Current())));
                    if (cs.IsNull()) continue;
                    gp_Cylinder cyl = cs->Cylinder();
                    if (std::abs(cyl.Position().Direction().Dot(axDir)) <
                        0.9999)
                        continue;
                    gp_Vec d(axLoc, cyl.Position().Location());
                    if (d.Magnitude() > 1e-6 &&
                        d.Crossed(gp_Vec(axDir)).Magnitude() > 1e-3)
                        continue;
                    const double dist = std::abs(cyl.Radius() - m_radius);
                    if (dist < bestD) { bestD = dist; bestR = cyl.Radius(); }
                }
                if (bestR > 1e-6 && bestD > 1e-6) {
                    std::fprintf(stderr, "[Thread] face ref did not resolve "
                                         "— coaxial fallback adopts r=%.4f "
                                         "(was %.4f)\n", bestR, m_radius);
                    m_radius = bestR;
                } else if (bestR <= 1e-6) {
                    std::fprintf(stderr, "[Thread] face ref did NOT resolve "
                                         "and no coaxial cylinder — keeping "
                                         "stored axis/radius %.4f\n",
                                 m_radius);
                }
            }
        }

        // Recompute path with the app hook installed: hand the multi-second
        // re-cut to the worker (body stays at its pre-thread state until the
        // result lands) so a cascade replay never freezes the UI.
        if (m_precomputed.IsNull() && s_asyncRecut && s_asyncRecut(*this, doc))
            return true;

        // The popup's worker thread may have already computed the result —
        // consume it; redo / editStep recompute synchronously as usual.
        TopoDS_Shape result;
        if (!m_precomputed.IsNull()) {
            result = m_precomputed;
            m_precomputed.Nullify();
        } else {
            result = buildResult(m_previousShape);
        }
        if (result.IsNull()) return false;

        doc.updateBody(m_bodyId, result);
        return true;
    } catch (...) {
        return false;
    }
}

bool ThreadOp::undo(Document& doc) {
    if (m_bodyId < 0 || m_previousShape.IsNull()) return false;
    try {
        doc.updateBody(m_bodyId, m_previousShape);
        return true;
    } catch (...) {
        return false;
    }
}

double ThreadOp::profileOpenFraction(ThreadProfile p) {
    return grooveSpec(p).openFrac;
}

std::string ThreadOp::description() const {
    char buf[128];
    char starts[24] = "";
    if (m_starts > 1)
        std::snprintf(starts, sizeof(starts), "%d-start ", m_starts);
    std::snprintf(buf, sizeof(buf), "%s %sthread Ø%s, pitch %s%s", m_isHole ? "Internal" : "External", starts, materializr::fmtLength(m_radius * 2.0).c_str(), materializr::fmtLength(m_pitch).c_str(), m_rightHanded ? "" : " (LH)");
    return buf;
}

void ThreadOp::renderProperties() {
    ImGui::Text(materializr::tr("%s Thread"), m_isHole ? "Internal" : "External");
    ImGui::Separator();
    materializr::lengthField(materializr::trFormat("Pitch (%s)", materializr::unitSuffix()).c_str(), &m_pitch);
    if (m_pitch < 0.1) m_pitch = 0.1;
    materializr::lengthField(materializr::trFormat("Depth (%s)", materializr::unitSuffix()).c_str(), &m_depth);
    if (m_depth < 0.05) m_depth = 0.05;
    // Past ~0.65·pitch the grooves merge and shred the crests into floating
    // helical fins (Steve found this empirically — "it's jumping lol").
    // Multi-start Rounded cuts with the rope tool, capped at 0.45·pitch —
    // same clamp buildResult applies, so the field shows what gets cut.
    const bool ropeCap = m_profile == ThreadProfile::Rounded && m_starts > 1;
    double maxDepth =
        std::min((ropeCap ? 0.45 : 0.65) * m_pitch, 0.45 * m_radius);
    if (m_depth > maxDepth) m_depth = maxDepth;
    // WRAPPED: this renders inside the history panel's inline editor, the
    // narrowest column in the app — unwrapped it ran off the panel edge.
    // (TextDisabled has no wrapping variant, hence the explicit colour push.)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", ropeCap
        ? "Depth caps at 0.45 \xC3\x97 pitch (multi-start rounded)."
        : "Depth caps at 0.65 \xC3\x97 pitch (ISO is 0.61).");
    ImGui::PopStyleColor();
    // Cross-section profile. Standard is the fast, shipped V-thread; the rest
    // are the maker/printing set (clean but slower — a boolean cut per turn).
    const char* kProfiles[] = {"Standard (V)", "Trapezoidal (ACME)",
                               "Square", "Buttress", "Rounded (print)"};
    int prof = static_cast<int>(m_profile);
    if (ImGui::Combo(materializr::tr("Profile"), &prof, kProfiles, IM_ARRAYSIZE(kProfiles)))
        m_profile = static_cast<ThreadProfile>(prof);
    if (profileTakesGrooveWidth(m_profile)) {
        materializr::lengthField(materializr::trFormat("Groove width (%s)", materializr::unitSuffix()).c_str(), &m_grooveWidth);
        if (m_grooveWidth < 0.0) m_grooveWidth = 0.0;
        ImGui::SetItemTooltip("%s", materializr::tr("Width of the cut at the surface. 0 = automatic (a set fraction of the pitch)."));
        if (m_grooveWidth <= 0.0)
            ImGui::TextDisabled("%s", materializr::trFormat("automatic: %s at this pitch", materializr::fmtLength(profileOpenFraction(m_profile) * m_pitch)).c_str());
    }
    if (m_profile != ThreadProfile::Standard) {
        materializr::lengthField(materializr::trFormat("Fit clearance (%s)", materializr::unitSuffix()).c_str(), &m_clearance);
        if (m_clearance < 0.0) m_clearance = 0.0;
        ImGui::SetItemTooltip("%s", materializr::tr("Radial gap so a PRINTED thread fits its mate (0.2\xE2\x80\x93""0.4mm typical). 0 = geometrically exact."));
        ImGui::TextDisabled("%s", materializr::tr("Non-Standard profiles cut per-turn \xE2\x80\x94 a long thread can take a while."));
    }
    bool rh = m_rightHanded;
    if (ImGui::Checkbox(materializr::tr("Right-handed"), &rh)) m_rightHanded = rh;
    // Multi-start was missing here entirely — a saved 3-start cap could not
    // have its start count edited after the fact. Same 1-6 range and stepped
    // style as the create panel.
    int starts = m_starts;
    materializr::inputNumberInt("Starts", &starts, 1, 1);
    m_starts = std::min(6, std::max(1, starts));
    if (m_starts > 1)
        ImGui::TextDisabled("%s", materializr::trFormat("lead %s/turn (%d interleaved helixes)", materializr::fmtLength(m_starts * m_pitch), m_starts).c_str());
    ImGui::TextUnformatted(materializr::trFormat("Diameter: %s   Length: %s", materializr::fmtLength(m_radius * 2.0), materializr::fmtLength(m_length)).c_str());
}

OperationDiff ThreadOp::captureDiff() const {
    OperationDiff d;
    if (m_bodyId >= 0 && !m_previousShape.IsNull())
        d.modifiedBefore.push_back({m_bodyId, m_previousShape});
    return d;
}

std::string ThreadOp::serializeParams() const {
    char buf[512];   // grew with gwidth
    std::snprintf(buf, sizeof(buf),
        "body=%d;radius=%.6f;length=%.6f;pitch=%.6f;depth=%.6f;hole=%d;rh=%d;"
        "profile=%d;clearance=%.6f;starts=%d;gwidth=%.6f;"
        "ox=%.9g;oy=%.9g;oz=%.9g;dx=%.9g;dy=%.9g;dz=%.9g;"
        "xx=%.9g;xy=%.9g;xz=%.9g",
        m_bodyId, m_radius, m_length, m_pitch, m_depth,
        m_isHole ? 1 : 0, m_rightHanded ? 1 : 0,
        static_cast<int>(m_profile), m_clearance, m_starts, m_grooveWidth,
        m_axOX, m_axOY, m_axOZ, m_axDX, m_axDY, m_axDZ,
        m_axXX, m_axXY, m_axXZ);
    std::string s = buf;
    // Target-face name LAST so its (delimiter-free) blob runs to end-of-string;
    // absent in old files, which just keep their absolute axis/radius.
    if (!m_faceRef.empty()) s += ";faceref=" + m_faceRef.serialize();
    return s;
}

bool ThreadOp::deserializeParams(const std::string& blob) {
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string key = blob.substr(pos, eq - pos);
        // faceref carries an opaque length-prefixed blob and is written last,
        // so read it to end-of-string (not to the next ';').
        if (key == "faceref") {
            m_faceRef = materializr::topo::Ref::parse(blob.substr(eq + 1));
            any = true;
            break;
        }
        std::string val = blob.substr(eq + 1, end - eq - 1);
        double d = std::atof(val.c_str());
        int    i = std::atoi(val.c_str());
        if      (key == "body")   { m_bodyId = i; any = true; }
        else if (key == "radius") { m_radius = d; any = true; }
        else if (key == "length") { m_length = d; any = true; }
        else if (key == "pitch")  { m_pitch = d; any = true; }
        else if (key == "depth")  { m_depth = d; any = true; }
        else if (key == "hole")   { m_isHole = (i != 0); any = true; }
        else if (key == "rh")     { m_rightHanded = (i != 0); any = true; }
        else if (key == "profile")   { m_profile = static_cast<ThreadProfile>(i); any = true; }
        else if (key == "clearance") { m_clearance = d; any = true; }
        else if (key == "starts")    { m_starts = i < 1 ? 1 : i; any = true; }
        else if (key == "gwidth")    { m_grooveWidth = d > 0.0 ? d : 0.0; any = true; }
        else if (key == "ox") { m_axOX = d; any = true; }
        else if (key == "oy") { m_axOY = d; any = true; }
        else if (key == "oz") { m_axOZ = d; any = true; }
        else if (key == "dx") { m_axDX = d; any = true; }
        else if (key == "dy") { m_axDY = d; any = true; }
        else if (key == "dz") { m_axDZ = d; any = true; }
        else if (key == "xx") { m_axXX = d; any = true; }
        else if (key == "xy") { m_axXY = d; any = true; }
        else if (key == "xz") { m_axXZ = d; any = true; }
        pos = end + 1;
    }
    // Rebuild the gp_Ax2 from the serialized components. buildResult reads
    // the components directly, so threads WORKED on reloaded files — but
    // getAxis() consumers (the sketch-on-cap true centre) silently got a
    // default Z-axis on every reloaded op and concluded "no thread axis
    // pierces this plane". Guarded: a degenerate/absent axis (old files)
    // throws inside gp and keeps the default.
    try {
        m_axis = gp_Ax2(gp_Pnt(m_axOX, m_axOY, m_axOZ),
                        gp_Dir(m_axDX, m_axDY, m_axDZ),
                        gp_Dir(m_axXX, m_axXY, m_axXZ));
    } catch (...) {}
    return any;
}

bool ThreadOp::rehydrateFromReload(const ReloadState& state, Document& /*doc*/) {
    if (m_bodyId < 0) return false;
    m_previousShape.Nullify();
    for (const auto& [id, shp] : state.modifiedBefore)
        if (id == m_bodyId) { m_previousShape = shp; break; }
    return !m_previousShape.IsNull();
}
