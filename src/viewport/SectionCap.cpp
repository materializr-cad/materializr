// Section outline and cap from the triangulation. The previous implementation
// ran two OCCT booleans per body: BRepAlgoAPI_Section for the outline and a
// half-space BRepAlgoAPI_Common, re-meshed, for the cap (735 ms on a 1683-face
// plate and 210 ms on a 54-face fused part per recompute, far worse on swept
// surfaces). Slicing the mesh the viewport already draws gives both in a few
// milliseconds and lands on the same pixels as the clipped body.
#include "SectionCap.h"

#include <BRepMesh_Triangulator.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <NCollection_List.hxx>
#include <NCollection_Vector.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <TColStd_SequenceOfInteger.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>
#include <gp_XYZ.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace materializr {
namespace {

// Endpoints closer than this (in mm, in the plane) are one vertex. Crossings
// of a mesh edge shared by two faces agree to floating-point noise; the finest
// mesh feature the app produces is the 0.01 mm Ultra deflection.
constexpr double kSnapMm = 1e-4;

struct Slice {
    std::vector<gp_Pnt2d> pts;                          // unique vertices, plane coords
    std::vector<std::pair<int, int>> segs;              // vertex index pairs
    // Snap grid: every vertex in each kSnapMm cell. A cell can hold several
    // vertices farther apart than the tolerance, and a hash collision only
    // adds candidates, so the distance check is what decides.
    std::unordered_map<uint64_t, std::vector<int>> cells;
    std::unordered_set<uint64_t> seen; // segments already taken, as sorted vertex pairs

    static uint64_t key(int64_t ix, int64_t iy)
    {
        return (static_cast<uint64_t>(ix) << 32) ^ (static_cast<uint64_t>(iy) & 0xffffffffULL);
    }

    int vertex(const gp_Pnt2d& p)
    {
        const int64_t ix = static_cast<int64_t>(std::floor(p.X() / kSnapMm));
        const int64_t iy = static_cast<int64_t>(std::floor(p.Y() / kSnapMm));
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy) {
                auto it = cells.find(key(ix + dx, iy + dy));
                if (it == cells.end()) continue;
                for (int v : it->second)
                    if (pts[v].Distance(p) <= kSnapMm) return v;
            }
        pts.push_back(p);
        cells[key(ix, iy)].push_back(static_cast<int>(pts.size()) - 1);
        return static_cast<int>(pts.size()) - 1;
    }
};

// Cut every triangle of every meshed face; each crossing yields one segment.
// Also reports the signed-distance range of all nodes for the straddle check.
void sliceTriangulation(const std::vector<FaceMesh>& faces, const gp_Ax3& frame, Slice& out,
                        double& dLo, double& dHi)
{
    const gp_Pnt o = frame.Location();
    const gp_Vec n(frame.Direction()), ex(frame.XDirection()), ey(frame.YDirection());
    auto to2d = [&](const gp_Pnt& p) { gp_Vec r(o, p); return gp_Pnt2d(r.Dot(ex), r.Dot(ey)); };
    auto placed = [&](const FaceMesh& f, int idx) {
        gp_Pnt p = f.tri->Node(idx);
        if (f.moved) p.Transform(f.trsf);
        return p;
    };

    std::vector<double> dist;
    for (const FaceMesh& f : faces) {
        if (f.tri.IsNull()) continue;
        const int nbNodes = f.tri->NbNodes();
        dist.assign(static_cast<size_t>(nbNodes) + 1, 0.0);
        for (int k = 1; k <= nbNodes; ++k) {
            dist[k] = gp_Vec(o, placed(f, k)).Dot(n);
            dLo = std::min(dLo, dist[k]);
            dHi = std::max(dHi, dist[k]);
        }
        for (int t = 1; t <= f.tri->NbTriangles(); ++t) {
            int idx[3];
            f.tri->Triangle(t).Get(idx[0], idx[1], idx[2]);
            bool pos[3];
            int nPos = 0;
            for (int k = 0; k < 3; ++k) {
                pos[k] = dist[idx[k]] >= 0.0; // on the plane counts as the discarded side
                nPos += pos[k] ? 1 : 0;
            }
            if (nPos == 0 || nPos == 3) continue;
            // The vertex alone on its side; the crossing points lie on its two edges.
            int a = 0;
            for (int k = 0; k < 3; ++k)
                if ((nPos == 1) == pos[k]) a = k;
            const gp_Pnt pa = placed(f, idx[a]);
            const double da = dist[idx[a]];
            gp_Pnt2d q[2];
            int qi = 0;
            for (int k = 0; k < 3; ++k) {
                if (k == a) continue;
                const gp_Pnt pk = placed(f, idx[k]);
                const double s = da / (da - dist[idx[k]]);
                q[qi++] = to2d(gp_Pnt(pa.XYZ() + (pk.XYZ() - pa.XYZ()) * s));
            }
            // Snap first, then drop only a segment whose ends are one vertex.
            // Dropping by raw length instead left a hole where a crossing of
            // one to two tolerances was split in half by a triangle diagonal.
            const int a0 = out.vertex(q[0]);
            const int a1 = out.vertex(q[1]);
            // A mesh edge lying IN the plane is produced by both triangles on
            // it; the second copy would close a two-vertex loop that is then
            // dropped, and the edge with it. Keep each segment once.
            if (a0 != a1 &&
                out.seen.insert(Slice::key(std::min(a0, a1), std::max(a0, a1))).second)
                out.segs.emplace_back(a0, a1);
        }
    }
}

// Follow segments end to end. Loops that close are filled and outlined;
// chains that do not (an unmeshed face, a non-manifold junction) are only
// outlined. No repair is attempted.
struct Chains {
    std::vector<std::vector<int>> loops;
    std::vector<std::vector<int>> open;
};

Chains followSegments(const Slice& sl)
{
    std::vector<std::vector<int>> adj(sl.pts.size());
    for (size_t i = 0; i < sl.segs.size(); ++i) {
        adj[sl.segs[i].first].push_back(static_cast<int>(i));
        adj[sl.segs[i].second].push_back(static_cast<int>(i));
    }
    std::vector<bool> used(sl.segs.size(), false);
    Chains out;
    for (size_t s0 = 0; s0 < sl.segs.size(); ++s0) {
        if (used[s0]) continue;
        used[s0] = true;
        const int start = sl.segs[s0].first;
        int cur = sl.segs[s0].second;
        std::vector<int> chain{start};
        bool closed = false;
        while (true) {
            if (cur == start) { closed = true; break; }
            chain.push_back(cur);
            int next = -1;
            for (int s : adj[cur])
                if (!used[s]) { next = s; break; }
            if (next < 0) break;
            used[next] = true;
            cur = sl.segs[next].first == cur ? sl.segs[next].second : sl.segs[next].first;
        }
        if (closed && chain.size() >= 3) out.loops.push_back(std::move(chain));
        else if (!closed && chain.size() >= 2) out.open.push_back(std::move(chain));
    }
    return out;
}

// Drop vertices that sit on the line through their neighbours. A plane that
// crosses a densely meshed planar face picks up one segment per triangle, so a
// plain rectangle can arrive with hundreds of points; the fill wants four.
void mergeCollinear(const std::vector<gp_Pnt2d>& pts, std::vector<int>& loop)
{
    bool changed = true;
    while (changed && loop.size() > 3) {
        changed = false;
        for (size_t i = 0; i < loop.size() && loop.size() > 3; ++i) {
            const gp_Pnt2d& a = pts[loop[(i + loop.size() - 1) % loop.size()]];
            const gp_Pnt2d& b = pts[loop[i]];
            const gp_Pnt2d& c = pts[loop[(i + 1) % loop.size()]];
            const double acx = c.X() - a.X(), acy = c.Y() - a.Y();
            const double len = std::hypot(acx, acy);
            if (len <= kSnapMm) continue;
            const double dist = std::fabs(acx * (b.Y() - a.Y()) - acy * (b.X() - a.X())) / len;
            if (dist <= kSnapMm) {
                loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                --i;
            }
        }
    }
}

double signedArea(const std::vector<gp_Pnt2d>& pts, const std::vector<int>& loop)
{
    double a = 0.0;
    for (size_t i = 0, n = loop.size(); i < n; ++i) {
        const gp_Pnt2d& p = pts[loop[i]];
        const gp_Pnt2d& q = pts[loop[(i + 1) % n]];
        a += p.X() * q.Y() - q.X() * p.Y();
    }
    return 0.5 * a;
}

bool contains(const std::vector<gp_Pnt2d>& pts, const std::vector<int>& loop, const gp_Pnt2d& t)
{
    bool in = false;
    for (size_t i = 0, n = loop.size(); i < n; ++i) {
        const gp_Pnt2d& p = pts[loop[i]];
        const gp_Pnt2d& q = pts[loop[(i + 1) % n]];
        if ((p.Y() > t.Y()) != (q.Y() > t.Y())) {
            const double x = p.X() + (t.Y() - p.Y()) * (q.X() - p.X()) / (q.Y() - p.Y());
            if (x > t.X()) in = !in;
        }
    }
    return in;
}

// Fill one material loop with its holes. BRepMesh_Triangulator is the 2D
// constrained Delaunay behind OCCT's own face mesher, fed points directly: no
// edges, vertices or wires to build, so a loop of hundreds of points costs
// microseconds where a polygon wire cost 50 us per edge. Material lies to the
// left of each wire: the outer loop runs counter-clockwise about the plane
// normal, holes clockwise. Wire indices are 0-based into the point vector and
// the triangles come back 1-based.
struct Ring {
    const std::vector<int>* loop;
    bool reverse;
};

void fillRegion(const std::vector<gp_Pnt2d>& pts, const std::vector<Ring>& rings,
                const gp_Ax3& frame, std::vector<float>& out)
{
    const gp_Pnt o = frame.Location();
    const gp_Vec ex(frame.XDirection()), ey(frame.YDirection());
    NCollection_Vector<gp_XYZ> xyz;
    NCollection_List<TColStd_SequenceOfInteger> wires;
    for (const Ring& r : rings) {
        TColStd_SequenceOfInteger wire;
        const int n = static_cast<int>(r.loop->size());
        for (int i = 0; i < n; ++i) {
            const gp_Pnt2d& p = pts[(*r.loop)[r.reverse ? n - 1 - i : i]];
            xyz.Append(o.XYZ() + ex.XYZ() * p.X() + ey.XYZ() * p.Y());
            wire.Append(xyz.Length() - 1);
        }
        wires.Append(wire);
    }
    BRepMesh_Triangulator triangulator(xyz, wires, frame.Direction());
    NCollection_List<Poly_Triangle> tris;
    if (!triangulator.Perform(tris)) return;
    for (NCollection_List<Poly_Triangle>::Iterator it(tris); it.More(); it.Next()) {
        int a = 0, b = 0, c = 0;
        it.Value().Get(a, b, c);
        const auto valid = [&](int idx) { return idx >= 1 && idx <= xyz.Length(); };
        if (!valid(a) || !valid(b) || !valid(c)) return; // before any vertex of it is out
        for (int idx : {a, b, c}) {
            const gp_XYZ& p = xyz.Value(idx - 1);
            out.push_back(static_cast<float>(p.X()));
            out.push_back(static_cast<float>(p.Y()));
            out.push_back(static_cast<float>(p.Z()));
        }
    }
}

} // namespace

std::vector<FaceMesh> faceMeshes(const TopoDS_Shape& shape)
{
    std::vector<FaceMesh> out;
    if (shape.IsNull()) return out;
    for (TopExp_Explorer fe(shape, TopAbs_FACE); fe.More(); fe.Next()) {
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(TopoDS::Face(fe.Current()), loc);
        FaceMesh fm{tri, Bnd_Box(), loc.Transformation(), loc.IsIdentity() == Standard_False};
        // A bare face stays in the list (null handle) with its box: a plane
        // through that box has a gap in its slice, see sliceSection.
        if (tri.IsNull()) {
            // Bare faces are the mesher's failures, so this is the geometry
            // most likely to upset the box too: a box we cannot get means
            // "assume a gap everywhere" (SetWhole), never "no gap".
            try { BRepBndLib::Add(fe.Current(), fm.box, Standard_False); }
            catch (...) { fm.box.SetWhole(); }
        }
        out.push_back(fm);
    }
    return out;
}

bool sliceSection(const std::vector<FaceMesh>& faces, const gp_Pln& cuttingPlane,
                  SectionSlice& out)
{
    const size_t lines0 = out.lines.size(), cap0 = out.cap.size();
    try {
        const gp_Ax3 frame = cuttingPlane.Position();
        Slice sl;
        double dLo = std::numeric_limits<double>::infinity();
        double dHi = -std::numeric_limits<double>::infinity();
        sliceTriangulation(faces, frame, sl, dLo, dHi);
        // Complete for THIS plane: a bare face the plane does not pass through
        // leaves no gap in this slice (a body with one face the mesher never
        // triangulates keeps its cap on every other plane).
        bool complete = true;
        for (const FaceMesh& f : faces)
            if (f.tri.IsNull() && (f.box.IsVoid() || !f.box.IsOut(cuttingPlane))) { complete = false; break; }
        // The body must straddle the plane; a plane tangent to a face is no cut.
        const double straddleEps = 1e-6;
        if (!(dLo < -straddleEps && dHi > straddleEps)) return false;
        if (sl.segs.empty()) return false;
        Chains ch = followSegments(sl);
        std::vector<std::vector<int>>& loops = ch.loops;
        for (auto& loop : loops) mergeCollinear(sl.pts, loop);

        // Outline: every loop edge, plus the open chains as they are.
        const gp_Pnt o = frame.Location();
        const gp_Vec ex(frame.XDirection()), ey(frame.YDirection());
        auto emit = [&](const gp_Pnt2d& a, const gp_Pnt2d& b) {
            for (const gp_Pnt2d* p : {&a, &b}) {
                const gp_XYZ w = o.XYZ() + ex.XYZ() * p->X() + ey.XYZ() * p->Y();
                out.lines.push_back(static_cast<float>(w.X()));
                out.lines.push_back(static_cast<float>(w.Y()));
                out.lines.push_back(static_cast<float>(w.Z()));
            }
        };
        for (const auto& loop : loops)
            for (size_t i = 0, n = loop.size(); i < n; ++i)
                emit(sl.pts[loop[i]], sl.pts[loop[(i + 1) % n]]);
        for (const auto& chain : ch.open)
            for (size_t i = 0; i + 1 < chain.size(); ++i)
                emit(sl.pts[chain[i]], sl.pts[chain[i + 1]]);

        if (loops.empty()) return out.lines.size() > lines0;
        // A face without a triangulation left a gap in the slice. A missing
        // OUTER wall leaves the loop open and nothing fills; a missing INNER
        // wall (a bore) leaves its loop out entirely, and the outer loop would
        // be filled solid over the hole. Outline only until the body is whole.
        if (!complete) return true;
        // Nesting: a loop inside an even number of others bounds material, an
        // odd number a hole; each hole belongs to the smallest loop around it.
        const size_t L = loops.size();
        std::vector<double> area(L);
        std::vector<int> depth(L, 0), parent(L, -1);
        for (size_t i = 0; i < L; ++i) area[i] = signedArea(sl.pts, loops[i]);
        for (size_t i = 0; i < L; ++i) {
            const gp_Pnt2d& p0 = sl.pts[loops[i][0]];
            const gp_Pnt2d& p1 = sl.pts[loops[i][1]];
            const gp_Pnt2d probe((p0.X() + p1.X()) * 0.5, (p0.Y() + p1.Y()) * 0.5);
            for (size_t j = 0; j < L; ++j) {
                if (i == j || !contains(sl.pts, loops[j], probe)) continue;
                ++depth[i];
                if (parent[i] < 0 || std::fabs(area[j]) < std::fabs(area[parent[i]])) parent[i] = static_cast<int>(j);
            }
        }

        // Fill each material loop with its holes.
        for (size_t i = 0; i < L; ++i) {
            if (depth[i] % 2 != 0) continue;
            std::vector<Ring> rings{{&loops[i], area[i] < 0.0}}; // outer counter-clockwise
            for (size_t h = 0; h < L; ++h)
                if (depth[h] == depth[i] + 1 && parent[h] == static_cast<int>(i))
                    rings.push_back({&loops[h], area[h] > 0.0}); // holes clockwise
            fillRegion(sl.pts, rings, frame, out.cap);
        }
    } catch (...) {
        out.lines.resize(lines0);
        out.cap.resize(cap0);
        return false;
    }
    return out.lines.size() > lines0 || out.cap.size() > cap0;
}

bool computeSectionCap(const TopoDS_Shape& shape, const gp_Pln& cuttingPlane,
                       std::vector<float>& outPositions)
{
    SectionSlice slice;
    if (!sliceSection(faceMeshes(shape), cuttingPlane, slice) || slice.cap.empty()) return false;
    outPositions.insert(outPositions.end(), slice.cap.begin(), slice.cap.end());
    return true;
}

} // namespace materializr
