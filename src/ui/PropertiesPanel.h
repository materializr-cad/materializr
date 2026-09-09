#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>

class History;
class Document;
class SelectionManager;

namespace materializr {
class Sketch;
class EventBus;
class SketchTool;
}

namespace materializr {

class PropertiesPanel {
public:
    PropertiesPanel();

    void setHistory(History* history);
    void setDocument(Document* doc);
    void setSelectionManager(const SelectionManager* sel);
    void setEventBus(materializr::EventBus* bus) { m_eventBus = bus; }
    // Per-frame sketch-mode context: while the user is editing a sketch, the
    // panel shows the editable size of the currently-selected element (a
    // circle's diameter, an arc's radius). Injected each frame by Application.
    void setSketchContext(bool inSketchMode, materializr::Sketch* active,
                          int activeSketchId, materializr::SketchTool* tool) {
        m_inSketchMode = inSketchMode;
        m_activeSketch = active;
        m_activeSketchId = activeSketchId;
        m_sketchTool = tool;
    }
    // Routes an element-size edit through Application::recordSketchMutation
    // (snapshot before/after → SketchEditOp) + re-solve + cascade. The panel
    // passes the raw mutation (e.g. setCircleRadius); the host wraps it.
    void setSketchMutateCallback(std::function<void(const std::function<void()>&)> cb) {
        m_sketchMutate = std::move(cb);
    }
    // Routes the plane panel's "Rotate About Axis…" button to Application,
    // which opens the hinge popup for that plane id.
    void setRotatePlaneCallback(std::function<void(int)> cb) { m_rotatePlane = std::move(cb); }
    // Attach / replace a reference image on an EXISTING plane. A reference
    // image is already just a plane plus a picture in the document model, so
    // any plane the user has built -- offset, face-derived, midplane -- can
    // host one, not only the ground plane the import flow makes.
    void setAttachRefImageCallback(std::function<void(int)> cb) {
        m_attachRefImage = std::move(cb);
    }
    // Called for non-history plane mutations (Flip Normal) so the host marks
    // the project dirty.
    void setDirtyCallback(std::function<void()> cb) { m_markDirty = std::move(cb); }
    // Returns a human-readable parametric-link hint for a selected body
    // (isBody=true) or sketch (isBody=false), or "" if there's no link to show.
    // The host (Application) computes it from the sketch→body op graph.
    void setLinkInfoCallback(std::function<std::string(bool, int)> cb) {
        m_linkInfo = std::move(cb);
    }
    // Re-link a detached sketch (or the detached sketch[es] driving a body) back
    // to its body. isBody distinguishes the two selection cases.
    void setRelinkCallback(std::function<void(bool, int)> cb) {
        m_relink = std::move(cb);
    }

    // Set which history step is being edited (-1 for none)
    void setEditingStep(int step);
    int getEditingStep() const;

    // Render. Returns true if a parameter was changed (needs history replay).
    bool render();
    // Body only, no ImGui::Begin/End - for hosts that place it themselves
    // (the im-touch right panel's Props tab). Same return as render().
    bool renderContent();

private:
    // Constraint editor for whichever sketch is currently selected (or for
    // the parent sketch of a selected region). Walks the live sketch's
    // constraints, lets the user retune dimensional ones inline, runs the
    // solver, and pushes a SketchEditOp on commit so the change is
    // undoable and survives save/load. `modified` is set true if a value
    // was committed this frame so the host can dirty its mesh + history.
    void renderSketchConstraintsPanel(int sketchId, bool& modified);
    // While in sketch mode: editable size of the selected element (circle
    // diameter / arc radius), or a read-only readout for a line. Writes go
    // through m_sketchMutate so they're undoable and cascade to bodies.
    void renderSketchElementPanel(bool& modified);
    // Read-only orientation readout + Flip Normal / Rotate-About-Axis actions
    // for a selected construction plane.
    void renderPlanePanel(int planeId, bool& modified);
    // Origin / direction / length readout + Flip Direction for a selected axis.
    void renderAxisPanel(int axisId, bool& modified);

    std::function<void(int)> m_rotatePlane;
    std::function<void(int)> m_attachRefImage;
    std::function<void()>    m_markDirty;

    History* m_history = nullptr;
    Document* m_document = nullptr;
    const SelectionManager* m_selection = nullptr;
    materializr::EventBus* m_eventBus = nullptr;
    int m_editingStep = -1;

    // Sketch-mode context (see setSketchContext).
    bool m_inSketchMode = false;
    materializr::Sketch* m_activeSketch = nullptr;
    int m_activeSketchId = -1;
    materializr::SketchTool* m_sketchTool = nullptr;
    std::function<void(const std::function<void()>&)> m_sketchMutate;
    std::function<std::string(bool, int)> m_linkInfo;
    std::function<void(bool, int)> m_relink;

    // Buffered text for each editable constraint value in the panel above.
    // Keyed by `constraint id`. Wiped when the panel switches to a
    // different sketch so values from the previous sketch don't leak in.
    int m_constraintPanelSketchId = -1;
    struct ConstraintEdit {
        char buf[24] = "0";
        bool focused = false;       // were we already focused-on last frame?
        // Snapshot of the sketch from the frame we first received focus,
        // used as the "before" state when we commit the SketchEditOp.
        std::shared_ptr<materializr::Sketch> beforeSnap;
    };
    std::map<int, ConstraintEdit> m_constraintEdits;

    // Per-body bbox-extent cache for the "Size: X × Y × Z mm" readout.
    // BRepBndLib::AddOptimal walks every face's surface densely (it's the
    // "tight" variant - significantly more expensive than the regular Add)
    // and on a complex NURBS body (airplane fuselage, etc.) runs 80-150ms.
    // Calling it every frame while the panel is open dropped the idle frame
    // rate to ~10 FPS the moment a body was selected. Key on the body's
    // TShape pointer so the cache self-invalidates whenever the topology
    // actually rebuilds (push/pull, fillet, revolve commit, etc.).
    // Stored extents are already in user-Z-up convention.
    std::map<int, std::pair<const void*, std::array<double, 3>>> m_bboxExtentCache;
};

} // namespace materializr
