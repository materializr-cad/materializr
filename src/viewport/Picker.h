#pragma once

#include <functional>
#include <algorithm>
#include <glm/glm.hpp>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <Bnd_Box.hxx>
#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

class Document;
class SelectionManager;

namespace materializr {

class Camera;

struct PickResult {
    bool hit = false;
    int bodyId = -1;
    int faceIndex = -1;
    // Set when the hit was on a construction plane (bodyId == -1). The
    // viewport input handler builds a SelectionType::Plane entry from this.
    int planeId = -1;
    // Construction-axis hit (bodyId == -1, planeId == -1). Builds a
    // SelectionType::Axis entry.
    int axisId = -1;
    TopoDS_Shape pickedShape;    // the picked face
    // Set, with `hit` still FALSE, when the ray missed every face but a visible
    // edge lies within the caller's edge-fallback radius of the cursor - a click
    // just OUTSIDE a silhouette edge, where half the edge's hit zone is over
    // background. bodyId / nearestEdge / edgeScreenDist are filled; nothing else
    // (no face, hit point or plane) is, and every `hit` consumer is unaffected.
    bool edgeOnlyHit = false;
    TopoDS_Shape nearestEdge;    // closest edge to hit point (if any)
    float edgeScreenDist = 1e6f; // screen distance to nearest edge in pixels
    // Closest vertex of the picked face to the cursor in screen pixels.
    // The viewport input handler uses this to expand a corner click into a
    // multi-edge selection (all edges meeting at that vertex), so the user
    // can fillet/chamfer a whole corner in one click. Empty when nothing
    // was hit.
    TopoDS_Shape nearestVertex;
    float vertexScreenDist = 1e6f;
    // Min(width, height) of the picked face's projected bbox in screen pixels.
    // The face-vs-edge classifier scales the edge-promotion threshold by this
    // so a small face on screen doesn't have every interior pixel "near" some
    // edge of its own boundary.
    float faceScreenSize = 1e6f;
    glm::vec3 hitPoint{0};
    float distance = 0;
};

// Screen radius (px) within which a click on a face promotes to its nearest
// edge. `basePx` is the full radius; the face's on-screen size caps it so a
// small face keeps an interior that still selects the face. That cap is what
// makes the target zoom-dependent: zoomed out, a face shrinks and so does the
// edge zone. It used to shrink without limit (¼ of the face), so a thin face
// such as a chamfer strip or fillet band left a 1-2 px target and edges took
// several tries. The floor keeps at least 40% of the base radius; zooming in
// grows the face past the cap and restores the full radius.
inline float edgePromotionRadiusPx(float basePx, float faceScreenSize) {
    const float interiorCap = std::max(faceScreenSize * 0.25f, basePx * 0.4f);
    return std::min(basePx, interiorCap);
}

class Picker {
public:
    Picker();

    // Per-body diagnostic prints inside pick() - set for ONE call (the
    // re-pick the click diagnostic runs), then cleared. Static so call
    // sites need no plumbing.
    static bool s_verbose;

    // Cast a ray from screen coordinates and find the nearest hit.
    // `edgeFallbackPx` > 0 enables the edge-only fallback (see PickResult::
    // edgeOnlyHit) with that screen radius. It is part of the unchanged-frame
    // cache key, and 0 (the default) costs nothing.
    PickResult pick(float screenX, float screenY,
                    float viewportWidth, float viewportHeight,
                    const Camera& camera, const Document& doc,
                    float edgeFallbackPx = 0.0f);

    // Hover calls pick() every rendered frame. When nothing it reads has
    // changed since the previous call (cursor, viewport, camera, and the
    // document's visible bodies, planes and axes) it returns the previous
    // result without touching any geometry. A body re-meshed in place keeps
    // its TShape, which this cannot see: Application::rebuildMeshes calls
    // invalidate() so the next pick runs in full.
    void invalidate() { m_lastValid = false; }
    // Whether the last pick() was answered from the previous result.
    bool lastPickWasCached() const { return m_lastCached; }

private:
    // Unproject screen point to world ray
    void screenToRay(float sx, float sy, float vpW, float vpH,
                     const Camera& camera,
                     glm::vec3& rayOrigin, glm::vec3& rayDir);

    // Test ray against an OCCT shape's bounding box
    bool rayIntersectsBBox(const glm::vec3& origin, const glm::vec3& dir,
                           const TopoDS_Shape& shape, float& tMin);

    int findNearestFace(const glm::vec3& origin, const glm::vec3& dir,
                        const TopoDS_Shape& shape, float& bestDist,
                        glm::vec3& hitPt, TopoDS_Shape& hitFace);

    // Face-resolving ray test for imported tessellated meshes. These can carry
    // thousands of faces, so the per-face findNearestFace path (which re-meshes
    // and explores every face + every edge each hover frame) is ruinous. Instead
    // we flatten the body ONCE into a cached list of world-space triangles, each
    // tagged with its owning face, and ray-test that. Returns the face index
    // (exploration order) and sets hitFace to the actual TopoDS_Face - so face
    // selection and sketch-on-face keep working - or -1 on a miss.
    int pickMeshBody(const glm::vec3& origin, const glm::vec3& dir,
                     const TopoDS_Shape& shape, float& bestDist,
                     glm::vec3& hitPt, TopoDS_Shape& hitFace);

    struct MeshTri { glm::vec3 v[3]; TopoDS_Face face; int faceIdx; };
    struct MeshCacheEntry {
        TopoDS_Shape shape;            // IsEqual() detects a pose change → rebuild
        std::vector<MeshTri> tris;
    };
    std::unordered_map<const void*, MeshCacheEntry> m_meshCache;

    // Per-body work a hovered frame should not repeat. Keyed by TShape;
    // IsSame re-validates the Location that both parts depend on. This
    // assumes one live body per TShape: every op that instances a body
    // (pattern, mirror) transforms with copy=true, so two bodies never share
    // one. Pruned in pick() alongside m_meshCache. Each part is filled on
    // first use:
    //  - box: the bounding box every pick tests for every visible body
    //    (BRepBndLib::Add per body per pick was ~0.3 ms on a 1683-face part).
    //    OCCT pads the triangulation box by the achieved deflection, so it
    //    stays valid across re-meshes at any quality (see rayIntersectsBBox).
    //  - edges: world-space polylines of every edge, so the hit body's
    //    nearest-edge search pays a screen projection per segment instead of
    //    GCPnts_TangentialDeflection over every edge (0.6-2 ms per frame on
    //    ordinary parts, far more on a thread's helices).
    struct EdgePolyline { TopoDS_Edge edge; std::vector<glm::vec3> pts; bool seam = false; };
    struct BodyCacheEntry {
        TopoDS_Shape shape;
        bool haveBox = false;
        Bnd_Box box;
        bool haveEdges = false;
        std::vector<EdgePolyline> edges;
    };
    std::unordered_map<const void*, BodyCacheEntry> m_bodyCache;
    BodyCacheEntry& bodyCache(const TopoDS_Shape& shape);
    const std::vector<EdgePolyline>& edgePolylines(const TopoDS_Shape& shape);

    // Everything pick() reads, captured so an unchanged frame can answer
    // from m_lastResult. Bodies compare with IsSame (TShape + Location),
    // planes and axes by value; hidden entries are left out so a change to
    // something hidden cannot force a re-pick.
    struct PickInputs {
        float sx = 0.0f, sy = 0.0f, vpW = 0.0f, vpH = 0.0f;
        float edgeFallbackPx = 0.0f;
        glm::mat4 view{1.0f};
        glm::mat4 proj{1.0f};
        std::vector<std::pair<int, TopoDS_Shape>> bodies;
        std::vector<double> datums;
    };
    static void gatherInputs(PickInputs& out, float sx, float sy, float vpW, float vpH,
                             const Camera& camera, const Document& doc,
                             float edgeFallbackPx);
    static bool sameInputs(const PickInputs& a, const PickInputs& b);
    PickInputs m_lastInputs;
    PickInputs m_nextInputs; // scratch; swapped into m_lastInputs on a miss
    PickResult m_lastResult;
    bool m_lastValid = false;
    bool m_lastCached = false;

    // Find the nearest edge to a world-space point, return screen distance.
    // `facePlaneNormal` is the outward (camera-facing) normal of the picked
    // face at `hitPt`; edges that lie noticeably BEHIND the face's tangent
    // plane at the hit are rejected so back-side edges don't get clicked
    // through. Pass a zero vector to skip the plane check.
    void findNearestEdge(const TopoDS_Shape& shape, const glm::vec3& hitPt,
                         const glm::vec3& facePlaneNormal,
                         float screenX, float screenY, float vpW, float vpH,
                         const Camera& camera,
                         TopoDS_Shape& nearestEdge, float& screenDist,
                         const std::function<bool(const glm::vec3&)>& occluded = {});

    // Edge-only fallback for a ray that missed every face: nearest visible edge
    // within `radiusPx` of the cursor, across all bodies near it on screen. Fills
    // result.edgeOnlyHit / bodyId / nearestEdge / edgeScreenDist on success.
    void pickEdgeOnly(float screenX, float screenY, float vpW, float vpH,
                      const Camera& camera, const Document& doc,
                      const std::vector<int>& bodyIds, float radiusPx,
                      PickResult& result);

    // True when some visible body's surface lies between the camera and `p`
    // (so an edge at `p` cannot be seen). The tangent-plane test in
    // findNearestEdge only rejects edges behind the PICKED face; it misses an
    // edge hidden behind a different, nearer face that sits in front of that
    // plane (the back of a rib, a boss's far side), which is what stole taps
    // on complex parts. `tolMm` absorbs the gap between an edge polyline and
    // the face mesh it lies on.
    bool pointOccluded(const glm::vec3& p, const Camera& camera,
                       const Document& doc, float tolMm);
};

} // namespace materializr
