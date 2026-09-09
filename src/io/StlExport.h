#pragma once
#include <string>
#include <TopoDS_Shape.hxx>

class Document;

namespace materializr {

struct StlExportOptions {
    // Print-quality tessellation. The old 0.1mm / 0.5rad left visible facet
    // ripples on small fillets (a 90° fillet got ~3 flat steps). 0.01mm chord
    // and ~5.7° between facets keep curved blends smooth at print scale.
    double linearDeflection = 0.01;  // mm - chord deviation (smaller = smoother)
    double angularDeflection = 0.1;  // radians (~5.7°)
    bool binary = true;              // Binary STL (smaller, faster) vs ASCII
};

struct StlExportResult {
    bool success = false;
    std::string errorMessage;
    int triangleCount = 0;
};

class StlExport {
public:
    // Export all visible bodies to a single STL file
    static StlExportResult exportFile(const std::string& filePath, const Document& doc,
                                       const StlExportOptions& options = {});

    // Export a single shape
    static StlExportResult exportShape(const std::string& filePath, const TopoDS_Shape& shape,
                                        const StlExportOptions& options = {});
};

} // namespace materializr
