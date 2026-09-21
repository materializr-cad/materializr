#include "StlExport.h"
#include "../core/Document.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <StlAPI_Writer.hxx>
#include <gp_Trsf.hxx>
#include <gp_Ax1.hxx>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <TopLoc_Location.hxx>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace materializr {

namespace {
struct V3 { double x, y, z; };

// Triangulate a small (already-closed) boundary loop for the STL export
// pinhole repair, WITHOUT assuming it's convex or that vertex 0 sees every
// other vertex - a blind fan from loop[0] folds back over itself whenever
// the loop bends the "wrong" way, which reads as the model being "folded in
// on itself" right at that spot even though the rest of the mesh is fine.
//
// The loop is a genuine 3D polygon (BRep imprecision keeps it only
// approximately planar), so: fit a best-fit normal (Newell's method, robust
// to that imprecision), project into that plane, then ear-clip in 2D - the
// standard approach for a simple polygon that may be non-convex. Returns
// triangles as indices INTO `loop` (the caller maps back to global vertex
// ids); empty if ear-clipping couldn't fully resolve it (a self-intersecting
// boundary, vanishingly rare for a real BRep edge loop), in which case the
// caller falls back to the old fan so a mesh is never LESS watertight than
// before, only better-shaped when there's a valid triangulation to find.
std::vector<std::array<int, 3>> triangulateLoop(const std::vector<V3>& loop) {
    const int m = static_cast<int>(loop.size());
    std::vector<std::array<int, 3>> out;
    if (m < 3) return out;

    // Best-fit normal via Newell's method.
    V3 n{0, 0, 0};
    for (int i = 0; i < m; ++i) {
        const V3& a = loop[i];
        const V3& b = loop[(i + 1) % m];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    double nlen = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (nlen < 1e-12) return out;   // degenerate (collinear) loop
    n.x /= nlen; n.y /= nlen; n.z /= nlen;

    // An in-plane basis (u, v) orthogonal to the normal.
    V3 arb = (std::fabs(n.x) < 0.9) ? V3{1, 0, 0} : V3{0, 1, 0};
    double d = arb.x * n.x + arb.y * n.y + arb.z * n.z;
    V3 u{arb.x - d * n.x, arb.y - d * n.y, arb.z - d * n.z};
    double ulen = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
    u.x /= ulen; u.y /= ulen; u.z /= ulen;
    V3 v{n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};

    std::vector<std::pair<double, double>> p2(m);
    for (int i = 0; i < m; ++i)
        p2[i] = {loop[i].x * u.x + loop[i].y * u.y + loop[i].z * u.z,
                 loop[i].x * v.x + loop[i].y * v.y + loop[i].z * v.z};

    auto cross2 = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };

    // Ear-clipping wants the polygon wound CCW in (u, v).
    std::vector<int> idx(m);
    for (int i = 0; i < m; ++i) idx[i] = i;
    double signedArea = 0.0;
    for (int i = 0; i < m; ++i) {
        const auto& a = p2[idx[i]]; const auto& b = p2[idx[(i + 1) % m]];
        signedArea += cross2(a.first, a.second, b.first, b.second);
    }
    if (signedArea < 0.0) std::reverse(idx.begin(), idx.end());

    auto isConvex = [&](int ip, int i, int inx) {
        const auto& A = p2[idx[ip]]; const auto& B = p2[idx[i]]; const auto& C = p2[idx[inx]];
        return cross2(B.first - A.first, B.second - A.second,
                      C.first - B.first, C.second - B.second) > 0.0;
    };
    auto pointInTri = [&](std::pair<double, double> P, std::pair<double, double> A,
                          std::pair<double, double> B, std::pair<double, double> C) {
        const double d1 = cross2(B.first - A.first, B.second - A.second, P.first - A.first, P.second - A.second);
        const double d2 = cross2(C.first - B.first, C.second - B.second, P.first - B.first, P.second - B.second);
        const double d3 = cross2(A.first - C.first, A.second - C.second, P.first - C.first, P.second - C.second);
        const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        return !(hasNeg && hasPos);
    };

    int guard = 0;
    while (idx.size() > 2 && guard++ < 4 * m + 16) {
        bool clipped = false;
        const int n2 = static_cast<int>(idx.size());
        for (int i = 0; i < n2; ++i) {
            const int ip = (i - 1 + n2) % n2, inx = (i + 1) % n2;
            if (!isConvex(ip, i, inx)) continue;
            bool anyInside = false;
            for (int j = 0; j < n2; ++j) {
                if (j == ip || j == i || j == inx) continue;
                if (pointInTri(p2[idx[j]], p2[idx[ip]], p2[idx[i]], p2[idx[inx]])) { anyInside = true; break; }
            }
            if (anyInside) continue;
            out.push_back({idx[ip], idx[i], idx[inx]});
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) return {};   // couldn't resolve - let the caller fall back
    }
    return out;
}
} // namespace

static int countTriangles(const TopoDS_Shape& shape) {
    int count = 0;
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TopoDS_Face& face = TopoDS::Face(explorer.Current());
        TopLoc_Location location;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);
        if (!triangulation.IsNull()) {
            count += triangulation->NbTriangles();
        }
    }
    return count;
}

StlExportResult StlExport::exportFile(const std::string& filePath, const Document& doc,
                                       const StlExportOptions& options) {
    StlExportResult result;

    std::vector<int> allIds = doc.getAllBodyIds();
    if (allIds.empty()) {
        result.errorMessage = "No bodies to export.";
        return result;
    }

    // Build a compound of all visible bodies
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);

    int bodyCount = 0;
    for (int id : allIds) {
        if (doc.isBodyVisible(id)) {
            const TopoDS_Shape& shape = doc.getBody(id);
            if (!shape.IsNull()) {
                builder.Add(compound, shape);
                ++bodyCount;
            }
        }
    }

    if (bodyCount == 0) {
        result.errorMessage = "No visible bodies to export.";
        return result;
    }

    return exportShape(filePath, compound, options);
}

StlExportResult StlExport::exportShape(const std::string& filePath, const TopoDS_Shape& inShape,
                                        const StlExportOptions& options) {
    StlExportResult result;

    if (inShape.IsNull()) {
        result.errorMessage = "Cannot export a null shape.";
        return result;
    }

    // Y-up scene → Z-up file, same proper rotation StepIO::exportBodies
    // applies (a bare Y/Z swap would MIRROR the part - inside-out STLs).
    // Slicers and other CAD expect Z-up; without this, exported parts lie
    // on their side on the print bed.
    gp_Trsf yUpToZUp;
    yUpToZUp.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)),
                         M_PI * 0.5);
    TopoDS_Shape shape = BRepBuilderAPI_Transform(inShape, yUpToZUp,
                                                  Standard_True).Shape();

    // Sew before meshing: closes any genuinely-free edges (faces that
    // are meant to be adjacent but, within tolerance, aren't actually
    // topologically shared) before they can crack the mesh below.
    // Non-solid shapes (a bare compound of faces, e.g. from a partial
    // repair) sew to a shell/compound rather than a solid, which is
    // fine here - only the triangulated faces matter for STL, not
    // solid-ness.
    {
        BRepBuilderAPI_Sewing sewer(1e-3);
        sewer.Add(shape);
        sewer.Perform();
        const TopoDS_Shape sewn = sewer.SewedShape();
        if (!sewn.IsNull()) shape = sewn;
    }

    // A boolean/fillet/chamfer chain can also leave a genuinely
    // NON-MANIFOLD edge behind - one real topological edge shared by
    // three (or more) faces instead of two, usually because a sliver
    // face the operation introduced never got merged into its big
    // neighbour. Sewing can't fix this (the edge already IS shared, so
    // there's no free-edge gap to close) and BRepCheck_Analyzer doesn't
    // flag it either (each face and edge is individually well-formed).
    // Record every such edge's mesh vertices now, from the real BRep
    // topology, so the weld below can tell a genuine 3-face edge apart
    // from an unrelated pair of vertices that only LOOK adjacent because
    // they happen to land in the same weld cell.
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaceMap;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaceMap);
    std::vector<std::pair<int,int>> nonManifoldRawEdges;

    // Tessellate the shape
    BRepMesh_IncrementalMesh mesh(
        shape, materializr::meshParams(options.linearDeflection, options.angularDeflection, false));

    if (!mesh.IsDone()) {
        result.errorMessage = "Tessellation failed.";
        return result;
    }

    // Gather, weld, and repair the mesh before writing.
    //
    // StlAPI_Writer dumps each face's triangulation independently, so every
    // BRep imperfection becomes a mesh crack: a body measured here (bridged
    // loft seams at sew-inflated tolerances, plus two ancient degenerate
    // point-edges at its base) exported with 6,904 open mesh edges. Slicers
    // then run their own repair, and on that part both Bambu Studio and
    // OrcaSlicer silently dropped the bottom 5 mm of the print.
    //
    // Welding vertices at 1e-3 mm alone took those 6,904 open edges to ONE
    // (aggressive welds go the other way -- they collapse thin features into
    // fresh degeneracies -- so the radius stays fixed and small). The fan
    // fill then closes any remaining pinhole loop up to 12 edges. Residual
    // boundaries are reported, not hidden.
    struct T3 { int a, b, c; };
    std::vector<V3> verts;
    std::vector<T3> tris;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face f = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(f, loc);
        if (t.IsNull()) continue;
        const int base = (int)verts.size();
        const gp_Trsf& tr = loc.Transformation();
        for (int k = 1; k <= t->NbNodes(); ++k) {
            gp_Pnt p = t->Node(k); p.Transform(tr);
            verts.push_back({p.X(), p.Y(), p.Z()});
        }
        const bool rev = (f.Orientation() == TopAbs_REVERSED);
        for (int k = 1; k <= t->NbTriangles(); ++k) {
            int a, b, c; t->Triangle(k).Get(a, b, c);
            if (rev) std::swap(b, c);
            tris.push_back({base + a - 1, base + b - 1, base + c - 1});
        }
        for (TopExp_Explorer ee(f, TopAbs_EDGE); ee.More(); ee.Next()) {
            const TopoDS_Edge& edge = TopoDS::Edge(ee.Current());
            if (edgeFaceMap.FindFromKey(edge).Extent() <= 2) continue;
            const Handle(Poly_PolygonOnTriangulation)& poly =
                BRep_Tool::PolygonOnTriangulation(edge, t, loc);
            if (poly.IsNull()) continue;
            const TColStd_Array1OfInteger& nodes = poly->Nodes();
            for (int k = nodes.Lower(); k < nodes.Upper(); ++k)
                nonManifoldRawEdges.push_back({base + nodes(k) - 1, base + nodes(k + 1) - 1});
        }
    }
    if (tris.empty()) {
        result.errorMessage = "Tessellation produced no triangles.";
        return result;
    }
    {   // weld at 1e-3 mm
        const double weld = 1e-3;
        std::unordered_map<long long, int> grid;
        std::vector<int> remap(verts.size());
        std::vector<V3> nv;
        auto cell = [&](double v) { return (long long)std::llround(v / weld); };
        for (std::size_t i = 0; i < verts.size(); ++i) {
            const long long h = (cell(verts[i].x) * 73856093LL)
                              ^ (cell(verts[i].y) * 19349663LL)
                              ^ (cell(verts[i].z) * 83492791LL);
            auto it = grid.find(h);
            if (it == grid.end()) { grid[h] = (int)nv.size();
                                    remap[i] = (int)nv.size(); nv.push_back(verts[i]); }
            else remap[i] = it->second;
        }
        auto buildTris = [&](const std::vector<int>& rmap) {
            std::vector<T3> out;
            out.reserve(tris.size());
            for (const T3& t : tris) {
                const int a = rmap[t.a], b = rmap[t.b], c = rmap[t.c];
                if (a == b || b == c || a == c) continue;   // degenerate after weld
                out.push_back({a, b, c});
            }
            return out;
        };
        std::vector<T3> nt = buildTris(remap);

        // Welded ids for the genuinely non-manifold BRep edges recorded
        // above - these are allowed to be shared by more than 2
        // triangles below without tripping the anti-fold guard.
        std::set<std::pair<int,int>> legitMultiUse;
        for (const auto& re : nonManifoldRawEdges) {
            int a = remap[re.first], b = remap[re.second];
            if (a > b) std::swap(a, b);
            legitMultiUse.insert({a, b});
        }

        // A weld only reconnects a real crack (2 faces' independent
        // triangulations of the SAME B-Rep edge) when it leaves that edge
        // shared by exactly 2 triangles, same as any interior edge. A grid
        // cell can also catch two vertices that are merely close in SPACE
        // for an unrelated reason - two turns of a fine-pitch thread, or a
        // fillet wrapping tightly around one - and welding those bridges
        // geometry that was never adjacent, which bakes as the mesh folding
        // over itself right there (not a crack: a fold). Detect any edge a
        // merge pushed past 2 users and undo THOSE vertices' merges - they
        // revert to their pre-weld, per-face identity, becoming open edges
        // instead (handled by the pinhole fill below, or reported as still
        // open) rather than silently-wrong folded geometry.
        std::map<std::pair<int,int>, int> ec;
        for (const T3& t : nt) {
            const int e[3][2] = {{t.a,t.b},{t.b,t.c},{t.c,t.a}};
            for (const auto& ed : e) { int a=ed[0], b=ed[1]; if (a>b) std::swap(a,b); ++ec[{a,b}]; }
        }
        std::unordered_map<int, char> poisoned;   // welded-id -> present
        for (const auto& kv : ec) {
            if (kv.second <= 2 || legitMultiUse.count(kv.first)) continue;
            poisoned[kv.first.first] = 1; poisoned[kv.first.second] = 1;
        }
        if (!poisoned.empty()) {
            std::vector<int> remap2 = remap;
            for (std::size_t i = 0; i < remap.size(); ++i)
                if (poisoned.count(remap[i])) remap2[i] = static_cast<int>(nv.size() + i); // unique per vertex

            std::unordered_map<long long, int> compact;
            std::vector<V3> nv2;
            std::vector<int> remap3(remap2.size());
            for (std::size_t i = 0; i < remap2.size(); ++i) {
                const long long key = remap2[i];
                auto it = compact.find(key);
                if (it == compact.end()) {
                    const int nid = static_cast<int>(nv2.size());
                    compact[key] = nid;
                    nv2.push_back(key >= static_cast<long long>(nv.size()) ? verts[i] : nv[remap2[i]]);
                    remap3[i] = nid;
                } else {
                    remap3[i] = it->second;
                }
            }
            nv.swap(nv2);
            remap.swap(remap3);
            nt = buildTris(remap);
        }
        verts.swap(nv); tris.swap(nt);
    }
    {   // close remaining pinholes: chain boundary edges into loops, fan-fill
        // any loop of up to 12 edges
        std::map<std::pair<int,int>, int> cnt;
        for (const T3& t : tris) {
            const int e[3][2] = {{t.a,t.b},{t.b,t.c},{t.c,t.a}};
            for (const auto& ed : e) {
                int a = ed[0], b = ed[1]; if (a > b) std::swap(a, b);
                cnt[{a,b}]++;
            }
        }
        std::multimap<int,int> nxt;     // directed boundary edges as found
        for (const T3& t : tris) {
            const int e[3][2] = {{t.a,t.b},{t.b,t.c},{t.c,t.a}};
            for (const auto& ed : e) {
                int a = ed[0], b = ed[1]; if (a > b) std::swap(a, b);
                if (cnt[{a,b}] == 1) nxt.insert({ed[0], ed[1]});
            }
        }
        int filled = 0, loops = 0;
        while (!nxt.empty()) {
            const int start = nxt.begin()->first;
            std::vector<int> loop{start};
            int cur = start; bool closed = false;
            for (int guard = 0; guard < 64; ++guard) {
                auto it = nxt.find(cur);
                if (it == nxt.end()) break;
                cur = it->second;
                nxt.erase(it);
                if (cur == start) { closed = true; break; }
                loop.push_back(cur);
            }
            ++loops;
            if (closed && loop.size() >= 3 && loop.size() <= 12) {
                std::vector<V3> loopPos;
                loopPos.reserve(loop.size());
                for (int id : loop) loopPos.push_back(verts[id]);
                std::vector<std::array<int, 3>> capTris = triangulateLoop(loopPos);
                if (!capTris.empty()) {
                    for (const auto& t : capTris)
                        tris.push_back({loop[t[0]], loop[t[1]], loop[t[2]]});
                } else {
                    // Ear-clipping couldn't resolve this one (a genuinely
                    // self-intersecting boundary) - fall back to the old
                    // blind fan so the mesh is never LESS watertight than
                    // before, even though this specific cap may still fold.
                    for (std::size_t k = 1; k + 1 < loop.size(); ++k)
                        tris.push_back({loop[0], loop[(int)k+1], loop[(int)k]});
                }
                ++filled;
            }
        }
        long open = 0;
        std::map<std::pair<int,int>, int> cnt2;
        for (const T3& t : tris) {
            const int e[3][2] = {{t.a,t.b},{t.b,t.c},{t.c,t.a}};
            for (const auto& ed : e) {
                int a = ed[0], b = ed[1]; if (a > b) std::swap(a, b);
                cnt2[{a,b}]++;
            }
        }
        for (const auto& kv : cnt2) if (kv.second != 2) ++open;
        std::fprintf(stderr, "[StlExport] repaired mesh: %zu tris, %d hole(s) "
                     "filled, %ld boundary edge(s) remain%s\n",
                     tris.size(), filled, open,
                     open ? "  <-- NOT fully watertight" : " (watertight)");
    }
    {   // write it ourselves (binary or ASCII)
        auto normal = [&](const T3& t, float* n) {
            const V3& A = verts[t.a]; const V3& B = verts[t.b]; const V3& C = verts[t.c];
            const double ux=B.x-A.x, uy=B.y-A.y, uz=B.z-A.z;
            const double vx=C.x-A.x, vy=C.y-A.y, vz=C.z-A.z;
            double nx=uy*vz-uz*vy, ny=uz*vx-ux*vz, nz=ux*vy-uy*vx;
            const double L = std::sqrt(nx*nx+ny*ny+nz*nz);
            if (L > 1e-20) { nx/=L; ny/=L; nz/=L; }
            n[0]=(float)nx; n[1]=(float)ny; n[2]=(float)nz;
        };
        if (options.binary) {
            std::ofstream out(filePath, std::ios::binary);
            if (!out) { result.errorMessage = "Failed to open " + filePath; return result; }
            char hdr[80] = "Materializr welded STL";
            out.write(hdr, 80);
            const std::uint32_t n = (std::uint32_t)tris.size();
            out.write((const char*)&n, 4);
            for (const T3& t : tris) {
                float buf[12];
                normal(t, buf);
                const V3* pts[3] = {&verts[t.a], &verts[t.b], &verts[t.c]};
                for (int k = 0; k < 3; ++k) {
                    buf[3+k*3+0]=(float)pts[k]->x;
                    buf[3+k*3+1]=(float)pts[k]->y;
                    buf[3+k*3+2]=(float)pts[k]->z;
                }
                out.write((const char*)buf, 48);
                const std::uint16_t attr = 0;
                out.write((const char*)&attr, 2);
            }
            if (!out.good()) { result.errorMessage = "Write failed: " + filePath; return result; }
        } else {
            std::ofstream out(filePath);
            if (!out) { result.errorMessage = "Failed to open " + filePath; return result; }
            out << "solid materializr\n";
            for (const T3& t : tris) {
                float n[3]; normal(t, n);
                out << " facet normal " << n[0] << ' ' << n[1] << ' ' << n[2]
                    << "\n  outer loop\n";
                for (int idx : {t.a, t.b, t.c}) {
                    const V3& p = verts[idx];
                    out << "   vertex " << p.x << ' ' << p.y << ' ' << p.z << "\n";
                }
                out << "  endloop\n endfacet\n";
            }
            out << "endsolid materializr\n";
        }
    }
    result.triangleCount = (int)tris.size();
    result.success = true;
    return result;
}

} // namespace materializr
