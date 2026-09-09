#include "GhostMesh.h"

#include <BRep_Tool.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <cmath>

namespace materializr {

namespace {

void push(std::vector<float>& out, const gp_Pnt& p, const gp_Vec& n)
{
    out.push_back(static_cast<float>(p.X()));
    out.push_back(static_cast<float>(p.Y()));
    out.push_back(static_cast<float>(p.Z()));
    out.push_back(static_cast<float>(n.X()));
    out.push_back(static_cast<float>(n.Y()));
    out.push_back(static_cast<float>(n.Z()));
}

// One triangle wound so its geometric normal agrees with `n`: the shader
// flips the normal of a back-facing triangle, so winding and normal must say
// the same thing.
void emitTri(std::vector<float>& out, const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c,
             const gp_Vec& n)
{
    const bool flip = gp_Vec(a, b).Crossed(gp_Vec(a, c)).Dot(n) < 0.0;
    push(out, a, n);
    push(out, flip ? c : b, n);
    push(out, flip ? b : c, n);
}

} // namespace

bool ghostPrismMesh(const TopoDS_Face& profile, const gp_Vec& sweep, std::vector<float>& out)
{
    if (profile.IsNull() || sweep.Magnitude() < 1e-9) return false;
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(profile, loc);
    if (tri.IsNull() || tri->NbTriangles() == 0) return false;

    // Every boundary edge must carry a polygon on this triangulation, or the
    // walls would have gaps; check before writing anything.
    std::vector<Handle(Poly_PolygonOnTriangulation)> polys;
    for (TopExp_Explorer ex(profile, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge& edge = TopoDS::Edge(ex.Current());
        if (BRep_Tool::Degenerated(edge)) continue;
        Handle(Poly_PolygonOnTriangulation) poly =
            BRep_Tool::PolygonOnTriangulation(edge, tri, loc);
        if (poly.IsNull() || poly->NbNodes() < 2) return false;
        polys.push_back(poly);
    }

    // Nodes in world coordinates, and their centroid for orienting the walls.
    const gp_Trsf trsf = loc.Transformation();
    const bool moved = !loc.IsIdentity();
    std::vector<gp_Pnt> nodes(static_cast<size_t>(tri->NbNodes()) + 1);
    gp_XYZ centroid(0, 0, 0);
    for (int i = 1; i <= tri->NbNodes(); ++i) {
        gp_Pnt p = tri->Node(i);
        if (moved) p.Transform(trsf);
        nodes[static_cast<size_t>(i)] = p;
        centroid += p.XYZ();
    }
    centroid /= static_cast<double>(tri->NbNodes());

    const gp_Vec up = sweep.Normalized();
    const gp_Vec down = -up;
    // Caps: the profile's triangles, the near cap facing against the sweep
    // and the far cap (translated by the sweep) facing along it.
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
        int n1, n2, n3;
        tri->Triangle(i).Get(n1, n2, n3);
        const gp_Pnt& a = nodes[static_cast<size_t>(n1)];
        const gp_Pnt& b = nodes[static_cast<size_t>(n2)];
        const gp_Pnt& c = nodes[static_cast<size_t>(n3)];
        emitTri(out, a, b, c, down);
        emitTri(out, a.Translated(sweep), b.Translated(sweep), c.Translated(sweep), up);
    }
    // Walls: one quad per polygon segment, facing away from the profile's
    // centroid. Exact for the outer boundary; a hole's walls face outward
    // from the profile's centre too, which for a translucent tint reads fine.
    for (const auto& poly : polys) {
        const TColStd_Array1OfInteger& idx = poly->Nodes();
        for (int k = idx.Lower(); k < idx.Upper(); ++k) {
            const gp_Pnt& a = nodes[static_cast<size_t>(idx(k))];
            const gp_Pnt& b = nodes[static_cast<size_t>(idx(k + 1))];
            gp_Vec n = gp_Vec(a, b).Crossed(up);
            if (n.Magnitude() < 1e-12) continue;
            n.Normalize();
            const gp_XYZ mid = (a.XYZ() + b.XYZ()) * 0.5;
            if (n.XYZ().Dot(mid - centroid) < 0.0) n.Reverse();
            const gp_Pnt a2 = a.Translated(sweep), b2 = b.Translated(sweep);
            emitTri(out, a, b, b2, n);
            emitTri(out, a, b2, a2, n);
        }
    }
    return true;
}

} // namespace materializr
