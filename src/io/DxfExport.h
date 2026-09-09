#pragma once
#include <string>

namespace materializr {

class Sketch;

struct DxfExportResult {
    bool success = false;
    std::string errorMessage;
    int entityCount = 0;
};

class DxfExport {
public:
    // Export a sketch's non-construction geometry as R12 ASCII DXF in
    // millimeters - the format laser cutters and CAM tools expect. Lines,
    // circles and arcs are exact entities; splines are sampled POLYLINEs.
    // Sketch coordinates go out as-is (DXF is Y-up like the sketch plane).
    static DxfExportResult exportSketch(const std::string& filePath, const Sketch& sketch);
};

} // namespace materializr
