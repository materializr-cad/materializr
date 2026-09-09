#pragma once
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include "SheetSpec.h"
#include "../modeling/FaceLineage.h"
#include "../modeling/GenerationLedger.h"

namespace materializr { class Sketch; class EventBus;
namespace topo { struct GenerationLedger; } }

struct BodyEntry {
    int id;
    std::string name;
    TopoDS_Shape shape;
    bool visible = true;
    glm::vec3 color = glm::vec3(0.80f, 0.80f, 0.82f); // default: light grey
    // -1 = at the root (not in any folder). >0 = a FolderEntry::id.
    int folderId = -1;
    // An imported tessellated mesh (e.g. STL): the shape is a sewn solid built
    // from many small facets, not analytic CAD geometry. The viewport uses this
    // to take a mesh-aware path (cached picking, optional wireframe). Serialized
    // to project files since it can't be re-derived from the shape - see ProjectIO.
    bool isMesh = false;
    // Set once the user marks this body as a fabrication sheet part (foam board,
    // sheet metal, plywood, …). Drives the unfold/flatten engine. Serialized to
    // project files (can't be re-derived from the shape) - see ProjectIO.
    materializr::SheetSpec sheet;
};

// Bodies can be grouped under a folder for organisation in the Items panel.
// Folder visibility and colour CASCADE to its bodies - toggling the folder
// hides/shows every body inside; setting the colour overwrites every body's
// colour (which can still be re-customised per body afterwards).
struct FolderEntry {
    int id;
    std::string name;
    bool visible = true;
    glm::vec3 color = glm::vec3(0.80f, 0.80f, 0.82f);
    bool expanded = true; // UI-only - collapsed folders hide children in panel
};

struct PlaneEntry {
    int id;
    std::string name;
    gp_Pln plane;
    bool visible = true;
    // Half-size of the rendered translucent quad in mm. Free to grow later
    // for autoscale; 50 mm (= 100 mm square) is a reasonable default that's
    // clearly visible against a typical part without dominating the scene.
    double halfSize = 50.0;
};

// Reference image - a photo carried on a construction plane so a real object
// can be traced without a 3D scanner. 1:1 with a host PlaneEntry (keyed by
// planeId): the plane contributes pose / selection / visibility / gizmo /
// sketch-on-plane; this entry carries the raster payload plus physical size
// and underlay opacity. fileBytes is the ORIGINAL compressed file (png/jpg),
// persisted verbatim into the project so a .materializr stays self-contained;
// decode happens at render time (dims are cached here for aspect math).
struct RefImageEntry {
    int planeId = -1;
    std::vector<unsigned char> fileBytes;
    int pixW = 0, pixH = 0;    // decoded pixel dims (aspect; set at import)
    double widthMM = 100.0;    // physical width; height follows the aspect
    float opacity = 0.6f;      // underlay strength for tracing
};

// Construction axis - a stored ray (origin + unit direction). Used as the
// rotation axis for Revolve (post-0.6) and any other "around a line"
// operation. Same plumbing shape as PlaneEntry: id / name / visibility /
// a render-extent (halfLength for the drawn segment in mm).
struct AxisEntry {
    int id;
    std::string name;
    gp_Pnt origin;
    gp_Dir direction;
    bool visible = true;
    double halfLength = 50.0; // visible segment is ±halfLength along direction
};

struct SketchEntry {
    int id;
    std::string name;
    std::shared_ptr<materializr::Sketch> sketch;
    bool visible = true;
};

class Document {
public:
    Document();
    ~Document();

    void setEventBus(materializr::EventBus* bus) { m_eventBus = bus; }

    // Body management
    int addBody(const TopoDS_Shape& shape, const std::string& name = "");
    // Create-or-reuse helper for undoable operations that recreate a body on
    // redo (Extrude / Pattern / Mirror / etc.). First call passes `id == -1`
    // and gets a fresh id allocated. Subsequent calls (redo after undo+remove)
    // pass the previously-assigned `id` and the body is reinstated under that
    // id, restoring folderId / colour / visibility / name from the tombstone
    // that removeBody stashed. `id` is updated in place to the final body id.
    void addOrPutBody(int& id, const TopoDS_Shape& shape, const std::string& name = "");
    void removeBody(int id);
    void updateBody(int id, const TopoDS_Shape& shape);

    // The generation ledger of the op that PRODUCED a body's current shape
    // (published by ops after updateBody; the pointer is owned by the
    // history-held op). Lets a downstream op's topo::Ref resolve a sub-shape
    // by lineage - e.g. a fillet re-finding a boolean SEAM edge, which no
    // geometric scheme can name. updateBody clears the entry (a stale ledger
    // is worse than none); the producing op re-publishes right after.
    // Stored BY VALUE (copied from the op): a non-owning pointer dangled the
    // moment a publishing op died before its consumer minted (SIGBUS in
    // topo::mint via a destroyed ExtrudeOp/FilletOp ledger). Copying is cheap
    // relative to that whole class of lifetime bugs.
    void setBodyLedger(int id, const materializr::topo::GenerationLedger* l) {
        if (l) m_bodyLedgers[id] = *l; else m_bodyLedgers.erase(id);
    }
    const materializr::topo::GenerationLedger* bodyLedger(int id) const {
        auto it = m_bodyLedgers.find(id);
        return it == m_bodyLedgers.end() ? nullptr : &it->second;
    }

    // Face-lineage map of a body's CURRENT shape (see FaceLineage.h): face →
    // stable ancestry ids. Published by ops after updateBody, same lifecycle
    // as the ledger - updateBody clears it (stale lineage is worse than none;
    // an op that doesn't re-publish leaves consumers on their geometric
    // fallback, which is exactly the pre-lineage behaviour). Persisted in new
    // saves; absent in old ones.
    void setBodyFaceIds(int id, materializr::topo::FaceIdMap m) {
        if (m.empty()) m_bodyFaceIds.erase(id);
        else m_bodyFaceIds[id] = std::move(m);
    }
    const materializr::topo::FaceIdMap* bodyFaceIds(int id) const {
        auto it = m_bodyFaceIds.find(id);
        return it == m_bodyFaceIds.end() ? nullptr : &it->second;
    }
    // Mint a fresh, never-reused lineage id (document-global; persisted).
    int mintFaceId() { return m_nextFaceId++; }
    int faceIdCounter() const { return m_nextFaceId; }
    void setFaceIdCounter(int n) { if (n > m_nextFaceId) m_nextFaceId = n; }
    // Add a body with an explicit id, or update the body that already has that
    // id. Keeps ids stable across save/load and history replay; bumps the id
    // counter so later auto-assigned ids don't collide.
    void putBody(int id, const TopoDS_Shape& shape, const std::string& name = "");
    const TopoDS_Shape& getBody(int id) const;
    std::string getBodyName(int id) const;
    void setBodyName(int id, const std::string& name);
    void setBodyVisible(int id, bool visible);
    bool isBodyVisible(int id) const;
    void setBodyMesh(int id, bool isMesh);
    bool isBodyMesh(int id) const;
    // Fabrication sheet-part metadata (foam board / sheet metal / …). getBodySheet
    // returns a default (isSheet=false) spec for a non-sheet or unknown body.
    void setBodySheet(int id, const materializr::SheetSpec& spec);
    materializr::SheetSpec getBodySheet(int id) const;
    bool isBodySheet(int id) const;
    glm::vec3 getBodyColor(int id) const;
    void setBodyColor(int id, const glm::vec3& color);
    std::vector<int> getAllBodyIds() const;

    // Folder management. Folders are pure UI grouping over bodies - they
    // don't own bodies (a body keeps its id and is only assigned a folderId).
    int addFolder(const std::string& name = "");
    void removeFolder(int folderId); // bodies in it return to root (folderId=-1)
    std::vector<int> getAllFolderIds() const;
    std::string getFolderName(int folderId) const;
    void setFolderName(int folderId, const std::string& name);
    bool isFolderVisible(int folderId) const;
    // Setting folder visibility CASCADES to every member body's visibility.
    void setFolderVisible(int folderId, bool visible);
    glm::vec3 getFolderColor(int folderId) const;
    // Setting folder colour CASCADES to every member body's colour.
    void setFolderColor(int folderId, const glm::vec3& color);
    bool isFolderExpanded(int folderId) const;
    void setFolderExpanded(int folderId, bool expanded);
    // Bodies-by-folder lookups.
    int getBodyFolder(int bodyId) const; // -1 if at root
    void setBodyFolder(int bodyId, int folderId); // -1 = move to root
    std::vector<int> getBodiesInFolder(int folderId) const; // folderId=-1 = root bodies

    // Sketch management
    int addSketch(std::shared_ptr<materializr::Sketch> sketch, const std::string& name = "");
    // Insert/replace a sketch under a SPECIFIC id (mirrors putBody). Used by
    // project load to preserve saved sketch ids so SketchEditOps - and extrude/
    // push-pull ops - that reference a sketch by id rebind correctly on reload.
    void putSketch(int id, std::shared_ptr<materializr::Sketch> sketch,
                   const std::string& name = "");
    void removeSketch(int id);
    std::shared_ptr<materializr::Sketch> getSketch(int id) const;
    std::string getSketchName(int id) const;
    void setSketchName(int id, const std::string& name);
    void setSketchVisible(int id, bool visible);
    bool isSketchVisible(int id) const;
    std::vector<int> getAllSketchIds() const;
    int sketchCount() const;
    // Reverse lookup: returns the document id of the given Sketch* (compared
    // by raw pointer against the held shared_ptrs), or -1 if not found.
    // SketchEditOp::serializeWithDocument uses this to stamp the live id
    // into the serialized snapshot.
    int findSketchId(const materializr::Sketch* sk) const;

    // Cascade sketch override - the EDITED sketch's final state, pinned for
    // the duration of a history replay. During History::editStep the replayed
    // SketchEditOp snapshots roll the LIVE sketch back through its history, so
    // an op that re-finds geometry from "the sketch the user just edited"
    // (fillet/chamfer generative anchors, see EdgeAnchor.h) would read a STALE
    // state mid-replay while the extrude below it was rebuilt from the final
    // one. cascadeFromSketchEdit pins a copy here around the replay; anchor
    // resolution prefers it over the live sketch.
    void setCascadeSketchOverride(int id, std::shared_ptr<materializr::Sketch> snap) {
        m_cascadeSketchOverrides[id] = std::move(snap);
    }
    void clearCascadeSketchOverrides() { m_cascadeSketchOverrides.clear(); }
    std::shared_ptr<materializr::Sketch> cascadeSketchOverride(int id) const {
        auto it = m_cascadeSketchOverrides.find(id);
        return it == m_cascadeSketchOverrides.end() ? nullptr : it->second;
    }

    // Construction planes - first-class document objects parallel to sketches.
    // PlaneAddedEvent / PlaneRemovedEvent let the renderer + Items panel
    // react without polling each frame.
    // `reuseId` >= 0 re-adds the plane under that id (redo of a plane-creation
    // step, including reloaded ones) so downstream references stay valid;
    // -1 allocates a fresh id as usual.
    int addPlane(const gp_Pln& plane, const std::string& name = "",
                 int reuseId = -1);
    void removePlane(int id);
    // Update an existing plane's gp_Pln (move + rotate gizmo write-back).
    // Fires PlaneChangedEvent so the renderer / selection-aware UI updates.
    void setPlane(int id, const gp_Pln& plane);
    // Reverse the plane's normal in place (ZReverse on its gp_Ax3), keeping
    // location + in-plane X direction. Flips which way new sketches/extrudes
    // on this plane face. Fires PlaneChangedEvent.
    void flipPlaneNormal(int id);
    const PlaneEntry* getPlane(int id) const;
    std::string getPlaneName(int id) const;
    void setPlaneName(int id, const std::string& name);
    void setPlaneVisible(int id, bool visible);
    bool isPlaneVisible(int id) const;
    std::vector<int> getAllPlaneIds() const;
    int planeCount() const;

    // Reference images - raster underlays hosted on construction planes
    // (see RefImageEntry). Keyed by the host plane's id; changes ride the
    // Plane*Event stream (the image renderer re-syncs off the same events the
    // plane renderer does). removePlane() drops the hosted image with it.
    void setRefImage(int planeId, RefImageEntry entry);   // add or replace
    const RefImageEntry* getRefImage(int planeId) const;
    void removeRefImage(int planeId);
    void setRefImageWidthMM(int planeId, double widthMM);
    void setRefImageOpacity(int planeId, float opacity);
    std::vector<int> getAllRefImagePlaneIds() const;

    // Construction axes - same shape as construction planes. Used by
    // Revolve and any other op that needs to rotate around a line.
    // Axis* events let the renderer + Items panel react without polling.
    // `reuseId` semantics match addPlane.
    int addAxis(const gp_Pnt& origin, const gp_Dir& direction,
                const std::string& name = "", int reuseId = -1);
    void removeAxis(int id);
    void setAxis(int id, const gp_Pnt& origin, const gp_Dir& direction);
    // Reverse an axis's direction in place (keeps origin). Flips which way
    // Revolve spins around it. Fires AxisChangedEvent.
    void flipAxisDirection(int id);
    const AxisEntry* getAxis(int id) const;
    std::string getAxisName(int id) const;
    void setAxisName(int id, const std::string& name);
    void setAxisVisible(int id, bool visible);
    bool isAxisVisible(int id) const;
    std::vector<int> getAllAxisIds() const;
    int axisCount() const;

    // Clear everything
    void clear();

    // Body count
    int bodyCount() const;

private:
    int findBodyIndex(int id) const;
    int findSketchIndex(int id) const;
    int findFolderIndex(int id) const;

    std::vector<BodyEntry> m_bodies;
    std::vector<PlaneEntry> m_planes;
    std::vector<RefImageEntry> m_refImages;
    std::vector<AxisEntry> m_axes;
    std::vector<SketchEntry> m_sketches;
    // See setCascadeSketchOverride - pinned final sketch states during a
    // cascade history replay. Empty outside cascadeFromSketchEdit.
    std::map<int, std::shared_ptr<materializr::Sketch>> m_cascadeSketchOverrides;
    // See setBodyLedger - non-owning, cleared on updateBody.
    std::map<int, materializr::topo::GenerationLedger> m_bodyLedgers;
    // See setBodyFaceIds - owned here (unlike the non-owning ledgers).
    std::map<int, materializr::topo::FaceIdMap> m_bodyFaceIds;
    int m_nextFaceId = 1;
    std::vector<FolderEntry> m_folders;
    // Tombstones: when a body is removed, its non-geometry metadata (folderId,
    // colour, visibility, name) is stashed here keyed by id. When putBody is
    // later called with the same id (the typical redo-after-undo path through
    // ops like Extrude / Pattern / Mirror), the metadata is restored. Without
    // this, a body recreated after undo would silently snap back to the root
    // folder, default colour, and visible=true.
    std::map<int, BodyEntry> m_bodyTombstones;
    int m_nextBodyId = 1;
    int m_nextPlaneId = 1;
    int m_nextAxisId = 1;
    int m_nextSketchId = 1;
    int m_nextFolderId = 1;
    materializr::EventBus* m_eventBus = nullptr;
};
