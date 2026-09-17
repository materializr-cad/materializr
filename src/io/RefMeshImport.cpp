#include "RefMeshImport.h"

#include <RWStl.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_Triangle.hxx>

#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>

#include <Standard_Failure.hxx>
#include <Standard_ErrorHandler.hxx>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

RefMeshLoadResult RefMeshImport::load(const std::string& filePath) {
    RefMeshLoadResult result;

    try {
    OCC_CATCH_SIGNALS

    Handle(Poly_Triangulation) mesh = RWStl::ReadFile(filePath.c_str());
    if (mesh.IsNull() || mesh->NbTriangles() == 0) {
        result.errorMessage = "Failed to read STL file (empty or unrecognized): " + filePath;
        return result;
    }

    // Same -90° about X as StlIO::import: STL is conventionally Z-up, this
    // app is Y-up.
    gp_Trsf zUpToYUp;
    zUpToYUp.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)),
                         -M_PI * 0.5);

    result.vertices.reserve(static_cast<size_t>(mesh->NbTriangles()) * 3 * 6);
    for (Standard_Integer i = 1; i <= mesh->NbTriangles(); ++i) {
        Standard_Integer n1, n2, n3;
        mesh->Triangle(i).Get(n1, n2, n3);
        gp_Pnt p1 = mesh->Node(n1).Transformed(zUpToYUp);
        gp_Pnt p2 = mesh->Node(n2).Transformed(zUpToYUp);
        gp_Pnt p3 = mesh->Node(n3).Transformed(zUpToYUp);

        gp_Vec e1(p1, p2), e2(p1, p3);
        gp_Vec normal = e1.Crossed(e2);
        if (normal.SquareMagnitude() < 1e-12) continue; // degenerate facet
        normal.Normalize();

        for (const gp_Pnt& p : {p1, p2, p3}) {
            result.vertices.push_back(static_cast<float>(p.X()));
            result.vertices.push_back(static_cast<float>(p.Y()));
            result.vertices.push_back(static_cast<float>(p.Z()));
            result.vertices.push_back(static_cast<float>(normal.X()));
            result.vertices.push_back(static_cast<float>(normal.Y()));
            result.vertices.push_back(static_cast<float>(normal.Z()));
        }
        ++result.triangleCount;
    }

    if (result.triangleCount == 0) {
        result.errorMessage = "STL contained no usable (non-degenerate) triangles: " + filePath;
        return result;
    }

    result.success = true;
    return result;

    } catch (const Standard_Failure& e) {
        result.errorMessage = std::string("STL read failed: ") +
                              (e.GetMessageString() ? e.GetMessageString() : "unknown error");
        return result;
    }
}

} // namespace materializr
