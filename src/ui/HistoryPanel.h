#pragma once
#include <set>
#include <string>

class History;
class Document;

namespace materializr {
class EventBus;
}

namespace materializr {

class HistoryPanel {
public:
    HistoryPanel();

    void setHistory(History* history);
    void setDocument(Document* doc);
    void setEventBus(EventBus* bus) { m_eventBus = bus; }

    // Lock history mutation (undo/redo buttons) while a live tool preview
    // owns the top of the history - an outside undo during a preview pops
    // the preview op, and the preview's next frame then pops the user's
    // last COMMITTED step (which the following push erases for good).
    void setHistoryLocked(bool locked) { m_historyLocked = locked; }

    // Render the panel. Returns true if history was modified (undo/redo/edit).
    bool render();
    // Panel body without the "History" window wrapper - for hosting inside
    // another container (im-touch shell right panel). Same return contract.
    bool renderContent();
    // Hide the bottom Undo/Redo button row and show the step counter inline
    // beside the "Operation History" label instead. The im-touch shell hosts
    // undo/redo in its top bar, so the row was redundant there (and its
    // clipped remnant at the panel split looked broken). render() - the
    // desktop window - resets this to true each frame.
    void setShowUndoRedo(bool show) { m_showUndoRedo = show; }

    // Open a given step in the inline editor (e.g. when the user clicks the face
    // a fillet/chamfer produced). -1 closes the editor.
    void setEditingStep(int step) { m_editingStep = step; m_showProperties = (step >= 0); }
    int getEditingStep() const { return m_editingStep; }

    // Soft-highlight a step (distinct from editing) so the list shows which step
    // owns the sketch element currently selected in the viewport. -1 = none.
    void setHighlightStep(int step) { m_highlightStep = step; }
    int getHighlightStep() const { return m_highlightStep; }

    // The step row the cursor is currently over (-1 if none), refreshed each
    // render. Drives the viewport's hover-preview highlight of an op's
    // geometry; the pinned/edited step (getEditingStep) drives the persistent
    // one.
    int getHoveredStep() const { return m_hoveredStep; }

private:
    History* m_history = nullptr;
    bool m_historyLocked = false;
    bool m_showUndoRedo = true;   // see setShowUndoRedo
    Document* m_document = nullptr;
    materializr::EventBus* m_eventBus = nullptr;
    int m_editingStep = -1;
    int m_highlightStep = -1; // step owning the viewport-selected sketch element
    int m_hoveredStep = -1;   // step row under the cursor this frame (hover preview)
    int m_enableFailStep = -1; // re-enabled step that still can't compute (#54)
    bool m_showProperties = false;
    // Measured height of the inline step-properties content last frame, so the
    // block sizes to the op's fields (a two-distance chamfer needs more than a
    // plain extrude) instead of a fixed box that scrolls when a selection is
    // field-heavy. Capped when rendered so it can't swallow the step list.
    float m_stepPropsH = 0.0f;
    // Pre-edit params of the step being edited: renderProperties binds the
    // input fields STRAIGHT to op members, so after a failed Apply the typed
    // (rejected) value would silently stick - restore this blob instead.
    int m_paramsSnapStep = -1;
    std::string m_paramsSnap;
    bool m_deleteConflict = false; // last delete was blocked by a dependent step
    // Steps with same typeId in a row collapse into a single expandable group
    // header. The set holds the START step index of each group the user has
    // currently collapsed. Groups default to expanded; the user clicks ▼ / ▶
    // to toggle. Keyed by start index - adding / deleting steps shifts the
    // run and the saved state effectively resets, which is acceptable.
    std::set<int> m_collapsedGroupStarts;
    // Set of group start indices we've already auto-classified once. Lets us
    // give historical (non-today) date buckets a collapsed default the first
    // frame they appear, while letting the user override afterwards.
    std::set<int> m_seenGroupStarts;
};

} // namespace materializr
