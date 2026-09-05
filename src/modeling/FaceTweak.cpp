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
#include <Geom_Curve.hxx>
#include <Geom_Plane.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Precision.hxx>
#include <ShapeFix_Face.hxx>
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
#include <vector>

namespace materializr::tweak {

namespace {

// Shape-keyed lookup. TopoDS handles have no operator<, and TopTools_DataMap
// hashes on IsSame, which is exactly the identity we want: the same vertex
// reached through two different faces must land on one entry.
struct SameKey {
    bool operator()(const TopoDS_Shape& a, const TopoDS_Shape& b) const {
        if (a.TShape().get() != b.TShape().get())
            return a.TShape().get() < b.TShape().get();
        return a.Location().HashCode() < b.Location().HashCode();
    }
};
template <class V> using ShapeMap = std::map<TopoDS_Shape, V, SameKey>;

bool planeOf(const TopoDS_Face& f, gp_Pln& out) {
    BRepAdaptor_Surface s(f, Standard_False);
    if (s.GetType() != GeomAbs_Plane) return false;
    out = s.Plane();
    return true;
}

// Where three planes meet. Cramer's rule on the three plane equations; the
// determinant is the scalar triple product of the normals, so it vanishes
// exactly when two of them are parallel or all three share a line — which is
// the Degenerate refusal, not a numerical accident to be smoothed over.
bool threePlanePoint(const gp_Pln& a, const gp_Pln& b, const gp_Pln& c,
                     gp_Pnt& out) {
    double A[3][3], d[3];
    const gp_Pln* p[3] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        double ca, cb, cc, cd;
        p[i]->Coefficients(ca, cb, cc, cd);
        A[i][0] = ca; A[i][1] = cb; A[i][2] = cc; d[i] = -cd;
    }
    const double det =
        A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
        A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
        A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    // The normals are unit vectors, so the determinant IS the sine of the
    // corner's solid angle — a scale-free measure. 1e-7 is about 0.006 degrees
    // off parallel, which no real corner is.
    if (std::abs(det) < 1e-7) return false;

    auto solve = [&](int col) {
        double M[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) M[i][j] = (j == col) ? d[i] : A[i][j];
        return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
               M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
               M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    };
    out = gp_Pnt(solve(0) / det, solve(1) / det, solve(2) / det);
    return true;
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

} // namespace

const char* refusalText(Refusal r) {
    switch (r) {
        case Refusal::None:              return "";
        case Refusal::FaceNotFound:      return "that face isn't part of this body";
        case Refusal::NotPlanar:         return "only flat faces can be tweaked yet";
        case Refusal::NeighbourNotPlanar: return "a curved face meets this one - not supported yet";
        case Refusal::NonManifoldCorner: return "a corner here isn't a plain three-face corner";
        case Refusal::NoChange:          return "sliding a flat face inside its own plane doesn't move anything - tilt it, or push it along its normal";
        case Refusal::Degenerate:        return "that move makes two faces parallel, so the corner has nowhere to go";
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

        // ── Find the face on the body, and the adjacency we'll rebuild from ──
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
        {
            const gp_Ax1 a = planeF.Axis(), b = movedF.Axis();
            if (a.Direction().IsParallel(b.Direction(), 1e-9) &&
                std::abs(movedF.Distance(planeF.Location())) < Precision::Confusion()) {
                res.refusal = Refusal::NoChange;
                return res;
            }
        }

        TopTools_IndexedDataMapOfShapeListOfShape vertFaces, edgeFaces;
        TopExp::MapShapesAndAncestors(body, TopAbs_VERTEX, TopAbs_FACE, vertFaces);
        TopExp::MapShapesAndAncestors(body, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

        // The surface each face sits on after the move: only the tweaked one
        // changes. Held as planes because every rebuilt face has to be planar
        // in this version anyway.
        auto surfaceOf = [&](const TopoDS_Face& f, gp_Pln& out) {
            if (f.IsSame(face)) { out = movedF; return true; }
            return planeOf(f, out);
        };

        // ── Corners: re-solve every vertex of the moved face ────────────────
        ShapeMap<gp_Pnt> movedVertices;
        for (TopExp_Explorer ex(face, TopAbs_VERTEX); ex.More(); ex.Next()) {
            const TopoDS_Vertex v = TopoDS::Vertex(ex.Current());
            if (movedVertices.count(v)) continue;
            if (!vertFaces.Contains(v)) { res.refusal = Refusal::NonManifoldCorner; return res; }

            // The distinct faces meeting here. MapShapesAndAncestors appends a
            // face once per occurrence, and a vertex is reached twice from the
            // same face whenever two of that face's edges meet at it — so the
            // raw list is longer than the corner is, and has to be deduplicated
            // before it can be counted.
            std::vector<TopoDS_Face> here;
            for (TopTools_ListIteratorOfListOfShape it(vertFaces.FindFromKey(v));
                 it.More(); it.Next()) {
                bool dup = false;
                for (const auto& f : here) if (f.IsSame(it.Value())) { dup = true; break; }
                if (!dup) here.push_back(TopoDS::Face(it.Value()));
            }
            if (here.size() != 3) { res.refusal = Refusal::NonManifoldCorner; return res; }

            gp_Pln pl[3];
            for (int i = 0; i < 3; ++i) {
                if (!surfaceOf(here[i], pl[i])) {
                    res.refusal = Refusal::NeighbourNotPlanar;
                    return res;
                }
            }
            gp_Pnt np;
            if (!threePlanePoint(pl[0], pl[1], pl[2], np)) {
                res.refusal = Refusal::Degenerate;
                return res;
            }
            movedVertices[v] = np;
        }
        if (movedVertices.empty()) { res.refusal = Refusal::NonManifoldCorner; return res; }

        // Fresh vertices, minted once so every face that shares a corner shares
        // the same TopoDS_Vertex and the shell sews without a tolerance hunt.
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
        auto isMovedVertex = [&](const TopoDS_Vertex& v) {
            return movedVertices.count(v) > 0;
        };

        // ── Edges: rebuild every one that has a corner on the move ──────────
        ShapeMap<TopoDS_Edge> newEdge;
        for (int i = 1; i <= edgeFaces.Extent(); ++i) {
            const TopoDS_Edge e = TopoDS::Edge(edgeFaces.FindKey(i));
            if (BRep_Tool::Degenerated(e)) continue;
            TopoDS_Vertex v1, v2;
            TopExp::Vertices(e, v1, v2);
            const bool m1 = isMovedVertex(v1), m2 = isMovedVertex(v2);
            if (!m1 && !m2) continue;

            bool onFace = false;
            for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next())
                if (ex.Current().IsSame(e)) { onFace = true; break; }

            if (onFace) {
                // Both ends moved and both faces are planar, so the edge is the
                // straight line between the new corners. No intersection needed:
                // the corners were already solved against this very pair of
                // planes, so the line through them IS plane∩plane.
                if (!m1 || !m2) { res.refusal = Refusal::NonManifoldCorner; return res; }
                BRepBuilderAPI_MakeEdge mk(vertexFor(v1), vertexFor(v2));
                if (!mk.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }
                newEdge[e] = mk.Edge();
            } else {
                // An edge running off the moved face into the rest of the body.
                // Neither of its faces moved, so its curve is untouched — only
                // the end that sat on the moved face has to slide along it.
                double f0, l0;
                Handle(Geom_Curve) c = BRep_Tool::Curve(e, f0, l0);
                if (c.IsNull()) { res.refusal = Refusal::BuildFailed; return res; }

                auto paramAt = [&](const TopoDS_Vertex& v, bool moved, double fallback,
                                   double& out) {
                    if (!moved) { out = fallback; return true; }
                    GeomAPI_ProjectPointOnCurve proj(pointFor(v), c);
                    if (proj.NbPoints() == 0) return false;
                    // The new corner has to actually LIE on this curve. If it
                    // doesn't, the move asked the body to do something this
                    // local rebuild can't express — say so rather than snapping
                    // the edge to the nearest point on it and calling it done.
                    if (proj.LowerDistance() > 1e-6) return false;
                    out = proj.LowerDistanceParameter();
                    return true;
                };
                double p1 = f0, p2 = l0;
                if (!paramAt(v1, m1, f0, p1) || !paramAt(v2, m2, l0, p2)) {
                    res.refusal = Refusal::OffCurve;
                    return res;
                }
                if (std::abs(p2 - p1) < Precision::PConfusion()) {
                    res.refusal = Refusal::Degenerate;
                    return res;
                }
                BRepBuilderAPI_MakeEdge mk(c, vertexFor(v1), vertexFor(v2),
                                           std::min(p1, p2), std::max(p1, p2));
                if (!mk.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }
                newEdge[e] = mk.Edge();
            }
        }

        // ── Faces: rebuild any that carry a rebuilt edge ────────────────────
        BRepBuilderAPI_Sewing sew(Precision::Confusion() * 10.0);
        int rebuilt = 0;
        for (TopExp_Explorer fx(body, TopAbs_FACE); fx.More(); fx.Next()) {
            const TopoDS_Face f = TopoDS::Face(fx.Current());
            bool touched = false;
            for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next())
                if (newEdge.count(ex.Current())) { touched = true; break; }
            if (!touched) { sew.Add(f); continue; }

            gp_Pln pl;
            if (!surfaceOf(f, pl)) { res.refusal = Refusal::NeighbourNotPlanar; return res; }

            // Rebuild every wire, substituting the edges that moved. Wire order
            // comes from BRepTools_WireExplorer, which walks connected — a plain
            // TopExp_Explorer returns them in map order and MakeWire then refuses
            // the ones that don't happen to arrive adjacent.
            std::vector<TopoDS_Wire> wires;
            bool wireOk = true;
            for (TopExp_Explorer wx(f, TopAbs_WIRE); wx.More() && wireOk; wx.Next()) {
                BRepBuilderAPI_MakeWire mw;
                for (BRepTools_WireExplorer we(TopoDS::Wire(wx.Current()), f);
                     we.More(); we.Next()) {
                    auto it = newEdge.find(we.Current());
                    mw.Add(it == newEdge.end() ? we.Current() : it->second);
                }
                if (!mw.IsDone()) { wireOk = false; break; }
                wires.push_back(mw.Wire());
            }
            if (!wireOk || wires.empty()) { res.refusal = Refusal::BuildFailed; return res; }

            BRepBuilderAPI_MakeFace mf(pl, wires.front());
            if (!mf.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }
            for (size_t i = 1; i < wires.size(); ++i) mf.Add(wires[i]);
            if (!mf.IsDone()) { res.refusal = Refusal::BuildFailed; return res; }

            // Wire winding decides which side of a planar face is material, and
            // it is never coordinated by hand here — the region builder's
            // long-standing lesson. Let ShapeFix settle it and check the face
            // came out with area.
            ShapeFix_Face fix(mf.Face());
            fix.FixOrientationMode() = 1;
            fix.FixWireMode() = 1;
            fix.Perform();
            const TopoDS_Face nf = fix.Face();
            GProp_GProps g;
            BRepGProp::SurfaceProperties(nf, g);
            if (g.Mass() <= 1e-12) { res.refusal = Refusal::BuildFailed; return res; }
            sew.Add(nf);
            ++rebuilt;
        }
        if (rebuilt == 0) { res.refusal = Refusal::BuildFailed; return res; }

        // ── Close it back up ────────────────────────────────────────────────
        sew.Perform();
        const TopoDS_Shape sewn = sew.SewedShape();
        if (sewn.IsNull() || sew.NbFreeEdges() > 0) {
            res.refusal = Refusal::BuildFailed;
            return res;
        }
        for (TopExp_Explorer ex(sewn, TopAbs_SHELL); ex.More(); ex.Next()) {
            TopoDS_Shell shell = TopoDS::Shell(ex.Current());
            BRepBuilderAPI_MakeSolid ms(shell);
            if (!ms.IsDone()) break;
            TopoDS_Shape solid = ms.Solid();
            // Sewing settles face orientation among themselves but not which
            // side is inside; a negative volume is an inside-out solid, and
            // reversing the shell is the whole fix.
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
