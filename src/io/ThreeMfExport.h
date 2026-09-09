#pragma once
#include <string>
#include "StepIO.h" // ExportResult

class Document;

namespace materializr {

// 3MF export of the visible bodies - the modern 3D-print interchange format:
// unambiguous millimeter units, one named object per body, indexed manifold
// meshes. The container is a ZIP with STORED entries written directly (CRC32
// from the zlib we already link for project files - no new dependency).
class ThreeMfExport {
public:
    static ExportResult exportFile(const std::string& filePath, const Document& doc);
};

} // namespace materializr
