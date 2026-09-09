#pragma once
#include <string>

namespace materializr {

class Sketch;

struct DxfImportResult {
    bool success = false;
    std::string errorMessage;
    int entityCount = 0;   // entities stamped into the sketch
    int skippedCount = 0;  // recognized-but-unsupported entities (TEXT, DIM, …)
};

// Import mirror of DxfExport (#47): stamp a DXF's profile entities into a
// sketch - LINE, CIRCLE, ARC, LWPOLYLINE/POLYLINE (bulge segments become
// arcs), SPLINE (de Boor-sampled polyline), ELLIPSE (sampled). This is the
// subset laser-cutter / CNC profile files actually contain; everything else
// is counted in skippedCount rather than failing the import.
//
// DXF is dimensioned, so scale is preserved exactly - $INSUNITS converts to
// millimeters (inch files come in at 25.4×) - and the geometry is only
// TRANSLATED so its bounding-box centre lands on the sketch origin (drawings
// often live thousands of units from their own origin).
class DxfImport {
public:
    static DxfImportResult importFile(const std::string& filePath, Sketch& sketch);
};

} // namespace materializr
