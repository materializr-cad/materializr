#include "BlendCut.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <ShapeFix_Wireframe.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include "UnifyTolerance.h"
#include <GProp_GProps.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace materializr {
namespace blendcut {
namespace {

// Permanent, env-gated diagnostics: MZR_BLENDCUT_DEBUG=1 traces which stage
// refuses a fallback build - this saga kept needing it re-added.
bool bcDebug() {
    static const bool on = std::getenv("MZR_BLENDCUT_DEBUG") != nullptr;
    return on;
}
#define BC_DBG(...) do { if (bcDebug()) std::fprintf(stderr, __VA_ARGS__); } while (0)

struct EdgeInfo {
    TopoDS_Edge edge;
    gp_Lin line;               // underlying infinite line of the edge
    gp_Pnt p0, p1, pm;         // endpoints + midpoint
    TopoDS_Face fRef, fOther;  // dRef is measured along fRef
    gp_Pln plnRef, plnOther;
    gp_Dir nRef, nOther;       // outward face normals at the edge
    gp_Dir dRefDir, dOtherDir; // in-face directions away from the edge
    bool concave = false;      // interior corner (chamfer FILLS, can't cut)
};

// A run of collinear fragments sharing one plane pair: swept as ONE tool
// over the union of their spans, which is what carries the blend across the
// gap a hole or pocket bit out of the edge.
struct Group {
    EdgeInfo rep;      // representative - dirs/normals valid for all members
    double tmin, tmax; // span along rep.line covered by every member
};

// A cutting solid plus the face of it that IS the blend surface (chamfer
// plane / fillet cylinder) - the caller finds the bevel on the cut result
// through it.
struct Tool {
    TopoDS_Shape solid;
    TopoDS_Face blendTemplate;
};

bool outwardNormal(const TopoDS_Face& f, gp_Dir& out) {
    try {
        BRepGProp_Face gf(f);
        Standard_Real u0, u1, v0, v1;
        gf.Bounds(u0, u1, v0, v1);
        gp_Pnt p;
        gp_Vec n;
        gf.Normal((u0 + u1) * 0.5, (v0 + v1) * 0.5, p, n);
        if (n.Magnitude() < 1e-12) return false;
        out = gp_Dir(n);
        return true;
    } catch (...) { return false; }
}

bool onFace(const TopoDS_Face& f, const gp_Pnt& p) {
    BRepClass_FaceClassifier cls(f, p, 1e-6);
    return cls.State() == TopAbs_IN || cls.State() == TopAbs_ON;
}

bool planesMatch(const gp_Pln& a, const gp_Pln& b) {
    if (!a.Axis().Direction().IsParallel(b.Axis().Direction(), 1e-4))
        return false;
    return a.Distance(b.Location()) < 1e-5;
}

bool analyzeEdge(const TopoDS_Shape& body, const TopoDS_Edge& e,
                 const TopTools_IndexedDataMapOfShapeListOfShape& efm,
                 const gp_Pln* refPln, double probeEps, EdgeInfo& out) {
    BRepAdaptor_Curve c(e);
    if (c.GetType() != GeomAbs_Line) return false;
    out.edge = e;
    out.line = c.Line();
    out.p0 = c.Value(c.FirstParameter());
    out.p1 = c.Value(c.LastParameter());
    out.pm = c.Value((c.FirstParameter() + c.LastParameter()) * 0.5);

    if (!efm.Contains(e)) return false;
    const TopTools_ListOfShape& fs = efm.FindFromKey(e);
    if (fs.Extent() < 2) return false;
    TopoDS_Face f1 = TopoDS::Face(fs.First());
    TopoDS_Face f2;
    for (const TopoDS_Shape& f : fs)
        if (!f.IsSame(f1)) { f2 = TopoDS::Face(f); break; }
    if (f2.IsNull()) return false;
    if (BRepAdaptor_Surface(f1).GetType() != GeomAbs_Plane ||
        BRepAdaptor_Surface(f2).GetType() != GeomAbs_Plane)
        return false;
    // dRef belongs to the asymmetric reference when one of this edge's pair
    // lies on its PLANE (by plane, not face identity: a pocket may have
    // fragmented the reference face, and the fragment bordering this edge is
    // a different TopoDS face on the same plane), else to the first face
    // (native Add() semantics).
    if (refPln && planesMatch(BRepAdaptor_Surface(f2).Plane(), *refPln))
        std::swap(f1, f2);
    out.fRef = f1;
    out.fOther = f2;
    out.plnRef = BRepAdaptor_Surface(f1).Plane();
    out.plnOther = BRepAdaptor_Surface(f2).Plane();

    if (!outwardNormal(f1, out.nRef) || !outwardNormal(f2, out.nOther))
        return false;
    gp_Dir t = out.line.Direction();
    // TRUE in-face directions, robust against nearby features (#57): of the
    // two candidates ±(n × t), pick the one whose probe points land ON the
    // face - by MAJORITY across samples along the edge (a hole under one
    // sample can't flip the answer, unlike the old single-midpoint probe) at
    // a small offset (0.2mm, retry 0.05 for very narrow faces).
    auto pickDir = [&](const TopoDS_Face& f, const gp_Dir& n,
                       gp_Dir& outDir) -> bool {
        gp_Dir cand = n.Crossed(t);
        auto votes = [&](const gp_Dir& d) {
            int hit = 0;
            const int N = 9;
            for (int i = 0; i < N; ++i) {
                const double u = (i + 0.5) / N;
                gp_Pnt base(out.p0.XYZ() * (1.0 - u) + out.p1.XYZ() * u);
                for (double eps : {0.2, 0.05})
                    if (onFace(f, base.Translated(gp_Vec(d) * eps))) {
                        ++hit;
                        break;
                    }
            }
            return hit;
        };
        const int plus = votes(cand), minus = votes(cand.Reversed());
        if (plus == 0 && minus == 0) return false; // can't see the face at all
        outDir = (plus >= minus) ? cand : cand.Reversed();
        return true;
    };
    if (!pickDir(f1, out.nRef, out.dRefDir) ||
        !pickDir(f2, out.nOther, out.dOtherDir))
        return false;

    // Classify the corner from the directions: CONVEX = each face runs to the
    // material side of the other's plane (d·n(other) < 0); CONCAVE (interior
    // corner, 270° of material - a chamfer FILLS it) = both run to the open
    // side. Mixed / near-tangent → refuse; the tool degenerates there. (Note
    // a solid-classifier probe on the outward bisector can NOT tell these
    // apart - it reads OUT for both 90° and 270° corners.)
    const double s1 = out.dRefDir.Dot(out.nOther);
    const double s2 = out.dOtherDir.Dot(out.nRef);
    if (s1 < -0.05 && s2 < -0.05) out.concave = false;
    else if (s1 > 0.05 && s2 > 0.05) out.concave = true;
    else return false;
    (void)body;
    (void)probeEps;
    return true;
}

double paramOn(const gp_Lin& l, const gp_Pnt& p) {
    return gp_Vec(l.Location(), p).Dot(gp_Vec(l.Direction()));
}

// Analyze every selected edge (all-or-nothing: one unsupported edge refuses
// the whole fallback so a multi-edge blend never comes back half-built) and
// merge collinear fragments on the same plane pair into single-span groups.
bool buildGroups(const TopoDS_Shape& body,
                 const std::vector<TopoDS_Edge>& edges, const gp_Pln* refPln,
                 double probeEps, bool asymmetric, bool wantConcave,
                 std::vector<Group>& groups) {
    TopTools_IndexedDataMapOfShapeListOfShape efm;
    TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, efm);

    std::vector<EdgeInfo> infos;
    for (const auto& e : edges) {
        EdgeInfo info;
        if (!analyzeEdge(body, e, efm, refPln, probeEps, info)) return false;
        if (info.concave != wantConcave) return false; // wrong tool family
        infos.push_back(info);
    }

    for (const auto& info : infos) {
        Group* home = nullptr;
        for (auto& g : groups) {
            if (!g.rep.line.Direction().IsParallel(info.line.Direction(),
                                                   1e-4))
                continue;
            if (g.rep.line.Distance(info.pm) > 1e-5) continue;
            const bool straight = planesMatch(g.rep.plnRef, info.plnRef) &&
                                  planesMatch(g.rep.plnOther, info.plnOther);
            const bool crossed = planesMatch(g.rep.plnRef, info.plnOther) &&
                                 planesMatch(g.rep.plnOther, info.plnRef);
            if (!straight && !crossed) continue;
            // Crossed = the per-edge "first face" ordering flipped between
            // fragments. Harmless when both setbacks are equal; genuinely
            // ambiguous for an asymmetric chamfer with no shared reference
            // face, so refuse rather than guess.
            if (crossed && !straight && asymmetric && !refPln) return false;
            home = &g;
            break;
        }
        if (!home) {
            groups.push_back({info, 0.0, 0.0});
            home = &groups.back();
            home->tmin = home->tmax = paramOn(info.line, info.p0);
        }
        for (const gp_Pnt* p : {&info.p0, &info.p1}) {
            double t = paramOn(home->rep.line, *p);
            home->tmin = std::min(home->tmin, t);
            home->tmax = std::max(home->tmax, t);
        }
    }
    return !groups.empty();
}

// Profile plane basis for one group: the corner point at the span start and
// the sweep vector covering the merged span.
bool groupSpan(const Group& g, gp_Pnt& P0, gp_Vec& sweep) {
    const double len = g.tmax - g.tmin;
    if (len < 1e-6) return false;
    P0 = g.rep.line.Location().Translated(gp_Vec(g.rep.line.Direction()) *
                                          g.tmin);
    sweep = gp_Vec(g.rep.line.Direction()) * len;
    return true;
}

// Sweep a closed planar profile wire and report the face generated by
// `blendEdge` (the profile edge that IS the blend cross-section).
TopoDS_Shape sweepProfile(const TopoDS_Wire& w, const TopoDS_Edge& blendEdge,
                          const gp_Vec& sweep, TopoDS_Face& outTemplate) {
    BRepBuilderAPI_MakeFace mf(w, Standard_True);
    if (!mf.IsDone()) return TopoDS_Shape();
    BRepPrimAPI_MakePrism prism(mf.Face(), sweep);
    if (!prism.IsDone()) return TopoDS_Shape();
    const TopTools_ListOfShape& gen = prism.Generated(blendEdge);
    if (gen.IsEmpty() || gen.First().ShapeType() != TopAbs_FACE)
        return TopoDS_Shape();
    outTemplate = TopoDS::Face(gen.First());
    return prism.Shape();
}

// Does the setback line (edge line displaced by `off`) land ON `f` for at
// least ONE of several samples along the group's span? A single-point check
// at the edge midpoint wrongly refused the exact case this fallback exists
// for: a bevel that legitimately sweeps ACROSS a hole in the adjacent face
// (the sample lands in the void). One sample on the face proves the blend
// runs along real face material somewhere; only when EVERY sample misses is
// the blend genuinely bigger than the face it runs along - refuse then.
// Sample support slightly INSIDE the setback (0.05mm short): a setback that
// equals the face's exact extent (B = the wall's full height) otherwise
// probes precisely ON the face's boundary edge, where both the face and the
// solid classifier are numerically coin-flip - B=3.0 on a 3.0 wall refused
// everything while 2.9 worked. The blend only needs support arbitrarily
// close to the setback, not exactly at it.
gp_Vec insetSetback(const gp_Vec& off) {
    const double m = off.Magnitude();
    if (m < 0.1) return off;
    return off * ((m - 0.05) / m);
}

bool setbackTouchesFace(const Group& g, const TopoDS_Face& f,
                        const gp_Vec& off) {
    const gp_Vec probe = insetSetback(off);
    const int kSamples = 9;
    for (int i = 0; i < kSamples; ++i) {
        const double t =
            g.tmin + (g.tmax - g.tmin) * (i + 0.5) / kSamples;
        gp_Pnt p = g.rep.line.Location()
                       .Translated(gp_Vec(g.rep.line.Direction()) * t)
                       .Translated(probe);
        if (onFace(f, p)) return true;
    }
    return false;
}

// The chamfer tool: a triangular wedge - apex just outside the corner, the
// two setback points A/B exactly where the chamfer plane meets the faces.
bool makeChamferTool(const Group& g, double dRef, double dOther, Tool& out) {
    const EdgeInfo& e = g.rep;
    gp_Pnt P0;
    gp_Vec sweep;
    if (!groupSpan(g, P0, sweep)) return false;
    gp_Pnt A = P0.Translated(gp_Vec(e.dRefDir) * dRef);
    gp_Pnt B = P0.Translated(gp_Vec(e.dOtherDir) * dOther);
    // Overshoot guard, hole-tolerant (see setbackTouchesFace).
    if (!setbackTouchesFace(g, e.fRef, gp_Vec(e.dRefDir) * dRef))
        return false;
    if (!setbackTouchesFace(g, e.fOther, gp_Vec(e.dOtherDir) * dOther))
        return false;
    gp_Vec outv = gp_Vec(e.nRef) + gp_Vec(e.nOther);
    if (outv.Magnitude() < 1e-9) return false;
    outv.Normalize();
    // Apex slightly OUTSIDE the corner so the tool's side faces cross the
    // body's faces transversally instead of lying exactly on them (tangent
    // boolean inputs are where cuts get fragile). Immediately outside a
    // convex corner is empty space, so a small bulge cannot over-cut.
    gp_Pnt C = P0.Translated(outv * (std::min(dRef, dOther) * 0.05));

    BRepBuilderAPI_MakePolygon poly;
    poly.Add(C);
    poly.Add(A);
    poly.Add(B);
    poly.Close();
    if (!poly.IsDone()) return false;
    // The profile edge running A–B sweeps into the chamfer-plane face.
    TopoDS_Edge abEdge;
    for (TopExp_Explorer ex(poly.Wire(), TopAbs_EDGE); ex.More(); ex.Next()) {
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(TopoDS::Edge(ex.Current()), v1, v2);
        gp_Pnt q1 = BRep_Tool::Pnt(v1), q2 = BRep_Tool::Pnt(v2);
        if ((q1.Distance(A) < 1e-7 && q2.Distance(B) < 1e-7) ||
            (q1.Distance(B) < 1e-7 && q2.Distance(A) < 1e-7)) {
            abEdge = TopoDS::Edge(ex.Current());
            break;
        }
    }
    if (abEdge.IsNull()) return false;
    out.solid = sweepProfile(poly.Wire(), abEdge, sweep, out.blendTemplate);
    return !out.solid.IsNull();
}

// The fillet tool: same wedge region but bounded by the arc of radius r
// tangent to both faces - apex outside the corner, straight sides to the
// tangency points A/B, arc A→B bulging toward the corner. Cutting it leaves
// exactly the convex fillet cylinder.
bool makeFilletTool(const Group& g, double r, Tool& out) {
    const EdgeInfo& e = g.rep;
    gp_Pnt P0;
    gp_Vec sweep;
    if (!groupSpan(g, P0, sweep)) return false;
    const double c = gp_Vec(e.nRef).Dot(gp_Vec(e.nOther));
    if (c <= -1.0 + 1e-9) return false; // knife edge - no wedge to round
    // Fillet centre: at distance r from BOTH planes, on the material side.
    gp_Vec w = gp_Vec(e.nRef) + gp_Vec(e.nOther);
    if (w.Magnitude() < 1e-9) return false;
    w.Normalize();
    w.Reverse(); // inward bisector
    const double s = r * std::sqrt(2.0 / (1.0 + c));
    gp_Pnt O = P0.Translated(w * s);
    // Tangency points: feet of O on each plane.
    gp_Pnt A = O.Translated(gp_Vec(e.nRef) * r);
    gp_Pnt B = O.Translated(gp_Vec(e.nOther) * r);
    // Same hole-tolerant overshoot guard as the chamfer: the tangency lines
    // must land on their faces SOMEWHERE along the span.
    if (!setbackTouchesFace(g, e.fRef, gp_Vec(P0, A)) ||
        !setbackTouchesFace(g, e.fOther, gp_Vec(P0, B)))
        return false;
    // Arc through the point of the circle nearest the corner (it bulges
    // toward the edge - the removed region lies between arc and corner).
    gp_Vec toCorner(O, P0);
    if (toCorner.Magnitude() < 1e-12) return false;
    toCorner.Normalize();
    gp_Pnt M = O.Translated(toCorner * r);
    GC_MakeArcOfCircle arc(A, M, B);
    if (!arc.IsDone()) return false;
    gp_Vec outv = gp_Vec(e.nRef) + gp_Vec(e.nOther);
    outv.Normalize();
    gp_Pnt C = P0.Translated(outv * (r * 0.05));

    BRepBuilderAPI_MakeEdge ca(C, A), bc(B, C), ab(arc.Value());
    if (!ca.IsDone() || !bc.IsDone() || !ab.IsDone()) return false;
    BRepBuilderAPI_MakeWire mw(ca.Edge(), ab.Edge(), bc.Edge());
    if (!mw.IsDone()) return false;
    // MakeWire may rework the edges it was fed (shared vertices, orientation)
    // so Generated() must be asked about the wire's OWN arc edge - the only
    // circular one of the three.
    TopoDS_Edge arcEdge;
    for (TopExp_Explorer ex(mw.Wire(), TopAbs_EDGE); ex.More(); ex.Next()) {
        if (BRepAdaptor_Curve(TopoDS::Edge(ex.Current())).GetType() ==
            GeomAbs_Circle) {
            arcEdge = TopoDS::Edge(ex.Current());
            break;
        }
    }
    if (arcEdge.IsNull()) return false;
    out.solid = sweepProfile(mw.Wire(), arcEdge, sweep, out.blendTemplate);
    return !out.solid.IsNull();
}

// Does `f` lie on the same support surface as the blend template? Covers the
// two surfaces the tools produce: the chamfer plane and the fillet cylinder.
bool sameSupportSurface(const TopoDS_Face& tmpl, const TopoDS_Face& f) {
    BRepAdaptor_Surface st(tmpl), sf(f);
    if (st.GetType() != sf.GetType()) return false;
    if (st.GetType() == GeomAbs_Plane)
        return planesMatch(st.Plane(), sf.Plane());
    if (st.GetType() == GeomAbs_Cylinder) {
        gp_Cylinder a = st.Cylinder(), b = sf.Cylinder();
        if (std::abs(a.Radius() - b.Radius()) > 1e-6) return false;
        if (!a.Axis().Direction().IsParallel(b.Axis().Direction(), 1e-4))
            return false;
        gp_Lin axisA(a.Axis());
        return axisA.Distance(b.Axis().Location()) < 1e-5;
    }
    return false;
}

// Subtract the tools from the body, validate, and collect the blend faces on
// the result. Captures the cut's generation maps into `ledger` on success.
bool applyCut(const TopoDS_Shape& body, const std::vector<Tool>& tools,
              topo::GenerationLedger& ledger, TopoDS_Shape& outShape,
              std::vector<TopoDS_Shape>& outBlendFaces) {
    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    for (const auto& t : tools) bb.Add(comp, t.solid);

    BRepAlgoAPI_Cut cut(body, comp);
    if (!cut.IsDone()) return false;
    TopoDS_Shape res = cut.Shape();
    if (res.IsNull()) return false;

    // The cut must have actually removed material (a sign error in the tool
    // directions would miss the body entirely and silently produce a no-op
    // "blend"), and the result must be sound.
    GProp_GProps gin, gout;
    BRepGProp::VolumeProperties(body, gin);
    BRepGProp::VolumeProperties(res, gout);
    if (gout.Mass() < 1e-9 || gout.Mass() >= gin.Mass() - 1e-9) return false;
    if (!BRepCheck_Analyzer(res).IsValid()) return false;

    // Blend faces on the result: the surviving pieces of each tool's blend
    // face. Modified() is authoritative; fall back to a same-surface scan if
    // a builder doesn't report tool-face history.
    for (const auto& t : tools) {
        bool found = false;
        try {
            for (const TopoDS_Shape& m : cut.Modified(t.blendTemplate))
                if (m.ShapeType() == TopAbs_FACE) {
                    outBlendFaces.push_back(m);
                    found = true;
                }
        } catch (...) {}
        if (!found) {
            for (TopExp_Explorer ex(res, TopAbs_FACE); ex.More(); ex.Next()) {
                if (sameSupportSurface(t.blendTemplate,
                                       TopoDS::Face(ex.Current()))) {
                    outBlendFaces.push_back(ex.Current());
                    found = true;
                }
            }
        }
        if (!found) return false; // a tool left no blend - over-cut or miss
    }

    ledger.capture(cut, body, TopAbs_EDGE);
    ledger.captureAdd(cut, body, TopAbs_FACE);
    outShape = res;
    return true;
}

// ── Concave (interior-corner) chamfer as a FILL (#57) ──────────────────────
// An interior corner (floor meets a rising wall) is chamfered by ADDING a
// ramp, not cutting. Native OCCT does this fine on clean geometry but gives
// up when the ramp's footprint crosses a feature (a hole in the floor). The
// additive twin of the wedge cut: fuse a ramp prism swept over the full span
// - straight across any feature - then RE-PIERCE it with each crossed void's
// own outline so a hole stays a hole, exactly as if the chamfer had preceded
// the feature in history.

// The fill tool: triangle P0→A→B where A/B sit ON the two faces at the
// chamfer setbacks and the apex is nudged INTO the corner material so the
// fuse meets the body transversally.
bool makeFillTool(const Group& g, double dRef, double dOther, Tool& out) {
    const EdgeInfo& e = g.rep;
    gp_Pnt P0;
    gp_Vec sweep;
    if (!groupSpan(g, P0, sweep)) return false;
    gp_Pnt A = P0.Translated(gp_Vec(e.dRefDir) * dRef);
    gp_Pnt B = P0.Translated(gp_Vec(e.dOtherDir) * dOther);
    // The ramp must rest on real face material somewhere along the span -
    // same hole-tolerant overshoot guard as the cut.
    if (!setbackTouchesFace(g, e.fRef, gp_Vec(e.dRefDir) * dRef)) {
        BC_DBG("[bc] fillTool: dRef=%.2f off fRef\n", dRef);
        return false;
    }
    if (!setbackTouchesFace(g, e.fOther, gp_Vec(e.dOtherDir) * dOther)) {
        BC_DBG("[bc] fillTool: dOther=%.2f off fOther\n", dOther);
        return false;
    }
    // For a concave corner the outward normal bisector points into the OPEN
    // quadrant the ramp will occupy; nudge the apex the other way (into the
    // corner material) so the fuse overlaps instead of kissing.
    gp_Vec inv = gp_Vec(e.nRef) + gp_Vec(e.nOther);
    if (inv.Magnitude() < 1e-9) return false;
    inv.Normalize();
    gp_Pnt C = P0.Translated(inv * (-std::min(dRef, dOther) * 0.05));

    BRepBuilderAPI_MakePolygon poly;
    poly.Add(C);
    poly.Add(A);
    poly.Add(B);
    poly.Close();
    if (!poly.IsDone()) return false;
    TopoDS_Edge abEdge;
    for (TopExp_Explorer ex(poly.Wire(), TopAbs_EDGE); ex.More(); ex.Next()) {
        TopoDS_Vertex v1, v2;
        TopExp::Vertices(TopoDS::Edge(ex.Current()), v1, v2);
        gp_Pnt q1 = BRep_Tool::Pnt(v1), q2 = BRep_Tool::Pnt(v2);
        if ((q1.Distance(A) < 1e-7 && q2.Distance(B) < 1e-7) ||
            (q1.Distance(B) < 1e-7 && q2.Distance(A) < 1e-7)) {
            abEdge = TopoDS::Edge(ex.Current());
            break;
        }
    }
    if (abEdge.IsNull()) return false;
    out.solid = sweepProfile(poly.Wire(), abEdge, sweep, out.blendTemplate);
    return !out.solid.IsNull();
}

// Hip-miter the fill prisms (#57): two blends meeting at a corner form a
// hip along the intersection of their slopes. Two facts make a naive
// plane-clip wrong here: (1) a previously-applied neighbour chamfer has
// already TRIMMED this edge back to its bevel toe, so the prism starts shy
// of the true corner and its flat cap lands right on the neighbour's
// diagonal - the "weird angle" wall; (2) the neighbour's slope plane
// extended to infinity dives far below a long prism, so a half-space clip
// picks the wrong side. Instead: EXTEND the span into the corner (bounded
// by the neighbour's footprint), then SUBTRACT the column standing above
// the neighbour's actual bevel face. Bounded solids only - no half-spaces.

// Trim group spans to where the corner can actually SUPPORT the blend: a
// previously fused ramp's cap base merges (coplanar) with the wall base into
// one longer edge, so a span built from the edge runs past the real wall end
// and the ramp stands on open floor - the "wall sticking up" artifact. Walk
// each end inward until BOTH setback samples land on their faces (bounded,
// so a feature at mid-span is untouched). Ends are then true corner points
// for the fan/extension logic.
void trimGroupEnds(const TopoDS_Shape& body, std::vector<Group>& groups,
                   double dRef, double dOther) {
    for (auto& g : groups) {
        // A setback point is supported if it lands ON its parent face - or
        // ON/INSIDE the body: a neighbouring fused ramp COVERS the floor
        // face there, but leaning into it is exactly what the fuse handles
        // (trimming there yanked the ramp back from the corner and left the
        // flat cap standing at the neighbour's toe whenever this setback
        // exceeded the neighbour's depth). Only genuinely open air trims.
        auto held = [&](const TopoDS_Face& f, const gp_Pnt& p) {
            if (onFace(f, p)) return true;
            BRepClass3d_SolidClassifier sc(body, p, 1e-7);
            return sc.State() != TopAbs_OUT;
        };
        const gp_Vec offRef = insetSetback(gp_Vec(g.rep.dRefDir) * dRef);
        const gp_Vec offOther =
            insetSetback(gp_Vec(g.rep.dOtherDir) * dOther);
        auto supported = [&](double t) {
            gp_Pnt P = g.rep.line.Location().Translated(
                gp_Vec(g.rep.line.Direction()) * t);
            return held(g.rep.fRef, P.Translated(offRef)) &&
                   held(g.rep.fOther, P.Translated(offOther));
        };
        // Walk each end to wherever support actually BEGINS - a fixed cap
        // left the ends hovering unsupported when the true wall started
        // deeper in (and hovering ends are exactly the artifact this exists
        // to prevent). Bounded at 45% of the span per end so a degenerate
        // situation can't consume the whole blend.
        const double step = 0.25;
        const double maxTrim = (g.tmax - g.tmin) * 0.45;
        double trimmed = 0.0;
        while (trimmed < maxTrim && g.tmax - g.tmin > step &&
               !supported(g.tmin + 0.05)) {
            g.tmin += step;
            trimmed += step;
        }
        if (trimmed > 0.0) BC_DBG("[bc] trim: tmin +%.2f\n", trimmed);
        trimmed = 0.0;
        while (trimmed < maxTrim && g.tmax - g.tmin > step &&
               !supported(g.tmax - 0.05)) {
            g.tmax -= step;
            trimmed += step;
        }
        if (trimmed > 0.0) BC_DBG("[bc] trim: tmax -%.2f\n", trimmed);
    }
}

// Extent of `f` beyond `endPt` along `dir` (projection of its bbox corners),
// clamped to [0, cap]. How far the ramp must reach into the corner.
double faceExtentBeyond(const TopoDS_Face& f, const gp_Pnt& endPt,
                        const gp_Vec& dir, double cap) {
    Bnd_Box bb;
    BRepBndLib::Add(f, bb);
    if (bb.IsVoid()) return 0.0;
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    double best = 0.0;
    for (int i = 0; i < 8; ++i) {
        gp_Pnt c(i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0);
        best = std::max(best, gp_Vec(endPt, c).Dot(dir));
    }
    return std::min(std::max(best, 0.0), cap);
}

// Existing inclined (bevel) faces of the body near a point - the neighbour
// chamfers this ramp must miter into. Inclined to at least ONE parent: a
// corner-mate's bevel is perpendicular to our wall, parents themselves and
// plain walls score 0/1 on both and never qualify.
std::vector<TopoDS_Face> bevelFacesNear(const TopoDS_Shape& body,
                                        const Group& g, const gp_Pnt& p,
                                        double tol = 1.0) {
    std::vector<TopoDS_Face> out;
    for (TopExp_Explorer fx(body, TopAbs_FACE); fx.More(); fx.Next()) {
        const TopoDS_Face f = TopoDS::Face(fx.Current());
        BRepAdaptor_Surface s(f);
        if (s.GetType() != GeomAbs_Plane) continue;
        const gp_Dir n = s.Plane().Axis().Direction();
        // Inclined = not (near-)coplanar with a parent and not a plain wall.
        // The ceiling must be TIGHT (≈1.8°): a shallow 11.4×3 ramp's normal
        // is 0.97 aligned with the floor, and the old 0.95 cap silently
        // classified real bevels as floor-like - no fan, no clip, ever.
        const double dr = std::abs(n.Dot(g.rep.nRef));
        const double doth = std::abs(n.Dot(g.rep.nOther));
        if (dr > 0.9995 || doth > 0.9995) continue;   // parent-parallel
        if (dr < 0.05 && doth < 0.05) continue;       // plain wall
        try {
            BRepExtrema_DistShapeShape dss(
                BRepBuilderAPI_MakeVertex(p).Vertex(), f);
            if (dss.IsDone() && dss.Value() < tol) out.push_back(f);
        } catch (...) {}
    }
    return out;
}

// Phase 1: extend each group's span into its hip corners and remember the
// clip faces. Sibling corners (two groups of the SAME op meeting at a
// vertex) extend by the op's own setback; body bevels by their footprint.
struct HipPlan {
    std::vector<TopoDS_Face> bodyClipFaces; // existing neighbour bevels
    std::vector<size_t> siblingClips;       // indices of corner-mate groups
};
std::vector<HipPlan> planHips(const TopoDS_Shape& body,
                              std::vector<Group>& groups, double maxSetback) {
    auto linePt = [](const Group& g, double t) {
        return g.rep.line.Location().Translated(
            gp_Vec(g.rep.line.Direction()) * t);
    };
    // Original (pre-extension) endpoints for corner matching.
    std::vector<std::array<gp_Pnt, 2>> ends;
    for (const auto& g : groups)
        ends.push_back({linePt(g, g.tmin), linePt(g, g.tmax)});

    std::vector<HipPlan> plans(groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        Group& g = groups[i];
        for (int e = 0; e < 2; ++e) {
            const gp_Pnt endPt = ends[i][e];
            const gp_Vec outDir =
                gp_Vec(g.rep.line.Direction()) * (e == 0 ? -1.0 : 1.0);
            double ext = 0.0;
            for (size_t j = 0; j < groups.size(); ++j) {
                if (j == i) continue;
                for (const auto& q : ends[j])
                    if (q.Distance(endPt) < 1e-3) {
                        ext = std::max(ext, maxSetback);
                        plans[i].siblingClips.push_back(j);
                    }
            }
            // Extend ONLY when the end vertex sits ON the bevel (a native
            // neighbour chamfer trimmed this edge back to its toe). At an
            // OUTSIDE block corner the bevel is a full setback away - no
            // extension there; the corner fan handles that join instead.
            for (const TopoDS_Face& f : bevelFacesNear(body, g, endPt, 0.3)) {
                ext = std::max(
                    ext, faceExtentBeyond(f, endPt, outDir, maxSetback + 1.0));
                plans[i].bodyClipFaces.push_back(f);
            }
            if (ext > 1e-9) {
                if (e == 0) g.tmin -= ext;
                else g.tmax += ext;
                BC_DBG("[bc] hip: group %zu end %d extended %.2f\n", i, e,
                       ext);
            }
        }
    }
    return plans;
}

// Phase 3: subtract from each tool the column standing above every clip
// face - the face extruded along this corner's outward bisector. Bounded by
// the face's real footprint, so it only bites in the hip overlap.
void applyHipClips(const TopoDS_Shape& body, std::vector<Tool>& tools,
                   const std::vector<const Group*>& groups,
                   const std::vector<HipPlan>& plans, double maxSetback) {
    for (size_t i = 0; i < tools.size(); ++i) {
        const Group& g = *groups[i];
        gp_Vec v = gp_Vec(g.rep.nRef) + gp_Vec(g.rep.nOther);
        if (v.Magnitude() < 1e-9) continue;
        v.Normalize();
        std::vector<TopoDS_Face> clipFaces = plans[i].bodyClipFaces;
        for (size_t j : plans[i].siblingClips)
            if (j < tools.size()) clipFaces.push_back(tools[j].blendTemplate);
        // OVERLAP detection: any inclined face of the body whose extent
        // intersects this ramp is a hip candidate - end-proximity alone
        // missed a neighbour that was itself FILL-built (a fused ramp does
        // not trim this edge back, so the span end sits at floor level, a
        // full setback away from the neighbour's slope). The bounded column
        // subtraction is inherently local, so over-collecting is safe: a
        // face whose footprint doesn't actually shade the ramp is a no-op.
        {
            Bnd_Box tb;
            BRepBndLib::Add(tools[i].solid, tb);
            tb.Enlarge(0.1);
            for (TopExp_Explorer fx(body, TopAbs_FACE); fx.More(); fx.Next()) {
                const TopoDS_Face f = TopoDS::Face(fx.Current());
                BRepAdaptor_Surface sf(f);
                if (sf.GetType() != GeomAbs_Plane) continue;
                const gp_Dir n = sf.Plane().Axis().Direction();
                const double dr = std::abs(n.Dot(g.rep.nRef));
                const double doth = std::abs(n.Dot(g.rep.nOther));
                if (dr > 0.9995 || doth > 0.9995) continue; // parent-parallel
                if (dr < 0.05 && doth < 0.05) continue;     // plain wall
                if (dr < 0.05 || doth < 0.05) {
                    // perpendicular-ish to one parent: fine (corner-mate)
                }
                Bnd_Box fb;
                BRepBndLib::Add(f, fb);
                if (fb.IsVoid() || tb.IsOut(fb)) continue;
                bool dup = false;
                for (const auto& c : clipFaces)
                    if (c.IsSame(f)) dup = true;
                if (!dup) clipFaces.push_back(f);
            }
        }
        BC_DBG("[bc] hip: tool %zu has %zu clip candidates\n", i,
               clipFaces.size());
        for (const TopoDS_Face& f : clipFaces) {
            try {
                gp_Dir fn;
                if (!outwardNormal(f, fn)) { BC_DBG("[bc] hip: no normal\n"); continue; }
                gp_Vec up = (v.Dot(gp_Vec(fn)) >= 0.0) ? v : v.Reversed();
                BRepPrimAPI_MakePrism col(f, up * (maxSetback + 2.0));
                if (!col.IsDone()) { BC_DBG("[bc] hip: column failed\n"); continue; }
                BRepAlgoAPI_Cut cut(tools[i].solid, col.Shape());
                if (!cut.IsDone()) { BC_DBG("[bc] hip: cut failed\n"); continue; }
                TopoDS_Shape clipped = cut.Shape();
                if (clipped.IsNull()) { BC_DBG("[bc] hip: cut null\n"); continue; }
                GProp_GProps gv, go;
                BRepGProp::VolumeProperties(clipped, gv);
                BRepGProp::VolumeProperties(tools[i].solid, go);
                if (gv.Mass() < 1e-9) { BC_DBG("[bc] hip: ate whole ramp\n"); continue; }
                if (gv.Mass() > go.Mass() - 1e-9) { BC_DBG("[bc] hip: graze no-op\n"); continue; }
                tools[i].solid = clipped;
                BC_DBG("[bc] hip: clipped tool %zu (%.1f -> %.1f)\n", i,
                       go.Mass(), gv.Mass());
            } catch (...) { BC_DBG("[bc] hip: exception\n"); }
        }
    }
}

// Corner FAN (#57): two interior-corner ramps wrapping an OUTSIDE plan
// corner of a raised block never overlap - each ends in a flat cap at the
// corner, side by side, which reads as two abrupt walls. Native chamfers
// join them with a triangular corner facet spreading from the wall-corner
// top W down to the two toes. With matching wall setbacks that facet bounds
// an exact tetrahedron {V, T1, T2, W}: V the shared floor corner, T1 our
// toe, T2 the neighbour's toe (read off its bevel face), W the shared top.
// Only fires when a neighbouring bevel's cap edge lies in our end plane AND
// one of its corners coincides with our own wall-top point (equal heights);
// anything else keeps the flat cap.
TopoDS_Shape tetraSolid(const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c,
                        const gp_Pnt& d, TopoDS_Face& outFanFace) {
    auto tri = [](const gp_Pnt& p, const gp_Pnt& q, const gp_Pnt& r) {
        BRepBuilderAPI_MakePolygon poly(p, q, r, Standard_True);
        return BRepBuilderAPI_MakeFace(poly.Wire(), Standard_True).Face();
    };
    try {
        BRepBuilderAPI_Sewing sew(1e-6);
        sew.Add(tri(a, b, c));
        sew.Add(tri(a, c, d));
        sew.Add(tri(a, d, b));
        TopoDS_Face fan = tri(b, c, d); // W-T1-T2 facet - the visible fan
        sew.Add(fan);
        sew.Perform();
        TopoDS_Shape shell = sew.SewedShape();
        if (shell.IsNull()) { BC_DBG("[bc] tetra: sew null\n"); return TopoDS_Shape(); }
        TopExp_Explorer sx(shell, TopAbs_SHELL);
        if (!sx.More()) { BC_DBG("[bc] tetra: no shell\n"); return TopoDS_Shape(); }
        BRepBuilderAPI_MakeSolid ms(TopoDS::Shell(sx.Current()));
        if (!ms.IsDone()) return TopoDS_Shape();
        TopoDS_Shape solid = ms.Solid();
        GProp_GProps gp;
        BRepGProp::VolumeProperties(solid, gp);
        if (std::abs(gp.Mass()) < 1e-9) return TopoDS_Shape();
        if (gp.Mass() < 0) solid.Reverse();
        outFanFace = fan;
        return solid;
    } catch (...) { return TopoDS_Shape(); }
}

void addCornerFans(const TopoDS_Shape& body,
                   const std::vector<const Group*>& groups,
                   const std::vector<std::array<gp_Pnt, 2>>& originalEnds,
                   double dRef, double dOther, std::vector<Tool>& tools) {
    // Search radius: the neighbour's SLOPE face sits a full setback away
    // from the floor-level corner vertex (only its vertical cap touches it),
    // so search wide; the cap-corner/wall-top matching below is the tight
    // gate that keeps this from firing spuriously.
    const double nearTol = std::max(dRef, dOther) + 1.0;
    const size_t nOrig = std::min(tools.size(), groups.size());
    for (size_t i = 0; i < nOrig; ++i) {
        const Group& g = *groups[i];
        for (int e = 0; e < 2; ++e) {
            // ORIGINAL span end - planHips may have extended the span for an
            // inside corner; the fan belongs at the true edge end.
            const gp_Pnt V = originalEnds[i][e];
            const gp_Vec outward =
                gp_Vec(g.rep.line.Direction()) * (e == 0 ? -1.0 : 1.0);
            const gp_Pnt A = V.Translated(gp_Vec(g.rep.dRefDir) * dRef);
            const gp_Pnt B = V.Translated(gp_Vec(g.rep.dOtherDir) * dOther);
            const auto nearFaces = bevelFacesNear(body, g, V, nearTol);
            BC_DBG("[bc] fan: group %zu end %d V=(%.1f,%.1f,%.1f) %zu candidates\n",
                   i, e, V.X(), V.Y(), V.Z(), nearFaces.size());
            for (const TopoDS_Face& f : nearFaces) {
                // W = the face vertex that IS our wall-top point (equal
                // heights - the tight gate); T1 = our own toe at this end;
                // T2 = the face's floor-level vertex nearest the corner
                // (the neighbour's toe where its ramp meets our end plane).
                gp_Pnt W, T1;
                bool haveW = false;
                std::vector<gp_Pnt> verts;
                for (TopExp_Explorer vx(f, TopAbs_VERTEX); vx.More();
                     vx.Next())
                    verts.push_back(
                        BRep_Tool::Pnt(TopoDS::Vertex(vx.Current())));
                for (const gp_Pnt& p : verts) {
                    if (p.Distance(A) < 0.3) { W = p; T1 = B; haveW = true; }
                    else if (p.Distance(B) < 0.3) { W = p; T1 = A; haveW = true; }
                    if (haveW) break;
                }
                if (!haveW) { BC_DBG("[bc] fan: no W match (A=(%.1f,%.1f,%.1f) B=(%.1f,%.1f,%.1f))\n",
                              A.X(),A.Y(),A.Z(),B.X(),B.Y(),B.Z()); continue; }
                gp_Vec wallDir(V, W);
                if (wallDir.Magnitude() < 1e-9) continue;
                wallDir.Normalize();
                // "Floor level" must be measured along the FLOOR's normal -
                // projecting onto the (slightly tilted, trim-offset) V→W
                // axis amplifies with distance and rejected an 11mm-away toe
                // over a 0.1mm corner offset. The floor parent is whichever
                // face's normal is the more parallel to the wall direction.
                const gp_Dir nFloor =
                    (std::abs(gp_Vec(g.rep.nRef).Dot(wallDir)) >
                     std::abs(gp_Vec(g.rep.nOther).Dot(wallDir)))
                        ? g.rep.nRef
                        : g.rep.nOther;
                gp_Pnt T2;
                double best = 1e18;
                for (const gp_Pnt& p : verts) {
                    if (p.Distance(W) < 1e-6) continue;
                    if (std::abs(gp_Vec(V, p).Dot(gp_Vec(nFloor))) > 0.3)
                        continue; // not floor level
                    const double d = p.Distance(V);
                    if (d < best) { best = d; T2 = p; }
                }
                // T2's reach is bounded by the NEIGHBOUR's own bevel size -
                // not ours: an 11.4-deep side ramp's toe is legitimately
                // 11.4mm out, and bounding by our setback silently rejected
                // the fan at exactly the corners Steve was building.
                double fDiag = 0.0;
                {
                    Bnd_Box fb;
                    BRepBndLib::Add(f, fb);
                    if (!fb.IsVoid()) {
                        double x0, y0, z0, x1, y1, z1;
                        fb.Get(x0, y0, z0, x1, y1, z1);
                        fDiag = gp_Pnt(x0, y0, z0)
                                    .Distance(gp_Pnt(x1, y1, z1));
                    }
                }
                if (best > fDiag + 1.0) continue;
                if (T2.Distance(V) < 1e-6 || T1.Distance(T2) < 1e-6) continue;
                // OUTSIDE corner only: the neighbour's toe continues past our
                // end ALONG our edge direction (the ramps wrap a block corner
                // in disjoint quadrants). An INSIDE corner's neighbour toe
                // sits out in our own floor quadrant - that join is handled
                // by extension + column clip, and a fan there is wrong.
                gp_Vec toT2(V, T2);
                const double along = toT2.Dot(outward);
                gp_Vec perp = toT2 - outward * along;
                if (along < 0.5 || perp.Magnitude() > 0.5) {
                    BC_DBG("[bc] fan: not an outside corner (along=%.2f "
                           "perp=%.2f)\n", along, perp.Magnitude());
                    continue;
                }
                // TRUE MITER, not a fan facet: sweep OUR profile around the
                // corner (to the neighbour's toe) and clip it by the
                // neighbour's actual slope PLANE. The clipped face lies IN
                // that plane, so after the fuse (+ coplanar merge) the two
                // slopes read as one continuous miter - a flat tetra here
                // showed up as a visible third facet ("almost acceptable").
                Tool piece;
                try {
                    const gp_Pnt Vb =
                        V.Translated(outward * (-0.05)); // overlap our ramp
                    // Apex overlap: shift the corner HORIZONTALLY into the
                    // wall, keeping it exactly AT floor height. The shift
                    // fills the neighbour's trim slot at the cap base (so
                    // the clip face lands edge-to-edge on the neighbour
                    // bevel and the coplanar merge takes), while putting
                    // nothing below the floor - a downward apex dip swept
                    // to a toe on the plate edge imprinted the outer wall
                    // as a coplanar strip + razor faces, the visible
                    // "bottom sliver" no merge could remove.
                    const gp_Dir nWall =
                        (std::abs(gp_Vec(g.rep.nRef).Dot(gp_Vec(nFloor))) >
                         0.9)
                            ? g.rep.nOther
                            : g.rep.nRef;
                    const gp_Pnt Cp = Vb.Translated(
                        gp_Vec(nWall) * (-std::min(dRef, dOther) * 0.05));
                    const gp_Pnt Ab =
                        Vb.Translated(gp_Vec(g.rep.dRefDir) * dRef);
                    const gp_Pnt Bb =
                        Vb.Translated(gp_Vec(g.rep.dOtherDir) * dOther);
                    BRepBuilderAPI_MakePolygon poly;
                    poly.Add(Cp);
                    poly.Add(Ab);
                    poly.Add(Bb);
                    poly.Close();
                    if (!poly.IsDone()) continue;
                    BRepBuilderAPI_MakeFace mf(poly.Wire(), Standard_True);
                    if (!mf.IsDone()) continue;
                    BRepPrimAPI_MakePrism pr(
                        mf.Face(), outward * (along + 0.05));
                    if (!pr.IsDone()) continue;
                    TopoDS_Shape pieceSolid = pr.Shape();
                    // Clip by the neighbour's slope plane: bounded box on the
                    // ABOVE side (the keep point sits just off the corner at
                    // floor level, always under the neighbour's slope there).
                    const gp_Pln nbPln = BRepAdaptor_Surface(f).Plane();
                    const gp_Dir nn = nbPln.Axis().Direction();
                    const gp_Pnt keepPt =
                        V.Translated(outward * 0.1)
                            .Translated(gp_Vec(V, W) * 0.02);
                    const double sdKeep =
                        gp_Vec(nbPln.Location(), keepPt).Dot(gp_Vec(nn));
                    if (std::abs(sdKeep) < 1e-9) continue;
                    const double sKeep = (sdKeep > 0) ? 1.0 : -1.0;
                    Bnd_Box pb;
                    BRepBndLib::Add(pieceSolid, pb);
                    double x0, y0, z0, x1, y1, z1;
                    pb.Get(x0, y0, z0, x1, y1, z1);
                    const double diag =
                        gp_Pnt(x0, y0, z0).Distance(gp_Pnt(x1, y1, z1));
                    const gp_Pnt centerOnPlane = keepPt.Translated(
                        gp_Vec(nn) * (-sdKeep));
                    const gp_Dir away = (sKeep > 0) ? nn.Reversed() : nn;
                    gp_Pnt corner = centerOnPlane
                        .Translated(gp_Vec(gp_Ax2(centerOnPlane, away)
                                               .XDirection()) * (-1.5 * diag))
                        .Translated(gp_Vec(gp_Ax2(centerOnPlane, away)
                                               .YDirection()) * (-1.5 * diag));
                    BRepPrimAPI_MakeBox mb(gp_Ax2(corner, away), 3.0 * diag,
                                           3.0 * diag, diag + 1.0);
                    BRepAlgoAPI_Cut cut(pieceSolid, mb.Shape());
                    if (!cut.IsDone()) continue;
                    TopoDS_Shape clipped = cut.Shape();
                    GProp_GProps gv;
                    BRepGProp::VolumeProperties(clipped, gv);
                    if (clipped.IsNull() || gv.Mass() < 1e-9) continue;
                    piece.solid = clipped;
                    piece.blendTemplate = tools[i].blendTemplate;
                } catch (...) { continue; }
                tools.push_back(piece);
                BC_DBG("[bc] hip: corner miter added at group %zu end %d\n",
                       i, e);
            }
        }
    }
}

// Fuse the ramp(s) onto the body, then re-pierce every void whose outline
// (an inner wire of one of the corner's faces) the ramp roofed over. Boss
// outlines - inner wires with material ABOVE the face - are left alone.
bool applyFill(const TopoDS_Shape& body, const std::vector<Tool>& tools,
               const std::vector<const Group*>& groups, double maxSetback,
               topo::GenerationLedger& ledger, TopoDS_Shape& outShape,
               std::vector<TopoDS_Shape>& outBlendFaces) {
    double toolVol = 0.0;
    TopoDS_Shape res = body;
    int ti = 0;
    for (const auto& t : tools) {
        GProp_GProps gt;
        BRepGProp::VolumeProperties(t.solid, gt);
        toolVol += gt.Mass();
        GProp_GProps gb;
        BRepGProp::VolumeProperties(res, gb);
        BRepAlgoAPI_Fuse fuse(res, t.solid);
        if (!fuse.IsDone()) { BC_DBG("[bc] fill: fuse not done\n"); return false; }
        res = fuse.Shape();
        if (res.IsNull()) { BC_DBG("[bc] fill: fuse null\n"); return false; }
        GProp_GProps ga;
        BRepGProp::VolumeProperties(res, ga);
        BC_DBG("[bc] fill: fuse tool %d vol %.1f: %.1f -> %.1f\n", ti++,
               gt.Mass(), gb.Mass(), ga.Mass());
        ledger.capture(fuse, body, TopAbs_EDGE);
        ledger.captureAdd(fuse, body, TopAbs_FACE);
    }

    // Re-pierce: for each inner wire of each corner face, if it outlines a
    // VOID the ramp actually roofed over, extrude the outline through the
    // ramp height and subtract - the hole punches through the new ramp just
    // as a feature cut after the chamfer would have. Two guards keep this
    // surgical: the outline's bbox must intersect a ramp's bbox (a wire on
    // the far side of the part is none of our business), and the region just
    // inside the outline must be EMPTY above the face all around its rim -
    // a centroid-only probe misread a screw boss's ring footprint (probe
    // fell down the boss's own bore) as a void and decapitated the boss.
    Bnd_Box toolBox;
    for (const auto& t : tools) BRepBndLib::Add(t.solid, toolBox);
    toolBox.Enlarge(0.1);
    for (const Group* g : groups) {
        for (const TopoDS_Face* fp : {&g->rep.fRef, &g->rep.fOther}) {
            const TopoDS_Face& f = *fp;
            BRepAdaptor_Surface surf(f);
            if (surf.GetType() != GeomAbs_Plane) continue;
            gp_Dir n;
            if (!outwardNormal(f, n)) continue;
            const TopoDS_Wire outer = BRepTools::OuterWire(f);
            for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More(); wx.Next()) {
                if (wx.Current().IsSame(outer)) continue;
                TopoDS_Wire w = TopoDS::Wire(wx.Current());
                Bnd_Box wb;
                BRepBndLib::Add(w, wb);
                if (toolBox.IsOut(wb)) continue; // ramp never reaches it
                // Face bounded by the void outline, on the face's plane.
                BRepBuilderAPI_MakeFace mfw(surf.Plane(), w, Standard_True);
                if (!mfw.IsDone()) continue;
                GProp_GProps gw;
                BRepGProp::SurfaceProperties(mfw.Face(), gw);
                if (gw.Mass() < 1e-9) continue;
                const gp_Pnt centroid = gw.CentreOfMass();
                // Void all around? Probe just above the face at points a
                // little inside the rim (plus the centroid) - ANY material
                // hit means a boss lives inside this outline; keep it.
                bool boss = false;
                {
                    auto probeIn = [&](const gp_Pnt& q) {
                        BRepClass3d_SolidClassifier sc(
                            body, q.Translated(gp_Vec(n) * 0.1), 1e-7);
                        return sc.State() == TopAbs_IN;
                    };
                    if (probeIn(centroid)) boss = true;
                    for (TopExp_Explorer exw(w, TopAbs_EDGE);
                         !boss && exw.More(); exw.Next()) {
                        BRepAdaptor_Curve wc(TopoDS::Edge(exw.Current()));
                        gp_Pnt m = wc.Value(
                            (wc.FirstParameter() + wc.LastParameter()) * 0.5);
                        gp_Vec toC(m, centroid);
                        if (toC.Magnitude() < 1e-9) continue;
                        toC.Normalize();
                        if (probeIn(m.Translated(
                                toC * std::min(0.2, m.Distance(centroid)))))
                            boss = true;
                    }
                }
                if (boss) continue;
                // Start the pierce slightly BELOW the face: the ramp's apex
                // nudge dips a hair under the face plane, and over a void
                // that sliver would otherwise survive as a floating skin.
                const double down = 0.05 * maxSetback + 0.01;
                gp_Trsf shift;
                shift.SetTranslation(gp_Vec(n) * (-down));
                TopoDS_Shape base =
                    BRepBuilderAPI_Transform(mfw.Face(), shift, true).Shape();
                BRepPrimAPI_MakePrism pierce(
                    base, gp_Vec(n) * (maxSetback + 1.0 + down));
                if (!pierce.IsDone()) continue;
                BRepAlgoAPI_Cut cut(res, pierce.Shape());
                if (!cut.IsDone()) { BC_DBG("[bc] fill: pierce cut not done\n"); return false; }
                TopoDS_Shape s = cut.Shape();
                if (s.IsNull()) { BC_DBG("[bc] fill: pierce cut null\n"); return false; }
                res = s;
            }
        }
    }

    // Merge coplanar face fragments (the fuse leaves seam edges wherever the
    // ramp lands exactly flush against an existing bevel or wall - visible as
    // hairline steps at the joint). UnifySameDomain is cosmetic-but-correct
    // here; fall back to the un-merged shape if it misbehaves.
    // Merge coplanar face fragments the fuse/booleans left behind - they read
    // in the viewport as a fan of seams across the ramp even though the
    // surface is geometrically flat. TWO merge strategies are needed and
    // NEITHER alone suffices:
    //   * plain UnifySameDomain merges the multi-edge picture-frame case
    //     cleanly, but leaves a miter wedge behind when a ZERO-LENGTH edge
    //     at the toe junction blocks it (the #58 single-miter case);
    //   * FixSmallEdges-then-USD drops that degenerate edge so the wedge
    //     merges, but CORRUPTS the multi-edge case into an invalid shape.
    // Run both, keep the valid candidate with the FEWEST faces (= most
    // merged), preferring plain USD on a tie. Merging is volume-neutral, so
    // reject any candidate that moved the volume more than rounding noise.
    {
        GProp_GProps gr;
        BRepGProp::VolumeProperties(res, gr);
        const double volTol = std::max(1e-2, gr.Mass() * 1e-6);
        auto faceCount = [](const TopoDS_Shape& s) {
            int n = 0;
            for (TopExp_Explorer fx(s, TopAbs_FACE); fx.More(); fx.Next()) n++;
            return n;
        };
        int bestFaces = faceCount(res);
        auto consider = [&](const TopoDS_Shape& cand) {
            if (cand.IsNull()) return;
            if (!BRepCheck_Analyzer(cand).IsValid()) return;
            GProp_GProps gc;
            BRepGProp::VolumeProperties(cand, gc);
            if (std::abs(gc.Mass() - gr.Mass()) > volTol) return;
            int n = faceCount(cand);
            if (n < bestFaces) { bestFaces = n; res = cand; }
        };
        try {
            ShapeUpgrade_UnifySameDomain u(res, Standard_True, Standard_True,
                                           Standard_False);
            u.SetAngularTolerance(materializr::kUnifyAngularTol);
            u.Build();
            consider(u.Shape());
        } catch (...) {}
        try {
            TopoDS_Shape pre = res;
            ShapeFix_Wireframe wf(pre);
            wf.SetPrecision(1e-4);
            wf.SetMaxTolerance(1e-3);
            wf.ModeDropSmallEdges() = Standard_True;
            wf.FixSmallEdges();
            wf.FixWireGaps();
            if (!wf.Shape().IsNull()) {
                ShapeUpgrade_UnifySameDomain u(wf.Shape(), Standard_True,
                                               Standard_True, Standard_False);
                u.SetAngularTolerance(materializr::kUnifyAngularTol);
                u.Build();
                consider(u.Shape());
            }
        } catch (...) {}
    }

    // Must have ADDED material, no more than the ramps themselves, and sound.
    GProp_GProps gin, gout;
    BRepGProp::VolumeProperties(body, gin);
    BRepGProp::VolumeProperties(res, gout);
    BC_DBG("[bc] fill: vol in=%.1f out=%.1f toolVol=%.1f valid=%d\n",
           gin.Mass(), gout.Mass(), toolVol,
           (int)BRepCheck_Analyzer(res).IsValid());
    if (gout.Mass() <= gin.Mass() + 1e-9 ||
        gout.Mass() > gin.Mass() + toolVol + 1e-3)
        return false;
    if (!BRepCheck_Analyzer(res).IsValid()) return false;

    // The ramp's slanted faces on the result.
    for (const auto& t : tools) {
        bool found = false;
        for (TopExp_Explorer ex(res, TopAbs_FACE); ex.More(); ex.Next()) {
            if (sameSupportSurface(t.blendTemplate, TopoDS::Face(ex.Current()))) {
                outBlendFaces.push_back(ex.Current());
                found = true;
            }
        }
        if (!found) { BC_DBG("[bc] fill: no blend face for a tool\n"); return false; }
    }

    outShape = res;
    return true;
}

} // namespace

bool cutChamfer(const TopoDS_Shape& body,
                const std::vector<TopoDS_Edge>& edges,
                double dRef, double dOther,
                const TopoDS_Face& refFace,
                topo::GenerationLedger& ledger,
                TopoDS_Shape& outShape,
                std::vector<TopoDS_Shape>& outBlendFaces) {
    outShape.Nullify();
    outBlendFaces.clear();
    if (body.IsNull() || edges.empty() || dRef <= 0.0 || dOther <= 0.0)
        return false;
    try {
        const bool asymmetric = std::abs(dRef - dOther) > 1e-12;
        gp_Pln refPln;
        bool hasRefPln = false;
        if (!refFace.IsNull()) {
            BRepAdaptor_Surface rs(refFace);
            if (rs.GetType() != GeomAbs_Plane) {
                if (asymmetric) return false; // can't aim dRef reliably
            } else {
                refPln = rs.Plane();
                hasRefPln = true;
            }
        }

        std::vector<Group> groups;
        if (!buildGroups(body, edges, hasRefPln ? &refPln : nullptr,
                         0.1 * std::min(dRef, dOther), asymmetric,
                         /*wantConcave=*/false, groups))
            return false;

        std::vector<Tool> tools;
        for (const auto& g : groups) {
            Tool t;
            if (!makeChamferTool(g, dRef, dOther, t)) return false;
            tools.push_back(t);
        }
        return applyCut(body, tools, ledger, outShape, outBlendFaces);
    } catch (...) {
        return false;
    }
}

bool fillChamfer(const TopoDS_Shape& body,
                 const std::vector<TopoDS_Edge>& edges,
                 double dRef, double dOther,
                 const TopoDS_Face& refFace,
                 topo::GenerationLedger& ledger,
                 TopoDS_Shape& outShape,
                 std::vector<TopoDS_Shape>& outBlendFaces) {
    outShape.Nullify();
    outBlendFaces.clear();
    if (body.IsNull() || edges.empty() || dRef <= 0.0 || dOther <= 0.0)
        return false;
    try {
        const bool asymmetric = std::abs(dRef - dOther) > 1e-12;
        gp_Pln refPln;
        bool hasRefPln = false;
        if (!refFace.IsNull()) {
            BRepAdaptor_Surface rs(refFace);
            if (rs.GetType() != GeomAbs_Plane) {
                if (asymmetric) return false;
            } else {
                refPln = rs.Plane();
                hasRefPln = true;
            }
        }

        std::vector<Group> groups;
        if (!buildGroups(body, edges, hasRefPln ? &refPln : nullptr,
                         0.1 * std::min(dRef, dOther), asymmetric,
                         /*wantConcave=*/true, groups))
            return false;

        // Hip corners: extend spans into neighbouring bevels FIRST (the
        // neighbour's chamfer trimmed this edge back to its toe, so the
        // prism must reach the true corner), build the tools on the
        // extended spans, then clip each by the column above every
        // neighbouring bevel face - the hip emerges by construction.
        const double maxSetback = std::max(dRef, dOther);
        trimGroupEnds(body, groups, dRef, dOther);
        std::vector<std::array<gp_Pnt, 2>> originalEnds;
        for (const auto& g : groups) {
            auto pt = [&](double t) {
                return g.rep.line.Location().Translated(
                    gp_Vec(g.rep.line.Direction()) * t);
            };
            originalEnds.push_back({pt(g.tmin), pt(g.tmax)});
        }
        std::vector<HipPlan> plans = planHips(body, groups, maxSetback);

        std::vector<Tool> tools;
        std::vector<const Group*> groupPtrs;
        for (const auto& g : groups) {
            Tool t;
            if (!makeFillTool(g, dRef, dOther, t)) return false;
            tools.push_back(t);
            groupPtrs.push_back(&g);
        }
        applyHipClips(body, tools, groupPtrs, plans, maxSetback);
        addCornerFans(body, groupPtrs, originalEnds, dRef, dOther, tools);
        return applyFill(body, tools, groupPtrs, maxSetback,
                         ledger, outShape, outBlendFaces);
    } catch (...) {
        return false;
    }
}

bool cutFillet(const TopoDS_Shape& body,
               const std::vector<TopoDS_Edge>& edges, double radius,
               topo::GenerationLedger& ledger, TopoDS_Shape& outShape,
               std::vector<TopoDS_Shape>& outBlendFaces) {
    outShape.Nullify();
    outBlendFaces.clear();
    if (body.IsNull() || edges.empty() || radius <= 0.0) return false;
    try {
        std::vector<Group> groups;
        if (!buildGroups(body, edges, nullptr, 0.1 * radius, false,
                         /*wantConcave=*/false, groups))
            return false;

        std::vector<Tool> tools;
        for (const auto& g : groups) {
            Tool t;
            if (!makeFilletTool(g, radius, t)) return false;
            tools.push_back(t);
        }
        return applyCut(body, tools, ledger, outShape, outBlendFaces);
    } catch (...) {
        return false;
    }
}

} // namespace blendcut
} // namespace materializr
