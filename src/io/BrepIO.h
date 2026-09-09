#pragma once
#include <string>
#include "StepIO.h" // For ImportResult and ExportResult

class Document;

namespace materializr {

// OCCT's native BREP format (ASCII, BRepTools::Write/Read) - the exact,
// lossless exchange path with FreeCAD and anything else OCCT-based. Same
// disk-axis convention as STEP: files are Z-up, the scene is Y-up.
class BrepIO {
public:
    // Import a .brep file, adding its solids/shells as bodies.
    static ImportResult import(const std::string& filePath, Document& doc);

    // Export all bodies (as a compound when there's more than one).
    static ExportResult exportFile(const std::string& filePath, const Document& doc);
};

} // namespace materializr
