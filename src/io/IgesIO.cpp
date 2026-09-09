#include "IgesIO.h"
#include "../core/Document.h"

#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Compound.hxx>
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <gp_Ax1.hxx>
#include <Interface_Static.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Standard_Failure.hxx>
#include <Standard_ErrorHandler.hxx>

#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

ImportResult IgesIO::import(const std::string& filePath, Document& doc) {
    ImportResult result;

    // OCCT can throw Standard_Failure / std::bad_alloc on malformed or adversarial
    // IGES geometry (during TransferRoots / shape handling). Catch it so a bad
    // import is a graceful error rather than an uncaught exception that aborts the
    // whole process (an instant crash, notably on Android) - mirrors StepIO::import.
    try {
    OCC_CATCH_SIGNALS // convert an OCCT kernel fault on a crafted file into the catch below
    IGESControl_Reader reader;
    IFSelect_ReturnStatus status = reader.ReadFile(filePath.c_str());

    if (status != IFSelect_RetDone) {
        result.errorMessage = "Failed to read IGES file: " + filePath;
        switch (status) {
            case IFSelect_RetError:
                result.errorMessage += " (read error)";
                break;
            case IFSelect_RetFail:
                result.errorMessage += " (read failure)";
                break;
            case IFSelect_RetVoid:
                result.errorMessage += " (file is empty)";
                break;
            default:
                result.errorMessage += " (unknown error)";
                break;
        }
        return result;
    }

    // Bound the work before the (synchronous, UI-thread) transfer: a crafted file
    // with a huge root/entity count would otherwise drive unbounded allocation and
    // time, and the per-sub-shape importCount could in theory overflow. 100k is far
    // beyond any real IGES model.
    const Standard_Integer kMaxEntities = 100000;
    if (reader.NbRootsForTransfer() > kMaxEntities) {
        result.errorMessage = "IGES file has too many entities to import";
        return result;
    }

    // Transfer all roots
    Standard_Integer nbRoots = reader.TransferRoots();
    if (nbRoots == 0) {
        result.errorMessage = "No shapes found in IGES file.";
        return result;
    }

    int importCount = 0;

    // Disk Z-up → scene Y-up, the StepIO convention. IGES skipped this for
    // years ("i don't think anyone even noticed it was sideways" - Steve),
    // which left IGES imports lying down relative to STEP imports (#46).
    gp_Trsf zUpToYUp;
    zUpToYUp.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)),
                         -M_PI * 0.5);

    // Iterate over transferred shapes
    for (Standard_Integer i = 1; i <= reader.NbShapes(); ++i) {
        if (importCount >= kMaxEntities) {
            result.errorMessage = "IGES file produced too many bodies to import";
            return result;
        }
        TopoDS_Shape shape = reader.Shape(i);
        try {
            BRepBuilderAPI_Transform xf(shape, zUpToYUp, /*copy=*/true);
            if (xf.IsDone() && !xf.Shape().IsNull()) shape = xf.Shape();
        } catch (...) {}

        // Explore for solids first
        bool foundSolids = false;
        for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
            const TopoDS_Solid& solid = TopoDS::Solid(explorer.Current());
            ++importCount;
            std::string name = "Imported_" + std::to_string(importCount);
            doc.addBody(solid, name);
            foundSolids = true;
        }

        // If no solids, try shells
        if (!foundSolids) {
            bool foundShells = false;
            for (TopExp_Explorer explorer(shape, TopAbs_SHELL); explorer.More(); explorer.Next()) {
                const TopoDS_Shell& shell = TopoDS::Shell(explorer.Current());
                ++importCount;
                std::string name = "Imported_" + std::to_string(importCount);
                doc.addBody(shell, name);
                foundShells = true;
            }

            // If no shells, try faces
            if (!foundShells) {
                for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
                    const TopoDS_Face& face = TopoDS::Face(explorer.Current());
                    ++importCount;
                    std::string name = "Imported_" + std::to_string(importCount);
                    doc.addBody(face, name);
                }
            }
        }
    }

    if (importCount == 0) {
        // If no specific sub-shapes found, add the top-level shapes directly
        for (Standard_Integer i = 1; i <= reader.NbShapes(); ++i) {
            TopoDS_Shape shape = reader.Shape(i);
            if (!shape.IsNull()) {
                ++importCount;
                std::string name = "Imported_" + std::to_string(importCount);
                doc.addBody(shape, name);
            }
        }
    }

    result.success = true;
    result.bodiesImported = importCount;
    return result;
    } catch (const Standard_Failure& e) {
        result.success = false;
        result.errorMessage = std::string("IGES import failed: ") + e.GetMessageString();
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = std::string("IGES import failed: ") + e.what();
    } catch (...) {
        result.success = false;
        result.errorMessage = "IGES import failed: unrecognized error";
    }
    return result;
}

ExportResult IgesIO::exportFile(const std::string& filePath, const Document& doc) {
    ExportResult result;

    std::vector<int> allIds = doc.getAllBodyIds();
    if (allIds.empty()) {
        result.errorMessage = "No bodies to export.";
        return result;
    }

    IGESControl_Writer writer;

    // Scene Y-up → disk Z-up (+90° about X), matching StepIO::exportBodies -
    // see the import-side note (#46).
    gp_Trsf yUpToZUp;
    yUpToZUp.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)),
                         M_PI * 0.5);

    int bodyCount = 0;
    for (int id : allIds) {
        if (doc.isBodyVisible(id)) {
            try {
                const TopoDS_Shape& shape = doc.getBody(id);
                if (!shape.IsNull()) {
                    TopoDS_Shape outShape = shape;
                    try {
                        BRepBuilderAPI_Transform xf(shape, yUpToZUp, /*copy=*/true);
                        if (xf.IsDone() && !xf.Shape().IsNull()) outShape = xf.Shape();
                    } catch (...) {}
                    writer.AddShape(outShape);
                    ++bodyCount;
                }
            } catch (const std::exception& e) {
                result.errorMessage = "Error accessing body ID " + std::to_string(id) +
                                      ": " + e.what();
                return result;
            }
        }
    }

    if (bodyCount == 0) {
        result.errorMessage = "No visible bodies to export.";
        return result;
    }

    writer.ComputeModel();

    if (!writer.Write(filePath.c_str())) {
        result.errorMessage = "Failed to write IGES file: " + filePath;
        return result;
    }

    result.success = true;
    return result;
}

} // namespace materializr
