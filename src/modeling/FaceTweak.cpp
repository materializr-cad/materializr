#include "FaceTweak.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_IntCS.hxx>
#include <GeomAPI_IntSS.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Wire.hxx>
#include <Standard_ErrorHandler.hxx> // OCC_CATCH_SIGNALS
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace materializr::tweak {

namespace {

// Shape-keyed lookup. TopoDS handles have no operator<, and the identity we
// want is IsSame: the same vertex reached through two different faces has to
// land on one entry.
struct SameKey {
    bool operator()(const TopoDS_Shape& a, const TopoDS_Shape& b) const {
        if (a.TShape().get() != b.TShape().get())
            return a.TShape().get() < b.TShape().get();
        return a.Location().HashCode() < b.Location().HashCode();
    }
};
template <class V> using ShapeMap = std::map<TopoDS_Shape, V, SameKey>;
using ShapeSet = std::set<TopoDS_Shape, SameKey>;

bool planeOf(const TopoDS_Face& f, gp_Pln& out) {
    BRepAdaptor_Surface s(f, Standard_False);
    if (s.GetType() != GeomAbs_Plane) return false;
    out = s.Plane();
    return true;
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

double areaOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(s, g);
    return g.Mass();
}

gp_Pnt midOf(const TopoDS_Edge& e) {
    double f, l;
    Handle(Geom_Curve) c = BRep_Tool::Curve(e, f, l);
    if (c.IsNull()) return gp_Pnt();
    return c->Value(0.5 * (f + l));
}

// Where a curve crosses a plane, nearest `near`. This is the corner solve: the
// edge running off the moved face keeps its geometry, so the new corner is
// simply where that edge now meets the face's new plane. Works for a line, an
// arc, a spline — which is the whole reason the neighbours are free to curve.
bool curveMeetsPlane(const Handle(Geom_Curve)& c, double f, double l,
                     const gp_Pln& pl, const gp_Pnt& near, gp_Pnt& out) {
    if (c.IsNull()) return false;
    Handle(Geom_Plane) gp = new Geom_Plane(pl);
    GeomAPI_IntCS ics(c, gp);
    if (!ics.IsDone() || ics.NbPoints() == 0) return false;

    // A circle can cross a plane twice and a spline more often than that. The
    // corner we want is the one the edge already had, so pick by proximity to
    // where the vertex sits now — not, say, the first solution the kernel
    // happens to report.
    // Deliberately NOT clamped to the edge's own parameter range. Pushing a
    // face outward puts the new corner PAST the end of the edge it slides
    // along — a box growing taller needs its vertical edges to lengthen, and
    // they lengthen along the same carrier line. Clamping looked like a
    // sensible guard and silently broke every outward move while leaving tilts
    // working, because a tilt keeps the corners between the old ends.
    (void)f;
    (void)l;
    bool found = false;
    double best = 1e300;
    for (int i = 1; i <= ics.NbPoints(); ++i) {
        const gp_Pnt p = ics.Point(i);
        const double d = p.Distance(near);
        if (d < best) { best = d; out = p; found = true; }
    }
    return found;
}

// Parameter of `p` on `c`, and whether it actually lies there.
bool paramOn(const Handle(Geom_Curve)& c, const gp_Pnt& p, double tol, double& out) {
    GeomAPI_ProjectPointOnCurve proj(p, c);
    if (proj.NbPoints() == 0) return false;
    if (proj.LowerDistance() > tol) return false;
    out = proj.LowerDistanceParameter();
    return true;
}

} // namespace

const char* refusalText(Refusal r) {
    switch (r) {
        case Refusal::None:              return "";
        case Refusal::FaceNotFound:      return "that face isn't part of this body";
        case Refusal::NotPlanar:         return "only flat faces can be tweaked yet";
        case Refusal::NonManifoldCorner: return "a corner of this face doesn't have a single edge running into the body";
        case Refusal::NoChange:          return "sliding a flat face inside its own plane doesn't move anything - tilt it, or push it along its normal";
        case Refusal::Degenerate:        return "that move takes the face past an edge it has to stay attached to";
        case Refusal::NoIntersection:    return "a neighbouring face no longer meets this one after that move";
        case Refusal::OffCurve:          return "an edge running off this face can't reach its new corner";
        case Refusal::BuildFailed:       return "the rebuilt body wouldn't close up";
        case Refusal::Invalid:           return "the rebuilt body failed validation";
    }
    return "";
}

Result moveFace(const TopoDS_Shape& body, const TopoDS_Face& face,
                const gp_Trsf& xf) {
    Result res;
    if (body.IsNull() || face.IsNull()) {
        res.refusal = Refusal::FaceNotFound;
        return res;
    }

    try {
        OCC_CATCH_SIGNALS

        bool onBody = false;
        for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next())
            if (ex.Current().IsSame(face)) { onBody = true; break; }
        if (!onBody) { res.refusal = Refusal::FaceNotFound; return res; }

        gp_Pln planeF;
        if (!planeOf(face, planeF)) { res.refusal = Refusal::NotPlanar; return res; }
        const gp_Pln movedF = planeF.Transformed(xf);

        // A move that leaves the plane where it was would rebuild every corner
        // onto its own position and report success having changed nothing. Say
        // so instead — the user made a gesture and deserves to know why the
        // body didn't budge.
        if (planeF.Axis().Direction().IsParallel(movedF.Axis().Direction(), 1e-9) &&
            std::abs(movedF.Distance(planeF.Location())) < Precision::Confusion()) {
            res.refusal = Refusal::NoChange;
            return res;
        }

        TopTools_IndexedDataMapOfShapeListOfShape vertEdges, edgeFaces;
        TopExp::MapShapesAndAncestors(body, TopAbs_VERTEX, TopAbs_EDGE, vertEdges);
        TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

        // The edges and vertices of the face being moved.
        ShapeSet edgesOnF, vertsOnF;
        for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next())
            if (!BRep_Tool::Degenerated(TopoDS::Edge(ex.Current())))
                edgesOnF.insert(ex.Current());
        for (TopExp_Explorer ex(face, TopAbs_VERTEX); ex.More(); ex.Next())
            vertsOnF.insert(ex.Current());
        if (edgesOnF.empty() || vertsOnF.empty()) {
            res.refusal = Refusal::BuildFailed;
            return res;
        }

        // ── Corners: where each leaving edge now crosses the moved plane ─────
        ShapeMap<gp_Pnt> movedVertices;
        for (const TopoDS_Shape& vs : vertsOnF) {
            const TopoDS_Vertex v = TopoDS::Vertex(vs);
            if (!vertEdges.Contains(v)) { res.refusal = Refusal::NonManifoldCorner; return res; }

            // The one edge at this corner that runs INTO the body. Its geometry
            // is untouched by the move — neither of its faces is the one being
            // moved — so it is the fixed thing the new corner slides along.
            TopoDS_Edge leaving;
            int nLeaving = 0;
            for (TopTools_ListIteratorOfListOfShape it(vertEdges.FindFromKey(v));
                 it.More(); it.Next()) {
                const TopoDS_Edge e = TopoDS::Edge(it.Value());
                if (BRep_Tool::Degenerated(e)) continue;
                if (edgesOnF.count(e)) continue;
                bool dup = false;
                if (!leaving.IsNull() && leaving.IsSame(e)) dup = true;
                if (dup) continue;
                leaving = e;
                ++nLeaving;
            }
            // Exactly one is the ordinary case: three faces at a corner, two of
            // their shared edges on the moved face and one heading away. Zero or
            // several means the corner isn't one this can solve — a vertex
            // buried inside the face's own wire, or four faces meeting at a
            // point.
            if (nLeaving != 1) { res.refusal = Refusal::NonManifoldCorner; return res; }

            double lf, ll;
            Handle(Geom_Curve) lc = BRep_Tool::Curve(leaving, lf, ll);
            gp_Pnt np;
            if (!curveMeetsPlane(lc, lf, ll, movedF, BRep_Tool::Pnt(v), np)) {
                res.refusal = Refusal::Degenerate;
                return res;
            }
            movedVertices[v] = np;
        }

        // Fresh vertices, minted once so every face sharing a corner shares the
        // same TopoDS_Vertex and the shell sews without a tolerance hunt.
        ShapeMap<TopoDS_Vertex> newVertex;
        for (const auto& [oldV, pnt] : movedVertices)
            newVertex[oldV] = BRepBuilderAPI_MakeVertex(pnt);

        auto vertexFor = [&](const TopoDS_Vertex& v) {
            auto it = newVertex.find(v);
            return it == newVertex.end() ? v : it->second;
        };
        auto pointFor = [&](const TopoDS_Vertex& v) {
            auto it = movedVertices.find(v);
            return it == movedVertices.end() ? BRep_Tool::Pnt(v) : it->second;
        };
        auto isMoved = [&](const TopoDS_Vertex& v) { return movedVertices.count(v) > 0; };

        // ── Edges on the moved face: the new plane against each neighbour ────
        ShapeMap<TopoDS_Edge> newEdge;
        for (const TopoDS_Shape& es : edgesOnF) {
            const TopoDS_Edge e = TopoDS::Edge(es);
            TopoDS_Vertex v1, v2;
            TopExp::Vertices(e, v1, v2);
            if (!isMoved(v1) || !isMoved(v2)) { res.refusal = Refusal::NonManifoldCorner; return res; }

            // The neighbour across this edge.
            TopoDS_Face nb;
            if (edgeFaces.Contains(e)) {
                for (TopTools_ListIteratorOfListOfShape it(edgeFaces.FindFromKey(e));
                     it.More(); it.Next()) {
                    if (it.Value().IsSame(face)) continue;
                    nb = TopoDS::Face(it.Value());
                    break;
                }
            }
            if (nb.IsNull()) { res.refusal = Refusal::BuildFailed; return res; }

            gp_Pln nbPlane;
            const bool nbPlanar = planeOf(nb, nbPlane);
            const bool closed = v1.IsSame(v2);

            if (nbPlanar && !closed) {
                // Two planes meet in a line, and both of its ends are already
                // solved exactly. Building the segment straight between them is
                // the same answer as intersecting the surfaces, without asking
                // the kernel for an approximation of a line.
                BRepBuilderAPI_MakeEdge mk(vertexFor(v1), vertexFor(v2));
                if (!mk.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }
                newEdge[e] = mk.Edge();
                continue;
            }

            // Curved neighbour (or a closed edge, which no straight segment can
            // be): intersect the surfaces for real. A plane through a cylinder
            // gives a circle or an ellipse, through a cone a conic — the kernel
            // returns these analytically for quadrics, so the rebuilt edge is
            // exact rather than a sampled approximation.
            Handle(Geom_Surface) nbSurf = BRep_Tool::Surface(nb);
            Handle(Geom_Plane) movedSurf = new Geom_Plane(movedF);
            GeomAPI_IntSS iss(movedSurf, nbSurf, Precision::Confusion());
            if (!iss.IsDone() || iss.NbLines() == 0) {
                res.refusal = Refusal::NoIntersection;
                return res;
            }
            // A plane can cut a surface in several branches (both nappes of a
            // cone, two lines through a cylinder). The branch we want is the one
            // this edge is already on, judged from its midpoint.
            const gp_Pnt oldMid = midOf(e);
            Handle(Geom_Curve) best;
            double bestD = 1e300;
            for (int i = 1; i <= iss.NbLines(); ++i) {
                Handle(Geom_Curve) c = iss.Line(i);
                if (c.IsNull()) continue;
                GeomAPI_ProjectPointOnCurve proj(oldMid, c);
                if (proj.NbPoints() == 0) continue;
                if (proj.LowerDistance() < bestD) { bestD = proj.LowerDistance(); best = c; }
            }
            if (best.IsNull()) { res.refusal = Refusal::NoIntersection; return res; }
            // GeomAPI_IntSS hands back its analytic results wrapped in a
            // Geom_TrimmedCurve. The wrapper reports IsPeriodic() == false even
            // around a circle, so a closed rim looked un-closable; unwrap to the
            // basis curve, which knows it is periodic and can be walked a full
            // turn from any parameter.
            while (!best.IsNull() &&
                   best->IsKind(STANDARD_TYPE(Geom_TrimmedCurve)))
                best = Handle(Geom_TrimmedCurve)::DownCast(best)->BasisCurve();
            if (best.IsNull()) { res.refusal = Refusal::NoIntersection; return res; }

            const double tol = std::max(BRep_Tool::Tolerance(e), 1e-5);
            if (closed) {
                // A full loop — the lid of a cylinder, the rim of a bore. It has
                // ONE vertex, sitting where the neighbour's seam runs into it,
                // and that vertex is shared with the neighbour's seam edges. So
                // the loop has to START there: built from the bare curve instead,
                // BRepBuilderAPI_MakeEdge mints its own vertex at the curve's
                // parameter zero, which lands somewhere else entirely and leaves
                // the shell with two coincident-ish vertices it cannot sew.
                double pv = 0.0;
                if (!best->IsPeriodic() ||
                    !paramOn(best, pointFor(v1), tol * 100.0, pv)) {
                    res.refusal = Refusal::OffCurve;
                    return res;
                }
                BRepBuilderAPI_MakeEdge mk(best, vertexFor(v1), vertexFor(v1),
                                           pv, pv + best->Period());
                if (!mk.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }
                newEdge[e] = mk.Edge();
                continue;
            }

            double p1 = 0.0, p2 = 0.0;
            if (!paramOn(best, pointFor(v1), tol * 100.0, p1) ||
                !paramOn(best, pointFor(v2), tol * 100.0, p2)) {
                res.refusal = Refusal::OffCurve;
                return res;
            }
            // On a periodic curve two arcs join the same pair of points, and
            // taking the wrong one turns the face inside out. Pick the arc whose
            // own midpoint sits nearest where this edge already runs.
            if (best->IsPeriodic()) {
                const double period = best->Period();
                const gp_Pnt aMid = best->Value(0.5 * (p1 + p2));
                const double p2Alt = (p2 > p1) ? p2 - period : p2 + period;
                const gp_Pnt bMid = best->Value(0.5 * (p1 + p2Alt));
                if (bMid.Distance(oldMid) < aMid.Distance(oldMid)) p2 = p2Alt;
            }
            if (std::abs(p2 - p1) < Precision::PConfusion()) {
                res.refusal = Refusal::Degenerate;
                return res;
            }
            BRepBuilderAPI_MakeEdge mk(best, vertexFor(v1), vertexFor(v2), p1, p2);
            if (!mk.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }
            newEdge[e] = mk.Edge();
        }

        // ── Edges leaving the face: same curve, new end ──────────────────────
        for (int i = 1; i <= edgeFaces.Extent(); ++i) {
            const TopoDS_Edge e = TopoDS::Edge(edgeFaces.FindKey(i));
            if (BRep_Tool::Degenerated(e) || edgesOnF.count(e)) continue;
            TopoDS_Vertex v1, v2;
            TopExp::Vertices(e, v1, v2);
            const bool m1 = isMoved(v1), m2 = isMoved(v2);
            if (!m1 && !m2) continue;

            double f0, l0;
            Handle(Geom_Curve) c = BRep_Tool::Curve(e, f0, l0);
            if (c.IsNull()) { res.refusal = Refusal::BuildFailed; return res; }

            // Neither of this edge's faces moved, so its curve is untouched —
            // only the end that sat on the moved face slides along it. The new
            // corner came OFF this curve, so it lies on it by construction; the
            // check is here for the case where it came off a different edge at a
            // shared corner.
            double p1 = f0, p2 = l0;
            const double tol = std::max(BRep_Tool::Tolerance(e), 1e-6) * 100.0;
            if (m1 && !paramOn(c, pointFor(v1), tol, p1)) { res.refusal = Refusal::OffCurve; return res; }
            if (m2 && !paramOn(c, pointFor(v2), tol, p2)) { res.refusal = Refusal::OffCurve; return res; }
            if (std::abs(p2 - p1) < Precision::PConfusion()) {
                res.refusal = Refusal::Degenerate;
                return res;
            }
            BRepBuilderAPI_MakeEdge mk(c, vertexFor(v1), vertexFor(v2),
                                       std::min(p1, p2), std::max(p1, p2));
            if (!mk.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }
            newEdge[e] = mk.Edge();
        }

        // ── Faces: rebuild any that carry a rebuilt edge ─────────────────────
        BRepBuilderAPI_Sewing sew(Precision::Confusion() * 10.0);
        int rebuilt = 0;
        for (TopExp_Explorer fx(body, TopAbs_FACE); fx.More(); fx.Next()) {
            const TopoDS_Face f = TopoDS::Face(fx.Current());
            bool touched = false;
            for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next())
                if (newEdge.count(ex.Current())) { touched = true; break; }
            if (!touched) { sew.Add(f); continue; }

            // The moved face gets the new plane; a neighbour keeps its own
            // surface and only its boundary changes.
            Handle(Geom_Surface) surf = f.IsSame(face)
                ? Handle(Geom_Surface)(new Geom_Plane(movedF))
                : BRep_Tool::Surface(f);
            if (surf.IsNull()) { res.refusal = Refusal::BuildFailed; return res; }

            // Wire order comes from BRepTools_WireExplorer, which walks
            // connected — a plain TopExp_Explorer returns edges in map order and
            // MakeWire then refuses the ones that don't arrive adjacent.
            std::vector<TopoDS_Wire> wires;
            bool wireOk = true;
            for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More() && wireOk; wx.Next()) {
                BRepBuilderAPI_MakeWire mw;
                int edges = 0;
                for (BRepTools_WireExplorer we(TopoDS::Wire(wx.Current()), f);
                     we.More(); we.Next()) {
                    // A degenerate edge is a parametric artefact — a cone apex,
                    // a sphere pole. It has no 3d curve to rebuild and carrying
                    // it into a wire of new edges only makes MakeWire refuse.
                    if (BRep_Tool::Degenerated(we.Current())) continue;
                    auto it = newEdge.find(we.Current());
                    if (it == newEdge.end()) {
                        mw.Add(we.Current());
                    } else {
                        // Carry the orientation the wire uses. WireExplorer
                        // hands back the edge as this wire reads it; the
                        // replacement was built FORWARD, and adding it that way
                        // reverses half the boundary. A planar face survives
                        // that because ShapeFix re-winds it, but a cylinder's
                        // SEAM appears in the wire twice with opposite
                        // orientations, so dropping them makes the same edge
                        // twice over and the face fails validation.
                        mw.Add(TopoDS::Edge(it->second.Oriented(
                            we.Current().Orientation())));
                    }
                    ++edges;
                }
                if (edges == 0) continue;
                if (!mw.IsDone()) { wireOk = false; break; }
                wires.push_back(mw.Wire());
            }
            if (!wireOk || wires.empty()) { res.refusal = Refusal::BuildFailed; return res; }

            BRepBuilderAPI_MakeFace mf(surf, wires.front(), Precision::Confusion());
            if (!mf.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }
            for (size_t i = 1; i < wires.size(); ++i) {
                mf.Add(wires[i]);
                if (!mf.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }
            }

            // Two jobs for ShapeFix here, and the second is what curved
            // neighbours need: the rebuilt edges carry 3d curves only, and a
            // face on a cylinder is meaningless to the kernel until each of its
            // edges has a pcurve in that cylinder's (u,v). The first is the
            // long-standing one — wire winding decides which side of a face is
            // material and is never coordinated by hand here.
            ShapeFix_Face fix(mf.Face());
            fix.FixOrientationMode() = 1;
            fix.FixWireMode() = 1;
            // A cylinder or cone rebuilt from its wire has to be told where its
            // seam is, or the face is left open in (u,v) and fails validation.
            fix.FixMissingSeamMode() = 1;
            // Adding pcurves is the WIRE fixer's job, reached through the face
            // fixer's tool — it is on by default, but stating it keeps the
            // dependency visible: without it a rebuilt cylindrical face has
            // edges with 3d curves and no (u,v) representation, and the sew
            // downstream quietly drops it.
            if (!fix.FixWireTool().IsNull())
                fix.FixWireTool()->FixAddPCurveMode() = 1;
            fix.Perform();
            const TopoDS_Face nf = fix.Face();
            if (areaOf(nf) <= 1e-12) { res.refusal = Refusal::BuildFailed; return res; }
            sew.Add(nf);
            ++rebuilt;
        }
        if (rebuilt == 0) { res.refusal = Refusal::BuildFailed; return res; }

        // ── Close it back up ────────────────────────────────────────────────
        sew.Perform();
        TopoDS_Shape sewn = sew.SewedShape();
        if (sewn.IsNull() || sew.NbFreeEdges() > 0) {
            res.refusal = Refusal::BuildFailed;
            return res;
        }

        // A healing pass over the assembled shell. The per-face ShapeFix above
        // can only see one face at a time, and the things that survive it are
        // exactly the ones that are only wrong in context: a seam edge rebuilt
        // from its 3d curve has lost BOTH of its pcurves on the cylinder it
        // divides, and no single-face fix restores the pair. Cheap on a shell
        // this size, and the result is checked below either way.
        {
            ShapeFix_Shape heal(sewn);
            heal.SetPrecision(Precision::Confusion());
            heal.SetMaxTolerance(1e-3);
            heal.Perform();
            if (!heal.Shape().IsNull()) sewn = heal.Shape();
        }
        for (TopExp_Explorer ex(sewn, TopAbs_SHELL); ex.More(); ex.Next()) {
            TopoDS_Shell shell = TopoDS::Shell(ex.Current());
            BRepBuilderAPI_MakeSolid ms(shell);
            if (!ms.IsDone()) break;
            TopoDS_Shape solid = ms.Solid();
            // Sewing settles orientation among the faces but not which side is
            // inside; a negative volume is an inside-out solid, and reversing
            // the shell is the whole fix.
            if (volumeOf(solid) < 0.0) {
                shell.Reverse();
                BRepBuilderAPI_MakeSolid ms2(shell);
                if (!ms2.IsDone()) break;
                solid = ms2.Solid();
            }
            if (!BRepCheck_Analyzer(solid).IsValid()) {
                res.refusal = Refusal::Invalid;
                return res;
            }
            if (volumeOf(solid) <= 1e-9) { res.refusal = Refusal::Degenerate; return res; }
            res.shape = solid;
            return res;
        }
        res.refusal = Refusal::BuildFailed;
        return res;
    } catch (...) {
        res.refusal = Refusal::BuildFailed;
        return res;
    }
}

} // namespace materializr::tweak
