#pragma once
#include <string>

#include "StepIO.h" // reuse ImportResult

class Document;

namespace materializr {

// STL import. Reads an ASCII or binary STL mesh, optionally decimates it
// (quadric edge collapse), sews the facets into a solid, and merges near-flat
// regions into single planar faces - so the result is selectable and sketchable
// (pick a flat face → "Sketch on Face"), useful for manually re-creating a model
// from a scan/print. The body is tagged isMesh so the viewport takes a
// mesh-aware path (cached picking, optional wireframe).
//
// A single `accuracy` in [0,1] drives the fidelity/cost trade-off:
//   0.0 = coarse/fast - aggressive decimation + wide flat-merge (big faces, snappy)
//   1.0 = faithful/slow - light decimation + tight merge (keeps detail, heavier)
// It maps to: decimation target triangles, UnifySameDomain angular tolerance
// (the flat-merge knob), and sewing tolerance. See StlIO.cpp for the mapping.
//
// Export still lives in StlExport (see StlExportPlugin); this is import only.
class StlIO {
public:
    static constexpr double kDefaultAccuracy = 0.5;

    static ImportResult import(const std::string& filePath, Document& doc,
                               double accuracy = kDefaultAccuracy);
};

} // namespace materializr
