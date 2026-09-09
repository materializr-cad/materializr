#pragma once
#include <string>
#include "StepIO.h" // ExportResult

class Document;

namespace materializr {

// Wavefront OBJ export of the visible bodies - the broadest mesh-interchange
// format (Blender & every DCC tool). One `o` group per body with the body's
// name, shared vertex/normal pool with running offsets, print-quality
// tessellation, and the same Z-up disk convention as the STL exporter.
class ObjExport {
public:
    static ExportResult exportFile(const std::string& filePath, const Document& doc);
};

} // namespace materializr
