#include "StlIO.h"
#include "MeshDecimate.h"
#include "../core/Document.h"

#include <RWStl.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_Triangle.hxx>

#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp_Explorer.hxx>

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <Geom_Plane.hxx>
#include <Bnd_Box.hxx>
#include <gp_Trsf.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

#include <cstdint>
#include <unordered_map>

#include <Standard_Failure.hxx>
#include <Standard_ErrorHandler.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {
namespace {

// Stage timing to stderr when MZR_STL_TIMING is set - used by bench_stl_import
// to choose the accuracy→triangle mapping and verify import never hangs.
struct StageTimer {
    bool on;
    std::chrono::steady_clock::time_point t0;
    StageTimer() : on(std::getenv("MZR_STL_TIMING") != nullptr),
                   t0(std::chrono::steady_clock::now()) {}
    void mark(const char* stage) {
        if (!on) return;
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - t0).count();
        std::fprintf(stderr, "[stl] %-18s %8.1f ms\n", stage, ms);
        t0 = now;
    }
};

// Hard ceiling on triangles fed to the builder regardless of accuracy. The
// per-face B-rep build + UnifySameDomain cost grows with facet count, so this
// is the anti-hang guard (tuned against bench_stl_import). The accuracy slider
// only moves the target BELOW this cap; it can never exceed it.
constexpr int kMaxSewTriangles = 60000;

inline uint64_t undirectedEdgeKey(int a, int b) {
    if (a > b) std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

// Reconcile triangle winding so neighbours agree across shared edges, and report
// whether the mesh is a closed 2-manifold. STL files routinely carry flipped or
// inconsistently-wound facets; building shared-edge B-rep topology from those
// directly yields a non-manifold/invalid shell (two faces using a shared edge in
// the same direction). A flood fill over edge adjacency flips offending
// neighbours so every interior edge ends up used once in each direction. Returns
// true only when every undirected edge is shared by exactly two facets (closed),
// which is the precondition for promoting the shell to a valid solid.
bool orientMeshConsistently(SimpleMesh& m) {
    const int nt = static_cast<int>(m.tris.size());
    std::unordered_map<uint64_t, std::vector<int>> edgeTris;
    edgeTris.reserve(nt * 3);
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
            edgeTris[undirectedEdgeKey(m.tris[t][k], m.tris[t][(k + 1) % 3])].push_back(t);

    // Closed 2-manifold ⇔ every edge has exactly two incident facets.
    bool watertight = !edgeTris.empty();
    for (const auto& kv : edgeTris)
        if (kv.second.size() != 2) { watertight = false; break; }

    // Does facet t traverse the directed edge u→v?
    auto traverses = [&](int t, int u, int v) {
        const auto& T = m.tris[t];
        return (T[0]==u && T[1]==v) || (T[1]==u && T[2]==v) || (T[2]==u && T[0]==v);
    };

    std::vector<char> visited(nt, 0);
    std::vector<int> stack;
    for (int seed = 0; seed < nt; ++seed) {
        if (visited[seed]) continue;
        visited[seed] = 1;
        stack.push_back(seed);
        while (!stack.empty()) {
            const int t = stack.back();
            stack.pop_back();
            const std::array<int, 3> T = m.tris[t]; // fixed: t is already oriented
            for (int k = 0; k < 3; ++k) {
                const int u = T[k], v = T[(k + 1) % 3];
                for (int nb : edgeTris[undirectedEdgeKey(u, v)]) {
                    if (nb == t || visited[nb]) continue;
                    // A consistent neighbour traverses the shared edge v→u (the
                    // opposite direction). If it goes u→v too, flip its winding.
                    if (traverses(nb, u, v)) std::swap(m.tris[nb][1], m.tris[nb][2]);
                    visited[nb] = 1;
                    stack.push_back(nb);
                }
            }
        }
    }
    return watertight;
}

} // namespace

ImportResult StlIO::import(const std::string& filePath, Document& doc, double accuracy) {
    ImportResult result;
    accuracy = std::clamp(accuracy, 0.0, 1.0);
    StageTimer timer;

    // OCCT can throw Standard_Failure (or raise a kernel signal) on malformed
    // meshes or while sewing degenerate facets. Catch it so a bad import fails
    // gracefully instead of aborting the process - on Android an uncaught fault
    // shows up as an instant crash.
    try {
    OCC_CATCH_SIGNALS

    Handle(Poly_Triangulation) mesh = RWStl::ReadFile(filePath.c_str());
    if (mesh.IsNull() || mesh->NbTriangles() == 0) {
        result.errorMessage = "Failed to read STL file (empty or unrecognized): " + filePath;
        return result;
    }
    timer.mark("read");

    // Pull the triangulation into a bare indexed mesh (RWStl already shares
    // vertices, so adjacent facets reference the same node - what the decimator
    // and the sewing step both need to weld cleanly).
    SimpleMesh smesh;
    smesh.nodes.reserve(mesh->NbNodes());
    for (Standard_Integer i = 1; i <= mesh->NbNodes(); ++i)
        smesh.nodes.push_back(mesh->Node(i));
    smesh.tris.reserve(mesh->NbTriangles());
    for (Standard_Integer i = 1; i <= mesh->NbTriangles(); ++i) {
        Standard_Integer n1, n2, n3;
        mesh->Triangle(i).Get(n1, n2, n3);
        smesh.tris.push_back({n1 - 1, n2 - 1, n3 - 1}); // 1-based → 0-based
    }
    result.trianglesBefore = static_cast<int>(smesh.tris.size());

    // Accuracy → decimation target. Geometric ramp 2k → 60k (capped). Decimation
    // must precede sewing: fewer facets both bound sewing time and pre-merge
    // fairly-flat regions (QEM collapses flats first).
    int target = static_cast<int>(std::lround(2000.0 * std::pow(100.0, accuracy)));
    target = std::min(target, kMaxSewTriangles);
    decimateMesh(smesh, target);
    result.trianglesAfter = static_cast<int>(smesh.tris.size());
    if (timer.on)
        std::fprintf(stderr, "[stl] decimate target=%d -> %d tris\n",
                     target, result.trianglesAfter);
    timer.mark("decimate");

    // Reconcile winding (STLs are often inconsistently wound) and learn whether
    // the mesh is closed - the precondition for a valid solid below.
    const bool watertight = orientMeshConsistently(smesh);
    timer.mark("orient");

    // Bounding-box diagonal scales the merge tolerance below.
    Bnd_Box box;
    for (const gp_Pnt& p : smesh.nodes) box.Add(p);
    double diag = 0.0;
    if (!box.IsVoid()) {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        const double dx = xmax - xmin, dy = ymax - ymin, dz = zmax - zmin;
        diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Build the shell DIRECTLY from the indexed mesh: one shared TopoDS_Vertex
    // per node, one shared TopoDS_Edge per mesh edge, faces referencing them.
    // We already know the connectivity from the indices, so there is nothing to
    // match geometrically - this is O(n). BRepBuilderAPI_Sewing, by contrast,
    // re-discovers connectivity by proximity search and is pathologically slow
    // on a tessellated mesh (it was the import "hang").
    BRep_Builder builder;
    std::vector<TopoDS_Vertex> verts(smesh.nodes.size());
    for (size_t i = 0; i < smesh.nodes.size(); ++i)
        verts[i] = BRepBuilderAPI_MakeVertex(smesh.nodes[i]);

    std::unordered_map<uint64_t, TopoDS_Edge> edgeMap;
    edgeMap.reserve(smesh.tris.size() * 3);
    auto edgeFor = [&](int a, int b) -> const TopoDS_Edge& {
        const int lo = a < b ? a : b, hi = a < b ? b : a;
        const uint64_t key = (uint64_t(uint32_t(lo)) << 32) | uint32_t(hi);
        auto it = edgeMap.find(key);
        if (it != edgeMap.end()) return it->second;
        // Canonical direction lo→hi; per-face orientation is applied below.
        TopoDS_Edge e = BRepBuilderAPI_MakeEdge(verts[lo], verts[hi]);
        return edgeMap.emplace(key, e).first->second;
    };

    TopoDS_Shell shell;
    builder.MakeShell(shell);
    int facesAdded = 0;
    for (const auto& tri : smesh.tris) {
        const int a = tri[0], b = tri[1], c = tri[2];
        const gp_Pnt& p1 = smesh.nodes[a];
        const gp_Pnt& p2 = smesh.nodes[b];
        const gp_Pnt& p3 = smesh.nodes[c];
        const gp_Vec n = gp_Vec(p1, p2).Crossed(gp_Vec(p1, p3));
        if (n.Magnitude() < 1e-14) continue; // degenerate facet

        // Three shared edges, each oriented to traverse a→b→c→a.
        try {
            BRepBuilderAPI_MakeWire wm;
            const int seq[3][2] = {{a, b}, {b, c}, {c, a}};
            for (auto& s : seq) {
                const TopoDS_Edge& e = edgeFor(s[0], s[1]);
                wm.Add(s[0] < s[1] ? e : TopoDS::Edge(e.Reversed()));
            }
            if (!wm.IsDone()) continue;
            Handle(Geom_Plane) plane =
                new Geom_Plane(gp_Ax3(p1, gp_Dir(n)));
            BRepBuilderAPI_MakeFace mf(plane, wm.Wire(), /*Inside=*/Standard_True);
            if (!mf.IsDone()) continue;
            builder.Add(shell, mf.Face());
            ++facesAdded;
        } catch (...) { continue; }
    }
    if (facesAdded == 0) {
        result.errorMessage = "STL contained no usable facets.";
        return result;
    }
    TopoDS_Shape sewn = shell;
    timer.mark("build-shell");

    // Promote to a solid ONLY when the mesh is a closed 2-manifold (every edge
    // shared by exactly two facets). An open/holey mesh stays a shell - still
    // selectable and sketchable, just without volume/boolean semantics - rather
    // than being wrapped as an invalid "solid" that misbehaves downstream.
    TopoDS_Shape solidified = sewn;
    if (watertight) {
        bool madeSolid = false;
        BRepBuilderAPI_MakeSolid mkSolid;
        for (TopExp_Explorer ex(sewn, TopAbs_SHELL); ex.More(); ex.Next()) {
            mkSolid.Add(TopoDS::Shell(ex.Current()));
            madeSolid = true;
        }
        if (madeSolid && mkSolid.IsDone() && !mkSolid.Solid().IsNull()) {
            TopoDS_Shape solidShape = mkSolid.Solid(); // copy-construct to a base handle
            // Consistent winding may still be globally inward; a negative volume
            // means inside-out, so flip it so material is on the inside.
            GProp_GProps vprops;
            BRepGProp::VolumeProperties(solidShape, vprops);
            if (vprops.Mass() < 0.0) solidShape.Reverse();
            solidified = solidShape;
        }
    }
    timer.mark("solid");

    // Merge near-coplanar facets into single planar faces. This is the knob that
    // makes "fairly flat" regions one pickable, sketchable face: a larger angular
    // tolerance merges adjacent facets whose normals differ slightly. Every input
    // face is planar, so merged faces stay planar. Accuracy → angle: 0 → 6°,
    // 1 → 0.5°. This is deliberately conservative - a wide tolerance (the old
    // 20°) lumps gently-curved/angled facets into one bogus "flat" face whose
    // plane is then unreliable to sketch on, which is exactly the low-accuracy
    // "can't tell what's flat" problem. Keep merging to genuinely near-coplanar
    // facets; the sketch-on-face path best-fits the region's plane regardless.
    const double angDeg = std::max(0.5, 6.0 * (1.0 - accuracy));
    const double angTol = angDeg * M_PI / 180.0;
    const double linTol = std::max(1e-6, diag * 1e-6);
    TopoDS_Shape finalShape = solidified;
    try {
        ShapeUpgrade_UnifySameDomain unify(solidified, /*unifyEdges=*/Standard_True,
                                           /*unifyFaces=*/Standard_True,
                                           /*concatBSplines=*/Standard_False);
        unify.SetSafeInputMode(Standard_False); // throwaway input - no need to copy
        unify.SetAngularTolerance(angTol);
        unify.SetLinearTolerance(linTol);
        unify.Build();
        if (!unify.Shape().IsNull()) finalShape = unify.Shape();
    } catch (...) {
        // Keep the un-unified shape on any failure - it's still valid geometry.
    }
    timer.mark("unify");

    // STL, like STEP, is conventionally Z-up; this viewer is Y-up. Rotate -90°
    // about X so the model stands on its natural ground plane.
    gp_Trsf zUpToYUp;
    zUpToYUp.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)),
                         -M_PI * 0.5);
    try {
        BRepBuilderAPI_Transform xf(finalShape, zUpToYUp, /*copy=*/true);
        if (xf.IsDone() && !xf.Shape().IsNull()) finalShape = xf.Shape();
    } catch (...) {}

    if (finalShape.IsNull()) {
        result.errorMessage = "STL import produced an empty shape.";
        return result;
    }

    for (TopExp_Explorer ex(finalShape, TopAbs_FACE); ex.More(); ex.Next())
        ++result.faceCount;

    const int bodyId = doc.addBody(finalShape, "Imported_STL");
    // Tag it so the viewport takes the mesh fast-path (cached picking + optional
    // wireframe) and so the flag round-trips through save/load.
    doc.setBodyMesh(bodyId, true);
    result.success = true;
    result.bodiesImported = 1;
    return result;
    } catch (const Standard_Failure& e) {
        result.success = false;
        result.errorMessage = std::string("STL import failed: ") + e.GetMessageString();
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = std::string("STL import failed: ") + e.what();
    } catch (...) {
        result.success = false;
        result.errorMessage = "STL import failed: unrecognized error";
    }
    return result;
}

} // namespace materializr
