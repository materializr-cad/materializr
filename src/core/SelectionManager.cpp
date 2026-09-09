#include "SelectionManager.h"
#include "EventBus.h"
#include "Events.h"
#include "../touch_mode.h"
#include <algorithm>

void SelectionManager::publishChanged() {
    // Every mutation funnels through here - the revision counter rides along
    // so per-selection memoizations (see revision()) invalidate exactly when
    // the selection actually changes.
    ++m_revision;
    if (m_eventBus) m_eventBus->publish(materializr::SelectionChangedEvent{});
}

void SelectionManager::clear() {
    m_selection.clear();
    m_navigationOnly = false;
    publishChanged();
}

void SelectionManager::select(const SelectionEntry& entry) {
    m_selection.clear();
    m_selection.push_back(entry);
    m_navigationOnly = false;
    publishChanged();
}

void SelectionManager::addToSelection(const SelectionEntry& entry) {
    if (findEntry(entry) >= 0) return;

    // A Body selection subsumes any Face / Edge / Vertex from the same
    // body - otherwise the user ends up with stale face highlights stuck
    // to the wireframe after a Ctrl+double-click promotes face to body.
    if (entry.type == SelectionType::Body && entry.bodyId >= 0) {
        m_selection.erase(
            std::remove_if(m_selection.begin(), m_selection.end(),
                [&](const SelectionEntry& e) {
                    return e.bodyId == entry.bodyId &&
                           (e.type == SelectionType::Face ||
                            e.type == SelectionType::Edge ||
                            e.type == SelectionType::Vertex);
                }),
            m_selection.end());
    } else if (materializr::touchMode() &&
               (entry.type == SelectionType::Face ||
                entry.type == SelectionType::Edge ||
                entry.type == SelectionType::Vertex) &&
               entry.bodyId >= 0) {
        // Touch only: picking a Face / Edge / Vertex while the whole body is
        // already selected drops the Body in favour of the sub-shape, so a tap
        // gives one unambiguous target to push/pull/fillet. With a mouse the
        // desktop/upstream model keeps both - a body and its sub-shapes coexist
        // in the selection (so an Android device with a mouse/keyboard attached,
        // running with touch mode off, behaves like the desktop).
        m_selection.erase(
            std::remove_if(m_selection.begin(), m_selection.end(),
                [&](const SelectionEntry& e) {
                    return e.bodyId == entry.bodyId &&
                           e.type == SelectionType::Body;
                }),
            m_selection.end());
    }

    m_selection.push_back(entry);
    m_navigationOnly = false;
    publishChanged();
}

void SelectionManager::removeFromSelection(const SelectionEntry& entry) {
    int idx = findEntry(entry);
    if (idx >= 0) {
        m_selection.erase(m_selection.begin() + idx);
        publishChanged();
    }
}

void SelectionManager::toggleSelection(const SelectionEntry& entry) {
    int idx = findEntry(entry);
    if (idx >= 0) {
        m_selection.erase(m_selection.begin() + idx);
    } else {
        m_selection.push_back(entry);
    }
    publishChanged();
}

bool SelectionManager::hasSelection() const {
    return !m_selection.empty();
}

SelectionType SelectionManager::primaryType() const {
    if (m_selection.empty()) {
        return SelectionType::None;
    }
    return m_selection.front().type;
}

const std::vector<SelectionEntry>& SelectionManager::getSelection() const {
    return m_selection;
}

int SelectionManager::selectedBodyCount() const {
    int count = 0;
    for (const auto& entry : m_selection) {
        if (entry.type == SelectionType::Body) {
            count++;
        }
    }
    return count;
}

int SelectionManager::selectedFaceCount() const {
    int count = 0;
    for (const auto& entry : m_selection) {
        if (entry.type == SelectionType::Face) {
            count++;
        }
    }
    return count;
}

int SelectionManager::selectedEdgeCount() const {
    int count = 0;
    for (const auto& entry : m_selection) {
        if (entry.type == SelectionType::Edge) {
            count++;
        }
    }
    return count;
}

int SelectionManager::selectedSketchCount() const {
    int count = 0;
    for (const auto& entry : m_selection) {
        if (entry.type == SelectionType::Sketch) {
            count++;
        }
    }
    return count;
}

int SelectionManager::selectedSketchRegionCount() const {
    int count = 0;
    for (const auto& entry : m_selection) {
        if (entry.type == SelectionType::SketchRegion) {
            count++;
        }
    }
    return count;
}

bool SelectionManager::hasSelectedBodies() const {
    return selectedBodyCount() > 0;
}

bool SelectionManager::hasSelectedFaces() const {
    return selectedFaceCount() > 0;
}

bool SelectionManager::hasSelectedEdges() const {
    return selectedEdgeCount() > 0;
}

bool SelectionManager::hasSelectedSketches() const {
    return selectedSketchCount() > 0;
}

bool SelectionManager::hasSelectedSketchRegions() const {
    return selectedSketchRegionCount() > 0;
}

int SelectionManager::findEntry(const SelectionEntry& entry) const {
    for (int i = 0; i < static_cast<int>(m_selection.size()); ++i) {
        const auto& e = m_selection[i];
        if (e.type != entry.type || e.bodyId != entry.bodyId ||
            e.subShapeIndex != entry.subShapeIndex ||
            e.sketchId != entry.sketchId) {
            continue;
        }
        // Edge picks share subShapeIndex (-1) on the same body - and so would
        // any future selection type that doesn't carry a sub-shape index - so
        // also disambiguate by shape identity when both shapes are present.
        // Otherwise Ctrl+clicking a second edge on the same body would look
        // "already selected" and silently get dropped. (Plane/Axis also lean on
        // this, since findEntry doesn't compare planeId/axisId.)
        // EXCEPT a whole Body, which is uniquely keyed by bodyId: a remesh can
        // replace its stored shape, so an IsSame test there would wrongly read
        // the same body as new and stack duplicate copies in the selection.
        if (entry.type != SelectionType::Body &&
            !e.shape.IsNull() && !entry.shape.IsNull() &&
            !e.shape.IsSame(entry.shape)) {
            continue;
        }
        return i;
    }
    return -1;
}
