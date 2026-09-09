#pragma once
// Imported meshes are REFERENCE bodies, and modelling operations refuse them.
//
// An STL is a tessellation, not a modelled solid: a real import measured here
// is a 4,881-face solid where the equivalent modelled part has a dozen analytic
// faces. Nothing downstream is built for that. A boolean on two such bodies
// completed in 101 seconds and produced an 8,745-face result - not a failure, a
// success nobody wants: the output is another mesh, with no analytic faces to
// fillet, sketch on, or edit afterwards. Fillet/chamfer/shell/thread are worse
// still, because they look for analytic edges and surfaces that a triangle soup
// simply does not have.
//
// So the rule is: an import is there to measure and trace against. You sketch
// on it (the face references still snap), and you model the replacement
// alongside it. Everything that would rewrite its topology declines, with a
// message saying why rather than freezing for two minutes first.
//
// This is a COMMAND-level gate, deliberately. The ops themselves stay capable:
// a project saved before this could contain a mesh boolean that already
// succeeded, and history replay has to reproduce it faithfully or the file
// changes shape on load.

#include <string>
#include <vector>
#include <cstddef>   // size_t

// Document and SelectionManager live at global scope, not in materializr.
class Document;
class SelectionManager;

namespace materializr {

// Ids among `bodies` that are imported meshes.
std::vector<int> meshBodiesAmong(const Document& doc, const std::vector<int>& bodies);

// Distinct body ids referenced by the current selection (bodies, and the bodies
// that own selected faces/edges), in selection order.
std::vector<int> selectedBodyIds(const SelectionManager& sel);

// The message to show when `opName` is refused. `meshCount` of `total` operands
// were meshes, so an all-mesh selection and a mixed one read differently.
std::string meshRefusalMessage(const char* opName, size_t meshCount, size_t total);

} // namespace materializr
