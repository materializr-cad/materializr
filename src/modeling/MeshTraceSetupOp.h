#pragma once
#include "../core/Operation.h"
#include <string>

// "Set Up Tracing Planes" (Application::beginMeshTraceSetup) as a real
// history step: three orthogonal construction planes + empty sketches
// through a mesh body's bbox center, plus hiding the source mesh. Undo
// removes all three planes/sketches and re-shows the mesh; redo re-adds
// them under the SAME ids (so any sketch content placed into them via
// "Place Sketch" afterward - itself NOT a separate history step, same as
// reference-image import - is naturally lost on undo along with the planes
// that carried it, and not restored by redo).
//
// Deliberately does not implement rehydrateFromReload: a reloaded project
// already carries the planes/sketches/mesh-trace links directly (their own
// CPLANE/SKETCH/MESHTRACE blocks in ProjectIO), so this step falls back to
// a baked ReplayOp on reload - the same accepted path every op that hasn't
// opted into full replay-editability takes (see OperationFactory.cpp).
class MeshTraceSetupOp : public Operation {
public:
    void setBodyId(int id) { m_bodyId = id; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Set Up Tracing Planes"; }
    std::string description() const override;
    void renderProperties() override {}
    std::string typeId() const override { return "mesh_trace_setup"; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;

    // Filled by execute() the first time it runs; Application reads these
    // right after pushOperation to know which plane to select/show first.
    int planeId(int index) const { return index >= 0 && index < 3 ? m_planes[index].planeId : -1; }

private:
    struct PlaneRec {
        int planeId = -1;
        int sketchId = -1;
        std::string name;
    };

    int m_bodyId = -1;
    bool m_wasBodyVisible = true;
    PlaneRec m_planes[3];
};
