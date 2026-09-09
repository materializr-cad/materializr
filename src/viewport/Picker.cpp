#include "Picker.h"
#include "Camera.h"
#include "../core/Document.h"
#include <cstdio>

#include <glm/gtc/matrix_transform.hpp>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include "core/MeshParams.h"
#include <TopoDS_Edge.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <BRepGProp_Face.hxx>
#include <gp_Vec.hxx>
#include <Geom_Surface.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>

#include <cmath>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

namespace materializr {

Picker::Picker() {}

bool Picker::s_verbose = false;

void Picker::screenToRay(float sx, float sy, float vpW, float vpH,
                         const Camera& camera,
                         glm::vec3& rayOrigin, glm::vec3& rayDir)
{
    // Convert screen coordinates to normalized device coordinates
    float ndcX = (2.0f * sx) / vpW - 1.0f;
    float ndcY = 1.0f - (2.0f * sy) / vpH; // flip Y

    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 proj = camera.getProjectionMatrix();
    glm::mat4 invVP = glm::inverse(proj * view);

    // Unproject near and far points
    glm::vec4 nearPt = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 farPt  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);

    nearPt /= nearPt.w;
    farPt  /= farPt.w;

    rayOrigin = glm::vec3(nearPt);
    rayDir = glm::normalize(glm::vec3(farPt) - glm::vec3(nearPt));
}

bool Picker::rayIntersectsBBox(const glm::vec3& origin, const glm::vec3& dir,
                               const TopoDS_Shape& shape, float& tMin)
{
    // The box is built once per body and reused until the body's shape or
    // location changes (bodyCache() re-validates with IsSame), so it must stay
    // valid when the renderer later re-meshes the body at another quality.
    // OCCT's triangulation box is enlarged by each face's achieved deflection
    // plus tolerance, so it contains the exact surface and therefore every
    // mesh the shape can have (a sphere of radius 10 meshed at Low reports
    // nodes out to 9.93 and a box out to 10.55). Faces with no triangulation
    // fall back to the geometry box, which is exact as well.
    BodyCacheEntry& entry = bodyCache(shape);
    if (!entry.haveBox) {
        BRepBndLib::Add(shape, entry.box);
        entry.haveBox = true;
    }
    const Bnd_Box& bbox = entry.box;
    if (bbox.IsVoid()) return false;

    double xMin, yMin, zMin, xMax, yMax, zMax;
    bbox.Get(xMin, yMin, zMin, xMax, yMax, zMax);

    // Add small gap to avoid zero-size boxes
    float gap = 1e-5f;
    glm::vec3 bMin(static_cast<float>(xMin) - gap,
                   static_cast<float>(yMin) - gap,
                   static_cast<float>(zMin) - gap);
    glm::vec3 bMax(static_cast<float>(xMax) + gap,
                   static_cast<float>(yMax) + gap,
                   static_cast<float>(zMax) + gap);

    // Ray-AABB intersection (slab method)
    float tMinVal = -std::numeric_limits<float>::max();
    float tMaxVal =  std::numeric_limits<float>::max();

    for (int i = 0; i < 3; ++i) {
        if (std::abs(dir[i]) < 1e-8f) {
            // Ray is parallel to slab
            if (origin[i] < bMin[i] || origin[i] > bMax[i])
                return false;
        } else {
            float invD = 1.0f / dir[i];
            float t1 = (bMin[i] - origin[i]) * invD;
            float t2 = (bMax[i] - origin[i]) * invD;

            if (t1 > t2) std::swap(t1, t2);

            tMinVal = std::max(tMinVal, t1);
            tMaxVal = std::min(tMaxVal, t2);

            if (tMinVal > tMaxVal)
                return false;
        }
    }

    if (tMaxVal < 0.0f)
        return false;

    tMin = tMinVal > 0.0f ? tMinVal : tMaxVal;
    return true;
}

// Moller-Trumbore ray-triangle intersection
static bool rayTriangleIntersect(const glm::vec3& origin, const glm::vec3& dir,
                                 const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
                                 float& t)
{
    const float EPSILON = 1e-7f;

    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;
    glm::vec3 h = glm::cross(dir, edge2);
    float a = glm::dot(edge1, h);

    if (a > -EPSILON && a < EPSILON)
        return false; // parallel

    float f = 1.0f / a;
    glm::vec3 s = origin - v0;
    float u = f * glm::dot(s, h);

    if (u < 0.0f || u > 1.0f)
        return false;

    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(dir, q);

    if (v < 0.0f || u + v > 1.0f)
        return false;

    t = f * glm::dot(edge2, q);
    return t > EPSILON;
}

int Picker::findNearestFace(const glm::vec3& origin, const glm::vec3& dir,
                            const TopoDS_Shape& shape, float& bestDist,
                            glm::vec3& hitPt, TopoDS_Shape& hitFace)
{
    // Pick against the renderer's triangulation and NEVER run the mesher
    // here. Every visible body is tessellated by rebuildMeshes before a pick
    // can reach it, and a body the renderer could not mesh is not drawn, so
    // it must not be pickable either. BRepMesh_IncrementalMesh on an already
    // meshed shape rebuilds its whole data model per call even when it
    // changes nothing (9 ms on a 54-face part, 66 ms on a 1683-face part),
    // and this runs every rendered frame the cursor rests on a body; at Low
    // quality it even re-meshed the body finer than the renderer asked for.
    // A face without a triangulation is simply skipped below.

    int faceIndex = -1;
    int currentFace = 0;
    bestDist = std::numeric_limits<float>::max();

    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TopoDS_Face& face = TopoDS::Face(explorer.Current());
        TopLoc_Location location;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, location);

        if (s_verbose) {
            if (triangulation.IsNull()) {
                std::fprintf(stderr, "      face %d: NULL triangulation\n",
                             currentFace);
            } else {
                gp_Pnt n1 = triangulation->Node(1).Transformed(
                    location.Transformation());
                std::fprintf(stderr,
                    "      face %d: %d tris, node1=(%.1f,%.1f,%.1f) "
                    "defl=%.3f\n",
                    currentFace, triangulation->NbTriangles(),
                    n1.X(), n1.Y(), n1.Z(),
                    triangulation->Deflection());
            }
        }

        if (triangulation.IsNull()) {
            ++currentFace;
            continue;
        }

        const gp_Trsf& trsf = location.Transformation();
        bool hasTransform = !location.IsIdentity();

        int nbTriangles = triangulation->NbTriangles();
        for (int i = 1; i <= nbTriangles; ++i) {
            const Poly_Triangle& tri = triangulation->Triangle(i);

            int n1, n2, n3;
            tri.Get(n1, n2, n3);

            gp_Pnt p1 = triangulation->Node(n1);
            gp_Pnt p2 = triangulation->Node(n2);
            gp_Pnt p3 = triangulation->Node(n3);

            if (hasTransform) {
                p1.Transform(trsf);
                p2.Transform(trsf);
                p3.Transform(trsf);
            }

            glm::vec3 v0(static_cast<float>(p1.X()), static_cast<float>(p1.Y()), static_cast<float>(p1.Z()));
            glm::vec3 v1(static_cast<float>(p2.X()), static_cast<float>(p2.Y()), static_cast<float>(p2.Z()));
            glm::vec3 v2(static_cast<float>(p3.X()), static_cast<float>(p3.Y()), static_cast<float>(p3.Z()));

            float t = 0.0f;
            if (rayTriangleIntersect(origin, dir, v0, v1, v2, t)) {
                if (t < bestDist) {
                    bestDist = t;
                    hitPt = origin + dir * t;
                    hitFace = face;
                    faceIndex = currentFace;
                }
            }
        }

        ++currentFace;
    }

    return faceIndex;
}

int Picker::pickMeshBody(const glm::vec3& origin, const glm::vec3& dir,
                         const TopoDS_Shape& shape, float& bestDist,
                         glm::vec3& hitPt, TopoDS_Shape& hitFace)
{
    const void* key = shape.TShape().get();
    auto it = m_meshCache.find(key);
    if (it == m_meshCache.end() || !it->second.shape.IsEqual(shape)) {
        // Build (or rebuild after a pose change) the flat world-space triangle
        // list, each tagged with its owning face. Coarse deflection - this is
        // only for hit-testing, and a mesh body's facets are already the limit.
        MeshCacheEntry entry;
        entry.shape = shape;
        BRepMesh_IncrementalMesh meshGen(shape, materializr::meshParams(0.5, 0.5, false));
        int faceIdx = 0;
        for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), ++faceIdx) {
            TopoDS_Face face = TopoDS::Face(ex.Current());
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull()) continue;
            const gp_Trsf& trsf = loc.Transformation();
            const bool hasX = !loc.IsIdentity();
            for (int i = 1; i <= tri->NbTriangles(); ++i) {
                int a, b, c;
                tri->Triangle(i).Get(a, b, c);
                gp_Pnt p1 = tri->Node(a), p2 = tri->Node(b), p3 = tri->Node(c);
                if (hasX) { p1.Transform(trsf); p2.Transform(trsf); p3.Transform(trsf); }
                MeshTri mt;
                mt.v[0] = glm::vec3(p1.X(), p1.Y(), p1.Z());
                mt.v[1] = glm::vec3(p2.X(), p2.Y(), p2.Z());
                mt.v[2] = glm::vec3(p3.X(), p3.Y(), p3.Z());
                mt.face = face;
                mt.faceIdx = faceIdx;
                entry.tris.push_back(mt);
            }
        }
        it = m_meshCache.insert_or_assign(key, std::move(entry)).first;
    }

    int hitFaceIdx = -1;
    float best = std::numeric_limits<float>::max();
    for (const MeshTri& mt : it->second.tris) {
        float t;
        if (rayTriangleIntersect(origin, dir, mt.v[0], mt.v[1], mt.v[2], t) && t < best) {
            best = t;
            hitFaceIdx = mt.faceIdx;
            hitFace = mt.face;
            hitPt = origin + dir * t;
        }
    }
    if (hitFaceIdx >= 0) bestDist = best;
    return hitFaceIdx;
}

Picker::BodyCacheEntry& Picker::bodyCache(const TopoDS_Shape& shape)
{
    const void* key = shape.TShape().get();
    auto it = m_bodyCache.find(key);
    if (it == m_bodyCache.end() || !it->second.shape.IsSame(shape)) {
        BodyCacheEntry fresh;
        fresh.shape = shape;
        it = m_bodyCache.insert_or_assign(key, std::move(fresh)).first;
    }
    return it->second;
}

const std::vector<Picker::EdgePolyline>&
Picker::edgePolylines(const TopoDS_Shape& shape)
{
    BodyCacheEntry& entry = bodyCache(shape);
    if (entry.haveEdges) return entry.edges;
    for (TopExp_Explorer exp(shape, TopAbs_EDGE); exp.More(); exp.Next()) {
        EdgePolyline pl;
        pl.edge = TopoDS::Edge(exp.Current());
        try {
            BRepAdaptor_Curve curve(pl.edge);
            GCPnts_TangentialDeflection discretizer(curve, 0.1, 0.1);
            const int nPts = discretizer.NbPoints();
            pl.pts.reserve(nPts > 0 ? static_cast<size_t>(nPts) : 0);
            for (int i = 1; i <= nPts; ++i) {
                gp_Pnt p = discretizer.Value(i);
                pl.pts.emplace_back(static_cast<float>(p.X()),
                                    static_cast<float>(p.Y()),
                                    static_cast<float>(p.Z()));
            }
        } catch (...) {
            continue;
        }
        if (pl.pts.size() >= 2) entry.edges.push_back(std::move(pl));
    }
    entry.haveEdges = true;
    return entry.edges;
}

void Picker::findNearestEdge(const TopoDS_Shape& shape, const glm::vec3& hitPt,
                             const glm::vec3& facePlaneNormal,
                             float screenX, float screenY, float vpW, float vpH,
                             const Camera& camera,
                             TopoDS_Shape& nearestEdge, float& screenDist)
{
    glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
    glm::vec2 mouse(screenX, screenY);
    screenDist = 1e6f;

    auto worldToScreen = [&](glm::vec3 p) -> glm::vec2 {
        glm::vec4 clip = vp * glm::vec4(p, 1.0f);
        if (std::abs(clip.w) < 1e-6f) return glm::vec2(1e6f);
        glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
        return glm::vec2((ndc.x * 0.5f + 0.5f) * vpW, (0.5f - ndc.y * 0.5f) * vpH);
    };

    // Face-plane occlusion: edges behind the picked face's tangent plane at
    // the hit point are hidden and must not steal the click. `facePlaneNormal`
    // is oriented camera-side by the caller, so signed distance against the
    // plane is positive in front and negative behind. The 0.3 mm slack covers
    // tessellation noise on curved silhouettes while rejecting any wall ≥ 0.3 mm
    // - and it's camera-angle-independent, unlike a view-direction depth check.
    bool havePlaneCheck = glm::length(facePlaneNormal) > 0.5f;
    glm::vec3 planeN = havePlaneCheck ? glm::normalize(facePlaneNormal) : glm::vec3(0.0f);
    const float planeTol = 0.3f;

    for (const EdgePolyline& pl : edgePolylines(shape)) {
        for (size_t i = 0; i + 1 < pl.pts.size(); ++i) {
            const glm::vec3& w1 = pl.pts[i];
            const glm::vec3& w2 = pl.pts[i + 1];

            glm::vec2 s1 = worldToScreen(w1);
            glm::vec2 s2 = worldToScreen(w2);

            glm::vec2 seg = s2 - s1;
            float segLen2 = glm::dot(seg, seg);
            float t = 0.0f;
            float d;
            if (segLen2 < 1e-6f) {
                d = glm::length(mouse - s1);
            } else {
                t = glm::clamp(glm::dot(mouse - s1, seg) / segLen2, 0.0f, 1.0f);
                d = glm::length(mouse - (s1 + t * seg));
            }

            // Skip edge segments hidden behind the picked face's plane.
            glm::vec3 wp = w1 + t * (w2 - w1);
            if (havePlaneCheck &&
                glm::dot(wp - hitPt, planeN) < -planeTol) continue;

            if (d < screenDist) {
                screenDist = d;
                nearestEdge = pl.edge;
            }
        }
    }
}

void Picker::gatherInputs(PickInputs& out, float sx, float sy, float vpW, float vpH,
                          const Camera& camera, const Document& doc)
{
    out.sx = sx; out.sy = sy; out.vpW = vpW; out.vpH = vpH;
    out.view = camera.getViewMatrix();
    out.proj = camera.getProjectionMatrix();
    out.bodies.clear();
    for (int id : doc.getAllBodyIds()) {
        if (!doc.isBodyVisible(id)) continue;
        out.bodies.emplace_back(id, doc.getBody(id));
    }
    out.datums.clear();
    for (int id : doc.getAllPlaneIds()) {
        const auto* e = doc.getPlane(id);
        if (!e || !doc.isPlaneVisible(id)) continue;
        const gp_Ax3& ax = e->plane.Position();
        const gp_Pnt& o = ax.Location();
        const gp_Dir& n = ax.Direction();
        const gp_Dir& x = ax.XDirection();
        out.datums.insert(out.datums.end(),
                          {double(id), o.X(), o.Y(), o.Z(), n.X(), n.Y(), n.Z(),
                           x.X(), x.Y(), x.Z(), e->halfSize});
    }
    for (int id : doc.getAllAxisIds()) {
        const auto* e = doc.getAxis(id);
        if (!e || !doc.isAxisVisible(id)) continue;
        out.datums.insert(out.datums.end(),
                          {double(id), e->origin.X(), e->origin.Y(), e->origin.Z(),
                           e->direction.X(), e->direction.Y(), e->direction.Z(),
                           e->halfLength});
    }
}

bool Picker::sameInputs(const PickInputs& a, const PickInputs& b)
{
    if (a.sx != b.sx || a.sy != b.sy || a.vpW != b.vpW || a.vpH != b.vpH) return false;
    if (a.view != b.view || a.proj != b.proj) return false;
    if (a.datums != b.datums || a.bodies.size() != b.bodies.size()) return false;
    for (size_t i = 0; i < a.bodies.size(); ++i) {
        if (a.bodies[i].first != b.bodies[i].first ||
            !a.bodies[i].second.IsSame(b.bodies[i].second))
            return false;
    }
    return true;
}

PickResult Picker::pick(float screenX, float screenY,
                        float viewportWidth, float viewportHeight,
                        const Camera& camera, const Document& doc)
{
    PickResult result;

    // Unchanged frame: answer from the previous result. The verbose
    // diagnostic re-pick exists for its per-body prints, so it always runs.
    m_lastCached = false;
    if (!s_verbose) {
        gatherInputs(m_nextInputs, screenX, screenY, viewportWidth, viewportHeight,
                     camera, doc);
        if (m_lastValid && sameInputs(m_nextInputs, m_lastInputs)) {
            m_lastCached = true;
            return m_lastResult;
        }
        std::swap(m_lastInputs, m_nextInputs);
    }
    m_lastValid = false; // set again once the walk below completes

    glm::vec3 rayOrigin, rayDir;
    screenToRay(screenX, screenY, viewportWidth, viewportHeight, camera, rayOrigin, rayDir);

    float nearestDist = std::numeric_limits<float>::max();

    std::vector<int> bodyIds = doc.getAllBodyIds();

    // Drop cache entries whose body is gone or was rebuilt with a new TShape
    // (delete, transform-copy, decimate, boolean). Without this the caches
    // would pin every edited body's shape + triangle / polyline lists alive
    // for the whole session. Cheap: one pointer per body.
    if (!m_meshCache.empty() || !m_bodyCache.empty()) {
        std::unordered_set<const void*> live;
        for (int id : bodyIds) {
            const TopoDS_Shape& s = doc.getBody(id);
            if (!s.IsNull()) live.insert(s.TShape().get());
        }
        for (auto it = m_meshCache.begin(); it != m_meshCache.end();)
            it = live.count(it->first) ? std::next(it) : m_meshCache.erase(it);
        for (auto it = m_bodyCache.begin(); it != m_bodyCache.end();)
            it = live.count(it->first) ? std::next(it) : m_bodyCache.erase(it);
    }

    for (int bodyId : bodyIds) {
        if (!doc.isBodyVisible(bodyId)) {
            if (s_verbose)
                std::fprintf(stderr, "  [Pick] body %d: INVISIBLE\n", bodyId);
            continue;
        }

        const TopoDS_Shape& shape = doc.getBody(bodyId);
        if (shape.IsNull()) {
            if (s_verbose)
                std::fprintf(stderr, "  [Pick] body %d: NULL shape\n", bodyId);
            continue;
        }

        // Quick bounding box rejection
        float bboxT = 0.0f;
        if (!rayIntersectsBBox(rayOrigin, rayDir, shape, bboxT)) {
            if (s_verbose) {
                Bnd_Box bb;
                BRepBndLib::Add(shape, bb);
                double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
                std::fprintf(stderr,
                    "  [Pick] body %d: bbox MISS (%.1f,%.1f,%.1f..%.1f,%.1f,%.1f)\n",
                    bodyId, x0, y0, z0, x1, y1, z1);
            }
            continue;
        }

        // Imported meshes: cached, face-resolving ray test - no per-frame
        // re-mesh, no O(edges) edge/vertex refinement. Still returns a real
        // TopoDS_Face so face selection + "Sketch on Face" work; we just skip
        // edge/corner promotion (meaningless on a faceted mesh anyway).
        if (doc.isBodyMesh(bodyId)) {
            float meshDist = 0.0f;
            glm::vec3 meshHitPt(0.0f);
            TopoDS_Shape meshFace;
            int meshFaceIdx =
                pickMeshBody(rayOrigin, rayDir, shape, meshDist, meshHitPt, meshFace);
            if (meshFaceIdx >= 0 && meshDist < nearestDist) {
                nearestDist = meshDist;
                result.hit = true;
                result.bodyId = bodyId;
                result.faceIndex = meshFaceIdx;
                result.pickedShape = meshFace;
                result.hitPoint = meshHitPt;
                result.distance = meshDist;
                result.nearestEdge = TopoDS_Shape();
                result.nearestVertex = TopoDS_Shape();
                result.edgeScreenDist = 1e6f;
                result.vertexScreenDist = 1e6f;
            }
            continue;
        }

        // Detailed face-level test
        float faceDist = 0.0f;
        glm::vec3 faceHitPt(0.0f);
        TopoDS_Shape faceShape;
        int faceIdx = findNearestFace(rayOrigin, rayDir, shape, faceDist, faceHitPt, faceShape);
        if (s_verbose)
            std::fprintf(stderr,
                "  [Pick] body %d: bbox ok, faceIdx=%d dist=%.2f\n",
                bodyId, faceIdx, faceDist);

        if (faceIdx >= 0 && faceDist < nearestDist) {
            nearestDist = faceDist;
            result.hit = true;
            result.bodyId = bodyId;
            result.faceIndex = faceIdx;
            result.pickedShape = faceShape;
            result.hitPoint = faceHitPt;
            result.distance = faceDist;

            // Compute the picked face's surface normal at the hit point and
            // orient it toward the camera. findNearestEdge uses this to reject
            // edges that lie behind the face's tangent plane (the proper
            // "is this edge on the same side as the camera?" check).
            glm::vec3 facePlaneN(0.0f);
            try {
                TopoDS_Face f = TopoDS::Face(faceShape);
                BRepGProp_Face prop(f);
                // Evaluate the normal AT THE HIT POINT, not the face's parametric
                // centre. On a curved face (cylinder, fillet, sphere) the centre
                // normal tilts the occlusion plane away from where the user
                // clicked, so far-side edges slip past it and get selected
                // "through" the surface. Projecting the hit point back onto the
                // surface gives the (u,v) there, and the normal at that (u,v) is
                // the correct local tangent plane. Flat faces are unaffected
                // (their normal is constant). Falls back to the centre if the
                // projection fails.
                double un, vn;
                bool haveUV = false;
                Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
                if (!surf.IsNull()) {
                    GeomAPI_ProjectPointOnSurf proj(
                        gp_Pnt(faceHitPt.x, faceHitPt.y, faceHitPt.z), surf);
                    if (proj.NbPoints() > 0) {
                        proj.LowerDistanceParameters(un, vn);
                        haveUV = true;
                        // Snap the hit onto the exact surface. The pick mesh
                        // is the renderer's (chords up to 0.5 mm at Low
                        // quality) and the measure tool consumes this point.
                        // The bound rejects a projection that wandered off
                        // to a far sheet of the untrimmed surface.
                        constexpr double kSnapMaxMm = 1.0;
                        if (proj.LowerDistance() < kSnapMaxMm) {
                            gp_Pnt on = proj.NearestPoint();
                            faceHitPt = glm::vec3(on.X(), on.Y(), on.Z());
                            result.hitPoint = faceHitPt;
                        }
                    }
                }
                if (!haveUV) {
                    double u1, u2, v1, v2;
                    prop.Bounds(u1, u2, v1, v2);
                    un = (u1 + u2) * 0.5; vn = (v1 + v2) * 0.5;
                }
                gp_Pnt fp; gp_Vec fn;
                prop.Normal(un, vn, fp, fn);
                if (fn.Magnitude() > 1e-10) {
                    fn.Normalize();
                    facePlaneN = glm::vec3(fn.X(), fn.Y(), fn.Z());
                    glm::vec3 toCam = glm::normalize(camera.getPosition() - faceHitPt);
                    if (glm::dot(facePlaneN, toCam) < 0.0f) facePlaneN = -facePlaneN;
                }
            } catch (...) {}

            // Find nearest edge on this body
            TopoDS_Shape edgeShape;
            float edgeDist = 1e6f;
            findNearestEdge(shape, faceHitPt, facePlaneN, screenX, screenY,
                           viewportWidth, viewportHeight, camera,
                           edgeShape, edgeDist);
            result.nearestEdge = edgeShape;
            result.edgeScreenDist = edgeDist;

            // Project the face's vertices to screen space and take the
            // bbox's shorter side. Callers use this to clamp the edge-
            // promotion threshold so a small on-screen face doesn't lose
            // every interior click to one of its own boundary edges. The
            // same loop also tracks the closest vertex to the cursor so a
            // corner click can be expanded into a multi-edge selection
            // upstream.
            float sMinX = 1e9f, sMinY = 1e9f, sMaxX = -1e9f, sMaxY = -1e9f;
            int sN = 0;
            float bestVertexD2 = 1e18f;
            glm::mat4 svp = camera.getProjectionMatrix() * camera.getViewMatrix();
            for (TopExp_Explorer vex(faceShape, TopAbs_VERTEX); vex.More(); vex.Next()) {
                gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vex.Current()));
                glm::vec4 clip = svp * glm::vec4(p.X(), p.Y(), p.Z(), 1.0f);
                if (std::abs(clip.w) < 1e-6f) continue;
                float sx = (clip.x / clip.w * 0.5f + 0.5f) * viewportWidth;
                float sy = (0.5f - clip.y / clip.w * 0.5f) * viewportHeight;
                sMinX = std::min(sMinX, sx); sMaxX = std::max(sMaxX, sx);
                sMinY = std::min(sMinY, sy); sMaxY = std::max(sMaxY, sy);
                ++sN;
                float dx = sx - screenX, dy = sy - screenY;
                float d2 = dx * dx + dy * dy;
                if (d2 < bestVertexD2) {
                    bestVertexD2 = d2;
                    result.nearestVertex = vex.Current();
                    result.vertexScreenDist = std::sqrt(d2);
                }
            }
            if (sN > 0) {
                result.faceScreenSize = std::min(sMaxX - sMinX, sMaxY - sMinY);
            }
        }
    }

    // ─── Construction-plane hit-test ──────────────────────────────────────
    // Planes are rendered as finite quads (halfSize × halfSize around the
    // origin in the plane's local X/Y). Ray-vs-plane gives t; checking
    // |u|,|v| ≤ halfSize bounds it to the visible quad. A plane only wins
    // over a body if it's closer (smaller t) - bodies still take priority
    // at the same point, so clicking through a body to a plane behind it
    // requires hiding the body first (consistent with Items panel filter).
    {
        std::vector<int> planeIds = doc.getAllPlaneIds();
        for (int pid : planeIds) {
            if (!doc.isPlaneVisible(pid)) continue;
            const auto* entry = doc.getPlane(pid);
            if (!entry) continue;
            const gp_Ax3& ax = entry->plane.Position();
            gp_Pnt   o3 = ax.Location();
            gp_Dir   n3 = ax.Direction();
            gp_Dir   xd = ax.XDirection();
            gp_Dir   yd = ax.YDirection();
            glm::vec3 O((float)o3.X(), (float)o3.Y(), (float)o3.Z());
            glm::vec3 N((float)n3.X(), (float)n3.Y(), (float)n3.Z());
            glm::vec3 X((float)xd.X(), (float)xd.Y(), (float)xd.Z());
            glm::vec3 Y((float)yd.X(), (float)yd.Y(), (float)yd.Z());
            float denom = glm::dot(N, rayDir);
            if (std::abs(denom) < 1e-6f) continue;          // ~parallel
            float t = glm::dot(O - rayOrigin, N) / denom;
            // Coplanar tie-break: a construction plane sitting ON a body face
            // (sketch-on-face plane, a part's base on the XY plane, etc.) has
            // t ≈ the face's t, but the analytic plane math and the mesh
            // ray-triangle math differ by float noise, so the plane would
            // randomly steal the click. Require the plane to be closer than
            // the nearest face by a small relative margin; on a tie the FACE
            // wins. With no face hit (nearestDist == FLT_MAX) the margin is
            // irrelevant and an open-space plane is still freely pickable.
            const float coplanarBias = nearestDist * 1e-3f;
            if (t <= 0.0f || t >= nearestDist - coplanarBias) continue; // behind / farther / coplanar
            glm::vec3 hit = rayOrigin + rayDir * t;
            glm::vec3 d = hit - O;
            float u = glm::dot(d, X);
            float v = glm::dot(d, Y);
            float h = static_cast<float>(entry->halfSize);
            if (std::abs(u) > h || std::abs(v) > h) continue;
            // Plane wins. bodyId stays -1 (planes aren't bodies);
            // result.planeId tells the caller to build a Plane selection.
            nearestDist = t;
            result.hit = true;
            result.bodyId = -1;
            result.faceIndex = -1;
            result.planeId = pid;
            result.pickedShape = TopoDS_Shape();
            result.hitPoint = hit;
            result.distance = t;
            result.nearestEdge = TopoDS_Shape();
            result.edgeScreenDist = 1e6f;
            result.faceScreenSize = 1e6f;
        }
    }

    // ─── Construction-axis hit-test ──────────────────────────────────────
    // Axes are 1D so a strict ray-line intersection almost never hits -
    // instead we measure the minimum distance from the cursor ray to the
    // axis line and accept any axis whose closest approach is within ~6 px
    // on screen at the hit depth. The closest-approach formula uses the
    // standard cross-product magnitude over the parallelogram base.
    {
        std::vector<int> axisIds = doc.getAllAxisIds();
        if (!axisIds.empty()) {
            const float hitPxRadius = 6.0f;
            // Approximate world units per pixel at any depth: convert one
            // pixel of horizontal extent at depth d to world units via the
            // perspective frustum's half-width. Cheap and good enough for a
            // pickability band.
            // glm::radians, not M_PI: this file is also in materializr_core, which
            // lacks the app target's _USE_MATH_DEFINES on MSVC.
            float fovRad = glm::radians(camera.getFov());
            float pxPerWorldRef = viewportHeight / (2.0f * std::tan(fovRad * 0.5f));
            for (int aid : axisIds) {
                if (!doc.isAxisVisible(aid)) continue;
                const auto* entry = doc.getAxis(aid);
                if (!entry) continue;
                glm::vec3 ao((float)entry->origin.X(), (float)entry->origin.Y(),
                              (float)entry->origin.Z());
                glm::vec3 ad((float)entry->direction.X(), (float)entry->direction.Y(),
                              (float)entry->direction.Z());
                // Closest-approach between ray (rayOrigin, rayDir) and the
                // axis line (ao, ad). Solve the 2-parameter min-distance
                // system; if the rays are parallel skip.
                glm::vec3 w0 = rayOrigin - ao;
                float a = glm::dot(rayDir, rayDir);          // 1
                float b = glm::dot(rayDir, ad);
                float c = glm::dot(ad, ad);                  // 1
                float d_ = glm::dot(rayDir, w0);
                float e  = glm::dot(ad, w0);
                float denom = a * c - b * b;
                if (denom < 1e-6f) continue;                  // parallel
                float sRay  = (b * e - c * d_) / denom;       // along rayDir
                float tAxis = (a * e - b * d_) / denom;       // along axis (signed)
                if (sRay <= 0.0f || sRay >= nearestDist) continue;
                // Clip to the visible axis segment so a click far from the
                // drawn line doesn't latch onto the infinite ray.
                float halfLen = static_cast<float>(entry->halfLength);
                if (std::abs(tAxis) > halfLen) continue;
                glm::vec3 pRay  = rayOrigin + rayDir * sRay;
                glm::vec3 pAxis = ao + ad * tAxis;
                float worldDist = glm::length(pRay - pAxis);
                // Convert that world separation to screen pixels at sRay's
                // depth. Hit if it's within the radius.
                float pxDist = worldDist * pxPerWorldRef / std::max(sRay, 1e-3f);
                if (pxDist > hitPxRadius) continue;
                nearestDist = sRay;
                result.hit = true;
                result.bodyId = -1;
                result.faceIndex = -1;
                result.planeId = -1;
                result.axisId = aid;
                result.pickedShape = TopoDS_Shape();
                result.hitPoint = pAxis;
                result.distance = sRay;
                result.nearestEdge = TopoDS_Shape();
                result.edgeScreenDist = 1e6f;
                result.faceScreenSize = 1e6f;
            }
        }
    }

    if (!s_verbose) {
        m_lastResult = result;
        m_lastValid = true;
    }
    return result;
}

} // namespace materializr
