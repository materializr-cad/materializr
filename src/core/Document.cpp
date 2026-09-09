#include "Document.h"
#include "EventBus.h"
#include "Events.h"
#include "../modeling/Sketch.h"
#include "ThrowTrace.h"
#include <gp_Ax3.hxx>
#include <algorithm>
#include <stdexcept>
#include <cstdio>

Document::Document() = default;
Document::~Document() = default;

int Document::addBody(const TopoDS_Shape& shape, const std::string& name) {
    BodyEntry entry;
    entry.id = m_nextBodyId++;
    entry.name = name.empty() ? ("Body " + std::to_string(entry.id)) : name;
    entry.shape = shape;
    entry.visible = true;
    m_bodies.push_back(std::move(entry));
    if (m_eventBus) m_eventBus->publish(materializr::DocumentModifiedEvent{true});
    return m_bodies.back().id;
}

void Document::addOrPutBody(int& id, const TopoDS_Shape& shape, const std::string& name) {
    if (id < 0) {
        id = addBody(shape, name);
    } else {
        // Reuse the prior id. putBody picks up the tombstone-stashed metadata
        // from removeBody, so folderId / colour / visibility / name persist
        // across undo/redo cycles.
        putBody(id, shape, name);
    }
}

void Document::removeBody(int id) {
    int idx = findBodyIndex(id);
    if (idx >= 0) {
        // Stash metadata before erasing so a later putBody with the same id
        // (undo/redo path through Extrude / Pattern / Mirror / etc.) can
        // restore the body's folderId, colour, visibility, and name.
        m_bodyTombstones[id] = m_bodies[idx];
        m_bodyLedgers.erase(id);
        m_bodyFaceIds.erase(id);
        m_bodies.erase(m_bodies.begin() + idx);
        if (m_eventBus) {
            // BodyRemovedEvent FIRST so the renderer drops the slot before
            // any DocumentModifiedEvent-triggered logic queries the scene;
            // banding fix from the push/pull preview-undo loop relies on
            // this ordering (see Events.h BodyRemovedEvent docs).
            m_eventBus->publish(materializr::BodyRemovedEvent{id});
            m_eventBus->publish(materializr::DocumentModifiedEvent{true});
        }
    }
}

void Document::updateBody(int id, const TopoDS_Shape& shape) {
    m_bodyLedgers.erase(id);  // producing op re-publishes after
    m_bodyFaceIds.erase(id);  // same lifecycle (stale lineage is worse than none)
    int idx = findBodyIndex(id);
    if (idx >= 0) {
        m_bodies[idx].shape = shape;
        if (m_eventBus) m_eventBus->publish(materializr::DocumentModifiedEvent{true});
    } else {
        std::fprintf(stderr, "[doc] updateBody id=%d NOT FOUND\n", id);
    }
}

void Document::putBody(int id, const TopoDS_Shape& shape, const std::string& name) {
    // Defensive: a bad bodyId leaking in through ReplayOp/ProjectIO would
    // sit in m_bodies forever and crash later getBody calls. Reject up
    // front. id==-1 specifically came up when a stale TransformOp targeting
    // a non-existent body was serialized into a project's history.
    if (id < 0) {
        std::fprintf(stderr,
                     "[doc] putBody id=%d - rejected (negative id).\n", id);
        return;
    }
    int idx = findBodyIndex(id);
    if (idx >= 0) {
        m_bodies[idx].shape = shape;
        if (!name.empty()) m_bodies[idx].name = name;
    } else {
        // If this id was previously removed (typical undo/redo through an op
        // that recreates a body), pull its metadata back from the tombstone
        // so folderId / colour / visibility / name aren't silently lost.
        auto tomb = m_bodyTombstones.find(id);
        if (tomb != m_bodyTombstones.end()) {
            BodyEntry entry = tomb->second;
            entry.id = id;
            entry.shape = shape;
            if (!name.empty()) entry.name = name;
            m_bodies.push_back(std::move(entry));
            m_bodyTombstones.erase(tomb);
        } else {
            BodyEntry entry;
            entry.id = id;
            entry.name = name.empty() ? ("Body " + std::to_string(id)) : name;
            entry.shape = shape;
            entry.visible = true;
            m_bodies.push_back(std::move(entry));
        }
    }
    if (id >= m_nextBodyId) m_nextBodyId = id + 1;
    if (m_eventBus) m_eventBus->publish(materializr::DocumentModifiedEvent{true});
}

const TopoDS_Shape& Document::getBody(int id) const {
    int idx = findBodyIndex(id);
    if (idx < 0) {
        // Record where this came from. Most callers guard this throw on
        // purpose (a body legitimately may be gone), so nothing is printed
        // here - the frame firewall in Application::run() renders the trace
        // only if the throw escapes, which is the case that is always a bug.
        materializr::captureThrowTrace();
        throw std::runtime_error("Body not found: " + std::to_string(id));
    }
    return m_bodies[idx].shape;
}

std::string Document::getBodyName(int id) const {
    int idx = findBodyIndex(id);
    if (idx < 0) {
        return "";
    }
    return m_bodies[idx].name;
}

void Document::setBodyName(int id, const std::string& name) {
    int idx = findBodyIndex(id);
    if (idx >= 0) {
        m_bodies[idx].name = name;
    }
}

void Document::setBodyVisible(int id, bool visible) {
    int idx = findBodyIndex(id);
    if (idx >= 0) {
        m_bodies[idx].visible = visible;
    }
}

bool Document::isBodyVisible(int id) const {
    int idx = findBodyIndex(id);
    if (idx < 0) {
        return false;
    }
    return m_bodies[idx].visible;
}

void Document::setBodyMesh(int id, bool isMesh) {
    int idx = findBodyIndex(id);
    if (idx >= 0) m_bodies[idx].isMesh = isMesh;
}

bool Document::isBodyMesh(int id) const {
    int idx = findBodyIndex(id);
    return idx >= 0 && m_bodies[idx].isMesh;
}

void Document::setBodySheet(int id, const materializr::SheetSpec& spec) {
    int idx = findBodyIndex(id);
    if (idx >= 0) m_bodies[idx].sheet = spec;
}

materializr::SheetSpec Document::getBodySheet(int id) const {
    int idx = findBodyIndex(id);
    return idx >= 0 ? m_bodies[idx].sheet : materializr::SheetSpec{};
}

bool Document::isBodySheet(int id) const {
    int idx = findBodyIndex(id);
    return idx >= 0 && m_bodies[idx].sheet.isSheet;
}

glm::vec3 Document::getBodyColor(int id) const {
    int idx = findBodyIndex(id);
    if (idx < 0) {
        return glm::vec3(0.80f, 0.80f, 0.82f);
    }
    return m_bodies[idx].color;
}

void Document::setBodyColor(int id, const glm::vec3& color) {
    int idx = findBodyIndex(id);
    if (idx >= 0) {
        m_bodies[idx].color = color;
    }
}

std::vector<int> Document::getAllBodyIds() const {
    std::vector<int> ids;
    ids.reserve(m_bodies.size());
    for (const auto& body : m_bodies) {
        ids.push_back(body.id);
    }
    return ids;
}

int Document::addSketch(std::shared_ptr<materializr::Sketch> sketch, const std::string& name) {
    SketchEntry entry;
    entry.id = m_nextSketchId++;
    entry.name = name.empty() ? ("Sketch " + std::to_string(entry.id)) : name;
    entry.sketch = std::move(sketch);
    entry.visible = true;
    m_sketches.push_back(std::move(entry));
    return m_sketches.back().id;
}

void Document::putSketch(int id, std::shared_ptr<materializr::Sketch> sketch,
                         const std::string& name) {
    if (id < 0) return;
    int idx = findSketchIndex(id);
    if (idx >= 0) {
        m_sketches[idx].sketch = std::move(sketch);
        if (!name.empty()) m_sketches[idx].name = name;
    } else {
        SketchEntry entry;
        entry.id = id;
        entry.name = name.empty() ? ("Sketch " + std::to_string(id)) : name;
        entry.sketch = std::move(sketch);
        entry.visible = true;
        m_sketches.push_back(std::move(entry));
    }
    // Keep freshly-assigned ids from colliding with a preserved (loaded) one.
    if (id >= m_nextSketchId) m_nextSketchId = id + 1;
}

void Document::removeSketch(int id) {
    int idx = findSketchIndex(id);
    if (idx >= 0) {
        m_sketches.erase(m_sketches.begin() + idx);
    }
}

std::shared_ptr<materializr::Sketch> Document::getSketch(int id) const {
    int idx = findSketchIndex(id);
    if (idx < 0) return nullptr;
    return m_sketches[idx].sketch;
}

std::string Document::getSketchName(int id) const {
    int idx = findSketchIndex(id);
    if (idx < 0) return "";
    return m_sketches[idx].name;
}

void Document::setSketchName(int id, const std::string& name) {
    int idx = findSketchIndex(id);
    if (idx >= 0) m_sketches[idx].name = name;
}

void Document::setSketchVisible(int id, bool visible) {
    int idx = findSketchIndex(id);
    if (idx >= 0) m_sketches[idx].visible = visible;
}

bool Document::isSketchVisible(int id) const {
    int idx = findSketchIndex(id);
    if (idx < 0) return false;
    return m_sketches[idx].visible;
}

std::vector<int> Document::getAllSketchIds() const {
    std::vector<int> ids;
    ids.reserve(m_sketches.size());
    for (const auto& s : m_sketches) ids.push_back(s.id);
    return ids;
}

int Document::sketchCount() const {
    return static_cast<int>(m_sketches.size());
}

int Document::findSketchId(const materializr::Sketch* sk) const {
    if (!sk) return -1;
    for (const auto& e : m_sketches) {
        if (e.sketch.get() == sk) return e.id;
    }
    return -1;
}

int Document::findSketchIndex(int id) const {
    for (int i = 0; i < static_cast<int>(m_sketches.size()); ++i) {
        if (m_sketches[i].id == id) return i;
    }
    return -1;
}

int Document::addPlane(const gp_Pln& plane, const std::string& name,
                       int reuseId) {
    PlaneEntry entry;
    if (reuseId >= 0 && !getPlane(reuseId)) {
        entry.id = reuseId;
        if (reuseId >= m_nextPlaneId) m_nextPlaneId = reuseId + 1;
    } else {
        entry.id = m_nextPlaneId++;
    }
    entry.name = name.empty() ? ("Plane " + std::to_string(entry.id)) : name;
    entry.plane = plane;
    m_planes.push_back(std::move(entry));
    int id = m_planes.back().id;
    if (m_eventBus) {
        m_eventBus->publish(materializr::PlaneAddedEvent{id});
        m_eventBus->publish(materializr::DocumentModifiedEvent{true});
    }
    return id;
}

void Document::setPlane(int id, const gp_Pln& plane) {
    for (auto& p : m_planes) {
        if (p.id == id) {
            p.plane = plane;
            if (m_eventBus) m_eventBus->publish(materializr::PlaneChangedEvent{id});
            return;
        }
    }
}

void Document::flipPlaneNormal(int id) {
    for (auto& p : m_planes) {
        if (p.id == id) {
            // ZReverse flips the gp_Ax3's main (Z = normal) direction while
            // keeping its location and X direction, so the in-plane U axis is
            // preserved and only the facing (and V) flips.
            gp_Ax3 pos = p.plane.Position();
            pos.ZReverse();
            p.plane = gp_Pln(pos);
            if (m_eventBus) m_eventBus->publish(materializr::PlaneChangedEvent{id});
            return;
        }
    }
}

void Document::removePlane(int id) {
    for (auto it = m_planes.begin(); it != m_planes.end(); ++it) {
        if (it->id == id) {
            m_planes.erase(it);
            // A hosted reference image can't outlive its plane - the plane IS
            // its pose/selection/visibility. Drop it silently (the
            // PlaneRemovedEvent below is what the image renderer watches).
            removeRefImage(id);
            if (m_eventBus) {
                m_eventBus->publish(materializr::PlaneRemovedEvent{id});
                m_eventBus->publish(materializr::DocumentModifiedEvent{true});
            }
            return;
        }
    }
}

// ─── Reference images (hosted on construction planes) ──────────────────────

void Document::setRefImage(int planeId, RefImageEntry entry) {
    entry.planeId = planeId;
    for (auto& r : m_refImages) {
        if (r.planeId == planeId) {
            r = std::move(entry);
            if (m_eventBus) {
                m_eventBus->publish(materializr::PlaneChangedEvent{planeId});
                m_eventBus->publish(materializr::DocumentModifiedEvent{true});
            }
            return;
        }
    }
    m_refImages.push_back(std::move(entry));
    if (m_eventBus) {
        m_eventBus->publish(materializr::PlaneChangedEvent{planeId});
        m_eventBus->publish(materializr::DocumentModifiedEvent{true});
    }
}

const RefImageEntry* Document::getRefImage(int planeId) const {
    for (const auto& r : m_refImages)
        if (r.planeId == planeId) return &r;
    return nullptr;
}

void Document::removeRefImage(int planeId) {
    for (auto it = m_refImages.begin(); it != m_refImages.end(); ++it) {
        if (it->planeId == planeId) {
            m_refImages.erase(it);
            if (m_eventBus) {
                m_eventBus->publish(materializr::PlaneChangedEvent{planeId});
                m_eventBus->publish(materializr::DocumentModifiedEvent{true});
            }
            return;
        }
    }
}

void Document::setRefImageWidthMM(int planeId, double widthMM) {
    for (auto& r : m_refImages) {
        if (r.planeId == planeId) {
            r.widthMM = widthMM;
            if (m_eventBus) {
                m_eventBus->publish(materializr::PlaneChangedEvent{planeId});
                m_eventBus->publish(materializr::DocumentModifiedEvent{true});
            }
            return;
        }
    }
}

void Document::setRefImageOpacity(int planeId, float opacity) {
    for (auto& r : m_refImages) {
        if (r.planeId == planeId) {
            r.opacity = opacity;
            if (m_eventBus)
                m_eventBus->publish(materializr::PlaneChangedEvent{planeId});
            return;
        }
    }
}

std::vector<int> Document::getAllRefImagePlaneIds() const {
    std::vector<int> ids;
    ids.reserve(m_refImages.size());
    for (const auto& r : m_refImages) ids.push_back(r.planeId);
    return ids;
}

const PlaneEntry* Document::getPlane(int id) const {
    for (const auto& p : m_planes) if (p.id == id) return &p;
    return nullptr;
}

std::string Document::getPlaneName(int id) const {
    const PlaneEntry* p = getPlane(id);
    return p ? p->name : std::string();
}

void Document::setPlaneName(int id, const std::string& name) {
    for (auto& p : m_planes) {
        if (p.id == id) {
            p.name = name;
            if (m_eventBus) m_eventBus->publish(materializr::PlaneChangedEvent{id});
            return;
        }
    }
}

void Document::setPlaneVisible(int id, bool visible) {
    for (auto& p : m_planes) {
        if (p.id == id) {
            p.visible = visible;
            if (m_eventBus) {
                m_eventBus->publish(materializr::PlaneChangedEvent{id});
                m_eventBus->publish(materializr::DocumentModifiedEvent{true});
            }
            return;
        }
    }
}

bool Document::isPlaneVisible(int id) const {
    const PlaneEntry* p = getPlane(id);
    return p ? p->visible : false;
}

std::vector<int> Document::getAllPlaneIds() const {
    std::vector<int> ids;
    ids.reserve(m_planes.size());
    for (const auto& p : m_planes) ids.push_back(p.id);
    return ids;
}

int Document::planeCount() const {
    return static_cast<int>(m_planes.size());
}

// ───── Construction axes ─────────────────────────────────────────────────
// Same event-driven shape as planes: add/remove fire Added/Removed,
// set* fire Changed. The render pass + Items panel listen and resync.

int Document::addAxis(const gp_Pnt& origin, const gp_Dir& direction,
                      const std::string& name, int reuseId) {
    AxisEntry entry;
    if (reuseId >= 0 && !getAxis(reuseId)) {
        entry.id = reuseId;
        if (reuseId >= m_nextAxisId) m_nextAxisId = reuseId + 1;
    } else {
        entry.id = m_nextAxisId++;
    }
    entry.name = name.empty() ? ("Axis " + std::to_string(entry.id)) : name;
    entry.origin = origin;
    entry.direction = direction;
    m_axes.push_back(std::move(entry));
    int id = m_axes.back().id;
    if (m_eventBus) {
        m_eventBus->publish(materializr::AxisAddedEvent{id});
        m_eventBus->publish(materializr::DocumentModifiedEvent{true});
    }
    return id;
}

void Document::removeAxis(int id) {
    for (auto it = m_axes.begin(); it != m_axes.end(); ++it) {
        if (it->id == id) {
            m_axes.erase(it);
            if (m_eventBus) {
                m_eventBus->publish(materializr::AxisRemovedEvent{id});
                m_eventBus->publish(materializr::DocumentModifiedEvent{true});
            }
            return;
        }
    }
}

void Document::setAxis(int id, const gp_Pnt& origin, const gp_Dir& direction) {
    for (auto& a : m_axes) {
        if (a.id == id) {
            a.origin = origin;
            a.direction = direction;
            if (m_eventBus) m_eventBus->publish(materializr::AxisChangedEvent{id});
            return;
        }
    }
}

void Document::flipAxisDirection(int id) {
    for (auto& a : m_axes) {
        if (a.id == id) {
            a.direction.Reverse();
            if (m_eventBus) m_eventBus->publish(materializr::AxisChangedEvent{id});
            return;
        }
    }
}

const AxisEntry* Document::getAxis(int id) const {
    for (const auto& a : m_axes) if (a.id == id) return &a;
    return nullptr;
}

std::string Document::getAxisName(int id) const {
    const AxisEntry* a = getAxis(id);
    return a ? a->name : std::string();
}

void Document::setAxisName(int id, const std::string& name) {
    for (auto& a : m_axes) {
        if (a.id == id) {
            a.name = name;
            if (m_eventBus) m_eventBus->publish(materializr::AxisChangedEvent{id});
            return;
        }
    }
}

void Document::setAxisVisible(int id, bool visible) {
    for (auto& a : m_axes) {
        if (a.id == id) {
            a.visible = visible;
            if (m_eventBus) {
                m_eventBus->publish(materializr::AxisChangedEvent{id});
                m_eventBus->publish(materializr::DocumentModifiedEvent{true});
            }
            return;
        }
    }
}

bool Document::isAxisVisible(int id) const {
    const AxisEntry* a = getAxis(id);
    return a ? a->visible : false;
}

std::vector<int> Document::getAllAxisIds() const {
    std::vector<int> ids;
    ids.reserve(m_axes.size());
    for (const auto& a : m_axes) ids.push_back(a.id);
    return ids;
}

int Document::axisCount() const {
    return static_cast<int>(m_axes.size());
}

void Document::clear() {
    // Announce each entity's removal before wiping the lists. Subscribers keep
    // caches keyed off these events - the plugin Plane/Axis renderers only
    // rebuild when a removal flips their dirty flag, and the TShape-keyed
    // selection caches evict on BodyRemovedEvent. Clearing silently left ghost
    // construction planes/axes rendered (but unclickable) after File → Close
    // Project.
    if (m_eventBus) {
        for (const auto& b : m_bodies)
            m_eventBus->publish(materializr::BodyRemovedEvent{b.id});
        for (const auto& p : m_planes)
            m_eventBus->publish(materializr::PlaneRemovedEvent{p.id});
        for (const auto& a : m_axes)
            m_eventBus->publish(materializr::AxisRemovedEvent{a.id});
    }
    m_bodies.clear();
    m_planes.clear();
    m_refImages.clear();
    m_axes.clear();
    m_sketches.clear();
    m_folders.clear();
    m_bodyTombstones.clear();
    m_bodyLedgers.clear();
    m_bodyFaceIds.clear();
    m_nextFaceId = 1;
    m_nextBodyId = 1;
    m_nextPlaneId = 1;
    m_nextAxisId = 1;
    m_nextSketchId = 1;
    m_nextFolderId = 1;
}

// ---- Folders ---------------------------------------------------------------

int Document::addFolder(const std::string& name) {
    FolderEntry entry;
    entry.id = m_nextFolderId++;
    entry.name = name.empty() ? ("Folder " + std::to_string(entry.id)) : name;
    m_folders.push_back(std::move(entry));
    if (m_eventBus) m_eventBus->publish(materializr::DocumentModifiedEvent{true});
    return m_folders.back().id;
}

void Document::removeFolder(int folderId) {
    int idx = findFolderIndex(folderId);
    if (idx < 0) return;
    // Orphan members back to root, then erase.
    for (auto& b : m_bodies) {
        if (b.folderId == folderId) b.folderId = -1;
    }
    m_folders.erase(m_folders.begin() + idx);
    if (m_eventBus) m_eventBus->publish(materializr::DocumentModifiedEvent{true});
}

std::vector<int> Document::getAllFolderIds() const {
    std::vector<int> ids;
    ids.reserve(m_folders.size());
    for (const auto& f : m_folders) ids.push_back(f.id);
    return ids;
}

std::string Document::getFolderName(int folderId) const {
    int idx = findFolderIndex(folderId);
    return idx < 0 ? "" : m_folders[idx].name;
}

void Document::setFolderName(int folderId, const std::string& name) {
    int idx = findFolderIndex(folderId);
    if (idx >= 0) m_folders[idx].name = name;
}

bool Document::isFolderVisible(int folderId) const {
    int idx = findFolderIndex(folderId);
    return idx < 0 ? false : m_folders[idx].visible;
}

void Document::setFolderVisible(int folderId, bool visible) {
    int idx = findFolderIndex(folderId);
    if (idx < 0) return;
    m_folders[idx].visible = visible;
    // Cascade to members.
    for (auto& b : m_bodies) {
        if (b.folderId == folderId) b.visible = visible;
    }
}

glm::vec3 Document::getFolderColor(int folderId) const {
    int idx = findFolderIndex(folderId);
    return idx < 0 ? glm::vec3(0.80f, 0.80f, 0.82f) : m_folders[idx].color;
}

void Document::setFolderColor(int folderId, const glm::vec3& color) {
    int idx = findFolderIndex(folderId);
    if (idx < 0) return;
    m_folders[idx].color = color;
    // Cascade to members - overwrites their colour. Re-customisable per body
    // afterwards (per-body picker still works as before).
    for (auto& b : m_bodies) {
        if (b.folderId == folderId) b.color = color;
    }
}

bool Document::isFolderExpanded(int folderId) const {
    int idx = findFolderIndex(folderId);
    return idx < 0 ? false : m_folders[idx].expanded;
}

void Document::setFolderExpanded(int folderId, bool expanded) {
    int idx = findFolderIndex(folderId);
    if (idx >= 0) m_folders[idx].expanded = expanded;
}

int Document::getBodyFolder(int bodyId) const {
    int idx = findBodyIndex(bodyId);
    return idx < 0 ? -1 : m_bodies[idx].folderId;
}

void Document::setBodyFolder(int bodyId, int folderId) {
    int bidx = findBodyIndex(bodyId);
    if (bidx < 0) return;
    if (folderId >= 0 && findFolderIndex(folderId) < 0) return; // unknown folder
    m_bodies[bidx].folderId = folderId;
}

std::vector<int> Document::getBodiesInFolder(int folderId) const {
    std::vector<int> ids;
    for (const auto& b : m_bodies) {
        if (b.folderId == folderId) ids.push_back(b.id);
    }
    return ids;
}

int Document::findFolderIndex(int id) const {
    for (int i = 0; i < static_cast<int>(m_folders.size()); ++i) {
        if (m_folders[i].id == id) return i;
    }
    return -1;
}

int Document::bodyCount() const {
    return static_cast<int>(m_bodies.size());
}

int Document::findBodyIndex(int id) const {
    for (int i = 0; i < static_cast<int>(m_bodies.size()); ++i) {
        if (m_bodies[i].id == id) {
            return i;
        }
    }
    return -1;
}
