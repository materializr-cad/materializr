#pragma once
#include <vector>
#include <TopoDS_Shape.hxx>

namespace materializr { class EventBus; }

enum class SelectionType {
    None,
    Body,
    Face,
    Edge,
    Vertex,
    Sketch,
    SketchRegion,
    Plane,
    Axis
};

struct SelectionEntry {
    SelectionType type = SelectionType::None;
    int bodyId = -1;
    int subShapeIndex = -1; // for face/edge/vertex within a body; region index for SketchRegion
    int sketchId = -1;      // when type == Sketch or SketchRegion
    int planeId = -1;       // when type == Plane (Document::PlaneEntry id)
    int axisId = -1;        // when type == Axis  (Document::AxisEntry id)
    TopoDS_Shape shape;     // the actual selected sub-shape
};

class SelectionManager {
public:
    SelectionManager() = default;
    ~SelectionManager() = default;

    void clear();
    void select(const SelectionEntry& entry);
    void addToSelection(const SelectionEntry& entry);
    void removeFromSelection(const SelectionEntry& entry);
    void toggleSelection(const SelectionEntry& entry);

    bool hasSelection() const;
    SelectionType primaryType() const; // type of first selected item
    const std::vector<SelectionEntry>& getSelection() const;

    int selectedBodyCount() const;
    int selectedFaceCount() const;
    int selectedEdgeCount() const;
    int selectedSketchCount() const;
    int selectedSketchRegionCount() const;

    void setEventBus(materializr::EventBus* bus) { m_eventBus = bus; }

    // For adaptive toolbar
    bool hasSelectedBodies() const;
    bool hasSelectedFaces() const;
    bool hasSelectedEdges() const;
    bool hasSelectedSketches() const;
    bool hasSelectedSketchRegions() const;

    // "Navigation-only" - the current selection was made for highlighting /
    // navigation purposes (e.g. clicking a body name in the Items panel) and
    // should not auto-activate the move/rotate/scale gizmo. Cleared by any
    // mutation through select / addToSelection / clear so a follow-up
    // viewport pick restores the default gizmo-on behaviour; ItemsPanel sets
    // it true again right after each panel-driven select call. Move / Rotate
    // / Scale toolbar actions also clear it (the user explicitly wants the
    // gizmo at that point).
    bool navigationOnly() const { return m_navigationOnly; }
    void setNavigationOnly(bool b) { m_navigationOnly = b; }

    // Monotonic counter bumped by every mutation (select/add/remove/toggle/
    // clear). Lets callers memoize per-selection work - e.g. the toolbar's
    // "Edit Diameter" candidate detection, which was re-running OCCT surface
    // queries every rendered frame while a face was selected.
    unsigned revision() const { return m_revision; }

private:
    int findEntry(const SelectionEntry& entry) const;
    void publishChanged();

    std::vector<SelectionEntry> m_selection;
    materializr::EventBus* m_eventBus = nullptr;
    bool m_navigationOnly = false;
    unsigned m_revision = 0;
};
