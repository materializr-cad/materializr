#include "Unfold.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Vertex.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GeomAbs_CurveType.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <BRepBuilderAPI_Copy.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_Triangle.hxx>
#include <TopLoc_Location.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>

namespace materializr {
namespace {

constexpr double kWeld = 1e-7;

struct Frame {
    gp_Pnt O;
    gp_Vec U, V;   // in-plane orthonormal basis
    gp_Vec n;      // outward normal (orientation-corrected) for fold angle
};

struct FaceData {
    Frame frame;
    std::vector<FlatLoop> localLoops;  // boundary in face-local 2D
    glm::dvec2 localCentroid{0, 0};
    bool planar = false;
    // Placement (face-local 2D → flat-pattern 2D): q' = R(θ)·(reflect?flipY:q) + t
    bool placed = false;
    double cosT = 1.0, sinT = 0.0;
    bool reflect = false;
    glm::dvec2 t{0, 0};
    glm::dvec2 placedCentroid{0, 0};
};

glm::dvec2 applyPlacement(const FaceData& f, glm::dvec2 q) {
    if (f.reflect) q.y = -q.y;
    return {f.cosT * q.x - f.sinT * q.y + f.t.x,
            f.sinT * q.x + f.cosT * q.y + f.t.y};
}

glm::dvec2 local2D(const FaceData& f, const gp_Pnt& p) {
    const gp_Vec d(f.frame.O, p);
    return {f.frame.U.Dot(d), f.frame.V.Dot(d)};
}

glm::dvec2 final2D(const FaceData& f, const gp_Pnt& p) {
    return applyPlacement(f, local2D(f, p));
}

// Sample a wire into ordered 3D points (closed; trailing duplicate trimmed).
std::vector<gp_Pnt> sampleWire(const TopoDS_Wire& w) {
    std::vector<gp_Pnt> pts;
    for (BRepTools_WireExplorer ex(w); ex.More(); ex.Next()) {
        const TopoDS_Edge e = ex.Current();
        BRepAdaptor_Curve c(e);
        GCPnts_TangentialDeflection d(c, 0.2, 0.4);
        const int n = d.NbPoints();
        const bool rev = (ex.Orientation() == TopAbs_REVERSED);
        for (int i = 1; i <= n; ++i) {
            const gp_Pnt p = d.Value(rev ? (n - i + 1) : i);
            if (!pts.empty() && pts.back().Distance(p) < kWeld) continue;
            pts.push_back(p);
        }
    }
    if (pts.size() > 1 && pts.front().Distance(pts.back()) < kWeld) pts.pop_back();
    return pts;
}

double cross2(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }

// Side of directed line a→b that point x is on (sign of the cross product).
double sideOf(glm::dvec2 a, glm::dvec2 b, glm::dvec2 x) { return cross2(b - a, x - a); }

bool segmentsCross(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c, glm::dvec2 d) {
    const double d1 = sideOf(c, d, a), d2 = sideOf(c, d, b);
    const double d3 = sideOf(a, b, c), d4 = sideOf(a, b, d);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

// Even-odd point-in-polygon test.
bool pointInPoly(const glm::dvec2& p, const std::vector<glm::dvec2>& poly) {
    bool in = false;
    const size_t n = poly.size();
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const glm::dvec2& a = poly[i];
        const glm::dvec2& b = poly[j];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x))
            in = !in;
    }
    return in;
}
// True AREA centroid (not the vertex average - a curved edge is densely
// tessellated and would drag a vertex-average toward it, landing the "centroid"
// outside the shape or inside a neighbour).
glm::dvec2 polyCentroid(const std::vector<glm::dvec2>& p) {
    const size_t n = p.size();
    if (n == 0) return {0, 0};
    double A = 0; glm::dvec2 c{0, 0};
    for (size_t i = 0; i < n; ++i) {
        const glm::dvec2& u = p[i];
        const glm::dvec2& v = p[(i + 1) % n];
        const double cr = u.x * v.y - v.x * u.y;
        A += cr; c += (u + v) * cr;
    }
    if (std::fabs(A) < 1e-12) { glm::dvec2 s{0, 0}; for (const auto& q : p) s += q; return s / double(n); }
    return c / (3.0 * A);   // A = 2·signed-area ⇒ divisor 3A = 6·area
}

// Polygon overlap: any boundary crossing, OR one polygon nested inside the other.
bool polysOverlap(const std::vector<glm::dvec2>& A, const std::vector<glm::dvec2>& B) {
    for (size_t i = 0; i < A.size(); ++i)
        for (size_t j = 0; j < B.size(); ++j)
            if (segmentsCross(A[i], A[(i + 1) % A.size()],
                              B[j], B[(j + 1) % B.size()]))
                return true;
    if (!A.empty() && pointInPoly(polyCentroid(A), B)) return true;
    if (!B.empty() && pointInPoly(polyCentroid(B), A)) return true;
    return false;
}

// Fraction of `poly`'s area that lies inside ANY of `others`, by grid sampling.
double coveredFraction(const std::vector<glm::dvec2>& poly,
                       const std::vector<const std::vector<glm::dvec2>*>& others) {
    if (poly.size() < 3 || others.empty()) return 0.0;
    glm::dvec2 lo{1e300, 1e300}, hi{-1e300, -1e300};
    for (const auto& p : poly) { lo.x=std::min(lo.x,p.x); lo.y=std::min(lo.y,p.y); hi.x=std::max(hi.x,p.x); hi.y=std::max(hi.y,p.y); }
    const int N = 14; const double cw = (hi.x-lo.x)/N, ch = (hi.y-lo.y)/N;
    if (cw <= 0 || ch <= 0) return 0.0;
    int inside = 0, covered = 0;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            const glm::dvec2 p{lo.x + (i+0.5)*cw, lo.y + (j+0.5)*ch};
            if (!pointInPoly(p, poly)) continue;
            ++inside;
            for (const auto* q : others) if (pointInPoly(p, *q)) { ++covered; break; }
        }
    return inside ? double(covered) / inside : 0.0;
}

// How badly a candidate face would BURY (or be buried by) the already-placed
// faces - the max, over both directions, of the covered-area fraction. ~0 for a
// clean hinge or a slight edge incursion (tolerated); large when a cap lands on
// top of the fan. Cumulative across all placed faces, so a circle covering many
// panels at once is caught.
double buryMetric(const std::vector<glm::dvec2>& cand,
                  const std::vector<const std::vector<glm::dvec2>*>& placed) {
    if (placed.empty()) return 0.0;
    double m = coveredFraction(cand, placed);          // candidate buried under the fan
    const std::vector<const std::vector<glm::dvec2>*> one{&cand};
    for (const auto* p : placed) m = std::max(m, coveredFraction(*p, one));  // fan buried under candidate
    return m;
}

} // namespace

FlatPattern unfoldPlanarFaces(const std::vector<TopoDS_Face>& faces) {
    FlatPattern out;
    const int nf = static_cast<int>(faces.size());
    if (nf == 0) { out.warning = "No faces to unfold."; return out; }

    std::vector<FaceData> fd(nf);
    int planarCount = 0;
    for (int i = 0; i < nf; ++i) {
        BRepAdaptor_Surface surf(faces[i]);
        if (surf.GetType() != GeomAbs_Plane) continue;  // v1: planar faces only
        const gp_Pln pln = surf.Plane();
        Frame fr;
        fr.O = pln.Location();
        fr.U = gp_Vec(pln.XAxis().Direction());
        gp_Vec z = gp_Vec(pln.Axis().Direction());
        fr.V = z.Crossed(fr.U);
        fr.n = (faces[i].Orientation() == TopAbs_REVERSED) ? -z : z;
        fd[i].frame = fr;
        fd[i].planar = true;
        ++planarCount;

        const TopoDS_Wire outer = BRepTools::OuterWire(faces[i]);
        glm::dvec2 csum{0, 0};
        int cn = 0;
        for (TopExp_Explorer wex(faces[i], TopAbs_WIRE); wex.More(); wex.Next()) {
            const TopoDS_Wire w = TopoDS::Wire(wex.Current());
            FlatLoop loop;
            loop.isHole = !w.IsSame(outer);
            for (const gp_Pnt& p : sampleWire(w)) {
                const glm::dvec2 q = local2D(fd[i], p);
                loop.pts.push_back(q);
                if (!loop.isHole) { csum += q; ++cn; }
            }
            if (loop.pts.size() >= 3) fd[i].localLoops.push_back(std::move(loop));
        }
        if (cn > 0) fd[i].localCentroid = csum / double(cn);
    }
    if (planarCount == 0) { out.warning = "No planar faces to unfold."; return out; }

    // Adjacency over shared edges. Build a compound so MapShapesAndAncestors sees
    // the faces as one shape and reports shared edges with both owners.
    BRep_Builder bb;
    TopoDS_Compound comp;
    bb.MakeCompound(comp);
    std::unordered_map<const void*, int> faceIndex;
    for (int i = 0; i < nf; ++i) {
        if (!fd[i].planar) continue;
        bb.Add(comp, faces[i]);
        faceIndex[faces[i].TShape().get()] = i;
    }
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(comp, TopAbs_EDGE, TopAbs_FACE, edgeFaces);

    struct Adj { int other; TopoDS_Edge edge; };
    std::vector<std::vector<Adj>> adj(nf);
    for (int e = 1; e <= edgeFaces.Extent(); ++e) {
        const TopoDS_Edge edge = TopoDS::Edge(edgeFaces.FindKey(e));
        const TopTools_ListOfShape& owners = edgeFaces.FindFromIndex(e);
        if (owners.Extent() != 2) continue;  // boundary (1) or non-manifold (>2)
        auto it = owners.begin();
        const int a = faceIndex.count(it->TShape().get()) ? faceIndex[it->TShape().get()] : -1;
        ++it;
        const int b = faceIndex.count(it->TShape().get()) ? faceIndex[it->TShape().get()] : -1;
        if (a < 0 || b < 0 || a == b) continue;
        adj[a].push_back({b, edge});
        adj[b].push_back({a, edge});
    }

    // BFS spanning tree from the first planar face; place each child by aligning
    // the shared edge to the parent's already-placed copy of it.
    int root = -1;
    for (int i = 0; i < nf; ++i) if (fd[i].planar) { root = i; break; }
    fd[root].placed = true;
    fd[root].placedCentroid = fd[root].localCentroid; // identity placement

    std::queue<int> bfs;
    bfs.push(root);
    while (!bfs.empty()) {
        const int parent = bfs.front();
        bfs.pop();
        for (const Adj& a : adj[parent]) {
            const int child = a.other;
            if (!fd[child].planar || fd[child].placed) continue;

            const gp_Pnt p0 = BRep_Tool::Pnt(TopExp::FirstVertex(a.edge, Standard_True));
            const gp_Pnt p1 = BRep_Tool::Pnt(TopExp::LastVertex(a.edge, Standard_True));
            const glm::dvec2 A0 = final2D(fd[parent], p0);
            const glm::dvec2 A1 = final2D(fd[parent], p1);
            const glm::dvec2 B0 = local2D(fd[child], p0);
            const glm::dvec2 B1 = local2D(fd[child], p1);
            if (glm::length(A1 - A0) < kWeld || glm::length(B1 - B0) < kWeld) continue;

            const double angA = std::atan2(A1.y - A0.y, A1.x - A0.x);
            const double parentSide = sideOf(A0, A1, fd[parent].placedCentroid);

            // Try both reflections; keep the one putting the child on the far side
            // of the hinge from its parent (so they don't overlap across the fold).
            bool bestReflect = false;
            double bestScore = -1e300;
            double bestCos = 1, bestSin = 0;
            glm::dvec2 bestT{0, 0}, bestCentroid{0, 0};
            for (int r = 0; r < 2; ++r) {
                const bool reflect = (r == 1);
                glm::dvec2 b0 = B0, b1 = B1, c = fd[child].localCentroid;
                if (reflect) { b0.y = -b0.y; b1.y = -b1.y; c.y = -c.y; }
                const double angB = std::atan2(b1.y - b0.y, b1.x - b0.x);
                const double th = angA - angB;
                const double ct = std::cos(th), st = std::sin(th);
                auto rot = [&](glm::dvec2 q) {
                    return glm::dvec2{ct * q.x - st * q.y, st * q.x + ct * q.y};
                };
                const glm::dvec2 t = A0 - rot(b0);
                const glm::dvec2 cc = rot(c) + t;
                const double childSide = sideOf(A0, A1, cc);
                // Score: child on opposite side of the hinge from the parent.
                const double score = (parentSide == 0.0) ? 0.0
                                     : (childSide * parentSide < 0 ? 1.0 : -1.0);
                if (score > bestScore) {
                    bestScore = score; bestReflect = reflect;
                    bestCos = ct; bestSin = st; bestT = t; bestCentroid = cc;
                }
            }
            fd[child].placed = true;
            fd[child].reflect = bestReflect;
            fd[child].cosT = bestCos;
            fd[child].sinT = bestSin;
            fd[child].t = bestT;
            fd[child].placedCentroid = bestCentroid;

            // Fold line + dihedral angle (between outward normals).
            FoldLine fl;
            fl.a = A0; fl.b = A1;
            const double dot = std::max(-1.0, std::min(1.0,
                fd[parent].frame.n.Dot(fd[child].frame.n)));
            fl.foldAngleDeg = std::acos(dot) * 180.0 / M_PI;
            out.folds.push_back(fl);

            bfs.push(child);
        }
    }

    // Emit placed faces.
    for (int i = 0; i < nf; ++i) {
        if (!fd[i].placed) continue;
        FlatFace ff;
        ff.sourceFaceIndex = i;
        for (const FlatLoop& loc : fd[i].localLoops) {
            FlatLoop placed;
            placed.isHole = loc.isHole;
            placed.pts.reserve(loc.pts.size());
            for (const glm::dvec2& q : loc.pts) placed.pts.push_back(applyPlacement(fd[i], q));
            ff.loops.push_back(std::move(placed));
        }
        out.faces.push_back(std::move(ff));
    }
    out.piecesPlaced = static_cast<int>(out.faces.size());

    // Overlap check on outer loops of non-touching faces (coarse).
    std::vector<const std::vector<glm::dvec2>*> outers;
    for (const FlatFace& f : out.faces)
        for (const FlatLoop& l : f.loops)
            if (!l.isHole) { outers.push_back(&l.pts); break; }
    for (size_t i = 0; i < outers.size() && !out.hasOverlap; ++i)
        for (size_t j = i + 1; j < outers.size(); ++j)
            if (polysOverlap(*outers[i], *outers[j])) { out.hasOverlap = true; break; }

    if (planarCount < static_cast<int>(faces.size()))
        out.warning = "Some non-planar faces were skipped.";
    if (out.piecesPlaced < planarCount)
        out.warning = "Some faces weren't connected to the main piece.";
    if (out.hasOverlap)
        out.warning = "The flattened net overlaps itself - it may need to be cut into pieces.";

    out.ok = out.piecesPlaced > 0;
    return out;
}

// ── Mesh-based unfold (handles curved/developable faces) ─────────────────────
namespace {

struct Tri {
    int v[3];        // global vertex indices
    gp_Vec normal;   // unit geometric normal
    // placement (triangle-local 2D → flat 2D): q' = R(θ)·(reflect?flipY:q)+t
    glm::dvec2 loc[3];     // local 2D of the 3 vertices
    glm::dvec2 centroid{0, 0};
    bool placed = false;
    double cosT = 1, sinT = 0;
    bool reflect = false;
    glm::dvec2 t{0, 0};
    glm::dvec2 placedCentroid{0, 0};
};

glm::dvec2 triApply(const Tri& tr, glm::dvec2 q) {
    if (tr.reflect) q.y = -q.y;
    return {tr.cosT * q.x - tr.sinT * q.y + tr.t.x,
            tr.sinT * q.x + tr.cosT * q.y + tr.t.y};
}

uint64_t vkey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

// Do two 2D triangles overlap in AREA? Separating-axis test over all 6 edge
// normals, with a tiny tolerance so triangles that merely share an edge (hinged
// neighbours) read as separated, not overlapping.
bool trisOverlap(const glm::dvec2 A[3], const glm::dvec2 B[3]) {
    auto sep = [&](glm::dvec2 ax, const glm::dvec2 P[3], const glm::dvec2 Q[3]) -> bool {
        double pmin = 1e300, pmax = -1e300, qmin = 1e300, qmax = -1e300;
        for (int i = 0; i < 3; ++i) {
            const double p = ax.x * P[i].x + ax.y * P[i].y;
            pmin = std::min(pmin, p); pmax = std::max(pmax, p);
            const double q = ax.x * Q[i].x + ax.y * Q[i].y;
            qmin = std::min(qmin, q); qmax = std::max(qmax, q);
        }
        const double eps = 1e-7 * (pmax - pmin + qmax - qmin + 1.0);
        return pmax <= qmin + eps || qmax <= pmin + eps; // separated on this axis
    };
    for (int i = 0; i < 3; ++i) {
        glm::dvec2 ea = A[(i + 1) % 3] - A[i];
        if (sep({-ea.y, ea.x}, A, B)) return false;
        glm::dvec2 eb = B[(i + 1) % 3] - B[i];
        if (sep({-eb.y, eb.x}, A, B)) return false;
    }
    return true; // no separating axis → overlapping
}

int sharedVertexCount(const int a[3], const int b[3]) {
    int n = 0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (a[i] == b[j]) ++n;
    return n;
}

} // namespace

FlatPattern unfoldFaces(const std::vector<TopoDS_Face>& faces,
                        double maxBevelDeg, double minFoldDeg) {
    FlatPattern out;
    if (faces.empty()) { out.warning = "No faces to unfold."; return out; }

    // Tessellation tolerances: a small linear deflection (scaled to model size)
    // plus the user's angular cap so each facet turns by ≤ maxBevelDeg.
    Bnd_Box box;
    for (const auto& f : faces) BRepBndLib::Add(f, box);
    double diag = 1.0;
    if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1; box.Get(x0, y0, z0, x1, y1, z1);
        diag = std::sqrt((x1-x0)*(x1-x0) + (y1-y0)*(y1-y0) + (z1-z0)*(z1-z0));
    }
    // Linear deflection is deliberately LOOSE so the angular cap ("max bevel per
    // score") is what actually controls facet density on curves - a tight linear
    // tolerance would force fine facets regardless of the slider.
    const double linDefl = std::max(1e-3, diag * 0.02);
    const double angRad = std::max(0.5, maxBevelDeg) * M_PI / 180.0;

    // Build one welded triangle mesh from all faces. Key by the quantised
    // coordinate TUPLE (not a hash of it) so distinct vertices never collide.
    std::vector<gp_Pnt> verts;
    std::map<std::array<int64_t, 3>, int> weld;
    const double q = 1.0 / std::max(1e-6, diag * 1e-6);
    auto weldVertex = [&](const gp_Pnt& p) -> int {
        const std::array<int64_t, 3> key{
            int64_t(std::llround(p.X() * q)),
            int64_t(std::llround(p.Y() * q)),
            int64_t(std::llround(p.Z() * q))};
        auto it = weld.find(key);
        if (it != weld.end()) return it->second;
        const int idx = int(verts.size());
        verts.push_back(p);
        weld.emplace(key, idx);
        return idx;
    };

    std::vector<Tri> tris;
    for (const TopoDS_Face& srcFace : faces) {
        // Mesh a COPY: the original face caches a (fine) triangulation from the
        // viewport, which BRepMesh would keep instead of re-meshing coarser - so
        // the bevel slider would have no effect. A copy starts with none.
        TopoDS_Face face = TopoDS::Face(BRepBuilderAPI_Copy(srcFace).Shape());
        BRepMesh_IncrementalMesh mesher(face, materializr::meshParams(linDefl, angRad, true));
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) continue;
        const gp_Trsf& trsf = loc.Transformation();
        const bool hasX = !loc.IsIdentity();
        const bool rev = (face.Orientation() == TopAbs_REVERSED);
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int a, b, c;
            tri->Triangle(i).Get(a, b, c);
            gp_Pnt p1 = tri->Node(a), p2 = tri->Node(b), p3 = tri->Node(c);
            if (hasX) { p1.Transform(trsf); p2.Transform(trsf); p3.Transform(trsf); }
            gp_Vec n = gp_Vec(p1, p2).Crossed(gp_Vec(p1, p3));
            if (n.Magnitude() < 1e-12) continue;
            n.Normalize();
            if (rev) n.Reverse();
            Tri t;
            t.v[0] = weldVertex(p1); t.v[1] = weldVertex(p2); t.v[2] = weldVertex(p3);
            if (t.v[0]==t.v[1] || t.v[1]==t.v[2] || t.v[0]==t.v[2]) continue;
            t.normal = n;
            // Triangle-local 2D frame: O=p1, U along p1→p2, V = n × U.
            gp_Vec U = gp_Vec(p1, p2); U.Normalize();
            gp_Vec V = n.Crossed(U);
            const gp_Pnt P[3] = {p1, p2, p3};
            glm::dvec2 csum{0, 0};
            for (int k = 0; k < 3; ++k) {
                gp_Vec d(p1, P[k]);
                t.loc[k] = {U.Dot(d), V.Dot(d)};
                csum += t.loc[k];
            }
            t.centroid = csum / 3.0;
            tris.push_back(t);
        }
    }
    const int nt = int(tris.size());
    if (nt == 0) { out.warning = "Nothing to tessellate."; return out; }

    // Edge → incident triangles.
    std::unordered_map<uint64_t, std::vector<int>> edgeTris;
    edgeTris.reserve(nt * 3);
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
            edgeTris[vkey(tris[t].v[k], tris[t].v[(k+1)%3])].push_back(t);

    auto triLocalOfVertex = [&](int t, int globalV) -> glm::dvec2 {
        for (int k = 0; k < 3; ++k) if (tris[t].v[k] == globalV) return tris[t].loc[k];
        return {0, 0};
    };
    auto finalOfVertex = [&](int t, int globalV) {
        return triApply(tris[t], triLocalOfVertex(t, globalV));
    };

    auto placedTriVerts = [&](int t, glm::dvec2 o[3]) {
        for (int k = 0; k < 3; ++k) o[k] = triApply(tris[t], tris[t].loc[k]);
    };
    // Place `child` flat against already-placed `parent` along their shared edge
    // (vi,vj), choosing the reflection that lands it on the far side of the
    // parent. Writes the placement into tris[child]; false if degenerate.
    auto computeChildPlacement = [&](int parent, int child, int vi, int vj) -> bool {
        const glm::dvec2 A0 = finalOfVertex(parent, vi), A1 = finalOfVertex(parent, vj);
        const glm::dvec2 B0 = triLocalOfVertex(child, vi), B1 = triLocalOfVertex(child, vj);
        if (glm::length(A1 - A0) < kWeld || glm::length(B1 - B0) < kWeld) return false;
        const double angA = std::atan2(A1.y - A0.y, A1.x - A0.x);
        const double pSide = sideOf(A0, A1, tris[parent].placedCentroid);
        double bestScore = -1e300; bool bestR = false;
        double bcos = 1, bsin = 0; glm::dvec2 bt{0,0}, bc{0,0};
        for (int r = 0; r < 2; ++r) {
            glm::dvec2 b0 = B0, b1 = B1, cc = tris[child].centroid;
            if (r) { b0.y=-b0.y; b1.y=-b1.y; cc.y=-cc.y; }
            const double angB = std::atan2(b1.y - b0.y, b1.x - b0.x);
            const double th = angA - angB, ct = std::cos(th), st = std::sin(th);
            auto rot = [&](glm::dvec2 q){ return glm::dvec2{ct*q.x-st*q.y, st*q.x+ct*q.y}; };
            const glm::dvec2 tt = A0 - rot(b0);
            const glm::dvec2 ccF = rot(cc) + tt;
            const double score = (pSide==0.0) ? 0.0 : (sideOf(A0,A1,ccF)*pSide<0 ? 1.0 : -1.0);
            if (score > bestScore) { bestScore=score; bestR=(r==1); bcos=ct; bsin=st; bt=tt; bc=ccF; }
        }
        tris[child].reflect = bestR; tris[child].cosT = bcos; tris[child].sinT = bsin;
        tris[child].t = bt; tris[child].placedCentroid = bc;
        return true;
    };

    // Greedy papercraft unfold: grow a flat piece one triangle at a time; if a
    // triangle would OVERLAP one already in its piece (which a doubly-curved
    // surface forces), leave it for another piece - the shared edge becomes a
    // cut/glue edge. A developable surface yields ONE piece; a compound-curved
    // one splits into several flat pieces. pieceOf[t] = which piece.
    std::vector<int> pieceOf(nt, -1);
    std::vector<std::vector<int>> pieceTris;
    for (int seed = 0; seed < nt; ++seed) {
        if (tris[seed].placed) continue;
        const int piece = int(pieceTris.size());
        pieceTris.emplace_back();
        tris[seed].placed = true; tris[seed].reflect = false;
        tris[seed].cosT = 1; tris[seed].sinT = 0; tris[seed].t = {0,0};
        tris[seed].placedCentroid = tris[seed].centroid;
        pieceOf[seed] = piece; pieceTris[piece].push_back(seed);
        std::queue<int> bfs; bfs.push(seed);
        while (!bfs.empty()) {
            const int parent = bfs.front(); bfs.pop();
            for (int k = 0; k < 3; ++k) {
                const int vi = tris[parent].v[k], vj = tris[parent].v[(k+1)%3];
                for (int child : edgeTris[vkey(vi, vj)]) {
                    if (child == parent || tris[child].placed) continue;
                    if (!computeChildPlacement(parent, child, vi, vj)) continue;
                    glm::dvec2 C[3]; placedTriVerts(child, C);
                    bool overlap = false;
                    for (int other : pieceTris[piece]) {
                        if (other == parent) continue;
                        if (sharedVertexCount(tris[child].v, tris[other].v) >= 2) continue; // hinged
                        glm::dvec2 O[3]; placedTriVerts(other, O);
                        if (trisOverlap(C, O)) { overlap = true; break; }
                    }
                    if (overlap) continue;  // start/join another piece instead
                    tris[child].placed = true;
                    pieceOf[child] = piece; pieceTris[piece].push_back(child);
                    bfs.push(child);
                }
            }
        }
    }
    const int nPieces = int(pieceTris.size());

    // Developability = integrated Gaussian curvature (total angle defect) over the
    // topological-interior vertices. ~0 = unrolls exactly; large = doubly-curved.
    std::vector<char> isTopoBoundaryV(verts.size(), 0);
    for (const auto& kv : edgeTris)
        if (kv.second.size() == 1) {
            isTopoBoundaryV[int(kv.first >> 32)] = 1;
            isTopoBoundaryV[int(kv.first & 0xffffffff)] = 1;
        }
    {
        std::vector<double> angleSum(verts.size(), 0.0);
        std::vector<char> seen(verts.size(), 0);
        for (const Tri& t : tris)
            for (int k = 0; k < 3; ++k) {
                const gp_Pnt& O = verts[t.v[k]];
                gp_Vec e1(O, verts[t.v[(k + 1) % 3]]);
                gp_Vec e2(O, verts[t.v[(k + 2) % 3]]);
                if (e1.Magnitude() < 1e-12 || e2.Magnitude() < 1e-12) continue;
                double c = std::max(-1.0, std::min(1.0, e1.Normalized().Dot(e2.Normalized())));
                angleSum[t.v[k]] += std::acos(c);
                seen[t.v[k]] = 1;
            }
        double defect = 0.0;
        for (size_t v = 0; v < verts.size(); ++v)
            if (seen[v] && !isTopoBoundaryV[v]) defect += std::fabs(2.0 * M_PI - angleSum[v]);
        out.curvatureDeg = defect * 180.0 / M_PI;
    }

    // Lay the pieces out in a row so they don't overlap each other on the sheet.
    std::vector<glm::dvec2> pieceOffset(nPieces, {0, 0});
    {
        double cursorX = 0.0;
        for (int p = 0; p < nPieces; ++p) {
            double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
            for (int t : pieceTris[p]) {
                glm::dvec2 V[3]; placedTriVerts(t, V);
                for (int k = 0; k < 3; ++k) {
                    minx = std::min(minx, V[k].x); miny = std::min(miny, V[k].y);
                    maxx = std::max(maxx, V[k].x); maxy = std::max(maxy, V[k].y);
                }
            }
            if (minx > maxx) continue;
            const double gap = std::max(2.0, 0.03 * (maxx - minx));
            pieceOffset[p] = {cursorX - minx, -miny};
            cursorX += (maxx - minx) + gap;
        }
    }
    auto finalOff = [&](int t, int v) { return finalOfVertex(t, v) + pieceOffset[pieceOf[t]]; };

    // Classify edges: fold (same piece, joins up) vs cut (boundary / seam / between
    // pieces). Cut segments carry the piece layout offset already.
    struct CutSeg { glm::dvec2 a, b; };
    std::vector<CutSeg> cuts;
    const double minFoldRad = minFoldDeg * M_PI / 180.0;
    for (const auto& kv : edgeTris) {
        const int va = int(kv.first >> 32), vb = int(kv.first & 0xffffffff);
        std::vector<int> placed;
        for (int t : kv.second) if (tris[t].placed) placed.push_back(t);
        if (placed.empty()) continue;
        if (placed.size() < 2) {
            cuts.push_back({finalOff(placed[0], va), finalOff(placed[0], vb)});
            continue;
        }
        const int t0 = placed[0], t1 = placed[1];
        const glm::dvec2 A0 = finalOff(t0, va), A1 = finalOff(t0, vb);
        const glm::dvec2 B0 = finalOff(t1, va), B1 = finalOff(t1, vb);
        const double eLen = glm::length(A1 - A0);
        const double tol = std::max(1e-4, 0.5 * eLen);
        const bool sameP = pieceOf[t0] == pieceOf[t1];
        const bool joined = sameP && glm::length(A0 - B0) < tol && glm::length(A1 - B1) < tol;
        if (joined) {
            const double dot = std::max(-1.0, std::min(1.0, tris[t0].normal.Dot(tris[t1].normal)));
            const double ang = std::acos(dot);
            if (ang > minFoldRad) {
                FoldLine fl; fl.a = A0; fl.b = A1; fl.foldAngleDeg = ang * 180.0 / M_PI;
                out.folds.push_back(fl);
            }
        } else {
            cuts.push_back({A0, A1});
            cuts.push_back({B0, B1});
        }
    }

    // Assemble cut segments into loops by welding 2D endpoints. Pieces are offset
    // apart, so a single global weld keeps each piece's outline distinct.
    {
        const double pq = 1.0 / std::max(1e-4, diag * 1e-5);
        std::map<std::pair<int64_t, int64_t>, int> pmap;
        std::vector<glm::dvec2> nodes;
        auto nodeOf = [&](glm::dvec2 p) -> int {
            std::pair<int64_t, int64_t> key{int64_t(std::llround(p.x * pq)),
                                            int64_t(std::llround(p.y * pq))};
            auto it = pmap.find(key);
            if (it != pmap.end()) return it->second;
            const int idx = int(nodes.size());
            nodes.push_back(p); pmap.emplace(key, idx);
            return idx;
        };
        std::vector<std::pair<int, int>> segs;
        for (const CutSeg& c : cuts) {
            const int na = nodeOf(c.a), nb = nodeOf(c.b);
            if (na != nb) segs.push_back({na, nb});
        }
        std::unordered_map<int, std::vector<int>> nodeSegs;
        for (int s = 0; s < int(segs.size()); ++s) {
            nodeSegs[segs[s].first].push_back(s);
            nodeSegs[segs[s].second].push_back(s);
        }
        std::vector<char> used(segs.size(), 0);
        FlatFace ff; ff.sourceFaceIndex = 0;
        for (int s = 0; s < int(segs.size()); ++s) {
            if (used[s]) continue;
            FlatLoop loop;
            int e = s, cur = segs[s].first;
            for (size_t guard = 0; guard <= segs.size(); ++guard) {
                used[e] = 1;
                const int nextN = (segs[e].first == cur) ? segs[e].second : segs[e].first;
                loop.pts.push_back(nodes[cur]);
                int ne = -1;
                for (int cand : nodeSegs[nextN]) if (!used[cand]) { ne = cand; break; }
                if (ne < 0) break;
                e = ne; cur = nextN;
            }
            if (loop.pts.size() >= 3) ff.loops.push_back(std::move(loop));
        }
        if (!ff.loops.empty()) out.faces.push_back(std::move(ff));
    }

    out.piecesPlaced = nPieces;
    out.ok = !out.faces.empty();
    if (nPieces > 1)
        out.warning = "Split into " + std::to_string(nPieces) +
                      " flat pieces - cut each and join along matching edges.";
    return out;
}

// ── Developable face-net (hinge whole faces, papercraft style) ───────────────
namespace {

std::array<int64_t, 3> qkey(const gp_Pnt& p, double q) {
    return {int64_t(std::llround(p.X() * q)),
            int64_t(std::llround(p.Y() * q)),
            int64_t(std::llround(p.Z() * q))};
}

// One face unrolled flat: its outline (outer + holes) in face-local 2D, interior
// facet-bend score lines, a 3D-vertex → 2D lookup (for hinge alignment), and a
// representative outward normal (for inter-face fold angles).
struct FaceNet {
    int faceIndex = -1;
    std::vector<FlatLoop> loops;       // unrolled outline, face-local 2D
    std::vector<FoldLine> innerFolds;  // facet bends within the face, local 2D
    glm::dvec2 centroid{0, 0};
    std::map<std::array<int64_t, 3>, glm::dvec2> vat;  // quantised 3D node → 2D
    gp_Vec normal{0, 0, 1};
    double curvatureDeg = 0.0;
    bool developable = true;
    bool ok = false;
};

// Unroll a single face's triangle mesh to 2D by hinge-BFS. Crucially we key
// vertices by the face's OWN triangulation node index (never by 3D coordinate),
// so a cone/cylinder seam stays open and the surface fans into one clean sector/
// strip instead of welding into a closed loop that self-overlaps. Returns the
// largest developable piece (a doubly-curved face splits; we keep the dominant
// part and flag it).
FaceNet unrollOneFace(const TopoDS_Face& srcFace, int faceIndex,
                      double qGlobal, double maxBevelDeg, double minFoldDeg) {
    FaceNet fn;
    fn.faceIndex = faceIndex;

    Bnd_Box box;
    BRepBndLib::Add(srcFace, box);
    double diag = 1.0;
    if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1; box.Get(x0, y0, z0, x1, y1, z1);
        diag = std::sqrt((x1-x0)*(x1-x0) + (y1-y0)*(y1-y0) + (z1-z0)*(z1-z0));
    }
    const double linDefl = std::max(1e-3, diag * 0.02);
    const double angRad = std::max(0.5, maxBevelDeg) * M_PI / 180.0;

    TopoDS_Face face = TopoDS::Face(BRepBuilderAPI_Copy(srcFace).Shape());
    BRepMesh_IncrementalMesh mesher(face, materializr::meshParams(linDefl, angRad, true));
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) return fn;
    const gp_Trsf& trsf = loc.Transformation();
    const bool hasX = !loc.IsIdentity();
    const bool rev = (face.Orientation() == TopAbs_REVERSED);

    const int nn = tri->NbNodes();
    std::vector<gp_Pnt> nodePos(nn + 1);
    for (int i = 1; i <= nn; ++i) {
        gp_Pnt p = tri->Node(i);
        if (hasX) p.Transform(trsf);
        nodePos[i] = p;
    }

    // Collapse degenerate poles/apexes: a true apex is a CLUSTER of many nodes at
    // one 3D point (the whole top of the UV grid collapses there), whereas a seam
    // is just TWO coincident nodes (one per side). Welding only clusters of ≥3
    // folds the apex to a single point - no spurious tip hole - while leaving the
    // 2-node seam open so the surface still fans into a sector instead of a tube.
    std::vector<int> rep(nn + 1);
    for (int i = 0; i <= nn; ++i) rep[i] = i;
    {
        std::map<std::array<int64_t, 3>, std::vector<int>> cl;
        for (int i = 1; i <= nn; ++i) cl[qkey(nodePos[i], qGlobal)].push_back(i);
        for (auto& kv : cl)
            if (kv.second.size() >= 3)
                for (int idx : kv.second) rep[idx] = kv.second.front();
    }

    std::vector<Tri> tris;
    gp_Vec accN(0, 0, 0);
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
        int a, b, c; tri->Triangle(i).Get(a, b, c);
        a = rep[a]; b = rep[b]; c = rep[c];
        const gp_Pnt p1 = nodePos[a], p2 = nodePos[b], p3 = nodePos[c];
        gp_Vec n = gp_Vec(p1, p2).Crossed(gp_Vec(p1, p3));
        if (n.Magnitude() < 1e-12) continue;
        const double area2 = n.Magnitude();
        n.Normalize(); if (rev) n.Reverse();
        accN += n * area2;
        Tri t;
        t.v[0] = a; t.v[1] = b; t.v[2] = c;   // node indices, NOT 3D-welded
        if (t.v[0]==t.v[1] || t.v[1]==t.v[2] || t.v[0]==t.v[2]) continue;
        t.normal = n;
        gp_Vec U = gp_Vec(p1, p2); U.Normalize();
        gp_Vec V = n.Crossed(U);
        const gp_Pnt P[3] = {p1, p2, p3};
        glm::dvec2 csum{0, 0};
        for (int k = 0; k < 3; ++k) { gp_Vec d(p1, P[k]); t.loc[k] = {U.Dot(d), V.Dot(d)}; csum += t.loc[k]; }
        t.centroid = csum / 3.0;
        tris.push_back(t);
    }
    const int nt = int(tris.size());
    if (nt == 0) return fn;
    if (accN.Magnitude() > 1e-12) { accN.Normalize(); fn.normal = accN; }

    std::unordered_map<uint64_t, std::vector<int>> et;
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) et[vkey(tris[t].v[k], tris[t].v[(k+1)%3])].push_back(t);

    auto triLocalOf = [&](int t, int gv) -> glm::dvec2 {
        for (int k = 0; k < 3; ++k) if (tris[t].v[k] == gv) return tris[t].loc[k];
        return {0, 0};
    };
    auto finalOf = [&](int t, int gv) { return triApply(tris[t], triLocalOf(t, gv)); };
    auto place = [&](int parent, int child, int vi, int vj) -> bool {
        const glm::dvec2 A0 = finalOf(parent, vi), A1 = finalOf(parent, vj);
        const glm::dvec2 B0 = triLocalOf(child, vi), B1 = triLocalOf(child, vj);
        if (glm::length(A1-A0) < kWeld || glm::length(B1-B0) < kWeld) return false;
        const double angA = std::atan2(A1.y-A0.y, A1.x-A0.x);
        const double pSide = sideOf(A0, A1, tris[parent].placedCentroid);
        double best = -1e300; bool bR = false; double bc = 1, bs = 0; glm::dvec2 bt{0,0}, bcc{0,0};
        for (int r = 0; r < 2; ++r) {
            glm::dvec2 b0 = B0, b1 = B1, cc = tris[child].centroid;
            if (r) { b0.y=-b0.y; b1.y=-b1.y; cc.y=-cc.y; }
            const double angB = std::atan2(b1.y-b0.y, b1.x-b0.x);
            const double th = angA-angB, ct = std::cos(th), st = std::sin(th);
            auto rot = [&](glm::dvec2 q){ return glm::dvec2{ct*q.x-st*q.y, st*q.x+ct*q.y}; };
            const glm::dvec2 tt = A0 - rot(b0);
            const glm::dvec2 ccF = rot(cc) + tt;
            const double sc = (pSide==0.0) ? 0.0 : (sideOf(A0,A1,ccF)*pSide<0 ? 1.0 : -1.0);
            if (sc > best) { best=sc; bR=(r==1); bc=ct; bs=st; bt=tt; bcc=ccF; }
        }
        tris[child].reflect=bR; tris[child].cosT=bc; tris[child].sinT=bs; tris[child].t=bt; tris[child].placedCentroid=bcc;
        return true;
    };

    // Spanning-tree hinge over the face's (connected) triangles - place EVERY
    // triangle, no overlap rejection. A developable face lays out flat and
    // watertight; orphaning a triangle here would only punch a hole. Splitting
    // into pieces happens at the FACE level (the net BFS), never inside a face.
    // The seed loop is defensive against a face whose mesh has disjoint islands.
    std::vector<int> pieceOf(nt, -1);
    std::vector<std::vector<int>> pieces;
    for (int seed = 0; seed < nt; ++seed) {
        if (tris[seed].placed) continue;
        const int pc = int(pieces.size()); pieces.emplace_back();
        tris[seed].placed=true; tris[seed].reflect=false; tris[seed].cosT=1; tris[seed].sinT=0;
        tris[seed].t={0,0}; tris[seed].placedCentroid=tris[seed].centroid;
        pieceOf[seed]=pc; pieces[pc].push_back(seed);
        std::queue<int> bfs; bfs.push(seed);
        while (!bfs.empty()) {
            const int parent = bfs.front(); bfs.pop();
            for (int k = 0; k < 3; ++k) {
                const int vi = tris[parent].v[k], vj = tris[parent].v[(k+1)%3];
                for (int child : et[vkey(vi, vj)]) {
                    if (child == parent || tris[child].placed) continue;
                    if (!place(parent, child, vi, vj)) continue;
                    tris[child].placed=true; pieceOf[child]=pc; pieces[pc].push_back(child); bfs.push(child);
                }
            }
        }
    }
    int big = 0;
    for (int p = 1; p < int(pieces.size()); ++p) if (pieces[p].size() > pieces[big].size()) big = p;
    if (pieces.empty() || pieces[big].empty()) return fn;
    fn.developable = (pieces.size() == 1);

    std::vector<glm::dvec2> node2D(nn + 1, {0, 0});
    std::vector<char> hasNode(nn + 1, 0);
    for (int t : pieces[big])
        for (int k = 0; k < 3; ++k) {
            const int gv = tris[t].v[k];
            if (!hasNode[gv]) { node2D[gv] = triApply(tris[t], tris[t].loc[k]); hasNode[gv] = 1; }
        }
    for (int i = 1; i <= nn; ++i) if (hasNode[i]) fn.vat[qkey(nodePos[i], qGlobal)] = node2D[i];

    // Edges of the big piece: 1 user → boundary (cut outline), 2 → interior bend.
    std::unordered_map<uint64_t, std::vector<int>> et2;
    for (int t : pieces[big])
        for (int k = 0; k < 3; ++k) et2[vkey(tris[t].v[k], tris[t].v[(k+1)%3])].push_back(t);

    std::vector<std::pair<int,int>> bedges;
    for (auto& kv : et2)
        if (kv.second.size() == 1) bedges.push_back({int(kv.first>>32), int(kv.first&0xffffffff)});
    std::unordered_map<int, std::vector<int>> ne;
    for (int s = 0; s < int(bedges.size()); ++s) { ne[bedges[s].first].push_back(s); ne[bedges[s].second].push_back(s); }
    std::vector<char> used(bedges.size(), 0);
    std::vector<std::vector<int>> loopSeqs;
    for (int s = 0; s < int(bedges.size()); ++s) {
        if (used[s]) continue;
        std::vector<int> seq; int e = s, cur = bedges[s].first;
        for (size_t g = 0; g <= bedges.size(); ++g) {
            used[e] = 1;
            const int nxt = (bedges[e].first == cur) ? bedges[e].second : bedges[e].first;
            seq.push_back(cur);
            int c2 = -1; for (int c : ne[nxt]) if (!used[c]) { c2 = c; break; }
            if (c2 < 0) break;
            e = c2; cur = nxt;
        }
        if (seq.size() >= 3) loopSeqs.push_back(std::move(seq));
    }
    if (loopSeqs.empty()) return fn;

    std::vector<FlatLoop> raw; std::vector<double> areas;
    for (auto& seq : loopSeqs) {
        FlatLoop l;
        for (int v : seq) l.pts.push_back(node2D[v]);
        double A = 0;
        for (size_t i = 0; i < l.pts.size(); ++i) {
            const glm::dvec2& p = l.pts[i];
            const glm::dvec2& q2 = l.pts[(i+1) % l.pts.size()];
            A += p.x*q2.y - q2.x*p.y;
        }
        areas.push_back(A); raw.push_back(std::move(l));
    }
    int outer = 0; double mxA = -1;
    for (size_t i = 0; i < areas.size(); ++i) if (std::fabs(areas[i]) > mxA) { mxA = std::fabs(areas[i]); outer = int(i); }
    for (size_t i = 0; i < raw.size(); ++i) { raw[i].isHole = (int(i) != outer); fn.loops.push_back(std::move(raw[i])); }
    { glm::dvec2 c{0,0}; int n = 0;
      for (const FlatLoop& l : fn.loops) { if (l.isHole) continue; for (auto& p : l.pts) { c += p; ++n; } break; }
      if (n) fn.centroid = c / double(n); }

    const double minFoldRad = minFoldDeg * M_PI / 180.0;
    for (auto& kv : et2)
        if (kv.second.size() == 2) {
            const int t0 = kv.second[0], t1 = kv.second[1];
            const double dot = std::max(-1.0, std::min(1.0, tris[t0].normal.Dot(tris[t1].normal)));
            const double ang = std::acos(dot);
            if (ang > minFoldRad) {
                const int va = int(kv.first>>32), vb = int(kv.first&0xffffffff);
                FoldLine fl; fl.a = node2D[va]; fl.b = node2D[vb]; fl.foldAngleDeg = ang * 180.0 / M_PI;
                fn.innerFolds.push_back(fl);
            }
        }

    { std::vector<char> bnd(nn + 1, 0);
      for (auto& kv : et2) if (kv.second.size() == 1) { bnd[int(kv.first>>32)]=1; bnd[int(kv.first&0xffffffff)]=1; }
      std::vector<double> as(nn + 1, 0.0); std::vector<char> sn(nn + 1, 0);
      for (int t : pieces[big])
          for (int k = 0; k < 3; ++k) {
              const gp_Pnt& O = nodePos[tris[t].v[k]];
              gp_Vec e1(O, nodePos[tris[t].v[(k+1)%3]]), e2(O, nodePos[tris[t].v[(k+2)%3]]);
              if (e1.Magnitude() < 1e-12 || e2.Magnitude() < 1e-12) continue;
              as[tris[t].v[k]] += std::acos(std::max(-1.0, std::min(1.0, e1.Normalized().Dot(e2.Normalized()))));
              sn[tris[t].v[k]] = 1;
          }
      double def = 0;
      for (int v = 1; v <= nn; ++v) if (sn[v] && !bnd[v]) def += std::fabs(2.0*M_PI - as[v]);
      fn.curvatureDeg = def * 180.0 / M_PI; }

    fn.ok = !fn.loops.empty();
    return fn;
}

} // namespace

FlatPattern unfoldDevelopableNet(const std::vector<TopoDS_Face>& faces,
                                 double maxBevelDeg, double minFoldDeg) {
    FlatPattern out;
    const int nf = int(faces.size());
    if (nf == 0) { out.warning = "No faces to unfold."; return out; }

    Bnd_Box box;
    for (const auto& f : faces) BRepBndLib::Add(f, box);
    double diag = 1.0;
    if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1; box.Get(x0, y0, z0, x1, y1, z1);
        diag = std::sqrt((x1-x0)*(x1-x0) + (y1-y0)*(y1-y0) + (z1-z0)*(z1-z0));
    }
    const double q = 1.0 / std::max(1e-6, diag * 1e-6);

    std::vector<FaceNet> fn(nf);
    int okCount = 0; double totalCurv = 0; int nonDev = 0;
    for (int i = 0; i < nf; ++i) {
        fn[i] = unrollOneFace(faces[i], i, q, maxBevelDeg, minFoldDeg);
        if (fn[i].ok) { ++okCount; totalCurv += fn[i].curvatureDeg; if (!fn[i].developable) ++nonDev; }
    }
    if (okCount == 0) { out.warning = "Nothing to unfold."; return out; }
    out.curvatureDeg = totalCurv;

    // Face adjacency over shared edges (compound so MapShapesAndAncestors reports
    // both owners of each shared edge).
    BRep_Builder bb; TopoDS_Compound comp; bb.MakeCompound(comp);
    std::unordered_map<const void*, int> faceIndex;
    for (int i = 0; i < nf; ++i) {
        if (!fn[i].ok) continue;
        bb.Add(comp, faces[i]); faceIndex[faces[i].TShape().get()] = i;
    }
    TopTools_IndexedDataMapOfShapeListOfShape ef;
    TopExp::MapShapesAndAncestors(comp, TopAbs_EDGE, TopAbs_FACE, ef);
    struct Adj { int other; TopoDS_Edge edge; };
    std::vector<std::vector<Adj>> adj(nf);
    for (int e = 1; e <= ef.Extent(); ++e) {
        const TopoDS_Edge edge = TopoDS::Edge(ef.FindKey(e));
        const TopTools_ListOfShape& ow = ef.FindFromIndex(e);
        if (ow.Extent() != 2) continue;
        auto it = ow.begin();
        const int a = faceIndex.count(it->TShape().get()) ? faceIndex[it->TShape().get()] : -1;
        ++it;
        const int b = faceIndex.count(it->TShape().get()) ? faceIndex[it->TShape().get()] : -1;
        if (a < 0 || b < 0 || a == b) continue;
        adj[a].push_back({b, edge}); adj[b].push_back({a, edge});
    }

    struct Place { bool placed=false; double cosT=1, sinT=0; bool reflect=false; glm::dvec2 t{0,0}; glm::dvec2 centroid{0,0}; int piece=-1; };
    struct PFold { int piece; FoldLine fl; };
    struct NetTrial {
        std::vector<Place> pl;
        std::vector<std::vector<int>> pieceFaces;
        std::vector<PFold> folds;
        double cost = 1e300;   // Σ per-piece bounding-box area - a material proxy.
    };

    // Per-face flat area, used to seed big faces first and (later) score nothing.
    auto loopAreaOf = [](const std::vector<glm::dvec2>& p) {
        double a = 0;
        for (size_t i = 0; i < p.size(); ++i) { const glm::dvec2& u = p[i]; const glm::dvec2& v = p[(i+1)%p.size()]; a += u.x*v.y - v.x*u.y; }
        return std::fabs(0.5 * a);
    };
    std::vector<double> faceArea(nf, 0.0);
    for (int i = 0; i < nf; ++i)
        for (const FlatLoop& l : fn[i].loops) faceArea[i] += (l.isHole ? -1.0 : 1.0) * loopAreaOf(l.pts);

    // Assemble one net for a given seed ORDER (first = preferred root). The greedy
    // hinge is deterministic per order, so trying several roots and keeping the
    // most compact result is what "vary automatically to the smallest flat area"
    // means; a large central flat face as the root yields the natural "panels fan
    // off the square" net.
    auto assemble = [&](const std::vector<int>& seedOrder) -> NetTrial {
        NetTrial tr; tr.pl.assign(nf, Place{});
        std::vector<std::vector<glm::dvec2>> outerCache(nf);
        auto applyP = [&](int i, glm::dvec2 v) {
            if (tr.pl[i].reflect) v.y = -v.y;
            return glm::dvec2{tr.pl[i].cosT*v.x - tr.pl[i].sinT*v.y + tr.pl[i].t.x,
                              tr.pl[i].sinT*v.x + tr.pl[i].cosT*v.y + tr.pl[i].t.y};
        };
        auto placedOuter = [&](int i) {
            std::vector<glm::dvec2> o;
            for (const FlatLoop& l : fn[i].loops)
                if (!l.isHole) { for (auto& p : l.pts) o.push_back(applyP(i, p)); break; }
            return o;
        };
        auto look = [&](int i, const gp_Pnt& p, glm::dvec2& o) {
            auto it = fn[i].vat.find(qkey(p, q));
            if (it == fn[i].vat.end()) return false;
            o = it->second; return true;
        };
        for (int seedF : seedOrder) {
            if (!fn[seedF].ok || tr.pl[seedF].placed) continue;
            const int piece = int(tr.pieceFaces.size()); tr.pieceFaces.emplace_back();
            tr.pl[seedF].placed=true; tr.pl[seedF].centroid=fn[seedF].centroid; tr.pl[seedF].piece=piece;
            outerCache[seedF] = placedOuter(seedF); tr.pieceFaces[piece].push_back(seedF);
            std::queue<int> bfs; bfs.push(seedF);
            while (!bfs.empty()) {
                const int parent = bfs.front(); bfs.pop();
                for (const Adj& a : adj[parent]) {
                    const int child = a.other;
                    if (!fn[child].ok || tr.pl[child].placed) continue;
                    const gp_Pnt p0 = BRep_Tool::Pnt(TopExp::FirstVertex(a.edge, Standard_True));
                    const gp_Pnt p1 = BRep_Tool::Pnt(TopExp::LastVertex(a.edge, Standard_True));
                    glm::dvec2 Pp0, Pp1, Bp0, Bp1;
                    if (!look(parent, p0, Pp0) || !look(parent, p1, Pp1) ||
                        !look(child, p0, Bp0) || !look(child, p1, Bp1)) continue;
                    const glm::dvec2 A0 = applyP(parent, Pp0), A1 = applyP(parent, Pp1);
                    if (glm::length(A1-A0) < kWeld || glm::length(Bp1-Bp0) < kWeld) continue;
                    const double angA = std::atan2(A1.y-A0.y, A1.x-A0.x);
                    const double pSide = sideOf(A0, A1, tr.pl[parent].centroid);
                    // Evaluate both fold directions; pick the one that buries the
                    // least (ties → fold away from the parent). A slight overlap is
                    // tolerated so the doubly-curved panels stay ONE connected net;
                    // only a substantial burial - a cap landing on top of the fan -
                    // splits off to a new piece.
                    // A curved shared edge (a round cap's arc against a panel) can't
                    // fold flat without a curve mismatch, so it tolerates far less
                    // burial than a clean straight-edge fold - a cap that only meets
                    // the net along arcs thus splits off as its own piece instead of
                    // overlapping, while straight-edge panel folds stay connected.
                    const bool straightHinge =
                        BRepAdaptor_Curve(a.edge).GetType() == GeomAbs_Line;
                    const double kBuryTol = straightHinge ? 0.30 : 0.04;
                    double best = -1e300; int bR = 0; double bc = 1, bs = 0;
                    glm::dvec2 bt{0,0}, bcc{0,0}; std::vector<glm::dvec2> bco; double bestBury = 1.0;
                    for (int r = 0; r < 2; ++r) {
                        glm::dvec2 b0 = Bp0, b1 = Bp1, cc = fn[child].centroid;
                        if (r) { b0.y=-b0.y; b1.y=-b1.y; cc.y=-cc.y; }
                        const double angB = std::atan2(b1.y-b0.y, b1.x-b0.x);
                        const double th = angA-angB, ct = std::cos(th), st = std::sin(th);
                        auto rot = [&](glm::dvec2 v){ return glm::dvec2{ct*v.x-st*v.y, st*v.x+ct*v.y}; };
                        const glm::dvec2 tt = A0 - rot(b0);
                        const glm::dvec2 ccF = rot(cc) + tt;
                        tr.pl[child].reflect=(r==1); tr.pl[child].cosT=ct; tr.pl[child].sinT=st;
                        tr.pl[child].t=tt; tr.pl[child].centroid=ccF;
                        std::vector<glm::dvec2> co = placedOuter(child);
                        std::vector<const std::vector<glm::dvec2>*> placed;
                        for (int of : tr.pieceFaces[piece]) placed.push_back(&outerCache[of]);
                        const double bury = buryMetric(co, placed);
                        const double side = (pSide==0.0) ? 0.0 : (sideOf(A0,A1,ccF)*pSide<0 ? 1.0 : -1.0);
                        const double score = -bury * 1000.0 + side;   // least burial wins; outward breaks ties
                        if (score > best) { best=score; bR=r; bc=ct; bs=st; bt=tt; bcc=ccF; bco=std::move(co); bestBury=bury; }
                    }
                    // A flat cap can't fold flush to a CURVED rim, so it always dips
                    // slightly into the panel. Rather than tolerate that sliver, slide
                    // the cap straight out (perpendicular to the hinge, away from the
                    // parent) until it sits just clear of the edge - touching, not
                    // overlapping. Straight-edge folds are real folds and stay put.
                    if (!straightHinge && bestBury > 0.005) {
                        glm::dvec2 e = A1 - A0; const double el = glm::length(e);
                        if (el > kWeld) {
                            e /= el; glm::dvec2 nrm{-e.y, e.x};
                            if (sideOf(A0, A1, tr.pl[parent].centroid) > 0) nrm = -nrm;  // away from parent
                            const double step = std::max(0.3, 0.01 * diag);
                            std::vector<const std::vector<glm::dvec2>*> placed;
                            for (int of : tr.pieceFaces[piece]) placed.push_back(&outerCache[of]);
                            for (int it = 0; it < 80 && bestBury > 0.003; ++it) {
                                bt += step * nrm; bcc += step * nrm;
                                for (auto& p : bco) p += step * nrm;
                                bestBury = buryMetric(bco, placed);
                            }
                        }
                    }
                    if (bestBury > kBuryTol) { tr.pl[child] = Place{}; continue; }  // would bury → new piece
                    tr.pl[child].placed=true; tr.pl[child].reflect=(bR==1); tr.pl[child].cosT=bc; tr.pl[child].sinT=bs;
                    tr.pl[child].t=bt; tr.pl[child].centroid=bcc; tr.pl[child].piece=piece;
                    outerCache[child] = std::move(bco); tr.pieceFaces[piece].push_back(child); bfs.push(child);
                    const double dot = std::max(-1.0, std::min(1.0, fn[parent].normal.Dot(fn[child].normal)));
                    const double ang = std::acos(dot);
                    if (ang > minFoldDeg * M_PI / 180.0) {
                        FoldLine fl; fl.a = A0; fl.b = A1; fl.foldAngleDeg = ang * 180.0 / M_PI;
                        tr.folds.push_back({piece, fl});
                    }
                }
            }
        }
        // Cost = Σ per-piece bounding-box area (counts wasted corners → favours
        // tight layouts / fewer pieces). Arrangement-independent.
        double cost = 0;
        for (const auto& pieceF : tr.pieceFaces) {
            double minx=1e300, miny=1e300, maxx=-1e300, maxy=-1e300;
            for (int f : pieceF)
                for (const FlatLoop& l : fn[f].loops)
                    for (auto& pp : l.pts) {
                        const glm::dvec2 w = applyP(f, pp);
                        minx=std::min(minx,w.x); miny=std::min(miny,w.y); maxx=std::max(maxx,w.x); maxy=std::max(maxy,w.y);
                    }
            if (maxx > minx && maxy > miny) cost += (maxx-minx) * (maxy-miny);
        }
        tr.cost = cost;
        return tr;
    };

    // Candidate roots: try each ok face as the root (capped by face count for
    // responsiveness), seeding the rest big-face-first. Keep the net with the
    // fewest pieces, then the smallest total bounding area.
    std::vector<int> byArea;
    for (int i = 0; i < nf; ++i) if (fn[i].ok) byArea.push_back(i);
    std::sort(byArea.begin(), byArea.end(), [&](int a, int b){ return faceArea[a] > faceArea[b]; });
    const int rootCap = (nf <= 12) ? 12 : (nf <= 40 ? 6 : 3);
    const int maxRoots = std::min(static_cast<int>(byArea.size()), rootCap);

    NetTrial best; bool haveBest = false;
    for (int ri = 0; ri < maxRoots; ++ri) {
        std::vector<int> order; order.reserve(byArea.size());
        order.push_back(byArea[ri]);
        for (int f : byArea) if (f != byArea[ri]) order.push_back(f);
        NetTrial tr = assemble(order);
        if (!haveBest) { best = std::move(tr); haveBest = true; continue; }
        const int tp = int(tr.pieceFaces.size()), bp = int(best.pieceFaces.size());
        if (tp < bp || (tp == bp && tr.cost < best.cost - 1e-6)) best = std::move(tr);
    }
    if (!haveBest) { out.warning = "Nothing to unfold."; return out; }

    // Expose the winning trial under the names the emit code below expects.
    auto& pl = best.pl;
    auto& pieceFaces = best.pieceFaces;
    auto& tmpFolds = best.folds;
    const int nPieces = int(pieceFaces.size());
    auto applyP = [&](int i, glm::dvec2 v) {
        if (pl[i].reflect) v.y = -v.y;
        return glm::dvec2{pl[i].cosT*v.x - pl[i].sinT*v.y + pl[i].t.x,
                          pl[i].sinT*v.x + pl[i].cosT*v.y + pl[i].t.y};
    };

    // Lay pieces out in a row.
    std::vector<glm::dvec2> pieceOffset(nPieces, {0, 0});
    { double cursorX = 0;
      for (int p = 0; p < nPieces; ++p) {
          double minx=1e300, miny=1e300, maxx=-1e300, maxy=-1e300;
          for (int f : pieceFaces[p])
              for (const FlatLoop& l : fn[f].loops)
                  for (auto& pp : l.pts) {
                      const glm::dvec2 w = applyP(f, pp);
                      minx=std::min(minx,w.x); miny=std::min(miny,w.y); maxx=std::max(maxx,w.x); maxy=std::max(maxy,w.y);
                  }
          if (minx > maxx) continue;
          const double gap = std::max(3.0, 0.03 * (maxx - minx));
          pieceOffset[p] = {cursorX - minx, -miny};
          cursorX += (maxx - minx) + gap;
      } }

    for (int i = 0; i < nf; ++i) {
        if (!pl[i].placed) continue;
        const glm::dvec2 off = pieceOffset[pl[i].piece];
        FlatFace ff; ff.sourceFaceIndex = fn[i].faceIndex;
        for (const FlatLoop& l : fn[i].loops) {
            FlatLoop pf; pf.isHole = l.isHole; pf.pts.reserve(l.pts.size());
            for (auto& p : l.pts) pf.pts.push_back(applyP(i, p) + off);
            ff.loops.push_back(std::move(pf));
        }
        out.faces.push_back(std::move(ff));
        for (const FoldLine& f : fn[i].innerFolds) {
            FoldLine fl; fl.a = applyP(i, f.a) + off; fl.b = applyP(i, f.b) + off; fl.foldAngleDeg = f.foldAngleDeg;
            out.folds.push_back(fl);
        }
    }
    for (const PFold& pf : tmpFolds) {
        FoldLine fl = pf.fl; const glm::dvec2 off = pieceOffset[pf.piece];
        fl.a += off; fl.b += off; out.folds.push_back(fl);
    }

    out.piecesPlaced = nPieces;
    out.ok = !out.faces.empty();
    if (nPieces > 1)
        out.warning = "Split into " + std::to_string(nPieces) +
                      " pieces - cut each and join along matching edges.";
    else if (nonDev > 0)
        out.warning = "Some faces are doubly-curved - score/fold lines approximate the curvature.";
    return out;
}

// ── Conformal (LSCM) flatten ─────────────────────────────────────────────────
namespace {

// Tessellate the faces into one welded triangle mesh (3D verts + triangles with
// in-plane local 2D coords + outward normal). Shared by the LSCM path.
void buildTriMesh(const std::vector<TopoDS_Face>& faces, double maxBevelDeg,
                  std::vector<gp_Pnt>& verts, std::vector<Tri>& tris) {
    Bnd_Box box;
    for (const auto& f : faces) BRepBndLib::Add(f, box);
    double diag = 1.0;
    if (!box.IsVoid()) {
        double x0,y0,z0,x1,y1,z1; box.Get(x0,y0,z0,x1,y1,z1);
        diag = std::sqrt((x1-x0)*(x1-x0)+(y1-y0)*(y1-y0)+(z1-z0)*(z1-z0));
    }
    const double linDefl = std::max(1e-3, diag * 0.02);
    const double angRad = std::max(0.5, maxBevelDeg) * M_PI / 180.0;
    const double q = 1.0 / std::max(1e-6, diag * 1e-6);
    std::map<std::array<int64_t, 3>, int> weld;
    auto weldVertex = [&](const gp_Pnt& p) -> int {
        std::array<int64_t,3> key{int64_t(std::llround(p.X()*q)),
                                  int64_t(std::llround(p.Y()*q)),
                                  int64_t(std::llround(p.Z()*q))};
        auto it = weld.find(key);
        if (it != weld.end()) return it->second;
        const int idx = int(verts.size());
        verts.push_back(p); weld.emplace(key, idx);
        return idx;
    };
    for (const TopoDS_Face& srcFace : faces) {
        TopoDS_Face face = TopoDS::Face(BRepBuilderAPI_Copy(srcFace).Shape());
        BRepMesh_IncrementalMesh mesher(face, materializr::meshParams(linDefl, angRad, true));
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) continue;
        const gp_Trsf& trsf = loc.Transformation();
        const bool hasX = !loc.IsIdentity();
        const bool rev = (face.Orientation() == TopAbs_REVERSED);
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int a, b, c; tri->Triangle(i).Get(a, b, c);
            gp_Pnt p1 = tri->Node(a), p2 = tri->Node(b), p3 = tri->Node(c);
            if (hasX) { p1.Transform(trsf); p2.Transform(trsf); p3.Transform(trsf); }
            gp_Vec n = gp_Vec(p1, p2).Crossed(gp_Vec(p1, p3));
            if (n.Magnitude() < 1e-12) continue;
            n.Normalize(); if (rev) n.Reverse();
            Tri t;
            t.v[0]=weldVertex(p1); t.v[1]=weldVertex(p2); t.v[2]=weldVertex(p3);
            if (t.v[0]==t.v[1]||t.v[1]==t.v[2]||t.v[0]==t.v[2]) continue;
            t.normal = n;
            gp_Vec U = gp_Vec(p1,p2); U.Normalize();
            gp_Vec V = n.Crossed(U);
            const gp_Pnt P[3] = {p1,p2,p3};
            for (int k=0;k<3;++k){ gp_Vec d(p1,P[k]); t.loc[k]={U.Dot(d), V.Dot(d)}; }
            tris.push_back(t);
        }
    }
}

// Cut a welded triangle mesh along seam paths until every connected component is
// a DISK (exactly one boundary loop) - the topology LSCM needs. A closed
// component (a sphere → 0 boundary loops) gets one slit between its two
// farthest-apart vertices; a component with extra boundary loops (a tube/funnel
// → 2 loops) gets each extra loop joined to the first by a shortest cut. Cutting
// = duplicating the vertices along the path (splitting each one's triangle fan at
// the cut edges) so the two banks of the seam get independent UVs and the surface
// opens flat. Edits tris[].v[] in place (loc[]/normal are per-triangle, untouched)
// and appends the duplicate points to `verts`. This is the auto-seam-cut that lets
// the conformal unwrap handle closed/multiply-connected surfaces.
void cutMeshToDisks(std::vector<gp_Pnt>& verts, std::vector<Tri>& tris) {
    const int nt = int(tris.size());
    if (nt == 0) return;
    const int nv = int(verts.size());

    std::unordered_map<uint64_t, std::vector<int>> et;
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) et[vkey(tris[t].v[k], tris[t].v[(k+1)%3])].push_back(t);

    // Connected components (union-find over triangles by shared edge).
    std::vector<int> uf(nt); for (int i = 0; i < nt; ++i) uf[i] = i;
    std::function<int(int)> find = [&](int x){ while (uf[x]!=x){ uf[x]=uf[uf[x]]; x=uf[x]; } return x; };
    for (auto& kv : et)
        if (kv.second.size() >= 2)
            for (size_t j = 1; j < kv.second.size(); ++j) uf[find(kv.second[j])] = find(kv.second[0]);

    std::vector<int> compOfV(nv, -1);
    std::vector<std::vector<int>> vadj(nv);
    for (int t = 0; t < nt; ++t) { const int c = find(t); for (int k=0;k<3;++k) compOfV[tris[t].v[k]] = c; }
    for (auto& kv : et) { const int a=int(kv.first>>32), b=int(kv.first&0xffffffff); vadj[a].push_back(b); vadj[b].push_back(a); }

    // Boundary loops (edges used by exactly one triangle), chained into vertex seqs.
    std::vector<std::pair<int,int>> bedges;
    for (auto& kv : et) if (kv.second.size() == 1) bedges.push_back({int(kv.first>>32), int(kv.first&0xffffffff)});
    std::unordered_map<int, std::vector<int>> bAdj;
    for (int s = 0; s < int(bedges.size()); ++s) { bAdj[bedges[s].first].push_back(s); bAdj[bedges[s].second].push_back(s); }
    std::vector<char> bused(bedges.size(), 0);
    std::vector<std::vector<int>> loops;
    for (int s = 0; s < int(bedges.size()); ++s) {
        if (bused[s]) continue;
        std::vector<int> seq; int e = s, cur = bedges[s].first;
        for (size_t g = 0; g <= bedges.size(); ++g) {
            bused[e] = 1;
            const int nx = (bedges[e].first == cur) ? bedges[e].second : bedges[e].first;
            seq.push_back(cur);
            int ne = -1; for (int c : bAdj[nx]) if (!bused[c]) { ne = c; break; }
            if (ne < 0) break;
            e = ne; cur = nx;
        }
        if (seq.size() >= 2) loops.push_back(std::move(seq));
    }
    std::map<int, std::vector<int>> loopsByComp;
    for (int i = 0; i < int(loops.size()); ++i) loopsByComp[compOfV[loops[i][0]]].push_back(i);
    std::map<int, std::vector<int>> vertsByComp;
    for (int v = 0; v < nv; ++v) if (compOfV[v] >= 0) vertsByComp[compOfV[v]].push_back(v);

    // Geodesic (edge-length-weighted) pathfinding so seams run straight across the
    // mesh - a jagged hop-shortest slit would become a lumpy flattened outline.
    // With no targets it instead returns the farthest-reached vertex; either way
    // it fills `par` with the shortest-path tree for backtracking.
    auto dijkstra = [&](const std::vector<int>& sources, const std::set<int>* targets,
                        std::unordered_map<int,int>& par) -> int {
        std::unordered_map<int,double> dist; par.clear();
        std::priority_queue<std::pair<double,int>, std::vector<std::pair<double,int>>,
                            std::greater<std::pair<double,int>>> pq;
        for (int s : sources) { dist[s] = 0.0; par[s] = -1; pq.push({0.0, s}); }
        // NB: `far` (and `near`) are legacy macros defined by <windows.h>, which
        // OCCT 7.9.3's Windows headers leak into this TU (8.0 doesn't) - using
        // them as identifiers breaks the MSVC parse. Hence `farthest`.
        int farthest = sources.empty() ? -1 : sources[0]; double farD = -1.0;
        while (!pq.empty()) {
            const double d = pq.top().first; const int u = pq.top().second; pq.pop();
            if (d > dist[u] + 1e-9) continue;
            if (targets && targets->count(u)) return u;
            if (d > farD) { farD = d; farthest = u; }
            for (int w : vadj[u]) {
                const double nd = d + verts[u].Distance(verts[w]);
                auto it = dist.find(w);
                if (it == dist.end() || nd < it->second) { dist[w] = nd; par[w] = u; pq.push({nd, w}); }
            }
        }
        return farthest;
    };
    auto pathTo = [&](const std::unordered_map<int,int>& par, int hit) {
        std::vector<int> path;
        for (int x = hit; x != -1; ) { path.push_back(x); auto it = par.find(x); x = (it == par.end() ? -1 : it->second); }
        return path;
    };

    // Per-vertex angle defect (2π − Σ incident-triangle angles): ~0 on a flat
    // interior vertex, large at a cone apex / sharp tip. Plus the boundary-vertex
    // set (endpoints of single-use edges).
    std::vector<double> angleSum(nv, 0.0);
    for (const Tri& t : tris)
        for (int k = 0; k < 3; ++k) {
            const gp_Pnt& O = verts[t.v[k]];
            gp_Vec e1(O, verts[t.v[(k+1)%3]]), e2(O, verts[t.v[(k+2)%3]]);
            if (e1.Magnitude() < 1e-12 || e2.Magnitude() < 1e-12) continue;
            angleSum[t.v[k]] += std::acos(std::max(-1.0, std::min(1.0, e1.Normalized().Dot(e2.Normalized()))));
        }
    std::vector<char> isBoundaryVert(nv, 0);
    for (const auto& b : bedges) { isBoundaryVert[b.first] = 1; isBoundaryVert[b.second] = 1; }

    std::set<uint64_t> cutEdges;
    auto addPath = [&](const std::vector<int>& path){ for (size_t i = 1; i < path.size(); ++i) cutEdges.insert(vkey(path[i-1], path[i])); };

    for (auto& cv : vertsByComp) {
        const int comp = cv.first;
        const std::vector<int>& clo = loopsByComp[comp];
        const int nB = int(clo.size());
        std::unordered_map<int,int> par;
        if (nB == 1) {
            // A topological disk can still need a slit: a large interior angle
            // defect (a cone apex / sharp tip) makes LSCM wrap the surface all the
            // way around it - a cone otherwise unwraps to a full disk instead of a
            // sector. Release it with a slit from the apex to the nearest boundary.
            int apex = -1; double worst = 1.0;          // ≈57° defect floor
            for (int v : cv.second)
                if (!isBoundaryVert[v]) {
                    const double defect = 2.0 * M_PI - angleSum[v];
                    if (defect > worst) { worst = defect; apex = v; }
                }
            if (apex < 0) continue;                      // a genuinely flat-ish disk
            std::set<int> bnd;
            for (int li : clo) for (int v : loops[li]) bnd.insert(v);
            const int hit = dijkstra({apex}, &bnd, par);
            if (hit >= 0) addPath(pathTo(par, hit));
            continue;
        }
        if (nB == 0) {                                  // closed → one geodesic slit
            if (cv.second.size() < 2) continue;
            const int a = dijkstra({cv.second[0]}, nullptr, par);  // farthest from seed
            const int b = dijkstra({a}, nullptr, par);             // geodesic-diameter end (par kept)
            addPath(pathTo(par, b));
        } else {                                        // ≥2 loops → join extras to loop 0
            std::set<int> grown(loops[clo[0]].begin(), loops[clo[0]].end());
            for (int k = 1; k < nB; ++k) {
                std::set<int> tgt(loops[clo[k]].begin(), loops[clo[k]].end());
                std::vector<int> src(grown.begin(), grown.end());
                const int hit = dijkstra(src, &tgt, par);
                if (hit >= 0) addPath(pathTo(par, hit));
                for (int v : loops[clo[k]]) grown.insert(v);  // later loops may attach to either
            }
        }
    }
    if (cutEdges.empty()) return;

    std::set<int> cutVerts;
    for (uint64_t e : cutEdges) { cutVerts.insert(int(e>>32)); cutVerts.insert(int(e&0xffffffff)); }
    std::unordered_map<int, std::vector<int>> vtris;
    for (int t = 0; t < nt; ++t) for (int k=0;k<3;++k) vtris[tris[t].v[k]].push_back(t);
    auto slotOf = [&](int t, int v){ for (int k=0;k<3;++k) if (tris[t].v[k]==v) return k; return -1; };

    // For each cut vertex, split its incident-triangle fan into banks separated by
    // the cut edges; bank 0 keeps the vertex, each other bank gets a fresh copy.
    // Computed on the ORIGINAL mesh, then applied, so it's order-independent.
    std::map<std::pair<int,int>, int> cornerRemap;      // (tri, slot) → new vertex index
    for (int v : cutVerts) {
        const std::vector<int>& tl = vtris[v];
        if (tl.size() <= 1) continue;
        std::unordered_map<int,int> idx; for (int i=0;i<int(tl.size());++i) idx[tl[i]] = i;
        std::vector<int> luf(tl.size()); for (int i=0;i<int(tl.size());++i) luf[i] = i;
        std::function<int(int)> lf = [&](int x){ while (luf[x]!=x){ luf[x]=luf[luf[x]]; x=luf[x]; } return x; };
        for (int i = 0; i < int(tl.size()); ++i) {
            const int t = tl[i];
            for (int k = 0; k < 3; ++k) {
                if (tris[t].v[k] != v) continue;
                for (int w : {tris[t].v[(k+1)%3], tris[t].v[(k+2)%3]}) {
                    if (cutEdges.count(vkey(v, w))) continue;     // cut edge → blocks the fan
                    for (int tj : et[vkey(v, w)]) if (tj != t && idx.count(tj)) luf[lf(i)] = lf(idx[tj]);
                }
            }
        }
        // Order banks deterministically by their smallest triangle index.
        std::map<int, std::vector<int>> banks; for (int i=0;i<int(tl.size());++i) banks[lf(i)].push_back(i);
        std::vector<std::pair<int,int>> ord;
        for (auto& g : banks) { int mn = tl[g.second[0]]; for (int i : g.second) mn = std::min(mn, tl[i]); ord.push_back({mn, g.first}); }
        std::sort(ord.begin(), ord.end());
        if (ord.size() <= 1) continue;                  // single bank → slit pinch point
        std::map<int,int> rank; for (int r=0;r<int(ord.size());++r) rank[ord[r].second] = r;
        std::map<int,int> rankToIndex; rankToIndex[0] = v;
        for (int i = 0; i < int(tl.size()); ++i) {
            const int t = tl[i], r = rank[lf(i)];
            auto it = rankToIndex.find(r);
            int ni;
            if (it != rankToIndex.end()) ni = it->second;
            else { ni = int(verts.size()); verts.push_back(verts[v]); rankToIndex[r] = ni; }
            cornerRemap[{t, slotOf(t, v)}] = ni;
        }
    }
    for (auto& kv : cornerRemap) tris[kv.first.first].v[kv.first.second] = kv.second;
}

} // namespace

FlatPattern unfoldConformal(const std::vector<TopoDS_Face>& faces,
                            double maxBevelDeg, double minFoldDeg) {
    FlatPattern out;
    std::vector<gp_Pnt> verts;
    std::vector<Tri> tris;
    buildTriMesh(faces, maxBevelDeg, verts, tris);
    // Open seams so closed / multiply-connected surfaces (a sphere, a wrapped
    // funnel tube) become disks LSCM can flatten - otherwise they collapse.
    cutMeshToDisks(verts, tris);
    const int nv = int(verts.size()), nt = int(tris.size());
    if (nt == 0) { out.warning = "Nothing to tessellate."; return out; }

    // LSCM least-squares rows over the 2·nv unknowns (u at 2i, v at 2i+1). Each
    // triangle contributes the two Cauchy-Riemann residuals, area-weighted.
    struct Row { std::vector<std::pair<int,double>> e; double b = 0.0; };
    std::vector<Row> rows;
    rows.reserve(nt * 2 + 4);
    for (const Tri& t : tris) {
        const glm::dvec2 L0=t.loc[0], L1=t.loc[1], L2=t.loc[2];
        const double As = 0.5*((L1.x-L0.x)*(L2.y-L0.y)-(L2.x-L0.x)*(L1.y-L0.y));
        if (std::fabs(As) < 1e-12) continue;
        const double w = std::sqrt(std::fabs(As));
        auto perp = [](glm::dvec2 v){ return glm::dvec2{-v.y, v.x}; };
        glm::dvec2 g[3] = { perp(L2-L1)/(2*As), perp(L0-L2)/(2*As), perp(L1-L0)/(2*As) };
        Row r1, r2;
        for (int j=0;j<3;++j) {
            const int ui = 2*t.v[j], vi = 2*t.v[j]+1;
            r1.e.push_back({ui,  w*g[j].x}); r1.e.push_back({vi, -w*g[j].y});
            r2.e.push_back({ui,  w*g[j].y}); r2.e.push_back({vi,  w*g[j].x});
        }
        rows.push_back(std::move(r1)); rows.push_back(std::move(r2));
    }

    // Edge → triangles (reused for components, boundary, and classification).
    std::unordered_map<uint64_t, std::vector<int>> edgeTris;
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
            edgeTris[vkey(tris[t].v[k], tris[t].v[(k+1)%3])].push_back(t);

    // Connected components (union-find over shared edges) - LSCM needs each
    // disconnected piece pinned independently, or the unpinned ones collapse.
    std::vector<int> uf(nt); for (int i = 0; i < nt; ++i) uf[i] = i;
    std::function<int(int)> findRoot = [&](int x){ while (uf[x]!=x){ uf[x]=uf[uf[x]]; x=uf[x]; } return x; };
    for (auto& kv : edgeTris)
        if (kv.second.size() >= 2)
            for (size_t j = 1; j < kv.second.size(); ++j) uf[findRoot(kv.second[j])] = findRoot(kv.second[0]);
    std::vector<int> compOfV(nv, -1);
    for (int t = 0; t < nt; ++t) { const int c = findRoot(t); for (int k=0;k<3;++k) compOfV[tris[t].v[k]] = c; }

    // Boundary vertices = endpoints of edges used by a single triangle.
    std::vector<char> isBoundaryV(nv, 0);
    for (auto& kv : edgeTris)
        if (kv.second.size() == 1) { isBoundaryV[int(kv.first>>32)]=1; isBoundaryV[int(kv.first&0xffffffff)]=1; }

    // HARD-pin two far-apart vertices PER COMPONENT, preferring BOUNDARY vertices
    // so we never pin a singular interior point (a cone apex / sphere pole) - that
    // was the fractal. Hard pinning (fixing the DOFs, not soft weights) keeps the
    // gauge exact and the system well-conditioned. Pin distance only sets scale
    // (corrected by the area rescale), so it can't distort the conformal shape.
    std::map<int, std::vector<int>> compVerts;
    for (int v = 0; v < nv; ++v) if (compOfV[v] >= 0) compVerts[compOfV[v]].push_back(v);
    std::unordered_map<int, double> pinned;   // dof → fixed value
    for (auto& cv : compVerts) {
        std::vector<int> cand;
        for (int v : cv.second) if (isBoundaryV[v]) cand.push_back(v);
        if (cand.size() < 2) cand = cv.second;          // closed piece → any verts
        if (cand.size() < 2) continue;
        int a = cand[0], b = cand[1]; double best = -1;
        for (size_t i = 0; i < cand.size(); ++i)
            for (size_t j = i+1; j < cand.size(); ++j) {
                const double d = verts[cand[i]].SquareDistance(verts[cand[j]]);
                if (d > best) { best = d; a = cand[i]; b = cand[j]; }
            }
        double d3 = verts[a].Distance(verts[b]); if (d3 < 1e-9) d3 = 1.0;
        pinned[2*a]=0.0; pinned[2*a+1]=0.0; pinned[2*b]=d3; pinned[2*b+1]=0.0;
    }

    // Reduce to the FREE unknowns; fold pinned values into each row's RHS.
    std::vector<int> freeIdx(2*nv, -1); int nFree = 0;
    for (int d = 0; d < 2*nv; ++d) if (!pinned.count(d)) freeIdx[d] = nFree++;
    struct FRow { std::vector<std::pair<int,double>> e; double b; };
    std::vector<FRow> frows; frows.reserve(rows.size());
    for (const Row& r : rows) {
        FRow fr; fr.b = 0.0;
        for (const auto& e : r.e) {
            auto it = pinned.find(e.first);
            if (it != pinned.end()) fr.b -= e.second * it->second;
            else fr.e.push_back({freeIdx[e.first], e.second});
        }
        frows.push_back(std::move(fr));
    }

    // Solve min ‖A x − b‖² over the free DOFs by CG on the normal equations.
    std::vector<double> x(nFree, 0.0);
    auto applyA  = [&](const std::vector<double>& v, std::vector<double>& y){
        y.assign(frows.size(), 0.0);
        for (size_t i=0;i<frows.size();++i){ double s=0; for (auto& e:frows[i].e) s+=e.second*v[e.first]; y[i]=s; }
    };
    auto applyAT = [&](const std::vector<double>& y, std::vector<double>& v){
        v.assign(nFree, 0.0);
        for (size_t i=0;i<frows.size();++i) for (auto& e:frows[i].e) v[e.first]+=e.second*y[i];
    };
    std::vector<double> bvec(frows.size(), 0.0);
    for (size_t i=0;i<frows.size();++i) bvec[i]=frows[i].b;
    std::vector<double> r, p, Ap, AtAp, Atb;
    {  // r = Aᵀ(b − A·0) = Aᵀb
        std::vector<double> Ax; applyA(x, Ax);
        for (size_t i=0;i<bvec.size();++i) Ax[i] = bvec[i]-Ax[i];
        applyAT(Ax, r);
    }
    p = r;
    double rsold = 0; for (double v : r) rsold += v*v;
    const double rs0 = rsold;
    for (int it = 0; it < 5000 && rsold > 1e-16*rs0 + 1e-24; ++it) {
        applyA(p, Ap);
        double pAAp = 0; for (double v : Ap) pAAp += v*v;
        if (pAAp < 1e-30) break;
        const double alpha = rsold / pAAp;
        for (int i=0;i<nFree;++i) x[i] += alpha*p[i];
        applyAT(Ap, AtAp);
        double rsnew = 0; for (int i=0;i<nFree;++i){ r[i]-=alpha*AtAp[i]; rsnew+=r[i]*r[i]; }
        const double beta = rsnew/rsold;
        for (int i=0;i<nFree;++i) p[i]=r[i]+beta*p[i];
        rsold = rsnew;
    }

    std::vector<glm::dvec2> uv(nv);
    for (int i=0;i<nv;++i) {
        auto get = [&](int d){ auto it=pinned.find(d); return it!=pinned.end()? it->second : x[freeIdx[d]]; };
        uv[i] = { get(2*i), get(2*i+1) };
    }

    // Rescale so total flat area ≈ true surface area (LSCM is scale-only-fixed).
    double area3D = 0, area2D = 0;
    for (const Tri& t : tris) {
        gp_Vec n = gp_Vec(verts[t.v[0]],verts[t.v[1]]).Crossed(gp_Vec(verts[t.v[0]],verts[t.v[2]]));
        area3D += 0.5*n.Magnitude();
        const glm::dvec2 a=uv[t.v[0]],b=uv[t.v[1]],c=uv[t.v[2]];
        area2D += 0.5*std::fabs((b.x-a.x)*(c.y-a.y)-(c.x-a.x)*(b.y-a.y));
    }
    const double scale = (area2D > 1e-12) ? std::sqrt(area3D/area2D) : 1.0;
    for (auto& p2 : uv) p2 *= scale;

    // Each component was solved near the origin → lay them out in a row so they
    // don't overlap each other on the sheet.
    {
        std::map<int, glm::dvec2> cmin, cmax;
        for (int v = 0; v < nv; ++v) {
            const int c = compOfV[v]; if (c < 0) continue;
            if (!cmin.count(c)) { cmin[c] = uv[v]; cmax[c] = uv[v]; }
            else {
                cmin[c].x = std::min(cmin[c].x, uv[v].x); cmin[c].y = std::min(cmin[c].y, uv[v].y);
                cmax[c].x = std::max(cmax[c].x, uv[v].x); cmax[c].y = std::max(cmax[c].y, uv[v].y);
            }
        }
        std::map<int, glm::dvec2> coff; double cursorX = 0;
        for (auto& kv : cmin) {
            const int c = kv.first; const double w = cmax[c].x - cmin[c].x;
            coff[c] = { cursorX - cmin[c].x, -cmin[c].y };
            cursorX += w + std::max(2.0, 0.05 * w);
        }
        for (int v = 0; v < nv; ++v) { const int c = compOfV[v]; if (c >= 0) uv[v] += coff[c]; }
    }

    // Worst-case per-triangle area stretch (distortion %).
    double maxRatio = 1.0;
    for (const Tri& t : tris) {
        gp_Vec n = gp_Vec(verts[t.v[0]],verts[t.v[1]]).Crossed(gp_Vec(verts[t.v[0]],verts[t.v[2]]));
        const double a3 = 0.5*n.Magnitude();
        const glm::dvec2 a=uv[t.v[0]],b=uv[t.v[1]],c=uv[t.v[2]];
        const double a2 = 0.5*std::fabs((b.x-a.x)*(c.y-a.y)-(c.x-a.x)*(b.y-a.y));
        if (a3 > 1e-9) { const double rr = a2/a3; maxRatio = std::max(maxRatio, std::max(rr, 1.0/std::max(rr,1e-9))); }
    }
    out.distortionPct = (maxRatio - 1.0) * 100.0;

    // Edge classification + curvature + loop assembly, using uv directly. Each
    // vertex has one global (u,v), so co-tree edges close up by construction.
    const double minFoldRad = minFoldDeg * M_PI/180.0;
    struct CutSeg { glm::dvec2 a, b; };
    std::vector<CutSeg> cuts;
    for (const auto& kv : edgeTris) {
        const int va = int(kv.first>>32), vb = int(kv.first & 0xffffffff);
        if (kv.second.size() == 1) {
            cuts.push_back({uv[va], uv[vb]});
        } else if (kv.second.size() == 2) {
            const double dot = std::max(-1.0,std::min(1.0, tris[kv.second[0]].normal.Dot(tris[kv.second[1]].normal)));
            const double ang = std::acos(dot);
            if (ang > minFoldRad) { FoldLine fl; fl.a=uv[va]; fl.b=uv[vb]; fl.foldAngleDeg=ang*180.0/M_PI; out.folds.push_back(fl); }
        } else {
            cuts.push_back({uv[va], uv[vb]});
        }
    }
    {  // curvature
        std::vector<double> angleSum(nv,0.0); std::vector<char> seen(nv,0);
        for (const Tri& t : tris) for (int k=0;k<3;++k){
            const gp_Pnt& O=verts[t.v[k]];
            gp_Vec e1(O,verts[t.v[(k+1)%3]]), e2(O,verts[t.v[(k+2)%3]]);
            if (e1.Magnitude()<1e-12||e2.Magnitude()<1e-12) continue;
            angleSum[t.v[k]] += std::acos(std::max(-1.0,std::min(1.0,e1.Normalized().Dot(e2.Normalized()))));
            seen[t.v[k]]=1;
        }
        double defect=0; for (int v=0;v<nv;++v) if (seen[v]&&!isBoundaryV[v]) defect+=std::fabs(2.0*M_PI-angleSum[v]);
        out.curvatureDeg = defect*180.0/M_PI;
    }
    // assemble cut loops by 2D weld
    {
        double diag = 1.0; { Bnd_Box bb; for (auto& f:faces) BRepBndLib::Add(f,bb); if(!bb.IsVoid()){double x0,y0,z0,x1,y1,z1;bb.Get(x0,y0,z0,x1,y1,z1);diag=std::sqrt((x1-x0)*(x1-x0)+(y1-y0)*(y1-y0)+(z1-z0)*(z1-z0));} }
        const double pq = 1.0/std::max(1e-4, diag*1e-5);
        std::map<std::pair<int64_t,int64_t>,int> pmap; std::vector<glm::dvec2> nodes;
        auto nodeOf=[&](glm::dvec2 p)->int{ std::pair<int64_t,int64_t> k{int64_t(std::llround(p.x*pq)),int64_t(std::llround(p.y*pq))}; auto it=pmap.find(k); if(it!=pmap.end())return it->second; int idx=int(nodes.size()); nodes.push_back(p); pmap.emplace(k,idx); return idx; };
        std::vector<std::pair<int,int>> segs;
        for (auto& c:cuts){ int na=nodeOf(c.a), nb=nodeOf(c.b); if(na!=nb) segs.push_back({na,nb}); }
        std::unordered_map<int,std::vector<int>> nodeSegs;
        for (int s=0;s<int(segs.size());++s){ nodeSegs[segs[s].first].push_back(s); nodeSegs[segs[s].second].push_back(s); }
        std::vector<char> used(segs.size(),0);
        FlatFace ff; ff.sourceFaceIndex=0;
        for (int s=0;s<int(segs.size());++s){ if(used[s])continue; FlatLoop loop; int e=s,cur=segs[s].first;
            for(size_t guard=0;guard<=segs.size();++guard){ used[e]=1; int nextN=(segs[e].first==cur)?segs[e].second:segs[e].first; loop.pts.push_back(nodes[cur]); int ne=-1; for(int cand:nodeSegs[nextN]) if(!used[cand]){ne=cand;break;} if(ne<0)break; e=ne;cur=nextN; }
            if (loop.pts.size()>=3) ff.loops.push_back(std::move(loop)); }
        if (!ff.loops.empty()) out.faces.push_back(std::move(ff));
    }

    out.piecesPlaced = 1;
    out.ok = !out.faces.empty();
    if (out.ok)
        out.warning = "Conformal flatten - one stretchy piece, up to " +
                      std::to_string(int(out.distortionPct + 0.5)) + "% area stretch.";
    return out;
}

} // namespace materializr
