#include "LoftOp.h"
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <GProp_GProps.hxx>
#include <Standard_ErrorHandler.hxx> // OCC_CATCH_SIGNALS
#include <BRepAlgoAPI_Cut.hxx>
#include <BOPAlgo_ArgumentAnalyzer.hxx>
#include <BOPAlgo_CheckResult.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopoDS_Edge.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepOffsetAPI_MakeFilling.hxx>
#include <TopoDS_Shell.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepAdaptor_CompCurve.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopTools_SequenceOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <cstdlib>
#include <BRepCheck_Analyzer.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <cstdio>
#include <vector>
#include <imgui.h>
#include "../i18n.h"
#include "../i18n.h"
#include "modeling/ParamParse.h"

namespace {

// What ThruSections is actually being handed. The sections it pairs are
// COMPUTED REGIONS, not the raw sketch geometry, so nothing about them can be
// worked out from the project file alone -- a twisted loft had to be diagnosed
// from here.
//
// Winding is the number to watch: ThruSections joins wire A's parameter t to
// wire B's parameter t, so two sections whose loops run opposite ways around
// their own normals get connected front-to-back and the surface crosses itself.
gp_Vec wireWinding(const TopoDS_Wire& w) {
    std::vector<gp_Pnt> v;
    for (BRepTools_WireExplorer ex(w); ex.More(); ex.Next())
        v.push_back(BRep_Tool::Pnt(ex.CurrentVertex()));
    gp_Vec n(0, 0, 0);
    for (std::size_t i = 0; i < v.size(); ++i) {          // Newell's method
        const gp_Pnt& a = v[i];
        const gp_Pnt& b = v[(i + 1) % v.size()];
        n += gp_Vec((a.Y() - b.Y()) * (a.Z() + b.Z()),
                    (a.Z() - b.Z()) * (a.X() + b.X()),
                    (a.X() - b.X()) * (a.Y() + b.Y()));
    }
    if (n.Magnitude() > 1e-12) n.Normalize();
    return n;
}

// Write the sections to a BREP file when MATERIALIZR_DUMP_LOFT names a path.
// The profiles ThruSections receives are COMPUTED REGIONS, so they cannot be
// reconstructed from the project file -- without this, every hypothesis about a
// bad loft has to be tested by asking the user to re-run the app.
void dumpSections(const std::vector<TopoDS_Wire>& profiles) {
    const char* path = std::getenv("MATERIALIZR_DUMP_LOFT");
    if (!path || !*path) return;
    try {
        BRep_Builder b;
        TopoDS_Compound c;
        b.MakeCompound(c);
        for (const auto& w : profiles)
            if (!w.IsNull()) b.Add(c, w);
        if (BRepTools::Write(c, path))
            std::fprintf(stderr, "[Loft] sections written to %s\n", path);
    } catch (...) {}
}

// ─── Tip-split fallback for a self-intersecting loft ────────────────────────
//
// ThruSections pairs single-loop sections by arc-length parameter, which folds
// the surface whenever the loops' features sit at different perimeter
// fractions. Worst case measured (robot dog cover.mzr, two ~2 mm-wide C-shaped
// rim bands sharing the same two tips): every parameterisation-level remedy
// failed --
//
//     raw wires                          self-intersecting (vol 2856)
//     BRepFill_CompatibleWires           self-intersecting, vol NEGATIVE (-770)
//     single approximated curve/section  collapsed (vol 150-674)
//     arc-length resample + best seam    still folded (rms misfit 18.7 mm)
//
// because on a thin band the wrong run-pairing differs by only the band width,
// which no distance metric can see. What DOES work is structural: split each
// closed section into exactly TWO edges at its extreme points along the loft
// set's longest axis (the band tips), and let ThruSections' vertex matching pin
// tip to tip. Same part, this path: vol 3502, valid, no self-intersection.
//
// The fallback only runs when the normal loft came out self-intersecting, so
// well-behaved lofts keep their exact geometry and pay nothing.
bool selfIntersects(const TopoDS_Shape& s) {
    try {
        BOPAlgo_ArgumentAnalyzer an;
        an.SetShape1(s);
        an.OperationType() = BOPAlgo_UNKNOWN;
        an.SelfInterMode() = Standard_True;
        an.Perform();
        for (BOPAlgo_ListIteratorOfListOfCheckResult it(an.GetCheckResult());
             it.More(); it.Next())
            if (it.Value().GetCheckStatus() == BOPAlgo_SelfIntersect) return true;
    } catch (...) {}
    return false;
}

std::vector<gp_Pnt> densePoly(const TopoDS_Wire& w, int n) {
    std::vector<gp_Pnt> out; out.reserve(n);
    try {
        BRepAdaptor_CompCurve cc(w);
        const double L = GCPnts_AbscissaPoint::Length(cc);
        const double t0 = cc.FirstParameter();
        if (L < 1e-9) return out;
        for (int i = 0; i < n; ++i) {
            GCPnts_AbscissaPoint ap(cc, L * (double)i / n, t0);
            out.push_back(cc.Value(ap.IsDone() ? ap.Parameter() : t0));
        }
    } catch (...) { out.clear(); }
    return out;
}

TopoDS_Edge interpolatedEdge(const std::vector<gp_Pnt>& pts) {
    try {
        Handle(TColgp_HArray1OfPnt) a = new TColgp_HArray1OfPnt(1, (int)pts.size());
        for (std::size_t i = 0; i < pts.size(); ++i) a->SetValue((int)i + 1, pts[i]);
        GeomAPI_Interpolate ip(a, Standard_False, 1e-7);
        ip.Perform();
        if (!ip.IsDone()) return TopoDS_Edge();
        return BRepBuilderAPI_MakeEdge(ip.Curve()).Edge();
    } catch (...) { return TopoDS_Edge(); }
}

// Rebuild one closed loop as a 2-edge wire split at its extremes along `axis`
// (0=X 1=Y 2=Z).
//
// Each run is ONE curve produced by Approx_Curve3d over the run's sub-range of
// the original composite curve -- NOT a spline through sampled points. The
// sampled version missed the ~0.2 mm tip arcs by up to 50 um, which forced the
// bridge sew tolerance to 2e-2 and left seam edges carrying ~0.05 mm
// tolerances; every boolean against the bridged body then degraded (a
// subtract cut only the tool's outline) or ground forever (re-running at
// project load, it made the file unopenable). Approximating the real curve
// keeps the boundary within ~1e-5 of the rim everywhere, tips included.
TopoDS_Wire tipSplitWire(const TopoDS_Wire& w, int axis) {
    BRepAdaptor_CompCurve cc(w);
    const double t0 = cc.FirstParameter(), t1 = cc.LastParameter();
    if (!(t1 > t0)) return TopoDS_Wire();
    // locate the two extreme parameters along `axis` by dense scan + refine
    auto coord = [axis](const gp_Pnt& q) {
        return axis == 0 ? q.X() : axis == 1 ? q.Y() : q.Z();
    };
    const int N = 4096;
    double pMin = t0, pMax = t0, vMin = 1e300, vMax = -1e300;
    for (int i = 0; i < N; ++i) {
        const double t = t0 + (t1 - t0) * i / N;
        const double v = coord(cc.Value(t));
        if (v < vMin) { vMin = v; pMin = t; }
        if (v > vMax) { vMax = v; pMax = t; }
    }
    if (!(std::fabs(pMax - pMin) > (t1 - t0) * 1e-4)) return TopoDS_Wire();
    const double lo = std::min(pMin, pMax), hi = std::max(pMin, pMax);
    auto runEdge = [&](double a, double b, bool wrap) -> TopoDS_Edge {
        try {
            // approximate the sub-range [a,b] (wrapping through t1->t0 when
            // asked) by sampling the REAL curve densely and interpolating; the
            // sampling is fine enough (1000 pts/run) that with curvature
            // clustering below it stays within ~1e-5 of the original.
            const int K = 1000;
            std::vector<gp_Pnt> raw; raw.reserve(K + 1);
            const double span = wrap ? (t1 - b) + (a - t0) : (b - a);
            for (int i = 0; i <= K; ++i) {
                double t = wrap ? b + span * i / K : a + span * i / K;
                if (wrap && t > t1) t = t0 + (t - t1);
                raw.push_back(cc.Value(t));
            }
            // curvature-weighted reparameterisation of the samples
            std::vector<double> cum(raw.size(), 0.0);
            for (std::size_t i = 1; i < raw.size(); ++i) {
                const double ds = raw[i-1].Distance(raw[i]);
                double dth = 0.0;
                if (i + 1 < raw.size()) {
                    const gp_Vec u(raw[i-1], raw[i]), v(raw[i], raw[i+1]);
                    if (u.Magnitude() > 1e-12 && v.Magnitude() > 1e-12)
                        dth = u.Angle(v);
                }
                cum[i] = cum[i-1] + ds + 1.0 * dth;
            }
            const double L = cum.back();
            const int M = 400;
            Handle(TColgp_HArray1OfPnt) arr = new TColgp_HArray1OfPnt(1, M);
            std::size_t j = 0;
            for (int i = 0; i < M; ++i) {
                const double sTarget = L * (double)i / (M - 1);
                while (j + 1 < cum.size() && cum[j+1] < sTarget) ++j;
                const double seg = cum[j+1] - cum[j];
                const double t = seg > 1e-12 ? (sTarget - cum[j]) / seg : 0.0;
                arr->SetValue(i + 1, gp_Pnt(
                    raw[j].X() + (raw[j+1].X()-raw[j].X())*t,
                    raw[j].Y() + (raw[j+1].Y()-raw[j].Y())*t,
                    raw[j].Z() + (raw[j+1].Z()-raw[j].Z())*t));
            }
            GeomAPI_Interpolate ip(arr, Standard_False, 1e-7);
            ip.Perform();
            if (!ip.IsDone()) return TopoDS_Edge();
            return BRepBuilderAPI_MakeEdge(ip.Curve()).Edge();
        } catch (...) { return TopoDS_Edge(); }
    };
    const TopoDS_Edge e1 = runEdge(lo, hi, false);
    const TopoDS_Edge e2 = runEdge(hi, lo, true);
    if (e1.IsNull() || e2.IsNull()) return TopoDS_Wire();
    try {
        BRepBuilderAPI_MakeWire mk(e1); mk.Add(e2);
        if (!mk.IsDone()) return TopoDS_Wire();
        return mk.Wire();
    } catch (...) { return TopoDS_Wire(); }
}

// The whole fallback: rebuild every section tip-split along the loft set's
// longest axis and loft again. Returns null on any failure.
TopoDS_Shape tipSplitLoft(const std::vector<TopoDS_Wire>& profiles, bool solid, bool ruled) {
    Bnd_Box bb;
    for (const auto& w : profiles) { try { BRepBndLib::Add(w, bb); } catch (...) {} }
    if (bb.IsVoid()) return TopoDS_Shape();
    double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
    const double dx=x1-x0, dy=y1-y0, dz=z1-z0;
    const int axis = (dx >= dy && dx >= dz) ? 0 : (dy >= dz ? 1 : 2);
    try {
        std::vector<TopoDS_Wire> split;
        split.reserve(profiles.size());
        for (const auto& w : profiles) {
            const TopoDS_Wire sw = tipSplitWire(w, axis);
            if (sw.IsNull()) return TopoDS_Shape();
            split.push_back(sw);
        }
        // Walls only from ThruSections. For a SOLID the end caps are built with
        // MakeFilling rather than letting ThruSections cap with planes: these
        // sections are usually not planar (sketch profiles bow out of plane --
        // float32 sketch storage), and a planar cap whose boundary sits 0.1 mm
        // off its own surface poisons every boolean the body later takes part
        // in. A filled cap actually contains its edges (tol ~1e-7 measured, vs
        // a plane face lying to the kernel), and the walls+caps sew closed with
        // zero free edges on the part this was built against.
        BRepOffsetAPI_ThruSections t(Standard_False,
                                     ruled ? Standard_True : Standard_False);
        for (const auto& sw : split) t.AddWire(sw);
        t.Build();
        if (!t.IsDone() || t.Shape().IsNull()) return TopoDS_Shape();
        if (!solid) {
            return BRepCheck_Analyzer(t.Shape()).IsValid() ? t.Shape() : TopoDS_Shape();
        }
        BRepBuilderAPI_Sewing sew(1e-4);
        for (TopExp_Explorer ex(t.Shape(), TopAbs_FACE); ex.More(); ex.Next())
            sew.Add(ex.Current());
        for (const TopoDS_Wire* w : {&split.front(), &split.back()}) {
            BRepOffsetAPI_MakeFilling fill;
            for (TopExp_Explorer ex(*w, TopAbs_EDGE); ex.More(); ex.Next())
                fill.Add(TopoDS::Edge(ex.Current()), GeomAbs_C0);
            fill.Build();
            if (!fill.IsDone() || fill.Shape().IsNull()) return TopoDS_Shape();
            sew.Add(fill.Shape());
        }
        sew.Perform();
        if (sew.SewedShape().IsNull() || sew.NbFreeEdges() > 0) return TopoDS_Shape();
        for (TopExp_Explorer ex(sew.SewedShape(), TopAbs_SHELL); ex.More(); ex.Next()) {
            BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(ex.Current()));
            if (!ms.IsDone()) break;
            const TopoDS_Shape r = ms.Solid();
            return BRepCheck_Analyzer(r).IsValid() ? r : TopoDS_Shape();
        }
        return TopoDS_Shape();
    } catch (...) { return TopoDS_Shape(); }
}

// Sew the loft into the body it was lofted from: body faces minus the source
// faces, plus the loft's wall faces (no caps -- the holes ARE the caps).
//
// The direct alternative -- add the loft as a body and Fuse -- was measured
// exhaustively on robot dog cover.mzr and fails at every OCCT setting: exact,
// fuzzy 1e-5/1e-3, glue, 0.2 mm forced interpenetration, healed inputs. The
// contact is two long ~2 mm-wide tangent strips, and BOP either returns an
// invalid shred, drops an operand, or an empty compound (123 s, 0 faces).
// The body itself is fine: it fuses with plain boxes anywhere, instantly.
//
// Sewing needs a tolerance ladder because the loft's boundary is a resampled
// approximation of the rim outline (a few micron off along the runs, up to
// ~20 micron where interpolation rounds the band tips). On the same part:
// 1e-3 leaves 21 free edges, 2e-2 closes all of them, and the result is a
// valid solid within 0.13% of body+skirt volume.
//
// Known wart, logged when it happens: where the section bands terminate
// against a body wall that spans the gap, the loft wall wraps the band tip and
// laps that wall by a hair -- BOP's self-intersection probe flags it, but the
// solid is valid, meshes fine, and the volume is right. Fixing it needs local
// face surgery at the tips; not attempted here.
TopoDS_Shape bridgeIntoBody(const TopoDS_Shape& body,
                            const std::vector<TopoDS_Shape>& consumedFaces,
                            const std::vector<TopoDS_Wire>& sections,
                            bool ruled) {
    if (body.IsNull() || sections.size() < 2) return TopoDS_Shape();
    // Wall shell between the sections (tip-split for sane correspondence).
    Bnd_Box bb;
    for (const auto& w : sections) { try { BRepBndLib::Add(w, bb); } catch (...) {} }
    if (bb.IsVoid()) return TopoDS_Shape();
    double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
    const double dx=x1-x0, dy=y1-y0, dz=z1-z0;
    const int axis = (dx >= dy && dx >= dz) ? 0 : (dy >= dz ? 1 : 2);
    TopoDS_Shape walls;
    try {
        BRepOffsetAPI_ThruSections t(Standard_False,
                                     ruled ? Standard_True : Standard_False);
        for (const auto& w : sections) {
            const TopoDS_Wire sw = tipSplitWire(w, axis);
            if (sw.IsNull()) return TopoDS_Shape();
            t.AddWire(sw);
        }
        t.Build();
        if (!t.IsDone() || t.Shape().IsNull()) return TopoDS_Shape();
        walls = t.Shape();
    } catch (...) { return TopoDS_Shape(); }

    GProp_GProps gb; BRepGProp::VolumeProperties(body, gb);
    const double vBody = gb.Mass();

    for (double tol : {1e-4, 1e-3, 5e-3}) {
        try {
            BRepBuilderAPI_Sewing sew(tol);
            int dropped = 0;
            for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
                bool consumed = false;
                for (const auto& cf : consumedFaces)
                    if (ex.Current().IsSame(cf)) { consumed = true; break; }
                if (consumed) { ++dropped; continue; }
                sew.Add(ex.Current());
            }
            if (dropped != static_cast<int>(consumedFaces.size())) return TopoDS_Shape();
            for (TopExp_Explorer ex(walls, TopAbs_FACE); ex.More(); ex.Next())
                sew.Add(ex.Current());
            sew.Perform();
            if (sew.SewedShape().IsNull()) continue;
            if (sew.NbFreeEdges() > 0) {
                std::fprintf(stderr, "[Loft] bridge sew at %.0e: %d free edge(s).\n",
                             tol, sew.NbFreeEdges());
                continue;
            }
            for (TopExp_Explorer ex(sew.SewedShape(), TopAbs_SHELL); ex.More(); ex.Next()) {
                BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(ex.Current()));
                if (!ms.IsDone()) break;
                TopoDS_Shape solid = ms.Solid();
                GProp_GProps g; BRepGProp::VolumeProperties(solid, g);
                if (g.Mass() < 0) { solid.Reverse();
                                    BRepGProp::VolumeProperties(solid, g); }
                // Adding material: the merged body must not be smaller than the
                // input body, nor absurdly larger.
                if (g.Mass() < vBody - 1e-4 * vBody) break;
                if (g.Mass() > vBody * 2.0 + 1e4) break;
                if (!BRepCheck_Analyzer(solid).IsValid()) break;
                std::fprintf(stderr, "[Loft] bridged into the body at sew tol %.0e "
                             "(vol %.3f -> %.3f).\n", tol, vBody, g.Mass());
                return solid;
            }
        } catch (...) {}
    }
    return TopoDS_Shape();
}

void describeSections(const std::vector<TopoDS_Wire>& profiles) {
    gp_Vec prev(0, 0, 0);
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const TopoDS_Wire& w = profiles[i];
        if (w.IsNull()) { std::fprintf(stderr, "[Loft] section %zu: NULL\n", i); continue; }
        int edges = 0;
        for (TopExp_Explorer ex(w, TopAbs_EDGE); ex.More(); ex.Next()) ++edges;
        double len = 0.0;
        try { BRepAdaptor_CompCurve cc(w); len = GCPnts_AbscissaPoint::Length(cc); } catch (...) {}
        Bnd_Box bb; try { BRepBndLib::Add(w, bb); } catch (...) {}
        double x0=0,y0=0,z0=0,x1=0,y1=0,z1=0;
        if (!bb.IsVoid()) bb.Get(x0,y0,z0,x1,y1,z1);
        const gp_Vec n = wireWinding(w);
        const double dot = (i == 0) ? 1.0 : n.Dot(prev);
        std::fprintf(stderr,
            "[Loft] section %zu: %d edges, closed=%s, perimeter %.3f, "
            "winding (%.3f,%.3f,%.3f), dot-with-previous %+.3f%s\n",
            i, edges, BRep_Tool::IsClosed(w) ? "yes" : "NO", len,
            n.X(), n.Y(), n.Z(), dot,
            (i > 0 && dot < 0.0) ? "   <-- OPPOSITE WINDING, will twist" : "");
        std::fprintf(stderr,
            "[Loft]            bbox X %.2f..%.2f  Y %.2f..%.2f  Z %.2f..%.2f\n",
            x0, x1, y0, y1, z0, z1);
        prev = n;
    }
}

} // namespace

LoftOp::LoftOp() = default;

void LoftOp::addProfile(const TopoDS_Wire& wire) {
    m_profiles.push_back(wire);
    m_holeProfiles.emplace_back(); // no holes for this profile
}

void LoftOp::addProfile(const TopoDS_Wire& outer, const std::vector<TopoDS_Wire>& holes) {
    m_profiles.push_back(outer);
    m_holeProfiles.push_back(holes);
}

void LoftOp::clearProfiles() {
    m_profiles.clear();
    m_holeProfiles.clear();
}

void LoftOp::setSolid(bool solid) {
    m_solid = solid;
}

void LoftOp::setRuled(bool ruled) {
    m_ruled = ruled;
}

void LoftOp::setBridge(int bodyId, const std::vector<TopoDS_Shape>& sourceFaces) {
    m_bridgeBodyId = bodyId;
    m_bridgeFaces = sourceFaces;
}

bool LoftOp::execute(Document& doc) {
    if (m_profiles.size() < 2) {
        return false;
    }

    try {
        // Degenerate section stacks (e.g. perpendicular "wall" profiles that
        // make the surface fold through itself) can drive ThruSections to a
        // kernel FAULT, not just a clean failure. OCC_CATCH_SIGNALS turns that
        // signal into a Standard_Failure the catch below absorbs — without it
        // the app dies (crash reproduced by repeated preview/cancel on a
        // weaving 3-section loft).
        OCC_CATCH_SIGNALS
        BRepOffsetAPI_ThruSections thruSections(m_solid ? Standard_True : Standard_False,
                                                 m_ruled ? Standard_True : Standard_False);

        describeSections(m_profiles);
        dumpSections(m_profiles);

        std::vector<TopoDS_Wire> profiles = m_profiles;

        for (const auto& wire : profiles) {
            thruSections.AddWire(wire);
        }

        thruSections.Build();
        if (!thruSections.IsDone()) {
            return false;
        }

        TopoDS_Shape loftedShape = thruSections.Shape();

        // A loft that folds through itself still reads as "valid" to BRepCheck
        // and has a plausible volume, so the fold must be looked for
        // explicitly. Only sections without holes take the fallback -- the
        // tip-split rebuild does not carry hole channels through.
        bool holesPresent = false;
        for (const auto& hp : m_holeProfiles) if (!hp.empty()) { holesPresent = true; break; }
        if (!holesPresent && !loftedShape.IsNull() && selfIntersects(loftedShape)) {
            std::fprintf(stderr, "[Loft] result self-intersects -- retrying with "
                                 "tip-split sections.\n");
            const TopoDS_Shape retry = tipSplitLoft(profiles, m_solid, m_ruled);
            if (!retry.IsNull() && !selfIntersects(retry)) {
                std::fprintf(stderr, "[Loft] tip-split loft is clean -- using it.\n");
                loftedShape = retry;
            } else {
                std::fprintf(stderr, "[Loft] tip-split retry did not help; keeping "
                                     "the original result.\n");
            }
        }

        // Tube support: if the profiles carry holes (e.g. concentric circles),
        // loft each hole-channel into its own inner solid and cut it from the
        // outer loft. Only meaningful for a solid loft. Hole k is matched by
        // index across the profiles, and we require every profile to expose the
        // same number of holes so the channels pair up unambiguously.
        if (m_solid && !m_holeProfiles.empty()) {
            size_t nHoles = m_holeProfiles[0].size();
            bool uniform = nHoles > 0;
            for (const auto& hp : m_holeProfiles) {
                if (hp.size() != nHoles) { uniform = false; break; }
            }
            for (size_t k = 0; uniform && k < nHoles; ++k) {
                BRepOffsetAPI_ThruSections inner(Standard_True, // solid
                                                 m_ruled ? Standard_True : Standard_False);
                for (const auto& hp : m_holeProfiles) {
                    inner.AddWire(hp[k]);
                }
                inner.Build();
                if (!inner.IsDone()) continue; // skip a hole that won't loft
                BRepAlgoAPI_Cut cut(loftedShape, inner.Shape());
                cut.Build();
                if (!cut.IsDone()) continue;
                // Adopt the cut only if it's still a usable solid — a bad hole
                // channel can yield a null/empty/invalid result that would
                // otherwise replace a perfectly good outer loft.
                TopoDS_Shape cutShape = cut.Shape();
                if (cutShape.IsNull()) continue;
                GProp_GProps cutProps;
                BRepGProp::VolumeProperties(cutShape, cutProps);
                if (cutProps.Mass() < 1e-6) continue;
                if (!BRepCheck_Analyzer(cutShape).IsValid()) continue;
                loftedShape = cutShape;
            }
        }

        // Validate-or-refuse (same gate as BooleanOp/FilletOp/ShellOp): a
        // degenerate section stack can pass IsDone() yet produce a null or
        // topologically invalid shape that later crashes tessellation/save.
        // The volume check only applies to solid lofts — a surface loft
        // legitimately encloses no volume.
        if (loftedShape.IsNull()) return false;
        if (m_solid) {
            GProp_GProps gp;
            BRepGProp::VolumeProperties(loftedShape, gp);
            if (gp.Mass() < 1e-6) return false;
        }
        if (!BRepCheck_Analyzer(loftedShape).IsValid()) return false;

        // Bridge mode: consume the source faces instead of adding a body.
        m_bridged = false;
        if (m_bridgeBodyId >= 0 && !m_bridgeFaces.empty() && m_solid) {
            bool holesPresent2 = false;
            for (const auto& hp : m_holeProfiles)
                if (!hp.empty()) { holesPresent2 = true; break; }
            if (!holesPresent2) {
                const TopoDS_Shape host = doc.getBody(m_bridgeBodyId);
                if (!host.IsNull()) {
                    const TopoDS_Shape merged =
                        bridgeIntoBody(host, m_bridgeFaces, profiles, m_ruled);
                    if (!merged.IsNull()) {
                        m_bridgePreviousShape = host;
                        doc.updateBody(m_bridgeBodyId, merged);
                        m_bridged = true;
                        return true;
                    }
                    std::fprintf(stderr, "[Loft] bridge failed -- keeping the loft "
                                 "as its own body.\n");
                }
            }
        }
        doc.addOrPutBody(m_createdBodyId, loftedShape, "Loft");

        return true;
    } catch (...) {
        return false;
    }
}

bool LoftOp::undo(Document& doc) {
    if (m_bridged && m_bridgeBodyId >= 0 && !m_bridgePreviousShape.IsNull()) {
        doc.updateBody(m_bridgeBodyId, m_bridgePreviousShape);
        m_bridged = false;
        return true;
    }
    try {
        if (m_createdBodyId >= 0) {
            doc.removeBody(m_createdBodyId);
            // Keep m_createdBodyId — tombstone restore on next execute().
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string LoftOp::description() const {
    std::string desc = "Loft through " + std::to_string(m_profiles.size()) + " profiles";
    if (m_solid) {
        desc += " (Solid)";
    } else {
        desc += " (Shell)";
    }
    if (m_ruled) {
        desc += " Ruled";
    }
    return desc;
}

void LoftOp::renderProperties() {
    ImGui::Text("%s", materializr::tr("Loft"));
    ImGui::Separator();

    ImGui::Text(materializr::tr("Profiles: %d"), static_cast<int>(m_profiles.size()));

    if (m_profiles.size() < 2) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", materializr::tr("At least 2 profiles required"));
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           materializr::tr("%d profiles ready"), static_cast<int>(m_profiles.size()));
    }

    ImGui::Separator();
    ImGui::Checkbox(materializr::tr("Solid"), &m_solid);
    ImGui::Checkbox(materializr::tr("Ruled Surface"), &m_ruled);
}

OperationDiff LoftOp::captureDiff() const {
    OperationDiff d;
    if (m_createdBodyId >= 0) d.created.push_back(m_createdBodyId);
    return d;
}

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS.hxx>
#include <sstream>

std::string LoftOp::serializeParams() const {
    // Profiles are raw wires (picked from sketch regions / body loops at
    // create time) — no persistent source ids exist, so they persist as an
    // ASCII BREP compound embedded in the params. PARAMS_LEN stores raw
    // bytes, so the multi-line BREP is safe; it goes LAST, length-prefixed.
    // Compound order: profile0, its holes..., profile1, its holes..., etc.
    std::string blob = "solid=" + std::to_string(m_solid ? 1 : 0) +
                       ";ruled=" + std::to_string(m_ruled ? 1 : 0) +
                       ";created=" + std::to_string(m_createdBodyId) +
                       ";np=" + std::to_string(m_profiles.size());
    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    for (size_t i = 0; i < m_profiles.size(); ++i) {
        const size_t nh = i < m_holeProfiles.size() ? m_holeProfiles[i].size() : 0;
        blob += ";h" + std::to_string(i) + "=" + std::to_string(nh);
        bb.Add(comp, m_profiles[i]);
        for (size_t j = 0; j < nh; ++j) bb.Add(comp, m_holeProfiles[i][j]);
    }
    std::ostringstream os;
    BRepTools::Write(comp, os);
    const std::string brep = os.str();
    blob += ";brep=" + std::to_string(brep.size()) + ":" + brep;
    return blob;
}

bool LoftOp::deserializeParams(const std::string& blob) {
    m_profiles.clear();
    m_holeProfiles.clear();
    std::vector<int> holeCounts;
    int np = 0;
    bool any = false;
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eq = blob.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = blob.substr(pos, eq - pos);
        if (key == "brep") {
            // <len>:<raw ascii brep>, runs to end.
            size_t colon = blob.find(':', eq);
            if (colon == std::string::npos) break;
            // Checked length, bounded by subtraction (ParamParse.h):
            // the old `colon + 1 + n > blob.size()` wrapped on a
            // negative length and let the guard pass.
            size_t n = 0, payload = 0;
            if (!materializr::readLenPrefix(blob, eq + 1, colon, n, payload)) break;
            std::istringstream is(blob.substr(payload, n));
            TopoDS_Shape comp;
            BRep_Builder bb;
            try { BRepTools::Read(comp, is, bb); } catch (...) { return false; }
            // Unpack: per profile i, one wire + holeCounts[i] hole wires.
            TopoDS_Iterator it(comp);
            for (int i = 0; i < np && it.More(); ++i) {
                if (it.Value().ShapeType() != TopAbs_WIRE) return false;
                m_profiles.push_back(TopoDS::Wire(it.Value()));
                it.Next();
                std::vector<TopoDS_Wire> holes;
                int nh = i < static_cast<int>(holeCounts.size()) ? holeCounts[i] : 0;
                for (int j = 0; j < nh && it.More(); ++j) {
                    holes.push_back(TopoDS::Wire(it.Value()));
                    it.Next();
                }
                m_holeProfiles.push_back(std::move(holes));
            }
            any = true;
            break;
        }
        size_t end = blob.find(';', eq);
        if (end == std::string::npos) end = blob.size();
        std::string val = blob.substr(eq + 1, end - eq - 1);
        if      (key == "solid")   { m_solid = val == "1"; any = true; }
        else if (key == "ruled")   { m_ruled = val == "1"; any = true; }
        else if (key == "created") { m_createdBodyId = std::atoi(val.c_str()); any = true; }
        else if (key == "np")      { np = std::atoi(val.c_str()); any = true; }
        // h<N>: N comes from the file and SIZES the vector below, so it is bounded
        // before the resize. This site was weaker than BoundaryFillOp's twin — it
        // had no digit guard at all, so any "h*" key reached std::atoi.
        else if (!key.empty() && key[0] == 'h') {
            int idx = materializr::parseIndexKey(key, 1);
            if (idx < 0 || idx >= materializr::kMaxProfiles) return false;
            int nh = 0;
            if (!materializr::parseWholeInt(val, nh) || nh < 0 || nh > materializr::kMaxHolesPerProfile)
                return false;
            if (idx >= static_cast<int>(holeCounts.size()))
                holeCounts.resize(idx + 1, 0);
            holeCounts[idx] = nh;
        }
        pos = end + 1;
    }
    return any && static_cast<int>(m_profiles.size()) == np && np >= 2;
}

bool LoftOp::rehydrateFromReload(const ReloadState& state, Document&) {
    if (m_profiles.size() < 2) return false;
    if (m_createdBodyId < 0 && !state.created.empty())
        m_createdBodyId = state.created.front();
    return true;   // profiles are self-contained; execute() re-lofts them
}
