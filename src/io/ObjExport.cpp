#include "ObjExport.h"
#include "../core/Document.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Trsf.hxx>

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

namespace {

// One body's indexed mesh: bit-exact vertex dedupe (Poly_Triangulation nodes
// on a face are already shared; across faces the seam coordinates come from
// the same TShape vertices, so exact comparison stitches them).
struct IndexedMesh {
    std::vector<gp_Pnt> vertices;
    std::vector<std::array<int, 3>> triangles; // 0-based into vertices
};

bool harvest(const TopoDS_Shape& shape, IndexedMesh& out) {
    BRepMesh_IncrementalMesh meshGen(shape, materializr::meshParams(0.01, 0.1, false));
    if (!meshGen.IsDone()) return false;

    std::map<std::tuple<double, double, double>, int> index;
    auto vid = [&](const gp_Pnt& p) {
        auto key = std::make_tuple(p.X(), p.Y(), p.Z());
        auto it = index.find(key);
        if (it != index.end()) return it->second;
        int id = static_cast<int>(out.vertices.size());
        out.vertices.push_back(p);
        index.emplace(key, id);
        return id;
    };

    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& face = TopoDS::Face(ex.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (tri.IsNull()) continue;
        const gp_Trsf& trsf = loc.Transformation();
        const bool moved = !loc.IsIdentity();
        for (int i = 1; i <= tri->NbTriangles(); ++i) {
            int n1, n2, n3;
            tri->Triangle(i).Get(n1, n2, n3);
            if (face.Orientation() == TopAbs_REVERSED) std::swap(n1, n2);
            gp_Pnt p1 = tri->Node(n1), p2 = tri->Node(n2), p3 = tri->Node(n3);
            if (moved) { p1.Transform(trsf); p2.Transform(trsf); p3.Transform(trsf); }
            out.triangles.push_back({vid(p1), vid(p2), vid(p3)});
        }
    }
    return !out.triangles.empty();
}

} // namespace

ExportResult ObjExport::exportFile(const std::string& filePath, const Document& doc) {
    ExportResult result;
    try {
        OCC_CATCH_SIGNALS

        // Y-up scene → Z-up file: the proper rotation StlExport uses (a bare
        // axis swap would mirror the part).
        gp_Trsf yUpToZUp;
        yUpToZUp.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)),
                             M_PI * 0.5);

        std::FILE* f = std::fopen(filePath.c_str(), "wb");
        if (!f) {
            result.errorMessage = "Could not open file for writing: " + filePath;
            return result;
        }
        std::fprintf(f, "# Materializr OBJ export (millimeters, Z-up)\n");

        int emitted = 0;
        long vertexOffset = 1; // OBJ indices are 1-based and file-global
        for (int id : doc.getAllBodyIds()) {
            if (!doc.isBodyVisible(id)) continue;
            const TopoDS_Shape& body = doc.getBody(id);
            if (body.IsNull()) continue;
            TopoDS_Shape shape = body;
            try {
                BRepBuilderAPI_Transform xf(body, yUpToZUp, true);
                if (xf.IsDone() && !xf.Shape().IsNull()) shape = xf.Shape();
            } catch (...) {}

            IndexedMesh mesh;
            if (!harvest(shape, mesh)) continue;

            std::string name = doc.getBodyName(id);
            if (name.empty()) name = "Body_" + std::to_string(id);
            for (char& ch : name)
                if (ch == ' ' || ch == '\t') ch = '_'; // OBJ names are one token

            std::fprintf(f, "o %s\n", name.c_str());
            for (const gp_Pnt& p : mesh.vertices)
                std::fprintf(f, "v %.6f %.6f %.6f\n", p.X(), p.Y(), p.Z());
            for (const auto& t : mesh.triangles)
                std::fprintf(f, "f %ld %ld %ld\n",
                             vertexOffset + t[0], vertexOffset + t[1],
                             vertexOffset + t[2]);
            vertexOffset += static_cast<long>(mesh.vertices.size());
            ++emitted;
        }
        std::fclose(f);

        if (emitted == 0) {
            result.errorMessage = "No visible bodies to export.";
            std::remove(filePath.c_str());
            return result;
        }
        result.success = true;
        return result;
    } catch (const Standard_Failure& e) {
        result.errorMessage = std::string("OCCT error during OBJ export: ") +
                              (e.GetMessageString() ? e.GetMessageString() : "unknown");
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = std::string("OBJ export failed: ") + e.what();
        return result;
    }
}

} // namespace materializr
