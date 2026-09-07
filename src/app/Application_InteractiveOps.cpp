#include "gl_common.h"

#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include "app/Application.h"
#include "viewport/Viewport.h"
#include "viewport/Camera.h"
#include "viewport/ShapeRenderer.h"
#include "core/Document.h"
#include "core/LengthEdit.h"
#include "core/MeshGuard.h"
#include "core/PlaneAxes.h"
#include "core/History.h"
#include "core/SelectionManager.h"

#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
#include "modeling/SketchTool.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/MoveFaceOp.h"
#include "modeling/MoveHoleOp.h"
#include "modeling/FilletOp.h"
#include "modeling/ChamferOp.h"
#include "modeling/ShellOp.h"
#include "modeling/TaperOp.h"
#include "modeling/ScaleFaceOp.h"
#include "modeling/PrimitiveOp.h"
#include "app/UserAxes.h"
#include "modeling/ResizeCylindricalOp.h"
#include "modeling/ThreadOp.h"
#include <BRepMesh_IncrementalMesh.hxx>
#include <future>
#include "modeling/PatternOp.h"
#include "modeling/LoftOp.h"
#include "modeling/GuidedLoftOp.h"
#include "modeling/BoundaryFillOp.h"
#include "modeling/PatchOp.h"
#include "modeling/SewOp.h"
#include "modeling/ConstructionPlaneOp.h"
#include "io/FileDialogs.h"
#include "modeling/ConstructionAxisOp.h"
#include <Geom_Plane.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_BezierSurface.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_BezierCurve.hxx>
#include <Geom_Surface.hxx>

#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS_Edge.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include "../i18n.h"
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <Geom_SurfaceOfRevolution.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>
#include <gp_Lin.hxx>
#include <gp_Vec.hxx>
#include <Geom_Curve.hxx>
#include <IntAna_QuadQuadGeo.hxx>
#include <Precision.hxx>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Implementations split out of Application.cpp — every interactive operation
// (Fillet / Chamfer / Edit Fillet-Chamfer, Edit Diameter, Shell, Extrude,
// Push/Pull) lives here. The shared pattern is begin → update (per-frame live
// preview) → commit (push to history) → cancel (rollback). The corresponding
// popup panels stay in Application_Dialogs.cpp; the on-viewport overlays
// (drag handles, arrows, measurement readouts) stay in Application_Viewport.cpp.
namespace materializr {

bool Application::refuseMeshSelection(const char* opName) {
    if (!m_document || !m_selection) return false;
    const std::vector<int> bodies = materializr::selectedBodyIds(*m_selection);
    if (bodies.empty()) return false;
    const std::vector<int> meshes = materializr::meshBodiesAmong(*m_document, bodies);
    if (meshes.empty()) return false;
    showToast(materializr::meshRefusalMessage(opName, meshes.size(), bodies.size()), 6.0);
    return true;
}


// ─── Edit Diameter (resize cylindrical / conical face) ──────────────────────
//
// Accepts a face pick (edits both end edges → stays a cylinder) or a single
// circular edge pick (edits just that end → makes a cone). The detection
// gathers axis + height + radii + hole-or-solid; the begin/update/commit path
// drives a ResizeCylindricalOp whose execute builds a ring solid and
// fuses/cuts it into the body.

materializr::CylindricalPick Application::detectCylindricalResizeCandidate() const {
    if (!m_selection || !m_document) return {};
    return materializr::detectCylindricalPick(*m_document, *m_selection);
}


// ─── Thread (helical screw thread) ──────────────────────────────────────────
//
// The Toolbar's Thread button dispatches through the same cylindrical-face
// detector as Edit Diameter; beginThread copies its m_resizeCyl* output and
// opens the popup. No live preview: a helical sweep + boolean per frame is a
// multi-second operation, so the thread computes once on Apply.

void Application::beginThread(const materializr::CylindricalPick& p) {
    if (refuseMeshSelection("Thread")) return;
    cancelActiveIops();
    if (!p.ok) return;
    // Threads need a true cylinder — a cone's helix would leave the surface.
    if (std::abs(p.bottomR - p.topR) > 1e-5) {
        std::fprintf(stderr, "[Thread] picked face is conical — thread needs "
                             "a cylinder\n");
        return;
    }
    m_threadBodyId  = p.bodyId;
    m_threadIsHole  = p.isHole;
    m_threadRadius  = p.bottomR;
    m_threadLength  = p.height;
    m_threadAxis[0] = p.axis.Location().X();
    m_threadAxis[1] = p.axis.Location().Y();
    m_threadAxis[2] = p.axis.Location().Z();
    m_threadAxis[3] = p.axis.Direction().X();
    m_threadAxis[4] = p.axis.Direction().Y();
    m_threadAxis[5] = p.axis.Direction().Z();
    m_threadAxis[6] = p.axis.XDirection().X();
    m_threadAxis[7] = p.axis.XDirection().Y();
    m_threadAxis[8] = p.axis.XDirection().Z();

    // ISO metric coarse defaults: nearest standard pitch for this diameter
    // (M10 → 1.5, M6 → 1.0, …), thread depth at the ISO ratio 0.6134·P.
    // Gives a recognisable "standard coarse bolt" out of the box instead of
    // a hairline scratch.
    static const struct { double d, p; } kIsoCoarse[] = {
        {1.6, 0.35}, {2.0, 0.4}, {2.5, 0.45}, {3.0, 0.5}, {4.0, 0.7},
        {5.0, 0.8},  {6.0, 1.0}, {8.0, 1.25}, {10.0, 1.5}, {12.0, 1.75},
        {16.0, 2.0}, {20.0, 2.5}, {24.0, 3.0}, {30.0, 3.5}, {36.0, 4.0},
        {42.0, 4.5}, {48.0, 5.0},
    };
    double dia = m_threadRadius * 2.0;
    double pitch = kIsoCoarse[0].p;
    double bestDelta = 1e9;
    for (const auto& e : kIsoCoarse) {
        double delta = std::abs(e.d - dia);
        if (delta < bestDelta) { bestDelta = delta; pitch = e.p; }
    }
    m_threadPitch = static_cast<float>(pitch);
    m_threadDepth = static_cast<float>(0.6134 * pitch);
    m_threadRightHanded = true;
    // A standard coarse bolt is single-start; a lingering starts=3 from the
    // last cap would silently lose the sweep fast path (and its geometry).
    m_threadStarts = 1;
    // Likewise a groove width from the last part would silently override the
    // ISO proportions this dialog just computed.
    m_threadGrooveWidth = 0.0f;
    materializr::formatLengthDigits(m_threadPitchBuf, sizeof(m_threadPitchBuf), m_threadPitch);
    materializr::formatLengthDigits(m_threadDepthBuf, sizeof(m_threadDepthBuf), m_threadDepth);

    // Name the picked cylinder face topologically so the committed thread
    // FOLLOWS an upstream edit (the cylinder moving or its diameter changing)
    // instead of floating at its original absolute axis. Best-effort — an
    // unnameable face (imported/primitive cylinder with no sketch) leaves the
    // ref empty and the thread keeps today's absolute-param behaviour.
    m_threadFaceRef = materializr::topo::Ref{};
    if (m_selection && m_document) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.type != SelectionType::Face || e.shape.IsNull() ||
                e.shape.ShapeType() != TopAbs_FACE)
                continue;
            try {
                materializr::topo::Context ctx;
                ctx.doc = m_document;
                ctx.shape = m_document->getBody(m_threadBodyId);
                ctx.type = TopAbs_FACE;
                m_threadFaceRef = materializr::topo::mint(TopoDS::Face(e.shape), ctx);
            } catch (...) {}
            break;
        }
    }

    m_threadActive = true;
}

// Build a ThreadOp from the popup's current state. Shared by the async
// commit (worker computes, main thread pushes) so both ops are identical.
std::unique_ptr<ThreadOp> Application::makeThreadOpFromState() const {
    auto op = std::make_unique<ThreadOp>();
    op->setBody(m_threadBodyId);
    op->setAxis(gp_Ax2(gp_Pnt(m_threadAxis[0], m_threadAxis[1], m_threadAxis[2]),
                       gp_Dir(m_threadAxis[3], m_threadAxis[4], m_threadAxis[5]),
                       gp_Dir(m_threadAxis[6], m_threadAxis[7], m_threadAxis[8])));
    op->setRadius(m_threadRadius);
    op->setLength(m_threadLength);
    op->setPitch(static_cast<double>(m_threadPitch));
    op->setDepth(static_cast<double>(m_threadDepth));
    op->setIsHole(m_threadIsHole);
    op->setRightHanded(m_threadRightHanded);
    op->setProfile(static_cast<ThreadProfile>(m_threadProfile));
    op->setClearance(static_cast<double>(m_threadClearance));
    op->setStarts(m_threadStarts);
    op->setGrooveWidth(static_cast<double>(m_threadGrooveWidth));
    op->setTargetFaceRef(m_threadFaceRef);
    return op;
}

void Application::commitThread() {
    if (m_threadBodyId < 0) { cancelThread(); return; }
    if (m_threadComputing) {
        // Assigning a new std::async future while the old one is in flight
        // BLOCKS until it finishes — exactly the "app frozen and punishing
        // the CPU" failure. One compute at a time.
        std::fprintf(stderr, "[Thread] Apply ignored — still computing\n");
        return;
    }
    std::fprintf(stderr, "[Thread] Apply: launching worker\n");
    // Kick the multi-second sweep + boolean onto a worker thread. renderThreadPanel
    // polls the future and pushes the real op — with the precomputed result — when
    // it lands.
    //
    // DEEP-COPY the shape into the worker. A bare TopoDS_Shape handle copy is
    // refcount-shared with the live document's TShape, whose lazy OCCT caches
    // (triangulation, bounding box, surface adaptors) the RENDER thread populates
    // and reads every frame — concurrent read/write on the same TShape is a data
    // race (UB → intermittent crash/corruption). BRepBuilderAPI_Copy gives the
    // worker an independent TShape, so the two threads share nothing.
    TopoDS_Shape live;
    try { live = m_document->getBody(m_threadBodyId); } catch (...) {}
    if (live.IsNull()) { cancelThread(); return; }
    TopoDS_Shape body = BRepBuilderAPI_Copy(live).Shape();
    if (body.IsNull()) { cancelThread(); return; }
    auto worker = std::make_shared<ThreadOp>();
    {
        auto cfg = makeThreadOpFromState();
        *worker = *cfg; // same params; worker only calls const buildResult()
    }
    // Fresh cancel token: the modal's Cancel button signals it and the
    // worker aborts (between turns + OCCT user-break mid-boolean).
    m_threadApplyCancel = std::make_shared<std::atomic<bool>>(false);
    worker->setCancelToken(m_threadApplyCancel);
    // Pre-mesh on the worker at the CURRENT quality so the renderer's
    // tessellate() reuses the cache — meshing the swept rod's helicoid faces
    // on the main thread froze the app ~10s after the popup closed. Finer
    // angular pass (helicoids show 0.3 rad facets); linear must match the
    // app's exactly for the cache check.
    float mdefl, mang;
    meshQualityParams(mdefl, mang);
    const float meshAng = std::min(mang, 0.15f);
    m_threadFuture = std::async(std::launch::async,
        [worker, body, mdefl, meshAng]() {
            TopoDS_Shape r = worker->buildResult(body);
            if (!r.IsNull()) {
                try {
                    BRepMesh_IncrementalMesh mesh(r, mdefl, Standard_False,
                                                  meshAng, Standard_True);
                    mesh.Perform();
                } catch (...) {}
            }
            return r;
        });
    m_threadComputing = true;
    // Popup stays up (disabled) so the modal has an anchor; state is cleared
    // when the future resolves.
}

void Application::cancelThread() {
    m_threadActive = false;
    m_threadBodyId = -1;
}





// ─── Interactive Shell ──────────────────────────────────────────────────────
//
// User picks a face on a body, clicks Shell, the popup pops with a thickness
// field defaulting to 1.0 mm. Typing rebuilds via ShellOp::execute against
// the snapshot for a live preview; Apply commits, Esc reverts.

// ─── Interactive Extrude (drag-to-distance) ─────────────────────────────────

// Interactive Extrude now lives in ExtrudeController (the first user of the
// base's LiveOp preview model — see InteractiveOpController.h). What used to
// be four methods of history churn here is a delegate each; the controller
// keeps ONE ExtrudeOp instance and toggles it undo/execute, so the preview
// body keeps its id and History stays untouched until commit.

void Application::beginInteractiveExtrude(const TopoDS_Shape& profile,
                                          ExtrudeMode mode, int targetBody,
                                          int sourceSketchId) {
    cancelAllInteractivePreviews();
    m_extrudeCtl.beginExtrude(iopContext(), profile, mode, targetBody,
                              sourceSketchId);
}

void Application::updateInteractiveExtrude(bool applySnap) {
    m_extrudeCtl.updateExtrude(iopContext(), applySnap);
}

void Application::commitInteractiveExtrude() {
    m_extrudeCtl.commit(iopContext());
    markDirty();
    m_meshesDirty = true;
}

void Application::cancelInteractiveExtrude() {
    m_extrudeCtl.cancel(iopContext());
    m_meshesDirty = true;
}

// ─── Sketch region picking ──────────────────────────────────────────────────
//
// Ray-cast a screen point against every visible sketch's regions. Feeds both
// hover highlighting and the click that produces a SketchRegion selection —
// which is in turn what Push/Pull (PushPullController) reads at begin.

Application::SketchRegionHit Application::pickSketchRegion(float screenX, float screenY,
                                                           float vpW, float vpH,
                                                           bool buildIfCold) const {
    SketchRegionHit hit;
    if (!m_document || !m_viewport) return hit;

    const Camera& cam = m_viewport->getCamera();
    glm::mat4 view = cam.getViewMatrix();
    glm::mat4 proj = cam.getProjectionMatrix();
    glm::mat4 invVP = glm::inverse(proj * view);

    // Build a world-space ray through a given pixel.
    auto rayAt = [&](float sx, float sy, glm::vec3& origin, glm::vec3& dir) {
        float ndcx = (sx / vpW) * 2.0f - 1.0f;
        float ndcy = 1.0f - (sy / vpH) * 2.0f;
        glm::vec4 n = invVP * glm::vec4(ndcx, ndcy, -1.0f, 1.0f);
        glm::vec4 f = invVP * glm::vec4(ndcx, ndcy, 1.0f, 1.0f);
        n /= n.w; f /= f.w;
        origin = glm::vec3(n);
        dir = glm::normalize(glm::vec3(f) - glm::vec3(n));
    };

    glm::vec3 rayOrigin, rayDir;
    rayAt(screenX, screenY, rayOrigin, rayDir);

    float bestT = std::numeric_limits<float>::infinity();

    auto testSketch = [&](int sketchId, const Sketch& sketch) {
        const gp_Pln& pln = sketch.getPlane();
        const gp_Ax3& ax = pln.Position();
        glm::vec3 planeOrigin(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
        glm::vec3 planeNormal(ax.Direction().X(), ax.Direction().Y(), ax.Direction().Z());
        glm::vec3 planeX(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
        glm::vec3 planeY(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());

        auto projectToPlane = [&](glm::vec3 o, glm::vec3 d, float& tOut, glm::vec2& p2dOut) -> bool {
            float denom = glm::dot(d, planeNormal);
            if (std::abs(denom) < 1e-8f) return false;
            float t = glm::dot(planeOrigin - o, planeNormal) / denom;
            if (t <= 0.0f) return false;
            glm::vec3 local = (o + d * t) - planeOrigin;
            tOut = t;
            p2dOut = glm::vec2(glm::dot(local, planeX), glm::dot(local, planeY));
            return true;
        };

        float t;
        glm::vec2 p2d;
        if (!projectToPlane(rayOrigin, rayDir, t, p2d)) return;
        if (t >= bestT) return;

        // Cold region cache: building it runs the OCCT general fuse — on a
        // heavy sketch (SVG import, text) that's a SECONDS-long stall. The
        // per-frame HOVER pick must never trigger it (unhiding a complex
        // sketch used to freeze the app on the very next mouse move); a
        // CLICK still builds (one user-initiated wait, exactly as before).
        if (!buildIfCold && !sketch.regionsCached()) return;

        // Screen-space pick tolerance: how far ~6px maps to on this plane, so the
        // boundary catch area is a consistent, comfortable width at any zoom.
        float tol = 0.0f;
        glm::vec3 o2, d2; float t2; glm::vec2 p2d2;
        rayAt(screenX + 6.0f, screenY, o2, d2);
        if (projectToPlane(o2, d2, t2, p2d2)) tol = glm::length(p2d2 - p2d);

        auto regions = sketch.buildRegions();
        // Resolve overlapping candidates by two ranked rules instead of
        // first-match (BOP region order is arbitrary):
        //   1. STRICT containment beats near-boundary proximity. A click
        //      inside a letter is also within `tol` of the surrounding
        //      region's hole edge; first-match let the big region steal
        //      every click that wasn't dead-centre in the stroke.
        //   2. Among matches of equal rank, the SMALLEST region wins —
        //      clicking inside a nested shape picks the shape, not the
        //      sea it sits in.
        int bestIdx = -1;
        bool bestInside = false;
        double bestArea = 0.0;
        for (size_t i = 0; i < regions.size(); ++i) {
            bool inside = sketch.isPointInRegion(regions[i], p2d);
            if (!inside &&
                !sketch.isPointInOrNearRegion(regions[i], p2d, tol))
                continue;
            double area = 0.0;
            try {
                GProp_GProps props;
                BRepGProp::SurfaceProperties(regions[i].face, props);
                area = std::abs(props.Mass());
            } catch (...) {}
            bool better =
                bestIdx < 0 ||
                (inside && !bestInside) ||
                (inside == bestInside && area < bestArea);
            if (better) {
                bestIdx = static_cast<int>(i);
                bestInside = inside;
                bestArea = area;
            }
        }
        if (bestIdx >= 0) {
            bestT = t;
            hit.sketchId = sketchId;
            hit.regionIndex = bestIdx;
            hit.worldPoint = rayOrigin + rayDir * t;
            return;
        }

        // Fallback: edge picking. Open profiles (an arc, an unclosed polyline,
        // a spline used as a loft rib, …) have no closed region, so the loop
        // above misses them entirely — which used to make such sketches
        // unselectable from the viewport. Test 2D distance from the click
        // point to each primitive; if it lands within `tol` of any line /
        // circle / arc / spline / polygon edge, treat that as a whole-
        // sketch hit (regionIndex stays -1) so the viewport input handler
        // can emit a SelectionType::Sketch.
        auto getPoint2D = [&](int ptId) -> glm::vec2 {
            const SketchPoint* sp = sketch.getPoint(ptId);
            return sp ? sp->pos : glm::vec2(0.0f);
        };
        auto distPointToSegment = [](glm::vec2 p, glm::vec2 a, glm::vec2 b) -> float {
            glm::vec2 ab = b - a;
            float len2 = glm::dot(ab, ab);
            if (len2 < 1e-12f) return glm::length(p - a);
            float u = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
            return glm::length(p - (a + u * ab));
        };

        bool nearEdge = false;
        for (const auto& ln : sketch.getLines()) {
            if (distPointToSegment(p2d, getPoint2D(ln.startPointId),
                                        getPoint2D(ln.endPointId)) <= tol) {
                nearEdge = true; break;
            }
        }
        if (!nearEdge) {
            for (const auto& c : sketch.getCircles()) {
                glm::vec2 ctr = getPoint2D(c.centerPointId);
                float r = static_cast<float>(c.radius);
                float d = std::abs(glm::length(p2d - ctr) - r);
                if (d <= tol) { nearEdge = true; break; }
            }
        }
        if (!nearEdge) {
            for (const auto& a : sketch.getArcs()) {
                glm::vec2 ctr = getPoint2D(a.centerPointId);
                float r = static_cast<float>(a.radius);
                glm::vec2 s = getPoint2D(a.startPointId);
                glm::vec2 e = getPoint2D(a.endPointId);
                float d = std::abs(glm::length(p2d - ctr) - r);
                if (d > tol) continue;
                // Angle-in-arc test: only count if p2d's angle from centre
                // falls within the CCW sweep from start to end.
                float a0 = std::atan2(s.y - ctr.y, s.x - ctr.x);
                float a1 = std::atan2(e.y - ctr.y, e.x - ctr.x);
                float ap = std::atan2(p2d.y - ctr.y, p2d.x - ctr.x);
                auto norm = [](float x) {
                    while (x < 0) x += 2.0f * static_cast<float>(M_PI);
                    while (x >= 2.0f * static_cast<float>(M_PI)) x -= 2.0f * static_cast<float>(M_PI);
                    return x;
                };
                float span = norm(a1 - a0);
                float here = norm(ap - a0);
                if (here <= span) { nearEdge = true; break; }
            }
        }
        if (!nearEdge) {
            // Splines + polygons: approximate by walking the control / vertex
            // chain as a polyline. Good enough for picking; the renderer
            // tessellates the same control sequence anyway.
            for (const auto& sp : sketch.getSplines()) {
                const auto& ids = sp.controlPointIds;
                for (size_t i = 1; i < ids.size(); ++i) {
                    if (distPointToSegment(p2d, getPoint2D(ids[i-1]),
                                                getPoint2D(ids[i])) <= tol) {
                        nearEdge = true; break;
                    }
                }
                if (nearEdge) break;
            }
        }
        if (!nearEdge) {
            for (const auto& poly : sketch.getPolygons()) {
                const auto& ids = poly.vertexPointIds;
                for (size_t i = 0; i < ids.size(); ++i) {
                    glm::vec2 a = getPoint2D(ids[i]);
                    glm::vec2 b = getPoint2D(ids[(i + 1) % ids.size()]);
                    if (distPointToSegment(p2d, a, b) <= tol) {
                        nearEdge = true; break;
                    }
                }
                if (nearEdge) break;
            }
        }

        if (nearEdge) {
            bestT = t;
            hit.sketchId = sketchId;
            hit.regionIndex = -1; // edge-only hit; caller emits SelectionType::Sketch
            hit.worldPoint = rayOrigin + rayDir * t;
        }
    };

    // Test the active sketch first (most relevant when in sketch mode)
    if (m_activeSketch) testSketch(m_activeSketchId, *m_activeSketch);
    // Then all stored sketches
    for (int sid : m_document->getAllSketchIds()) {
        if (!m_document->isSketchVisible(sid)) continue;
        if (sid == m_activeSketchId) continue;
        auto sk = m_document->getSketch(sid);
        if (sk) testSketch(sid, *sk);
    }

    return hit;
}


// ─── Move Face (in-plane slide → whole-body shear) ──────────────────────────



// Restore the on-face sketches to their snapshot planes, then slide them by
// `v` (so the live preview never compounds). v = (0,0,0) just restores.



// Bake the just-released ring drag into the accumulated tilt (so the next ring
// drag stacks on top), then reset the live angle.

// Configure an op with the current gesture (Move / Rotate / Scale) + hole flags.





// ─── User Z-up axis convention helpers ─────────────────────────────────────
//
// The world stays Y-up internally (camera, transforms, viewcube), but the
// user-facing axis radios in tools like Pattern and Split read with 3D-
// printer convention: X = left/right, Y = forward/back, Z = up. So when
// the user picks "Z" we drive the rotation around the world Y axis, etc.
// Keeping the mapping in one place means the toolbar can label its radios
// in user terms while the modeling ops keep getting world-space vectors.

glm::vec3 Application::userAxisToWorldVec(int userIdx) {
    return materializr::userAxisToWorldVec(userIdx); // shared: UserAxes.h
}

int Application::userAxisToWorldIdx(int userIdx) {
    return materializr::userAxisToWorldIdx(userIdx);
}

// ─── Interactive Pattern (Linear / Radial) ─────────────────────────────────
//
// User clicks Linear Pattern or Radial Pattern in the toolbar (PatternPlugin
// fires a requestInteractiveOp via PluginContext, which the main frame loop
// picks up and dispatches to beginPattern). The popup lets the user adjust
// axis / count / spacing (linear) or axis / count / angle / origin (radial)
// with a live preview. Each parameter change re-pushes a PatternOp onto
// history via updatePattern; commit leaves the op there, cancel undoes it.

void Application::beginPattern(PatternKind kind) {
    cancelActiveIops();
    // Find the first selected body. PatternOp clones one source body.
    int bodyId = -1;
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::Body && e.bodyId >= 0) {
                bodyId = e.bodyId; break;
            }
        }
    }
    if (bodyId < 0) return;

    m_patternKind   = kind;
    m_patternBodyId = bodyId;
    m_patternAxisIdx = (kind == PatternKind::Linear) ? 0 : 2; // default X for linear, Z for radial
    m_patternAxisId = -1; // start on a world axis; user can pick a construction axis
    m_patternCount    = (kind == PatternKind::Linear) ? 3   : 6;
    m_patternDistance = 5.0f;
    m_patternAngle    = 360.0f;
    // Default radial origin = body's bbox centre — that puts the rotation
    // axis through the body the first time so the user sees a sensible
    // ring immediately and can re-pick the origin via the viewport.
    m_patternOriginX = m_patternOriginY = m_patternOriginZ = 0.0f;
    try {
        Bnd_Box bb;
        BRepBndLib::Add(m_document->getBody(bodyId), bb);
        if (!bb.IsVoid()) {
            double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
            m_patternOriginX = static_cast<float>((x0 + x1) * 0.5);
            m_patternOriginY = static_cast<float>((y0 + y1) * 0.5);
            m_patternOriginZ = static_cast<float>((z0 + z1) * 0.5);
        }
    } catch (...) {}
    m_patternPickingOrigin = false;
    m_patternPreview.clear(*m_document);
    m_patternInputFocus    = true;
    std::snprintf(m_patternCountBuf,    sizeof(m_patternCountBuf),    "%d", m_patternCount);
    materializr::formatLengthDigits(m_patternDistanceBuf, sizeof(m_patternDistanceBuf), m_patternDistance);
    std::snprintf(m_patternAngleBuf,    sizeof(m_patternAngleBuf),    "%.1f", m_patternAngle);
    m_patternActive = true;

    updatePattern(); // initial preview
}

void Application::updatePattern() {
    if (!m_patternActive || m_patternBodyId < 0) return;

    // Retract the applied preview so the new parameters land on a clean
    // document. NOT a history undo: the preview never reaches History (see
    // LiveOpPreview for the three bugs that came of doing it that way), and
    // the SAME PatternOp instance is re-used, so its id-reuse pool keeps every
    // copy's body id stable across the whole gesture.
    m_patternPreview.retract(*m_document);
    if (m_patternCount < 2) {
        // Nothing to preview at count=1 (a pattern of 1 is just the source).
        m_meshesDirty = true;
        return;
    }
    if (!m_patternPreview.op())
        m_patternPreview.hold(std::make_unique<PatternOp>(), *m_document);
    auto* op = static_cast<PatternOp*>(m_patternPreview.op());
    op->setBody(m_patternBodyId);

    // Axis direction comes from the chosen world axis, or — when the user
    // picked a construction axis from the dropdown — from that axis's own
    // direction (and, for radial, its origin too, since the axis defines the
    // full rotation line).
    glm::vec3 axisDir = userAxisToWorldVec(m_patternAxisIdx);
    float originX = m_patternOriginX, originY = m_patternOriginY, originZ = m_patternOriginZ;
    if (m_patternAxisId >= 0) {
        if (const auto* a = m_document->getAxis(m_patternAxisId)) {
            axisDir = glm::vec3((float)a->direction.X(),
                                (float)a->direction.Y(),
                                (float)a->direction.Z());
            originX = (float)a->origin.X();
            originY = (float)a->origin.Y();
            originZ = (float)a->origin.Z();
        }
    }

    if (m_patternKind == PatternKind::Linear) {
        op->setType(PatternType::Linear);
        op->setCount(m_patternCount);
        op->setLinearSpacing(axisDir.x * m_patternDistance,
                             axisDir.y * m_patternDistance,
                             axisDir.z * m_patternDistance);
    } else {
        op->setType(PatternType::Radial);
        op->setCount(m_patternCount);
        op->setRadialAxis(axisDir.x, axisDir.y, axisDir.z);
        op->setRadialOrigin(originX, originY, originZ);
        op->setTotalAngle(m_patternAngle);
    }
    m_patternPreview.apply(*m_document);
    m_meshesDirty = true;
}

void Application::commitPattern() {
    // The previewed instance IS the final op — record it without re-running
    // it. (It used to already BE a history step by this point, which is what
    // made the preview undoable mid-gesture.)
    m_patternPreview.commit(*m_history);
    m_patternActive        = false;
    m_patternPickingOrigin = false;
    m_patternBodyId        = -1;
    m_meshesDirty = true;
}

void Application::cancelPattern() {
    m_patternPreview.clear(*m_document);
    m_patternActive        = false;
    m_patternPickingOrigin = false;
    m_patternBodyId        = -1;
    m_meshesDirty = true;
}

// ─── Loft (interactive popup) ──────────────────────────────────────────────
//
// LoftPlugin walks the selection and, when 2+ distinct sketches are present,
// fires requestInteractiveOp(InteractiveOp::Loft). The main frame loop dispatches that to
// beginLoft(), which snapshots one profile section per selected sketch (the
// outer wire of each sketch's outermost region, in click order — that order
// is the skinning order) and opens the popup. updateLoft re-pushes a preview
// LoftOp each frame the user changes a toggle / flips / reorders a section,
// commitLoft leaves the final op on history, cancelLoft undoes the preview.

// The outermost closed region of a sketch (largest outer-wire bbox) plus its
// hole wires — shared by Loft and Boundary Fill. Concentric profiles decompose
// into multiple regions; taking the outermost keeps the holes as channels
// instead of grabbing the inner disk.
static TopoDS_Wire outermostRegionWire(materializr::Sketch* sk,
                                       std::vector<TopoDS_Wire>& holesOut,
                                       bool* fromRegionOut = nullptr) {
    holesOut.clear();
    if (fromRegionOut) *fromRegionOut = false;
    if (!sk) return {};
    auto regions = sk->buildRegions();
    if (!regions.empty()) {
        size_t best = 0;
        double bestDiag = -1.0;
        for (size_t i = 0; i < regions.size(); ++i) {
            if (regions[i].outerWire.IsNull()) continue;
            Bnd_Box bb;
            BRepBndLib::Add(regions[i].outerWire, bb);
            if (bb.IsVoid()) continue;
            double x0, y0, z0, x1, y1, z1;
            bb.Get(x0, y0, z0, x1, y1, z1);
            double dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
            double diag = dx * dx + dy * dy + dz * dz;
            if (diag > bestDiag) { bestDiag = diag; best = i; }
        }
        holesOut = regions[best].holeWires;
        if (fromRegionOut) *fromRegionOut = true;   // regions are closed loops
        return regions[best].outerWire;
    }
    auto wires = sk->buildWires();
    if (!wires.empty()) return wires[0];
    return {};
}

// The outer boundary of a face, as a loft section.
//
// A face already carries a real closed loop with real edges and real vertices,
// so this sidesteps the sketch region builder entirely. That matters: a section
// synthesised from spline-heavy sketch geometry can come out with corners that
// do not correspond between sections, which lofts into a self-intersecting
// solid (issue #83). Picking two faces gives ThruSections two clean loops.
//
// Inner wires come along as holes, so lofting between two ring-shaped faces
// still produces a tube.
static TopoDS_Wire faceSectionWire(const TopoDS_Shape& faceShape,
                                   std::vector<TopoDS_Wire>& holesOut) {
    holesOut.clear();
    if (faceShape.IsNull() || faceShape.ShapeType() != TopAbs_FACE) return {};
    const TopoDS_Face f = TopoDS::Face(faceShape);
    TopoDS_Wire outer;
    try {
        outer = BRepTools::OuterWire(f);
    } catch (...) { return {}; }
    if (outer.IsNull()) return {};
    for (TopExp_Explorer ex(f, TopAbs_WIRE); ex.More(); ex.Next()) {
        const TopoDS_Wire w = TopoDS::Wire(ex.Current());
        if (!w.IsSame(outer)) holesOut.push_back(w);
    }
    return outer;
}

void Application::beginLoft() {
    if (refuseMeshSelection("Loft")) return;
    if (!m_selection || !m_document) return;

    // Snapshot every distinct selected sketch, in click order — with three
    // ribs the loft runs first→last, so the order the user picked them in IS
    // the loft order (reorderable later in the panel).
    std::vector<int> sketchIds;
    auto addId = [&](int id) {
        if (id < 0) return;
        for (int x : sketchIds) if (x == id) return;
        sketchIds.push_back(id);
    };
    // Selected FACES are sections too, in click order alongside sketches.
    std::vector<TopoDS_Shape> faceShapes;
    std::vector<int> faceBodyIds;
    for (const auto& e : m_selection->getSelection()) {
        if ((e.type == SelectionType::Sketch ||
             e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
            addId(e.sketchId);
        } else if (e.type == SelectionType::Face && !e.shape.IsNull() &&
                   e.shape.ShapeType() == TopAbs_FACE) {
            bool dup = false;
            for (const auto& f : faceShapes) if (f.IsSame(e.shape)) { dup = true; break; }
            if (!dup) { faceShapes.push_back(e.shape); faceBodyIds.push_back(e.bodyId); }
        }
    }
    {
        // What the selection actually held when Loft was pressed. Without this
        // a face selection that quietly fails is indistinguishable from one
        // that was never seen.
        int nSketch=0, nRegion=0, nFace=0, nBody=0, nEdge=0, nOther=0;
        for (const auto& e : m_selection->getSelection()) {
            switch (e.type) {
                case SelectionType::Sketch:       ++nSketch; break;
                case SelectionType::SketchRegion: ++nRegion; break;
                case SelectionType::Face:         ++nFace;   break;
                case SelectionType::Body:         ++nBody;   break;
                case SelectionType::Edge:         ++nEdge;   break;
                default:                          ++nOther;  break;
            }
        }
        std::fprintf(stderr,
            "[Loft] beginLoft: selection has %d sketch, %d region, %d face, "
            "%d body, %d edge, %d other -> %zu sketch id(s), %zu face shape(s)\n",
            nSketch, nRegion, nFace, nBody, nEdge, nOther,
            sketchIds.size(), faceShapes.size());
    }
    if (sketchIds.size() + faceShapes.size() < 2) {
        std::fprintf(stderr, "[Loft] beginLoft: fewer than 2 sections, giving up.\n");
        return;
    }


    // Classify each sketch: a closed region = a SECTION; no closed region but
    // an open wire = a RAIL candidate (a side-silhouette curve). One section +
    // 1–2 rails = guided loft (base swept along the rails — the "pyramid with
    // rounded sides" shape a section stack can't express). Anything else =
    // plain N-section loft.
    m_loftSections.clear();
    m_loftRails.clear();
    m_loftRailsMode = false;
    int unusable = 0;
    // All sections faces of ONE body -> the loft can bridge into that body
    // (consume the faces, sew, one solid) instead of adding a separate body
    // that can never be unioned. See LoftOp::setBridge.
    m_loftBridgeBodyId = -1;
    m_loftBridgeFaces.clear();
    if (sketchIds.empty() && faceShapes.size() >= 2 && !faceBodyIds.empty()) {
        bool sameBody = faceBodyIds[0] >= 0;
        for (int id : faceBodyIds) if (id != faceBodyIds[0]) { sameBody = false; break; }
        if (sameBody) {
            m_loftBridgeBodyId = faceBodyIds[0];
            m_loftBridgeFaces = faceShapes;
        }
    }
    for (const TopoDS_Shape& fs : faceShapes) {
        LoftSection sec;
        sec.sketchId = -1;                  // a face section has no sketch
        sec.outer = faceSectionWire(fs, sec.holes);
        if (sec.outer.IsNull()) {
            std::fprintf(stderr, "[Loft] a selected face has no usable outer wire.\n");
            ++unusable;
            continue;
        }
        int n = 0;
        for (TopExp_Explorer ex(sec.outer, TopAbs_EDGE); ex.More(); ex.Next()) ++n;
        std::fprintf(stderr, "[Loft] face section: %d edges, %zu hole(s).\n",
                     n, sec.holes.size());
        m_loftSections.push_back(std::move(sec));
    }
    for (int id : sketchIds) {
        LoftSection sec;
        sec.sketchId = id;
        bool fromRegion = false;
        {
            auto sk = m_document->getSketch(id);
            sec.outer = outermostRegionWire(sk.get(), sec.holes, &fromRegion);
        }
        // "Closed" = the sketch produced a REGION. (Never trust TopoDS's
        // Closed() flag — it's builder-advisory and the region walker doesn't
        // set it, which mis-filed plain circles as open and refused the loft.)
        if (!sec.outer.IsNull() && fromRegion) {
            m_loftSections.push_back(std::move(sec));
            continue;
        }
        // No closed region — take the sketch's longest OPEN chain as a rail.
        // (buildWires() can't serve here: it prunes every non-cycle edge.)
        if (auto sk = m_document->getSketch(id)) {
            TopoDS_Wire open = sk->buildOpenWire();
            if (!open.IsNull()) {
                m_loftRails.push_back({id, open});
                continue;
            }
        }
        std::fprintf(stderr, "[Loft] sketch %d has no usable geometry.\n", id);
        ++unusable;
    }

    if (m_loftSections.size() == 1 && !m_loftRails.empty() &&
        m_loftRails.size() <= 2) {
        // Guided mode: the single closed profile is the base, the open
        // sketches are its rails.
        if (auto sk = m_document->getSketch(m_loftSections[0].sketchId))
            m_loftBasePlane = sk->getPlane();
        m_loftRailsMode = true;
    } else if (m_loftSections.size() >= 2) {
        // Plain section loft; open sketches (if any) don't participate.
        if (!m_loftRails.empty())
            showToast(std::to_string(m_loftRails.size()) +
                      " open sketch(es) ignored — rails need exactly ONE "
                      "closed base profile.");
        m_loftRails.clear();
    } else {
        const bool tooManyRails =
            m_loftSections.size() == 1 && m_loftRails.size() > 2;
        const int nClosed = static_cast<int>(m_loftSections.size());
        const int nOpen   = static_cast<int>(m_loftRails.size());
        m_loftSections.clear();
        m_loftRails.clear();
        showToast(tooManyRails
            ? "Guided loft takes at most two rail curves - deselect the "
              "extras."
            : "Selected " + std::to_string(nClosed) + " closed profile(s) + " +
              std::to_string(nOpen) + " open curve(s). Loft needs 2+ closed "
              "profiles (sections), or exactly 1 closed + 1-2 open (rails).");
        return;
    }
    if (unusable > 0)
        showToast(std::to_string(unusable) + " empty sketch(es) skipped.");

    m_loftSolid = true;
    m_loftRuled = false;
    m_loftPreview.clear(*m_document);
    m_loftActive = true;

    updateLoft();
}

void Application::updateLoft() {
    if (!m_loftActive || !m_history || !m_document) return;
    if (m_loftRailsMode ? m_loftSections.empty() : m_loftSections.size() < 2)
        return;

    // Retract the applied preview so the new parameters land on a clean
    // document. NOT a history undo — the preview never reaches History, and
    // holding ONE instance keeps the lofted body's id stable while the user
    // flips and reorders sections (see LiveOpPreview).
    m_loftPreview.retract(*m_document);

    if (m_loftRailsMode) {
        // Rails mode is a DIFFERENT op class, so switching into it swaps the
        // held instance rather than re-syncing it.
        auto* held = dynamic_cast<GuidedLoftOp*>(m_loftPreview.op());
        if (!held) {
            m_loftPreview.hold(std::make_unique<GuidedLoftOp>(), *m_document);
            held = static_cast<GuidedLoftOp*>(m_loftPreview.op());
        }
        held->setBase(m_loftSections[0].outer, m_loftBasePlane);
        held->clearRails();
        for (const LoftRail& r : m_loftRails) held->addRail(r.wire);
        held->setSolid(m_loftSolid);
        if (!m_loftPreview.apply(*m_document))
            showToast("Guided loft failed - rails must rise away from the "
                      "base profile's plane.");
        m_meshesDirty = true;
        return;
    }

    auto* held = dynamic_cast<LoftOp*>(m_loftPreview.op());
    if (!held) {
        m_loftPreview.hold(std::make_unique<LoftOp>(), *m_document);
        held = static_cast<LoftOp*>(m_loftPreview.op());
    }
    LoftOp* op = held;
    op->clearProfiles();
    for (const LoftSection& sec : m_loftSections) {
        // Flip reverses the wire's vertex order so it pairs differently
        // against its neighbours — the standard remedy for the "apex pinch /
        // twist" output when start vertices are misaligned. Holes reverse
        // with it, so inner channels pair consistently.
        if (sec.reverse) {
            std::vector<TopoDS_Wire> holes;
            holes.reserve(sec.holes.size());
            for (const auto& h : sec.holes)
                holes.push_back(TopoDS::Wire(h.Reversed()));
            op->addProfile(TopoDS::Wire(sec.outer.Reversed()), holes);
        } else {
            op->addProfile(sec.outer, sec.holes);
        }
    }
    op->setSolid(m_loftSolid);
    op->setRuled(m_loftRuled);
    if (m_loftBridgeBodyId >= 0)
        op->setBridge(m_loftBridgeBodyId, m_loftBridgeFaces);
    m_loftPreview.apply(*m_document);
    m_meshesDirty = true;
}

void Application::commitLoft() {
    // Record the already-applied instance without re-running it.
    m_loftPreview.commit(*m_history);
    m_loftActive = false;
    m_loftSections.clear();
    m_loftRails.clear();
    m_loftRailsMode = false;
    m_meshesDirty = true;
}

// ─── Cascade: re-execute Extrudes that consumed a just-edited sketch ───────
//
// Triggered by SketchEditedEvent. Walks the live history forward and, for
// every enabled ExtrudeOp whose source sketch matches, rebuilds the profile
// from the current sketch state and re-runs execute() — which uses
// addOrPutBody under the hood, so the resulting body keeps the same id and
// the user sees its shape morph in place.
//
// We deliberately stop at Extrude. Downstream ops (Fillet, Chamfer, Pattern,
// Mirror, Push/Pull) reference faces / edges of the extruded body — when
// the body's topology shifts, those references go stale (the toponaming
// problem). Re-running them blindly would produce wrong-edge fillets or
// outright crashes. For the user's "edit a dimension and watch the prism
// follow" workflow this trade-off is fine: simple chains just work; chained
// workflows leave the downstream ops on the stale body and the user
// manually re-runs them.
const std::map<int, std::set<int>>& Application::sketchBodyLinks() const {
    // Memoized on the history revision — the Properties panel reads the link
    // hint every frame a body/sketch is selected, and this walk (dynamic_cast
    // + captureDiff per step, fresh map/set nodes) was running per frame.
    if (m_history && m_linkMapRevision == m_history->revision())
        return m_linkMapCache;
    std::map<int, std::set<int>>& links = m_linkMapCache;
    links.clear();
    if (!m_history) return links;
    m_linkMapRevision = m_history->revision();
    int n = m_history->stepCount();
    for (int i = 0; i < n; ++i) {
        const Operation* op = m_history->getStep(i);
        if (!op) continue;
        std::set<int> srcSketches;
        if (auto* ext = dynamic_cast<const ExtrudeOp*>(op)) {
            if (ext->getSketchId() >= 0) srcSketches.insert(ext->getSketchId());
        } else if (auto* pp = dynamic_cast<const PushPullOp*>(op)) {
            for (int t = 0; t < pp->targetCount(); ++t)
                if (pp->getSketchIdAt(t) >= 0) srcSketches.insert(pp->getSketchIdAt(t));
        }
        if (srcSketches.empty()) continue;
        // Bodies this step touched = created + modified.
        OperationDiff diff = op->captureDiff();
        std::set<int> bodies(diff.created.begin(), diff.created.end());
        for (const auto& [id, _] : diff.modifiedBefore) bodies.insert(id);
        for (int s : srcSketches)
            links[s].insert(bodies.begin(), bodies.end());
    }
    return links;
}

bool Application::bodySafelyRederivable(int bodyId, int viaSketchId) const {
    if (!m_history) return false;
    int n = m_history->stepCount();
    for (int i = 0; i < n; ++i) {
        const Operation* op = m_history->getStep(i);
        if (!op || !op->isEnabled()) continue;
        OperationDiff d = op->captureDiff();
        bool touches = false;
        for (int c : d.created) if (c == bodyId) { touches = true; break; }
        if (!touches)
            for (const auto& [id, _] : d.modifiedBefore)
                if (id == bodyId) { touches = true; break; }
        if (!touches) continue;
        // The only op allowed to touch this body is its own sketch's extrude /
        // push-pull. Anything else (fillet, chamfer, boolean, a second feature,
        // a transform) means re-deriving at a new position would break — not safe.
        if (auto* ext = dynamic_cast<const ExtrudeOp*>(op)) {
            if (ext->getSketchId() == viaSketchId) continue;
        } else if (auto* pp = dynamic_cast<const PushPullOp*>(op)) {
            bool fromSketch = false;
            for (int t = 0; t < pp->targetCount(); ++t)
                if (pp->getSketchIdAt(t) == viaSketchId) { fromSketch = true; break; }
            if (fromSketch) continue;
        }
        return false;
    }
    return true;
}

void Application::relinkSketch(bool isBody, int id) {
    if (!m_document) return;
    std::vector<int> sketches;
    if (isBody) {
        const auto& links = sketchBodyLinks();
        for (const auto& [sid, bodies] : links)
            if (bodies.count(id)) sketches.push_back(sid);
    } else {
        sketches.push_back(id);
    }
    bool changed = false;
    for (int sid : sketches)
        if (auto sk = m_document->getSketch(sid); sk && sk->isDetachedFromBody()) {
            sk->setDetachedFromBody(false);
            changed = true;
        }
    if (changed) {
        markDirty();
        m_meshesDirty = true;
        showToast("Sketch re-linked — editing it will drive the body again.");
    }
}

std::string Application::linkHintFor(bool isBody, int id) const {
    if (!m_document) return "";
    const auto& links = sketchBodyLinks();
    auto nameList = [&](const std::set<int>& ids, bool bodies) {
        std::string s;
        for (int v : ids) {
            if (!s.empty()) s += ", ";
            s += bodies ? m_document->getBodyName(v) : m_document->getSketchName(v);
        }
        return s;
    };
    if (isBody) {
        std::set<int> live, detached;
        for (const auto& [sid, bodyIds] : links) {
            if (!bodyIds.count(id)) continue;
            auto sk = m_document->getSketch(sid);
            (sk && sk->isDetachedFromBody() ? detached : live).insert(sid);
        }
        if (live.empty() && detached.empty()) return "";
        if (!live.empty())
            return std::string(materializr::tr("Built from ")) + nameList(live, false) +
                   materializr::tr(" — editing it updates this body.");
        return std::string(materializr::tr("Detached from ")) + nameList(detached, false) +
               materializr::tr(" — moved independently; sketch edits won't update this body.");
    }
    // Sketch: what body it drives + whether it's detached.
    auto it = links.find(id);
    if (it == links.end() || it->second.empty()) return "";
    auto sk = m_document->getSketch(id);
    std::string bodies = nameList(it->second, true);
    if (sk && sk->isDetachedFromBody())
        return std::string(materializr::tr("Detached — moved independently; edits won't update ")) +
               bodies + ".";
    return std::string(materializr::tr("Drives ")) + bodies +
           materializr::tr(" — editing this sketch updates it.");
}

void Application::cascadeFromSketchEdit(int sketchId) {
    if (sketchId < 0 || !m_history || !m_document) return;
    // A detached sketch has been deliberately broken out of unison with its
    // body (moved on its own in 3D) — editing it must NOT retro-drive the body.
    if (auto sk = m_document->getSketch(sketchId); sk && sk->isDetachedFromBody())
        return;
    int n = m_history->stepCount();

    // Re-derive the profile of every sketch-sourced extrude / push-pull that
    // references the edited sketch, and remember the EARLIEST such step. We do
    // NOT execute them in isolation here — that used to overwrite the body with
    // just the bare extrude, discarding everything downstream (hollows,
    // fillets) and leaving a "cube with N holes". Instead we replay the whole
    // chain from the earliest affected step below.
    int earliest = -1, matched = 0;
    for (int i = 0; i < n; ++i) {
        Operation* op = const_cast<Operation*>(m_history->getStep(i));
        if (!op || !op->isEnabled()) continue;
        if (auto* ext = dynamic_cast<ExtrudeOp*>(op)) {
            if (ext->getSketchId() != sketchId) continue;
            ++matched;
            if (ext->rebuildProfileFromSketch(*m_document) && earliest < 0) earliest = i;
        } else if (auto* pp = dynamic_cast<PushPullOp*>(op)) {
            bool refs = false;
            int tc = pp->targetCount();
            for (int t = 0; t < tc; ++t)
                if (pp->getSketchIdAt(t) == sketchId) { refs = true; break; }
            if (!refs) continue;
            ++matched;
            if (pp->rebuildProfileFromSketch(*m_document, sketchId) && earliest < 0)
                earliest = i;
        }
    }
    if (earliest < 0) {
        std::fprintf(stderr, "[Cascade] sketchId=%d: %d matched, none re-derivable\n",
                     sketchId, matched);
        if (matched > 0) {
            // A body IS driven by this sketch, but its profile couldn't be
            // re-derived from the new geometry — tell the user instead of
            // silently leaving the sketch changed and the body stale.
            showToast("Updated the sketch, but the body built from it couldn't "
                      "rebuild from the new shape \xE2\x80\x94 the model is unchanged.");
        }
        // matched == 0: nothing in the model is built from this sketch (e.g.
        // editing a freshly-duplicated sketch before it's extruded). That's the
        // normal case while sketching — stay silent, just refresh.
        m_meshesDirty = true;
        return;
    }

    // Replay the chain from the earliest re-derived op forward, TRANSACTIONALLY:
    // the new profiles take effect and ALL downstream ops re-run on the updated
    // geometry. If any can't follow (e.g. a fillet whose edge no longer exists
    // after the change), the entire model is restored — never half-built.
    //
    // Snapshot every body's shape BEFORE the replay so we can re-tessellate
    // ONLY the bodies it actually changes — not the whole scene. On a multi-
    // body project a sketch edit touching one body would otherwise re-mesh
    // every body, which is the dominant cost on a tablet. A re-executed op
    // hands its bodies a fresh TShape, so IsEqual cleanly separates changed
    // bodies from untouched ones; created/removed ids are covered below.
    std::map<int, TopoDS_Shape> beforeBodies;
    for (int id : m_document->getAllBodyIds())
        beforeBodies[id] = m_document->getBody(id);

    // Pin the edited sketch's FINAL state for the replay: re-executing the
    // chain rolls the live sketch back through its SketchEditOp snapshots, so
    // mid-replay it holds a STALE state — while the extrude below was rebuilt
    // from the final one. A fillet/chamfer re-finding its edges from "the
    // sketch the user just edited" (generative anchors) must read the final
    // state or it looks for the old geometry and fails every time.
    if (auto sk = m_document->getSketch(sketchId))
        m_document->setCascadeSketchOverride(
            sketchId, std::make_shared<materializr::Sketch>(*sk));
    bool ok = m_history->editStep(earliest, *m_document, /*transactional=*/true);
    // A step that can't follow the change (its geometry no longer exists on
    // the re-derived body) used to revert the WHOLE edit behind a message
    // that guessed at the culprit. Instead: disable the failing step, retry,
    // and tell the user exactly which feature to re-apply (#53). Bounded —
    // if several steps fail we stop rather than gut the history.
    std::vector<int> disabledSteps;
    while (!ok && m_history->lastEditFailStep() >= 0 &&
           disabledSteps.size() < 8) {
        const int bad = m_history->lastEditFailStep();
        const Operation* op = m_history->getStep(bad);
        if (!op) break;
        std::fprintf(stderr, "[Cascade] disabling step %d (%s) and retrying\n",
                     bad, op->name().c_str());
        m_history->setStepEnabled(bad, false, *m_document);
        disabledSteps.push_back(bad);
        ok = m_history->editStep(earliest, *m_document, /*transactional=*/true);
    }
    if (!ok && !disabledSteps.empty()) {
        // Still failing — restore what we disabled and fall back to a clean
        // full revert (never leave the history silently gutted).
        for (int i : disabledSteps)
            m_history->setStepEnabled(i, true, *m_document);
        disabledSteps.clear();
    }
    m_document->clearCascadeSketchOverrides();
    std::fprintf(stderr, "[Cascade] sketchId=%d replay from step %d: %s\n",
                 sketchId, earliest, ok ? "applied" : "reverted");
    if (!ok) {
        std::string culprit;
        if (m_history->lastEditFailStep() >= 0) {
            if (const Operation* op =
                    m_history->getStep(m_history->lastEditFailStep()))
                culprit = " (step " +
                          std::to_string(m_history->lastEditFailStep() + 1) +
                          ": " + op->description() + ")";
        }
        showToast("Couldn't update the model for that sketch change \xE2\x80\x94 a "
                  "downstream feature" + culprit + " couldn't follow it, so "
                  "the model was left unchanged.");
    } else if (!disabledSteps.empty()) {
        std::string names;
        for (size_t i = 0; i < disabledSteps.size(); ++i) {
            const Operation* op = m_history->getStep(disabledSteps[i]);
            names += (i ? ", " : "") + std::string("step ") +
                     std::to_string(disabledSteps[i] + 1) +
                     (op ? " (" + op->description() + ")" : "");
        }
        showToast("Model updated \xE2\x80\x94 but " + names +
                  " couldn't follow the change and was DISABLED. Its edge/face "
                  "picks no longer exist on the new shape \xE2\x80\x94 delete "
                  "it and re-apply the feature (re-enabling would retry the "
                  "old picks).", 9.0);
    }

    // Partial remesh: mark only bodies whose shape changed, plus any that were
    // created or removed. On a failed (reverted) replay every body is restored
    // to its snapshot TShape, so nothing is marked — no needless remesh at all.
    std::set<int> nowIds;
    for (int id : m_document->getAllBodyIds()) {
        nowIds.insert(id);
        auto it = beforeBodies.find(id);
        if (it == beforeBodies.end() ||
            !it->second.IsEqual(m_document->getBody(id)))
            m_dirtyBodyIds.insert(id);            // changed or newly created
    }
    for (const auto& [id, shp] : beforeBodies)
        if (!nowIds.count(id)) m_dirtyBodyIds.insert(id); // removed → mesh cleared
}

void Application::cancelLoft() {
    m_loftPreview.clear(*m_document);
    m_loftActive = false;
    m_loftSections.clear();
    m_loftRails.clear();
    m_loftRailsMode = false;
    m_meshesDirty = true;
}


// ─── Boundary Fill (interactive popup) ──────────────────────────────────────
//
// BoundaryFillPlugin fires requestInteractiveOp(InteractiveOp::BoundaryFill) with 2+
// closed sketches selected. Same live-preview scaffolding as Loft: one
// BoundaryFillOp is pushed as the preview, re-pushed on toggle, committed on
// Apply, undone on Cancel.

void Application::beginBoundaryFill() {
    if (refuseMeshSelection("Boundary Fill")) return;
    if (!m_selection || !m_document) return;

    std::vector<int> sketchIds;
    auto addId = [&](int id) {
        if (id < 0) return;
        for (int x : sketchIds) if (x == id) return;
        sketchIds.push_back(id);
    };
    for (const auto& e : m_selection->getSelection()) {
        if ((e.type == SelectionType::Sketch ||
             e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
            addId(e.sketchId);
        }
    }

    m_bfillProfiles.clear();
    for (int id : sketchIds) {
        auto sk = m_document->getSketch(id);
        if (!sk) continue;
        BFillProfile prof;
        prof.sketchId = id;
        bool fromRegion = false;
        prof.outer = outermostRegionWire(sk.get(), prof.holes, &fromRegion);
        if (prof.outer.IsNull() || !fromRegion) {
            std::fprintf(stderr,
                "[BoundaryFill] sketch %d has no closed region — skipped.\n", id);
            continue;
        }
        prof.plane = sk->getPlane();
        m_bfillProfiles.push_back(std::move(prof));
    }
    if (m_bfillProfiles.size() < 2) {
        m_bfillProfiles.clear();
        showToast("Boundary Fill needs at least two sketches with a closed "
                  "region (e.g. top + front + side silhouettes).");
        return;
    }

    m_bfillPreview.clear(*m_document);
    m_bfillActive = true;
    updateBoundaryFill();
}

void Application::updateBoundaryFill() {
    if (!m_bfillActive || !m_history || !m_document) return;
    if (m_bfillProfiles.size() < 2) return;

    // Retract the applied preview, re-sync the SAME instance, re-execute.
    // History sees nothing until commit — see LiveOpPreview for the three
    // bugs the old push-a-real-step-per-frame version carried.
    m_bfillPreview.retract(*m_document);
    if (!m_bfillPreview.op())
        m_bfillPreview.hold(std::make_unique<BoundaryFillOp>(), *m_document);
    auto* op = static_cast<BoundaryFillOp*>(m_bfillPreview.op());
    op->clearProfiles();
    for (const BFillProfile& p : m_bfillProfiles)
        op->addProfile(p.outer, p.holes, p.plane);
    if (!m_bfillPreview.apply(*m_document))
        showToast("Boundary Fill: the silhouettes don't enclose a common "
                  "volume - make sure they overlap in space.");
    m_meshesDirty = true;
}

void Application::commitBoundaryFill() {
    m_bfillPreview.commit(*m_history);   // record the applied instance as-is
    m_bfillActive = false;
    m_bfillProfiles.clear();
    m_meshesDirty = true;
}

void Application::cancelBoundaryFill() {
    m_bfillPreview.clear(*m_document);
    m_bfillActive = false;
    m_bfillProfiles.clear();
    m_meshesDirty = true;
}

// ─── Patch (interactive popup) ──────────────────────────────────────────────
//
// PatchPlugin fires requestInteractiveOp(InteractiveOp::Patch) with edges
// selected. Same live-preview scaffolding as Loft and Boundary Fill: one
// PatchOp is held against the document, re-synced on every parameter change,
// committed on Apply, undone on Cancel.
//
// The op wants a body id so it can find each boundary edge's neighbouring face
// (the thing tangency is measured against) and so it can sew the finished patch
// back in. Edges from two different bodies have no single such body, so the
// patch is fitted as a standalone surface instead — which is the honest answer
// for geometry that bridges a gap rather than filling one.

void Application::beginPatch() {
    if (refuseMeshSelection("Patch")) return;
    if (!m_selection || !m_document) return;

    m_patchEdges.clear();
    m_patchSupports.clear();
    m_patchBodyId = -1;

    bool oneBody = true;
    for (const auto& e : m_selection->getSelection()) {
        if (e.shape.IsNull()) continue;
        if (e.type == SelectionType::Edge && e.shape.ShapeType() == TopAbs_EDGE) {
            bool dup = false;
            for (const auto& have : m_patchEdges)
                if (have.IsSame(e.shape)) { dup = true; break; }
            if (dup) continue;
            m_patchEdges.push_back(TopoDS::Edge(e.shape));
            if (e.bodyId >= 0) {
                if (m_patchBodyId < 0) m_patchBodyId = e.bodyId;
                else if (m_patchBodyId != e.bodyId) oneBody = false;
            }
        } else if (e.type == SelectionType::Face &&
                   e.shape.ShapeType() == TopAbs_FACE) {
            m_patchSupports.push_back(TopoDS::Face(e.shape));
        }
    }

    if (m_patchEdges.empty()) {
        showToast("Patch needs the edges around the opening - Ctrl-click each "
                  "one, then click Patch.");
        return;
    }
    // Edges spanning two bodies: keep them, drop the heal target. The fit still
    // works; it just can't sew into a body that only owns half the boundary.
    if (!oneBody) m_patchBodyId = -1;

    m_patchPreview.clear(*m_document);
    m_patchParams = PatchParams{};
    m_patchShowAdvanced = false;
    m_patchActive = true;
    updatePatch();
}

void Application::updatePatch() {
    if (!m_patchActive || !m_history || !m_document) return;
    if (m_patchEdges.empty()) return;

    // Retract the applied preview, re-sync the SAME instance, re-execute —
    // History sees nothing until commit. See LiveOpPreview.
    m_patchPreview.retract(*m_document);
    if (!m_patchPreview.op())
        m_patchPreview.hold(std::make_unique<PatchOp>(), *m_document);
    auto* op = static_cast<PatchOp*>(m_patchPreview.op());

    op->setBody(m_patchBodyId);
    op->setEdges(m_patchEdges);
    op->setSupportFaces(m_patchSupports);
    op->setContinuity(static_cast<PatchOp::Continuity>(m_patchParams.continuity));

    PatchOp::Solver s;
    s.degree      = m_patchParams.degree;
    s.nbPtsOnCur  = m_patchParams.nbPtsOnCur;
    s.nbIter      = m_patchParams.nbIter;
    s.anisotropic = m_patchParams.anisotropic;
    s.tol3d       = m_patchParams.tol3d;
    s.tolAng      = m_patchParams.tolAng;
    s.tolCurv     = m_patchParams.tolCurv;
    s.maxDeg      = m_patchParams.maxDeg;
    s.maxSegments = m_patchParams.maxSegments;
    op->setSolver(s);

    if (!m_patchPreview.apply(*m_document))
        showToast("Patch: no surface fits these edges. They need to form one "
                  "closed ring around the opening.");
    m_meshesDirty = true;
}

void Application::commitPatch() {
    m_patchPreview.commit(*m_history);   // record the applied instance as-is
    m_patchActive = false;
    m_patchEdges.clear();
    m_patchSupports.clear();
    m_meshesDirty = true;
}

void Application::cancelPatch() {
    m_patchPreview.clear(*m_document);
    m_patchActive = false;
    m_patchEdges.clear();
    m_patchSupports.clear();
    m_meshesDirty = true;
}

// ─── Sew (one-shot) ─────────────────────────────────────────────────────────

void Application::beginSew() {
    if (refuseMeshSelection("Sew")) return;
    if (!m_selection || !m_document || !m_history) return;

    std::vector<int> ids;
    for (const auto& e : m_selection->getSelection()) {
        if (e.bodyId < 0) continue;
        bool dup = false;
        for (int x : ids) if (x == e.bodyId) { dup = true; break; }
        if (!dup) ids.push_back(e.bodyId);
    }
    if (ids.empty()) {
        showToast("Sew needs the surfaces selected - pick the bodies to stitch "
                  "together, then click Sew.");
        return;
    }

    auto op = std::make_unique<SewOp>();
    op->setBodies(ids);
    SewOp* raw = op.get();
    if (!m_history->pushOperation(std::move(op), *m_document)) {
        showToast("Sew: those surfaces wouldn't join - they may not touch "
                  "anywhere, or there was nothing to stitch.");
        return;
    }

    // Report what it managed, not that it ran. "Closed" and "still open" are
    // different outcomes and the second one is actionable — the edge count is
    // how many gaps are left to patch.
    if (raw->madeSolid()) {
        showToast("Sewed " + std::to_string(raw->facesSewn()) +
                  " faces into a solid.");
    } else {
        showToast("Sewed " + std::to_string(raw->facesSewn()) + " faces, but " +
                  std::to_string(raw->freeEdgesLeft()) +
                  " edge(s) are still open - it isn't a solid yet.");
    }
    // The consumed bodies are gone; a selection naming them would resolve to
    // nothing.
    m_selection->clear();
    m_meshesDirty = true;
}

// ─── Construction Plane (interactive popup) ────────────────────────────────
//
// Same architecture as Loft: ConstructionPlanePlugin fires a plain
// requestInteractiveOp(InteractiveOp::ConstructionPlane). Application reads the current
// selection (a planar face unlocks the "Parallel to face" option), opens a
// live-previewed popup with XY/XZ/YZ + offset, then commits a single
// ConstructionPlaneOp on Apply or undoes the preview on Cancel.

void Application::beginConstructionPlane() {
    if (!m_history || !m_document) return;

    m_planeOpHaveFace = false;
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::Face && !e.shape.IsNull() &&
                e.shape.ShapeType() == TopAbs_FACE) {
                try {
                    TopoDS_Face f = TopoDS::Face(e.shape);
                    Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
                    if (!surf.IsNull() && surf->IsKind(STANDARD_TYPE(Geom_Plane))) {
                        m_planeOpBaseFace = Handle(Geom_Plane)::DownCast(surf)->Pln();
                        m_planeOpHaveFace = true;
                        break;
                    }
                } catch (...) {}
            }
        }
    }

    // Gather inputs for the derived modes (Midplane / Normal-to-Axis /
    // Tangent-to-Cylinder) from the selection in one pass.
    m_planeOpHaveTwoPlanes = false;
    m_planeOpHaveAxis = false;
    m_planeOpHaveCylinder = false;
    {
        std::vector<gp_Pln> planarPlanes;        // planar faces + construction planes
        std::vector<gp_Ax1> axes;                // construction axes + straight edges
        struct CylInfo { gp_Ax1 axis; double radius; };
        std::vector<CylInfo> cylinders;
        std::vector<gp_Pnt> vertices;
        if (m_selection) {
            for (const auto& e : m_selection->getSelection()) {
                if (e.type == SelectionType::Plane && e.planeId >= 0) {
                    if (const auto* pe = m_document->getPlane(e.planeId))
                        planarPlanes.push_back(pe->plane);
                    continue;
                }
                if (e.type == SelectionType::Axis && e.axisId >= 0) {
                    if (const auto* a = m_document->getAxis(e.axisId))
                        axes.emplace_back(a->origin, a->direction);
                    continue;
                }
                if (e.shape.IsNull()) continue;
                try {
                    if (e.type == SelectionType::Face && e.shape.ShapeType() == TopAbs_FACE) {
                        Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(e.shape));
                        if (!s.IsNull() && s->IsKind(STANDARD_TYPE(Geom_Plane))) {
                            planarPlanes.push_back(Handle(Geom_Plane)::DownCast(s)->Pln());
                        } else {
                            Handle(Geom_CylindricalSurface) cyl =
                                Handle(Geom_CylindricalSurface)::DownCast(s);
                            if (!cyl.IsNull())
                                cylinders.push_back({gp_Ax1(cyl->Cylinder().Position().Location(),
                                                            cyl->Cylinder().Position().Direction()),
                                                     cyl->Cylinder().Radius()});
                        }
                    } else if (e.type == SelectionType::Edge) {
                        BRepAdaptor_Curve adaptor(TopoDS::Edge(e.shape));
                        if (adaptor.GetType() == GeomAbs_Line)
                            axes.push_back(adaptor.Line().Position());
                    } else if (e.type == SelectionType::Vertex) {
                        vertices.push_back(BRep_Tool::Pnt(TopoDS::Vertex(e.shape)));
                    }
                } catch (...) {}
            }
        }

        if (planarPlanes.size() >= 2) {
            m_planeOpHaveTwoPlanes = true;
            m_planeOpPlaneA = planarPlanes[0];
            m_planeOpPlaneB = planarPlanes[1];
        }
        if (!axes.empty()) {
            m_planeOpHaveAxis = true;
            m_planeOpAxis = axes[0];
            m_planeOpAxisPoint = vertices.empty() ? axes[0].Location() : vertices[0];
        }
        if (!cylinders.empty()) {
            m_planeOpHaveCylinder = true;
            m_planeOpCylAxis = cylinders[0].axis;
            m_planeOpCylRadius = cylinders[0].radius;
            // Side reference for the tangent: radial toward a selected vertex,
            // else a second plane's normal, else world +X.
            if (!vertices.empty()) {
                gp_Vec v(m_planeOpCylAxis.Location(), vertices[0]);
                m_planeOpCylRefDir = (v.Magnitude() > 1e-9) ? gp_Dir(v) : gp_Dir(1, 0, 0);
            } else if (!planarPlanes.empty()) {
                m_planeOpCylRefDir = planarPlanes[0].Axis().Direction();
            } else {
                m_planeOpCylRefDir = gp_Dir(1, 0, 0);
            }
        }
    }

    // A cylindrical face also yields an axis (its centreline), so the
    // Normal-to-Axis mode can build a cross-section plane perpendicular to
    // the cylinder without a separate construction axis. Only fill it in when
    // no explicit axis/edge was selected.
    if (!m_planeOpHaveAxis && m_planeOpHaveCylinder) {
        m_planeOpHaveAxis = true;
        m_planeOpAxis = m_planeOpCylAxis;
        m_planeOpAxisPoint = m_planeOpCylAxis.Location();
    }

    // Pick the most likely mode from what's selected so the common "select
    // geometry, click the button" flow lands ready-to-go: two planes →
    // Midplane; a planar face → Parallel-to-face; a cylinder → Tangent;
    // an axis/edge → Normal-to-axis; nothing relevant → XY.
    if      (m_planeOpHaveTwoPlanes) m_planeOpKindIdx = 4;  // 2 faces → midplane intent
    else if (m_planeOpHaveFace)      m_planeOpKindIdx = 3;
    else if (m_planeOpHaveCylinder)  m_planeOpKindIdx = 6;  // cylinder → tangent default
    else if (m_planeOpHaveAxis)      m_planeOpKindIdx = 5;
    else                             m_planeOpKindIdx = 0;
    m_planeOpOffset = 0.0;
    materializr::formatLengthDigits(m_planeOpOffsetBuf, sizeof(m_planeOpOffsetBuf), m_planeOpOffset);
    m_planeOpPreview.clear(*m_document);
    m_planeOpActive = true;

    updateConstructionPlane();
}

void Application::beginConstructionPlaneMode(int kindIdx) {
    // Open the plane popup (captures selection, pushes a default preview) then
    // force the explicitly-requested mode. The sidebar only offers a mode when
    // its inputs are present, so the override is always valid.
    beginConstructionPlane();
    if (!m_planeOpActive) return;
    m_planeOpKindIdx = kindIdx;
    updateConstructionPlane();
}

void Application::updateConstructionPlane() {
    if (!m_planeOpActive || !m_history || !m_document) return;

    // Retract the applied preview before building the next one. The op is
    // rebuilt per frame (its parameters vary by kind, not by setter), but it
    // never reaches History until commit — see LiveOpPreview.
    m_planeOpPreview.retract(*m_document);

    auto op = std::make_unique<ConstructionPlaneOp>();
    switch (m_planeOpKindIdx) {
        case 0: op->setType(PlaneCreationType::XY);
                op->setOffset(m_planeOpOffset);
                op->setName("XY Plane"); break;
        case 1: op->setType(PlaneCreationType::XZ);
                op->setOffset(m_planeOpOffset);
                op->setName("XZ Plane"); break;
        case 2: op->setType(PlaneCreationType::YZ);
                op->setOffset(m_planeOpOffset);
                op->setName("YZ Plane"); break;
        case 3:
            if (m_planeOpHaveFace) {
                // ParallelToFace places the plane at p1 with the base plane's
                // normal — push p1 along that normal by the offset so the
                // slider drives it away from the face like the user expects.
                gp_Dir n = m_planeOpBaseFace.Axis().Direction();
                gp_Pnt o = m_planeOpBaseFace.Axis().Location();
                gp_Pnt p(o.X() + n.X() * m_planeOpOffset,
                         o.Y() + n.Y() * m_planeOpOffset,
                         o.Z() + n.Z() * m_planeOpOffset);
                op->setBasePlane(m_planeOpBaseFace);
                op->setPoints(p, gp_Pnt(0,0,0), gp_Pnt(0,0,0));
                op->setType(PlaneCreationType::ParallelToFace);
                op->setName("Face Plane");
            } else {
                op->setType(PlaneCreationType::XY);
                op->setName("XY Plane");
            }
            break;
        case 4:   // Midplane
        case 5:   // Normal to axis / edge
        case 6:   // Tangent to cylinder
        case 7: { // Through (containing) the cylinder axis
            gp_Dir N; gp_Pnt P0;
            if (computeDerivedPlaneNP(m_planeOpKindIdx, N, P0)) {
                // Push the through-point along N by the offset so the slider
                // shifts the result off the derived position, reusing the
                // ParallelToFace (normal + point) form.
                gp_Pnt p(P0.X() + N.X() * m_planeOpOffset,
                         P0.Y() + N.Y() * m_planeOpOffset,
                         P0.Z() + N.Z() * m_planeOpOffset);
                op->setBasePlane(gp_Pln(P0, N));
                op->setPoints(p, gp_Pnt(0, 0, 0), gp_Pnt(0, 0, 0));
                if (m_planeOpKindIdx == 4) {
                    op->setType(PlaneCreationType::Midplane);     op->setName("Midplane");
                } else if (m_planeOpKindIdx == 5) {
                    op->setType(PlaneCreationType::NormalToAxis); op->setName("Normal-to-Axis Plane");
                } else if (m_planeOpKindIdx == 6) {
                    op->setType(PlaneCreationType::TangentToCylinder); op->setName("Tangent Plane");
                } else {
                    op->setType(PlaneCreationType::ThroughAxis);  op->setName("Through-Axis Plane");
                }
            } else {
                op->setType(PlaneCreationType::XY);
                op->setName("XY Plane");
            }
            break;
        }
    }
    m_planeOpPreview.hold(std::move(op), *m_document);
    if (m_planeOpPreview.apply(*m_document)) {
        // Auto-select the freshly-previewed plane so the move/rotate gizmo
        // appears on it. ConstructionPlaneOp::execute push_backs to
        // Document, so the just-added id is the back of getAllPlaneIds().
        auto ids = m_document->getAllPlaneIds();
        if (!ids.empty()) {
            SelectionEntry e;
            e.type = SelectionType::Plane;
            e.planeId = ids.back();
            m_selection->select(e);
        }
    }
    // The preview plane is a NEW plane every time, so both the pending image
    // and the rotation have to follow it -- otherwise the photo vanished and
    // the tilt silently reset the moment the user changed the alignment or
    // nudged the offset.
    applyPlaneOpRotation();
    reattachPlaneOpRefImage();
    m_meshesDirty = true;
}

// Re-apply the ABSOLUTE rotation to the freshly rebuilt preview plane. Applied
// X, then Y, then Z about the plane's own origin, so the three fields describe
// one orientation rather than a history of nudges.
void Application::applyPlaneOpRotation() {
    if (!m_document) return;
    if (std::abs(m_planeOpRotX) <= 1e-4f && std::abs(m_planeOpRotY) <= 1e-4f &&
        std::abs(m_planeOpRotZ) <= 1e-4f)
        return;
    const auto ids = m_document->getAllPlaneIds();
    if (ids.empty()) return;
    const int pid = ids.back();
    const auto* entry = m_document->getPlane(pid);
    if (!entry) return;
    gp_Pln pln = entry->plane;
    const gp_Pnt o = pln.Position().Location();
    // User Z-up display convention: user Y = world Z, user Z = world Y --
    // the same mapping the Origin / Normal readouts use.
    const std::pair<gp_Dir, float> spins[3] = {
        { gp_Dir(1, 0, 0), m_planeOpRotX },
        { gp_Dir(0, 0, 1), m_planeOpRotY },
        { gp_Dir(0, 1, 0), m_planeOpRotZ },
    };
    for (const auto& [axis, deg] : spins) {
        if (std::abs(deg) <= 1e-4f) continue;
        gp_Trsf t;
        t.SetRotation(gp_Ax1(o, axis), deg * M_PI / 180.0);
        pln.Transform(t);
    }
    m_document->setPlane(pid, pln);
}

// Put the pending image on whatever plane the preview currently is.
void Application::reattachPlaneOpRefImage() {
    if (!m_planeOpHasPendingImage || !m_document) return;
    const auto ids = m_document->getAllPlaneIds();
    if (ids.empty()) return;
    m_document->setRefImage(ids.back(), m_planeOpPendingImage);
    m_meshesDirty = true;
}

// Choose the file for a plane being created. Attaching immediately is the
// point: the photo renders on the live preview plane, so alignment, offset and
// calibration all happen with it visible rather than after the fact.
void Application::pickPlaneOpRefImage() {
    materializr::FileDialogs::openFile(
        "Reference Image",
        {{"Images", "*.png *.jpg *.jpeg *.bmp *.PNG *.JPG *.JPEG *.BMP"}},
        [this](const std::string& path) {
            RefImageEntry e;
            std::string base;
            if (!loadRefImageFile(path, e, base)) {
                m_planeOpWantRefImage = false;   // nothing loaded; untick
                return;
            }
            m_planeOpPendingImage = std::move(e);
            m_planeOpHasPendingImage = true;
            reattachPlaneOpRefImage();
        });
}

void Application::commitConstructionPlane() {
    // The image is already on the preview plane (re-attached after every
    // apply), so commit just keeps it and clears the staging state.
    m_planeOpWantRefImage = false;
    m_planeOpHasPendingImage = false;
    m_planeOpPendingImage = RefImageEntry{};
    m_planeOpRotX = m_planeOpRotY = m_planeOpRotZ = 0.0f;
    m_planeOpPreview.commit(*m_history);
    m_planeOpActive = false;
    m_meshesDirty = true;

    // The plane now exists and previewApply auto-selected it, so the id is the
    // most recent one. Attaching here (rather than inside the op) keeps the
    // image bytes out of the history step while still letting ANY plane type
    // carry one.
}

void Application::cancelConstructionPlane() {
    m_planeOpRotX = m_planeOpRotY = m_planeOpRotZ = 0.0f;
    m_planeOpWantRefImage = false;
    m_planeOpHasPendingImage = false;
    m_planeOpPendingImage = RefImageEntry{};
    m_planeOpPreview.clear(*m_document);
    m_planeOpActive = false;
    m_meshesDirty = true;
}

void Application::beginPrimitivePopup(int kindIdx) {
    cancelAllInteractivePreviews();
    m_primitivePopupActive = true;
    m_primitivePopupKind   = kindIdx;
    // Per-kind defaults so opening a fresh popup always starts at a sensible
    // size (kindIdx switches via the toolbar buttons; we re-seed the kind-
    // specific fields without touching the others, which keeps any custom
    // origin / extents the user last typed when they reopen the same kind).
    m_primitivePopupOrigin[0] = 0.0;
    m_primitivePopupOrigin[1] = 0.0;
    m_primitivePopupOrigin[2] = 0.0;
    switch (kindIdx) {
    case 0: // Box
        m_primitivePopupExtents[0] = 10.0;
        m_primitivePopupExtents[1] = 10.0;
        m_primitivePopupExtents[2] = 10.0;
        break;
    case 1: // Cylinder
        m_primitivePopupRadius = 5.0;
        m_primitivePopupHeight = 10.0;
        break;
    case 2: // Sphere
        m_primitivePopupRadius = 5.0;
        break;
    case 3: // Cone
        m_primitivePopupRadius      = 5.0;
        m_primitivePopupTopRadius   = 0.0;
        m_primitivePopupHeight      = 10.0;
        break;
    case 4: // Torus
        m_primitivePopupRadius      = 5.0;
        m_primitivePopupMinorRadius = 2.0;
        break;
    }
}

void Application::commitPrimitivePopup() {
    using K = materializr::PrimitiveOp::Kind;
    auto op = std::make_unique<materializr::PrimitiveOp>();
    K kind = K::Box;
    switch (m_primitivePopupKind) {
        case 0: kind = K::Box;      break;
        case 1: kind = K::Cylinder; break;
        case 2: kind = K::Sphere;   break;
        case 3: kind = K::Cone;     break;
        case 4: kind = K::Torus;    break;
    }
    op->setKind(kind);
    op->setOrigin(m_primitivePopupOrigin[0],
                  m_primitivePopupOrigin[1],
                  m_primitivePopupOrigin[2]);
    op->setBoxExtents(m_primitivePopupExtents[0],
                      m_primitivePopupExtents[1],
                      m_primitivePopupExtents[2]);
    op->setRadius(m_primitivePopupRadius);
    op->setHeight(m_primitivePopupHeight);
    op->setTopRadius(m_primitivePopupTopRadius);
    op->setMinorRadius(m_primitivePopupMinorRadius);
    if (m_history->pushOperation(std::move(op), *m_document)) {
        auto ids = m_document->getAllBodyIds();
        if (!ids.empty()) {
            SelectionEntry e;
            e.type = SelectionType::Body;
            e.bodyId = ids.back();
            m_selection->select(e);
        }
        m_meshesDirty = true;
    }
    m_primitivePopupActive = false;
}

void Application::cancelPrimitivePopup() {
    m_primitivePopupActive = false;
}

bool Application::computeDerivedPlaneNP(int kindIdx, gp_Dir& outNormal,
                                        gp_Pnt& outPoint) const {
    if (kindIdx == 4) { // Midplane — centred between the two captured planes
        if (!m_planeOpHaveTwoPlanes) return false;
        gp_Dir nA = m_planeOpPlaneA.Axis().Direction();
        gp_Dir nB = m_planeOpPlaneB.Axis().Direction();
        // Align B's normal with A before averaging so two faces pointing at
        // each other (antiparallel) don't cancel to a zero vector.
        gp_Vec sum(nA);
        if (nA.Dot(nB) < 0.0) sum -= gp_Vec(nB); else sum += gp_Vec(nB);
        if (sum.Magnitude() < 1e-9) return false;
        outNormal = gp_Dir(sum);
        gp_Pnt a = m_planeOpPlaneA.Axis().Location();
        gp_Pnt b = m_planeOpPlaneB.Axis().Location();
        // Midpoint of the two plane origins sits exactly on the perpendicular
        // midplane (its component along N is the average of the two offsets);
        // the in-plane component is irrelevant to the resulting plane.
        outPoint = gp_Pnt((a.X() + b.X()) * 0.5,
                          (a.Y() + b.Y()) * 0.5,
                          (a.Z() + b.Z()) * 0.5);
        return true;
    }
    if (kindIdx == 5) { // Normal to axis/edge through the captured point
        if (!m_planeOpHaveAxis) return false;
        outNormal = m_planeOpAxis.Direction();
        outPoint  = m_planeOpAxisPoint;
        return true;
    }
    if (kindIdx == 6) { // Tangent to cylinder, on the reference-direction side
        if (!m_planeOpHaveCylinder) return false;
        gp_Vec axv(m_planeOpCylAxis.Direction());
        gp_Vec ref(m_planeOpCylRefDir);
        gp_Vec radial = ref - axv * ref.Dot(axv);   // perp-to-axis component
        if (radial.Magnitude() < 1e-9) {
            // Reference is parallel to the axis — fall back to any perpendicular.
            gp_Ax2 tmp(m_planeOpCylAxis.Location(), m_planeOpCylAxis.Direction());
            radial = gp_Vec(tmp.XDirection());
        }
        outNormal = gp_Dir(radial);
        gp_Pnt c = m_planeOpCylAxis.Location();
        outPoint = gp_Pnt(c.X() + outNormal.X() * m_planeOpCylRadius,
                          c.Y() + outNormal.Y() * m_planeOpCylRadius,
                          c.Z() + outNormal.Z() * m_planeOpCylRadius);
        return true;
    }
    if (kindIdx == 7) { // Plane CONTAINING the cylinder axis (longitudinal)
        if (!m_planeOpHaveCylinder) return false;
        gp_Vec axv(m_planeOpCylAxis.Direction());
        gp_Vec ref(m_planeOpCylRefDir);
        gp_Vec radial = ref - axv * ref.Dot(axv);     // perp-to-axis component
        if (radial.Magnitude() < 1e-9) {
            gp_Ax2 tmp(m_planeOpCylAxis.Location(), m_planeOpCylAxis.Direction());
            radial = gp_Vec(tmp.XDirection());
        }
        // The plane contains both the axis and the radial reference, so its
        // normal is perpendicular to both. Offset (applied by the caller along
        // this normal) slides it to a parallel chord cut.
        gp_Vec nrm = axv.Crossed(radial);
        if (nrm.Magnitude() < 1e-9) return false;
        outNormal = gp_Dir(nrm);
        outPoint  = m_planeOpCylAxis.Location();
        return true;
    }
    return false;
}

// ─── Construction Axis interactive popup ───────────────────────────────────
//
// Same skeleton as the plane popup: begin seeds defaults, update rebuilds
// the ConstructionAxisOp on every radio/value change (preview-undo first),
// commit leaves the last preview in place, cancel undoes the preview.
// Auto-selects the just-pushed axis so the user can immediately pivot to
// the Move gizmo / Revolve later.

void Application::beginConstructionAxis() {
    if (m_axisOpActive) return; // already open
    m_axisOpActive = true;
    m_axisOpOrigin[0] = m_axisOpOrigin[1] = m_axisOpOrigin[2] = 0.0;
    for (int i = 0; i < 3; ++i) {
        materializr::formatLengthDigits(m_axisOpOriginBuf[i], sizeof(m_axisOpOriginBuf[i]), m_axisOpOrigin[i]);
    }

    // Gather selection-derived inputs (cylinder centreline, straight edge,
    // two vertices, a planar face's normal, two planes' intersection).
    m_axisOpHaveCylinder = m_axisOpHaveEdge = m_axisOpHaveTwoVerts = false;
    m_axisOpHaveFaceNormal = m_axisOpHaveTwoPlanes = false;
    {
        std::vector<gp_Pln> planarPlanes;
        std::vector<gp_Pnt> vertices;
        if (m_selection) {
            for (const auto& e : m_selection->getSelection()) {
                if (e.type == SelectionType::Plane && e.planeId >= 0) {
                    if (const auto* pe = m_document->getPlane(e.planeId))
                        planarPlanes.push_back(pe->plane);
                    continue;
                }
                if (e.shape.IsNull()) continue;
                try {
                    if (e.type == SelectionType::Face && e.shape.ShapeType() == TopAbs_FACE) {
                        Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(e.shape));
                        if (s.IsNull()) continue;
                        Handle(Geom_CylindricalSurface) cyl =
                            Handle(Geom_CylindricalSurface)::DownCast(s);
                        if (!cyl.IsNull()) {
                            if (!m_axisOpHaveCylinder) {
                                m_axisOpCylAxis = gp_Ax1(cyl->Cylinder().Position().Location(),
                                                         cyl->Cylinder().Position().Direction());
                                m_axisOpHaveCylinder = true;
                            }
                        } else if (s->IsKind(STANDARD_TYPE(Geom_Plane))) {
                            gp_Pln pln = Handle(Geom_Plane)::DownCast(s)->Pln();
                            planarPlanes.push_back(pln);
                            if (!m_axisOpHaveFaceNormal) {
                                // Anchor at the FACE CENTROID — the plane's
                                // parametric origin is wherever the surface
                                // happens to start, often a corner ("axis on
                                // lower corner instead of straight out of
                                // the face").
                                gp_Pnt anchor = pln.Axis().Location();
                                try {
                                    GProp_GProps pr;
                                    BRepGProp::SurfaceProperties(
                                        TopoDS::Face(e.shape), pr);
                                    anchor = pr.CentreOfMass();
                                } catch (...) {}
                                m_axisOpFacePt = anchor;
                                m_axisOpFaceNormal = pln.Axis().Direction();
                                m_axisOpHaveFaceNormal = true;
                            }
                        }
                    } else if (e.type == SelectionType::Edge) {
                        BRepAdaptor_Curve ad(TopoDS::Edge(e.shape));
                        if (ad.GetType() == GeomAbs_Line && !m_axisOpHaveEdge) {
                            m_axisOpEdgeAxis = ad.Line().Position();
                            m_axisOpHaveEdge = true;
                        }
                    } else if (e.type == SelectionType::Vertex) {
                        vertices.push_back(BRep_Tool::Pnt(TopoDS::Vertex(e.shape)));
                    }
                } catch (...) {}
            }
        }
        if (vertices.size() >= 2) {
            m_axisOpHaveTwoVerts = true;
            m_axisOpV1 = vertices[0]; m_axisOpV2 = vertices[1];
        }
        if (planarPlanes.size() >= 2) {
            m_axisOpHaveTwoPlanes = true;
            m_axisOpPlaneA = planarPlanes[0]; m_axisOpPlaneB = planarPlanes[1];
        }
    }

    // Default to the most likely mode for the selection; else user-Z (up).
    if      (m_axisOpHaveCylinder)   m_axisOpKindIdx = 3;
    else if (m_axisOpHaveEdge)       m_axisOpKindIdx = 4;
    else if (m_axisOpHaveTwoVerts)   m_axisOpKindIdx = 5;
    else if (m_axisOpHaveTwoPlanes)  m_axisOpKindIdx = 7;
    else if (m_axisOpHaveFaceNormal) m_axisOpKindIdx = 6;
    else                             m_axisOpKindIdx = 2;

    m_axisOpPreview.clear(*m_document);
    updateConstructionAxis();
}

void Application::beginConstructionAxisMode(int kindIdx) {
    beginConstructionAxis();
    if (!m_axisOpActive) return;
    m_axisOpKindIdx = kindIdx;   // override the auto-default
    updateConstructionAxis();
}

bool Application::computeDerivedAxisOD(int kindIdx, gp_Pnt& outOrigin,
                                       gp_Dir& outDir) const {
    switch (kindIdx) {
        case 3: // cylinder centreline
            if (!m_axisOpHaveCylinder) return false;
            outOrigin = m_axisOpCylAxis.Location();
            outDir    = m_axisOpCylAxis.Direction();
            return true;
        case 4: // along straight edge
            if (!m_axisOpHaveEdge) return false;
            outOrigin = m_axisOpEdgeAxis.Location();
            outDir    = m_axisOpEdgeAxis.Direction();
            return true;
        case 5: { // through two vertices
            if (!m_axisOpHaveTwoVerts) return false;
            gp_Vec v(m_axisOpV1, m_axisOpV2);
            if (v.Magnitude() < 1e-9) return false;
            outOrigin = m_axisOpV1;
            outDir    = gp_Dir(v);
            return true;
        }
        case 6: // normal to a planar face, through its point
            if (!m_axisOpHaveFaceNormal) return false;
            outOrigin = m_axisOpFacePt;
            outDir    = m_axisOpFaceNormal;
            return true;
        case 7: { // intersection line of two planes
            if (!m_axisOpHaveTwoPlanes) return false;
            IntAna_QuadQuadGeo inter(m_axisOpPlaneA, m_axisOpPlaneB,
                                     Precision::Angular(), Precision::Confusion());
            if (!inter.IsDone() || inter.NbSolutions() < 1) return false;
            gp_Lin line = inter.Line(1);
            outOrigin = line.Location();
            outDir    = line.Direction();
            return true;
        }
        default: return false;
    }
}

void Application::updateConstructionAxis() {
    if (!m_axisOpActive || !m_history || !m_document) return;

    // Undo the previous preview so we don't stack axes on every radio /
    // origin tweak (same dance the plane popup uses).
    m_axisOpPreview.retract(*m_document);

    auto op = std::make_unique<ConstructionAxisOp>();
    if (m_axisOpKindIdx <= 2) {
        // World axes through a typed origin. User-Z-up remap (same as the
        // plane popup): the popup's X / Y / Z labels are in the user's Z-up
        // convention, so user-Y → world-Z (depth) and user-Z → world-Y (up).
        AxisCreationType kind = AxisCreationType::WorldY;
        const char* nm = "Axis";
        switch (m_axisOpKindIdx) {
            case 0: kind = AxisCreationType::WorldX; nm = "X Axis"; break;
            case 1: kind = AxisCreationType::WorldZ; nm = "Y Axis"; break;
            case 2: kind = AxisCreationType::WorldY; nm = "Z Axis"; break;
        }
        op->setType(kind);
        op->setOrigin(gp_Pnt(m_axisOpOrigin[0], m_axisOpOrigin[1], m_axisOpOrigin[2]));
        op->setName(nm);
    } else if (m_axisOpKindIdx == 5) {
        // Two points → TwoPoints (its computeAxis derives direction from p1→p2).
        if (m_axisOpHaveTwoVerts) {
            op->setPoints(m_axisOpV1, m_axisOpV2);
            op->setType(AxisCreationType::TwoPoints);
            op->setName("Axis (2 points)");
        } else {
            op->setType(AxisCreationType::WorldY); op->setName("Z Axis");
        }
    } else {
        // Derived (cylinder / edge / face-normal / plane-intersection): the
        // host resolves (origin, direction); the op passes them through.
        gp_Pnt o; gp_Dir d;
        if (computeDerivedAxisOD(m_axisOpKindIdx, o, d)) {
            op->setOrigin(o);
            op->setDirection(d);
            switch (m_axisOpKindIdx) {
                case 3: op->setType(AxisCreationType::FromCylinderAxis);
                        op->setName("Cylinder Axis"); break;
                case 4: op->setType(AxisCreationType::AlongEdge);
                        op->setName("Edge Axis"); break;
                case 6: op->setType(AxisCreationType::ThroughFaceNormal);
                        op->setName("Face-Normal Axis"); break;
                case 7: op->setType(AxisCreationType::TwoPlanesIntersection);
                        op->setName("Plane-Intersection Axis"); break;
            }
        } else {
            op->setType(AxisCreationType::WorldY); op->setName("Z Axis");
        }
    }

    m_axisOpPreview.hold(std::move(op), *m_document);
    if (m_axisOpPreview.apply(*m_document)) {
        // Auto-select the freshly-previewed axis so click-Move workflows pick
        // it up immediately. Mirrors the construction-plane popup.
        auto ids = m_document->getAllAxisIds();
        if (!ids.empty()) {
            SelectionEntry e;
            e.type = SelectionType::Axis;
            e.axisId = ids.back();
            m_selection->select(e);
        }
    }
    m_meshesDirty = true;
}

void Application::commitConstructionAxis() {
    m_axisOpPreview.commit(*m_history);
    m_axisOpActive = false;
    m_meshesDirty = true;
}

void Application::cancelConstructionAxis() {
    m_axisOpPreview.clear(*m_document);
    m_axisOpActive = false;
    m_meshesDirty = true;
}

// ─── Sketch-mode Pattern (Linear / Radial) ─────────────────────────────────
//
// Simpler than body patterns: no live preview, just a modal popup that takes
// count + spacing (linear) or count + angle + origin (radial), then runs an
// inline geometry copy similar to SketchCopy and pushes a SketchEditOp.

void Application::beginSketchPattern(PatternKind kind) {
    if (!m_inSketchMode || !m_activeSketch || !m_sketchTool) return;
    m_sketchPatternKind = kind;
    m_sketchPatternCount    = 3;
    m_sketchPatternDistance = 5.0f;
    m_sketchPatternAngle    = 360.0f;
    m_sketchPatternOriginX  = 0.0f;
    m_sketchPatternOriginY  = 0.0f;
    std::snprintf(m_sketchPatternCountBuf,    sizeof(m_sketchPatternCountBuf),    "%d", m_sketchPatternCount);
    materializr::formatLengthDigits(m_sketchPatternDistanceBuf, sizeof(m_sketchPatternDistanceBuf), m_sketchPatternDistance);
    std::snprintf(m_sketchPatternAngleBuf,    sizeof(m_sketchPatternAngleBuf),    "%.1f", m_sketchPatternAngle);
    std::snprintf(m_sketchPatternOXBuf, sizeof(m_sketchPatternOXBuf), "%.2f", m_sketchPatternOriginX);
    std::snprintf(m_sketchPatternOYBuf, sizeof(m_sketchPatternOYBuf), "%.2f", m_sketchPatternOriginY);
    m_sketchPatternFocusInput = true;
    m_sketchPatternActive = true;

    // Capture the selection model NOW (before previewing). On each preview
    // frame we restore from `before` and re-transform these ids; if we
    // re-read the selection from SketchTool after restoring, ids would
    // reference (now-vanished) preview copies.
    m_sketchPatternPts.clear();
    m_sketchPatternLines.clear();
    m_sketchPatternSelectAll = !m_sketchTool->hasElementSelection();
    if (!m_sketchPatternSelectAll) {
        m_sketchPatternPts.insert(m_sketchTool->getSelectedPoints().begin(),
                                  m_sketchTool->getSelectedPoints().end());
        m_sketchPatternLines = m_sketchTool->getSelectedLines();
        for (int lid : m_sketchPatternLines) {
            for (const auto& l : m_activeSketch->getLines()) {
                if (l.id == lid) {
                    m_sketchPatternPts.insert(l.startPointId);
                    m_sketchPatternPts.insert(l.endPointId);
                    break;
                }
            }
        }
    } else {
        for (const auto& p : m_activeSketch->getPoints()) m_sketchPatternPts.insert(p.id);
        for (const auto& l : m_activeSketch->getLines())  m_sketchPatternLines.insert(l.id);
        // Circles + arcs only included via the "selectAll" path (they have no
        // first-class selection state in SketchTool).
        for (const auto& c : m_activeSketch->getCircles())
            m_sketchPatternPts.insert(c.centerPointId);
        for (const auto& a : m_activeSketch->getArcs()) {
            m_sketchPatternPts.insert(a.centerPointId);
            m_sketchPatternPts.insert(a.startPointId);
            m_sketchPatternPts.insert(a.endPointId);
        }
    }

    // Snapshot the sketch for preview rollback / commit diff.
    m_sketchPatternBefore = std::make_shared<materializr::Sketch>(*m_activeSketch);
    updateSketchPattern();
}

void Application::updateSketchPattern() {
    if (!m_sketchPatternActive || !m_activeSketch || !m_sketchPatternBefore) return;
    // Restore the pre-preview state, then re-apply the transform from
    // current parameters. This is how every preview frame stays clean —
    // no leftover copies from earlier preview iterations.
    *m_activeSketch = *m_sketchPatternBefore;
    if (m_sketchPatternCount < 2 || m_sketchPatternPts.empty()) return;

    for (int step = 1; step < m_sketchPatternCount; ++step) {
        std::unordered_map<int,int> remap;
        auto xform = [&](glm::vec2 p) -> glm::vec2 {
            if (m_sketchPatternKind == PatternKind::Linear) {
                return p + glm::vec2(m_sketchPatternDistance * step, 0.0f);
            }
            float stepDeg = m_sketchPatternAngle / m_sketchPatternCount;
            float angRad = (stepDeg * step) * static_cast<float>(M_PI) / 180.0f;
            float dx = p.x - m_sketchPatternOriginX;
            float dy = p.y - m_sketchPatternOriginY;
            float ca = std::cos(angRad), sa = std::sin(angRad);
            return glm::vec2(m_sketchPatternOriginX + dx * ca - dy * sa,
                             m_sketchPatternOriginY + dx * sa + dy * ca);
        };
        for (int oldId : m_sketchPatternPts) {
            auto* p = m_activeSketch->getPoint(oldId);
            if (!p) continue;
            remap[oldId] = m_activeSketch->addPoint(xform(p->pos));
        }
        for (int lid : m_sketchPatternLines) {
            for (const auto& l : m_activeSketch->getLines()) {
                if (l.id != lid) continue;
                auto sIt = remap.find(l.startPointId);
                auto eIt = remap.find(l.endPointId);
                if (sIt != remap.end() && eIt != remap.end())
                    m_activeSketch->addLine(sIt->second, eIt->second);
                break;
            }
        }
        if (m_sketchPatternSelectAll) {
            auto circles = m_activeSketch->getCircles();
            for (const auto& c : circles) {
                auto it = remap.find(c.centerPointId);
                if (it != remap.end()) m_activeSketch->addCircle(it->second, c.radius);
            }
            auto arcs = m_activeSketch->getArcs();
            for (const auto& a : arcs) {
                auto ic = remap.find(a.centerPointId);
                auto is = remap.find(a.startPointId);
                auto ie = remap.find(a.endPointId);
                if (ic != remap.end() && is != remap.end() && ie != remap.end())
                    m_activeSketch->addArc(ic->second, is->second, ie->second, a.radius);
            }
        }
    }
}

void Application::commitSketchPattern() {
    if (!m_sketchPatternActive) return;
    updateSketchPattern(); // make sure the in-doc state reflects current params
    auto after = std::make_shared<materializr::Sketch>(*m_activeSketch);
    if (m_sketchPatternBefore &&
        (m_sketchPatternBefore->getPoints().size()  != after->getPoints().size() ||
         m_sketchPatternBefore->getLines().size()   != after->getLines().size() ||
         m_sketchPatternBefore->getCircles().size() != after->getCircles().size() ||
         m_sketchPatternBefore->getArcs().size()    != after->getArcs().size())) {
        auto op = std::make_unique<SketchEditOp>(m_activeSketch,
                                                  m_sketchPatternBefore, after);
        m_history->pushExecuted(std::move(op));
    }
    m_sketchPatternActive = false;
    m_sketchPatternPickingOrigin = false;
    m_sketchPatternBefore.reset();
    m_sketchPatternPts.clear();
    m_sketchPatternLines.clear();
}

void Application::cancelSketchPattern() {
    if (m_sketchPatternBefore && m_activeSketch) {
        *m_activeSketch = *m_sketchPatternBefore;
    }
    m_sketchPatternActive = false;
    m_sketchPatternPickingOrigin = false;
    m_sketchPatternBefore.reset();
    m_sketchPatternPts.clear();
    m_sketchPatternLines.clear();
}

// ── Rotate Plane About Axis ─────────────────────────────────────────────
// Build the list of candidate hinge lines for the target plane and open the
// popup. Each entry is a gp_Ax1 resolved up-front (transient — we never touch
// the document's axis list). Order: the plane's own U / V axes (tilt in
// place), then every construction axis, then a selected straight edge / a
// selected cylindrical face's centreline if either is in the selection.
void Application::beginRotatePlaneAboutAxis(int planeId) {
    if (!m_document) return;
    const auto* pe = m_document->getPlane(planeId);
    if (!pe) return;

    m_rotPlaneId = planeId;
    m_rotPlaneOriginal = pe->plane;
    m_rotPlaneAngle = 0.0f;
    std::snprintf(m_rotPlaneAngleBuf, sizeof(m_rotPlaneAngleBuf), "0.0");
    m_rotPlaneHinges.clear();
    m_rotPlaneHingeLabels.clear();

    // Tilt-in-place options: the plane's own X / Y directions through its
    // centre, so rotation reorients without translating the plane.
    const gp_Ax3& ax = m_rotPlaneOriginal.Position();
    gp_Pnt center = ax.Location();
    m_rotPlaneHinges.emplace_back(center, ax.XDirection());
    m_rotPlaneHingeLabels.emplace_back("Tilt about plane U (in-plane X)");
    m_rotPlaneHinges.emplace_back(center, ax.YDirection());
    m_rotPlaneHingeLabels.emplace_back("Tilt about plane V (in-plane Y)");

    int defaultIdx = 0;

    // Every construction axis in the document.
    for (int aid : m_document->getAllAxisIds()) {
        const auto* a = m_document->getAxis(aid);
        if (!a) continue;
        m_rotPlaneHinges.emplace_back(a->origin, a->direction);
        m_rotPlaneHingeLabels.emplace_back("Axis: " + a->name);
    }

    // Hinge about real geometry: a co-selected straight edge or cylindrical
    // face. Default to it when present — a co-selected hinge is almost
    // certainly why the user opened this rather than an in-place tilt.
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.shape.IsNull()) continue;
            if (e.type == SelectionType::Edge) {
                BRepAdaptor_Curve adaptor(TopoDS::Edge(e.shape));
                if (adaptor.GetType() == GeomAbs_Line) {
                    defaultIdx = static_cast<int>(m_rotPlaneHinges.size());
                    m_rotPlaneHinges.push_back(adaptor.Line().Position());
                    m_rotPlaneHingeLabels.emplace_back("Selected edge");
                }
            } else if (e.type == SelectionType::Face) {
                Handle(Geom_CylindricalSurface) cyl =
                    Handle(Geom_CylindricalSurface)::DownCast(
                        BRep_Tool::Surface(TopoDS::Face(e.shape)));
                if (!cyl.IsNull()) {
                    defaultIdx = static_cast<int>(m_rotPlaneHinges.size());
                    m_rotPlaneHinges.emplace_back(
                        cyl->Cylinder().Position().Location(),
                        cyl->Cylinder().Position().Direction());
                    m_rotPlaneHingeLabels.emplace_back("Selected face centreline");
                }
            }
        }
    }

    m_rotPlaneHingeIdx = defaultIdx;
    m_rotPlaneActive = true;
}

// Live preview / commit core: rotate the snapshot plane about the chosen
// hinge by the current angle and write it through Document::setPlane. Always
// re-bases from m_rotPlaneOriginal so dragging the angle doesn't accumulate.
void Application::applyRotatePlanePreview() {
    if (!m_document || m_rotPlaneId < 0) return;
    if (m_rotPlaneHingeIdx < 0 ||
        m_rotPlaneHingeIdx >= static_cast<int>(m_rotPlaneHinges.size())) return;
    gp_Trsf t;
    t.SetRotation(m_rotPlaneHinges[m_rotPlaneHingeIdx],
                  m_rotPlaneAngle * M_PI / 180.0);
    gp_Pln rotated = m_rotPlaneOriginal;
    rotated.Transform(t);
    m_document->setPlane(m_rotPlaneId, rotated);
}

void Application::cancelRotatePlaneAboutAxis() {
    if (m_document && m_rotPlaneId >= 0)
        m_document->setPlane(m_rotPlaneId, m_rotPlaneOriginal);
    m_rotPlaneActive = false;
    m_rotPlaneId = -1;
    m_rotPlaneHinges.clear();
    m_rotPlaneHingeLabels.clear();
}

// ─── Async thread re-cut (cascade/editStep recompute path) ──────────────────
// A sketch edit cascading through a Thread step used to re-run the multi-
// second helix sweep + boolean synchronously on the UI thread ("not
// responding"). ThreadOp::execute now hands the recompute here: the body is
// left at its pre-thread state (visually unthreaded for a moment) and the
// re-cut runs on a worker; pollThreadRecuts applies the result when it lands.

bool Application::launchThreadRecut(ThreadOp& op, int attempts) {
    if (!m_document) return false;
    TopoDS_Shape live;
    try { live = m_document->getBody(op.getBodyId()); } catch (...) {}
    if (live.IsNull()) return false;
    // DEEP-COPY for the worker (same reasoning as commitThread: the live
    // TShape's lazy caches are touched by the render thread every frame).
    TopoDS_Shape body = BRepBuilderAPI_Copy(live).Shape();
    if (body.IsNull()) return false;
    auto worker = std::make_shared<ThreadOp>(op); // params copy; buildResult const
    PendingThreadRecut p;
    p.op = &op;
    p.bodyId = op.getBodyId();
    p.launchedFrom = live;
    p.attempts = attempts;
    p.cancel = std::make_shared<std::atomic<bool>>(false);
    worker->setCancelToken(p.cancel);
    // Pre-mesh at the CURRENT quality so the renderer reuses the cache
    // instead of freezing the main thread on the helicoid faces; finer
    // angular pass (0.3 rad shows facets on threads).
    float rdefl, rang;
    meshQualityParams(rdefl, rang);
    const float recutAng = std::min(rang, 0.15f);
    p.fut = std::async(std::launch::async,
                       [worker, body, rdefl, recutAng]() {
                           std::fprintf(stderr, "[Thread] recut worker "
                                                "started\n");
                           const auto t0 = std::chrono::steady_clock::now();
                           TopoDS_Shape r = worker->buildResult(body);
                           if (!r.IsNull()) {
                               try {
                                   BRepMesh_IncrementalMesh mesh(
                                       r, rdefl, Standard_False,
                                       recutAng, Standard_True);
                                   mesh.Perform();
                               } catch (...) {}
                           }
                           const double secs =
                               std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count();
                           std::fprintf(stderr, "[Thread] recut worker done "
                                                "in %.1fs (%s)\n", secs,
                                        r.IsNull() ? "failed" : "ok");
                           return r;
                       });
    m_threadRecuts.push_back(std::move(p));
    return true;
}

void Application::installThreadRecutHook() {
    ThreadOp::setAsyncRecutHook([this](ThreadOp& op, Document& doc) -> bool {
        // Only the live document (headless/temp docs keep the sync path).
        if (!m_document || &doc != m_document) return false;
        // Single-flight per op: a request while one is pending stays pending —
        // the landing check sees the body changed since launch and RELAUNCHES
        // against the current state, so the newest edit always wins.
        for (auto& p : m_threadRecuts)
            if (p.op == &op) { p.attempts = 1; return true; } // re-arm budget
        if (!launchThreadRecut(op, 1)) return false;
        // No toast: renderThreadPanel draws the blocking re-cut modal (with
        // Cancel) while m_threadRecuts is non-empty — the app was effectively
        // unusable during a re-cut anyway, so the modal says so honestly.
        return true;
    });
}

void Application::pollThreadRecuts() {
    // Reap abandoned (cancelled) workers: std::async futures BLOCK in their
    // destructor, so they park in m_threadZombies until actually done.
    for (size_t i = 0; i < m_threadZombies.size();) {
        if (m_threadZombies[i].wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready) {
            m_threadZombies[i].get(); // discard
            m_threadZombies.erase(m_threadZombies.begin() + i);
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < m_threadRecuts.size();) {
        auto& p = m_threadRecuts[i];
        if (p.fut.wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) { ++i; continue; }
        TopoDS_Shape result = p.fut.get();

        // The op must still be an applied history step.
        int stepIdx = -1;
        for (int k = 0; k <= m_history->currentStep(); ++k)
            if (m_history->getStep(k) == p.op) { stepIdx = k; break; }
        TopoDS_Shape cur;
        try { cur = m_document->getBody(p.bodyId); } catch (...) {}

        if (stepIdx < 0 || cur.IsNull()) {
            // Step deleted / body gone — drop.
            m_threadRecuts.erase(m_threadRecuts.begin() + i);
            continue;
        }
        if (!cur.IsSame(p.launchedFrom)) {
            // The body changed while the worker ran (a second cascade re-ran
            // the chain and committed a NEW TShape — e.g. the sketch edit
            // fired two cascades). This result is stale: RELAUNCH against the
            // current body instead of silently dropping it, or the thread
            // never lands ("it said background but nothing happened").
            ThreadOp* op = p.op;
            int attempts = p.attempts;
            m_threadRecuts.erase(m_threadRecuts.begin() + i);
            if (attempts < 3) launchThreadRecut(*op, attempts + 1);
            else std::fprintf(stderr, "[Thread] recut gave up after %d stale "
                                      "attempts\n", attempts);
            continue;
        }
        if (result.IsNull()) {
            // New geometry can't take the thread — suspend the step with the
            // standard explainer banner instead of silently no-opping.
            m_history->suspendStep(stepIdx);
            showToast("Thread couldn't re-cut on the new geometry \xE2\x80\x94 "
                      "check the Thread step.");
        } else {
            std::fprintf(stderr, "[Thread] recut landed — applying to body "
                                 "%d\n", p.bodyId);
            m_document->updateBody(p.bodyId, result);
            m_meshesDirty = true;
        }
        m_threadRecuts.erase(m_threadRecuts.begin() + i);
    }
}

void Application::cancelThreadRecuts() {
    if (m_threadRecuts.empty()) return;
    for (auto& p : m_threadRecuts) {
        if (p.cancel) p.cancel->store(true);
        // The body is sitting at its pre-thread state with the Thread step
        // still claiming to be applied — suspend it (same explainer banner
        // as a failed re-cut) so the history stays honest.
        int stepIdx = -1;
        for (int k = 0; k <= m_history->currentStep(); ++k)
            if (m_history->getStep(k) == p.op) { stepIdx = k; break; }
        if (stepIdx >= 0) m_history->suspendStep(stepIdx);
        m_threadZombies.push_back(std::move(p.fut));
    }
    m_threadRecuts.clear();
    m_meshesDirty = true;
    showToast("Thread re-cut cancelled \xE2\x80\x94 the Thread step is "
              "suspended; re-enable it in History to re-cut.");
}

void Application::flushThreadRecuts() {
    // Save path: a snapshot taken mid-recut would bake the unthreaded body
    // under a Thread step. Block the (rare, few-second) remainder instead.
    while (!m_threadRecuts.empty()) {
        m_threadRecuts.front().fut.wait();
        pollThreadRecuts();
    }
}

} // namespace materializr
