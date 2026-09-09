#include "StlExport.h"
#include "../core/Document.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <BRepBuilderAPI_Transform.hxx>
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
#include <TopoDS_Face.hxx>
#include <TopExp_Explorer.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

namespace materializr {

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
    struct V3 { double x, y, z; };
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
        std::vector<T3> nt;
        nt.reserve(tris.size());
        for (const T3& t : tris) {
            const int a = remap[t.a], b = remap[t.b], c = remap[t.c];
            if (a == b || b == c || a == c) continue;   // degenerate after weld
            nt.push_back({a, b, c});
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
                for (std::size_t k = 1; k + 1 < loop.size(); ++k)
                    tris.push_back({loop[0], loop[(int)k+1], loop[(int)k]});
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
