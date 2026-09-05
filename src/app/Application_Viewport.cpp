#include "ui/UiTheme.h"
#include "ui/OpDialogGrip.h"
#include "ui_scale.h"
#include "touch_mode.h"
#include "gl_common.h"
#include "viewport/GridScale.h"
#include <cmath>
#include <chrono>
#include <cstdio>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "app/Application.h"
#include "app/Window.h"
#include "viewport/Viewport.h"
#include "viewport/Grid.h"
#include "viewport/ShapeRenderer.h"
#include "viewport/SketchRenderer.h"
#include "viewport/ViewCube.h"
#include "viewport/Picker.h"
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include "io/ImageEncode.h"
#include "viewport/Gizmo.h"
#include "viewport/SelectionHighlight.h"
#include "ui/TouchIcons.h"   // im-touch circle-confirm bubble ✗/✓ glyphs
#include "ui/TouchWidgets.h"
#include "viewport/BoxSelect.h"
#include "viewport/SectionView.h"
#include "viewport/EdgeRenderer.h"
#include "viewport/BackgroundRenderer.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/SelectionManager.h"
#include "core/Verbose.h"
#include "core/NumParse.h"
#include "ui/Toolbar.h"
#include "ui/HistoryPanel.h"
#include "ui/ItemsPanel.h"
#include "ui/StatusBar.h"
#include "ui/ThemeManager.h"
#include "ui/PropertiesPanel.h"
#include "ui/AboutDialog.h"
#include "ui/ShortcutsPanel.h"
#include "ui/HelpPanel.h"
#include "ui/MeasureTool.h"
#include "ui/StepperRow.h"
#include "ui/UpdateChecker.h"
#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/ReplayOp.h"
#include "modeling/SketchTransformOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/TransformOp.h"
#include "modeling/BatchTransformOp.h"
#include "modeling/PlaneTransformOp.h"
#include "modeling/AxisTransformOp.h"
#include "modeling/MirrorOp.h"
#include "modeling/FilletOp.h"
#include "ui/LengthField.h"
#include <cstring>
#include "modeling/ChamferOp.h"
#include "modeling/DeleteOp.h"
#include "modeling/SeparateBodyOp.h"
#include "modeling/SketchEditOp.h"
#include "io/StepIO.h"
#include "io/StlExport.h"
#include "io/FileDialogs.h"
#include "io/ProjectIO.h"
#include "io/Settings.h"
#include "core/EventBus.h"
#include "core/Events.h"
#include "plugin/PluginContext.h"
#include "plugin/PluginRegistry.h"

namespace materializr { namespace force_link { void linkAll(); } }

#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName/DockBuilder: viewport re-dock after im-touch
#include "../i18n.h"
#include "../i18n.h"
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Ax3.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS.hxx>
#include <Geom_Plane.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_XYZ.hxx>
#include "../ui/NumField.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Implementations split out of Application.cpp — the giant 3D viewport
// renderer plus its drag-projection helper.
namespace materializr {

// Smallest on-screen cell, in pixels, the sketch grid will draw. Below this
// the shader's density fade greys the lines out anyway, so a finer lattice
// costs fill rate and shows nothing.
constexpr float kGridMinPx = 8.0f;

// opDialogDragGrip moved to ui/OpDialogGrip.h — Move Face's panel (in
// FaceOpControllers.cpp) uses it too.

// Map a screen drag onto a world direction: project the mouse delta onto the
// screen-space image of `normal` at `origin`. Falls back to vertical drag when
// that direction is nearly perpendicular to the screen (face head-on) — otherwise
// normalizing a near-zero vector yields NaN, which propagates into a NaN prism
// and crashes the boolean kernel.
// Convert a mouse drag (pixels) into a world distance along `normal` — EXACT:
// +1 world unit along the axis spans a measurable pixel vector on screen, and
// the returned distance is the drag's projection onto that vector divided by
// its pixel length. The face/arrow therefore tracks the cursor 1:1 at every
// zoom level (same philosophy as the exact pan) instead of the old fixed
// 0.05 mm-per-pixel, which felt sluggish zoomed in and jumpy zoomed out.
// Near head-on the axis's screen footprint collapses and exact tracking would
// turn one pixel into metres, so the sensitivity gain over a screen-parallel
// axis at the same depth is clamped (÷ max(|sin θ|, 0.25) worth).
static float projectDragOntoNormal(const glm::vec3& origin, const glm::vec3& normal,
                                   const glm::vec2& mouseDelta, const glm::mat4& vp,
                                   const glm::vec2& viewportPx,
                                   const glm::vec3& viewDir) {
    glm::vec4 o = vp * glm::vec4(origin, 1.0f);
    glm::vec4 t = vp * glm::vec4(origin + normal, 1.0f);
    if (o.w <= 1e-5f || t.w <= 1e-5f) return -mouseDelta.y * 0.05f;
    glm::vec2 os(o.x / o.w, o.y / o.w), ts(t.x / t.w, t.y / t.w);
    // NDC → pixels (screen +y is down).
    glm::vec2 sd((ts.x - os.x) * 0.5f * viewportPx.x,
                 -(ts.y - os.y) * 0.5f * viewportPx.y);
    float lenPx = glm::length(sd); // pixels spanned by +1 world unit
    if (lenPx < 1e-4f) return -mouseDelta.y * 0.05f; // truly head-on: legacy feel
    // Foreshortening guard: lenPx shrinks by |sin θ| (θ = axis vs view dir).
    // Cap the world-per-pixel gain at 4× the screen-parallel rate so a
    // near-head-on arrow stays controllable.
    float sinT = 1.0f;
    if (glm::length(normal) > 1e-6f && glm::length(viewDir) > 1e-6f) {
        float c = glm::dot(glm::normalize(normal), glm::normalize(viewDir));
        sinT = std::sqrt(std::max(0.0f, 1.0f - c * c));
    }
    if (sinT > 1e-4f) {
        float lenPxParallel = lenPx / sinT;      // footprint if screen-parallel
        lenPx = std::max(lenPx, lenPxParallel * 0.25f);
    }
    return glm::dot(mouseDelta, sd / glm::length(sd)) / lenPx;
}

void Application::gizmoPreviewApply(const glm::mat4& m) {
    // Push the drag's accumulated transform onto every dragged body's mesh
    // slots (shape + edges). GPU-only — the document is untouched, so a drag
    // frame costs two uniform updates per body instead of re-tessellating
    // the body (see the Revolve live preview, which pioneered the pattern).
    //
    // Mirror it for the world-space chrome. Because the document never moves,
    // the gizmo (positioned from the body's bbox centre) and the selection
    // outline (cached in world coords) both sat at the PRE-drag pose for the
    // whole drag: the body slid out from under its own gizmo and snapped back
    // together on release, and the outline had to be hidden outright. Every
    // preview passes through here, so this is the one place that has to know.
    m_gizmoPreviewXf = m;
    for (auto& [id, orig] : m_gizmoDragOriginals) {
        if (m_shapeRenderer) {
            int slot = m_shapeRenderer->findSlotByBody(id);
            if (slot >= 0) m_shapeRenderer->setModelMatrix(slot, m);
        }
        if (m_edgeRenderer) {
            int slot = m_edgeRenderer->findSlotByBody(id);
            if (slot >= 0) m_edgeRenderer->setModelMatrix(slot, m);
        }
    }
}

// Does this Radius constraint measure an ARC (rather than a circle)?
//
// Drafting convention splits the two: an arc is called out by RADIUS ("R 10"),
// a full circle by DIAMETER ("Ø 20"). Constraint::value stores a radius for
// both, so the label, the edit-popup prefill, and the popup's commit all have
// to agree on which convention applies — hence one helper rather than three
// copies of the scan. Non-Radius types are never arc radii.

// Recover the two comparable entities behind a dimension, so the edit popup
// can offer "Make equal" (equal length for two lines, equal radius for two
// circles/arcs). Returns false when the dimension has no such pair.
//
// Derived rather than stored: every dimension that came from a two-entity pick
// still carries enough to reconstruct the pair, so no extra state has to be
// threaded through PendingDimension and the option stays available when the
// user re-opens an OLD dimension's popup, not just a freshly placed one.
static bool dimensionEqualPair(const Sketch& sk, const Constraint& c,
                               int& outA, int& outB) {
    auto isCurveId = [&sk](int id) {
        for (const auto& x : sk.getCircles()) if (x.id == id) return true;
        for (const auto& x : sk.getArcs())    if (x.id == id) return true;
        return false;
    };
    switch (c.type) {
        case ConstraintType::Angle:      // entityA/entityB are both line ids
        case ConstraintType::CircleGap:  // entityA/entityB are both curve ids
            outA = c.entityA; outB = c.entityB;
            return outA >= 0 && outB >= 0 && outA != outB;
        case ConstraintType::DistancePointLine: {
            // entityA is a POINT on the second line, entityB the first line.
            // Recover the second line as the one owning that point — that is
            // exactly how resolveDimension's parallel branch built the pair.
            for (const auto& l : sk.getLines()) {
                if (l.id == c.entityB) continue;
                if (l.startPointId == c.entityA || l.endPointId == c.entityA) {
                    outA = c.entityB; outB = l.id;
                    return true;
                }
            }
            return false;
        }
        case ConstraintType::Distance: {
            // Only meaningful when both ends are circle/arc centres (a
            // centre-to-centre dim); two bare points have no "length" to
            // equalise. Map each centre back to its curve.
            auto curveOfCentre = [&sk](int ptId) {
                for (const auto& x : sk.getCircles())
                    if (x.centerPointId == ptId) return x.id;
                for (const auto& x : sk.getArcs())
                    if (x.centerPointId == ptId) return x.id;
                return -1;
            };
            int ca = curveOfCentre(c.entityA), cb = curveOfCentre(c.entityB);
            if (ca < 0 || cb < 0 || ca == cb) return false;
            outA = ca; outB = cb;
            return isCurveId(ca) && isCurveId(cb);
        }
        default:
            return false;
    }
}

// Sketch-space auto anchor of a dimension's label: line/pair midpoint,
// circle/arc center, or the midpoint of the point-to-line perpendicular foot
// segment. Label offsets (Constraint::labelOffX/Y) are stored relative to
// this, so it has to stay in sync with the dimension-label render pass below.
glm::vec2 Application::dimensionAutoAnchor(const PendingDimension& pd) const {
    if (!m_activeSketch || !pd.valid) return glm::vec2(0.0f);
    const Sketch& sk = *m_activeSketch;
    auto lineEnds = [&sk](int id, glm::vec2& s, glm::vec2& e) {
        for (const auto& l : sk.getLines())
            if (l.id == id) {
                const SketchPoint* sp = sk.getPoint(l.startPointId);
                const SketchPoint* ep = sk.getPoint(l.endPointId);
                if (!sp || !ep) return false;
                s = sp->pos; e = ep->pos; return true;
            }
        return false;
    };
    switch (pd.type) {
        case ConstraintType::Distance: {
            const SketchPoint* a = sk.getPoint(pd.entityA);
            const SketchPoint* b = sk.getPoint(pd.entityB);
            return (a && b) ? 0.5f * (a->pos + b->pos) : glm::vec2(0.0f);
        }
        case ConstraintType::Radius: {
            for (const auto& c : sk.getCircles())
                if (c.id == pd.entityA) {
                    const SketchPoint* ctr = sk.getPoint(c.centerPointId);
                    if (ctr) return ctr->pos;
                }
            for (const auto& a : sk.getArcs())
                if (a.id == pd.entityA) {
                    const SketchPoint* ctr = sk.getPoint(a.centerPointId);
                    if (ctr) return ctr->pos;
                }
            return glm::vec2(0.0f);
        }
        case ConstraintType::DistancePointLine: {
            const SketchPoint* p = sk.getPoint(pd.entityA);
            glm::vec2 s, e;
            if (p && lineEnds(pd.entityB, s, e)) {
                glm::vec2 d = e - s;
                float len2 = glm::dot(d, d);
                if (len2 > 1e-12f) {
                    // Foot clamped to the physical segment: the constraint
                    // measures against the infinite carrier, but a label
                    // anchored past the segment's end hangs over unrelated
                    // geometry (rectangle corners made parallel-line dims
                    // look attached to a third line).
                    float t = glm::clamp(glm::dot(p->pos - s, d) / len2, 0.0f, 1.0f);
                    glm::vec2 foot = s + d * t;
                    return 0.5f * (p->pos + foot);
                }
            }
            return p ? p->pos : glm::vec2(0.0f);
        }
        case ConstraintType::Angle: {
            glm::vec2 as, ae, bs, be;
            if (lineEnds(pd.entityA, as, ae) && lineEnds(pd.entityB, bs, be))
                return 0.25f * (as + ae + bs + be);
            return glm::vec2(0.0f);
        }
        case ConstraintType::CircleGap: {
            glm::vec2 cA(0.0f), cB(0.0f);
            double rA = -1.0, rB = -1.0;
            auto grab = [&](int id, glm::vec2& ctr, double& r) {
                for (const auto& c : sk.getCircles())
                    if (c.id == id) {
                        const SketchPoint* p = sk.getPoint(c.centerPointId);
                        if (p) { ctr = p->pos; r = c.radius; }
                        return;
                    }
                for (const auto& a : sk.getArcs())
                    if (a.id == id) {
                        const SketchPoint* p = sk.getPoint(a.centerPointId);
                        if (p) { ctr = p->pos; r = a.radius; }
                        return;
                    }
            };
            grab(pd.entityA, cA, rA);
            grab(pd.entityB, cB, rB);
            if (rA < 0.0 || rB < 0.0) return 0.0f * cA;
            glm::vec2 d = cB - cA;
            float len = glm::length(d);
            if (len < 1e-6f) return 0.5f * (cA + cB);
            glm::vec2 u = d / len;
            // Midpoint of the gap: from A's rim to B's rim along the centre line.
            glm::vec2 rimA = cA + u * static_cast<float>(rA);
            glm::vec2 rimB = cB - u * static_cast<float>(rB);
            return 0.5f * (rimA + rimB);
        }
        default: return glm::vec2(0.0f);
    }
}

void Application::renderViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGuiWindowFlags vpFlags = 0;
    if (!classicLayout()) {
        // Modern/im-touch: pin the viewport (undocked, chrome-less) into the
        // center rect the layout's renderer computed earlier this frame.
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(m_touchVpX, m_touchVpY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(m_touchVpW, m_touchVpH), ImGuiCond_Always);
        vpFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                  ImGuiWindowFlags_NoDocking |
                  ImGuiWindowFlags_NoBringToFrontOnFocus;
    } else {
        // Classic shell self-heal: the viewport always lives in the
        // dockspace's central node (its tab bar is off, so the user can't
        // re-dock it by hand). The touch shell actively UNDOCKS it every
        // frame (SetNextWindowDockID(0) above) and that undocked state can
        // even reach imgui.ini — so coming back from im-touch (or launching
        // classic with such an ini) found the viewport floating over the
        // Tools panel. If it's floating here, put it back.
        if (ImGuiWindow* w = ImGui::FindWindowByName("Viewport")) {
            if (w->DockId == 0) {
                // 0x08BD597D = the dockspace id (see renderDockspace()).
                if (const ImGuiDockNode* central =
                        ImGui::DockBuilderGetCentralNode(0x08BD597Du))
                    ImGui::SetNextWindowDockID(central->ID, ImGuiCond_Always);
            }
        }
    }
    ImGui::Begin("Viewport", nullptr, vpFlags);

    // Cache the viewport window's screen rect so the touch collapse handles can
    // anchor to the panel/viewport boundaries (which move as panels collapse).
    {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        m_viewportWinX = wp.x; m_viewportWinY = wp.y;
        m_viewportWinW = ws.x; m_viewportWinH = ws.y;
    }

    // Classic: the project tab strip lives at the top of the viewport (and
    // ONLY the viewport — it's a plain tab bar, not a dock node, so tabs
    // can't be dragged into the panel docks). Drawn before contentSize is
    // measured so the 3D image and all item-relative picking shift down
    // together; suppressed while the landing page owns the screen.
    if (classicLayout() && !landingPageUp()) renderViewportTabBar();

    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    int w = static_cast<int>(contentSize.x);
    int h = static_cast<int>(contentSize.y);

    if (w > 0 && h > 0) {
        // Render the offscreen 3D viewport at the display's PIXEL resolution, not
        // logical points, so it stays crisp on HiDPI/Retina screens. Otherwise the
        // FBO is point-sized and ImGui::Image upscales it — soft/blurry at 2x. Only
        // the render target scales: the image is still laid out at contentSize
        // (points) and picking works in point-space (mouse + viewport both points
        // → NDC), so neither needs to change. DisplayFramebufferScale is (1,1) on
        // non-HiDPI displays, making this a no-op there; it's re-read every frame,
        // so dragging the window between monitors of different density adapts live.
        // Cap the scale at 2x. Retina is 2.0, and the FBO's color + depth/stencil
        // + MSAA buffers (all rebuilt on every resize) cost ~quadratically in it,
        // so a display reporting >2x would burn memory for no visible gain. macOS
        // backing scale is only ever 1 or 2, so this clamp is a safety bound, not
        // a behaviour change there.
        const ImGuiIO& io = ImGui::GetIO();
        const float fbScaleX = std::min(io.DisplayFramebufferScale.x, 2.0f);
        const float fbScaleY = std::min(io.DisplayFramebufferScale.y, 2.0f);
        const int fbw = static_cast<int>(contentSize.x * fbScaleX);
        const int fbh = static_cast<int>(contentSize.y * fbScaleY);
        m_viewport->resize(fbw, fbh);

        bool geomChanged = m_meshesDirty || !m_dirtyBodyIds.empty();
        if (geomChanged) {
            rebuildMeshes();
            m_meshesDirty = false;
            // rebuildMeshes() also clears m_dirtyBodyIds on completion.
        }

        m_viewport->bind();

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        // Viewport background tracks the UI theme: a soft light gradient in light
        // mode, the dark gradient otherwise. (The grid switches to a dark-on-light
        // line palette below so it stays readable on the light background.)
        const bool lightBg = m_themeManager &&
                             m_themeManager->getTheme() == Theme::Light;
        if (lightBg) {
            m_backgroundRenderer->setTopColor(glm::vec3(0.92f, 0.93f, 0.96f));
            m_backgroundRenderer->setBottomColor(glm::vec3(0.78f, 0.80f, 0.85f));
        } else {
            m_backgroundRenderer->setTopColor(glm::vec3(0.22f, 0.22f, 0.28f));
            m_backgroundRenderer->setBottomColor(glm::vec3(0.12f, 0.12f, 0.15f));
        }
        m_backgroundRenderer->render();
        glEnable(GL_DEPTH_TEST);

        Camera& cam = m_viewport->getCamera();
        // Viewport height in ImGui points (the units mouse/touch deltas use):
        // lets Camera::pan translate exactly one pixel's world size per pixel
        // dragged, so pan tracking stays 1:1 at every zoom level.
        cam.setViewHeightPx(contentSize.y);
        glm::mat4 view = cam.getViewMatrix();
        glm::mat4 proj = cam.getProjectionMatrix();

        // Grid: in any sketch mode (whether on a face or from scratch), lay the
        // infinite world grid on the sketch plane so it shows face-on and any
        // nearby face can be referenced. Outside sketch mode, use the XZ ground.
        // Deferred into a lambda and invoked AFTER the solid geometry below, so
        // the grid (which no longer writes depth) blends over bodies instead of
        // punching through coplanar faces.
        auto drawGrid = [&]() {
            Grid::Plane gp; // defaults to the XZ ground
            bool sketching = m_inSketchMode && m_activeSketch;
            if (sketching) {
                const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                auto v3 = [](const gp_Dir& d){ return glm::vec3(d.X(), d.Y(), d.Z()); };
                // Use the world-aligned anchor (computed at sketch entry) as
                // the grid origin so grid lines pass through whole world-grid
                // intersections on the sketch plane instead of being shifted
                // by the face's off-grid centre.
                gp.origin = m_sketchSnappedAnchor;
                gp.u = v3(ax.XDirection());
                gp.v = v3(ax.YDirection());
                gp.normal = v3(ax.Direction());
            }
            // Centre the fade on the point of the plane directly under the camera
            // (the eye's projection onto the plane). It's always finite and moves
            // with the camera, so the fade follows cursor-zoom (the reason we left
            // the orbit target) WITHOUT the divergence the view-ray∩plane had: as
            // the view approaches edge-on that intersection raced to the horizon
            // and snapped back on crossing, a one-frame brightness pop that read as
            // a glitch. The eye-projection has no such discontinuity. For a view
            // ray that genuinely hits the plane near the camera, blend toward that
            // hit so framing stays centred where you're looking at normal angles.
            glm::vec3 ro = cam.getPosition();
            float eyeH = glm::dot(ro - gp.origin, gp.normal);
            glm::vec3 fadeCenter = ro - eyeH * gp.normal; // eye → plane
            {
                glm::vec3 rd = cam.getTarget() - ro;
                float rl = glm::length(rd);
                if (rl > 1e-6f) {
                    rd /= rl;
                    float denom = glm::dot(rd, gp.normal);
                    if (std::abs(denom) > 1e-6f) {
                        float t = glm::dot(gp.origin - ro, gp.normal) / denom;
                        // Only adopt the look-point when it's a sane, near hit —
                        // not the horizon-bound intersection at grazing angles.
                        float cap = std::max(std::abs(eyeH), 10.0f) * 32.0f;
                        if (t > 0.0f && t < cap) fadeCenter = ro + rd * t;
                    }
                }
            }
            // Fade radius sized to the view so the grid fills it without a hard
            // edge. Perspective used to key this off the camera→TARGET distance,
            // but on large projects the orbit target drifts away from the content
            // on screen (cursor-zoom onto a small part can leave it millimetres
            // from the camera) and the grid faded out within arm's reach of the
            // eye — "the grid disappears when I'm not even zoomed in". Key it off
            // the camera→fadeCenter distance instead: the point on the plane the
            // fade is centred on is, by construction, where the view is actually
            // looking. The eye height above the plane is the floor so a low
            // camera looking outward still gets a sane radius.
            float fadeDist = cam.isOrthographic()
                ? cam.getOrthoSize() * 8.0f
                : std::max(glm::length(fadeCenter - ro), std::abs(eyeH)) * 8.0f;
            // Suppress the minor (1×) grid tier when the project is big and
            // the user isn't actively sketching / moving — at that zoom the
            // 1-mm lines are clutter that drowns the major (10-mm) lines.
            // The minor tier comes back during sketch / gizmo drag because
            // that's when fine snapping actually matters.
            bool interactive = m_inSketchMode || m_gizmoDragging ||
                               m_extrudeCtl.active() || m_ppCtl.active() ||
                               m_edgeCtl.active();
            float minorAlpha = 1.0f;
            if (!interactive) {
                // This used to walk every visible body's bbox on every frame
                // to decide whether the project is "big enough" to suppress
                // the 1× minor grid. On a 65-body airplane that's ~65 OCCT
                // bbox calls per frame; even cheap each, the cumulative
                // baseline cost is real. We only need this threshold check
                // to feel responsive — not to update every frame — so cache
                // the verdict and refresh every ~0.25s. A topology change
                // can wait that long to flip the grid tier.
                static double s_nextCheckTime = 0.0;
                static bool   s_hideMinor    = false;
                double now = ImGui::GetTime();
                if (now >= s_nextCheckTime) {
                    try {
                        Bnd_Box bb;
                        bool any = false;
                        for (int id : m_document->getAllBodyIds()) {
                            if (!m_document->isBodyVisible(id)) continue;
                            BRepBndLib::Add(m_document->getBody(id), bb);
                            any = true;
                        }
                        if (any && !bb.IsVoid()) {
                            double xmn,ymn,zmn,xmx,ymx,zmx;
                            bb.Get(xmn,ymn,zmn,xmx,ymx,zmx);
                            double ext = std::max({xmx-xmn, ymx-ymn, zmx-zmn});
                            s_hideMinor = (ext > 100.0);
                        }
                    } catch (...) {}
                    s_nextCheckTime = now + 0.25;
                }
                if (s_hideMinor) minorAlpha = 0.0f;
            }
            // (fadeCenter + fadeDist were computed above, before the minor-tier
            // check, because the fade radius is sized off the fade centre.)
            // One grid (m_grid) serves both the XZ ground and the sketch plane,
            // driven by the opacity setting in every mode. The shader gives each
            // tier its own screen-space density fade, so the fine face-on sketch
            // grid dissolves as one even sheet (no moiré "plaid") and dims
            // uniformly under the opacity slider. In sketch mode the view is
            // FACE-ON, so a distance fade would just cull the grid's edges as
            // opacity drops; flatten it (huge fade distance) so opacity is the
            // only thing dimming the grid. The angled world view keeps its soft
            // horizon fade.
            float gridFade;
            float worldGridAlpha = m_sketchGridOpacity;
            if (sketching) {
                gridFade = 1.0e6f;
            } else {
                gridFade = std::max(fadeDist, 10.0f);
                // Near edge-on the whole visible grid sits beyond the fade disc and
                // vanishes. Grow the fade radius hard as the view grazes the plane
                // so the grid reaches the horizon and stays drawn (the pristine-grid
                // coverage greys distant cells, so no moiré). NOTE: no alpha fade
                // here — basing it on the view-to-target angle wrongly blanked the
                // grid whenever you looked horizontally at something ABOVE it.
                glm::vec3 vd = cam.getTarget() - cam.getPosition();
                float vl = glm::length(vd);
                if (vl > 1e-6f) {
                    float graze = std::abs(glm::dot(vd / vl, gp.normal)); // 0=edge-on,1=face-on
                    gridFade /= std::max(graze, 0.005f);                  // up to ~200× near grazing
                }
            }
            // depthBias: + draws the grid ON the coplanar sketch face; - lets a
            // coplanar body face (e.g. a body sitting on the XZ ground) occlude
            // the ground grid instead of it bleeding through.
            // The DRAWN lattice may be coarser than the snap step, never
            // finer — see GridScale.h for why decades-only keeps every drawn
            // line on a snap point. Without this, switching feet -> mm leaves a
            // 1 mm grid in a view framing 40 ft and the grid reads as gone.
            float gridDrawStepMm = std::max(m_sketchGridStep, 0.01f);
            if (sketching) {
                const glm::vec2 g0 = screenToSketch(0.0f, 0.0f, contentSize.x, contentSize.y);
                const glm::vec2 g1 = screenToSketch(1.0f, 0.0f, contentSize.x, contentSize.y);
                gridDrawStepMm = gridDrawStep(gridDrawStepMm,
                                              glm::length(g1 - g0), kGridMinPx);
            }
            m_grid->render(view, proj, fadeCenter, gridFade,
                           gp, gridDrawStepMm,
                           minorAlpha, worldGridAlpha /*globalAlpha*/,
                           sketching ? 1.0f : 0.0f /*sketchGrid: uniform single tier*/,
                           sketching ? 0.0005f : -0.0005f /*depthBias*/,
                           lightBg ? 1.0f : 0.0f /*lightBg palette*/,
                           m_sketchGridThickness /*sketch grid line width*/);
        };
        // OUTSIDE sketch mode the world grid draws EARLY — before the plugin
        // passes — so translucent pass content (reference-image photos,
        // construction-plane quads) blends OVER it: a photo's opacity then
        // genuinely reveals the grid/ground beneath instead of fading to the
        // bare background. Bodies still paint over the early grid opaquely, so
        // the final image is unchanged for solid geometry. IN sketch mode the
        // grid stays late (below) so its lines land ON TOP of the photo being
        // traced.
        if (!(m_inSketchMode && m_activeSketch)) drawGrid();
        // Plugin-registered render passes (e.g. ConstructionPlanePlugin's
        // plane quads) draw between the grid/background and the body/edge
        // pass. PluginContext receives the same view+proj, and each pass'
        // initialize() ran in initRenderers so GL state is ready.
        // Passes that draw BEHIND geometry. Nothing registers here today: the
        // reference photo used to (priority 490, "it is meant to be traced
        // over"), but drawing early does NOT put a translucent overlay behind
        // the model — with glDepthMask(GL_FALSE) it leaves no depth, so it
        // loses to every body regardless of where it actually sits in 3D. A
        // photo the camera is nearer to than the part vanished anyway. Depth
        // TEST does the "behind" part correctly on its own, so the photo moved
        // up with the planes (see the in-front block below).
        if (m_pluginContext) {
            for (auto& pass :
                 materializr::PluginRegistry::instance().renderPasses()) {
                if (pass.priority < kBodyPassPriority && pass.render)
                    pass.render(*m_pluginContext, view, proj);
            }
        }
        // Section view: feed the clip plane to the body shader and refresh
        // the intersection-curve overlay when geometry or the plane changed.
        if (m_sectionEnabled && m_sectionView) {
            gp_Pln pl = sectionBasePlane();
            gp_Pnt o = pl.Location();
            gp_Dir n = pl.Axis().Direction();
            glm::vec3 p(o.X() + n.X() * m_sectionOffset,
                        o.Y() + n.Y() * m_sectionOffset,
                        o.Z() + n.Z() * m_sectionOffset);
            m_shapeRenderer->setSectionPlane(true, p,
                                             glm::vec3(n.X(), n.Y(), n.Z()));
            m_edgeRenderer->setSectionPlane(true, p,
                                            glm::vec3(n.X(), n.Y(), n.Z()));
            // DEBOUNCED overlay recompute. The GPU clip planes above track
            // the slider instantly; SectionView::update() runs a full OCCT
            // plane-section + cap triangulation on the MAIN thread — on a
            // swept-thread body (helicoid BSplines) that's seconds PER
            // recompute, and firing it on every drag tick froze the app
            // solid. Recompute only once the plane has RESTED for 250 ms
            // (the overlay/cap pops in when the drag pauses).
            // ASYNC overlay recompute. One recompute on a swept-thread body
            // measured 100.78s — on the main thread that froze the whole
            // app (Steve's section-view hang). The GPU clip planes above
            // track the slider instantly; the overlay (curves + caps)
            // computes on a WORKER from deep-copied shapes, debounced until
            // the plane rests, newest-plane-wins (a superseded compute is
            // user-break-cancelled mid-boolean).
            const uint32_t nowMs = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (m_sectionDirty || geomChanged) {
                m_sectionRestMs = nowMs;
                m_sectionPending = true;
                m_sectionDirty = false;
                // Supersede an in-flight compute — its plane is stale.
                if (m_sectionFut.valid() && m_sectionCancel)
                    m_sectionCancel->store(true);
            }
            m_sectionView->setEnabled(true);
            if (m_sectionPending && !m_sectionFut.valid() &&
                nowMs - m_sectionRestMs >= 250) {
                gp_Pln cutting = pl;
                {
                    gp_Pnt o2 = cutting.Location();
                    o2.Translate(gp_Vec(n) *
                                 static_cast<double>(m_sectionOffset));
                    cutting.SetLocation(o2);
                }
                std::vector<std::pair<TopoDS_Shape, glm::vec3>> bodies;
                for (int id : m_document->getAllBodyIds()) {
                    if (!m_document->isBodyVisible(id)) continue;
                    try {
                        const TopoDS_Shape& s = m_document->getBody(id);
                        if (s.IsNull()) continue;
                        bodies.emplace_back(BRepBuilderAPI_Copy(s).Shape(),
                                            m_document->getBodyColor(id));
                    } catch (...) { continue; }
                }
                m_sectionCancel = std::make_shared<std::atomic<bool>>(false);
                auto cancel = m_sectionCancel;
                m_sectionFut = std::async(
                    std::launch::async,
                    [bodies = std::move(bodies), cutting, cancel]() {
                        const auto t0 = std::chrono::steady_clock::now();
                        auto r = materializr::SectionView::compute(
                            bodies, cutting, cancel.get());
                        const double secs = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - t0).count();
                        if (secs > 0.5)
                            std::fprintf(stderr, "[Perf] section overlay "
                                                 "computed in %.2fs on the "
                                                 "worker\n", secs);
                        return r;
                    });
                m_sectionPending = false;
            }
        } else {
            m_shapeRenderer->setSectionPlane(false, glm::vec3(0.0f),
                                             glm::vec3(0.0f, 1.0f, 0.0f));
            m_edgeRenderer->setSectionPlane(false, glm::vec3(0.0f),
                                            glm::vec3(0.0f, 1.0f, 0.0f));
            if (m_sectionView) m_sectionView->setEnabled(false);
            // Kill an in-flight compute — its result is unwanted.
            if (m_sectionFut.valid() && m_sectionCancel)
                m_sectionCancel->store(true);
        }
        // Land a finished section compute (also reaps a cancelled one).
        if (m_sectionFut.valid() &&
            m_sectionFut.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            auto r = m_sectionFut.get();
            const bool cancelled = m_sectionCancel && m_sectionCancel->load();
            if (!cancelled && m_sectionEnabled && m_sectionView)
                m_sectionView->apply(std::move(r));
        }
        m_shapeRenderer->render(view, proj, cam.getPosition());
        m_edgeRenderer->render(view, proj);
        // Passes that draw IN FRONT — the reference photo (500), construction
        // planes (501) and axes (502),
        // which is what both already claim ("above body") but never got: the
        // whole block ran before the bodies, and PlaneRenderer draws with
        // glDepthMask(GL_FALSE). Correct for a transparent overlay, but it
        // leaves no depth behind, so any body drawn afterwards passed the depth
        // test and painted straight over it. A plane 2mm ABOVE a body therefore
        // rendered as if it were UNDER it (Steve). Depth TEST is still on, so a
        // plane genuinely behind a body stays correctly hidden.
        if (m_pluginContext) {
            for (auto& pass :
                 materializr::PluginRegistry::instance().renderPasses()) {
                if (pass.priority >= kBodyPassPriority && pass.render)
                    pass.render(*m_pluginContext, view, proj);
            }
        }
        if (m_sectionView) m_sectionView->render(view, proj);

        // Render selection highlight (face/edge/body)
        // The highlight is cached in world coords, so it can't follow a
        // GPU-model-matrix preview on its own. For a gizmo drag it doesn't
        // need to: the shader's only transform is u_mvp = projection * view
        // applied to world-space vertices, so folding the preview into the
        // view matrix moves the outline with the body exactly, with no change
        // to SelectionHighlight and no re-tessellation. m_gizmoPreviewXf is
        // identity when no drag is running, so this is a no-op the rest of
        // the time. (Revolve still hides it — that preview deforms the
        // geometry rather than rigidly transforming it, so no single matrix
        // can carry the outline along.)
        if (!m_revolveLiveActive) {
            m_selectionHighlight->render(*m_selection, *m_document,
                                         view * m_gizmoPreviewXf, proj);
        }

        // Sketch-mode grid drawn here — after bodies/edges/section/highlight —
        // so it blends over solid geometry (and the reference photo being
        // traced) and fades cleanly under the opacity slider, rather than
        // punching grid lines through coplanar faces (the old "grey grid baked
        // into the face that opacity couldn't remove"). The non-sketch world
        // grid already drew EARLY, before the plugin passes (see above).
        if (m_inSketchMode && m_activeSketch) drawGrid();

        // Update gizmo visibility and position based on selection.
        // Suppressed by navigationOnly so a panel pick highlights the body
        // without dropping a move/rotate/scale widget on top of it; the user
        // gets the gizmo back by either explicitly clicking Move/Rotate/Scale
        // (which clears the flag) or by picking again in the viewport.
        //
        // Also hidden whenever ANY interactive op is active — Push/Pull,
        // Loft, Construction Plane, Pattern, Shell, Resize, etc. Without
        // this guard the rotate/move gizmo "sticks around" on top of those
        // popups' previews, looking like an extra widget the user can grab.
        const bool anyInteractiveOpActive =
            m_inSketchMode || m_extrudeCtl.active() || m_edgeCtl.active() ||
            m_ppCtl.active() || anyIopActive() ||
            m_patternActive || m_loftActive || m_planeOpActive ||
            m_sketchPatternActive || m_revolveActive;
        bool gizmoShown = false;
        if (m_selection->hasSelectedBodies() && !m_selection->navigationOnly() &&
            !anyInteractiveOpActive) {
            const auto& sel = m_selection->getSelection();
            int bodyId = sel[0].bodyId;
            try {
                const TopoDS_Shape& shape = m_document->getBody(bodyId);
                // Cache the body's bbox-centre keyed on its TShape pointer.
                // BRepBndLib::Add walks every face's surface and on a complex
                // body (trimmed NURBS heavy) is 50-150ms — running it every
                // frame while a body is selected pinned idle FPS to ~6 with
                // the cooling fan ramping. The TShape pointer is invalidated
                // exactly when topology rebuilds (push/pull, fillet, rotate
                // copy=true, revolve apply) so the cache self-recomputes the
                // moment the geometry changes shape.
                const void* tsh = shape.TShape().get();
                auto cit = m_gizmoCenterCache.find(bodyId);
                glm::vec3 center;
                if (cit != m_gizmoCenterCache.end() &&
                    cit->second.tsh == tsh &&
                    cit->second.loc == shape.Location()) {
                    center = cit->second.center;
                } else {
                    Bnd_Box bbox;
                    BRepBndLib::Add(shape, bbox);
                    double xmin, ymin, zmin, xmax, ymax, zmax;
                    bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                    center = glm::vec3((xmin+xmax)*0.5f,
                                       (ymin+ymax)*0.5f,
                                       (zmin+zmax)*0.5f);
                    m_gizmoCenterCache[bodyId] = {tsh, shape.Location(), center};
                }
                // Follow the live drag. Transforming the centre by the preview
                // matrix is right for all three modes rather than just adding
                // the translation: rotate and scale build their matrix about
                // the pivot, so a centre that sits on the pivot maps to itself
                // and the gizmo correctly stays put, while a multi-body drag
                // about a shared pivot moves this body's centre by exactly the
                // amount the body moved. It also picks up the grid-SNAPPED
                // translation for free — the snap is applied before the matrix
                // is built, so the gizmo can't drift off-grid from the body.
                if (m_gizmoDragging)
                    center = glm::vec3(m_gizmoPreviewXf * glm::vec4(center, 1.0f));
                m_gizmo->setPosition(center);
                m_gizmo->setVisible(true);
                gizmoShown = true;
            } catch (...) {}
        }
        // Sketch-as-construction-plane: when a Sketch OR a SketchRegion is
        // selected (no body in selection), not in sketch-edit, in perspective
        // view, show the gizmo at the parent sketch's plane origin so the
        // user can move/rotate it in 3D — effectively repositioning it like
        // a construction plane. Ortho view is excluded because the user is
        // then implicitly "looking at" the sketch and dragging in 3D there
        // is disorienting. SketchRegion picks count: the user clicked
        // *inside* the sketch, so they're indicating the sketch.
        // Sketch gizmo arm-state: clear if the first sketch in the selection
        // changed since the user armed it. That way clicking a different
        // sketch hides the gizmo and re-surfaces the Tools options first.
        int firstSketchInSel = -1;
        if (m_selection) {
            for (const auto& e : m_selection->getSelection()) {
                if ((e.type == SelectionType::Sketch ||
                     e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
                    firstSketchInSel = e.sketchId; break;
                }
            }
        }
        if (m_sketchGizmoArmed && m_sketchGizmoArmedFor != firstSketchInSel) {
            m_sketchGizmoArmed = false;
            m_sketchGizmoArmedFor = -1;
        }

        if (!gizmoShown && !m_selection->hasSelectedBodies() &&
            (m_selection->hasSelectedSketches() ||
             m_selection->hasSelectedSketchRegions()) &&
            !m_selection->navigationOnly() &&
            !anyInteractiveOpActive &&
            !cam.isOrthographic() &&
            m_sketchGizmoArmed) {
            const auto& sel = m_selection->getSelection();
            int sketchId = -1;
            for (const auto& e : sel) {
                if ((e.type == SelectionType::Sketch ||
                     e.type == SelectionType::SketchRegion) && e.sketchId >= 0) {
                    sketchId = e.sketchId; break;
                }
            }
            auto sk = (sketchId >= 0) ? m_document->getSketch(sketchId) : nullptr;
            if (sk) {
                // Centre the gizmo on the geometric centroid of the sketch's
                // 2D points (mapped through the plane). That's the natural
                // pivot for Move + Rotate: the gizmo sits on the artwork
                // rather than on whatever world point the sketch's plane
                // happens to be anchored at. Falls back to the plane origin
                // when the sketch has no points (e.g., right after creation).
                const auto& pts = sk->getPoints();
                glm::vec3 pivot;
                if (!pts.empty()) {
                    glm::vec2 mn( FLT_MAX,  FLT_MAX), mx(-FLT_MAX, -FLT_MAX);
                    for (const auto& p : pts) {
                        mn = glm::min(mn, p.pos); mx = glm::max(mx, p.pos);
                    }
                    glm::vec2 c2 = (mn + mx) * 0.5f;
                    const gp_Ax3& ax = sk->getPlane().Position();
                    glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                    glm::vec3 X(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                    glm::vec3 Y(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                    pivot = O + X * c2.x + Y * c2.y;
                } else {
                    const gp_Pnt& o = sk->getPlane().Position().Location();
                    pivot = glm::vec3(o.X(), o.Y(), o.Z());
                }
                m_gizmo->setPosition(pivot);
                m_gizmo->setVisible(true);
                gizmoShown = true;
            }
        }
        // Construction plane gizmo: same Move/Rotate handles as the sketch-
        // as-plane case, but anchored at the plane's gp_Pln origin since
        // planes have no point cloud to centroid. Auto-selected during
        // ConstructionPlane placement (Application_InteractiveOps), so the
        // gizmo appears the moment the popup pushes its preview.
        //
        // The anyInteractiveOpActive guard from the body/sketch path
        // *includes* m_planeOpActive; we exclude that one bit here because
        // for a construction plane the popup IS the placement workflow and
        // the gizmo is its primary manipulation surface. Other ops
        // (sketch-edit, extrude, push/pull, edge ops, pattern, loft) still
        // suppress the gizmo as before.
        const bool blockedByOtherOp =
            m_inSketchMode || m_extrudeCtl.active() || m_edgeCtl.active() ||
            m_ppCtl.active() || anyIopActive() ||
            m_patternActive || m_loftActive || m_sketchPatternActive;
        // Construction-axis gizmo — Move only. Same arming pattern as
        // planes: implicit during the Construction Axis popup
        // (m_axisOpActive), opt-in via W/E after commit.
        int firstAxisInSel = -1;
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::Axis && e.axisId >= 0) {
                firstAxisInSel = e.axisId; break;
            }
        }
        if (m_axisGizmoArmed && m_axisGizmoArmedFor != firstAxisInSel) {
            m_axisGizmoArmed = false;
            m_axisGizmoArmedFor = -1;
        }
        const bool axisImplicitArm = m_axisOpActive;
        if (!gizmoShown && !m_selection->hasSelectedBodies() &&
            !m_selection->navigationOnly() &&
            !cam.isOrthographic() &&
            (m_axisGizmoArmed || axisImplicitArm)) {
            int aid = firstAxisInSel;
            if (aid >= 0) {
                const auto* entry = m_document->getAxis(aid);
                if (entry) {
                    // Axes are 1D — Rotate / Scale aren't meaningful, so
                    // snap to Translate if the user's in either of those.
                    if (m_gizmo->getMode() != GizmoMode::Translate) {
                        m_gizmo->setMode(GizmoMode::Translate);
                    }
                    const gp_Pnt& o = entry->origin;
                    m_gizmo->setPosition(glm::vec3((float)o.X(),
                                                   (float)o.Y(),
                                                   (float)o.Z()));
                    m_gizmo->setVisible(true);
                    gizmoShown = true;
                }
            }
        }

        // Clear the plane-arm flag if the selection moved off the plane it
        // was armed for. Keeps "click plane → highlight only; click Move →
        // arm" UX consistent: switching planes hides the previous gizmo and
        // re-surfaces the Tools panel.
        int firstPlaneInSel = -1;
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::Plane && e.planeId >= 0) {
                firstPlaneInSel = e.planeId; break;
            }
        }
        if (m_planeGizmoArmed && m_planeGizmoArmedFor != firstPlaneInSel) {
            m_planeGizmoArmed = false;
            m_planeGizmoArmedFor = -1;
        }

        // During the original Construction Plane placement popup we treat
        // the gizmo as implicitly armed so the user can manipulate the
        // preview from the start. Outside the popup we require an explicit
        // arm (Move/Rotate button or W/E key) — same UX sketches use.
        const bool planeImplicitArm = m_planeOpActive;
        if (!gizmoShown && !m_selection->hasSelectedBodies() &&
            !m_selection->navigationOnly() &&
            !blockedByOtherOp && !cam.isOrthographic() &&
            (m_planeGizmoArmed || planeImplicitArm)) {
            int planeId = firstPlaneInSel;
            if (planeId >= 0) {
                const auto* entry = m_document->getPlane(planeId);
                if (entry) {
                    // Scale isn't meaningful for a plane (it's an infinite
                    // surface; the quad is just a visual handle). Snap to
                    // Translate if the user lands here in Scale mode so the
                    // gizmo handles actually do something on drag.
                    if (m_gizmo->getMode() == GizmoMode::Scale) {
                        m_gizmo->setMode(GizmoMode::Translate);
                    }
                    const gp_Pnt& o = entry->plane.Position().Location();
                    m_gizmo->setPosition(glm::vec3(o.X(), o.Y(), o.Z()));
                    m_gizmo->setVisible(true);
                    gizmoShown = true;
                }
            }
        }

        if (!gizmoShown) m_gizmo->setVisible(false);

        if (m_gizmo->isVisible()) {
            m_gizmo->render(view, proj);
        }

        // Controller 3D gizmo meshes (the Move Face arrows/rings/cubes). This
        // MUST stay inside the FBO-bound 3D pass: the controller-overlay loop
        // further down runs after unbind(), where a raw GL draw lands in the
        // window framebuffer and the UI paints straight over it — see
        // IopGizmo3D in IopViewport.h.
        {
            IopGizmo3D g3d;
            // Colours arrive packed; Gizmo takes a float vec3, so unpack here
            // — the controller side never sees either type.
            auto unpack = [](unsigned c) {
                return glm::vec3(((c      ) & 0xFF) / 255.0f,
                                 ((c >>  8) & 0xFF) / 255.0f,
                                 ((c >> 16) & 0xFF) / 255.0f);
            };
            g3d.arrow = [&](const glm::vec3& at, const glm::vec3& d, unsigned c) {
                m_gizmo->renderArrowAlong(view, proj, at, d, unpack(c));
            };
            g3d.ring = [&](const glm::vec3& at, const glm::vec3& a, unsigned c) {
                m_gizmo->renderRingAbout(view, proj, at, a, unpack(c));
            };
            g3d.cube = [&](const glm::vec3& at, const glm::vec3& d, unsigned c) {
                m_gizmo->renderCubeAlong(view, proj, at, d, unpack(c));
            };
            for (const auto* c : m_iops)
                if (c->active()) c->drawGizmos3D(g3d);
        }

        // Render all stored sketches (visible only) plus the active sketch
        for (int sid : m_document->getAllSketchIds()) {
            if (!m_document->isSketchVisible(sid)) continue;
            if (m_inSketchMode && sid == m_activeSketchId) continue; // drawn below with tool
            auto sk = m_document->getSketch(sid);
            if (sk) {
                m_sketchRenderer->render(sk.get(), nullptr, view, proj, nullptr);
            }
        }
        if (m_inSketchMode && m_activeSketch) {
            // Keep the tool's snap step in sync with the user-chosen grid. The
            // grid itself is the infinite world grid above (now aligned to the
            // sketch plane), so face sketches no longer need a separate per-face
            // grid — drawing across to neighbouring faces just works.
            m_sketchTool->setGridStep(m_sketchGridStep);
            // Sketch millimetres per screen pixel, measured by unprojecting two
            // points one pixel apart — exact for any camera and any plane
            // orientation. Pointing tolerances are a screen distance, so they
            // need the zoom, not just the grid.
            {
                const glm::vec2 p0 = screenToSketch(0.0f, 0.0f, contentSize.x, contentSize.y);
                const glm::vec2 p1 = screenToSketch(1.0f, 0.0f, contentSize.x, contentSize.y);
                m_sketchTool->setPixelScale(glm::length(p1 - p0));
            }
            m_sketchRenderer->render(m_activeSketch.get(), m_sketchTool.get(), view, proj,
                                     m_sketchSolver.get());
        }

        // Highlight hovered/selected sketch regions: translucent fill +
        // boundary, so a selected region reads as a surface (Steve: "a
        // slight shading of the selected sketch regions").
        auto highlightRegion = [&](int sketchId, int regionIdx, const glm::vec3& color, float w,
                                   float fillAlpha) {
            if (sketchId < 0 || regionIdx < 0) return;
            std::shared_ptr<Sketch> sk;
            if (sketchId == m_activeSketchId && m_activeSketch) sk = m_activeSketch;
            else sk = m_document->getSketch(sketchId);
            if (!sk) return;
            if (fillAlpha > 0.0f)
                m_sketchRenderer->renderRegionFill(sk.get(), regionIdx, color,
                                                   fillAlpha, view, proj);
            m_sketchRenderer->renderRegionBoundary(sk.get(), regionIdx, color, w, view, proj);
        };
        // Selected regions in solid yellow
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::SketchRegion) {
                highlightRegion(e.sketchId, e.subShapeIndex,
                                glm::vec3(1.0f, 0.85f, 0.1f), 4.0f, 0.28f);
            } else if (e.type == SelectionType::Sketch && e.sketchId >= 0) {
                // Whole-sketch highlight — covers every primitive (so open
                // profiles light up too, not just closed regions).
                std::shared_ptr<Sketch> sk;
                if (e.sketchId == m_activeSketchId && m_activeSketch) sk = m_activeSketch;
                else sk = m_document->getSketch(e.sketchId);
                if (sk) {
                    m_sketchRenderer->renderSketchHighlight(
                        sk.get(), glm::vec3(1.0f, 0.85f, 0.1f), 4.0f, view, proj);
                }
            }
        }
        // Highlight the geometry a history step touches — hovering a step
        // previews it, selecting (pinning) a step keeps it. Lets the user see
        // WHAT each step made before editing it. Hover wins over the pinned
        // step so moving the cursor down the list previews each in turn.
        if (m_historyPanel && m_history) {
            const int hov = m_historyPanel->getHoveredStep();
            const int es  = m_historyPanel->getEditingStep();
            const int step = (hov >= 0) ? hov : es;
            const Operation* op = (step >= 0) ? m_history->getStep(step) : nullptr;
            // A hover preview is a touch dimmer than a pinned selection.
            const float k = (hov >= 0 && hov != es) ? 0.8f : 1.0f;
            if (op) {
                if (auto* se = dynamic_cast<const SketchEditOp*>(op)) {
                    // Sketch step: the WHOLE sketch outline normally; the
                    // specific edited element(s) only while we're actually IN
                    // that sketch (Steve — the relevant sketch is enough for a
                    // history item; element precision is for sketch editing).
                    auto tgt = se->getTarget();
                    int sid = (tgt && m_document) ? m_document->findSketchId(tgt.get()) : -1;
                    bool shown = tgt && (tgt == m_activeSketch ||
                                         (sid >= 0 && m_document->isSketchVisible(sid)));
                    if (shown) {
                        if (m_inSketchMode && tgt == m_activeSketch) {
                            std::set<int> lines, circles, arcs;
                            se->getEditedElements(lines, circles, arcs);
                            m_sketchRenderer->renderElementsHighlight(
                                tgt.get(), lines, circles, arcs,
                                glm::vec3(1.0f, 0.55f, 0.1f), 5.0f, view, proj);
                        } else {
                            m_sketchRenderer->renderSketchHighlight(
                                tgt.get(), glm::vec3(0.2f * k, 0.8f * k, 1.0f * k),
                                4.0f, view, proj);
                        }
                    }
                } else {
                    // 3D op: fillet/chamfer highlight their exact blend faces;
                    // every other op (extrude / boolean / pattern / shell /
                    // revolve / pushpull / …) highlights the bodies it created
                    // or modified, read from its captureDiff.
                    std::vector<TopoDS_Shape> shapes;
                    if (auto* f = dynamic_cast<const FilletOp*>(op))
                        shapes = f->getGeneratedFaces();
                    else if (auto* c = dynamic_cast<const ChamferOp*>(op))
                        shapes = c->getGeneratedFaces();
                    else {
                        OperationDiff d = op->captureDiff();
                        auto pushBody = [&](int id) {
                            try {
                                TopoDS_Shape s = m_document->getBody(id);
                                if (!s.IsNull()) shapes.push_back(s);
                            } catch (...) {}
                        };
                        for (int id : d.created) pushBody(id);
                        for (const auto& m : d.modifiedBefore) pushBody(m.first);
                    }
                    if (!shapes.empty() && m_selectionHighlight)
                        m_selectionHighlight->highlightShapes(
                            shapes, view, proj,
                            glm::vec3(0.25f * k, 0.7f * k, 1.0f * k));
                }
            }
        }

        // Hovered region in cyan (drawn last so it's on top)
        highlightRegion(m_hoveredSketchId, m_hoveredRegionIndex,
                        glm::vec3(0.2f, 0.9f, 1.0f), 3.0f, 0.12f);

        // Box-select rectangle (screen-space, drawn last so it's on top).
        if (m_boxSelect && m_boxSelect->isActive()) {
            m_boxSelect->render(contentSize.x, contentSize.y);
        }

        m_viewport->unbind();

        ImGui::Image(
            static_cast<ImTextureID>(m_viewport->getTextureID()),
            contentSize,
            ImVec2(0, 1),
            ImVec2(1, 0)
        );

        // --- Live dimension overlay: a measuring annotation (offset line with
        // arrowheads + value) shown while drawing a sketch line/circle, extruding,
        // or moving a body. Drawn in screen space over the viewport image. ---
        {
            ImVec2 imgMin = ImGui::GetItemRectMin();
            ImVec2 imgSize = ImGui::GetItemRectSize();
            glm::mat4 vpMat = proj * view;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            auto toImg = [&](glm::vec3 w, ImVec2& out) -> bool {
                glm::vec4 c = vpMat * glm::vec4(w, 1.0f);
                if (c.w <= 1e-5f) return false; // behind camera
                out = ImVec2(imgMin.x + (c.x / c.w * 0.5f + 0.5f) * imgSize.x,
                             imgMin.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * imgSize.y);
                return true;
            };
            // Style enum lets callers pick between the subtle sketch / move
            // gizmo readout (Normal) and the bolder fluorescent-yellow arrow
            // used during active body operations (Bold) — push/pull, fillet,
            // chamfer. Sketch inferences keep Normal because 4-5 of them can
            // run at once and the heavier visuals would clutter the canvas.
            enum class DimStyle { Normal, Bold };
            auto drawDim = [&](glm::vec3 aW, glm::vec3 bW, const char* label,
                               DimStyle style = DimStyle::Normal) {
                ImVec2 sa, sb;
                if (!toImg(aW, sa) || !toImg(bW, sb)) return;
                ImVec2 dir(sb.x - sa.x, sb.y - sa.y);
                float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (len < 2.0f) return;
                dir.x /= len; dir.y /= len;
                ImVec2 perp(-dir.y, dir.x);
                const bool bold = (style == DimStyle::Bold);
                const float off    = bold ? 30.0f : 26.0f;
                const float ah     = bold ? 13.0f : 7.0f;     // arrowhead size
                const float thick  = bold ? 3.0f  : 1.5f;     // dim line thickness
                const float extThk = bold ? 1.5f  : 1.0f;     // extension thickness
                ImVec2 da(sa.x + perp.x * off, sa.y + perp.y * off);
                ImVec2 db(sb.x + perp.x * off, sb.y + perp.y * off);
                // Bold uses a saturated amber/yellow that pops against both the
                // steel-blue body palette and the dark gray viewport. A black
                // outline (drawn one thickness wider underneath) keeps it
                // readable against any surface colour.
                ImU32 col       = bold ? IM_COL32(255, 200,  60, 255)
                                       : IM_COL32(235, 235, 240, 255);
                ImU32 outline   = IM_COL32( 20,  20,  28, 230);
                ImU32 ext       = bold ? IM_COL32(255, 200,  60, 200)
                                       : IM_COL32(170, 170, 180, 150);
                dl->AddLine(sa, da, ext, extThk);                 // extension lines
                dl->AddLine(sb, db, ext, extThk);
                if (bold) dl->AddLine(da, db, outline, thick + 2.0f); // halo
                dl->AddLine(da, db, col, thick);                  // dimension line
                auto arrow = [&](ImVec2 tip, ImVec2 along) {
                    ImVec2 base(tip.x - along.x * ah, tip.y - along.y * ah);
                    ImVec2 wing1(base.x + perp.x * ah * 0.5f, base.y + perp.y * ah * 0.5f);
                    ImVec2 wing2(base.x - perp.x * ah * 0.5f, base.y - perp.y * ah * 0.5f);
                    if (bold) {
                        // Black halo around the arrowhead so it doesn't melt
                        // into the body silhouette when the camera is edge-on.
                        ImVec2 g1(base.x + perp.x * (ah * 0.5f + 1.6f),
                                  base.y + perp.y * (ah * 0.5f + 1.6f));
                        ImVec2 g2(base.x - perp.x * (ah * 0.5f + 1.6f),
                                  base.y - perp.y * (ah * 0.5f + 1.6f));
                        ImVec2 gt(tip.x + along.x * 1.6f, tip.y + along.y * 1.6f);
                        dl->AddTriangleFilled(gt, g1, g2, outline);
                    }
                    dl->AddTriangleFilled(tip, wing1, wing2, col);
                };
                arrow(da, ImVec2(-dir.x, -dir.y));
                arrow(db, dir);
                // Skip the centred label when the caller passes a null/empty
                // label — for the fillet/chamfer drag handle we instead pin
                // the readout to the mouse cursor (drawn by the caller),
                // matching the arc-angle preview's "follow the mouse" pill.
                if (label && label[0]) {
                    ImVec2 ts = ImGui::CalcTextSize(label);
                    ImVec2 mid((da.x + db.x) * 0.5f + perp.x * 12.0f,
                               (da.y + db.y) * 0.5f + perp.y * 12.0f);
                    ImVec2 tp(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f);
                    dl->AddRectFilled(ImVec2(tp.x - 4, tp.y - 3),
                                      ImVec2(tp.x + ts.x + 4, tp.y + ts.y + 3),
                                      IM_COL32(20, 20, 28, bold ? 235 : 205), 3.0f);
                    if (bold) dl->AddRect(ImVec2(tp.x - 4, tp.y - 3),
                                          ImVec2(tp.x + ts.x + 4, tp.y + ts.y + 3),
                                          col, 3.0f, 0, 1.5f);
                    dl->AddText(tp, col, label);
                }
            };

            // Revolve indicator — yellow arced arrow showing the rotation
            // axis + current angle while the Revolve popup is up. Now
            // CLICKABLE: press on the arc to grab it, drag tangentially to
            // spin the body around the axis live. The cursor's projected
            // angle around the axis drives a per-frame delta which feeds
            // back into m_revolveAngle + the existing live-preview machinery.
            m_revolveArcWasHovered = false;
            if (m_revolveActive && m_revolveWhatIdx == 0) {
                gp_Pnt axisOrigin(0, 0, 0);
                gp_Dir axisDir(0, 0, 1);
                if (m_revolveAxisId >= 0) {
                    const auto* a = m_document->getAxis(m_revolveAxisId);
                    if (a) { axisOrigin = a->origin; axisDir = a->direction; }
                } else {
                    switch (m_revolveWorldAxisIdx) {
                        case 0: axisDir = gp_Dir(1, 0, 0); break;
                        case 1: axisDir = gp_Dir(0, 0, 1); break;
                        case 2: axisDir = gp_Dir(0, 1, 0); break;
                    }
                }
                glm::vec3 O((float)axisOrigin.X(), (float)axisOrigin.Y(),
                            (float)axisOrigin.Z());
                glm::vec3 D = glm::normalize(glm::vec3((float)axisDir.X(),
                                                       (float)axisDir.Y(),
                                                       (float)axisDir.Z()));
                // Orthonormal basis in the plane of rotation. Pick a
                // world-aligned helper not collinear with D so the cross
                // product never collapses; both quarter-rolls (T, B) form
                // the arc plane.
                glm::vec3 helper = (std::abs(D.y) < 0.9f) ? glm::vec3(0, 1, 0)
                                                          : glm::vec3(1, 0, 0);
                glm::vec3 T = glm::normalize(glm::cross(D, helper));
                glm::vec3 B = glm::normalize(glm::cross(D, T));

                // Radius scales with the picked axis's half-length so the
                // arc reads at similar visual size whatever the document
                // is up to. Clamped to a useful range.
                float radius = 12.0f;
                if (m_revolveAxisId >= 0) {
                    const auto* a = m_document->getAxis(m_revolveAxisId);
                    if (a) radius = std::clamp((float)a->halfLength * 0.4f,
                                                8.0f, 60.0f);
                }
                // Sweep visualises the current angle but caps at ±270° so
                // a 360° revolve doesn't render as a closed circle that
                // hides its own arrowhead. Below 5° we draw a default
                // 45°-ish stub so the gizmo stays grabbable when the
                // user just opened the popup (angle reset to 0) — the
                // stub fades into a real sweep the moment they drag.
                float ang = m_revolveAngle * (float)M_PI / 180.0f;
                float sign = (ang >= 0.0f) ? 1.0f : -1.0f;
                float sweep = std::min(std::abs(ang), (float)(M_PI * 1.5));
                sweep *= sign;
                if (std::abs(sweep) < (float)(M_PI / 36.0)) { // <5°
                    sweep = (float)(M_PI / 4.0);              // 45° hint stub
                }

                constexpr int N = 40;
                ImVec2 pts[N + 1];
                int nPts = 0;
                for (int i = 0; i <= N; ++i) {
                    float t = float(i) / float(N);
                    float a = sweep * t;
                    glm::vec3 p = O + radius * (std::cos(a) * T + std::sin(a) * B);
                    ImVec2 sp;
                    if (toImg(p, sp)) pts[nPts++] = sp;
                }
                if (nPts > 1) {
                    // Cursor → arc hit-test (closest point on any segment;
                    // 10 px pickability band — generous because the arc is
                    // thin and the user is moving deliberately when they
                    // grab it).
                    ImVec2 mp = ImGui::GetMousePos();
                    float bestD = FLT_MAX;
                    for (int i = 0; i + 1 < nPts; ++i) {
                        float dx = pts[i + 1].x - pts[i].x;
                        float dy = pts[i + 1].y - pts[i].y;
                        float L2 = dx * dx + dy * dy;
                        if (L2 < 1e-6f) continue;
                        float t = ((mp.x - pts[i].x) * dx +
                                   (mp.y - pts[i].y) * dy) / L2;
                        t = std::clamp(t, 0.0f, 1.0f);
                        float cx = pts[i].x + dx * t;
                        float cy = pts[i].y + dy * t;
                        float d = std::hypot(mp.x - cx, mp.y - cy);
                        if (d < bestD) bestD = d;
                    }
                    bool arcHovered = (bestD < 10.0f) &&
                                       ImGui::IsWindowHovered(
                                           ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                           ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
                    m_revolveArcWasHovered = arcHovered || m_revolveArcDragging;

                    // Hovered or dragging → brighter + thicker so the user
                    // sees the affordance.
                    const bool hot = arcHovered || m_revolveArcDragging;
                    const ImU32 col     = hot ? IM_COL32(255, 230,  80, 255)
                                              : IM_COL32(255, 200,  60, 255);
                    const ImU32 outline = IM_COL32( 20,  20,  28, 220);
                    const float thick   = hot ? 5.5f : 3.5f;
                    const float haloThk = thick + 2.5f;
                    dl->AddPolyline(pts, nPts, outline, 0, haloThk);
                    dl->AddPolyline(pts, nPts, col,     0, thick);

                    // Project cursor onto the rotation plane to read its
                    // angular position. Returns false if the cursor's ray
                    // is parallel to the plane (~edge-on view).
                    auto cursorAngle = [&](float* outAng) -> bool {
                        ImVec2 wp = ImGui::GetItemRectMin();
                        // contentSize / view / proj are in scope from the
                        // surrounding renderViewport block.
                        float lx = mp.x - wp.x;
                        float ly = mp.y - wp.y;
                        float ndcX = (2.0f * lx) / contentSize.x - 1.0f;
                        float ndcY = 1.0f - (2.0f * ly) / contentSize.y;
                        glm::mat4 invVP = glm::inverse(proj * view);
                        glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
                        glm::vec4 farH  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
                        glm::vec3 rayO = glm::vec3(nearH) / nearH.w;
                        glm::vec3 rayD = glm::normalize(
                            glm::vec3(farH) / farH.w - rayO);
                        float denom = glm::dot(rayD, D);
                        if (std::abs(denom) < 1e-5f) return false;
                        float t = glm::dot(O - rayO, D) / denom;
                        if (t <= 0.0f) return false;
                        glm::vec3 hit = rayO + rayD * t;
                        glm::vec3 v = hit - O;
                        *outAng = std::atan2(glm::dot(v, B), glm::dot(v, T));
                        return true;
                    };

                    // Press → grab the arc, capture initial cursor angle
                    // and body angle. Subsequent frames track the delta.
                    if (arcHovered && !m_revolveArcDragging &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        float ca = 0.0f;
                        if (cursorAngle(&ca)) {
                            m_revolveArcDragging = true;
                            m_revolveArcDragLastCursorAng = ca;
                            m_revolveArcDragAngleAccum = 0.0f;
                            m_revolveArcDragStartBodyAng = m_revolveAngle;
                        }
                    }
                    // Drag → integrate the per-frame angular delta into the
                    // body angle. atan2 wrap-around handled by clamping the
                    // delta to [-π, π] before accumulating.
                    if (m_revolveArcDragging &&
                        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        float ca = 0.0f;
                        if (cursorAngle(&ca)) {
                            float d = ca - m_revolveArcDragLastCursorAng;
                            while (d >  (float)M_PI) d -= 2.0f * (float)M_PI;
                            while (d < -(float)M_PI) d += 2.0f * (float)M_PI;
                            m_revolveArcDragAngleAccum += d;
                            m_revolveArcDragLastCursorAng = ca;
                            float deg = m_revolveArcDragStartBodyAng +
                                        m_revolveArcDragAngleAccum *
                                        180.0f / (float)M_PI;
                            // Snap to 5° increments — the arc drag is the
                            // coarse positioning tool; fine adjustments
                            // happen through the popup's typed-angle
                            // InputText, which doesn't snap.
                            deg = std::round(deg / 5.0f) * 5.0f;
                            // Cap to two full turns so a wild drag doesn't
                            // run away off-screen forever.
                            deg = std::clamp(deg, -720.0f, 720.0f);
                            if (std::abs(deg - m_revolveAngle) > 0.05f) {
                                m_revolveAngle = deg;
                                std::snprintf(m_revolveAngleBuf,
                                              sizeof(m_revolveAngleBuf),
                                              "%.1f", m_revolveAngle);
                                revolveLiveApply(m_revolveAngle);
                            }
                        }
                    }
                    if (m_revolveArcDragging &&
                        !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        m_revolveArcDragging = false;
                    }

                    // Arrowhead at the arc's end. Direction = tangent to
                    // the arc at the last sample (perpendicular to the
                    // radius vector there, in the sweep direction).
                    float endA = sweep;
                    glm::vec3 endP = O + radius * (std::cos(endA) * T + std::sin(endA) * B);
                    glm::vec3 tangent = glm::normalize(
                        -std::sin(endA) * T + std::cos(endA) * B);
                    if (sweep < 0) tangent = -tangent;
                    glm::vec3 outward = glm::normalize(
                        std::cos(endA) * T + std::sin(endA) * B);
                    float ah = 11.0f;
                    // Build a small triangle in world space (using the
                    // arc-plane basis), project to screen, fill.
                    glm::vec3 wTip  = endP + tangent * (ah * 0.18f); // arc-units
                    glm::vec3 wBase = endP - tangent * (ah * 0.18f);
                    glm::vec3 wW1   = wBase + outward * (ah * 0.10f);
                    glm::vec3 wW2   = wBase - outward * (ah * 0.10f);
                    ImVec2 sTip, sW1, sW2;
                    if (toImg(wTip, sTip) && toImg(wW1, sW1) && toImg(wW2, sW2)) {
                        dl->AddTriangleFilled(sTip, sW1, sW2, col);
                        dl->AddTriangle(sTip, sW1, sW2, outline, 1.2f);
                    }

                    // Angle label near the arc midpoint.
                    float midA = sweep * 0.5f;
                    glm::vec3 midW = O + (radius + 4.0f) *
                        (std::cos(midA) * T + std::sin(midA) * B);
                    ImVec2 sMid;
                    if (toImg(midW, sMid)) {
                        char lbl[24];
                        std::snprintf(lbl, sizeof(lbl), "%.1f\xC2\xB0",
                                      m_revolveAngle);
                        ImVec2 ts = ImGui::CalcTextSize(lbl);
                        ImVec2 tp(sMid.x - ts.x * 0.5f, sMid.y - ts.y * 0.5f);
                        dl->AddRectFilled(
                            ImVec2(tp.x - 5, tp.y - 3),
                            ImVec2(tp.x + ts.x + 5, tp.y + ts.y + 3),
                            IM_COL32(20, 20, 28, 235), 3.0f);
                        dl->AddRect(
                            ImVec2(tp.x - 5, tp.y - 3),
                            ImVec2(tp.x + ts.x + 5, tp.y + ts.y + 3),
                            col, 3.0f, 0, 1.5f);
                        dl->AddText(tp, col, lbl);
                    }
                }
            }

            // Scale Face 2D gizmo: one arrow per in-plane axis, tip
            // tracking the live percentage. Drag a tip to scale that
            // direction (handled in the input section below). Arrows are
            // labelled by COLOUR (red / blue) matching the panel's per-
            // axis sliders — the old "U" / "V" letters meant nothing to
            // the user.
            // Controller handle overlays. Was a ScaleFace-specific block that
            // read its gizmo frame through public accessors; now any
            // controller that wants handles draws its own through IopOverlay,
            // and this loop doesn't know which op it is.
            {
                IopOverlay ov;
                ov.toScreen = [&](const glm::vec3& w, glm::vec2& out) {
                    ImVec2 s;
                    if (!toImg(w, s)) return false;
                    out = glm::vec2(s.x, s.y);
                    return true;
                };
                ov.line = [&](glm::vec2 a, glm::vec2 b, unsigned c, float th) {
                    dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), c, th);
                };
                ov.disc = [&](glm::vec2 c, float r, unsigned col) {
                    dl->AddCircleFilled(ImVec2(c.x, c.y), r, col);
                };
                ov.label = [&](glm::vec2 at, const char* txt, unsigned col) {
                    ImVec2 ts = ImGui::CalcTextSize(txt);
                    ImVec2 tp(at.x + 10.0f, at.y - ts.y * 0.5f);
                    dl->AddRectFilled(ImVec2(tp.x - 4, tp.y - 2),
                                      ImVec2(tp.x + ts.x + 4, tp.y + ts.y + 2),
                                      IM_COL32(20, 20, 28, 220), 3.0f);
                    dl->AddText(tp, col, txt);
                };
                ov.triangle = [&](glm::vec2 a, glm::vec2 b, glm::vec2 c,
                                  unsigned col) {
                    dl->AddTriangleFilled(ImVec2(a.x, a.y), ImVec2(b.x, b.y),
                                          ImVec2(c.x, c.y), col);
                };
                ov.rect = [&](glm::vec2 mn, glm::vec2 mx, unsigned col,
                              float rounding, float thickness) {
                    if (thickness <= 0.0f)
                        dl->AddRectFilled(ImVec2(mn.x, mn.y), ImVec2(mx.x, mx.y),
                                          col, rounding);
                    else
                        dl->AddRect(ImVec2(mn.x, mn.y), ImVec2(mx.x, mx.y), col,
                                    rounding, 0, thickness);
                };
                ov.text = [&](glm::vec2 at, const char* txt, unsigned col) {
                    dl->AddText(ImVec2(at.x, at.y), col, txt);
                };
                ov.textSize = [&](const char* txt) {
                    ImVec2 ts = ImGui::CalcTextSize(txt);
                    return glm::vec2(ts.x, ts.y);
                };
                {
                    ImVec2 mp = ImGui::GetMousePos();
                    ov.mouse = glm::vec2(mp.x, mp.y);
                }
                // Every ACTIVE controller gets the overlay, whether or not it
                // takes pointer input. These used to be one gate, which was
                // only ever true by coincidence — the four controllers that
                // drew handles also dragged them. Split draws a ghost plane and
                // has nothing to drag, and under the old gate it silently drew
                // nothing. drawOverlay's default is a no-op, so a controller
                // that doesn't draw still costs nothing here.
                for (const auto* c : m_iops)
                    if (c->active())
                        c->drawOverlay(ov);
            }

            char dbuf[40];
            // im-touch: the distance input well (rendered later, different
            // scope) anchors near the geometry being modified. The anchor is
            // LATCHED in world space when the action starts — the well must
            // not chase the growing arrow while values change (same rule as
            // the sketch bubbles' frozen endpoints), but projecting the
            // latched point each frame keeps it tracking camera pan/zoom.
            {
                const bool actionLive =
                    m_extrudeCtl.active() || (m_ppCtl.active() && m_ppCtl.hasArrow()) ||
                    m_edgeCtl.active();
                if (!actionLive) m_actionAnchorLatched = false;
                m_actionAnchorValid = false;
                if (actionLive) {
                    if (!m_actionAnchorLatched) {
                        m_actionAnchorW =
                            m_extrudeCtl.active()
                                ? m_extrudeCtl.origin() +
                                      m_extrudeCtl.normal() * m_extrudeCtl.distance()
                            : m_ppCtl.active()
                                ? m_ppCtl.origin() +
                                      m_ppCtl.normal() * m_ppCtl.distance()
                                : m_edgeCtl.mid();   // fillet/chamfer: edge midpoint
                        m_actionAnchorLatched = true;
                    }
                    ImVec2 aS;
                    if (toImg(m_actionAnchorW, aS)) {
                        m_actionAnchorX = aS.x;
                        m_actionAnchorY = aS.y;
                        m_actionAnchorValid = true;
                    }
                }
            }
            if (m_extrudeCtl.active()) {
                std::snprintf(dbuf, sizeof(dbuf), "%s", materializr::fmtLength(std::abs(m_extrudeCtl.distance())).c_str());
                drawDim(m_extrudeCtl.origin(),
                        m_extrudeCtl.origin() + m_extrudeCtl.normal() * m_extrudeCtl.distance(), dbuf,
                        DimStyle::Bold);
            } else if (m_ppCtl.active() && m_ppCtl.hasArrow()) {
                // Arrow out of the face + signed-distance measurement.
                // Push/pull STARTS at 0 mm (no change), and drawDim draws
                // nothing for a near-zero span — which left the face with no
                // visible handle at all (extrude starts non-zero, so it
                // always shows one). Until the distance moves, draw a
                // STARTER handle instead: a double-headed arrow through the
                // face centre along ±normal, in the Bold palette.
                if (std::abs(m_ppCtl.distance()) > 0.05f) {
                    std::snprintf(dbuf, sizeof(dbuf), "%s", materializr::fmtLength(m_ppCtl.distance()).c_str());
                    drawDim(m_ppCtl.origin(),
                            m_ppCtl.origin() + m_ppCtl.normal() * m_ppCtl.distance(), dbuf,
                            DimStyle::Bold);
                } else {
                    ImVec2 so, sn;
                    if (toImg(m_ppCtl.origin(), so) &&
                        toImg(m_ppCtl.origin() + m_ppCtl.normal(), sn)) {
                        ImVec2 d(sn.x - so.x, sn.y - so.y);
                        const float L = std::sqrt(d.x * d.x + d.y * d.y);
                        if (L > 1e-3f) { d.x /= L; d.y /= L; }
                        else           { d = ImVec2(0.0f, -1.0f); }
                        const float s3   = uiScale();
                        const float half = 40.0f * s3;
                        const float ah   = 12.0f * s3;
                        const ImVec2 a(so.x - d.x * half, so.y - d.y * half);
                        const ImVec2 b(so.x + d.x * half, so.y + d.y * half);
                        const ImU32 col     = IM_COL32(255, 200,  60, 255);
                        const ImU32 outline = IM_COL32( 20,  20,  28, 230);
                        const ImVec2 perp(-d.y, d.x);
                        dl->AddLine(a, b, outline, 5.0f);
                        dl->AddLine(a, b, col, 3.0f);
                        auto head = [&](ImVec2 tip, float sign) {
                            ImVec2 back(tip.x - d.x * ah * sign,
                                        tip.y - d.y * ah * sign);
                            ImVec2 w1(back.x + perp.x * ah * 0.5f,
                                      back.y + perp.y * ah * 0.5f);
                            ImVec2 w2(back.x - perp.x * ah * 0.5f,
                                      back.y - perp.y * ah * 0.5f);
                            dl->AddTriangleFilled(tip, w1, w2, col);
                            dl->AddTriangle(tip, w1, w2, outline, 1.2f);
                        };
                        head(b,  1.0f);
                        head(a, -1.0f);
                        const std::string hintS = materializr::fmtLength(0.0) + " — drag"; const char* hint = hintS.c_str();
                        ImVec2 ts = ImGui::CalcTextSize(hint);
                        ImVec2 tp(so.x + perp.x * 18.0f * s3 - ts.x * 0.5f,
                                  so.y + perp.y * 18.0f * s3 - ts.y * 0.5f);
                        dl->AddRectFilled(ImVec2(tp.x - 5, tp.y - 3),
                                          ImVec2(tp.x + ts.x + 5, tp.y + ts.y + 3),
                                          outline, 3.0f);
                        dl->AddText(tp, col, hint);
                    }
                }
            } else if (m_gizmoDragging && !m_planeGizmoDrag.empty()) {
                // Construction-plane drag readout. The world-axis dim line
                // from origin (used for body/sketch translate below) isn't
                // useful here — for an askew plane the user cares about
                // distance along the plane's own normal and the rotation
                // angle, not the world-coord delta. Pin a compact pill near
                // the cursor with the values that matter for plane work.
                ImVec2 mp = ImGui::GetMousePos();
                if (m_gizmo->getMode() == GizmoMode::Translate) {
                    // Two values: how far the drag moved the plane along its
                    // own normal (signed, this drag only), and where that
                    // puts the plane in absolute terms (signed distance
                    // from world origin along the same normal). The
                    // absolute reading is what the popup's Offset slider
                    // would show for an axis-aligned plane and stays
                    // meaningful when the plane is askew.
                    const auto& pln = m_planeGizmoDrag.front().second;
                    const gp_Dir& nd = pln.Position().Direction();
                    gp_Pnt o = pln.Position().Location();
                    glm::vec3 nrm((float)nd.X(), (float)nd.Y(), (float)nd.Z());
                    glm::vec3 origBefore((float)o.X(), (float)o.Y(), (float)o.Z());
                    float delta    = glm::dot(m_gizmoTotalDelta, nrm);
                    float absAfter = glm::dot(origBefore + m_gizmoTotalDelta, nrm);
                    if (m_snapToGrid && m_sketchGridStep > 0.0f) {
                        float step = m_sketchGridStep;
                        delta    = std::round(delta    / step) * step;
                        absAfter = std::round(absAfter / step) * step;
                    }
                    std::snprintf(dbuf, sizeof(dbuf),
                                  "\xCE\x94 %s   |   Origin %s",
                                  materializr::fmtLength(delta).c_str(),
                                  materializr::fmtLength(absAfter).c_str());
                } else if (m_gizmo->getMode() == GizmoMode::Rotate) {
                    char axL = '?';
                    if (std::abs(m_gizmoRotAxis.x) > 0.5f)      axL = 'X';
                    else if (std::abs(m_gizmoRotAxis.y) > 0.5f) axL = 'Z'; // user Z = world Y
                    else if (std::abs(m_gizmoRotAxis.z) > 0.5f) axL = 'Y'; // user Y = world Z
                    // Mirror the plane-drag snap policy (5° hard, soft 15°)
                    // so the readout matches what's actually being applied.
                    float shownAng = m_gizmoTotalAngle;
                    if (m_snapToGrid) {
                        shownAng = std::round(shownAng / 5.0f) * 5.0f;
                    } else {
                        float n = std::round(shownAng / 15.0f) * 15.0f;
                        if (std::abs(shownAng - n) < 3.0f) shownAng = n;
                    }
                    std::snprintf(dbuf, sizeof(dbuf),
                                  "%.1f\xC2\xB0  about %c",
                                  shownAng, axL);
                } else {
                    dbuf[0] = '\0';
                }
                if (dbuf[0]) {
                    ImVec2 ts = ImGui::CalcTextSize(dbuf);
                    ImVec2 tp(mp.x + 14.0f, mp.y - ts.y * 0.5f);
                    dl->AddRectFilled(
                        ImVec2(tp.x - 5, tp.y - 3),
                        ImVec2(tp.x + ts.x + 5, tp.y + ts.y + 3),
                        IM_COL32(20, 20, 28, 235), 3.0f);
                    dl->AddRect(
                        ImVec2(tp.x - 5, tp.y - 3),
                        ImVec2(tp.x + ts.x + 5, tp.y + ts.y + 3),
                        IM_COL32(255, 200, 60, 255), 3.0f, 0, 1.5f);
                    dl->AddText(tp, IM_COL32(255, 200, 60, 255), dbuf);
                }
            }

            // Rotate (°) / Scale (%) readout near the body during a gizmo drag —
            // the analogue of the mm readout for moves. Uses the cached pivot
            // so sketch-only drags get a readout even with no body shape.
            // Suppressed for plane-only drags so we don't show two readouts
            // (the cursor-pinned plane one above already covers it with the
            // 5° snap policy applied).
            if (m_gizmoDragging && m_planeGizmoDrag.empty() &&
                (m_gizmo->getMode() == GizmoMode::Rotate ||
                 m_gizmo->getMode() == GizmoMode::Scale)) {
                try {
                    glm::vec3 bc = m_gizmoSharedPivot;
                    {
                        ImVec2 sp;
                        if (toImg(bc, sp)) {
                            char rb[48];
                            if (m_gizmo->getMode() == GizmoMode::Rotate) {
                                // Show the ACTUAL applied angle (matches the
                                // snap policy: hard 15° when snap-on, soft
                                // 45° when off) plus the rotation axis name
                                // so the user sees which axis the value is
                                // about — m_gizmoRotAxis is the world-axis
                                // unit vector set when the drag started.
                                float shown;
                                if (m_snapToGrid) {
                                    shown = std::round(m_gizmoTotalAngle / 15.0f) * 15.0f;
                                } else {
                                    float n = std::round(m_gizmoTotalAngle / 45.0f) * 45.0f;
                                    shown = (std::abs(m_gizmoTotalAngle - n) < 7.0f)
                                                ? n : m_gizmoTotalAngle;
                                }
                                // World-axis -> user-axis remap (same as the
                                // translate label above): world Y is user Z
                                // (the green "up" axis), world Z is user Y.
                                char ra = '?';
                                if (std::abs(m_gizmoRotAxis.x) > 0.5f) ra = 'X';
                                else if (std::abs(m_gizmoRotAxis.y) > 0.5f) ra = 'Z';
                                else if (std::abs(m_gizmoRotAxis.z) > 0.5f) ra = 'Y';
                                if (ra == '?')
                                    std::snprintf(rb, sizeof(rb), "%.0f\xC2\xB0", shown);
                                else
                                    std::snprintf(rb, sizeof(rb), "%c  %.0f\xC2\xB0", ra, shown);
                            } else
                                std::snprintf(rb, sizeof(rb), "X %.0f%%  Y %.0f%%  Z %.0f%%",
                                              m_gizmoTotalScale.x*100, m_gizmoTotalScale.y*100,
                                              m_gizmoTotalScale.z*100);
                            ImVec2 ts = ImGui::CalcTextSize(rb);
                            ImVec2 tp(sp.x - ts.x*0.5f, sp.y - ts.y - 14.0f);
                            dl->AddRectFilled(ImVec2(tp.x-4, tp.y-2), ImVec2(tp.x+ts.x+4, tp.y+ts.y+2),
                                              IM_COL32(20,20,28,205), 3.0f);
                            dl->AddText(tp, IM_COL32(235,235,240,255), rb);
                        }
                    }
                } catch (...) {}
            }

            // Sketch preview dimensions: line length, circle diameter (across the
            // full width — makers dimension by diameter), rectangle both sides.
            if (m_inSketchMode && m_activeSketch && m_sketchTool && m_sketchTool->hasPreview()) {
                const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                glm::vec3 X(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                glm::vec3 Y(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                auto sketch2world = [&](glm::vec2 p) { return O + p.x * X + p.y * Y; };

                // Length readout: two decimals with trailing zeros trimmed, so
                // short segments read at hundredth precision (0.27) while round
                // values stay clean (0.90 -> 0.9, 1.00 -> 1). The old tenths
                // format hid everything under 0.1 mm. (No <cstring> needed —
                // trim over the fixed buffer by index.)
                auto fmtLen = [](char* out, size_t n, float v) {   // v is mm; prints display unit
                    char num[32];
                    int m = std::snprintf(num, sizeof(num), "%.*f",
                                          materializr::unitInfo(materializr::currentUnit()).decimals,
                                          materializr::toDisplay(v));
                    if (m > 0) {
                        bool hasDot = false;
                        for (int k = 0; k < m; ++k) if (num[k] == '.') { hasDot = true; break; }
                        if (hasDot) {
                            int e = m - 1;
                            while (e > 0 && num[e] == '0') num[e--] = '\0';
                            if (e >= 0 && num[e] == '.') num[e] = '\0';
                        }
                    }
                    std::snprintf(out, n, "%s %s", num, materializr::unitSuffix());
                };

                SketchToolMode pm = m_sketchTool->getPreviewType();
                glm::vec2 ps = m_sketchTool->getPreviewStart();
                glm::vec2 pe = m_sketchTool->getPreviewEnd();

                if (pm == SketchToolMode::Line) {
                    float length = glm::length(pe - ps);
                    if (length > 1e-3f) {
                        fmtLen(dbuf, sizeof(dbuf), length);
                        drawDim(sketch2world(ps), sketch2world(pe), dbuf);
                    }
                } else if (pm == SketchToolMode::Circle) {
                    // ps = centre, pe = a point on the rim. Span the full diameter.
                    glm::vec2 rvec = pe - ps;
                    float dia = 2.0f * glm::length(rvec);
                    if (dia > 1e-3f) {
                        fmtLen(dbuf, sizeof(dbuf), dia);
                        { const size_t l = std::strlen(dbuf); std::snprintf(dbuf + l, sizeof(dbuf) - l, " dia"); }
                        drawDim(sketch2world(ps - rvec), sketch2world(pe), dbuf);
                    }
                } else if (pm == SketchToolMode::Rectangle) {
                    // ps, pe = opposite corners. Dimension the bottom and right sides.
                    glm::vec2 bl(ps.x, ps.y), br(pe.x, ps.y), tr(pe.x, pe.y);
                    float w = std::abs(pe.x - ps.x), h = std::abs(pe.y - ps.y);
                    if (w > 1e-3f) {
                        fmtLen(dbuf, sizeof(dbuf), w);
                        drawDim(sketch2world(bl), sketch2world(br), dbuf);
                    }
                    if (h > 1e-3f) {
                        fmtLen(dbuf, sizeof(dbuf), h);
                        drawDim(sketch2world(br), sketch2world(tr), dbuf);
                    }
                } else if (pm == SketchToolMode::Arc) {
                    // Arc is a 3-click tool. While placing the second click
                    // (clicks==1) we just have a chord — show its length.
                    // Once the second click lands (clicks==2) we have three
                    // points on a circle, so we can compute the sweep and
                    // show the user "this is a 90° quarter, this is a 180°
                    // semicircle, this is 270°…" while they're aiming the
                    // third click. Same circumcircle math the renderer uses.
                    int clicks = m_sketchTool->getClickCount();
                    if (clicks == 1) {
                        float length = glm::length(pe - ps);
                        if (length > 1e-3f) {
                            fmtLen(dbuf, sizeof(dbuf), length);
                            drawDim(sketch2world(ps), sketch2world(pe), dbuf);
                        }
                    } else if (clicks == 2) {
                        glm::vec2 second = m_sketchTool->getSecondClick();
                        float ax = ps.x, ay = ps.y;
                        float bx = pe.x, by = pe.y;
                        float cx = second.x, cy = second.y;
                        float D = 2.0f * (ax*(by-cy) + bx*(cy-ay) + cx*(ay-by));
                        if (std::abs(D) > 1e-10f) {
                            float ux = ((ax*ax+ay*ay)*(by-cy) +
                                        (bx*bx+by*by)*(cy-ay) +
                                        (cx*cx+cy*cy)*(ay-by)) / D;
                            float uy = ((ax*ax+ay*ay)*(cx-bx) +
                                        (bx*bx+by*by)*(ax-cx) +
                                        (cx*cx+cy*cy)*(bx-ax)) / D;
                            glm::vec2 ctr(ux, uy);
                            float r = glm::length(ps - ctr);
                            if (r > 1e-3f) {
                                // Arc semantics (matches SketchRenderer):
                                //   ps     = first click  = arc START
                                //   second = second click = arc END
                                //   pe     = cursor       = the through-point
                                //                           that picks which
                                //                           side of the chord
                                //                           the arc bows.
                                // Sweep is start → end; the through-point
                                // only decides direction. (Previously I'd
                                // labelled mA/eA inverted, which flipped the
                                // (dm<ds) branch and reported the long-way
                                // arc — e.g. a 25° sliver as 335°.)
                                float sA = std::atan2(ps.y - ctr.y, ps.x - ctr.x);
                                float eA = std::atan2(second.y - ctr.y,
                                                       second.x - ctr.x);
                                float mA = std::atan2(pe.y - ctr.y, pe.x - ctr.x);
                                auto norm = [](float a){
                                    const float TWO_PI = 2.0f * (float)M_PI;
                                    while (a < 0) a += TWO_PI;
                                    while (a >= TWO_PI) a -= TWO_PI;
                                    return a;
                                };
                                float ds = norm(eA - sA);
                                float dm = norm(mA - sA);
                                float sweep = (dm < ds) ? ds : (ds - 2.0f * (float)M_PI);
                                float deg = std::abs(sweep * 180.0f / (float)M_PI);
                                // Pin label 14 px to the right of the cursor —
                                // a consistent place to look that doesn't
                                // dance around when the inferred circle's
                                // centre/radius shift mid-drag. Tags common
                                // sweeps (¼ / ½ / ¾ / full) so the user can
                                // lock onto canonical values by eye.
                                ImVec2 mp = ImGui::GetMousePos();
                                const char* tag =
                                    (std::abs(deg -  90.0f) < 1.5f) ? "  (¼)"
                                  : (std::abs(deg - 180.0f) < 1.5f) ? "  (½)"
                                  : (std::abs(deg - 270.0f) < 1.5f) ? "  (¾)"
                                  : (std::abs(deg - 360.0f) < 1.5f) ? "  (full)"
                                  : "";
                                std::snprintf(dbuf, sizeof(dbuf),
                                              "%.1f\xC2\xB0%s", deg, tag);
                                ImVec2 ts = ImGui::CalcTextSize(dbuf);
                                ImVec2 tp(mp.x + 14.0f, mp.y - ts.y * 0.5f);
                                dl->AddRectFilled(
                                    ImVec2(tp.x - 5, tp.y - 3),
                                    ImVec2(tp.x + ts.x + 5, tp.y + ts.y + 3),
                                    IM_COL32(20, 20, 28, 225), 3.0f);
                                dl->AddText(tp,
                                            IM_COL32(255, 235, 120, 255), dbuf);
                            }
                        }
                    }
                } else if (pm == SketchToolMode::Polygon) {
                    // ps = centre, pe = a vertex (vertex 0 lands under the
                    // cursor). Show the circumradius (centre→vertex) as a
                    // radius dimension — the polygon analogue of the circle's
                    // diameter readout — tagged with the side count.
                    glm::vec2 rvec = pe - ps;
                    float r = glm::length(rvec);
                    if (r > 1e-3f) {
                        char num[40];
                        fmtLen(num, sizeof(num), r);
                        std::snprintf(dbuf, sizeof(dbuf), "R %s \xC2\xB7 %d-gon",
                                      num, m_sketchTool->getPolygonSides());
                        drawDim(sketch2world(ps), sketch2world(pe), dbuf);
                    }
                }
            }

            // im-touch confirm bubble: a circle or rectangle drawn by press-
            // drag-release is HELD as a preview on lift (see the sketch
            // release handler) — a small floating bar near the shape offers
            // exact-value entry plus ✗/✓. Circle shows one readout + pad
            // (typed diameter wins over the dragged size, matching
            // applyDimension); rectangle shows TWO amount wells (Width /
            // Height, seeded from the drag) and ✓ commits the exact corner
            // computed from them. ✗ drops the shape (neither tool placed
            // geometry on press, so onCancel is a clean reset). Tapping the
            // canvas to draw the next shape auto-commits the held one via
            // the press handler.
            if (m_sketchShapeConfirmPending) {
                const SketchToolMode holdMode =
                    m_sketchTool ? m_sketchTool->getMode() : SketchToolMode::None;
                const bool holdAlive = m_inSketchMode && m_sketchTool &&
                    m_activeSketch &&
                    (holdMode == SketchToolMode::Circle ||
                     holdMode == SketchToolMode::Rectangle) &&
                    m_sketchTool->isPlacing() && m_sketchTool->hasPreview();
                if (!holdAlive) {
                    // Undo / cancel / tool switch dissolved the hold.
                    m_sketchShapeConfirmPending = false;
                } else {
                    // FROZEN endpoints from the lift — not the live preview:
                    // stray motion twitching the held preview must not move
                    // the input box (or flip the commit direction) mid-edit.
                    const glm::vec2 ps = m_sketchShapeAnchorPs;
                    const glm::vec2 pe = m_sketchShapeAnchorPe;
                    const float diaNow = 2.0f * glm::length(pe - ps);
                    {
                        const float sc = uiScale();
                        // Keep the confirm bubble OFF the drawing area (Steve).
                        // Layout-aware: Modern centres it over the right-hand
                        // panel band; im-touch (no persistent panel) and
                        // Classic (docked column already at the right edge) hug
                        // the far-right edge. Vertically centred in all three.
                        const ImGuiViewport* vpb = ImGui::GetMainViewport();
                        const float bmargin = 16.0f * sc;
                        const float rightEdge = vpb->WorkPos.x + vpb->WorkSize.x;
                        const float cy = vpb->WorkPos.y + vpb->WorkSize.y * 0.5f;
                        float anchorX = rightEdge - bmargin;
                        float pivotX  = 1.0f;
                        if (m_uiLayout == UiLayout::Modern && m_touchVpW > 0.0f) {
                            // Modern: park it at the RIGHT SIDE OF THE VIEWPORT
                            // (just inside the right panel's edge), not over the
                            // panel (Steve). m_touchVpX + m_touchVpW is the
                            // viewport's right edge; the bubble's right edge sits
                            // there.
                            anchorX = (m_touchVpX + m_touchVpW) - bmargin;
                            pivotX  = 1.0f;
                        }
                        ImGui::SetNextWindowPos(ImVec2(anchorX, cy),
                                                ImGuiCond_Always,
                                                ImVec2(pivotX, 0.5f));
                        ImGui::SetNextWindowBgAlpha(0.95f);
                        ImGui::Begin("##CircleConfirm", nullptr,
                            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_NoFocusOnAppearing);
                        bool commit = false;
                        const float keySide = 46.0f * sc;
                        if (holdMode == SketchToolMode::Circle) {
                            // Native keyboard (tap the field) instead of the
                            // in-app number pad — the im-touch bubble now
                            // matches Modern. The placeholder shows the dragged
                            // diameter until you type an exact value; empty
                            // buffer = keep the drag (handled at commit).
                            ImGui::TextDisabled("%s", materializr::trFormat("Diameter (%s)", materializr::unitSuffix()).c_str());
                            char hint[32];
                            std::snprintf(hint, sizeof(hint), "%.1f (drag)", diaNow);
                            ImGui::SetNextItemWidth(touchui::numberPadWidth(keySide));
                            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                                ImVec2(uiW(12.0f), uiW(12.0f)));
                            // The pad, with the empty-is-meaningful hint:
                            // nothing typed = keep the dragged diameter (the
                            // ✓ below parses the buffer; empty buffer means
                            // "keep"). A commit writes the buffer back so
                            // that contract is untouched.
                            double diaPadV = 0.0;
                            (void)materializr::parseFinite(m_sketchShapeDimBuf,
                                                           diaPadV);
                            if (touchui::numberField("##bubbleDia", nullptr,
                                                     &diaPadV, materializr::lengthFormat(),
                                                     nullptr, hint)) {
                                if (diaPadV > 0.0)
                                    std::snprintf(m_sketchShapeDimBuf,
                                                  sizeof(m_sketchShapeDimBuf),
                                                  "%.6g", diaPadV);
                                else
                                    m_sketchShapeDimBuf[0] = '\0';
                            }
                            ImGui::PopStyleVar();
                        } else {
                            // Rectangle: two native Width/Height fields (tap to
                            // raise the keyboard) — matches Modern and the
                            // circle branch above. Seeded from the drag; ✓
                            // places the rectangle at the shown W x H.
                            const float fieldW = touchui::numberPadWidth(keySide);
                            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                                ImVec2(uiW(10.0f), uiW(10.0f)));
                            ImGui::TextDisabled("%s", materializr::trFormat("Width (%s)", materializr::unitSuffix()).c_str());
                            ImGui::SetNextItemWidth(fieldW);
                            materializr::lengthField("##bubbleW", &m_sketchShapeDimW);
                            ImGui::TextDisabled("%s", materializr::trFormat("Height (%s)", materializr::unitSuffix()).c_str());
                            ImGui::SetNextItemWidth(fieldW);
                            materializr::lengthField("##bubbleH", &m_sketchShapeDimH);
                            ImGui::PopStyleVar();
                            if (m_sketchShapeDimW < 0.01f) m_sketchShapeDimW = 0.01f;
                            if (m_sketchShapeDimH < 0.01f) m_sketchShapeDimH = 0.01f;
                        }
                        ImGui::Spacing();
                        if (touchui::iconButton("bubbleDrop", MZ_ICON_DISCARD,
                                                46.0f * sc)) {
                            m_sketchTool->onCancel();
                            m_sketchShapeConfirmPending = false;
                        }
                        ImGui::SameLine(0.0f, 12.0f * sc);
                        if (touchui::pillButton("bubbleOk", MZ_ICON_FINISH,
                                                nullptr, /*accent=*/true))
                            commit = true;
                        if (commit && m_sketchShapeConfirmPending) {
                            if (holdMode == SketchToolMode::Circle) {
                                float v = 0.0f;
                                const bool useTyped = [&] {
                                    double mm = 0.0;   // "2in" honoured; bare = display unit
                                    if (!materializr::parseLength(m_sketchShapeDimBuf, mm) || mm <= 0.0) return false;
                                    v = static_cast<float>(mm);
                                    return true;
                                }();
                                recordSketchMutation([&] {
                                    if (useTyped)
                                        m_sketchTool->applyDimension(v);
                                    else
                                        m_sketchTool->onMouseDown(
                                            m_sketchShapePendingPos, false);
                                });
                            } else {
                                // Commit the exact W×H through the second-
                                // corner tap the drag would have made. ps is
                                // the EFFECTIVE opposite corner for both draw
                                // modes; Center mode sizes from the centre, so
                                // the target sits half a side away from it.
                                const glm::vec2 dir(
                                    pe.x >= ps.x ? 1.0f : -1.0f,
                                    pe.y >= ps.y ? 1.0f : -1.0f);
                                glm::vec2 target;
                                if (m_sketchTool->getRectMode() ==
                                    SketchTool::RectMode::Center) {
                                    const glm::vec2 ctr = 0.5f * (ps + pe);
                                    target = ctr +
                                        glm::vec2(dir.x * m_sketchShapeDimW * 0.5f,
                                                  dir.y * m_sketchShapeDimH * 0.5f);
                                } else {
                                    target = ps +
                                        glm::vec2(dir.x * m_sketchShapeDimW,
                                                  dir.y * m_sketchShapeDimH);
                                }
                                recordSketchMutation([&] {
                                    m_sketchTool->onMouseDown(target, false);
                                });
                            }
                            m_sketchShapeConfirmPending = false;
                        }
                        ImGui::End();
                    }
                }
            }

            // Inference guides — the SketchUp-style "purple line down from
            // point 1" ghost markers. Drawn during placement / hover so the
            // user can see WHY the cursor is being snapped before they click.
            // Pure visual cue: nothing in the placed geometry remembers them.
            // Touch Move (nav-lock) mode isn't drawing/selecting, so the snap
            // guides are just visual noise here — same reasoning as the Select
            // tool suppressing them in SketchTool::onMouseMove.
            if (m_inSketchMode && m_activeSketch && m_sketchTool && !m_moveModeToggle) {
                const gp_Ax3& iax = m_activeSketch->getPlane().Position();
                glm::vec3 iO(iax.Location().X(), iax.Location().Y(), iax.Location().Z());
                glm::vec3 iX(iax.XDirection().X(), iax.XDirection().Y(), iax.XDirection().Z());
                glm::vec3 iY(iax.YDirection().X(), iax.YDirection().Y(), iax.YDirection().Z());
                auto sk2w = [&](glm::vec2 p) { return iO + p.x * iX + p.y * iY; };

                // Dark halo drawn under every coloured marker / guide so the
                // inferences read clearly against the busy sketch background
                // (light-blue host face, blue sketch lines, dark grid). Without
                // this the mid-saturation guides are nearly invisible.
                const ImU32 halo = IM_COL32(0, 0, 0, 220);
                // All inferences share a single colour now — the markers'
                // distinct shapes (square / triangle / diamond) plus the
                // cursor's text label carry the meaning; varied colours just
                // added visual noise without communicating anything useful.
                const ImU32 inferenceCol = IM_COL32(140, 220, 255, 255); // bright cyan
                for (const auto& g : m_sketchTool->getActiveInferences()) {
                    ImU32 col = inferenceCol;
                    bool dashed = false;
                    switch (g.kind) {
                        case InferenceGuide::Endpoint:
                        case InferenceGuide::Midpoint:
                        case InferenceGuide::OnLine:
                        case InferenceGuide::OnCircle:
                            // Markers: shape carries the meaning.
                            break;
                        case InferenceGuide::AxisHFromPoint:
                        case InferenceGuide::AxisVFromPoint:
                        case InferenceGuide::PerpToPrev:
                        case InferenceGuide::ParallelToPrev:
                        case InferenceGuide::AngleSnap:
                        case InferenceGuide::OnLineExtension:
                        case InferenceGuide::TangentToCircle:
                        case InferenceGuide::CornerBisector:
                        case InferenceGuide::CornerTangent:
                            dashed = true;
                            break;
                        case InferenceGuide::PerpToRef:
                            // Hover-charged perpendicular ray — cyan to stand
                            // apart from the chain-relative guides.
                            dashed = true;
                            col = IM_COL32(80, 220, 235, 255);
                            break;
                        case InferenceGuide::Symmetry:
                            // Mirror pairing (source ↔ snapped point) — purple.
                            dashed = true;
                            col = IM_COL32(200, 100, 255, 255);
                            break;
                    }
                    ImVec2 sa, sb;
                    if (!toImg(sk2w(g.from), sa)) continue;
                    if (!toImg(sk2w(g.to), sb))   continue;
                    if (g.kind == InferenceGuide::Endpoint) {
                        // Square marker AT the anchor point, with dark halo.
                        const float r = 5.0f;
                        dl->AddRect(ImVec2(sa.x - r - 1, sa.y - r - 1),
                                    ImVec2(sa.x + r + 1, sa.y + r + 1),
                                    halo, 0.0f, 0, 4.0f);
                        dl->AddRect(ImVec2(sa.x - r, sa.y - r),
                                    ImVec2(sa.x + r, sa.y + r), col, 0.0f, 0, 2.5f);
                    } else if (g.kind == InferenceGuide::Midpoint) {
                        // Triangle marker with halo.
                        const float r = 5.5f;
                        ImVec2 t0(sa.x,         sa.y - r);
                        ImVec2 t1(sa.x - r,     sa.y + r * 0.6f);
                        ImVec2 t2(sa.x + r,     sa.y + r * 0.6f);
                        dl->AddTriangle(t0, t1, t2, halo, 4.0f);
                        dl->AddTriangle(t0, t1, t2, col, 2.5f);
                    } else if (g.kind == InferenceGuide::OnLine ||
                               g.kind == InferenceGuide::OnCircle) {
                        // Diamond marker with halo. A rim landing gets the same
                        // shape as an on-edge landing because it means the same
                        // thing to the user — the point is ON that geometry.
                        const float r = 5.0f;
                        ImVec2 d0(sa.x,     sa.y - r);
                        ImVec2 d1(sa.x + r, sa.y);
                        ImVec2 d2(sa.x,     sa.y + r);
                        ImVec2 d3(sa.x - r, sa.y);
                        dl->AddQuad(d0, d1, d2, d3, halo, 4.0f);
                        dl->AddQuad(d0, d1, d2, d3, col, 2.2f);
                    } else if (dashed) {
                        // Dashed guide with a dark halo behind each dash.
                        ImVec2 d(sb.x - sa.x, sb.y - sa.y);
                        float len = std::sqrt(d.x * d.x + d.y * d.y);
                        if (len < 1.0f) continue;
                        d.x /= len; d.y /= len;
                        // Extend the guide past the ANCHOR end only so it
                        // still reads as an axis — extending past the cursor
                        // end buried the endpoint marker under dashes ("I
                        // cannot see where the vertex actually is").
                        const float extend = 60.0f;
                        ImVec2 a0(sa.x - d.x * extend, sa.y - d.y * extend);
                        float total = len + extend;
                        const float dashOn = 11.0f, dashOff = 7.0f;
                        for (float t = 0.0f; t < total; t += dashOn + dashOff) {
                            float t1 = std::min(t + dashOn, total);
                            ImVec2 p0(a0.x + d.x * t,  a0.y + d.y * t);
                            ImVec2 p1(a0.x + d.x * t1, a0.y + d.y * t1);
                            dl->AddLine(p0, p1, halo, 5.0f);
                            dl->AddLine(p0, p1, col,  2.5f);
                        }
                        // Marker at the reference point so the user can see
                        // which existing feature the guide is sourced from.
                        if (g.kind == InferenceGuide::AxisHFromPoint ||
                            g.kind == InferenceGuide::AxisVFromPoint ||
                            g.kind == InferenceGuide::PerpToRef ||
                            g.kind == InferenceGuide::CornerBisector ||
                            g.kind == InferenceGuide::CornerTangent) {
                            dl->AddCircleFilled(sa, 5.5f, halo);
                            dl->AddCircleFilled(sa, 4.0f, col);
                        }
                        // Symmetry: mark BOTH the source (sa) and the mirrored
                        // snap target (sb) so the pairing is obvious.
                        if (g.kind == InferenceGuide::Symmetry) {
                            dl->AddCircleFilled(sa, 5.0f, halo);
                            dl->AddCircleFilled(sa, 3.5f, col);
                            dl->AddCircleFilled(sb, 5.5f, halo);
                            dl->AddCircleFilled(sb, 4.0f, col);
                        }
                    }
                }

                // Hover-charged reference: a cyan ring on the dwelt-on
                // anchor (sketch point, sketch line midpoint, face vertex,
                // or face-edge midpoint) so it's clear which feature is
                // sourcing the guides. Anchor position is read from the
                // tool directly — the kinds without a sketch-element id
                // (face refs) wouldn't survive a getPoint() lookup. Gated
                // on m_inSketchMode above so face features only highlight
                // during a sketch.
                if (m_sketchTool->hasChargedRef()) {
                    ImVec2 cs;
                    if (toImg(sk2w(m_sketchTool->getChargedPos()), cs)) {
                        const ImU32 ring = IM_COL32(80, 220, 235, 235);
                        dl->AddCircle(cs, 9.0f, halo, 0, 4.0f);
                        dl->AddCircle(cs, 9.0f, ring, 0, 2.0f);
                        dl->AddCircleFilled(cs, 2.5f, ring);
                    }
                }

                // Text / SVG placement: dashed rectangle following the
                // cursor — the measured (rotated) extents of the artwork,
                // so the user sees exactly where it will land before
                // clicking. Baseline drawn solid for orientation.
                if ((m_sketchTool->getMode() == SketchToolMode::Text ||
                     m_sketchTool->getMode() == SketchToolMode::Svg ||
                     m_sketchTool->getMode() == SketchToolMode::Airfoil) &&
                    m_sketchTool->hasTextPreviewBox()) {
                    const glm::vec2 anchor = m_sketchTool->getCurrentPos();
                    const glm::vec2 mn = m_sketchTool->getTextPreviewMin();
                    const glm::vec2 mx = m_sketchTool->getTextPreviewMax();
                    // Anchor the box where the STAMP actually anchors, or the
                    // preview lies about where the geometry will land. Text and
                    // SVG stamp about their bounding-box centre; an airfoil
                    // stamps its LEADING EDGE, which is box-space (0,0) -- the
                    // box was drawn centred for it and the profile then landed
                    // half a chord away from where it was shown.
                    const glm::vec2 center =
                        (m_sketchTool->getMode() == SketchToolMode::Airfoil)
                            ? glm::vec2(0.0f, 0.0f)
                            : (mn + mx) * 0.5f;
                    const float a = glm::radians(
                        static_cast<float>(m_sketchTool->getTextAngle()));
                    const float ca = std::cos(a), sa2 = std::sin(a);
                    auto rot = [&](glm::vec2 p) {
                        p -= center;
                        return anchor + glm::vec2(p.x * ca - p.y * sa2,
                                                  p.x * sa2 + p.y * ca);
                    };
                    const glm::vec2 corners[4] = {
                        rot({mn.x, mn.y}), rot({mx.x, mn.y}),
                        rot({mx.x, mx.y}), rot({mn.x, mx.y})};
                    ImVec2 sc[4];
                    bool vis = true;
                    for (int i = 0; i < 4 && vis; ++i)
                        vis = toImg(sk2w(corners[i]), sc[i]);
                    if (vis) {
                        const ImU32 boxCol = IM_COL32(140, 220, 255, 200);
                        auto dashSeg = [&](ImVec2 p0, ImVec2 p1) {
                            ImVec2 d(p1.x - p0.x, p1.y - p0.y);
                            float len = std::sqrt(d.x * d.x + d.y * d.y);
                            if (len < 1.0f) return;
                            d.x /= len; d.y /= len;
                            const float on = 8.0f, off = 6.0f;
                            for (float t = 0.0f; t < len; t += on + off) {
                                float t1 = std::min(t + on, len);
                                dl->AddLine(
                                    ImVec2(p0.x + d.x * t, p0.y + d.y * t),
                                    ImVec2(p0.x + d.x * t1,
                                           p0.y + d.y * t1),
                                    boxCol, 1.5f);
                            }
                        };
                        for (int i = 0; i < 4; ++i)
                            dashSeg(sc[i], sc[(i + 1) % 4]);
                        // Solid baseline (y=0 of the glyphs) — shows which
                        // way is "up" for the letters at a glance.
                        ImVec2 b0, b1;
                        if (toImg(sk2w(rot({mn.x, 0.0f})), b0) &&
                            toImg(sk2w(rot({mx.x, 0.0f})), b1))
                            dl->AddLine(b0, b1, boxCol, 2.0f);
                        // Live glyph preview: the actual letter contours, so the
                        // user sees WHAT will land, not just where. (Text only —
                        // empty for SVG, which keeps the box.)
                        const auto& gloops = m_sketchTool->getTextPreviewLoops();
                        const ImU32 glyphCol = IM_COL32(150, 230, 255, 235);
                        for (const auto& gl : gloops) {
                            const size_t n = gl.size();
                            for (size_t i = 0; i < n; ++i) {
                                ImVec2 pa, pb;
                                if (toImg(sk2w(rot(gl[i])), pa) &&
                                    toImg(sk2w(rot(gl[(i + 1) % n])), pb))
                                    dl->AddLine(pa, pb, glyphCol, 1.3f);
                            }
                        }
                    }
                }

                // Interactive Mirror: the dashed mirror line + a live ghost of the
                // reflected geometry. The move/rotate GIZMO itself is drawn in the
                // input-handling scope (so its hit-test and visual stay in sync).
                if (m_sketchTool->getMode() == SketchToolMode::Mirror &&
                    m_sketchTool->isMirrorActive()) {
                    const glm::vec2 anchor = m_sketchTool->getMirrorAnchor();
                    const float ang = m_sketchTool->getMirrorAngle();
                    const glm::vec2 dir(std::cos(ang), std::sin(ang));
                    ImVec2 aS, dProbe;
                    if (toImg(sk2w(anchor), aS) &&
                        toImg(sk2w(anchor + dir * 0.01f), dProbe)) {
                        ImVec2 dS(dProbe.x - aS.x, dProbe.y - aS.y);
                        float dl2 = std::sqrt(dS.x * dS.x + dS.y * dS.y);
                        if (dl2 > 1e-4f) { dS.x /= dl2; dS.y /= dl2; }
                        else { dS = ImVec2(0.0f, 1.0f); }
                        const ImU32 lineCol = IM_COL32(120, 200, 255, 220);
                        // Long dashed line through the anchor.
                        const float L = 4000.0f;
                        ImVec2 p0(aS.x - dS.x * L, aS.y - dS.y * L);
                        ImVec2 p1(aS.x + dS.x * L, aS.y + dS.y * L);
                        {
                            ImVec2 d(p1.x - p0.x, p1.y - p0.y);
                            float len = std::sqrt(d.x * d.x + d.y * d.y);
                            if (len > 1.0f) {
                                d.x /= len; d.y /= len;
                                const float on = 10.0f, off = 8.0f;
                                for (float t = 0.0f; t < len; t += on + off) {
                                    float t1 = std::min(t + on, len);
                                    dl->AddLine(ImVec2(p0.x + d.x * t, p0.y + d.y * t),
                                                ImVec2(p0.x + d.x * t1, p0.y + d.y * t1),
                                                lineCol, 1.6f);
                                }
                            }
                        }
                        // Live ghost of the reflected geometry.
                        std::vector<std::vector<glm::vec2>> polys;
                        std::vector<glm::vec2> pts;
                        m_sketchTool->getMirrorPreview(polys, pts);
                        const ImU32 ghost = IM_COL32(255, 170, 60, 230);
                        for (const auto& poly : polys)
                            for (size_t i = 0; i + 1 < poly.size(); ++i) {
                                ImVec2 a, b;
                                if (toImg(sk2w(poly[i]), a) && toImg(sk2w(poly[i + 1]), b))
                                    dl->AddLine(a, b, ghost, 1.8f);
                            }
                        for (const auto& q : pts) {
                            ImVec2 s;
                            if (toImg(sk2w(q), s)) dl->AddCircleFilled(s, 3.0f, ghost);
                        }

                        // Move/rotate gizmo on the anchor (drawn here, in the
                        // always-run pass, so it's visible WITHOUT a canvas tap;
                        // hit-test + drag live in the input scope). Geometry/
                        // constants mirror that block so the drawn handles line up
                        // with their hit zones.
                        ImVec2 gsx, gsy;
                        if (toImg(sk2w(anchor + glm::vec2(1.0f, 0.0f)), gsx) &&
                            toImg(sk2w(anchor + glm::vec2(0.0f, 1.0f)), gsy)) {
                            glm::vec2 gvx(gsx.x - aS.x, gsx.y - aS.y);
                            glm::vec2 gvy(gsy.x - aS.x, gsy.y - aS.y);
                            if (glm::length(gvx) > 1e-3f && glm::length(gvy) > 1e-3f) {
                                gvx = glm::normalize(gvx); gvy = glm::normalize(gvy);
                                const bool gt = materializr::touchMode();
                                const float gArm = gt ? 90.0f : 70.0f;
                                const float gRing = gt ? 130.0f : 100.0f;
                                const float gCtr = gt ? 11.0f : 6.5f;
                                ImVec2 gex(aS.x + gvx.x * gArm, aS.y + gvx.y * gArm);
                                ImVec2 gey(aS.x + gvy.x * gArm, aS.y + gvy.y * gArm);
                                auto gcol = [&](ImU32 base, SketchGizmoHandle h) {
                                    return (m_mirrorGizmoHandle == h)
                                               ? IM_COL32(255, 240, 80, 255) : base;
                                };
                                dl->AddCircle(aS, gRing, gcol(IM_COL32(220, 180, 60, 220),
                                              SketchGizmoHandle::Rotate), 64, 2.5f);
                                dl->AddLine(aS, gex, gcol(IM_COL32(230, 70, 70, 230),
                                            SketchGizmoHandle::MoveX), 3.0f);
                                dl->AddLine(aS, gey, gcol(IM_COL32(90, 200, 90, 230),
                                            SketchGizmoHandle::MoveY), 3.0f);
                                auto ghead = [&](ImVec2 tip, glm::vec2 d, ImU32 cc) {
                                    glm::vec2 pp(-d.y, d.x);
                                    dl->AddTriangleFilled(tip,
                                        ImVec2(tip.x - d.x * 14.0f + pp.x * 6.0f, tip.y - d.y * 14.0f + pp.y * 6.0f),
                                        ImVec2(tip.x - d.x * 14.0f - pp.x * 6.0f, tip.y - d.y * 14.0f - pp.y * 6.0f),
                                        cc);
                                };
                                ghead(gex, gvx, gcol(IM_COL32(230, 70, 70, 230), SketchGizmoHandle::MoveX));
                                ghead(gey, gvy, gcol(IM_COL32(90, 200, 90, 230), SketchGizmoHandle::MoveY));
                                dl->AddCircleFilled(aS, gCtr, gcol(IM_COL32(240, 240, 230, 230),
                                                    SketchGizmoHandle::MoveFree));
                                dl->AddCircle(aS, gCtr, IM_COL32(30, 30, 40, 230), 0, 1.2f);
                            }
                        }
                    }
                }

                // Element move gizmo, drawn here in the always-run pass so a
                // selection armed by a toolbar action (Copy) shows the gizmo
                // without a canvas tap. Hit-test + drag stay in the input scope.
                // Skipped while a gizmo drag is in flight (the input scope then
                // owns the visual, anchored at the drag-start centroid).
                if (m_sketchTool->getMode() == SketchToolMode::Select &&
                    m_sketchTool->hasElementSelection() &&
                    m_sketchGizmoHandle == SketchGizmoHandle::None) {
                    std::set<int> inv(m_sketchTool->getSelectedPoints().begin(),
                                      m_sketchTool->getSelectedPoints().end());
                    for (int lid : m_sketchTool->getSelectedLines())
                        for (const auto& l : m_activeSketch->getLines())
                            if (l.id == lid) { inv.insert(l.startPointId); inv.insert(l.endPointId); break; }
                    for (int cid : m_sketchTool->getSelectedCircles())
                        for (const auto& cc : m_activeSketch->getCircles())
                            if (cc.id == cid) { inv.insert(cc.centerPointId); break; }
                    for (int aid : m_sketchTool->getSelectedArcs())
                        for (const auto& aa : m_activeSketch->getArcs())
                            if (aa.id == aid) { inv.insert(aa.centerPointId); inv.insert(aa.startPointId); inv.insert(aa.endPointId); break; }
                    for (int sid : m_sketchTool->getSelectedSplines())
                        for (const auto& sp : m_activeSketch->getSplines())
                            if (sp.id == sid) { for (int cp : sp.controlPointIds) inv.insert(cp); break; }
                    glm::vec2 gc{0.0f}; int gn = 0;
                    for (int id : inv) if (auto* p = m_activeSketch->getPoint(id)) { gc += p->pos; ++gn; }
                    ImVec2 ec, egx, egy;
                    if (gn > 0 &&
                        toImg(sk2w(gc / static_cast<float>(gn)), ec) &&
                        toImg(sk2w(gc / static_cast<float>(gn) + glm::vec2(1.0f, 0.0f)), egx) &&
                        toImg(sk2w(gc / static_cast<float>(gn) + glm::vec2(0.0f, 1.0f)), egy)) {
                        glm::vec2 evx(egx.x - ec.x, egx.y - ec.y);
                        glm::vec2 evy(egy.x - ec.x, egy.y - ec.y);
                        if (glm::length(evx) > 1e-3f && glm::length(evy) > 1e-3f) {
                            evx = glm::normalize(evx); evy = glm::normalize(evy);
                            const bool et = materializr::touchMode();
                            const float eArm = et ? 90.0f : 70.0f;
                            const float eRing = et ? 130.0f : 100.0f;
                            const float eCtr = et ? 11.0f : 6.5f;
                            ImVec2 eex(ec.x + evx.x * eArm, ec.y + evx.y * eArm);
                            ImVec2 eey(ec.x + evy.x * eArm, ec.y + evy.y * eArm);
                            dl->AddCircle(ec, eRing, IM_COL32(220, 180, 60, 220), 64, 2.5f);
                            dl->AddLine(ec, eex, IM_COL32(230, 70, 70, 230), 3.0f);
                            dl->AddLine(ec, eey, IM_COL32(90, 200, 90, 230), 3.0f);
                            auto ehead = [&](ImVec2 tip, glm::vec2 d, ImU32 cc) {
                                glm::vec2 pp(-d.y, d.x);
                                dl->AddTriangleFilled(tip,
                                    ImVec2(tip.x - d.x * 14.0f + pp.x * 6.0f, tip.y - d.y * 14.0f + pp.y * 6.0f),
                                    ImVec2(tip.x - d.x * 14.0f - pp.x * 6.0f, tip.y - d.y * 14.0f - pp.y * 6.0f), cc);
                            };
                            ehead(eex, evx, IM_COL32(230, 70, 70, 230));
                            ehead(eey, evy, IM_COL32(90, 200, 90, 230));
                            dl->AddCircleFilled(ec, eCtr, IM_COL32(240, 240, 230, 230));
                            dl->AddCircle(ec, eCtr, IM_COL32(30, 30, 40, 230), 0, 1.2f);
                        }
                    }
                }

                // Inference label near the cursor — names which snap(s) are
                // active so the user understands WHY the cursor jumped. The
                // killer SketchUp feature ("Endpoint" / "On midpoint" /
                // "Perpendicular" floating next to the crosshair).
                if (!m_sketchTool->getActiveInferences().empty()) {
                    // The angle-snap increment is a setting, so the label has to
                    // read it rather than claim 15 degrees.
                    char angleLbl[32];
                    std::snprintf(angleLbl, sizeof(angleLbl), "Angle Snap (%d\xc2\xb0)",
                                  m_sketchTool->getAngleSnapDeg());
                    auto kindName = [&](InferenceGuide::Kind k) -> const char* {
                        switch (k) {
                            case InferenceGuide::Endpoint:       return "Endpoint";
                            case InferenceGuide::Midpoint:       return "Midpoint";
                            case InferenceGuide::OnLine:         return "On Line";
                            case InferenceGuide::OnCircle:       return "On Circle";
                            case InferenceGuide::AxisHFromPoint: return "On Horizontal Axis";
                            case InferenceGuide::AxisVFromPoint: return "On Vertical Axis";
                            case InferenceGuide::PerpToPrev:     return "Perpendicular";
                            case InferenceGuide::ParallelToPrev: return "Parallel";
                            case InferenceGuide::AngleSnap:      return angleLbl;
                            case InferenceGuide::OnLineExtension: return "On Line Extension";
                            case InferenceGuide::TangentToCircle: return "Tangent";
                            case InferenceGuide::PerpToRef:       return "Perpendicular from Point";
                            case InferenceGuide::Symmetry:        return "Symmetry";
                            case InferenceGuide::CornerBisector:  return "Corner Bisector";
                            case InferenceGuide::CornerTangent:   return "Corner Tangent";
                        }
                        return "";
                    };
                    // Concatenate distinct names so two simultaneous inferences
                    // (the intersection case) read as "Perpendicular + On Vertical Axis".
                    std::string label;
                    for (const auto& g : m_sketchTool->getActiveInferences()) {
                        const char* n = kindName(g.kind);
                        if (!n || !*n) continue;
                        if (!label.empty()) label += "  +  ";
                        // Skip duplicates within the same set.
                        if (label.find(n) == std::string::npos) label += n;
                    }
                    if (!label.empty()) {
                        ImVec2 mp = ImGui::GetMousePos();
                        // One row LOWER than the dimension readout (which
                        // pins ~14 px right of the cursor) — on short lines
                        // the two used to stack on the same spot and the
                        // guide name sat right on top of the measurement.
                        ImVec2 tp(mp.x + 16.0f, mp.y + 36.0f);
                        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
                        // Dark background pill so the text stays legible no
                        // matter what's underneath in the viewport.
                        dl->AddRectFilled(
                            ImVec2(tp.x - 5.0f, tp.y - 3.0f),
                            ImVec2(tp.x + ts.x + 5.0f, tp.y + ts.y + 3.0f),
                            IM_COL32(20, 20, 28, 220), 4.0f);
                        dl->AddText(tp, IM_COL32(255, 240, 200, 255), label.c_str());
                    }
                }
            }

            // Dimension value labels — Distance / Radius / Angle constraints
            // get a numeric text overlay near their geometry so the user can
            // see (and later edit) the locked value. Free / undimensioned
            // geometry stays label-free.
            if (m_inSketchMode && m_activeSketch) {
                const gp_Ax3& dax = m_activeSketch->getPlane().Position();
                glm::vec3 dO(dax.Location().X(), dax.Location().Y(), dax.Location().Z());
                glm::vec3 dX(dax.XDirection().X(), dax.XDirection().Y(), dax.XDirection().Z());
                glm::vec3 dY(dax.YDirection().X(), dax.YDirection().Y(), dax.YDirection().Z());
                auto dim2world = [&](glm::vec2 p) { return dO + p.x * dX + p.y * dY; };
                // Reset the per-frame "click swallowed by a label" flag —
                // re-evaluated below as labels are drawn and hit-tested.
                m_dimEditingClickedThisFrame = false;
                // The Dimension tool's commit path (applyPendingDimension, in
                // Application.cpp) sets m_dimEditingId + this flag from outside
                // any ImGui window scope. OpenPopup only takes effect when
                // called from the window that owns the popup's ID stack, so
                // defer it to here — same scope as the label-click OpenPopup
                // calls below.
                if (m_dimOpenEditRequested) {
                    ImGui::OpenPopup("##DimEdit");
                    m_dimOpenEditRequested = false;
                }
                // Cursor in sketch mm — the space labelOffX/Y lives in, so a
                // drag can be expressed as an offset directly.
                const ImVec2 dimMp = ImGui::GetMousePos();
                const glm::vec2 dimCursor = screenToSketch(
                    dimMp.x - imgMin.x, dimMp.y - imgMin.y, imgSize.x, imgSize.y);
                // Opens the value editor for a constraint. Called on RELEASE
                // of a press that did not turn into a drag — see the label
                // press/drag handling below.
                auto openDimEdit = [&](const Constraint& c) {
                    m_dimEditingId = c.id;
                    // Seed what the label shows: degrees for an angle; the
                    // DIAMETER for a circle's Radius constraint (Ø), the
                    // radius for an arc's (R); lengths in the display unit.
                    const auto kind = c.type == ConstraintType::Angle  ? materializr::DimKind::Angle
                                    : c.type == ConstraintType::Radius ? materializr::DimKind::Radius
                                                                       : materializr::DimKind::Length;
                    materializr::seedDimensionText(m_dimEditingBuf, sizeof(m_dimEditingBuf), kind,
                        kind == materializr::DimKind::Radius && materializr::constraintIsArcRadius(*m_activeSketch, c),
                        c.value);
                    m_dimEditingFocus = true;
                    m_dimEditingClickedThisFrame = true;
                    ImGui::OpenPopup("##DimEdit");
                };
                auto drawLabel = [&](glm::vec2 pos, const char* text,
                                     const Constraint& c, glm::vec2 anchor) {
                    ImVec2 sp;
                    if (!toImg(dim2world(pos), sp)) return;
                    ImVec2 ts = ImGui::CalcTextSize(text);
                    ImVec2 tp(sp.x - ts.x * 0.5f, sp.y - ts.y * 0.5f);
                    ImVec2 rMin(tp.x - 4, tp.y - 2);
                    ImVec2 rMax(tp.x + ts.x + 4, tp.y + ts.y + 2);
                    // Hover highlights the label so users see it's clickable.
                    bool hovered = ImGui::IsMouseHoveringRect(rMin, rMax);
                    ImU32 bg = hovered ? IM_COL32(45, 45, 60, 235)
                                       : IM_COL32(20, 20, 28, 220);
                    dl->AddRectFilled(rMin, rMax, bg, 3.0f);
                    // Driving dimensions in the usual amber; reference ones in
                    // a muted grey so a glance separates "this number controls
                    // the shape" from "this number just reports it".
                    dl->AddText(tp, c.isDriving ? IM_COL32(255, 235, 120, 255)
                                                : IM_COL32(170, 178, 190, 255), text);
                    // A press LATCHES the label for dragging. The edit popup
                    // is deferred to release (below, after the loop) and only
                    // fires if the pointer stayed put — otherwise the tag can
                    // never be repositioned, because every attempt to move it
                    // opens the value editor instead.
                    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                        m_dimDragId < 0 && m_dimEditingId != c.id) {
                        m_dimDragId    = c.id;
                        m_dimDragMoved = false;
                        m_dimDragGrab  = pos - dimCursor;
                    }
                    // Live drag: rewrite this constraint's stored offset so the
                    // label tracks the cursor. Offsets are relative to the
                    // type's geometric anchor, so the tag keeps its place when
                    // the solver later moves the geometry under it.
                    if (m_dimDragId == c.id &&
                        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        // Threshold FIRST, and write nothing until it is
                        // crossed. IsMouseDown is already true on the press
                        // frame, so writing unconditionally would store an
                        // offset for a plain click — and since `want` equals
                        // the label's current position, that offset is the
                        // AUTO one. The label would silently become
                        // user-placed: it grows a leader line and stops
                        // tracking automatic positioning (Distance's auto
                        // offset scales with segment length, so a frozen one
                        // no longer rescales). Clicking to edit must leave the
                        // stored offset exactly as it found it.
                        const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                        if (materializr::dimDragExceedsThreshold(d.x, d.y))
                            m_dimDragMoved = true;
                        if (m_dimDragMoved) {
                            const glm::vec2 want = dimCursor + m_dimDragGrab;
                            double offX = 0.0, offY = 0.0;
                            materializr::dimLabelOffset(want.x, want.y,
                                                        anchor.x, anchor.y,
                                                        offX, offY);
                            for (auto& mc : m_activeSketch->getMutableConstraints()) {
                                if (mc.id != c.id) continue;
                                mc.labelOffX = offX;
                                mc.labelOffY = offY;
                                break;
                            }
                        }
                        m_dimEditingClickedThisFrame = true;  // don't also pick
                    }
                };
                // Resolves a constraint's label position from its stored
                // offset (Constraint::labelOffX/Y, relative to the type's
                // geometric anchor — see dimensionAutoAnchor) or the auto
                // position when unset (legacy / not yet user-placed). Draws a
                // thin leader from the anchor to the label whenever the user
                // has moved it off the auto spot.
                // `leaderFrom` overrides where a placed label's leader starts
                // (default: the anchor). DistancePointLine passes its dim
                // line's midpoint — the offset math must stay anchored on
                // dimensionAutoAnchor (what applyPendingDimension stored
                // against), but a leader drawn from that anchor points at the
                // constraint's pinned corner, not the dimension line.
                auto placeLabel = [&](glm::vec2 anchor, glm::vec2 autoOff,
                                      const char* text, const Constraint& c,
                                      const glm::vec2* leaderFrom = nullptr) {
                    bool placed = (c.labelOffX != 0.0 || c.labelOffY != 0.0);
                    glm::vec2 lpos = placed
                        ? anchor + glm::vec2(static_cast<float>(c.labelOffX),
                                             static_cast<float>(c.labelOffY))
                        : anchor + autoOff;
                    if (placed) {
                        glm::vec2 lsrc = leaderFrom ? *leaderFrom : anchor;
                        ImVec2 sa, sb;
                        if (toImg(dim2world(lsrc), sa) && toImg(dim2world(lpos), sb))
                            dl->AddLine(sa, sb, IM_COL32(255, 235, 120, 90), 1.0f);
                    }
                    // Reference (non-driving) dimensions are bracketed, the
                    // standard drafting notation for a measurement that does
                    // not control the geometry. One place, so every dimension
                    // type picks it up — drawLabel handles the colour.
                    if (!c.isDriving) {
                        char ref[48];
                        std::snprintf(ref, sizeof(ref), "[%s]", text);
                        drawLabel(lpos, ref, c, anchor);
                    } else {
                        drawLabel(lpos, text, c, anchor);
                    }
                };
                char lbl[40];
                for (const auto& c : m_activeSketch->getConstraints()) {
                    if (c.type == ConstraintType::Distance) {
                        const SketchPoint* p1 = m_activeSketch->getPoint(c.entityA);
                        const SketchPoint* p2 = m_activeSketch->getPoint(c.entityB);
                        if (!p1 || !p2) continue;
                        glm::vec2 mid = 0.5f * (p1->pos + p2->pos);
                        glm::vec2 perp(-(p2->pos.y - p1->pos.y), p2->pos.x - p1->pos.x);
                        float pl = glm::length(perp);
                        // Push the label well off the line so it doesn't sit
                        // under the move/rotate gizmo when both selected
                        // points are highlighted. Scaled by the segment
                        // length so the offset stays proportional and the
                        // label doesn't end up miles away on tiny dimensions.
                        if (pl > 1e-6f) {
                            float segLen = glm::length(p2->pos - p1->pos);
                            float off = std::max(3.0f, segLen * 0.18f);
                            perp = perp / pl * off;
                        }
                        std::snprintf(lbl, sizeof(lbl), "%s", materializr::fmtLength(c.value).c_str());
                        placeLabel(mid, perp, lbl, c);
                    } else if (c.type == ConstraintType::Radius) {
                        glm::vec2 center(0.0f);
                        float radius = 1.0f;
                        bool found = false;
                        for (const auto& circ : m_activeSketch->getCircles()) {
                            if (circ.id == c.entityA) {
                                const SketchPoint* cp = m_activeSketch->getPoint(circ.centerPointId);
                                if (cp) {
                                    center = cp->pos;
                                    radius = static_cast<float>(circ.radius);
                                    found = true;
                                }
                                break;
                            }
                        }
                        if (!found) {
                            for (const auto& arc : m_activeSketch->getArcs()) {
                                if (arc.id == c.entityA) {
                                    const SketchPoint* cp = m_activeSketch->getPoint(arc.centerPointId);
                                    if (cp) {
                                        center = cp->pos;
                                        radius = static_cast<float>(arc.radius);
                                        found = true;
                                    }
                                    break;
                                }
                            }
                        }
                        if (!found) continue;
                        // Auto label sits tangentially outside the circle (up
                        // and to the right) so the centre stays clear for
                        // concentric-circle drawing. Offset is the radius + a
                        // small constant so it floats just past the perimeter.
                        glm::vec2 autoOff =
                            glm::vec2(0.7071f, 0.7071f) * (radius + 1.2f);
                        // Drafting convention: arcs read as R, circles as Ø.
                        if (materializr::constraintIsArcRadius(*m_activeSketch, c))
                            std::snprintf(lbl, sizeof(lbl), "R %s", materializr::fmtLength(c.value).c_str());
                        else
                            std::snprintf(lbl, sizeof(lbl), "\xC3\x98 %s",
                                          materializr::fmtLength(c.value * 2.0).c_str());
                        placeLabel(center, autoOff, lbl, c);
                    } else if (c.type == ConstraintType::Angle) {
                        // SolidWorks-style angle dim: find the vertex where the
                        // two constrained lines meet, draw a small arc spanning
                        // the angle, and place the °-label hugging the outside
                        // of the arc. Falls back to the old line-A midpoint
                        // label only if the geometry doesn't admit a sensible
                        // vertex.
                        const SketchLine* lA = nullptr;
                        const SketchLine* lB = nullptr;
                        for (const auto& l : m_activeSketch->getLines()) {
                            if (l.id == c.entityA) lA = &l;
                            else if (l.id == c.entityB) lB = &l;
                        }
                        if (!lA || !lB) continue;
                        const SketchPoint* aS = m_activeSketch->getPoint(lA->startPointId);
                        const SketchPoint* aE = m_activeSketch->getPoint(lA->endPointId);
                        const SketchPoint* bS = m_activeSketch->getPoint(lB->startPointId);
                        const SketchPoint* bE = m_activeSketch->getPoint(lB->endPointId);
                        if (!aS || !aE || !bS || !bE) continue;

                        // Vertex = the closest pair of endpoints between the two
                        // lines. Average them so the arc is anchored even when
                        // the lines don't quite touch (Coincident-loose).
                        glm::vec2 aEnds[2] = { aS->pos, aE->pos };
                        glm::vec2 bEnds[2] = { bS->pos, bE->pos };
                        int ai = 0, bi = 0; float minD = FLT_MAX;
                        for (int i = 0; i < 2; ++i)
                            for (int j = 0; j < 2; ++j) {
                                float d = glm::length(aEnds[i] - bEnds[j]);
                                if (d < minD) { minD = d; ai = i; bi = j; }
                            }
                        glm::vec2 vertex = 0.5f * (aEnds[ai] + bEnds[bi]);
                        glm::vec2 dirA = aEnds[1 - ai] - vertex;
                        glm::vec2 dirB = bEnds[1 - bi] - vertex;
                        float lenA = glm::length(dirA), lenB = glm::length(dirB);
                        if (lenA < 1e-4f || lenB < 1e-4f) continue;
                        dirA /= lenA; dirB /= lenB;

                        // Arc radius scales with the shorter line so the arc
                        // stays inside the visible angle. Clamped so it never
                        // shrinks below 2mm (illegible) or balloons past 20mm
                        // (covers neighbouring geometry).
                        float arcR = std::min(lenA, lenB) * 0.25f;
                        arcR = std::clamp(arcR, 2.0f, 20.0f);

                        // Pick the SHORTER arc between dirA and dirB so the
                        // label sits in the same wedge the user sees as "the
                        // angle". atan2 + signed delta wrapped to (-π, π].
                        float angA = std::atan2(dirA.y, dirA.x);
                        float angB = std::atan2(dirB.y, dirB.x);
                        float diff = angB - angA;
                        while (diff >  M_PI) diff -= 2.0f * M_PI;
                        while (diff < -M_PI) diff += 2.0f * M_PI;

                        // Sample the arc and AddPolyline through screen-space —
                        // sketches can sit on arbitrarily-oriented planes, so
                        // we can't draw a flat 2D arc; each sample goes through
                        // dim2world → toImg like the rest of the sketch overlay.
                        constexpr int N = 28;
                        ImVec2 pts[N + 1];
                        int nPts = 0;
                        for (int i = 0; i <= N; ++i) {
                            float t = float(i) / float(N);
                            float a = angA + diff * t;
                            glm::vec2 p2 = vertex + glm::vec2(std::cos(a), std::sin(a)) * arcR;
                            ImVec2 ip;
                            if (toImg(dim2world(p2), ip)) pts[nPts++] = ip;
                        }
                        if (nPts > 1) {
                            // Light halo so the arc reads against the sketch
                            // grid lines, then the dim-yellow stroke on top.
                            dl->AddPolyline(pts, nPts, IM_COL32(20, 20, 28, 200),
                                            0, 3.0f);
                            dl->AddPolyline(pts, nPts, IM_COL32(255, 235, 120, 230),
                                            0, 1.5f);
                        }

                        // Auto label hugs the outside of the arc midpoint.
                        float midA = angA + diff * 0.5f;
                        glm::vec2 labelPos = vertex +
                            glm::vec2(std::cos(midA), std::sin(midA)) * (arcR + 2.5f);
                        float deg = std::abs(static_cast<float>(diff * 180.0 / M_PI));
                        std::snprintf(lbl, sizeof(lbl), "%.1f\xC2\xB0", deg);
                        // The stored offset is relative to dimensionAutoAnchor's
                        // Angle anchor (mean of all four line endpoints), NOT
                        // `vertex` — applyPendingDimension() computed it that
                        // way, so the leader/placed-position math has to match
                        // or a placed label would drift from where it was
                        // dropped. autoOff folds that mismatch away: it's
                        // defined so anchor + autoOff == labelPos exactly,
                        // reproducing today's arc-hugging auto placement
                        // whenever the constraint has no stored offset.
                        glm::vec2 angleAnchor =
                            0.25f * (aS->pos + aE->pos + bS->pos + bE->pos);
                        glm::vec2 autoOff = labelPos - angleAnchor;
                        placeLabel(angleAnchor, autoOff, lbl, c);
                    } else if (c.type == ConstraintType::DistancePointLine) {
                        // Validate the entities exist before trusting
                        // dimensionAutoAnchor's result — it silently returns
                        // (0,0) on a dangling id, which would otherwise plant
                        // a stray label at the sketch origin.
                        const SketchPoint* dp = m_activeSketch->getPoint(c.entityA);
                        const SketchLine* dpLine = nullptr;
                        for (const auto& l : m_activeSketch->getLines())
                            if (l.id == c.entityB) { dpLine = &l; break; }
                        if (!dp || !dpLine) continue;
                        const SketchPoint* ls = m_activeSketch->getPoint(dpLine->startPointId);
                        const SketchPoint* le = m_activeSketch->getPoint(dpLine->endPointId);
                        if (!ls || !le) continue;
                        glm::vec2 seg = le->pos - ls->pos;
                        float segLen = glm::length(seg);
                        if (segLen < 1e-6f) continue;
                        glm::vec2 dirA = seg / segLen;
                        glm::vec2 nA(-dirA.y, dirA.x);
                        float sd = glm::dot(dp->pos - ls->pos, nA);

                        PendingDimension pd;
                        pd.type = c.type; pd.entityA = c.entityA;
                        pd.entityB = c.entityB; pd.valid = true;
                        glm::vec2 anchor = dimensionAutoAnchor(pd);
                        bool hasOff = (c.labelOffX != 0.0 || c.labelOffY != 0.0);
                        glm::vec2 labelPos = hasOff
                            ? anchor + glm::vec2(static_cast<float>(c.labelOffX),
                                                 static_cast<float>(c.labelOffY))
                            : anchor;
                        // CAD-style perpendicular dimension line, drawn at the
                        // label's station along the line. The constraint pins
                        // one endpoint of the measured pair — in rectangles
                        // that's a corner sitting on a NEIGHBORING edge, and a
                        // leader pointing there read as "the dim attached to a
                        // third line". The dim line spans the measured gap
                        // wherever the label sits instead.
                        float tL = glm::clamp(glm::dot(labelPos - ls->pos, dirA),
                                              0.0f, segLen);
                        glm::vec2 baseA = ls->pos + dirA * tL;
                        glm::vec2 baseB = baseA + nA * sd;
                        ImVec2 pA, pB;
                        if (toImg(dim2world(baseA), pA) && toImg(dim2world(baseB), pB)) {
                            dl->AddLine(pA, pB, IM_COL32(20, 20, 28, 200), 3.0f);
                            dl->AddLine(pA, pB, IM_COL32(255, 235, 120, 230), 1.5f);
                        }
                        std::snprintf(lbl, sizeof(lbl), "%s", materializr::fmtLength(c.value).c_str());
                        glm::vec2 midDim = 0.5f * (baseA + baseB);
                        placeLabel(anchor, midDim - anchor, lbl, c, &midDim);
                    } else if (c.type == ConstraintType::CircleGap) {
                        // Draw the gap segment between the two facing rims,
                        // label at its midpoint (the leader runs from there).
                        auto grab = [&](int id, glm::vec2& ctr, double& r) -> bool {
                            for (const auto& cc : m_activeSketch->getCircles())
                                if (cc.id == id) {
                                    const SketchPoint* p = m_activeSketch->getPoint(cc.centerPointId);
                                    if (p) { ctr = p->pos; r = cc.radius; return true; }
                                }
                            for (const auto& aa : m_activeSketch->getArcs())
                                if (aa.id == id) {
                                    const SketchPoint* p = m_activeSketch->getPoint(aa.centerPointId);
                                    if (p) { ctr = p->pos; r = aa.radius; return true; }
                                }
                            return false;
                        };
                        glm::vec2 cA, cB; double rA = 0, rB = 0;
                        if (!grab(c.entityA, cA, rA) || !grab(c.entityB, cB, rB)) continue;
                        glm::vec2 d = cB - cA;
                        float len = glm::length(d);
                        if (len < 1e-6f) continue;
                        glm::vec2 u = d / len;
                        glm::vec2 rimA = cA + u * static_cast<float>(rA);
                        glm::vec2 rimB = cB - u * static_cast<float>(rB);
                        ImVec2 pA, pB;
                        if (toImg(dim2world(rimA), pA) && toImg(dim2world(rimB), pB)) {
                            dl->AddLine(pA, pB, IM_COL32(20, 20, 28, 200), 3.0f);
                            dl->AddLine(pA, pB, IM_COL32(255, 235, 120, 230), 1.5f);
                        }
                        PendingDimension pd;
                        pd.type = c.type; pd.entityA = c.entityA;
                        pd.entityB = c.entityB; pd.valid = true;
                        glm::vec2 anchor = dimensionAutoAnchor(pd);
                        std::snprintf(lbl, sizeof(lbl), "%s", materializr::fmtLength(c.value).c_str());
                        placeLabel(anchor, glm::vec2(0.0f), lbl, c, &anchor);
                    }
                }

                // Release ends a label drag. A press that never really moved is
                // a click, and opens the editor — so tapping a tag still edits
                // it, while dragging one repositions it.
                if (m_dimDragId >= 0 &&
                    ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    if (!m_dimDragMoved) {
                        for (const auto& c : m_activeSketch->getConstraints())
                            if (c.id == m_dimDragId) { openDimEdit(c); break; }
                    } else {
                        // The offset was written live during the drag, so the
                        // sketch already holds the new position; just mark the
                        // project dirty so it is saved.
                        markDirty();
                    }
                    m_dimDragId    = -1;
                    m_dimDragMoved = false;
                }

                // Dimension tool feedback: highlight the hovered pickable
                // entity while picking, and ghost the pending label at the
                // cursor (with a leader from its anchor) once enough entities
                // are picked to resolve a dimension.
                if (m_sketchTool->getMode() == SketchToolMode::Dimension) {
                    ImVec2 mp = ImGui::GetMousePos();
                    glm::vec2 sketchCursor = screenToSketch(
                        mp.x - imgMin.x, mp.y - imgMin.y, imgSize.x, imgSize.y);
                    DimPick hov = m_sketchTool->dimHitTest(sketchCursor);
                    if (hov.kind == DimEntityKind::Point) {
                        const SketchPoint* p = m_activeSketch->getPoint(hov.id);
                        ImVec2 sp;
                        if (p && toImg(dim2world(p->pos), sp))
                            dl->AddCircle(sp, 7.0f, IM_COL32(255, 235, 120, 255), 0, 2.0f);
                    } else if (hov.kind == DimEntityKind::Line) {
                        for (const auto& l : m_activeSketch->getLines()) {
                            if (l.id != hov.id) continue;
                            const SketchPoint* a = m_activeSketch->getPoint(l.startPointId);
                            const SketchPoint* b = m_activeSketch->getPoint(l.endPointId);
                            ImVec2 sa, sb;
                            if (a && b && toImg(dim2world(a->pos), sa) &&
                                toImg(dim2world(b->pos), sb))
                                dl->AddLine(sa, sb, IM_COL32(255, 235, 120, 200), 3.0f);
                            break;
                        }
                    } else if (hov.kind == DimEntityKind::Circle ||
                               hov.kind == DimEntityKind::Arc) {
                        glm::vec2 hCenter(0.0f);
                        float hRadius = 0.0f;
                        bool hFound = false;
                        if (hov.kind == DimEntityKind::Circle) {
                            for (const auto& circ : m_activeSketch->getCircles())
                                if (circ.id == hov.id) {
                                    const SketchPoint* cp = m_activeSketch->getPoint(circ.centerPointId);
                                    if (cp) {
                                        hCenter = cp->pos;
                                        hRadius = static_cast<float>(circ.radius);
                                        hFound = true;
                                    }
                                    break;
                                }
                        } else {
                            for (const auto& arc : m_activeSketch->getArcs())
                                if (arc.id == hov.id) {
                                    const SketchPoint* cp = m_activeSketch->getPoint(arc.centerPointId);
                                    if (cp) {
                                        hCenter = cp->pos;
                                        hRadius = static_cast<float>(arc.radius);
                                        hFound = true;
                                    }
                                    break;
                                }
                        }
                        if (hFound) {
                            // Screen-space radius: project the center and a
                            // point one radius away along the sketch plane's
                            // X axis, then measure the pixel span between
                            // them (the plane may be tilted/scaled on screen).
                            ImVec2 sc, sr;
                            if (toImg(dim2world(hCenter), sc) &&
                                toImg(dim2world(hCenter + glm::vec2(hRadius, 0.0f)), sr)) {
                                float screenR = std::hypot(sr.x - sc.x, sr.y - sc.y);
                                dl->AddCircle(sc, screenR, IM_COL32(255, 235, 120, 255), 0, 2.0f);
                            }
                        }
                    }

                    const PendingDimension& pend = m_sketchTool->getPendingDimension();
                    if (pend.valid &&
                        (m_sketchTool->getDimPhase() == DimPhase::PlaceLabel ||
                         m_sketchTool->getDimPhase() == DimPhase::PickSecondOrPlace)) {
                        char gbuf[40];
                        if (pend.type == ConstraintType::Angle)
                            std::snprintf(gbuf, sizeof(gbuf), "%.1f\xC2\xB0",
                                          std::abs(pend.measured) * 180.0 / M_PI);
                        else if (pend.type == ConstraintType::Radius)
                            std::snprintf(gbuf, sizeof(gbuf), "\xC3\x98 %s",
                                          materializr::fmtLength(pend.measured * 2.0).c_str());
                        else
                            std::snprintf(gbuf, sizeof(gbuf), "%s", materializr::fmtLength(pend.measured).c_str());
                        glm::vec2 ganchor = dimensionAutoAnchor(pend);
                        // Pending point-to-line / parallel-line dims preview
                        // the same CAD-style perpendicular dimension line the
                        // committed render draws, following the cursor's
                        // station — the leader then hugs the dim line rather
                        // than the corner point carrying the constraint.
                        if (pend.type == ConstraintType::DistancePointLine) {
                            const SketchPoint* gp = m_activeSketch->getPoint(pend.entityA);
                            const SketchLine* gl = nullptr;
                            for (const auto& l : m_activeSketch->getLines())
                                if (l.id == pend.entityB) { gl = &l; break; }
                            const SketchPoint* gs = gl ? m_activeSketch->getPoint(gl->startPointId) : nullptr;
                            const SketchPoint* ge = gl ? m_activeSketch->getPoint(gl->endPointId) : nullptr;
                            if (gp && gs && ge) {
                                glm::vec2 seg = ge->pos - gs->pos;
                                float segLen = glm::length(seg);
                                if (segLen > 1e-6f) {
                                    glm::vec2 dirA = seg / segLen;
                                    glm::vec2 nA(-dirA.y, dirA.x);
                                    float sd = glm::dot(gp->pos - gs->pos, nA);
                                    float tL = glm::clamp(
                                        glm::dot(sketchCursor - gs->pos, dirA),
                                        0.0f, segLen);
                                    glm::vec2 baseA = gs->pos + dirA * tL;
                                    glm::vec2 baseB = baseA + nA * sd;
                                    ImVec2 gA, gB;
                                    if (toImg(dim2world(baseA), gA) &&
                                        toImg(dim2world(baseB), gB)) {
                                        dl->AddLine(gA, gB, IM_COL32(20, 20, 28, 200), 3.0f);
                                        dl->AddLine(gA, gB, IM_COL32(255, 235, 120, 230), 1.5f);
                                    }
                                    ganchor = 0.5f * (baseA + baseB);
                                }
                            }
                        }
                        // A pending diameter (single circle/arc picked, still
                        // deciding whether a second circle turns it into a
                        // gap) draws a diameter line ACROSS the circle rather
                        // than a leader from the centre — a centre-anchored
                        // leader reads as "measuring from the centre" and hid
                        // that a second circle click makes a rim gap.
                        bool drewSpecial = false;
                        if (pend.type == ConstraintType::Radius) {
                            glm::vec2 ctr(0.0f); float rad = 0.0f; bool found = false;
                            for (const auto& circ : m_activeSketch->getCircles())
                                if (circ.id == pend.entityA) {
                                    const SketchPoint* cp = m_activeSketch->getPoint(circ.centerPointId);
                                    if (cp) { ctr = cp->pos; rad = static_cast<float>(circ.radius); found = true; }
                                    break;
                                }
                            for (const auto& arc : m_activeSketch->getArcs())
                                if (arc.id == pend.entityA) {
                                    const SketchPoint* cp = m_activeSketch->getPoint(arc.centerPointId);
                                    if (cp) { ctr = cp->pos; rad = static_cast<float>(arc.radius); found = true; }
                                    break;
                                }
                            if (found) {
                                // Diameter line oriented toward the cursor.
                                glm::vec2 d = sketchCursor - ctr;
                                float len = glm::length(d);
                                glm::vec2 u = (len > 1e-6f) ? d / len : glm::vec2(1.0f, 0.0f);
                                ImVec2 e1, e2;
                                if (toImg(dim2world(ctr - u * rad), e1) &&
                                    toImg(dim2world(ctr + u * rad), e2)) {
                                    dl->AddLine(e1, e2, IM_COL32(20, 20, 28, 200), 3.0f);
                                    dl->AddLine(e1, e2, IM_COL32(255, 235, 120, 230), 1.5f);
                                    ImVec2 sc;
                                    if (toImg(dim2world(sketchCursor), sc))
                                        dl->AddText(ImVec2(sc.x + 8, sc.y - 8),
                                                    IM_COL32(255, 235, 120, 220), gbuf);
                                    drewSpecial = true;
                                }
                            }
                        }
                        ImVec2 sa, sc;
                        if (!drewSpecial &&
                            toImg(dim2world(ganchor), sa) &&
                            toImg(dim2world(sketchCursor), sc)) {
                            dl->AddLine(sa, sc, IM_COL32(255, 235, 120, 90), 1.0f);
                            dl->AddText(ImVec2(sc.x + 8, sc.y - 8),
                                        IM_COL32(255, 235, 120, 220), gbuf);
                        }
                    }
                }

                // Edit popup for the currently-clicked dimension label.
                // BeginPopup returning false (Esc / click-outside) closes the
                // edit and leaves the constraint unchanged. Enter commits the
                // typed value, re-runs the solver, and marks the project dirty.
                // A left click landing while the edit popup is up belongs to
                // the popup (typically the dismiss-click outside it). The
                // Dimension tool's routing later this frame checks this flag
                // so that click can't double as a fresh entity pick — without
                // it, dismissing the popup over a line started an unwanted
                // new dimension on that line.
                m_dimPopupSwallowClick =
                    (m_dimEditingId >= 0) &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                if (m_dimEditingId >= 0) {
                    // Latch "the popup just consumed an Escape" BEFORE
                    // BeginPopup below can act on it and clear
                    // m_dimEditingId — the global Escape chain in
                    // handleShortcuts() runs AFTER this render pass, so by
                    // then m_dimEditingId would already read -1 and the
                    // chain would wrongly treat the press as a fresh
                    // Dimension-mode / sketch-exit step instead of "just
                    // closed the popup". Covers both ImGui Escape behaviours
                    // here: while the InputText has focus, the first Escape
                    // only defocuses it (BeginPopup still returns true,
                    // m_dimEditingId unchanged) — this block is still
                    // entered next frame with the popup still up, so a
                    // second Escape (the one that actually closes it) is
                    // caught the same way.
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        m_dimPopupConsumedEsc = true;
                    }
                    // Restore normal padding — the viewport window's
                    // WindowPadding(0,0) is still pushed here, and the popup
                    // captures the style at its own Begin.
                    ImGui::PushStyleVar(
                        ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0f * uiScale(), 8.0f * uiScale()));
                    if (ImGui::BeginPopup("##DimEdit")) {
                        ImGui::TextUnformatted(materializr::tr("Edit dimension"));
                        ImGui::Separator();
                        if (m_dimEditingFocus) {
                            ImGui::SetKeyboardFocusHere();
                            m_dimEditingFocus = false;
                        }
                        ImGui::SetNextItemWidth(120.0f);
                        // What is being edited decides the widget. Angles are
                        // degrees and never see a unit. Lengths take a TEXT
                        // field on desktop so "2in" / "50mm" can be typed;
                        // under touch the number pad edits the value in the
                        // display unit (the pad has no letters).
                        materializr::DimKind dimKind = materializr::DimKind::Length;
                        bool dimIsArc = false;
                        for (const auto& cc : m_activeSketch->getConstraints()) {
                            if (cc.id != m_dimEditingId) continue;
                            dimKind = cc.type == ConstraintType::Angle  ? materializr::DimKind::Angle
                                    : cc.type == ConstraintType::Radius ? materializr::DimKind::Radius
                                                                        : materializr::DimKind::Length;
                            dimIsArc = dimKind == materializr::DimKind::Radius &&
                                       materializr::constraintIsArcRadius(*m_activeSketch, cc);
                            break;
                        }
                        bool dimCommitted = false;
                        if (dimKind == materializr::DimKind::Angle || materializr::touchMode()) {
                            // Buffer stays authoritative — every seed site
                            // writes it — so parse it in, let the numeric
                            // widget edit it, and write the commit back.
                            double dimPadV = 0.0;
                            (void)materializr::parseFinite(m_dimEditingBuf, dimPadV);
                            char padFmt[8] = "%.2f";
                            if (dimKind != materializr::DimKind::Angle)
                                std::snprintf(padFmt, sizeof(padFmt), "%%.%df",
                                              materializr::unitInfo(materializr::currentUnit()).decimals);
                            if (materializr::inputNumber("##dimval", &dimPadV, 0.0, 0.0, padFmt,
                                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                                std::snprintf(m_dimEditingBuf, sizeof(m_dimEditingBuf), "%.6g", dimPadV);
                                dimCommitted = true;
                            }
                        } else {
                            // Enter, or focus leaving after an edit, commits.
                            if (ImGui::InputText("##dimval", m_dimEditingBuf, sizeof(m_dimEditingBuf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue) ||
                                ImGui::IsItemDeactivatedAfterEdit())
                                dimCommitted = true;
                            ImGui::SetItemTooltip("%s", materializr::tr(
                                "A bare number is in the display unit; add a unit to "
                                "override it, e.g. 2in or 50mm."));
                        }
                        if (dimCommitted) {
                            // recordSketchMutation snapshots before/after and
                            // pushes a SketchEditOp so the dimension edit is
                            // Ctrl-Z-able and visible in the History panel.
                            recordSketchMutation([&]{
                                for (auto& cn : m_activeSketch->getMutableConstraints()) {
                                    if (cn.id != m_dimEditingId) continue;
                                    // Convert to mm FIRST, then halve a circle's
                                    // typed diameter — applyDimensionEdit owns
                                    // that order. A refusal (garbage, <= 0)
                                    // leaves the value alone. Typing a number
                                    // says this measurement should CONTROL the
                                    // geometry, so it promotes a reference
                                    // dimension to driving.
                                    if (materializr::applyDimensionEdit(dimKind, dimIsArc,
                                                                        m_dimEditingBuf, cn.value) &&
                                        constraintSupportsReference(cn.type))
                                        cn.isDriving = true;
                                    break;
                                }
                                if (m_sketchSolver) m_sketchSolver->solve(*m_activeSketch);
                            });
                            markDirty();
                            m_meshesDirty = true;
                            m_dimEditingId = -1;
                            ImGui::CloseCurrentPopup();
                        }
                        // Driving toggle. A dimension placed by the Dimension
                        // tool starts as reference (measures only); this is
                        // where the user promotes it to control the geometry,
                        // or demotes a driving one back to an annotation.
                        // Hidden for geometric constraints, where "reference"
                        // is meaningless (see constraintSupportsReference).
                        {
                            Constraint* cur = nullptr;
                            for (auto& cn : m_activeSketch->getMutableConstraints())
                                if (cn.id == m_dimEditingId) { cur = &cn; break; }
                            if (cur && constraintSupportsReference(cur->type)) {
                                ImGui::Spacing();
                                bool drv = cur->isDriving;
                                if (ImGui::Checkbox(materializr::tr("Driving"), &drv)) {
                                    recordSketchMutation([&]{
                                        for (auto& cn : m_activeSketch->getMutableConstraints()) {
                                            if (cn.id != m_dimEditingId) continue;
                                            cn.isDriving = drv;
                                            break;
                                        }
                                        if (m_sketchSolver) m_sketchSolver->solve(*m_activeSketch);
                                    });
                                    markDirty();
                                    m_meshesDirty = true;
                                }
                                ImGui::SameLine();
                                ImGui::TextDisabled(drv ? "(controls geometry)"
                                                        : "(measures only)");
                            }
                            // "Make equal" — converts a two-entity dimension
                            // into an Equal constraint on the same pair (equal
                            // length for lines, equal radius for circles/arcs).
                            // Equal carries no measurement, so it is always a
                            // driving constraint; the numeric dimension it
                            // replaces is consumed by the conversion.
                            int eqA = -1, eqB = -1;
                            if (cur && dimensionEqualPair(*m_activeSketch, *cur, eqA, eqB)) {
                                ImGui::Spacing();
                                if (ImGui::Button(materializr::tr("Make equal"))) {
                                    int convId = m_dimEditingId;
                                    recordSketchMutation([&]{
                                        for (auto& cn : m_activeSketch->getMutableConstraints()) {
                                            if (cn.id != convId) continue;
                                            cn.type = ConstraintType::Equal;
                                            cn.entityA = eqA;
                                            cn.entityB = eqB;
                                            cn.value = 0.0;
                                            cn.isDriving = true;
                                            break;
                                        }
                                        if (m_sketchSolver) m_sketchSolver->solve(*m_activeSketch);
                                    });
                                    markDirty();
                                    m_meshesDirty = true;
                                    m_dimEditingId = -1;
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::SameLine();
                                ImGui::TextDisabled("%s", materializr::tr("(equal length / radius)"));
                            }
                        }
                        // Delete removes the dimension constraint entirely, as
                        // one undoable step ("Remove …" in History).
                        ImGui::Spacing();
                        if (ImGui::Button(materializr::tr("Delete")) ||
                            ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
                            int delId = m_dimEditingId;
                            recordSketchMutation([&]{
                                m_activeSketch->removeConstraint(delId);
                                if (m_sketchSolver) m_sketchSolver->solve(*m_activeSketch);
                            });
                            markDirty();
                            m_meshesDirty = true;
                            m_dimEditingId = -1;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", materializr::tr("(Del)"));
                        ImGui::EndPopup();
                    } else {
                        // Popup closed without committing — drop edit state.
                        m_dimEditingId = -1;
                    }
                    ImGui::PopStyleVar(); // ##DimEdit WindowPadding
                }
            }

            // Measure tool — Line mode: render the two captured points and the
            // segment between them in screen space, in a colour deliberately
            // unlike anything else (body edges = white/cyan, sketches = blue,
            // selection highlight = yellow). Magenta-purple here. The first
            // captured point also shows on its own (before the second click)
            // so the user gets feedback that the click landed.
            if (m_measureTool && m_measureTool->getMode() == MeasureMode::Line) {
                const ImU32 measureCol  = IM_COL32(210, 90, 240, 255);
                const ImU32 measureGlow = IM_COL32(210, 90, 240, 90);

                int captured = m_measureTool->getCapturedPointCount();
                ImVec2 sp1, sp2;
                bool ok1 = (captured >= 1) &&
                           toImg(m_measureTool->getCapturedPoint(0), sp1);
                bool ok2 = (captured >= 2) &&
                           toImg(m_measureTool->getCapturedPoint(1), sp2);

                if (ok1) {
                    dl->AddCircleFilled(sp1, 8.0f, measureGlow);
                    dl->AddCircleFilled(sp1, 4.5f, measureCol);
                }
                if (ok2) {
                    dl->AddCircleFilled(sp2, 8.0f, measureGlow);
                    dl->AddCircleFilled(sp2, 4.5f, measureCol);
                }
                if (ok1 && ok2) {
                    dl->AddLine(sp1, sp2, measureCol, 2.5f);
                    // Centred distance label on the midpoint of the segment.
                    const auto& results = m_measureTool->getResults();
                    if (!results.empty()) {
                        char lbl[40];
                        std::snprintf(lbl, sizeof(lbl), "%s", materializr::fmtLength(results[0].value).c_str());
                        ImVec2 ts = ImGui::CalcTextSize(lbl);
                        ImVec2 mid((sp1.x + sp2.x) * 0.5f - ts.x * 0.5f,
                                   (sp1.y + sp2.y) * 0.5f - ts.y - 6.0f);
                        dl->AddRectFilled(ImVec2(mid.x - 4, mid.y - 2),
                                          ImVec2(mid.x + ts.x + 4, mid.y + ts.y + 2),
                                          IM_COL32(30, 18, 38, 220), 3.0f);
                        dl->AddText(mid, measureCol, lbl);
                    }
                }
            }
        }

        bool viewportHovered = ImGui::IsItemHovered();
        if (materializr::touchMode()) {
            // ImGui drops IsItemHovered() a couple of frames into a press-drag — the
            // window-move grab claims the ActiveId, so the plain Image stops reading
            // as hovered and this whole input block (incl. the live sketch preview's
            // onMouseMove) would freeze until release. Latch it: once a left press
            // begins over the viewport, keep the block alive until the button lifts.
            if (viewportHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                m_viewportInputLatch = true;
            // A two-finger takeover releases the button AND parks the cursor
            // off-screen (so the widget under the first finger can't fire,
            // #39). Hold the latch through the gesture: pan/zoom consumption
            // and the release-frame handlers (placement rollback, box-select
            // end) live inside this hovered scope and must keep running,
            // exactly as they did when the cursor stayed frozen at the press.
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                !(m_window && m_window->twoFingerActive()))
                m_viewportInputLatch = false;
            if (m_viewportInputLatch) viewportHovered = true;
        }
        // Tell the input layer whether a long-press may arm right now. Over a
        // slider/panel it must not (slow slider drags popped the ring); while a
        // sketch DRAWING tool is active it must not either — a slow, precise
        // press-drag-release (within the drag slop) would arm the hold, skip
        // the placement, and pop a context menu instead. Same while an
        // interactive op's arrow owns the one-finger drag. Select mode keeps
        // it: hold-still = context menu, hold-then-drag = box select.
        {
            // Viewport OR the Items panel (its rows have context menus and it has
            // no sliders, so it's safe). Items hover is from last frame — it
            // renders after the viewport — but a stationary long-press is stable
            // across frames, so the lag is harmless.
            bool allowLongPress = viewportHovered ||
                                  (m_itemsPanel && m_itemsPanel->isHovered()) ||
                                  // im-touch has no ItemsPanel; its own tree
                                  // overlay reports hover the same way so a
                                  // press-and-hold on a row opens its menu.
                                  m_imTouchTreeHovered ||
                                  // The tab strip: its per-tab Save / Save As /
                                  // Close menu is right-click-only, so without
                                  // this there is no touch route to it at all.
                                  // No sliders live there, so the drag hazard
                                  // that gates this list doesn't apply.
                                  m_tabBarHovered;
            if (m_inSketchMode && m_sketchTool &&
                m_sketchTool->getMode() != SketchToolMode::Select)
                allowLongPress = false;
            if (m_ppCtl.active() || m_extrudeCtl.active() || m_edgeCtl.active() ||
                anyIopActive())
                allowLongPress = false;
            if (m_window) m_window->setTouchOverViewport(allowLongPress);
            // Consume-and-clear: the tab strips set this during their own
            // render, which may be before or after this pass depending on the
            // layout. A stationary long-press is stable across frames, so a
            // one-frame lag is harmless (same reasoning as the Items hover).
            m_tabBarHovered = false;
            // Strict canvas hover (excludes the Items panel) gates touch
            // drag-to-scroll: a vertical drag over any panel scrolls it, but
            // over the 3D canvas the one-finger drag stays an orbit.
            if (m_window) m_window->setTouchOnCanvas(viewportHovered);
        }
        if (viewportHovered) {
            ImGuiIO& io = ImGui::GetIO();
            // Multi-select toggle = the touch stand-in for holding Ctrl. Force
            // io.KeyCtrl on for this hovered-viewport scope so every selection
            // path below treats taps as additive; restored automatically on exit
            // (so panels rendered later see the real Ctrl state).
            struct CtrlForce {
                ImGuiIO& io; bool saved;
                CtrlForce(ImGuiIO& i, bool f) : io(i), saved(i.KeyCtrl) { if (f) io.KeyCtrl = true; }
                ~CtrlForce() { io.KeyCtrl = saved; }
            } ctrlForce_(io, m_multiSelectToggle);
            if (io.MouseWheel != 0.0f) {
                // Zoom toward whatever the cursor is over (Blender/Fusion-360
                // feel). Ray-cast against the document for a real hit point;
                // fall back to the ray's intersection with the plane through
                // the current target perpendicular to the view direction —
                // that gives a sensible focus even over empty space and means
                // empty-space scrolling matches the legacy dolly-to-target
                // behaviour.
                ImVec2 mp = ImGui::GetMousePos();
                ImVec2 wpz = ImGui::GetItemRectMin();
                float lx = mp.x - wpz.x;
                float ly = mp.y - wpz.y;
                glm::vec3 focus = cam.getTarget();
                bool gotHit = false;
                // Reuse this frame's (or last frame's) hover pick — same
                // cursor, same ray. A fresh full-document ray-cast per
                // wheel tick was the zoom stutter on dense scenes; when
                // the hover pick was skipped (mid-drag etc.) the plane
                // fallback below still gives a sensible focus.
                if (m_zoomFocusFrame >= 0 &&
                    ImGui::GetFrameCount() - m_zoomFocusFrame <= 2) {
                    if (m_zoomFocusHit) {
                        focus = m_zoomFocusPoint;
                        gotHit = true;
                    }
                }
                if (!gotHit) {
                    // Unproject the cursor into the world (NDC → ray) and
                    // intersect with the plane through the camera target
                    // perpendicular to the view direction. That gives us a
                    // focus point at the same depth as the current target,
                    // so empty-space scrolling matches the legacy
                    // dolly-to-target behaviour and full-space scrolling
                    // zooms toward whatever's under the cursor.
                    float ndcX = (2.0f * lx) / contentSize.x - 1.0f;
                    float ndcY = 1.0f - (2.0f * ly) / contentSize.y;
                    glm::mat4 invVP = glm::inverse(
                        cam.getProjectionMatrix() * cam.getViewMatrix());
                    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
                    glm::vec4 farH  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);
                    glm::vec3 rayO  = glm::vec3(nearH) / nearH.w;
                    glm::vec3 rayD  = glm::normalize(glm::vec3(farH) / farH.w - rayO);
                    glm::vec3 viewDir = glm::normalize(cam.getPosition() - cam.getTarget());
                    float denom = glm::dot(rayD, -viewDir);
                    if (std::abs(denom) > 1e-4f) {
                        float t = glm::dot(cam.getTarget() - rayO, -viewDir) / denom;
                        if (t > 0.0f) focus = rayO + rayD * t;
                    }
                }
                cam.zoomToward(focus, io.MouseWheel);
            }
            // Camera drag uses the configurable bindings (File > Settings). The
            // orbit button pans instead when Shift is held; a distinct pan button
            // always pans.
            //
            // In TRACKPAD mode both orbit and pan are bound to the LEFT button —
            // the same button the gizmo and picker use. Without suppression, a
            // gizmo-handle drag would also yank the camera, so the gizmo and
            // picker block the camera-drag while they own the interaction.
            // Scale Face joins gizmoOwnsDrag the moment its click handler
            // claims an axis (sets dragAxis on the down-frame). Without
            // this, trackpad mode — left-orbit, left-pan — would steal the
            // subsequent drag-threshold frame and run orbit instead of the
            // axis drag, so the gizmo "felt unclickable". Same story for
            // the fillet / chamfer arrow handles via m_edgeCtl.dragging(),
            // which the edge-op click hit-test sets on the down-frame
            // when the cursor is near the visible arrow line.
            bool gizmoOwnsDrag = m_gizmoDragging ||
                                 anyIopDraggingHandle() ||
                                 m_edgeCtl.dragging() ||
                                 m_ppCtl.sticky() ||
                                 m_extrudeCtl.sticky();
            if (materializr::touchMode()) {
                // A one-finger press-and-hold drives box-select, not orbit/pan — so
                // suppress the camera drag (and the two-finger consume below) while
                // it's engaged.
                if (m_window && m_window->isTouchHoldSelect()) gizmoOwnsDrag = true;
            }
            // Camera-drag suppression for the one-finger orbit only. In sketch
            // mode (touch) a one-finger drag drives the sketch rubber-band preview
            // (touch has no hover), so don't orbit. Two-finger pan/zoom still
            // works — it's gated on gizmoOwnsDrag, not this local.
            bool suppressCamDrag = gizmoOwnsDrag;
            // Shift+Left is the trackpad-mode pan gesture (orbit and pan are
            // both bound to Left, so Shift is the only thing telling them
            // apart). It has to lock the tools out from the PRESS frame, not
            // just once the drag is recognised: camDragging below only turns
            // true after ImGui's drag threshold, and a drawing tool places its
            // point on the press — so Shift+drag panned AND left a stray line
            // or circle behind, unless the user first switched to Select
            // (Steve: "i cannot do that without first changing to select/move,
            // otherwise i am drawing sketch elements even if i shift+click").
            // Shift means nothing to the sketch tools or the viewport picker
            // (multi-select is Ctrl), so swallowing it costs nothing. Touch
            // pans with two fingers and has no Shift to hold.
            const bool shiftPanGesture =
                !materializr::touchMode() && io.KeyShift &&
                (m_orbitButton == ImGuiMouseButton_Left ||
                 m_panButton   == ImGuiMouseButton_Left);
            // A primary drag should drive the ACTIVE TOOL, not the camera, while a
            // tool owns it. In sketch mode that's the rubber-band; during an
            // interactive op it's the op's arrow/handle (push/pull, extrude, edge
            // fillet/chamfer, move/scale face) — read below as a LEFT-button drag
            // gated on `!camDragging`. If that drag also orbits/pans the camera,
            // the gate never fires and the op "doesn't work".
            const bool toolWantsDrag =
                m_inSketchMode || m_ppCtl.active() || m_extrudeCtl.active() ||
                m_edgeCtl.active() || anyIopActive();
            if (materializr::touchMode()) {
                // Touch has no hover and the one finger is the only pointer, so the
                // tool always owns the drag. Two-finger still pans/zooms; Move (nav
                // lock) forces orbit.
                if (toolWantsDrag && !m_moveModeToggle) suppressCamDrag = true;
                if (m_moveModeToggle) suppressCamDrag = gizmoOwnsDrag;
            } else {
                // Desktop: the op reads a LEFT-button drag. Only steal the camera
                // drag when the orbit OR pan button IS Left (left-orbit / trackpad
                // mode) — that's the case where left-drag would orbit instead of
                // driving the op (the bug). With the default bindings (orbit=middle,
                // pan=right) Left is already free for the op, so we change nothing
                // and middle/right stay live for orbit/pan DURING the op.
                const bool leftIsCamera = (m_orbitButton == ImGuiMouseButton_Left ||
                                           m_panButton  == ImGuiMouseButton_Left);
                // ...EXCEPT keep Shift+Left-drag free for panning. In trackpad
                // mode (orbit+pan both Left) Shift+drag is the pan gesture, but
                // a sketch/op claimed Left entirely, so there was no way to pan
                // while sketching. Shift held → don't suppress → the Shift-pan
                // below fires and camDragging turns the sketch input off, so it
                // pans without also drawing.
                // Handle-aware, not blanket. The blanket version locked the
                // camera for the WHOLE op, so in trackpad mode (orbit and pan
                // both on Left) there was no way to orbit while a fillet /
                // chamfer / push-pull was underway. The controllers hit-test
                // on the press frame (edge arrows within 12 px, gizmo axes,
                // push-pull sticky), so gizmoOwnsDrag already says whether
                // THIS drag is the op's -- an empty-canvas drag orbits, a
                // handle drag drives the value, exactly like default
                // bindings where middle-orbit stays live during an op.
                // Sketch mode keeps the blanket: the rubber-band preview
                // owns the cursor with no handle to test against.
                if (leftIsCamera && !shiftPanGesture) {
                    if (m_inSketchMode) suppressCamDrag = true;
                    else if (toolWantsDrag) suppressCamDrag = gizmoOwnsDrag;
                }
                // Alt+Left-drag is reserved for box-select when left IS the camera
                // (trackpad / left-orbit mode) — Alt is otherwise unused, and Shift
                // is already pan. Don't let it orbit; the box-select code below
                // claims the drag. Only in the bare 3D view (no sketch/op running).
                if (leftIsCamera && io.KeyAlt && !m_inSketchMode && !m_extrudeCtl.active() &&
                    !m_ppCtl.active() && !m_edgeCtl.active() && !m_gizmoDragging)
                    suppressCamDrag = true;
            }
            // Pan depth anchor: Camera::pan is exact 1:1 screen tracking, but in
            // perspective "one pixel's world size" depends on DEPTH — and the
            // camera target is a bad proxy for it on large projects (cursor-zoom
            // leaves it metres from, or millimetres in front of, the geometry on
            // screen; that's what made pan twitchy up close and frozen far out).
            // Anchor each pan gesture to the hover pick from the mouse-down
            // frame instead — the same cached pick cursor-zoom reuses — so the
            // point you grab moves with the cursor. No fresh hit (empty space)
            // → -1 → Camera falls back to the target distance. The anchor is
            // captured ONCE per button-hold: picking pauses during camera drags,
            // and mid-drag the grabbed content stays at the same depth anyway
            // (pan is a screen-parallel translation).
            if (!ImGui::IsMouseDown(m_orbitButton) &&
                !ImGui::IsMouseDown(m_panButton))
                m_panAnchorHeld = false;
            if (!ImGui::IsMouseDown(m_orbitButton)) m_orbitAnchorHeld = false;
            auto anchoredPan = [&](float dx, float dy) {
                if (!m_panAnchorHeld) {
                    m_panAnchorHeld = true;
                    float d = -1.0f;
                    if (m_zoomFocusHit && m_zoomFocusFrame >= 0 &&
                        ImGui::GetFrameCount() - m_zoomFocusFrame <= 3)
                        d = glm::length(m_zoomFocusPoint - cam.getPosition());
                    cam.setPanRefDist(d);
                }
                cam.pan(dx, dy);
            };
            if (!suppressCamDrag && ImGui::IsMouseDragging(m_orbitButton)) {
                ImVec2 delta = io.MouseDelta;
                if (io.KeyShift) anchoredPan(delta.x, delta.y);
                else {
                    // Re-anchor the orbit pivot onto the geometry near the VIEW
                    // CENTRE, once per gesture, so the orbit spins around the
                    // object instead of a point that drifted behind it. Moving
                    // the target is seamless (a tiny mouse delta preserves the
                    // camera position; only the pivot relocates). Desktop only;
                    // a total miss (nothing near centre) leaves the target as-is.
                    if (!m_orbitAnchorHeld) {
                        m_orbitAnchorHeld = true;
                        if (!materializr::touchMode() && m_picker && m_document) {
                            auto pk = [&](float sx, float sy, glm::vec3& out) -> bool {
                                try {
                                    auto r = m_picker->pick(sx, sy, contentSize.x,
                                                            contentSize.y, cam, *m_document);
                                    if (r.hit) { out = r.hitPoint; return true; }
                                } catch (...) {}
                                return false;
                            };
                            const float cx = contentSize.x * 0.5f;
                            const float cy = contentSize.y * 0.5f;
                            glm::vec3 pivot;
                            bool got = pk(cx, cy, pivot); // dead-centre wins outright
                            if (!got) {
                                // Centre is over a gap: sample expanding rings and
                                // AVERAGE the first ring that hits. One part flanking
                                // the centre → pivot leans to it (nearest); two parts
                                // flanking a gap → pivot lands between them (the
                                // middle-ground blend).
                                const float radii[] = {40.0f, 85.0f, 150.0f};
                                for (float rad : radii) {
                                    glm::vec3 sum(0.0f); int n = 0;
                                    for (int a = 0; a < 8; ++a) {
                                        float ang = 6.2831853f * a / 8.0f;
                                        glm::vec3 h;
                                        if (pk(cx + rad * std::cos(ang),
                                               cy + rad * std::sin(ang), h)) {
                                            sum += h; ++n;
                                        }
                                    }
                                    if (n > 0) { pivot = sum / float(n); got = true; break; }
                                }
                            }
                            if (got) cam.setTarget(pivot);
                        }
                    }
                    // Touch orbit honours the user's sensitivity slider; desktop
                    // mouse orbit is unscaled (factor 1).
                    const float os = materializr::touchMode() ? m_touchOrbitSens : 1.0f;
                    cam.orbit(delta.x * os, delta.y * os);
                }
            }
            if (!suppressCamDrag && m_panButton != m_orbitButton &&
                ImGui::IsMouseDragging(m_panButton)) {
                anchoredPan(io.MouseDelta.x, io.MouseDelta.y);
            }

            if (materializr::touchMode() && m_window) {
                // Two-finger touch gestures (recognised in Window::pollEvents):
                // centroid movement pans, pinch zooms. Applied here so they share
                // the viewport-hovered gate and gizmo-ownership suppression above.
                float tdx = 0.0f, tdy = 0.0f, tdz = 0.0f;
                // Pan: 1:1 (content glued to the fingers, like scrolling a web
                // page — Camera::pan is pixel-exact now), with a small deadzone
                // so two-finger jitter doesn't creep the view. The old 0.275
                // damping compensated for the legacy distance-fraction pan
                // being several times faster than 1:1; with exact tracking the
                // baseline is 1.0 and the slider scales from there.
                if (!gizmoOwnsDrag && m_window->consumeTouchPan(tdx, tdy)) {
                    if (std::fabs(tdx) > 0.5f || std::fabs(tdy) > 0.5f) {
                        const int fc = ImGui::GetFrameCount();
                        if (fc - m_lastTouchPanFrame > 10) {
                            // New two-finger gesture. Touch has no hover pick to
                            // reuse, so ray-cast the viewport centre once per
                            // gesture (not per frame — the picker walk is the
                            // dominant per-frame cost on dense scenes) to anchor
                            // the pan depth to the content actually in view.
                            float d = -1.0f;
                            try {
                                auto r = m_picker->pick(contentSize.x * 0.5f,
                                                        contentSize.y * 0.5f,
                                                        contentSize.x, contentSize.y,
                                                        cam, *m_document);
                                if (r.hit)
                                    d = glm::length(r.hitPoint - cam.getPosition());
                            } catch (...) {}
                            cam.setPanRefDist(d);
                        }
                        m_lastTouchPanFrame = fc;
                        cam.pan(tdx * m_touchPanSens, tdy * m_touchPanSens);
                    }
                }
                // Pinch zoom: continuous (fires every frame), so a fraction of a
                // wheel tick. Deadzoned to keep a pure pan from zooming.
                if (m_window->consumeTouchZoom(tdz)) {
                    // Base 0.015 = old 0.006 × 2.5: the faster zoom that used to
                    // need a 2.5x slider is now the 1.0x baseline (raw zoom was
                    // painfully slow). Slider still scales from here.
                    if (std::fabs(tdz) > 1.5f) cam.zoom(tdz * 0.015f * m_touchZoomSens);
                }
            }

            // Pause interactive operations while a camera button is also being
            // dragged — otherwise the changing view matrix re-projects the same
            // mouse motion onto a moving target each frame and the value jolts.
            // suppressCamDrag means the left-drag is NOT orbiting the camera (in
            // sketch mode it's drawing the rubber-band). When it's suppressed the
            // view isn't moving, so this must read false — otherwise it would gate
            // off the sketch input block and freeze the live preview a few px into
            // the drag (once IsMouseDragging crosses its threshold). Outside sketch
            // mode suppressCamDrag == gizmoOwnsDrag, so this changes nothing there.
            bool camDragging = !gizmoOwnsDrag && !suppressCamDrag &&
                (ImGui::IsMouseDragging(m_orbitButton) ||
                 (m_panButton != m_orbitButton && ImGui::IsMouseDragging(m_panButton)));

            // Pattern axis-origin picker. While the user is picking the
            // axis origin in the radial-pattern popup, we project the mouse
            // ray onto the world plane PERPENDICULAR to the chosen rotation
            // axis (so e.g. Z-axis rotation picks on the floor XY plane).
            // Coordinates snap to the current sketch grid step; the chosen
            // axis-coordinate is forced to 0 so the axis line passes through
            // the world origin in that dimension. A yellow dot follows the
            // cursor; left-click commits the origin and exits pick mode.
            if (m_patternActive && m_patternPickingOrigin && !camDragging) {
                ImVec2 mp = ImGui::GetMousePos();
                ImVec2 wp = ImGui::GetItemRectMin();
                glm::mat4 invVP = glm::inverse(proj * view);
                float nx = ((mp.x - wp.x) / contentSize.x) * 2.0f - 1.0f;
                float ny = 1.0f - ((mp.y - wp.y) / contentSize.y) * 2.0f;
                glm::vec4 np = invVP * glm::vec4(nx, ny, -1.0f, 1.0f);
                glm::vec4 fp = invVP * glm::vec4(nx, ny,  1.0f, 1.0f);
                glm::vec3 ro(np / np.w);
                glm::vec3 rd = glm::normalize(glm::vec3(fp / fp.w) - ro);
                // Picker plane = perpendicular to the rotation axis. Use the
                // user-axis → world mapping so "Z" picks on the floor (XZ in
                // Y-up world) which is what the user expects.
                glm::vec3 planeN = userAxisToWorldVec(m_patternAxisIdx);
                int worldIdx = userAxisToWorldIdx(m_patternAxisIdx);
                float denom = glm::dot(rd, planeN);
                if (std::abs(denom) > 1e-6f) {
                    float t = -glm::dot(ro, planeN) / denom;
                    if (t > 0.0f) {
                        glm::vec3 hit = ro + rd * t;
                        float step = std::max(m_sketchGridStep, 0.01f);
                        hit.x = std::round(hit.x / step) * step;
                        hit.y = std::round(hit.y / step) * step;
                        hit.z = std::round(hit.z / step) * step;
                        hit[worldIdx] = 0.0f;
                        // Project the snapped 3D point back to screen for the dot.
                        glm::vec4 clip = (proj * view) * glm::vec4(hit, 1.0f);
                        auto* dl = ImGui::GetForegroundDrawList();

                        // Picker plane grid: faint lines at the snap step,
                        // drawn on the same plane the dot snaps to. Lets the
                        // user see where the snap points are. Capped at
                        // 50 lines per direction so a fine snap step doesn't
                        // produce a wall of pixels.
                        auto worldToScreen = [&](const glm::vec3& p, ImVec2& out) -> bool {
                            glm::vec4 c = (proj * view) * glm::vec4(p, 1.0f);
                            if (c.w <= 0.0f) return false;
                            glm::vec3 ndc(c / c.w);
                            out = ImVec2(wp.x + (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                         wp.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * contentSize.y);
                            return true;
                        };
                        // Two in-plane unit vectors. Choose by which world
                        // axis is the plane normal so they're axis-aligned.
                        glm::vec3 axU, axV;
                        if (worldIdx == 0)      { axU = glm::vec3(0,1,0); axV = glm::vec3(0,0,1); }
                        else if (worldIdx == 1) { axU = glm::vec3(1,0,0); axV = glm::vec3(0,0,1); }
                        else                    { axU = glm::vec3(1,0,0); axV = glm::vec3(0,1,0); }
                        int half = std::min(50, static_cast<int>(50.0f / step));
                        float halfRange = half * step;
                        for (int i = -half; i <= half; ++i) {
                            float t = i * step;
                            glm::vec3 p0 = t * axU - halfRange * axV;
                            glm::vec3 p1 = t * axU + halfRange * axV;
                            glm::vec3 q0 = -halfRange * axU + t * axV;
                            glm::vec3 q1 =  halfRange * axU + t * axV;
                            ImVec2 s0, s1, t0, t1;
                            // Axes through origin pop in yellow; the surrounding
                            // grid is medium-grey at clearly-visible alpha.
                            ImU32 col = (i == 0) ? IM_COL32(255, 220, 50, 220)
                                                 : IM_COL32(180, 180, 180, 160);
                            float thick = (i == 0) ? 1.5f : 1.0f;
                            if (worldToScreen(p0, s0) && worldToScreen(p1, s1))
                                dl->AddLine(s0, s1, col, thick);
                            if (worldToScreen(q0, t0) && worldToScreen(q1, t1))
                                dl->AddLine(t0, t1, col, thick);
                        }

                        if (clip.w > 0.0f) {
                            glm::vec3 ndc(clip / clip.w);
                            ImVec2 sp(wp.x + (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                      wp.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * contentSize.y);
                            dl->AddCircleFilled(sp, 7.0f, IM_COL32(255, 220, 50, 255));
                            dl->AddCircle      (sp, 7.0f, IM_COL32(0, 0, 0, 255), 0, 1.5f);
                            // Coord label next to the dot.
                            char buf[64];
                            std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)",
                                          hit.x, hit.y, hit.z);
                            dl->AddText(ImVec2(sp.x + 10.0f, sp.y - 8.0f),
                                        IM_COL32(255, 220, 50, 255), buf);
                        }
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            m_patternOriginX = hit.x;
                            m_patternOriginY = hit.y;
                            m_patternOriginZ = hit.z;
                            m_patternPickingOrigin = false;
                            updatePattern();
                        }
                    }
                }
                // While picking, suppress the rest of the viewport input so
                // the click doesn't also fire a body pick / gizmo drag.
                // We do that by returning from this section after handling.
                // (Caller still does the camera + UI rendering paths.)
            }

            // Shared by the exact drag→distance mapping below: viewport size in
            // points (the units MouseDelta uses) and the view direction for the
            // head-on sensitivity clamp.
            const glm::vec2 vpPx(contentSize.x, contentSize.y);
            const glm::vec3 viewDirW =
                glm::normalize(cam.getTarget() - cam.getPosition());

            // (Interactive extrude's drag is ExtrudeController::onViewportInput,
            // dispatched with the other controllers' below.)

            // Push/Pull face arrow: left-drag moves the distance along the face normal.
            // Scale Face gizmo input: press near a handle tip claims it,
            // dragging projects onto that axis and scales proportionally
            // to the face's half-extent.
            // Controller handle input. Generic over m_iops: each controller
            // runs its own hit-test and drag through IopViewport, so adding an
            // op with a gizmo no longer means adding a block here.
            {
                IopViewport vp;
                vp.toScreen = [&](const glm::vec3& w, glm::vec2& out) {
                    glm::vec4 clip = proj * view * glm::vec4(w, 1.0f);
                    if (clip.w <= 1e-6f) return false;
                    glm::vec3 ndc = glm::vec3(clip) / clip.w;
                    ImVec2 wp = ImGui::GetItemRectMin();
                    out = glm::vec2(wp.x + (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                    wp.y + (0.5f - ndc.y * 0.5f) * contentSize.y);
                    return true;
                };
                vp.dragAlongAxis = [&](const glm::vec3& origin,
                                       const glm::vec3& axis,
                                       const glm::vec2& d) {
                    return projectDragOntoNormal(origin, axis,
                                                 glm::vec2(d.x, d.y),
                                                 proj * view, vpPx, viewDirW);
                };
                const ImVec2 mp = ImGui::GetMousePos();
                vp.mouse = glm::vec2(mp.x, mp.y);
                vp.mouseDelta = glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
                vp.clicked  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                vp.dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left);
                vp.released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
                vp.down     = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                // Trackpad mode: orbit AND pan are both on Left, so a handle
                // can only be worked click-move-click (Push/Pull's sticky
                // drag). Never on touch — a tap would toggle sticky on AND
                // feed the direct drag, doubling the distance.
                vp.trackpadInput = (m_orbitButton == ImGuiMouseButton_Left &&
                                    m_panButton   == ImGuiMouseButton_Left) &&
                                   !materializr::touchMode();
                vp.uiCaptured = io.WantCaptureMouse;
                {
                    // Cursor pick ray + camera frame (Move Face's ring
                    // latch and plane intersection work off these).
                    const ImVec2 rwp = ImGui::GetItemRectMin();
                    float nx = ((mp.x - rwp.x) / contentSize.x) * 2.0f - 1.0f;
                    float ny = 1.0f - ((mp.y - rwp.y) / contentSize.y) * 2.0f;
                    glm::mat4 invVP = glm::inverse(proj * view);
                    glm::vec4 np = invVP * glm::vec4(nx, ny, -1.0f, 1.0f);
                    glm::vec4 fp = invVP * glm::vec4(nx, ny,  1.0f, 1.0f);
                    vp.rayOrigin = glm::vec3(np / np.w);
                    vp.rayDir = glm::normalize(glm::vec3(fp / fp.w) - vp.rayOrigin);
                    vp.camPos = glm::vec3(cam.getPosition());
                    vp.camTarget = glm::vec3(cam.getTarget());
                }
                for (auto* c : m_iops) {
                    if (!c->active() || !c->wantsViewportInput()) continue;
                    // Release still has to reach a latched handle even while
                    // the camera is dragging, or the handle stays stuck down.
                    if (camDragging && !c->draggingHandle()) continue;
                    c->onViewportInput(vp, iopContext());
                }
            }


            // Gizmo input + Face hover highlighting + picking (suppressed while an
            // interactive op owns the left-drag: extrude, push/pull, fillet/chamfer,
            // or the pattern axis-origin picker).
            if (!m_inSketchMode && !m_extrudeCtl.active() && !m_ppCtl.active() && !m_edgeCtl.active() &&
                !anyIopWantsViewportInput() &&
                !(m_patternActive && m_patternPickingOrigin)) {
                ImVec2 mousePos = ImGui::GetMousePos();
                ImVec2 winPos = ImGui::GetItemRectMin();
                float localX = mousePos.x - winPos.x;
                float localY = mousePos.y - winPos.y;

                // Gizmo interaction takes priority
                bool gizmoConsumedInput = false;
                if (m_gizmo->isVisible()) {
                    bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                    bool mouseJustPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

                    GizmoResult gResult = m_gizmo->handleInput(
                        localX, localY, contentSize.x, contentSize.y,
                        mouseDown, mouseJustPressed, cam);

                    // Helpers shared by drag-apply and commit.
                    auto axisDirOf = [](GizmoAxis a) -> glm::vec3 {
                        if (a == GizmoAxis::X) return glm::vec3(1, 0, 0);
                        if (a == GizmoAxis::Y) return glm::vec3(0, 1, 0);
                        return glm::vec3(0, 0, 1);
                    };
                    // Angle snap policy:
                    //  - snap-to-grid OFF: free rotation with a 7° soft-snap
                    //    near every 45° tick, so the user can land squarely
                    //    on canonical angles without typing.
                    //  - snap-to-grid ON: hard 15° increments, matching the
                    //    rest of the app's angle-snap behaviour (line-draw
                    //    angle snap, sketch rotate popup, etc.).
                    auto softSnap45 = [this](float deg) {
                        // Construction-plane drags need finer granularity —
                        // plane orientation is often a precise angle (15°
                        // chamfer-line, 30° draft, etc.). Use 5° hard snap
                        // when snap is on and a soft 15° anchor otherwise,
                        // versus the body/sketch defaults of 15°/45°.
                        const bool planeDrag = !m_planeGizmoDrag.empty();
                        if (m_snapToGrid) {
                            float step = planeDrag ? 5.0f : 15.0f;
                            return std::round(deg / step) * step;
                        }
                        float anchor = planeDrag ? 15.0f : 45.0f;
                        float n = std::round(deg / anchor) * anchor;
                        return (std::abs(deg - n) < 3.0f) ? n : deg;
                    };

                    // Start drag: save originals for every selected body (so Move
                    // can apply to all of them) and reset accumulators. When the
                    // selection has no bodies but does have standalone sketches,
                    // capture their before-planes instead — the per-frame drag
                    // path below mutates plane(s); on release we push one
                    // SketchTransformOp per dragged sketch.
                    if (gResult.activeAxis != GizmoAxis::None && !m_gizmoDragging) {
                        m_gizmoDragOriginals.clear();
                        m_sketchGizmoDragSketches.clear();
                        m_planeGizmoDrag.clear();
                        m_axisGizmoDrag.clear();
                        auto addedSketch = [&](int sid) {
                            for (auto& [eid, _] : m_sketchGizmoDragSketches)
                                if (eid == sid) return true;
                            return false;
                        };
                        auto addedPlane = [&](int pid) {
                            for (auto& [eid, _] : m_planeGizmoDrag)
                                if (eid == pid) return true;
                            return false;
                        };
                        for (const auto& sel : m_selection->getSelection()) {
                            if (sel.type == SelectionType::Body) {
                                try {
                                    m_gizmoDragOriginals.push_back(
                                        {sel.bodyId, m_document->getBody(sel.bodyId)});
                                } catch (...) {}
                            } else if ((sel.type == SelectionType::Sketch ||
                                        sel.type == SelectionType::SketchRegion) &&
                                       sel.sketchId >= 0) {
                                // Region picks count as picks of their parent
                                // sketch; dedup so two regions of the same
                                // sketch don't double-transform it.
                                if (addedSketch(sel.sketchId)) continue;
                                auto sk = m_document->getSketch(sel.sketchId);
                                if (sk) {
                                    m_sketchGizmoDragSketches.push_back(
                                        {sel.sketchId, sk->getPlane()});
                                }
                            } else if (sel.type == SelectionType::Plane &&
                                       sel.planeId >= 0) {
                                if (addedPlane(sel.planeId)) continue;
                                const auto* entry = m_document->getPlane(sel.planeId);
                                if (entry) {
                                    m_planeGizmoDrag.push_back(
                                        {sel.planeId, entry->plane});
                                }
                            } else if (sel.type == SelectionType::Axis &&
                                       sel.axisId >= 0) {
                                bool dup = false;
                                for (auto& e : m_axisGizmoDrag)
                                    if (e.id == sel.axisId) { dup = true; break; }
                                if (dup) continue;
                                const auto* entry = m_document->getAxis(sel.axisId);
                                if (entry) {
                                    m_axisGizmoDrag.push_back(
                                        {sel.axisId, entry->origin, entry->direction});
                                }
                            }
                        }
                        if (!m_gizmoDragOriginals.empty() ||
                            !m_sketchGizmoDragSketches.empty() ||
                            !m_planeGizmoDrag.empty() ||
                            !m_axisGizmoDrag.empty()) {
                            if (!m_gizmoDragOriginals.empty()) {
                                m_gizmoDragBodyId = m_gizmoDragOriginals.front().first;
                                m_gizmoDragOriginalShape = m_gizmoDragOriginals.front().second;
                            } else {
                                m_gizmoDragBodyId = -1;
                                m_gizmoDragOriginalShape = TopoDS_Shape();
                            }
                            // Primary body's bbox, captured ONCE — the original
                            // never changes during the drag, and BRepBndLib per
                            // frame is 50-150 ms on a complex body (the Scale
                            // branch reads the diagonal every drag frame). The
                            // sketch-only fallback (pivot, zero extent) is
                            // applied per-frame below where the pivot exists.
                            m_gizmoDragBBoxMin = glm::vec3(0.0f);
                            m_gizmoDragBBoxMax = glm::vec3(0.0f);
                            if (!m_gizmoDragOriginalShape.IsNull()) {
                                try {
                                    Bnd_Box ob;
                                    BRepBndLib::Add(m_gizmoDragOriginalShape, ob);
                                    if (!ob.IsVoid()) {
                                        double x1,y1,z1,x2,y2,z2;
                                        ob.Get(x1,y1,z1,x2,y2,z2);
                                        m_gizmoDragBBoxMin = glm::vec3(x1,y1,z1);
                                        m_gizmoDragBBoxMax = glm::vec3(x2,y2,z2);
                                    }
                                } catch (...) {}
                            }
                            m_gizmoDragging = true;
                            m_gizmoTotalDelta = glm::vec3(0.0f);
                            m_gizmoPreviewXf = glm::mat4(1.0f);
                            m_gizmoTotalAngle = 0.0f;
                            m_gizmoScaleAccum = glm::vec3(0.0f);
                            m_gizmoTotalScale = glm::vec3(1.0f);
                            // Pre-compute the shared pivot once — the originals
                            // don't change during the drag, so this is constant.
                            // For sketch-only drag we use the GEOMETRIC
                            // CENTROID of each sketch's points (mapped through
                            // its plane), then average across sketches — same
                            // pivot the gizmo's display uses. This makes
                            // Rotate spin the sketch in place instead of
                            // around a remote plane-anchor point.
                            m_gizmoSharedPivot = glm::vec3(0.0f);
                            // Track lowest world-Y across the drag set so the
                            // translate readout can report bottom-Z (user
                            // convention) instead of centre-Z. Seeded with
                            // FLT_MAX; falls back to pivot.y at the bottom
                            // of this block if nothing valid contributed.
                            float bottomY = FLT_MAX;
                            int np = 0;
                            for (auto& [sid, plnBefore] : m_sketchGizmoDragSketches) {
                                auto sk = m_document->getSketch(sid);
                                glm::vec3 ctr;
                                if (sk && !sk->getPoints().empty()) {
                                    glm::vec2 mn2( FLT_MAX,  FLT_MAX), mx2(-FLT_MAX, -FLT_MAX);
                                    for (const auto& p : sk->getPoints()) {
                                        mn2 = glm::min(mn2, p.pos); mx2 = glm::max(mx2, p.pos);
                                    }
                                    glm::vec2 c2 = (mn2 + mx2) * 0.5f;
                                    const gp_Ax3& ax = plnBefore.Position();
                                    glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                                    glm::vec3 X(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                                    glm::vec3 Y(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                                    ctr = O + X * c2.x + Y * c2.y;
                                } else {
                                    const gp_Pnt& o = plnBefore.Position().Location();
                                    ctr = glm::vec3(o.X(), o.Y(), o.Z());
                                }
                                m_gizmoSharedPivot += ctr;
                                ++np;
                            }
                            for (auto& [id, orig] : m_gizmoDragOriginals) {
                                try {
                                    Bnd_Box bb; BRepBndLib::Add(orig, bb);
                                    if (bb.IsVoid()) continue;
                                    double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
                                    m_gizmoSharedPivot += glm::vec3((x0+x1)*0.5f, (y0+y1)*0.5f, (z0+z1)*0.5f);
                                    if (static_cast<float>(y0) < bottomY) bottomY = static_cast<float>(y0);
                                    ++np;
                                } catch (...) {}
                            }
                            // Construction planes pivot at the plane's origin
                            // (no point cloud to centroid). Rotation thus
                            // spins the plane around its own anchor — what
                            // you'd expect when nudging a placement plane.
                            for (auto& [pid, plnBefore] : m_planeGizmoDrag) {
                                const gp_Pnt& o = plnBefore.Position().Location();
                                m_gizmoSharedPivot += glm::vec3(o.X(), o.Y(), o.Z());
                                ++np;
                            }
                            for (auto& a : m_axisGizmoDrag) {
                                m_gizmoSharedPivot += glm::vec3(a.origin.X(),
                                                                 a.origin.Y(),
                                                                 a.origin.Z());
                                ++np;
                            }
                            if (np > 0) m_gizmoSharedPivot /= static_cast<float>(np);
                            m_gizmoSharedBottomY = (bottomY < FLT_MAX) ? bottomY
                                                                       : m_gizmoSharedPivot.y;
                        }
                    }

                    // During drag: accumulate totals and (re)apply to the ORIGINAL
                    // shape each frame, so snapping and per-axis scale stay stable.
                    if (gResult.changed && m_gizmoDragging) {
                        try {
                            // BBox of the primary body — only valid when at least
                            // one body is in the drag. For sketch-only drag we
                            // fall back to a zero-extent box centred on the
                            // gizmo pivot so the Scale branch's `os` ends up at
                            // 1 (Scale isn't meaningful for a sketch's plane
                            // anyway — translate and rotate are the supported
                            // modes for the sketch-only path below).
                            // Primary bbox from the drag-start capture — see
                            // the drag-start block; recomputing BRepBndLib per
                            // frame was a big slice of the drag lag on complex
                            // bodies. Sketch-only drag: zero-extent at the
                            // pivot so the Scale branch's `os` ends up 1.
                            double ox1,oy1,oz1,ox2,oy2,oz2;
                            if (!m_gizmoDragOriginalShape.IsNull()) {
                                ox1 = m_gizmoDragBBoxMin.x; oy1 = m_gizmoDragBBoxMin.y;
                                oz1 = m_gizmoDragBBoxMin.z;
                                ox2 = m_gizmoDragBBoxMax.x; oy2 = m_gizmoDragBBoxMax.y;
                                oz2 = m_gizmoDragBBoxMax.z;
                            } else {
                                ox1 = ox2 = m_gizmoSharedPivot.x;
                                oy1 = oy2 = m_gizmoSharedPivot.y;
                                oz1 = oz2 = m_gizmoSharedPivot.z;
                            }

                            if (gResult.mode == GizmoMode::Translate) {
                                m_gizmoTotalDelta += gResult.delta;
                                glm::vec3 d = m_gizmoTotalDelta;
                                if (m_snapToGrid && m_sketchGridStep > 0.0f) {
                                    // Absolute-position snap: the pivot lands on
                                    // grid intersections (matches sketch grid
                                    // behaviour). Snap ONLY the axes that moved —
                                    // an axis-constrained drag (e.g. Y only)
                                    // leaves the other components ~0, and snapping
                                    // those too yanked a resting off-grid X/Z onto
                                    // the grid, drifting the body sideways (bug #6).
                                    float step = m_sketchGridStep;
                                    glm::vec3 absAfter = m_gizmoSharedPivot + d;
                                    auto s = [&](float v){ return std::round(v/step)*step; };
                                    const float eps = 1e-5f;
                                    if (std::abs(d.x) > eps) d.x = s(absAfter.x) - m_gizmoSharedPivot.x;
                                    if (std::abs(d.y) > eps) d.y = s(absAfter.y) - m_gizmoSharedPivot.y;
                                    if (std::abs(d.z) > eps) d.z = s(absAfter.z) - m_gizmoSharedPivot.z;
                                }
                                gp_Trsf trsf; trsf.SetTranslation(gp_Vec(d.x, d.y, d.z));
                                // GPU-only preview: push the translation as a
                                // model matrix onto the dragged bodies' mesh
                                // slots. No document write, no re-tessellation,
                                // no edge re-discretization — a drag frame on a
                                // dense body costs uniform updates instead of a
                                // remesh (the "moving one part lags on complex
                                // projects" report). The real transform lands
                                // once, on release.
                                gizmoPreviewApply(
                                    glm::translate(glm::mat4(1.0f), d));
                                // Standalone sketches in the drag: transform
                                // each one's plane from its captured before-
                                // plane so the live preview shows the new
                                // pose; final SketchTransformOp pushed on
                                // release will replay the same transform.
                                for (auto& [sid, plnBefore] : m_sketchGizmoDragSketches) {
                                    auto sk = m_document->getSketch(sid);
                                    if (!sk) continue;
                                    gp_Pln pln = plnBefore;
                                    pln.Transform(trsf);
                                    sk->setPlane(pln);
                                }
                                // Construction planes ride the same drag.
                                // No history op yet (placement-mode write-
                                // back); plane edits land directly via
                                // Document::setPlane and the renderer's
                                // PlaneChangedEvent subscriber refreshes.
                                for (auto& [pid, plnBefore] : m_planeGizmoDrag) {
                                    gp_Pln pln = plnBefore;
                                    pln.Transform(trsf);
                                    m_document->setPlane(pid, pln);
                                }
                                // Construction axes: translate the origin
                                // point only (direction is invariant under
                                // translation). Same direct-write pattern.
                                for (auto& a : m_axisGizmoDrag) {
                                    gp_Pnt newO(a.origin.X() + d.x,
                                                a.origin.Y() + d.y,
                                                a.origin.Z() + d.z);
                                    m_document->setAxis(a.id, newO, a.direction);
                                }
                            } else if (gResult.mode == GizmoMode::Rotate) {
                                glm::vec3 ad = axisDirOf(gResult.activeAxis);
                                m_gizmoRotAxis = ad;
                                m_gizmoTotalAngle += glm::dot(gResult.delta, ad);
                                float ang = softSnap45(m_gizmoTotalAngle);
                                // Pivot was captured once at drag start —
                                // reusing it avoids 65 bbox computations per
                                // frame for a large multi-selection.
                                const glm::vec3& pivot = m_gizmoSharedPivot;
                                gp_Trsf trsf;
                                trsf.SetRotation(gp_Ax1(gp_Pnt(pivot.x, pivot.y, pivot.z),
                                                        gp_Dir(ad.x, ad.y, ad.z)),
                                                 ang * M_PI / 180.0);
                                // GPU-only preview — see the Translate branch.
                                glm::mat4 pm(1.0f);
                                pm = glm::translate(pm, pivot);
                                pm = glm::rotate(pm, glm::radians(ang), ad);
                                pm = glm::translate(pm, -pivot);
                                gizmoPreviewApply(pm);
                                // Same rotation applied to each sketch plane.
                                for (auto& [sid, plnBefore] : m_sketchGizmoDragSketches) {
                                    auto sk = m_document->getSketch(sid);
                                    if (!sk) continue;
                                    gp_Pln pln = plnBefore;
                                    pln.Transform(trsf);
                                    sk->setPlane(pln);
                                }
                                // And to each selected construction plane.
                                for (auto& [pid, plnBefore] : m_planeGizmoDrag) {
                                    gp_Pln pln = plnBefore;
                                    pln.Transform(trsf);
                                    m_document->setPlane(pid, pln);
                                }
                            } else { // Scale — per-axis, non-uniform about the centre
                                float os = static_cast<float>(glm::length(
                                    glm::vec3(ox2-ox1, oy2-oy1, oz2-oz1)));
                                if (os < 0.001f) os = 1.0f;
                                int ai = gResult.activeAxis == GizmoAxis::X ? 0
                                       : gResult.activeAxis == GizmoAxis::Y ? 1 : 2;
                                m_gizmoScaleAccum[ai] += (ai==0?gResult.delta.x:ai==1?gResult.delta.y:gResult.delta.z);
                                if (m_scaleUniform) {
                                    float f = glm::clamp(1.0f + m_gizmoScaleAccum[ai]/os, 0.05f, 20.0f);
                                    f = std::round(f * 100.0f) / 100.0f;
                                    m_gizmoTotalScale = glm::vec3(f);
                                } else {
                                    for (int k = 0; k < 3; ++k) {
                                        float f = glm::clamp(1.0f + m_gizmoScaleAccum[k]/os, 0.05f, 20.0f);
                                        m_gizmoTotalScale[k] = std::round(f * 100.0f) / 100.0f;
                                    }
                                }
                                // Cached pivot — see Rotate branch above.
                                const glm::vec3& pivot = m_gizmoSharedPivot;
                                // GPU-only preview — see the Translate branch.
                                // Same affine map as the commit's gp_GTrsf:
                                // x' = pivot + S * (x - pivot).
                                glm::mat4 pm(1.0f);
                                pm = glm::translate(pm, pivot);
                                pm = glm::scale(pm, m_gizmoTotalScale);
                                pm = glm::translate(pm, -pivot);
                                gizmoPreviewApply(pm);
                            }
                        } catch (...) {}
                        gizmoConsumedInput = true;
                    }

                    // End drag: commit the right operation for the gizmo's mode.
                    // Single body -> a TransformOp (parameters stay editable in
                    // the Properties panel). Multi-body -> a single batched
                    // ReplayOp snapshot so the history shows one entry, not one
                    // per body.
                    if (m_gizmoDragging && gResult.activeAxis == GizmoAxis::None && !mouseDown) {
                        // The live drag was GPU-only (model matrices on the mesh
                        // slots; the document never moved). Reset the matrices
                        // first — the ops below apply the REAL transform to the
                        // document and the partial remesh redraws the bodies at
                        // their committed pose.
                        gizmoPreviewReset();
                        try {
                            GizmoMode gm = m_gizmo->getMode();
                            const size_t nBodies = m_gizmoDragOriginals.size();
                            const bool isMulti = nBodies > 1;

                            // Defensive: the document should already hold the
                            // originals (the GPU preview never wrote to it), but
                            // restore anyway so any TransformOp captures the
                            // right previousShape on execute() even if some
                            // path did write through.
                            for (auto& [id, orig] : m_gizmoDragOriginals) {
                                m_document->updateBody(id, orig);
                            }
                            // Sketch planes WERE live-written during the drag —
                            // restore before the SketchTransformOps run their
                            // own execute() with the cumulative gp_Trsf.
                            for (auto& [sid, plnBefore] : m_sketchGizmoDragSketches) {
                                auto sk = m_document->getSketch(sid);
                                if (sk) sk->setPlane(plnBefore);
                            }

                            // ---- Parametric link classification (link model) ----
                            // A 3D move breaks the parametric link and is applied as
                            // a RIGID transform: the body translates/rotates as-is
                            // (fillets, chamfers and features from other sketches all
                            // ride along), which never fails — unlike re-deriving the
                            // body at a new position, which can't re-attach a fillet
                            // or a cut from an un-moved sketch. So a moved sketch (and
                            // any sketch driving a moved body) is detached; parametric
                            // editing stays available on sketches you DON'T move.
                            std::set<int> inDragBodies, inDragSketches;
                            for (auto& [id, _] : m_gizmoDragOriginals) inDragBodies.insert(id);
                            for (auto& [sid, _] : m_sketchGizmoDragSketches) inDragSketches.insert(sid);
                            std::map<int, std::set<int>> skLinks = sketchBodyLinks();
                            std::set<int> detachSketches;  // links to break (any moved, linked sketch)
                            std::set<int> bodyAloneDetach; // detached sketches with no SketchTransformOp
                            for (int s : inDragSketches)
                                if (skLinks.count(s)) detachSketches.insert(s);
                            for (int b : inDragBodies)
                                for (auto& [s, bodies] : skLinks)
                                    if (bodies.count(b) && !inDragSketches.count(s)) {
                                        detachSketches.insert(s);
                                        bodyAloneDetach.insert(s);
                                    }
                            // Unison sketches: in-drag sketches that drive an in-drag
                            // body. On the single-body path these ride along inside the
                            // body's TransformOp (one atomic op, so the sketch always
                            // follows through undo/redo) and are skipped by the separate
                            // SketchTransformOp loop. bodyFollowHandled tracks which.
                            std::set<int> unisonSketches, bodyFollowHandled;
                            for (int s : inDragSketches) {
                                auto it = skLinks.find(s);
                                if (it == skLinks.end()) continue;
                                for (int b : it->second)
                                    if (inDragBodies.count(b)) { unisonSketches.insert(s); break; }
                            }
                            // Hybrid: a unison move on a body that can be safely
                            // re-derived (no fillet/chamfer/boolean/other feature)
                            // STAYS LINKED — the body follows by re-derivation and
                            // remains editable. A body with features can't re-bind
                            // after the move, so it moves rigidly and de-links.
                            std::set<int> rederiveSketches, rederiveBodies;
                            for (int s : unisonSketches) {
                                auto it = skLinks.find(s);
                                if (it == skLinks.end()) continue;
                                std::set<int> sBodies;
                                bool allSafe = true;
                                for (int b : it->second)
                                    if (inDragBodies.count(b)) {
                                        sBodies.insert(b);
                                        if (!bodySafelyRederivable(b, s)) allSafe = false;
                                    }
                                if (allSafe && !sBodies.empty()) {
                                    rederiveSketches.insert(s);
                                    rederiveBodies.insert(sBodies.begin(), sBodies.end());
                                }
                            }
                            // Re-derived (linked) sketches must NOT be detached.
                            for (int s : rederiveSketches) detachSketches.erase(s);

                            // Shared pivot captured once at drag start.
                            const glm::vec3& pivot = m_gizmoSharedPivot;

                            glm::vec3 d = m_gizmoTotalDelta;
                            if (gm == GizmoMode::Translate &&
                                m_snapToGrid && m_sketchGridStep > 0.0f) {
                                // Absolute snap — same rule as the live drag:
                                // snap ONLY the axes that moved, so an
                                // axis-constrained move doesn't drift the others
                                // onto the grid (bug #6).
                                float step = m_sketchGridStep;
                                glm::vec3 absAfter = m_gizmoSharedPivot + d;
                                auto s = [&](float v){ return std::round(v/step)*step; };
                                const float eps = 1e-5f;
                                if (std::abs(d.x) > eps) d.x = s(absAfter.x) - m_gizmoSharedPivot.x;
                                if (std::abs(d.y) > eps) d.y = s(absAfter.y) - m_gizmoSharedPivot.y;
                                if (std::abs(d.z) > eps) d.z = s(absAfter.z) - m_gizmoSharedPivot.z;
                            }
                            float ang = (gm == GizmoMode::Rotate)
                                            ? softSnap45(m_gizmoTotalAngle) : 0.0f;

                            bool validMove   = glm::length(d) > 1e-4f;
                            bool validRotate = std::abs(ang) > 1e-3f;
                            bool validScale  = glm::length(m_gizmoTotalScale - glm::vec3(1.0f)) > 1e-3f;
                            bool anyValid    = (gm == GizmoMode::Translate && validMove) ||
                                               (gm == GizmoMode::Rotate    && validRotate) ||
                                               (gm == GizmoMode::Scale     && validScale);

                            // Plane drags write to Document::setPlane during the
                            // live drag — no body op is needed (or correct: a
                            // TransformOp with bodyId=-1 would later crash on
                            // history replay via getBody(-1)). Skip the body /
                            // sketch commit branches entirely when only planes
                            // are in the drag.
                            const bool planeOnly = (nBodies == 0) &&
                                                   m_sketchGizmoDragSketches.empty() &&
                                                   !m_planeGizmoDrag.empty();
                            // Axis-only drags also write through Document
                            // directly during the live drag; skip the body /
                            // sketch commit branches the same way (avoids
                            // pushing a phantom TransformOp with bodyId=-1
                            // that crashed v0.6.0 for plane-only drags).
                            const bool axisOnly  = (nBodies == 0) &&
                                                   m_sketchGizmoDragSketches.empty() &&
                                                   m_planeGizmoDrag.empty() &&
                                                   !m_axisGizmoDrag.empty();
                            if (!anyValid) {
                                /* below-threshold drag: leave history alone */
                            } else if (planeOnly) {
                                // Plane gizmo wrote the new pose to Document via
                                // setPlane during the drag; record an undoable
                                // PlaneTransformOp (before = m_planeGizmoDrag's
                                // stored pose, after = current) so Ctrl+Z works.
                                // One batched op covers a multi-plane drag.
                                // A plane still being previewed by the Construction
                                // Plane popup is ALREADY in the document, so the gizmo
                                // will happily drag it -- but its ConstructionPlaneOp
                                // does not reach history until the popup is committed.
                                // Recording the move first leaves history in an order
                                // that cannot replay: move, move, then create. On the
                                // next load the moves apply and the creation then puts
                                // the plane back at its birth pose, so a plane the user
                                // positioned and saved reopens at the origin.
                                //
                                // Seen on robot dog cover.mzr: of 156 steps exactly one
                                // ran backwards in time -- the construction_plane at
                                // t=1787594362 sitting after its own moves at t=...368
                                // and t=...377, with the saved CPLANE back at (0,0,0)
                                // despite the moves recording (0,0,0) -> (-84,0,0) ->
                                // (-84,65,0).
                                //
                                // So commit the creation before recording the move. The
                                // user dragging the previewed plane is a clear enough
                                // signal they want to keep it; Esc after this undoes
                                // both steps rather than discarding the preview, which
                                // is the ordinary history behaviour.
                                if (m_planeOpActive) {
                                    std::fprintf(stderr,
                                        "[Plane] gizmo moved a plane still previewed by the "
                                        "popup - committing its creation first so history "
                                        "stays in order.\n");
                                    commitConstructionPlane();
                                }
                                if (m_history) {
                                    std::vector<PlaneTransformOp::Entry> entries;
                                    for (auto& [pid, plnBefore] : m_planeGizmoDrag) {
                                        const auto* pe = m_document->getPlane(pid);
                                        if (pe) entries.push_back({pid, plnBefore, pe->plane});
                                    }
                                    if (!entries.empty()) {
                                        const char* lbl = (gm == GizmoMode::Rotate)
                                                              ? "Rotate Plane" : "Move Plane";
                                        m_history->pushExecuted(
                                            std::make_unique<PlaneTransformOp>(
                                                lbl, std::move(entries)));
                                    }
                                }
                            } else if (axisOnly) {
                                // Axis gizmo wrote the new pose via setAxis during
                                // the drag; record an undoable AxisTransformOp
                                // (before = m_axisGizmoDrag's stored pose, after =
                                // current) so Ctrl+Z works. One batched op covers
                                // a multi-axis drag.
                                if (m_history) {
                                    std::vector<AxisTransformOp::Entry> entries;
                                    for (auto& a : m_axisGizmoDrag) {
                                        const auto* ae = m_document->getAxis(a.id);
                                        if (ae) entries.push_back({a.id, a.origin, a.direction,
                                                                   ae->origin, ae->direction});
                                    }
                                    if (!entries.empty()) {
                                        const char* lbl = (gm == GizmoMode::Rotate)
                                                              ? "Rotate Axis" : "Move Axis";
                                        m_history->pushExecuted(
                                            std::make_unique<AxisTransformOp>(
                                                lbl, std::move(entries)));
                                    }
                                }
                            } else if (isMulti) {
                                // Batched commit: one BatchTransformOp covering all
                                // bodies EXCEPT any that follow their sketch by
                                // re-derivation (those rebuild via the cascade
                                // below). A real op (not a baked ReplayOp) so it
                                // reloads editable and re-applies to the LIVE bodies
                                // on replay — see BatchTransformOp / the
                                // "batchtransform bakes" bug.
                                std::vector<int> batchIds;
                                for (auto& [id, orig] : m_gizmoDragOriginals)
                                    if (!rederiveBodies.count(id)) batchIds.push_back(id);
                                // The drag transform as a gp_GTrsf (covers rigid
                                // Move/Rotate and non-uniform Scale uniformly). The
                                // doc bodies are still at their originals (the drag
                                // preview is GPU-only), so op->execute re-applies
                                // this to them — exactly as the single-body path does.
                                gp_GTrsf batchG;
                                if (gm == GizmoMode::Scale) {
                                    batchG.SetVectorialPart(gp_Mat(
                                        m_gizmoTotalScale.x,0,0,
                                        0,m_gizmoTotalScale.y,0,
                                        0,0,m_gizmoTotalScale.z));
                                    batchG.SetTranslationPart(gp_XYZ(
                                        pivot.x - m_gizmoTotalScale.x * pivot.x,
                                        pivot.y - m_gizmoTotalScale.y * pivot.y,
                                        pivot.z - m_gizmoTotalScale.z * pivot.z));
                                } else {
                                    gp_Trsf trsf;
                                    if (gm == GizmoMode::Translate) {
                                        trsf.SetTranslation(gp_Vec(d.x, d.y, d.z));
                                    } else {
                                        trsf.SetRotation(
                                            gp_Ax1(gp_Pnt(pivot.x, pivot.y, pivot.z),
                                                   gp_Dir(m_gizmoRotAxis.x,
                                                          m_gizmoRotAxis.y,
                                                          m_gizmoRotAxis.z)),
                                            ang * M_PI / 180.0);
                                    }
                                    batchG = gp_GTrsf(trsf);
                                }
                                std::string label;
                                std::string desc;
                                if (gm == GizmoMode::Translate) {
                                    label = "Move (" + std::to_string(nBodies) + " bodies)";
                                    char buf[96];
                                    std::snprintf(buf, sizeof(buf), "Move %d bodies by %s", (int)nBodies, materializr::fmtVec3(d.x, d.y, d.z).c_str());
                                    desc = buf;
                                } else if (gm == GizmoMode::Rotate) {
                                    label = "Rotate (" + std::to_string(nBodies) + " bodies)";
                                    char buf[96];
                                    std::snprintf(buf, sizeof(buf), "Rotate %d bodies by %.1f°",
                                                  (int)nBodies, ang);
                                    desc = buf;
                                } else {
                                    label = "Scale (" + std::to_string(nBodies) + " bodies)";
                                    char buf[128];
                                    std::snprintf(buf, sizeof(buf),
                                                  "Scale %d bodies (%.0f%%, %.0f%%, %.0f%%)",
                                                  (int)nBodies,
                                                  m_gizmoTotalScale.x * 100.0f,
                                                  m_gizmoTotalScale.y * 100.0f,
                                                  m_gizmoTotalScale.z * 100.0f);
                                    desc = buf;
                                }
                                auto op = std::make_unique<BatchTransformOp>();
                                op->setBodies(std::move(batchIds));
                                op->setTransform(batchG);
                                op->setLabel(label);
                                op->setDescription(desc);
                                m_history->pushOperation(std::move(op), *m_document);
                            } else if (m_gizmoDragBodyId >= 0 &&
                                       !rederiveBodies.count(m_gizmoDragBodyId)) {
                                // Single body: keep the TransformOp path so the
                                // Properties panel still lets the user edit the
                                // translation/angle/scale after the fact.
                                // (Skipped when this body stays linked and follows
                                // its sketch via re-derivation in the cascade below.)
                                auto op = std::make_unique<TransformOp>();
                                op->setBodyId(m_gizmoDragBodyId);
                                op->setCenter(pivot.x, pivot.y, pivot.z);
                                if (gm == GizmoMode::Translate) {
                                    op->setType(TransformType::Translate);
                                    op->setTranslation(d.x, d.y, d.z);
                                } else if (gm == GizmoMode::Rotate) {
                                    op->setType(TransformType::Rotate);
                                    op->setRotation(m_gizmoRotAxis.x,
                                                    m_gizmoRotAxis.y,
                                                    m_gizmoRotAxis.z, ang);
                                } else {
                                    op->setType(TransformType::Scale);
                                    op->setScaleXYZ(m_gizmoTotalScale.x,
                                                    m_gizmoTotalScale.y,
                                                    m_gizmoTotalScale.z);
                                }
                                // Carry the body-only de-link so one undo reverts
                                // both the move and the broken link.
                                for (int s : bodyAloneDetach) op->addDetachSketch(s);
                                // Unison: any in-drag sketch driving THIS body rides
                                // along inside this op (moves + de-links atomically),
                                // so it always follows the body through undo/redo.
                                for (int s : unisonSketches) {
                                    auto it = skLinks.find(s);
                                    if (it != skLinks.end() &&
                                        it->second.count(m_gizmoDragBodyId)) {
                                        op->addFollowSketch(s);
                                        op->addDetachSketch(s);
                                        bodyFollowHandled.insert(s);
                                    }
                                }
                                m_history->pushOperation(std::move(op), *m_document);
                            }

                            // Standalone-sketch commit: push one
                            // SketchTransformOp per dragged sketch with the
                            // same cumulative gp_Trsf applied during the live
                            // drag. Scale is intentionally ignored — scaling
                            // a sketch's plane is a no-op (the plane is
                            // 2D-infinite); fall through to the body Scale
                            // branch when bodies are also present.
                            if (anyValid && !m_sketchGizmoDragSketches.empty() &&
                                (gm == GizmoMode::Translate || gm == GizmoMode::Rotate)) {
                                gp_Trsf trsf;
                                if (gm == GizmoMode::Translate) {
                                    trsf.SetTranslation(gp_Vec(d.x, d.y, d.z));
                                } else {
                                    trsf.SetRotation(
                                        gp_Ax1(gp_Pnt(pivot.x, pivot.y, pivot.z),
                                               gp_Dir(m_gizmoRotAxis.x,
                                                      m_gizmoRotAxis.y,
                                                      m_gizmoRotAxis.z)),
                                        ang * M_PI / 180.0);
                                }
                                for (auto& [sid, plnBefore] : m_sketchGizmoDragSketches) {
                                    // Unison sketch already moved atomically inside the
                                    // body's TransformOp (single-body path) — don't move
                                    // it twice.
                                    if (bodyFollowHandled.count(sid)) continue;
                                    auto op = std::make_unique<materializr::SketchTransformOp>();
                                    op->setSketch(sid);
                                    op->setTransform(trsf);
                                    // Bundle the de-link so undo reverts it too.
                                    if (detachSketches.count(sid)) op->setDetachTarget(1);
                                    m_history->pushOperation(std::move(op), *m_document);
                                }
                            }

                            // bodyAloneDetach sketches have no SketchTransformOp; the
                            // single-body TransformOp branch above already absorbed
                            // them via addDetachSketch (undoable). For the rarer
                            // multi-body path the op can't carry them, so set directly.
                            if (isMulti)
                                for (int s : bodyAloneDetach)
                                    if (auto sk = m_document->getSketch(s)) sk->setDetachedFromBody(true);
                            // Linked unison: the body's TransformOp was skipped — let it
                            // follow its now-moved sketch by re-derivation (stays editable).
                            for (int s : rederiveSketches)
                                cascadeFromSketchEdit(s);
                            if (!detachSketches.empty() || !rederiveSketches.empty())
                                markDirty();

                            // Partial remesh: only the dragged bodies changed.
                            // (cascadeFromSketchEdit marks its own re-derived
                            // bodies; sketch/plane/axis writes don't have body
                            // meshes.) The old full m_meshesDirty re-tessellated
                            // every visible body once per release — a visible
                            // hitch on a many-body project.
                            for (auto& [id, orig] : m_gizmoDragOriginals)
                                m_dirtyBodyIds.insert(id);
                        } catch (...) {}

                        m_gizmoDragging = false;
                        m_gizmoDragOriginalShape.Nullify();
                        m_gizmoDragBodyId = -1;
                        // Plane and axis drags don't push a history op
                        // (no parametric undo for these direct
                        // setPlane/setAxis writes), so isDirty() won't
                        // catch them via the history-step delta. Mark the
                        // project dirty explicitly so the new position
                        // survives the next close + reopen.
                        if (!m_planeGizmoDrag.empty() ||
                            !m_axisGizmoDrag.empty()) {
                            markDirty();
                        }
                        m_gizmoDragOriginals.clear();
                        m_sketchGizmoDragSketches.clear();
                        m_planeGizmoDrag.clear();
                        m_axisGizmoDrag.clear();
                    }

                    if (gResult.activeAxis != GizmoAxis::None) {
                        gizmoConsumedInput = true;
                    }
                }

                // Skip hover-pick during active camera drag. Picker iterates
                // every visible body and ray-tests their faces — on a
                // complex project (50+ bodies, dense meshes) that's the
                // dominant per-frame cost. During orbit/pan/zoom the user
                // isn't going to pick anything anyway, so we keep the last
                // hover state and re-enable picking the moment they
                // release the drag.
                if (!gizmoConsumedInput && !m_viewCube->wasHovered() &&
                    !m_snapWidgetHovered && !m_revolveArcWasHovered &&
                    !camDragging) {
                    auto result = m_picker->pick(localX, localY,
                        contentSize.x, contentSize.y, cam, *m_document);
                    // Cache for the zoom handler: cursor-zoom needs the
                    // point under the cursor, and re-picking the whole
                    // document on EVERY wheel tick made zooming stutter on
                    // dense scenes (SVG-extruded bodies carry hundreds of
                    // faces). This frame's hover pick is the same ray.
                    m_zoomFocusHit = result.hit;
                    m_zoomFocusPoint = result.hitPoint;
                    m_zoomFocusFrame = ImGui::GetFrameCount();

                    m_hoveredBodyId = result.hit ? result.bodyId : -1;

                    // Sketch-region hover (takes priority over body picking
                    // when present). This one pick serves BOTH hover and
                    // clicks, so region caches are only allowed to BUILD on a
                    // click frame: hover must never trigger the OCCT fuse on
                    // a cold (freshly-unhidden) complex sketch — that read as
                    // "unhide sketch → app not responding". Until first
                    // click, a cold sketch simply has no hover fill.
                    const bool regionClickFrame =
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                        ImGui::IsMouseClicked(ImGuiMouseButton_Right);
                    SketchRegionHit regionHit = pickSketchRegion(localX, localY,
                        contentSize.x, contentSize.y,
                        /*buildIfCold=*/regionClickFrame);
                    // Reject a sketch region that sits behind a body under the cursor —
                    // only what's visible should be selectable. Compare hit distances
                    // from the camera (origin-independent) and drop the region if the
                    // body face is meaningfully nearer.
                    //
                    // Tolerance: the body-face hit comes from a mesh-triangle
                    // intersection, the sketch hit from an analytical plane
                    // intersection, so when the sketch lies ON the body face
                    // they're coplanar by intent but can differ by mesh
                    // tessellation jitter. A tight 1µm threshold (the previous
                    // value) rejected nearly every sketch-on-face hit, making
                    // vertical sketches drawn on body faces unselectable. Use
                    // 0.5 mm + 0.5 % of view distance instead; an actually-
                    // occluding face is normally many mm in front.
                    //
                    // Additionally, if the body face under the cursor IS the
                    // sketch's source face (i.e. the sketch was drawn on this
                    // exact face), the sketch is intentionally "on top" and
                    // must never be rejected.
                    if (regionHit.sketchId >= 0 && result.hit) {
                        glm::vec3 camPos = cam.getPosition();
                        float bodyD = glm::length(result.hitPoint - camPos);
                        float sketchD = glm::length(regionHit.worldPoint - camPos);
                        float tol = std::max(0.5f, sketchD * 0.005f);
                        bool onHostFace = false;
                        auto sk = m_document->getSketch(regionHit.sketchId);
                        if (sk && !sk->getSourceFace().IsNull() &&
                            !result.pickedShape.IsNull() &&
                            result.pickedShape.ShapeType() == TopAbs_FACE) {
                            onHostFace = sk->getSourceFace().IsSame(result.pickedShape);
                        }
                        // NEAREST-FIRST + BIDIRECTIONAL CYCLING. The two
                        // ambiguous cases — a sketch slot between bodies
                        // pulled FROM it (intent: the face behind) vs a
                        // sketch floating in front of unrelated geometry
                        // (intent: the sketch) — are indistinguishable by
                        // any stateless distance rule; we tried them all.
                        // So: whatever is nearest along the ray wins the
                        // FIRST click, and clicking the SAME spot again
                        // cycles to the other. Everything is reachable in
                        // at most two clicks, nothing is ever unselectable,
                        // and hover always highlights what a click would
                        // pick. Host-face regions (sketch-on-face) keep
                        // outright priority.
                        ImVec2 mpNow = ImGui::GetMousePos();
                        bool sameSpot =
                            std::abs(mpNow.x - m_pickCyclePos.x) < 6.0f &&
                            std::abs(mpNow.y - m_pickCyclePos.y) < 6.0f;
                        // Double-click is two same-spot clicks; without
                        // this guard its SECOND click triggers depth
                        // cycling and tunnels to the sketch behind a body
                        // instead of selecting the body. Double-click has
                        // one job — select the whole body — so it's immune
                        // to cycling.
                        const bool dbl =
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                        // When the intent is clearly to TOGGLE regions in a
                        // multi-selection — during Project Sketch, or with Ctrl
                        // held — a same-spot re-click must stay on the region so
                        // it deselects, NOT cycle down to the face behind it
                        // (which cleared the selection / stole the click).
                        const bool stickToRegion =
                            m_projectSketchCtl.active() || ImGui::GetIO().KeyCtrl;
                        int forced = -1; // -1 none, 0 face, 1 region
                        // Touch: a FAST same-spot re-tap is a double-tap (→ body),
                        // not a deliberate "show behind" cycle (IsMouseDoubleClicked
                        // is unreliable for touch). Keep it on the FACE so the
                        // double-tap escalation can take it to the body; cycling to
                        // the sketch behind needs a slower, deliberate re-tap.
                        const bool touchFastReTap =
                            materializr::touchMode() && sameSpot && m_pickCycleTick > 0.0 &&
                            (ImGui::GetTime() - m_pickCycleTick) <
                                ImGui::GetIO().MouseDoubleClickTime;
                        if (touchFastReTap) forced = 0;
                        else if (!dbl && sameSpot && m_pickCycleLast == 0) forced = 1;
                        else if (!dbl && sameSpot && m_pickCycleLast == 1 &&
                                 !stickToRegion) forced = 0;
                        // DEFAULT = the ORIGINAL pre-saga semantics:
                        // region wins on ties / when nearer, face wins only
                        // when the body surface is CLEARLY in front. Every
                        // alternative rule tried during the dual-pull saga
                        // was judged against a document the old preview
                        // engine was corrupting per-frame; with the engine
                        // fixed, the original rule gets retried on honest
                        // evidence. Cycling remains purely as an escape
                        // hatch on deliberate same-spot re-clicks.
                        // Ties go to the FACE: a body whose surface sits
                        // within tolerance of the sketch plane (thin or
                        // symmetric pulls — the plane runs through the
                        // body's middle) must not have its caps stolen by
                        // the region. The region wins only when STRICTLY
                        // nearer; everything demoted is one same-spot
                        // click away via cycling.
                        // SKETCH-FIRST (Steve's call, reversing the old "ties go
                        // to the face"): the FIRST click takes the sketch region
                        // whenever it's coplanar-or-in-front of the face; the slow
                        // same-spot SECOND click cycles to the face below it. The
                        // face only wins the first click when it's STRICTLY in
                        // front (the sketch genuinely sits behind a body), and
                        // even then the sketch is one same-spot click away.
                        const bool faceStrictlyNearer =
                            bodyD < sketchD - tol;
                        bool pickRegion;
                        if (dbl) {
                            // Fast double-click is for the BODY — the region
                            // (even an on-host one) must step aside so the
                            // body-select branch below fires.
                            pickRegion = false;
                        } else if (forced == 1) {
                            pickRegion = true;          // cycle → region
                        } else if (forced == 0) {
                            pickRegion = false;         // cycle → face (overrides onHostFace)
                        } else {
                            // Default first click: sketch-first. onHostFace and a
                            // coplanar/in-front sketch both take the region; the
                            // face only wins when it's strictly in front.
                            pickRegion = onHostFace || !faceStrictlyNearer;
                        }
                        if (!pickRegion) {
                            regionHit.sketchId = -1;
                            regionHit.regionIndex = -1;
                        }
                    }
                    m_hoveredSketchId = regionHit.sketchId;
                    m_hoveredRegionIndex = regionHit.regionIndex;

                    // Selection clicks are OFF while a legacy history-churn
                    // preview is live (push/pull, extrude, pattern, resize,
                    // thread): those previews undo + re-push their op every
                    // frame, so the document's bodies flicker out of
                    // existence and change ids mid-frame — a click can land
                    // in the gap and select nothing, the stale id, or the
                    // sketch region behind the preview ("the pulled face is
                    // unclickable"). Controller iops (Shell/Taper/Scale/
                    // Projection) keep clicks: their previews don't churn
                    // the document, and Projection's live region scoping
                    // NEEDS clicks while its panel is open.
                    const bool clickSelectionAllowed =
                        !m_ppCtl.active() && !m_extrudeCtl.active() &&
                        !m_patternActive && !anyIopActive() &&
                        !m_threadActive &&
                        !m_moveModeToggle &&  // Move (nav lock): taps don't select
                        !shiftPanGesture;     // Shift+Left is a pan, not a pick
                    // Touch: commit selection on a GENUINE tap-LIFT (Window::
                    // consumeSingleTap), not the press frame — else the first
                    // finger-down of a following nav gesture (one-finger orbit /
                    // two-finger pan-zoom) re-picks or clears the selection before
                    // the drag is even recognized (#68). The pick `result` on the
                    // lift frame is at the tap position (no motion parked it), so
                    // the same downstream branches select the right thing. Desktop
                    // keeps click-to-select on press.
                    float _tapSelX = 0.0f, _tapSelY = 0.0f;
                    const bool selectClicked =
                        materializr::touchMode()
                            ? (m_window && m_window->consumeSingleTap(_tapSelX, _tapSelY))
                            : ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                    (void)_tapSelX; (void)_tapSelY;
                    bool regionConsumedClick = false;
                    if (clickSelectionAllowed && regionHit.regionIndex >= 0 &&
                        selectClicked) {
                        SelectionEntry entry;
                        entry.type = SelectionType::SketchRegion;
                        entry.sketchId = regionHit.sketchId;
                        entry.subShapeIndex = regionHit.regionIndex;
                        if (io.KeyCtrl || m_projectSketchCtl.active()) {
                            // Toggle: Ctrl+clicking an already-selected
                            // region deselects it — fixing a bad pick in a
                            // multi-region selection shouldn't mean starting
                            // the whole selection over. During Project Sketch
                            // every click toggles, so building up the set of
                            // regions to project needs no modifier held.
                            m_selection->toggleSelection(entry);
                        } else {
                            m_selection->select(entry);
                        }
                        {
                            ImVec2 mp2 = ImGui::GetMousePos();
                            m_pickCyclePos = glm::vec2(mp2.x, mp2.y);
                        }
                        m_pickCycleTick = ImGui::GetTime();
                        m_pickCycleLast = 1; // region picked; same-spot → face
                        regionConsumedClick = true;
                    }
                    // Edge-only hit (open profile — arc, spline, polyline that
                    // doesn't close) — emit a whole-sketch selection so users
                    // can pick open vertical sketches that would otherwise be
                    // unpickable from the viewport. Skipped when a body face
                    // sits in front of the edge.
                    else if (clickSelectionAllowed &&
                             regionHit.regionIndex < 0 && regionHit.sketchId >= 0 &&
                             selectClicked) {
                        // Same generous occlusion tolerance + source-face
                        // exemption as the region branch above — sketches on
                        // body faces are coplanar by intent.
                        bool occluded = false;
                        if (result.hit) {
                            glm::vec3 camPos = cam.getPosition();
                            float bodyD = glm::length(result.hitPoint - camPos);
                            float sketchD = glm::length(regionHit.worldPoint - camPos);
                            float tol = std::max(0.5f, sketchD * 0.005f);
                            bool onHostFace = false;
                            auto sk = m_document->getSketch(regionHit.sketchId);
                            if (sk && !sk->getSourceFace().IsNull() &&
                                !result.pickedShape.IsNull() &&
                                result.pickedShape.ShapeType() == TopAbs_FACE) {
                                onHostFace = sk->getSourceFace().IsSame(result.pickedShape);
                            }
                            // Mirror of the region branch above:
                            // nearest-first with same-spot cycling.
                            ImVec2 mpE = ImGui::GetMousePos();
                            bool sameSpotE =
                                std::abs(mpE.x - m_pickCyclePos.x) < 6.0f &&
                                std::abs(mpE.y - m_pickCyclePos.y) < 6.0f;
                            int forcedE = -1;
                            if (sameSpotE && m_pickCycleLast == 0) forcedE = 1;
                            else if (sameSpotE && m_pickCycleLast == 1) forcedE = 0;
                            bool sketchStrictlyNearerE =
                                sketchD < bodyD - tol;
                            if (!(onHostFace || forcedE == 1 ||
                                  (forcedE == -1 && sketchStrictlyNearerE)))
                                occluded = true;
                        }
                        if (!occluded) {
                            SelectionEntry entry;
                            entry.type = SelectionType::Sketch;
                            entry.sketchId = regionHit.sketchId;
                            if (io.KeyCtrl) {
                                m_selection->addToSelection(entry);
                            } else {
                                m_selection->select(entry);
                            }
                            regionConsumedClick = true;
                        }
                    }

                    // Mirror "across a face" mode: the next planar face click
                    // defines the mirror plane (Esc cancels via handleShortcuts).
                    if (m_mirrorPickFace && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (result.hit && !result.pickedShape.IsNull() &&
                            result.pickedShape.ShapeType() == TopAbs_FACE && m_mirrorBodyId >= 0) {
                            try {
                                TopoDS_Face face = TopoDS::Face(result.pickedShape);
                                Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
                                if (!surf.IsNull() && surf->IsKind(STANDARD_TYPE(Geom_Plane))) {
                                    gp_Pln pln = Handle(Geom_Plane)::DownCast(surf)->Pln();
                                    const gp_Ax3& ax = pln.Position();
                                    auto op = std::make_unique<MirrorOp>();
                                    op->setBody(m_mirrorBodyId);
                                    op->setPlane(MirrorPlane::Custom);
                                    op->setCustomPlane(gp_Ax2(ax.Location(), ax.Direction()));
                                    op->setKeepOriginal(true);
                                    if (m_history->pushOperation(std::move(op), *m_document))
                                        m_meshesDirty = true;
                                }
                            } catch (...) {}
                        }
                        m_mirrorPickFace = false;
                        regionConsumedClick = true; // don't also change selection
                    }

                    // Measure: point-to-point click capture. When the tool is
                    // in PointToPoint mode, a left-click on something the
                    // picker can hit is captured as a measurement point and
                    // we skip the normal selection logic for that click.
                    bool measureConsumedClick = false;
                    if (m_measureTool &&
                        m_measureTool->getMode() == MeasureMode::Line &&
                        result.hit &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        m_measureTool->capturePoint(result.hitPoint);
                        measureConsumedClick = true;
                        regionConsumedClick = true; // suppress selection paths
                    }
                    (void)measureConsumedClick;

                    // Click-resolution diagnostic: one line per left click,
                    // stating exactly what the pick decided — body/face hit,
                    // region hit, and which path consumed the click. This is
                    // the "face won't select" field tool, so it lives behind
                    // --verbose: normal launches don't pay the stderr flush
                    // per click (or the full verbose RE-PICK + slot dump on
                    // every missed click, which walks the whole document).
                    if (materializr::isVerbose() &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        std::fprintf(stderr,
                            "[Click] hit=%d body=%d shape=%s edgeDist=%.1f | "
                            "region sk=%d idx=%d | consumed=%d\n",
                            result.hit ? 1 : 0, result.bodyId,
                            result.pickedShape.IsNull() ? "null"
                            : (result.pickedShape.ShapeType() == TopAbs_FACE
                                   ? "face" : "other"),
                            result.edgeScreenDist,
                            regionHit.sketchId, regionHit.regionIndex,
                            regionConsumedClick ? 1 : 0);
                        // When the pick MISSED, re-run it verbosely so each
                        // body reports bbox/triangle verdicts, and dump the
                        // renderer's slot table — a slot whose body isn't in
                        // the document (or whose mesh is stale) is the
                        // phantom the user is clicking.
                        if (!result.hit) {
                            Picker::s_verbose = true;
                            (void)m_picker->pick(localX, localY,
                                contentSize.x, contentSize.y, cam,
                                *m_document);
                            Picker::s_verbose = false;
                            m_shapeRenderer->debugDumpSlots();
                            for (int id : m_document->getAllBodyIds())
                                std::fprintf(stderr, "  [Doc] body %d%s\n",
                                             id,
                                             m_document->isBodyVisible(id)
                                                 ? "" : " (hidden)");
                        }
                    }

                    // Double-click to select body, single-click to select face.
                    // Axis / plane hits don't have a body to escalate to, so
                    // the double-click falls through to the single-click
                    // handler below (which builds the right Selection entry).
                    // On touch, a long-press right after a tap reads as a
                    // double-click and would select the whole body unintentionally.
                    // Body selection on touch goes through the long-press menu's
                    // Body branch (and Multi-Select tap) instead, so disable
                    // double-tap-to-body in touch mode.
                    // Body escalation: desktop double-CLICK, or touch double-TAP
                    // (consumeDoubleTap, fired on the 2nd quick release). Running it
                    // as the IF branch means the single-select else-if below is
                    // SKIPPED on the escalation frame — so the body can't be reverted
                    // back to a face even when a quick tap's down+up share one frame.
                    const bool touchDbl =
                        materializr::touchMode() && m_window && m_window->consumeDoubleTap();
                    const bool bodyEscalate = touchDbl ||
                        (!materializr::touchMode() &&
                         ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                    if (clickSelectionAllowed && !regionConsumedClick && bodyEscalate) {
                        // Touch: ImGui's 2nd-tap click can arrive a few frames after
                        // this escalation (its mouse queue lags Window's raw finger
                        // events); ignore face-select clicks briefly so it can't
                        // revert the body back to a face.
                        if (touchDbl) m_suppressFaceClickUntil = ImGui::GetTime() + 0.5;
                        // Desktop uses the freshly-picked body; touch escalates the
                        // face the taps already selected (robust to a stale pick on
                        // the lift frame).
                        int dbid = -1;
                        if (touchDbl) {
                            for (const auto& e : m_selection->getSelection())
                                if (e.type == SelectionType::Face && e.bodyId >= 0) dbid = e.bodyId;
                        } else if (result.hit) {
                            dbid = result.bodyId;
                        }
                      if (dbid >= 0) {
                        SelectionEntry entry;
                        entry.type = SelectionType::Body;
                        entry.bodyId = dbid;
                        try { entry.shape = m_document->getBody(dbid); } catch (...) {}
                        if (io.KeyCtrl) {
                            // Replace this body's faces/edges with the body itself —
                            // keep other selected items, no stray faces.
                            std::vector<SelectionEntry> drop;
                            for (const auto& e : m_selection->getSelection())
                                if (e.bodyId == dbid && e.type != SelectionType::Body)
                                    drop.push_back(e);
                            for (const auto& e : drop) m_selection->removeFromSelection(e);
                            m_selection->addToSelection(entry);
                        } else {
                            m_selection->select(entry);
                        }
                      }
                        // A double-click is a deliberate "select the whole
                        // body". Its FIRST click already ran the face branch
                        // and left the cycle state at face (m_pickCycleLast=0)
                        // on this spot. Clear it so the next HOVER frame here
                        // doesn't think it's mid-cycle and highlight the sketch
                        // region hidden behind the body (purely visual — a
                        // click never selected it, but it looked clickable).
                        m_pickCycleLast = -1;
                    } else if (clickSelectionAllowed && !regionConsumedClick &&
                               selectClicked &&
                               !(materializr::touchMode() &&
                                 ImGui::GetTime() < m_suppressFaceClickUntil)) {
                        int ownerStep = -1; // fillet/chamfer step to open in the editor
                        // Multi-Select forces io.KeyCtrl for this viewport scope, so
                        // the normal pick below adds/toggles the SUB-SHAPE you tap —
                        // edges (the common fillet/chamfer target), faces, vertices —
                        // additively. Whole-body multi-select is via the long-press
                        // menu's Body branch (also additive while Multi-Select is on).
                        if (result.hit && result.axisId >= 0) {
                            // Construction-axis hit — own selection path,
                            // skip body/face/edge branching.
                            SelectionEntry entry;
                            entry.type = SelectionType::Axis;
                            entry.axisId = result.axisId;
                            if (io.KeyCtrl) m_selection->toggleSelection(entry);
                            else            m_selection->select(entry);
                        } else if (result.hit && result.planeId >= 0) {
                            // Construction-plane hit takes its own selection path —
                            // no edge / face / fillet-edit branching applies.
                            SelectionEntry entry;
                            entry.type = SelectionType::Plane;
                            entry.planeId = result.planeId;
                            if (io.KeyCtrl) m_selection->toggleSelection(entry);
                            else            m_selection->select(entry);
                        } else if (result.hit) {
                            SelectionEntry entry;
                            // Vertex pick takes priority over edge / face when the
                            // cursor lands within ~8 px of a face corner AND is
                            // closer to that corner than to the nearest edge.
                            // Selection EXPANDS to every edge meeting at the
                            // vertex, so a single click on a box corner picks all
                            // three adjacent edges in one go — fillet/chamfer that
                            // whole corner without re-picking. Threshold matches
                            // the edge-vs-face one and is clamped to ¼ of the
                            // face's on-screen size so tiny faces don't lose
                            // every click to their own corner radius.
                            // (Steve: "click a corner, highlight all immediate
                            //  edges".)
                            // Tighter than the edge threshold (8 px) and the
                            // edge-distance comparison is dropped on purpose:
                            // at a corner the picked face's incident edges
                            // start AT the vertex, so their screen distance is
                            // ~equal to the vertex's and the vertex would never
                            // win otherwise. Giving the vertex its own 6 px
                            // privileged window means "click within 6 px of a
                            // corner = corner pick" and edge clicks have to
                            // happen further from the endpoints.
                            float vertexThresh = std::min(6.0f, result.faceScreenSize * 0.25f);
                            bool vertexClickHandled = false;
                            if (!result.nearestVertex.IsNull() &&
                                result.vertexScreenDist < vertexThresh &&
                                result.bodyId >= 0) {
                                try {
                                    const TopoDS_Shape& bodyShape =
                                        m_document->getBody(result.bodyId);
                                    TopTools_IndexedDataMapOfShapeListOfShape vMap;
                                    TopExp::MapShapesAndAncestors(
                                        bodyShape, TopAbs_VERTEX, TopAbs_EDGE, vMap);
                                    if (vMap.Contains(result.nearestVertex)) {
                                        const TopTools_ListOfShape& edges =
                                            vMap.FindFromKey(result.nearestVertex);
                                        if (!edges.IsEmpty()) {
                                            if (!io.KeyCtrl) m_selection->clear();
                                            for (const TopoDS_Shape& e : edges) {
                                                SelectionEntry ee;
                                                ee.type = SelectionType::Edge;
                                                ee.bodyId = result.bodyId;
                                                ee.shape = e;
                                                m_selection->addToSelection(ee);
                                            }
                                            vertexClickHandled = true;
                                        }
                                    }
                                } catch (...) {}
                            }
                            if (vertexClickHandled) {
                                // Corner expansion already populated the selection;
                                // skip the edge-vs-face fallback below. Reset
                                // pick-cycle so the next click at the SAME spot
                                // is a fresh pick rather than "give me the next
                                // candidate at this position".
                                ImVec2 mp2 = ImGui::GetMousePos();
                                m_pickCyclePos = glm::vec2(mp2.x, mp2.y);
                                m_pickCycleTick = ImGui::GetTime();
                                m_pickCycleLast = -1;
                            } else {
                                // If click is near an edge, select edge; otherwise face. The
                                // 8 px threshold is clamped to ¼ of the face's on-screen size
                                // so a small face (say 20 px wide) doesn't lose every interior
                                // click to one of its own boundary edges (every interior pixel
                                // is within 8 px of an edge when the face is itself only 16 px
                                // across).
                                // A fingertip is far less precise than a cursor, so
                                // widen the edge-promotion radius in touch mode (8 px
                                // ~ 0.85 mm at 240 dpi is unhittable). Still clamped
                                // to ¼ of the face so small faces keep their interior.
                                const float edgeBase = materializr::touchMode() ? 24.0f : 8.0f;
                                float edgeThresh = std::min(edgeBase, result.faceScreenSize * 0.25f);
                                if (result.edgeScreenDist < edgeThresh && !result.nearestEdge.IsNull()) {
                                    entry.type = SelectionType::Edge;
                                    entry.bodyId = result.bodyId;
                                    entry.shape = result.nearestEdge;
                                } else {
                                    entry.type = SelectionType::Face;
                                    entry.bodyId = result.bodyId;
                                    entry.subShapeIndex = result.faceIndex;
                                    entry.shape = result.pickedShape;
                                    // Trace a clicked face back to the fillet/chamfer that
                                    // produced it, so the user can re-edit it after the fact.
                                    if (!entry.shape.IsNull()) {
                                        int upTo = m_history->currentStep();
                                        for (int s = 0; s <= upTo; ++s) {
                                            const Operation* op = m_history->getStep(s);
                                            if (op && op->isEnabled() && op->ownsFace(entry.shape)) {
                                                ownerStep = s;
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (io.KeyCtrl) {
                                    // Toggle so a Ctrl+click on something
                                    // already selected deselects just that
                                    // one (matches the plane / axis paths
                                    // above). Was addToSelection, which
                                    // could only grow the set — making the
                                    // user clear and re-pick to drop one
                                    // item. (Steve: trackpad-mode multi-
                                    // select had no way to drop a member.)
                                    m_selection->toggleSelection(entry);
                                } else {
                                    m_selection->select(entry);
                                }
                                // Click-cycling: a face was picked here; the
                                // next click at the SAME spot offers the
                                // covered sketch region instead (if any).
                                {
                                    ImVec2 mp2 = ImGui::GetMousePos();
                                    m_pickCyclePos = glm::vec2(mp2.x, mp2.y);
                                }
                                m_pickCycleTick = ImGui::GetTime();
                                m_pickCycleLast = 0; // face picked; same-spot → region
                            }
                        } else {
                            // Empty-space click: begin a box-select drag instead of
                            // clearing immediately. The release handler below decides
                            // whether to multi-select bodies (drag had area) or treat
                            // it as a plain click and clear.
                            // During Project Sketch an empty-space click is
                            // almost always a near-miss on a tiny region (e.g.
                            // where the aperture rings converge). Do NOTHING —
                            // don't clear the region set, don't box-select. The
                            // "Clear" button is the deliberate reset.
                            const bool projecting = m_projectSketchCtl.active();
                            bool boxEligible = !m_inSketchMode && !m_extrudeCtl.active() &&
                                !m_ppCtl.active() && !m_edgeCtl.active() && !m_gizmoDragging &&
                                !projecting &&
                                // Normally box-select needs Left free of the camera;
                                // in trackpad / left-camera mode, Alt+Left frees it.
                                ((m_orbitButton != ImGuiMouseButton_Left &&
                                  m_panButton  != ImGuiMouseButton_Left) ||
                                 io.KeyAlt);
                            if (boxEligible && m_boxSelect) {
                                ImVec2 mp = ImGui::GetMousePos();
                                ImVec2 wp = ImGui::GetItemRectMin();
                                m_boxSelect->begin(glm::vec2(mp.x - wp.x, mp.y - wp.y));
                            } else if (!io.KeyCtrl && !projecting) {
                                m_selection->clear();
                            }
                        }
                        // Open the owning fillet/chamfer in the History editor, or
                        // close that editor when clicking anything else.
                        m_historyPanel->setEditingStep(ownerStep);
                    }

                    if (materializr::touchMode()) {
                        // Touch press-and-hold begins a box-select at the held point
                        // (trackpad mode reserves left-drag for orbit, so the desktop
                        // empty-space path never fires under touch).
                        if (m_window && m_window->isTouchHoldSelect() && m_boxSelect &&
                            !m_boxSelect->isActive() && !m_inSketchMode && !m_extrudeCtl.active() &&
                            !m_ppCtl.active() && !m_edgeCtl.active() && !m_gizmoDragging) {
                            ImVec2 mp = ImGui::GetMousePos();
                            ImVec2 wp = ImGui::GetItemRectMin();
                            m_boxSelect->begin(glm::vec2(mp.x - wp.x, mp.y - wp.y));
                        }
                    }

                    // Box-select drag + release. Update while LEFT is held; on
                    // release, intersect bodies' screen-space bboxes with the
                    // rectangle and add them to selection (Ctrl preserves the
                    // existing selection).
                    if (m_boxSelect && m_boxSelect->isActive()) {
                        ImVec2 mp = ImGui::GetMousePos();
                        ImVec2 wp = ImGui::GetItemRectMin();
                        glm::vec2 curScreen(mp.x - wp.x, mp.y - wp.y);
                        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                            m_boxSelect->update(curScreen);
                        }
                        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                            glm::vec2 mn = m_boxSelect->getMin();
                            glm::vec2 mx = m_boxSelect->getMax();
                            m_boxSelect->end();

                            // Tiny rectangle = treat as a plain click → clear.
                            if (glm::distance(mn, mx) < 4.0f) {
                                if (!io.KeyCtrl) m_selection->clear();
                            } else {
                                if (!io.KeyCtrl) m_selection->clear();
                                glm::mat4 vp = proj * view;
                                for (int id : m_document->getAllBodyIds()) {
                                    if (!m_document->isBodyVisible(id)) continue;
                                    try {
                                        const TopoDS_Shape& shape = m_document->getBody(id);
                                        Bnd_Box bb; BRepBndLib::Add(shape, bb);
                                        if (bb.IsVoid()) continue;
                                        double x0,y0,z0,x1,y1,z1;
                                        bb.Get(x0,y0,z0,x1,y1,z1);
                                        // Project the 8 bbox corners into screen pixels,
                                        // skipping any behind the camera (w<=0).
                                        glm::vec2 bMin( FLT_MAX,  FLT_MAX);
                                        glm::vec2 bMax(-FLT_MAX, -FLT_MAX);
                                        bool any = false;
                                        for (int c = 0; c < 8; ++c) {
                                            glm::vec4 p((c & 1) ? x1 : x0,
                                                        (c & 2) ? y1 : y0,
                                                        (c & 4) ? z1 : z0, 1.0f);
                                            glm::vec4 cp = vp * p;
                                            if (cp.w <= 0.0f) continue;
                                            glm::vec2 ndc(cp.x / cp.w, cp.y / cp.w);
                                            glm::vec2 sp(
                                                (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                                (1.0f - (ndc.y * 0.5f + 0.5f)) * contentSize.y);
                                            bMin = glm::min(bMin, sp);
                                            bMax = glm::max(bMax, sp);
                                            any = true;
                                        }
                                        if (!any) continue;
                                        // AABB overlap test against the box rect.
                                        if (bMax.x >= mn.x && bMin.x <= mx.x &&
                                            bMax.y >= mn.y && bMin.y <= mx.y) {
                                            SelectionEntry e;
                                            e.type = SelectionType::Body;
                                            e.bodyId = id;
                                            e.shape = shape;
                                            m_selection->addToSelection(e);
                                        }
                                    } catch (...) {}
                                }

                                // Sketches also participate in box-select. We
                                // compute each sketch's on-screen AABB from the
                                // 3D world positions of its 2D points (lines /
                                // circles / arcs / splines / polygons all share
                                // the SketchPoint table for their endpoints,
                                // start/end, controls, and vertices respectively
                                // — circles add a centre + radius extent which
                                // we account for explicitly). Sketches with no
                                // points are skipped. Active sketch included.
                                auto projectSketch = [&](const Sketch& sk,
                                                         glm::vec2& outMin,
                                                         glm::vec2& outMax) -> bool {
                                    const gp_Pln& pln = sk.getPlane();
                                    const gp_Ax3& ax = pln.Position();
                                    glm::vec3 origin(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                                    glm::vec3 xd(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                                    glm::vec3 yd(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                                    auto to3d = [&](glm::vec2 p) {
                                        return origin + xd * p.x + yd * p.y;
                                    };
                                    outMin = glm::vec2( FLT_MAX,  FLT_MAX);
                                    outMax = glm::vec2(-FLT_MAX, -FLT_MAX);
                                    bool any = false;
                                    auto addWorld = [&](glm::vec3 w) {
                                        glm::vec4 cp = vp * glm::vec4(w, 1.0f);
                                        if (cp.w <= 0.0f) return;
                                        glm::vec2 ndc(cp.x / cp.w, cp.y / cp.w);
                                        glm::vec2 sp(
                                            (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                            (1.0f - (ndc.y * 0.5f + 0.5f)) * contentSize.y);
                                        outMin = glm::min(outMin, sp);
                                        outMax = glm::max(outMax, sp);
                                        any = true;
                                    };
                                    for (const auto& pt : sk.getPoints()) addWorld(to3d(pt.pos));
                                    for (const auto& c : sk.getCircles()) {
                                        const SketchPoint* ctr = sk.getPoint(c.centerPointId);
                                        if (!ctr) continue;
                                        float r = static_cast<float>(c.radius);
                                        addWorld(to3d(glm::vec2(ctr->pos.x + r, ctr->pos.y)));
                                        addWorld(to3d(glm::vec2(ctr->pos.x - r, ctr->pos.y)));
                                        addWorld(to3d(glm::vec2(ctr->pos.x, ctr->pos.y + r)));
                                        addWorld(to3d(glm::vec2(ctr->pos.x, ctr->pos.y - r)));
                                    }
                                    return any;
                                };
                                auto considerSketch = [&](int sid, const Sketch& sk) {
                                    glm::vec2 bMin, bMax;
                                    if (!projectSketch(sk, bMin, bMax)) return;
                                    if (!(bMax.x >= mn.x && bMin.x <= mx.x &&
                                          bMax.y >= mn.y && bMin.y <= mx.y))
                                        return;
                                    // Region-granular: box-select picks up
                                    // the sketch's closed REGIONS, exactly
                                    // like Ctrl+clicking each one — so the
                                    // toolbar offers the same per-region
                                    // tools and downstream ops take the
                                    // same (working) path. The whole-sketch
                                    // entry survives only for open profiles
                                    // that have no regions to offer.
                                    auto regions = sk.buildRegions();
                                    int added = 0;
                                    const gp_Pln& pln = sk.getPlane();
                                    const gp_Ax3& rax = pln.Position();
                                    glm::vec3 rorigin(rax.Location().X(), rax.Location().Y(), rax.Location().Z());
                                    glm::vec3 rxd(rax.XDirection().X(), rax.XDirection().Y(), rax.XDirection().Z());
                                    glm::vec3 ryd(rax.YDirection().X(), rax.YDirection().Y(), rax.YDirection().Z());
                                    for (size_t ri = 0; ri < regions.size(); ++ri) {
                                        glm::vec2 rp = regions[ri].representativePoint;
                                        glm::vec3 w = rorigin + rxd * rp.x + ryd * rp.y;
                                        glm::vec4 cp = vp * glm::vec4(w, 1.0f);
                                        if (cp.w <= 0.0f) continue;
                                        glm::vec2 ndc(cp.x / cp.w, cp.y / cp.w);
                                        glm::vec2 sp(
                                            (ndc.x * 0.5f + 0.5f) * contentSize.x,
                                            (1.0f - (ndc.y * 0.5f + 0.5f)) * contentSize.y);
                                        if (sp.x < mn.x || sp.x > mx.x ||
                                            sp.y < mn.y || sp.y > mx.y)
                                            continue;
                                        SelectionEntry e;
                                        e.type = SelectionType::SketchRegion;
                                        e.sketchId = sid;
                                        e.subShapeIndex = static_cast<int>(ri);
                                        m_selection->addToSelection(e);
                                        added++;
                                    }
                                    if (added == 0 && regions.empty()) {
                                        SelectionEntry e;
                                        e.type = SelectionType::Sketch;
                                        e.sketchId = sid;
                                        m_selection->addToSelection(e);
                                    }
                                };
                                for (int sid : m_document->getAllSketchIds()) {
                                    if (!m_document->isSketchVisible(sid)) continue;
                                    auto sk = m_document->getSketch(sid);
                                    if (sk) considerSketch(sid, *sk);
                                }
                                if (m_activeSketch && m_activeSketchId < 0) {
                                    // In-progress sketch (not yet committed to
                                    // the document) — its id is -1, which the
                                    // selection layer accepts.
                                    considerSketch(m_activeSketchId, *m_activeSketch);
                                }
                            }
                        }
                    }

                    // Right click on a face: context menu (only if not a pan drag)
                    ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
                    bool wasDragging = (std::abs(dragDelta.x) > 1.0f || std::abs(dragDelta.y) > 1.0f);
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !wasDragging) {
                        if (result.hit && result.planeId >= 0) {
                            // Right-click on a construction plane → its own menu
                            // (Flip Normal / Rotate About Axis), mirroring the
                            // Items-panel plane context menu so the normal can be
                            // adjusted right on the plane in the viewport.
                            m_contextMenuPlaneId = result.planeId;
                            m_contextMenuSketchId = -1;
                            m_contextMenuFace.Nullify();
                            m_contextMenuPending = true;
                        } else {
                            // Unified object menu — any of face / body / sketch
                            // in the clicked area gets its own submenu (touch
                            // can't disambiguate the pick, so we offer all).
                            if (result.hit && !result.pickedShape.IsNull()) {
                                m_contextMenuBodyId = result.bodyId;
                                m_contextMenuFace = result.pickedShape;
                            } else {
                                m_contextMenuBodyId = -1;
                                m_contextMenuFace.Nullify();
                            }
                            m_contextMenuSketchId = m_hoveredSketchId; // -1 if none
                            m_contextMenuPlaneId = -1;
                            if (!m_contextMenuFace.IsNull() || m_contextMenuSketchId >= 0)
                                m_contextMenuPending = true;
                        }
                    }
                }
            }

            // Sketch mode mouse input — ray-plane intersection. Skipped while
            // the camera is being dragged so the in-progress preview (e.g. the
            // line endpoint following the cursor) doesn't jolt as the view moves,
            // and while Shift is held in trackpad mode — that press belongs to
            // the pan gesture, and a drawing tool would otherwise place its point
            // on it before the drag was ever recognised (see shiftPanGesture).
            // Suppress while the ViewCube widget is being hovered/dragged so its
            // click doesn't pass through and start a line draw underneath.
            if (m_inSketchMode && m_activeSketch && !camDragging &&
                !shiftPanGesture &&
                !m_viewCube->wasHovered() && !m_snapWidgetHovered &&
                !m_revolveArcWasHovered) {
                ImVec2 mousePos = ImGui::GetMousePos();
                ImVec2 winPos = ImGui::GetItemRectMin();
                float localX = mousePos.x - winPos.x;
                float localY = mousePos.y - winPos.y;
                glm::vec2 sketchCoord = screenToSketch(localX, localY, contentSize.x, contentSize.y);

                // Sketch pattern axis-origin picker. While the radial sketch
                // pattern popup is asking for an origin, the next click in
                // the sketch viewport sets it (snapped to the sketch grid)
                // instead of going through SketchTool's normal input.
                bool patternPickingNow = m_sketchPatternActive && m_sketchPatternPickingOrigin;
                if (patternPickingNow) {
                    float step = std::max(m_sketchGridStep, 0.01f);
                    glm::vec2 snapped(std::round(sketchCoord.x / step) * step,
                                      std::round(sketchCoord.y / step) * step);
                    ImVec2 sp(mousePos.x, mousePos.y);
                    auto* dl = ImGui::GetForegroundDrawList();
                    dl->AddCircleFilled(sp, 7.0f, IM_COL32(255, 220, 50, 255));
                    dl->AddCircle      (sp, 7.0f, IM_COL32(0, 0, 0, 255), 0, 1.5f);
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f)", snapped.x, snapped.y);
                    dl->AddText(ImVec2(sp.x + 10.0f, sp.y - 8.0f),
                                IM_COL32(255, 220, 50, 255), buf);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        m_sketchPatternOriginX = snapped.x;
                        m_sketchPatternOriginY = snapped.y;
                        std::snprintf(m_sketchPatternOXBuf, sizeof(m_sketchPatternOXBuf),
                                      "%.2f", m_sketchPatternOriginX);
                        std::snprintf(m_sketchPatternOYBuf, sizeof(m_sketchPatternOYBuf),
                                      "%.2f", m_sketchPatternOriginY);
                        m_sketchPatternPickingOrigin = false;
                        updateSketchPattern();
                    }
                }

                // Interactive mirror: the mirror line is driven by the SAME
                // move/rotate gizmo style as sketch elements (axis arrows +
                // free-move centre + rotate ring), so it's uniform and rotate is
                // built in. Handled here so it pre-empts the drawing/select
                // routing below. Move handles slide the anchor; the ring spins
                // the mirror angle.
                bool mirrorActiveNow = m_sketchTool->getMode() == SketchToolMode::Mirror &&
                                       m_sketchTool->isMirrorActive();
                if (mirrorActiveNow) {
                    ImDrawList* mgl = ImGui::GetWindowDrawList();
                    const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                    glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                    glm::vec3 Xw(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                    glm::vec3 Yw(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                    auto sk2w = [&](glm::vec2 p) { return O + p.x * Xw + p.y * Yw; };
                    glm::mat4 mvp = proj * view;
                    auto mToImg = [&](glm::vec3 w, ImVec2& out) -> bool {
                        glm::vec4 c = mvp * glm::vec4(w, 1.0f);
                        if (c.w <= 1e-5f) return false;
                        out = ImVec2(winPos.x + (c.x / c.w * 0.5f + 0.5f) * contentSize.x,
                                     winPos.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * contentSize.y);
                        return true;
                    };
                    glm::vec2 anchor = m_sketchTool->getMirrorAnchor();
                    ImVec2 sc, sx, sy;
                    bool projOk = mToImg(sk2w(anchor), sc) &&
                                  mToImg(sk2w(anchor + glm::vec2(1.0f, 0.0f)), sx) &&
                                  mToImg(sk2w(anchor + glm::vec2(0.0f, 1.0f)), sy);
                    glm::vec2 vx(sx.x - sc.x, sx.y - sc.y);
                    glm::vec2 vy(sy.x - sc.x, sy.y - sc.y);
                    if (projOk && glm::length(vx) > 1e-3f && glm::length(vy) > 1e-3f) {
                        vx = glm::normalize(vx); vy = glm::normalize(vy);
                        const bool touch = materializr::touchMode();
                        const float armLen  = touch ? 90.0f : 70.0f;
                        const float ringR   = touch ? 130.0f : 100.0f;
                        const float centerR = touch ? 11.0f : 6.5f;
                        const float pickPx  = touch ? 22.0f : 8.0f;
                        ImVec2 ex(sc.x + vx.x * armLen, sc.y + vx.y * armLen);
                        ImVec2 ey(sc.x + vy.x * armLen, sc.y + vy.y * armLen);

                        auto distSegSq = [](glm::vec2 q, glm::vec2 a, glm::vec2 b) {
                            glm::vec2 ab = b - a;
                            float len2 = glm::dot(ab, ab);
                            if (len2 < 1e-6f) return glm::dot(q - a, q - a);
                            float t = glm::clamp(glm::dot(q - a, ab) / len2, 0.0f, 1.0f);
                            glm::vec2 pr = a + t * ab;
                            return glm::dot(q - pr, q - pr);
                        };
                        glm::vec2 mv(mousePos.x, mousePos.y);
                        glm::vec2 scV(sc.x, sc.y), exV(ex.x, ex.y), eyV(ey.x, ey.y);
                        float distC = glm::length(mv - scV);
                        float dxSq = distSegSq(mv, scV, exV), dySq = distSegSq(mv, scV, eyV);
                        float distRing = std::abs(distC - ringR);
                        float pickSq = pickPx * pickPx;
                        SketchGizmoHandle hover = SketchGizmoHandle::None;
                        if      (distC < centerR + pickPx * 0.6f)                 hover = SketchGizmoHandle::MoveFree;
                        else if (dxSq < pickSq && distC < armLen + 14.0f)         hover = SketchGizmoHandle::MoveX;
                        else if (dySq < pickSq && distC < armLen + 14.0f)         hover = SketchGizmoHandle::MoveY;
                        else if (distRing < pickPx)                              hover = SketchGizmoHandle::Rotate;
                        (void)mgl; // gizmo is DRAWN in the always-run render pass
                                   // (so it's visible without a canvas tap); this
                                   // block only hit-tests + drags.

                        // Arm a handle on press.
                        if (m_mirrorGizmoHandle == SketchGizmoHandle::None &&
                            hover != SketchGizmoHandle::None && !io.KeyCtrl &&
                            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            m_mirrorGizmoHandle = hover;
                            m_mirrorGizmoStartAnchor = anchor;
                            m_mirrorGizmoGrab = sketchCoord;
                            m_mirrorGizmoStartAngle = m_sketchTool->getMirrorAngle();
                        }
                    }
                    // Apply the drag.
                    if (m_mirrorGizmoHandle != SketchGizmoHandle::None &&
                        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        if (m_mirrorGizmoHandle == SketchGizmoHandle::Rotate) {
                            glm::vec2 v0 = m_mirrorGizmoGrab        - m_mirrorGizmoStartAnchor;
                            glm::vec2 v1 = sketchCoord              - m_mirrorGizmoStartAnchor;
                            if (glm::length(v0) > 1e-4f && glm::length(v1) > 1e-4f) {
                                float dRad = std::atan2(v1.y, v1.x) - std::atan2(v0.y, v0.x);
                                float deg = (m_mirrorGizmoStartAngle + dRad) * 180.0f / static_cast<float>(M_PI);
                                deg = std::round(deg / 5.0f) * 5.0f; // snap to 5° increments
                                m_sketchTool->setMirrorAngle(deg * static_cast<float>(M_PI) / 180.0f);
                            }
                        } else {
                            glm::vec2 delta = sketchCoord - m_mirrorGizmoGrab;
                            if (m_mirrorGizmoHandle == SketchGizmoHandle::MoveX) delta.y = 0.0f;
                            else if (m_mirrorGizmoHandle == SketchGizmoHandle::MoveY) delta.x = 0.0f;
                            glm::vec2 na = m_mirrorGizmoStartAnchor + delta;
                            // Snap to HALF the grid step: reflection doubles the
                            // line's travel, so a half-step line move lands the
                            // mirrored elements on whole grid increments (e.g. a
                            // 0.5 mm line nudge → 1 mm mirror shift on a 1 mm grid).
                            float step = m_sketchTool->getGridStep() * 0.5f;
                            if (step > 0.0f) {
                                if (m_mirrorGizmoHandle != SketchGizmoHandle::MoveY)
                                    na.x = std::round(na.x / step) * step;
                                if (m_mirrorGizmoHandle != SketchGizmoHandle::MoveX)
                                    na.y = std::round(na.y / step) * step;
                            }
                            m_sketchTool->setMirrorAnchor(na);
                        }
                    }
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                        m_mirrorGizmoHandle = SketchGizmoHandle::None;
                }

                if (!patternPickingNow && !mirrorActiveNow) {
                // === Sketch Move/Rotate gizmo ====================================
                // Drawn on the selection centroid in Select mode. Axis arrows for
                // constrained X/Y move, centre dot for free move, ring for rotate.
                // Held-drag: click a handle → drag → release commits. While the
                // gizmo owns the drag, the normal sketch tool input is skipped so
                // both don't try to mutate the same points.
                ImDrawList* gdl = ImGui::GetWindowDrawList();
                glm::mat4 gvp = proj * view;
                auto gToImg = [&](glm::vec3 w, ImVec2& out) -> bool {
                    glm::vec4 c = gvp * glm::vec4(w, 1.0f);
                    if (c.w <= 1e-5f) return false; // behind camera
                    out = ImVec2(winPos.x + (c.x / c.w * 0.5f + 0.5f) * contentSize.x,
                                 winPos.y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * contentSize.y);
                    return true;
                };
                bool gizmoOwnsInput = (m_sketchGizmoHandle != SketchGizmoHandle::None);
                if (m_sketchTool->getMode() == SketchToolMode::Select &&
                    m_sketchTool->hasElementSelection()) {

                    // Resolve involved point ids (selected points + endpoints of
                    // selected lines) and compute centroid.
                    std::set<int> involved(m_sketchTool->getSelectedPoints().begin(),
                                           m_sketchTool->getSelectedPoints().end());
                    for (int lid : m_sketchTool->getSelectedLines()) {
                        for (const auto& l : m_activeSketch->getLines()) {
                            if (l.id == lid) {
                                involved.insert(l.startPointId);
                                involved.insert(l.endPointId);
                                break;
                            }
                        }
                    }
                    // Selected circles/arcs: dragging their defining points moves the
                    // whole curve rigidly (centre carries the circle; centre+ends carry
                    // the arc), so the gizmo arms on a rim-selected circle too.
                    for (int cid : m_sketchTool->getSelectedCircles()) {
                        for (const auto& c : m_activeSketch->getCircles())
                            if (c.id == cid) { involved.insert(c.centerPointId); break; }
                    }
                    for (int aid : m_sketchTool->getSelectedArcs()) {
                        for (const auto& a : m_activeSketch->getArcs())
                            if (a.id == aid) {
                                involved.insert(a.centerPointId);
                                involved.insert(a.startPointId);
                                involved.insert(a.endPointId);
                                break;
                            }
                    }
                    // Splines move rigidly by dragging all their control points.
                    for (int sid : m_sketchTool->getSelectedSplines()) {
                        for (const auto& sp : m_activeSketch->getSplines())
                            if (sp.id == sid) {
                                for (int cp : sp.controlPointIds) involved.insert(cp);
                                break;
                            }
                    }
                    glm::vec2 c{0.0f};
                    int nInv = 0;
                    for (int id : involved)
                        if (auto* p = m_activeSketch->getPoint(id)) { c += p->pos; ++nInv; }
                    if (nInv > 0) {
                        c /= static_cast<float>(nInv);
                        // During a drag, anchor the gizmo visual at the drag-start
                        // centroid so it doesn't slide around with the selection.
                        glm::vec2 gizmoC = gizmoOwnsInput ? m_sketchGizmoCenter : c;

                        const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                        glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                        glm::vec3 Xw(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                        glm::vec3 Yw(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                        auto sk2w = [&](glm::vec2 p) { return O + p.x * Xw + p.y * Yw; };

                        ImVec2 sc, sx, sy;
                        bool projOk = gToImg(sk2w(gizmoC), sc) &&
                                      gToImg(sk2w(gizmoC + glm::vec2(1.0f, 0.0f)), sx) &&
                                      gToImg(sk2w(gizmoC + glm::vec2(0.0f, 1.0f)), sy);
                        glm::vec2 vx(sx.x - sc.x, sx.y - sc.y);
                        glm::vec2 vy(sy.x - sc.x, sy.y - sc.y);
                        if (projOk && glm::length(vx) > 1e-3f && glm::length(vy) > 1e-3f) {
                            vx = glm::normalize(vx);
                            vy = glm::normalize(vy);
                            const bool  gzTouch = materializr::touchMode();
                            const float armLen  = gzTouch ? 90.0f : 70.0f;
                            const float ringR   = gzTouch ? 130.0f : 100.0f;
                            const float centerR = gzTouch ? 11.0f : 6.5f;
                            ImVec2 ex(sc.x + vx.x * armLen, sc.y + vx.y * armLen);
                            ImVec2 ey(sc.x + vy.x * armLen, sc.y + vy.y * armLen);

                            // Hit-test against handles in screen space.
                            auto distSegSq = [](glm::vec2 q, glm::vec2 a, glm::vec2 b) {
                                glm::vec2 ab = b - a;
                                float len2 = glm::dot(ab, ab);
                                if (len2 < 1e-6f) return glm::dot(q - a, q - a);
                                float t = glm::clamp(glm::dot(q - a, ab) / len2, 0.0f, 1.0f);
                                glm::vec2 proj = a + t * ab;
                                return glm::dot(q - proj, q - proj);
                            };
                            glm::vec2 mv(mousePos.x, mousePos.y);
                            glm::vec2 scV(sc.x, sc.y), exV(ex.x, ex.y), eyV(ey.x, ey.y);
                            float distC    = glm::length(mv - scV);
                            float dxSq     = distSegSq(mv, scV, exV);
                            float dySq     = distSegSq(mv, scV, eyV);
                            float distRing = std::abs(distC - ringR);

                            const float pickPx = gzTouch ? 22.0f : 8.0f;
                            const float pickPxSq = pickPx * pickPx;
                            SketchGizmoHandle hover = SketchGizmoHandle::None;
                            if      (distC < centerR + 4.0f)                            hover = SketchGizmoHandle::MoveFree;
                            else if (dxSq < pickPxSq && distC < armLen + 12.0f)         hover = SketchGizmoHandle::MoveX;
                            else if (dySq < pickPxSq && distC < armLen + 12.0f)         hover = SketchGizmoHandle::MoveY;
                            else if (distRing < pickPx)                                 hover = SketchGizmoHandle::Rotate;

                            // Yellow highlight for the active or hovered handle.
                            auto col = [&](ImU32 base, SketchGizmoHandle h) -> ImU32 {
                                bool hot = (hover == h) || (m_sketchGizmoHandle == h);
                                return hot ? IM_COL32(255, 240, 80, 255) : base;
                            };
                            ImU32 colX      = col(IM_COL32(230,  70,  70, 230), SketchGizmoHandle::MoveX);
                            ImU32 colY      = col(IM_COL32( 90, 200,  90, 230), SketchGizmoHandle::MoveY);
                            ImU32 colRing   = col(IM_COL32(220, 180,  60, 220), SketchGizmoHandle::Rotate);
                            ImU32 colCenter = col(IM_COL32(240, 240, 230, 230), SketchGizmoHandle::MoveFree);

                            // Rotate ring (behind the arrows so arrowheads aren't clipped).
                            gdl->AddCircle(sc, ringR, colRing, 64, 2.5f);
                            // Axis arrows.
                            gdl->AddLine(sc, ex, colX, 3.0f);
                            gdl->AddLine(sc, ey, colY, 3.0f);
                            auto arrowhead = [&](ImVec2 tip, glm::vec2 d, ImU32 c) {
                                glm::vec2 perp(-d.y, d.x);
                                ImVec2 a(tip.x - d.x * 14.0f + perp.x * 6.0f,
                                         tip.y - d.y * 14.0f + perp.y * 6.0f);
                                ImVec2 b(tip.x - d.x * 14.0f - perp.x * 6.0f,
                                         tip.y - d.y * 14.0f - perp.y * 6.0f);
                                gdl->AddTriangleFilled(tip, a, b, c);
                            };
                            arrowhead(ex, vx, colX);
                            arrowhead(ey, vy, colY);
                            // Centre free-move dot, on top.
                            gdl->AddCircleFilled(sc, centerR, colCenter);
                            gdl->AddCircle(sc, centerR, IM_COL32(30, 30, 40, 230), 0, 1.2f);

                            // Live angle label while dragging the rotate ring, plus
                            // a small type-in popup once the drag is released so the
                            // user can refine the angle exactly.
                            if (m_sketchGizmoHandle == SketchGizmoHandle::Rotate) {
                                char angBuf[24];
                                std::snprintf(angBuf, sizeof(angBuf), "%.1f\xC2\xB0",
                                              m_sketchGizmoRotateDegrees);
                                ImVec2 ts = ImGui::CalcTextSize(angBuf);
                                ImVec2 lp(sc.x + 14.0f, sc.y - 14.0f - ts.y);
                                gdl->AddRectFilled(ImVec2(lp.x - 4, lp.y - 2),
                                                   ImVec2(lp.x + ts.x + 4, lp.y + ts.y + 2),
                                                   IM_COL32(20, 20, 28, 220), 3.0f);
                                gdl->AddText(lp, IM_COL32(255, 240, 80, 255), angBuf);
                            }

                            // Track the popup's screen-space anchor each frame so
                            // the camera can pan/orbit and the popup follows the
                            // centroid. Actual popup rendering happens at top scope
                            // below — keeping it out of these nested ifs is what
                            // killed the hover flicker.
                            m_sketchGizmoAdjustAnchor = glm::vec2(sc.x + 20.0f, sc.y + 16.0f);

                            // Start drag: clicking a handle arms the gizmo, snapshots
                            // the involved points, and stops the click from reaching
                            // the SketchTool below. Ctrl+click is the multi-select
                            // modifier for sketch elements — never intercept it for
                            // the gizmo, even if the click happens to land on a
                            // handle (the ring is large and crosses sketch lines).
                            if (!gizmoOwnsInput && hover != SketchGizmoHandle::None &&
                                !io.KeyCtrl &&
                                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                m_sketchGizmoHandle = hover;
                                m_sketchGizmoCenter = c;
                                m_sketchGizmoAnchor = sketchCoord;
                                m_sketchGizmoBefore = std::make_shared<Sketch>(*m_activeSketch);
                                m_sketchGizmoOriginals.clear();
                                for (int id : involved)
                                    if (auto* p = m_activeSketch->getPoint(id))
                                        m_sketchGizmoOriginals.push_back({id, p->pos});
                                gizmoOwnsInput = true;
                            }
                        }
                    }
                }

                // Apply the gizmo drag (skipped once Rotate has transitioned to
                // popup-adjust mode — there's no live drag anymore at that point).
                // MoveX/MoveY clamp the cursor delta to the chosen sketch axis and
                // snap the resulting centroid to grid; MoveFree snaps both axes;
                // Rotate spins around the centroid with a 15° soft snap.
                if (gizmoOwnsInput && !m_sketchGizmoRotateAdjusting) {
                    glm::vec2 cur = sketchCoord;
                    if (m_sketchGizmoHandle == SketchGizmoHandle::Rotate) {
                        glm::vec2 v0 = m_sketchGizmoAnchor - m_sketchGizmoCenter;
                        glm::vec2 v1 = cur                  - m_sketchGizmoCenter;
                        if (glm::length(v0) > 1e-4f && glm::length(v1) > 1e-4f) {
                            float angRad = std::atan2(v1.y, v1.x) - std::atan2(v0.y, v0.x);
                            float angDeg = angRad * 180.0f / static_cast<float>(M_PI);
                            // Soft 15° snap: lock when within 3° of an increment so
                            // the user can ride the snap without it feeling sticky.
                            float snapped = std::round(angDeg / 15.0f) * 15.0f;
                            if (std::abs(angDeg - snapped) < 3.0f) angDeg = snapped;
                            m_sketchGizmoRotateDegrees = angDeg;
                            float rad = angDeg * static_cast<float>(M_PI) / 180.0f;
                            float ca = std::cos(rad), sa = std::sin(rad);
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                glm::vec2 d = orig - m_sketchGizmoCenter;
                                glm::vec2 r(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
                                m_activeSketch->movePoint(id, m_sketchGizmoCenter + r);
                            }
                        }
                    } else {
                        glm::vec2 delta = cur - m_sketchGizmoAnchor;
                        if (m_sketchGizmoHandle == SketchGizmoHandle::MoveX) delta.y = 0.0f;
                        else if (m_sketchGizmoHandle == SketchGizmoHandle::MoveY) delta.x = 0.0f;
                        // Snap the new centroid (on the unconstrained axes) so the
                        // numbers land on grid lines.
                        float step = m_sketchTool->getGridStep();
                        if (step > 0.0f) {
                            glm::vec2 nc = m_sketchGizmoCenter + delta;
                            if (m_sketchGizmoHandle != SketchGizmoHandle::MoveY)
                                nc.x = std::round(nc.x / step) * step;
                            if (m_sketchGizmoHandle != SketchGizmoHandle::MoveX)
                                nc.y = std::round(nc.y / step) * step;
                            delta = nc - m_sketchGizmoCenter;
                        }
                        for (auto& [id, orig] : m_sketchGizmoOriginals)
                            m_activeSketch->movePoint(id, orig + delta);
                    }
                    // Re-solve so dimensional/geometric constraints hold while
                    // the gizmo drags geometry — otherwise a constrained
                    // circle/line could be dragged off its dimension and the
                    // stale value would keep displaying (the select-drag path
                    // already solves via SketchTool::onMouseMove; the gizmo
                    // path bypassed it).
                    if (m_sketchSolver) m_sketchSolver->solve(*m_activeSketch);

                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                        if (m_sketchGizmoHandle == SketchGizmoHandle::Rotate) {
                            // Transition to popup-adjust mode instead of committing
                            // straight away — the user can refine the angle by
                            // typing, then Apply / Enter commits. Use a real ImGui
                            // popup so focus semantics don't fight the viewport
                            // window (a Begin/End float here flickers on hover).
                            std::snprintf(m_sketchGizmoRotateBuf,
                                          sizeof(m_sketchGizmoRotateBuf),
                                          "%.1f", m_sketchGizmoRotateDegrees);
                            m_sketchGizmoRotateAdjusting = true;
                            ImGui::OpenPopup("##SketchRotateAdjust");
                        } else {
                            bool changed = false;
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                if (auto* p = m_activeSketch->getPoint(id))
                                    if (glm::distance(p->pos, orig) > 1e-5f) {
                                        changed = true; break;
                                    }
                            }
                            if (changed) {
                                auto after = std::make_shared<Sketch>(*m_activeSketch);
                                auto op = std::make_unique<SketchEditOp>(
                                    m_activeSketch, m_sketchGizmoBefore, after);
                                m_history->pushExecuted(std::move(op));
                                // Cascade the gizmo move/rotate to any body built
                                // from this sketch. Point-drag and dimensional edits
                                // already publish this; the gizmo path didn't, so a
                                // gizmo-moved line/circle changed the sketch but left
                                // the body stale.
                                if (m_eventBus && m_activeSketchId >= 0)
                                    m_eventBus->publish(SketchEditedEvent{m_activeSketchId});
                                m_meshesDirty = true;
                            }
                            m_sketchGizmoHandle = SketchGizmoHandle::None;
                            m_sketchGizmoBefore.reset();
                            m_sketchGizmoOriginals.clear();
                        }
                    }
                }

                // === Rotate-angle adjust popup ====================================
                // Real ImGui popup (OpenPopup/BeginPopup) instead of a Begin/End
                // floating window: focus, hover hit-test, and z-order all behave
                // correctly when rendered at top scope. The earlier nested-Begin
                // approach flickered when the cursor hovered the popup.
                if (m_sketchGizmoRotateAdjusting) {
                    ImGui::SetNextWindowPos(ImVec2(m_sketchGizmoAdjustAnchor.x,
                                                   m_sketchGizmoAdjustAnchor.y),
                                            ImGuiCond_Appearing);
                    // Restore normal padding — the viewport window's
                    // WindowPadding(0,0) is still pushed here, and the popup
                    // captures the style at its own Begin.
                    ImGui::PushStyleVar(
                        ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0f * uiScale(), 8.0f * uiScale()));
                    if (ImGui::BeginPopup("##SketchRotateAdjust",
                                          ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.30f, 1.0f), "%s", materializr::tr("Rotation (deg)"));
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                        float rotPadV = m_sketchGizmoRotateDegrees;
                        (void)materializr::parseFinite(m_sketchGizmoRotateBuf,
                                                       rotPadV);
                        bool typedEnter = materializr::inputNumber(
                            "##sketchRotAng", &rotPadV, 0.0f, 0.0f, "%.1f",
                            ImGuiInputTextFlags_EnterReturnsTrue);
                        if (typedEnter)
                            std::snprintf(m_sketchGizmoRotateBuf,
                                          sizeof(m_sketchGizmoRotateBuf),
                                          "%.6g", rotPadV);
                        // Pad commit counts as the click-off: the buffer was
                        // just rewritten, so the same live re-apply runs.
                        if (typedEnter || ImGui::IsItemDeactivatedAfterEdit()) {
                            // Re-apply the typed value live as the user clicks off
                            // the field, so the sketch reflects what they typed.
                            float deg = m_sketchGizmoRotateDegrees;
                            (void)materializr::parseFinite(m_sketchGizmoRotateBuf, deg);
                            m_sketchGizmoRotateDegrees = deg;
                            float rad = deg * static_cast<float>(M_PI) / 180.0f;
                            float ca = std::cos(rad), sa = std::sin(rad);
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                glm::vec2 d = orig - m_sketchGizmoCenter;
                                glm::vec2 r(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
                                m_activeSketch->movePoint(id, m_sketchGizmoCenter + r);
                            }
                        }
                        ImGui::Separator();
                        bool apply  = ImGui::Button(materializr::tr("Apply"), materializr::uiSz(70, 0)) || typedEnter;
                        ImGui::SameLine();
                        bool cancel = ImGui::Button(materializr::tr("Cancel"), materializr::uiSz(70, 0));

                        if (apply) {
                            float deg = m_sketchGizmoRotateDegrees;
                            (void)materializr::parseFinite(m_sketchGizmoRotateBuf, deg);
                            float rad = deg * static_cast<float>(M_PI) / 180.0f;
                            float ca = std::cos(rad), sa = std::sin(rad);
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                glm::vec2 d = orig - m_sketchGizmoCenter;
                                glm::vec2 r(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
                                m_activeSketch->movePoint(id, m_sketchGizmoCenter + r);
                            }
                            bool changed = false;
                            for (auto& [id, orig] : m_sketchGizmoOriginals) {
                                if (auto* p = m_activeSketch->getPoint(id))
                                    if (glm::distance(p->pos, orig) > 1e-5f) {
                                        changed = true; break;
                                    }
                            }
                            if (changed) {
                                auto after = std::make_shared<Sketch>(*m_activeSketch);
                                auto op = std::make_unique<SketchEditOp>(
                                    m_activeSketch, m_sketchGizmoBefore, after);
                                m_history->pushExecuted(std::move(op));
                                // Cascade the gizmo move/rotate to any body built
                                // from this sketch. Point-drag and dimensional edits
                                // already publish this; the gizmo path didn't, so a
                                // gizmo-moved line/circle changed the sketch but left
                                // the body stale.
                                if (m_eventBus && m_activeSketchId >= 0)
                                    m_eventBus->publish(SketchEditedEvent{m_activeSketchId});
                                m_meshesDirty = true;
                            }
                            m_sketchGizmoHandle = SketchGizmoHandle::None;
                            m_sketchGizmoBefore.reset();
                            m_sketchGizmoOriginals.clear();
                            m_sketchGizmoRotateAdjusting = false;
                            ImGui::CloseCurrentPopup();
                        } else if (cancel) {
                            for (auto& [id, orig] : m_sketchGizmoOriginals)
                                m_activeSketch->movePoint(id, orig);
                            m_sketchGizmoHandle = SketchGizmoHandle::None;
                            m_sketchGizmoBefore.reset();
                            m_sketchGizmoOriginals.clear();
                            m_sketchGizmoRotateAdjusting = false;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    } else {
                        // Click-outside auto-closed the popup → treat as commit
                        // (whatever angle is currently applied stays). This is the
                        // friendlier default for a CAD adjust popup; Esc reverts
                        // via the shortcut handler.
                        bool changed = false;
                        for (auto& [id, orig] : m_sketchGizmoOriginals) {
                            if (auto* p = m_activeSketch->getPoint(id))
                                if (glm::distance(p->pos, orig) > 1e-5f) {
                                    changed = true; break;
                                }
                        }
                        if (changed) {
                            auto after = std::make_shared<Sketch>(*m_activeSketch);
                            auto op = std::make_unique<SketchEditOp>(
                                m_activeSketch, m_sketchGizmoBefore, after);
                            m_history->pushExecuted(std::move(op));
                        }
                        m_sketchGizmoHandle = SketchGizmoHandle::None;
                        m_sketchGizmoBefore.reset();
                        m_sketchGizmoOriginals.clear();
                        m_sketchGizmoRotateAdjusting = false;
                    }
                    ImGui::PopStyleVar(); // ##SketchRotateAdjust WindowPadding
                }

                // Hit-test helper shared by box-select start and chain-select
                // (double-click). Mirrors handleSelectTool: nearest point first,
                // then a nearby line, within the same tolerance.
                auto pickSketchAt = [&](glm::vec2 pos, int& outPointId, int& outLineId,
                                        int& outCurveId) {
                    outPointId = -1; outLineId = -1; outCurveId = -1;
                    const float pointTol = 0.3f; // matches SketchTool::findCoincidentPoint
                    for (const auto& pt : m_activeSketch->getPoints()) {
                        if (glm::length(pos - pt.pos) < pointTol) { outPointId = pt.id; return; }
                    }
                    float bestD = 0.0f;
                    const float lineTol = std::max(m_sketchTool->getGridStep() * 0.5f, 0.5f);
                    for (const auto& l : m_activeSketch->getLines()) {
                        const SketchPoint* a = m_activeSketch->getPoint(l.startPointId);
                        const SketchPoint* b = m_activeSketch->getPoint(l.endPointId);
                        if (!a || !b) continue;
                        glm::vec2 ab = b->pos - a->pos;
                        float len2 = glm::dot(ab, ab);
                        if (len2 < 1e-12f) continue;
                        float t = glm::clamp(glm::dot(pos - a->pos, ab) / len2, 0.0f, 1.0f);
                        glm::vec2 proj = a->pos + ab * t;
                        float d = glm::distance(proj, pos);
                        if (d < lineTol && (outLineId < 0 || d < bestD)) {
                            outLineId = l.id; bestD = d;
                        }
                    }
                    // Circle / arc perimeters — only consulted when no line landed,
                    // mirroring SketchTool::handleSelectTool. Clicking the blue rim
                    // (not just the tiny centre point) counts as hitting the element.
                    if (outLineId < 0) {
                        float bestC = 0.0f;
                        for (const auto& c : m_activeSketch->getCircles()) {
                            const SketchPoint* ctr = m_activeSketch->getPoint(c.centerPointId);
                            if (!ctr) continue;
                            float d = std::abs(glm::distance(pos, ctr->pos) -
                                               static_cast<float>(c.radius));
                            if (d < lineTol && (outCurveId < 0 || d < bestC)) {
                                outCurveId = c.id; bestC = d;
                            }
                        }
                        for (const auto& a : m_activeSketch->getArcs()) {
                            const SketchPoint* ctr = m_activeSketch->getPoint(a.centerPointId);
                            if (!ctr) continue;
                            float d = std::abs(glm::distance(pos, ctr->pos) -
                                               static_cast<float>(a.radius));
                            if (d < lineTol && (outCurveId < 0 || d < bestC)) {
                                outCurveId = a.id; bestC = d;
                            }
                        }
                    }
                };

                if (gizmoOwnsInput) {
                    // Suppress normal sketch input while the gizmo owns the drag /
                    // popup adjust.
                } else if (m_sketchTool->getMode() == SketchToolMode::Select &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    // Double-click behaviour:
                    //   on a line → select every line in its connected chain
                    //               (lines sharing endpoints, transitively),
                    //   empty space → select every element in the sketch.
                    int hitPt = -1, hitLn = -1, hitCv = -1;
                    pickSketchAt(sketchCoord, hitPt, hitLn, hitCv);
                    if (hitLn >= 0) {
                        // BFS over lines via shared endpoint ids.
                        std::set<int> selPts, selLns;
                        std::vector<int> stack{hitLn};
                        while (!stack.empty()) {
                            int lid = stack.back(); stack.pop_back();
                            if (!selLns.insert(lid).second) continue;
                            for (const auto& l : m_activeSketch->getLines()) {
                                if (l.id != lid) continue;
                                selPts.insert(l.startPointId);
                                selPts.insert(l.endPointId);
                                // walk neighbours via the two endpoints
                                for (const auto& n : m_activeSketch->getLines()) {
                                    if (n.id == lid || selLns.count(n.id)) continue;
                                    if (n.startPointId == l.startPointId ||
                                        n.startPointId == l.endPointId   ||
                                        n.endPointId   == l.startPointId ||
                                        n.endPointId   == l.endPointId) {
                                        stack.push_back(n.id);
                                    }
                                }
                                break;
                            }
                        }
                        m_sketchTool->setSelection(selPts, selLns);
                    } else {
                        m_sketchTool->selectAll();
                    }
                    m_sketchDragBefore.reset(); // not a drag
                } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                    // Right-click in sketch mode opens the constraint context menu
                    // — but only when (a) the click wasn't a camera-orbit drag and
                    // (b) the user has at least one sketch element selected. The
                    // formal-constraints UI lives entirely here so the toolbar
                    // stays clean for newcomers.
                    ImVec2 rDrag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
                    bool wasOrbiting = (std::abs(rDrag.x) > 1.0f || std::abs(rDrag.y) > 1.0f);
                    if (!wasOrbiting && m_sketchTool->hasElementSelection()) {
                        m_sketchCtxMenuPending = true;
                    }
                } else if (m_sketchTool->getMode() == SketchToolMode::Spline &&
                           m_sketchTool->isPlacing() &&
                           ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    // Double-click anywhere ends the in-progress spline
                    // placement (commits with the points already placed)
                    // without leaving sketch mode. The existing "click the
                    // last control point again" exit works but only when the
                    // user lands within 0.4 mm of the previous click —
                    // double-clicking is the universal "I'm done" gesture
                    // (Inkscape / SketchUp / etc.). (Steve: "ending splines
                    // is awkward — click, Enter, click instead of just
                    // clicking the same point twice".)
                    recordSketchMutation([&]{ m_sketchTool->onConfirm(); });
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                           !m_dimEditingClickedThisFrame && !m_moveModeToggle) {
                    // m_dimEditingClickedThisFrame is set when a click landed
                    // on a dimension-label hit-rect this frame; swallow the
                    // click here so it doesn't ALSO place a sketch point at
                    // that screen position underneath the popup.
                    // Pick first to decide between element-drag and box-select.
                    bool tryBoxSelect = (m_sketchTool->getMode() == SketchToolMode::Select);
                    int hitPt = -1, hitLn = -1, hitCv = -1;
                    if (tryBoxSelect) pickSketchAt(sketchCoord, hitPt, hitLn, hitCv);
                    bool hitElement = (hitPt >= 0 || hitLn >= 0 || hitCv >= 0);

                    if (tryBoxSelect && !hitElement && m_boxSelect) {
                        // Empty space in Select mode → start a box-select drag.
                        // Don't clear the existing selection yet; the release
                        // handler does that based on whether the rect is tiny.
                        m_boxSelect->begin(glm::vec2(localX, localY));
                        m_sketchBoxSelectActive = true;
                        m_sketchDragBefore.reset();
                    } else if (m_sketchTool->getMode() == SketchToolMode::Select) {
                        // Select/drag mutates point positions only — no structural
                        // change — so recordSketchMutation's signature wouldn't see
                        // it. Snapshot manually for the drag-commit on mouse-up.
                        m_sketchDragBefore = std::make_shared<Sketch>(*m_activeSketch);
                        recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                    } else if (m_sketchTool->getMode() == SketchToolMode::Dimension) {
                        // Picking mutates nothing — no undo record. The commit
                        // below records the constraint add as one SketchEditOp.
                        // A click that landed while the ##DimEdit popup was up
                        // belongs to the popup (dismiss) — never a fresh pick.
                        if (!m_dimPopupSwallowClick) {
                            m_sketchTool->onMouseDown(sketchCoord, false);
                            if (m_sketchTool->dimReadyToCommit())
                                applyPendingDimension();
                            // Surface a refused pick instead of letting the
                            // click look like it simply missed the geometry.
                            if (const char* why = m_sketchTool->consumeDimRejection())
                                showToast(why, 2.5);
                        }
                    } else if (materializr::touchMode()) {
                        // A held circle awaiting its ✗/✓ bubble: drawing the
                        // next shape auto-commits it as-released (the bubble
                        // is an offer, not a gate).
                        if (m_sketchShapeConfirmPending) {
                            if (m_sketchTool->isPlacing())
                                recordSketchMutation([&]{
                                    m_sketchTool->onMouseDown(
                                        m_sketchShapePendingPos, false); });
                            m_sketchShapeConfirmPending = false;
                        }
                        // Drawing tool, touch: press-drag-release. The point normally
                        // lands on release so the drag can preview the radius / bulge
                        // / segment (touch has no hover to preview between taps).
                        m_sketchPressActive = true;
                        m_sketchDownX = io.MousePos.x;
                        m_sketchDownY = io.MousePos.y;
                        m_sketchDragCenterPlaced = false;
                        // Remember the undo depth before any pre-place below, so a
                        // two-finger gesture taking this press over can roll back
                        // exactly what it added (see the release handler).
                        m_sketchPressStepBefore =
                            m_history ? m_history->stepCount() : 0;
                        // The drag tools (circle/rectangle) are a single
                        // click-drag-release gesture: drop the centre / first
                        // corner now, on press, so the drag sizes the shape and
                        // the release commits it.
                        //
                        // Line gets the same press-drag-release feel for its
                        // FIRST vertex: dropping the start point on press lets the
                        // drag rubber-band the segment (with the live length read-
                        // out) and the release commit the end — instead of the
                        // unnatural tap-here, tap-there. Only the first vertex
                        // pre-places (guarded by !isPlacing); once a chain is going
                        // its start is already the previous endpoint, so each
                        // further segment just previews from there and commits its
                        // point on release. Tap-tap still works as a fallback, and
                        // polyline chaining / auto-close are untouched.
                        //
                        // Arc rides the same path for its first two points: press
                        // drops the start, the drag previews the chord, release
                        // sets the end — then a second tap sets the bulge/angle.
                        SketchToolMode m = m_sketchTool->getMode();
                        if ((m == SketchToolMode::Circle || m == SketchToolMode::Rectangle ||
                             m == SketchToolMode::Line || m == SketchToolMode::Arc) &&
                            !m_sketchTool->isPlacing()) {
                            recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                            m_sketchDragCenterPlaced = true;
                        }
                    } else if (m_sketchTool->getMode() == SketchToolMode::Line) {
                        // Line, desktop: press-drag-release, coexisting with
                        // click-chain (#25). Drop the start on press if fresh; the
                        // release commits the endpoint — a drag draws one segment,
                        // click-click chains a polyline. Tracking mirrors the touch
                        // path so the release handler can measure the drag.
                        m_sketchPressActive = true;
                        m_sketchDownX = io.MousePos.x;
                        m_sketchDownY = io.MousePos.y;
                        m_sketchDragCenterPlaced = false;
                        if (!m_sketchTool->isPlacing()) {
                            recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                            m_sketchDragCenterPlaced = true;
                        }
                    } else {
                        // Drawing tool, desktop: place on press; hover previews.
                        recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                    }
                }

                // Box-select drag/release in sketch mode.
                if (m_sketchBoxSelectActive && m_boxSelect && m_boxSelect->isActive()) {
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                        m_boxSelect->update(glm::vec2(localX, localY));
                    }
                    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                        glm::vec2 mn = m_boxSelect->getMin();
                        glm::vec2 mx = m_boxSelect->getMax();
                        m_boxSelect->end();
                        m_sketchBoxSelectActive = false;

                        if (glm::distance(mn, mx) < 4.0f) {
                            // Tiny rect — plain click on empty space. Clear unless Ctrl.
                            if (!io.KeyCtrl) m_sketchTool->clearElementSelection();
                        } else {
                            // Project sketch points to viewport-local screen pixels,
                            // pick anything that lands in the rectangle, plus any
                            // line whose endpoints' AABB overlaps the rectangle.
                            const gp_Ax3& ax = m_activeSketch->getPlane().Position();
                            glm::vec3 O(ax.Location().X(), ax.Location().Y(), ax.Location().Z());
                            glm::vec3 Xw(ax.XDirection().X(), ax.XDirection().Y(), ax.XDirection().Z());
                            glm::vec3 Yw(ax.YDirection().X(), ax.YDirection().Y(), ax.YDirection().Z());
                            glm::mat4 vp = proj * view;
                            auto projectPt = [&](glm::vec2 sp, glm::vec2& out) -> bool {
                                glm::vec3 w = O + sp.x * Xw + sp.y * Yw;
                                glm::vec4 cp = vp * glm::vec4(w, 1.0f);
                                if (cp.w <= 1e-5f) return false;
                                out = glm::vec2((cp.x / cp.w * 0.5f + 0.5f) * contentSize.x,
                                                (1.0f - (cp.y / cp.w * 0.5f + 0.5f)) * contentSize.y);
                                return true;
                            };

                            std::set<int> selPts = io.KeyCtrl ? m_sketchTool->getSelectedPoints()
                                                              : std::set<int>{};
                            std::set<int> selLns = io.KeyCtrl ? m_sketchTool->getSelectedLines()
                                                              : std::set<int>{};
                            // Cache projected positions so the line test reuses them.
                            std::map<int, glm::vec2> ptScreen;
                            for (const auto& p : m_activeSketch->getPoints()) {
                                glm::vec2 sp;
                                if (!projectPt(p.pos, sp)) continue;
                                ptScreen[p.id] = sp;
                                if (sp.x >= mn.x && sp.x <= mx.x &&
                                    sp.y >= mn.y && sp.y <= mx.y) {
                                    selPts.insert(p.id);
                                }
                            }
                            for (const auto& l : m_activeSketch->getLines()) {
                                auto a = ptScreen.find(l.startPointId);
                                auto b = ptScreen.find(l.endPointId);
                                if (a == ptScreen.end() || b == ptScreen.end()) continue;
                                glm::vec2 lmin = glm::min(a->second, b->second);
                                glm::vec2 lmax = glm::max(a->second, b->second);
                                if (lmax.x >= mn.x && lmin.x <= mx.x &&
                                    lmax.y >= mn.y && lmin.y <= mx.y) {
                                    selLns.insert(l.id);
                                }
                            }
                            // Curves were previously skipped — box-select only
                            // caught points + lines. Test each curve's projected
                            // sample points against the rect so a drag grabs
                            // circles, arcs and splines too.
                            std::set<int> selCircs = io.KeyCtrl ? m_sketchTool->getSelectedCircles() : std::set<int>{};
                            std::set<int> selArcs  = io.KeyCtrl ? m_sketchTool->getSelectedArcs()    : std::set<int>{};
                            std::set<int> selSpls  = io.KeyCtrl ? m_sketchTool->getSelectedSplines() : std::set<int>{};
                            auto anySampleInRect = [&](const std::vector<glm::vec2>& s2d) {
                                for (glm::vec2 q : s2d) {
                                    glm::vec2 sp;
                                    if (projectPt(q, sp) &&
                                        sp.x >= mn.x && sp.x <= mx.x &&
                                        sp.y >= mn.y && sp.y <= mx.y)
                                        return true;
                                }
                                return false;
                            };
                            for (const auto& c : m_activeSketch->getCircles()) {
                                const SketchPoint* ctr = m_activeSketch->getPoint(c.centerPointId);
                                if (!ctr) continue;
                                std::vector<glm::vec2> ring;
                                for (int i = 0; i < 16; ++i) {
                                    float t = 2.0f * 3.14159265f * i / 16;
                                    ring.push_back(ctr->pos + glm::vec2(std::cos(t), std::sin(t)) *
                                                            static_cast<float>(c.radius));
                                }
                                if (anySampleInRect(ring)) selCircs.insert(c.id);
                            }
                            for (const auto& a : m_activeSketch->getArcs()) {
                                const SketchPoint* ctr = m_activeSketch->getPoint(a.centerPointId);
                                const SketchPoint* sp0 = m_activeSketch->getPoint(a.startPointId);
                                const SketchPoint* ep0 = m_activeSketch->getPoint(a.endPointId);
                                if (!ctr || !sp0 || !ep0) continue;
                                float a0 = std::atan2(sp0->pos.y - ctr->pos.y, sp0->pos.x - ctr->pos.x);
                                float a1 = std::atan2(ep0->pos.y - ctr->pos.y, ep0->pos.x - ctr->pos.x);
                                while (a1 < a0) a1 += 2.0f * 3.14159265f;
                                std::vector<glm::vec2> samp;
                                for (int i = 0; i <= 16; ++i) {
                                    float t = a0 + (a1 - a0) * i / 16;
                                    samp.push_back(ctr->pos + glm::vec2(std::cos(t), std::sin(t)) *
                                                            static_cast<float>(a.radius));
                                }
                                if (anySampleInRect(samp)) selArcs.insert(a.id);
                            }
                            for (const auto& sp : m_activeSketch->getSplines())
                                if (anySampleInRect(m_activeSketch->sampleSpline2D(sp, 12)))
                                    selSpls.insert(sp.id);
                            m_sketchTool->setSelectionFull(selPts, selLns, selCircs, selArcs, selSpls);
                        }
                    }
                }

                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !m_sketchBoxSelectActive) {
                    m_sketchTool->onMouseUp(sketchCoord);
                    if (materializr::touchMode()) {
                    // Press-drag-release: place the drawing tool's point now, at
                    // the release position (the preview followed the drag here).
                    // Skip if the release was a two-finger gesture taking over,
                    // not a genuine lift.
                    if (m_sketchPressActive &&
                        m_sketchTool->getMode() != SketchToolMode::Select &&
                        !m_moveModeToggle &&
                        m_window && !m_window->lastLeftReleaseWasGesture()) {
                        float ddx = io.MousePos.x - m_sketchDownX;
                        float ddy = io.MousePos.y - m_sketchDownY;
                        float slop = 12.0f * (m_window ? m_window->uiScale() : 1.0f);
                        bool moved = (ddx * ddx + ddy * ddy) > slop * slop;
                        if (m_sketchDragCenterPlaced) {
                            // A tool whose first point dropped on press (circle/
                            // rectangle centre, or line's start vertex): complete
                            // it only if the finger dragged out a size/length. A
                            // no-drag tap leaves it placing, so a second tap sets
                            // the radius / corner / endpoint — a stationary tap
                            // mustn't commit a zero-size shape (tap-tap still works).
                            if (moved) {
                                const SketchToolMode sm = m_sketchTool->getMode();
                                if (imTouchLayout() &&
                                    (sm == SketchToolMode::Circle ||
                                     sm == SketchToolMode::Rectangle)) {
                                    // im-touch: hold the shape as a preview and
                                    // offer the ✗/✓ + exact-value bubble instead
                                    // of committing on lift. Modern uses the
                                    // inline dimension field (Steve's choice),
                                    // not this bubble.
                                    m_sketchShapeConfirmPending = true;
                                    m_sketchShapePendingPos = sketchCoord;
                                    m_sketchShapeDimBuf[0] = '\0';
                                    // Freeze the preview endpoints at lift —
                                    // the bubble anchors to these so it can't
                                    // move if the held preview twitches.
                                    // (Preview start is the EFFECTIVE opposite
                                    // corner in both rect draw modes, so
                                    // |pe-ps| is the full side.)
                                    const glm::vec2 ps = m_sketchTool->getPreviewStart();
                                    const glm::vec2 pe = m_sketchTool->getPreviewEnd();
                                    m_sketchShapeAnchorPs = ps;
                                    m_sketchShapeAnchorPe = pe;
                                    m_sketchShapeDimW = std::abs(pe.x - ps.x);
                                    m_sketchShapeDimH = std::abs(pe.y - ps.y);
                                } else {
                                    recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                                    // LINE: a genuine drag draws ONE segment — end
                                    // the chain so a lone line doesn't leave the
                                    // tool placing. Tap-tap still chains (#25).
                                    if (m_sketchTool->getMode() == SketchToolMode::Line)
                                        m_sketchTool->onConfirm();
                                }
                            }
                        } else if (m_sketchTool->getMode() == SketchToolMode::Line &&
                                   m_sketchTool->isPlacing()) {
                            // LINE, chain live: commit the segment at the tap /
                            // release position — no typing required (#25). A drag
                            // draws a single segment and ends the chain; a
                            // stationary tap commits and keeps chaining a polyline.
                            // A zero-length tap on the anchor is rejected inside
                            // handleLineTool. (Typing an exact length via the
                            // dimension field stays available, just no longer the
                            // only way to commit.)
                            recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                            if (moved) m_sketchTool->onConfirm();
                        } else {
                            // Multi-point tool (incl. a drag-committed line
                            // segment), or a drag tool's second tap: place the
                            // point at release.
                            recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                        }
                    } else if (m_sketchPressActive &&
                               m_sketchTool->getMode() != SketchToolMode::Select &&
                               !m_moveModeToggle &&
                               m_window && m_window->lastLeftReleaseWasGesture() &&
                               m_sketchTool->isPlacing()) {
                        // A two-finger pan/zoom began right after this finger's
                        // press started a drawing placement. Roll that placement
                        // back so two-finger navigation needs no Move button and
                        // leaves nothing behind — no stray start vertex, and no
                        // half-placed state to corrupt the next tap. Undo the step
                        // the press pushed (Line drops its fresh start vertex here;
                        // circle/rect/arc push nothing, so the guard skips), then
                        // clear the tool's in-progress placement.
                        if (m_history &&
                            m_history->stepCount() > m_sketchPressStepBefore &&
                            m_history->canUndo() &&
                            (!m_inSketchMode ||
                             m_history->currentStep() > m_sketchEntryHistoryStep)) {
                            m_history->undo(*m_document);
                        }
                        m_sketchTool->onCancel();
                        if (m_activeSketch) {
                            m_activeSketch->pruneOrphanPoints();
                            if (m_activeSketchId >= 0)
                                cascadeFromSketchEdit(m_activeSketchId);
                        }
                        m_meshesDirty = true;
                    }
                    m_sketchPressActive = false;
                    m_sketchDragCenterPlaced = false;
                    }
                    else if (m_sketchPressActive &&
                             m_sketchTool->getMode() == SketchToolMode::Line) {
                        // Desktop line press-drag-release (#25): commit the
                        // endpoint at the release position. A stationary click
                        // that only just dropped the fresh start waits for the
                        // next click (click-chain); otherwise commit — and a
                        // genuine drag ends the chain (a single segment).
                        float ddx = io.MousePos.x - m_sketchDownX;
                        float ddy = io.MousePos.y - m_sketchDownY;
                        float slop = 5.0f * (m_window ? m_window->uiScale() : 1.0f);
                        bool moved = (ddx * ddx + ddy * ddy) > slop * slop;
                        if (!(m_sketchDragCenterPlaced && !moved)) {
                            recordSketchMutation([&]{ m_sketchTool->onMouseDown(sketchCoord, io.KeyCtrl); });
                            if (moved) m_sketchTool->onConfirm();
                        }
                        m_sketchPressActive = false;
                        m_sketchDragCenterPlaced = false;
                    }
                    if (m_sketchDragBefore) {
                        // Compare point positions; commit a SketchEditOp if any moved.
                        const auto& before = m_sketchDragBefore->getPoints();
                        const auto& after  = m_activeSketch->getPoints();
                        bool changed = (before.size() != after.size());
                        for (size_t i = 0; !changed && i < before.size(); ++i) {
                            if (glm::distance(before[i].pos, after[i].pos) > 1e-5f)
                                changed = true;
                        }
                        if (changed) {
                            auto after_ptr = std::make_shared<Sketch>(*m_activeSketch);
                            auto op = std::make_unique<SketchEditOp>(
                                m_activeSketch, m_sketchDragBefore, after_ptr);
                            m_history->pushExecuted(std::move(op));
                            // Cascade the move to any body built from this sketch.
                            // Dimensional edits (circle Ø / constraints) publish
                            // this so the body follows; a drag-move pushed the
                            // SketchEditOp but never published, so moving a line/
                            // circle changed the sketch but left the body stale.
                            if (m_eventBus && m_activeSketchId >= 0)
                                m_eventBus->publish(
                                    SketchEditedEvent{m_activeSketchId});
                            m_meshesDirty = true;
                        }
                        m_sketchDragBefore.reset();
                    }
                }
                if (!gizmoOwnsInput) {
                    // Drive the hover-to-charge dwell timer every frame (even
                    // when the cursor is still) so a paused hover can charge a
                    // reference point; then update the rubber-band snap.
                    m_sketchTool->updateHoverCharge(ImGui::GetTime(), sketchCoord);
                    m_sketchTool->onMouseMove(sketchCoord);
                }
                } // if (!patternPickingNow)
            }
        }
    }

    // ViewCube overlay. In im-touch-lite the top-right button cluster floats
    // over the viewport corner where the cube lives — drop the cube below it.
    const float uisVc = uiScale();
    if (imTouchLayout())
        m_viewCube->setExtraOffset(0.0f, 68.0f * uisVc); // clear the im-touch top-right cluster
    else if (modernLayout())
        m_viewCube->setExtraOffset(8.0f * uisVc, -8.0f * uisVc); // centre Home in the corner
    else
        m_viewCube->setExtraOffset(0.0f, 0.0f);
    ViewCubeAction vcAction = m_viewCube->render(
        m_viewport->getCamera(), m_invertCubeDrag,
        m_themeManager && m_themeManager->getTheme() == Theme::Light,
        m_window && m_window->lastLeftReleaseWasGesture());
    if (vcAction != ViewCubeAction::None) {
        handleViewCubeAction(static_cast<int>(vcAction));
    }

    // Snap-grid corner widget — small square next to the ViewCube showing the
    // current grid step. Click to open settings (snap toggle + step radios);
    // changes save immediately.
    renderSnapWidget();

    // Context action bars overlaid on the viewport (touch mode). Each is a
    // SEPARATE window so a tap on — or a few px around — a button is captured by
    // ImGui and can't fall through to the canvas and drop a stray vertex. The
    // window padding is that surrounding hit-target margin.
    if (materializr::touchMode()) {
        const ImVec2 vpMin = ImGui::GetWindowPos();
        const ImVec2 vpSize = ImGui::GetWindowSize();
        const float pad = ImGui::GetStyle().FramePadding.x;
        const float btnH = ImGui::GetFrameHeight() * 1.7f;   // tall touch targets
        const ImGuiWindowFlags overlayFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;
        auto wideButton = [&](const char* label) -> bool {
            return ImGui::Button(label, ImVec2(ImGui::CalcTextSize(label).x + pad * 5.0f, btnH));
        };

        // Bottom-left: selection / sketch-shape actions.
        bool selectionContext = !m_inSketchMode ||
            (m_sketchTool && m_sketchTool->getMode() == SketchToolMode::Select);
        bool placing = m_inSketchMode && m_sketchTool && m_sketchTool->isPlacing();
        // In the modern/im-touch layouts the Multi-Select toggle is hosted in
        // their own chrome instead (down here it overlapped the FULL pill).
        // Keep the rest of this bar — Delete, and the chain-tool Finish/Back —
        // which those layouts don't replicate.
        const bool multiInLegacy = classicLayout();
        const bool deleteHere = m_inSketchMode && m_sketchTool &&
                                m_sketchTool->hasElementSelection();
        // Move (navigation lock) lives in this bar too. While on, a one-finger
        // drag orbits and taps don't draw/select, so pan/zoom can't
        // inadvertently start a drawing. Shown while a one-finger drag is
        // reserved for a tool — sketch editing, or an interactive op whose
        // arrow/handle owns the drag (push/pull, extrude, edge ops, move/scale
        // face) — since that's exactly when orbit needs an escape hatch.
        // Hidden (and forced off) elsewhere: a plain drag already orbits
        // there, so the lock is redundant. (It used to sit bottom-RIGHT, but
        // the im-touch layout parks its sketch Finish/Discard FABs in that
        // corner, which buried it.)
        const bool navLockRelevant = m_inSketchMode ||
            m_ppCtl.active() || m_extrudeCtl.active() || m_edgeCtl.active() ||
            anyIopActive();
        if (!navLockRelevant) m_moveModeToggle = false;
        if ((selectionContext && (multiInLegacy || deleteHere)) ||
            (placing && classicLayout()) || navLockRelevant) {
            // im-touch outside a sketch: the History toggle owns the very
            // corner (its timeline strip stays visible during interactive
            // ops), so sit just above it. Everywhere else, flush corner.
            const float clearHist = (imTouchLayout() && !m_inSketchMode)
                                        ? 78.0f * uiScale() : 0.0f;
            ImGui::SetNextWindowPos(ImVec2(vpMin.x + 6.0f,
                                           vpMin.y + vpSize.y - 6.0f - clearHist),
                                    ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGui::Begin("##ViewportBarLeft", nullptr, overlayFlags);

            bool anyBtn = false;
            if (navLockRelevant) {
                if (m_moveModeToggle) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.85f, 0.55f, 0.15f, 0.97f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.65f, 0.25f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.75f, 0.48f, 0.12f, 1.0f));
                }
                bool clicked = wideButton(m_moveModeToggle ? "Move: On" : "Move: Off");
                bool hov = ImGui::IsItemHovered();
                if (m_moveModeToggle) ImGui::PopStyleColor(3);
                if (clicked) m_moveModeToggle = !m_moveModeToggle;
                if (hov) ImGui::SetTooltip("%s", materializr::tr("Navigation lock: one finger orbits;\ntaps don't draw or select"));
                anyBtn = true;
            }

            if (selectionContext) {
                if (multiInLegacy) {
                    if (anyBtn) ImGui::SameLine();
                    anyBtn = true;
                    int pops = 0;
                    if (m_multiSelectToggle) {
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.48f, 0.85f, 0.95f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.58f, 0.95f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.16f, 0.40f, 0.75f, 1.0f));
                        pops = 3;
                    }
                    if (wideButton(m_multiSelectToggle ? "Multi-Select: On" : "Multi-Select: Off"))
                        m_multiSelectToggle = !m_multiSelectToggle;
                    bool hov = ImGui::IsItemHovered();
                    if (pops) ImGui::PopStyleColor(pops);
                    if (hov) ImGui::SetTooltip("%s", materializr::tr("Add taps to the current selection\n(the touch equivalent of holding Ctrl)"));
                }

                // Delete the selected sketch elements — the touch twin of the
                // Delete key (which a bare tablet doesn't have). Only shown in
                // sketch Select mode with elements actually selected.
                if (deleteHere) {
                    if (anyBtn) ImGui::SameLine();
                    anyBtn = true;
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.68f, 0.24f, 0.24f, 0.97f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.34f, 0.34f, 1.0f));
                    bool del = wideButton("Delete");
                    bool dhov = ImGui::IsItemHovered();
                    ImGui::PopStyleColor(2);
                    if (del) deleteSelectedSketchElements();
                    if (dhov) ImGui::SetTooltip("%s", materializr::tr("Delete the selected sketch elements (undoable)"));
                }
            }
            if (placing && classicLayout()) {
                SketchToolMode mode = m_sketchTool->getMode();
                // Circle/Rectangle are a single press-drag-release gesture now,
                // so their only "placing" window is mid-drag with the finger
                // down — a button there is unreachable (it just flickered in and
                // out). Show no bar for them; Undo backs out an unwanted
                // circle/rect. The bar stays for the genuinely multi-step tools:
                // line/spline chains, and arc.
                bool atomicGesture = (mode == SketchToolMode::Circle ||
                                      mode == SketchToolMode::Rectangle);
                // "Finish" commits the points placed so far (chaining tools).
                bool chainTool = (mode == SketchToolMode::Line ||
                                  mode == SketchToolMode::Spline ||
                                  mode == SketchToolMode::Polygon);
                if (!atomicGesture) {
                    bool prev = anyBtn;
                    if (chainTool) {
                        if (prev) ImGui::SameLine();
                        prev = true;
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.60f, 0.32f, 0.97f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.72f, 0.42f, 1.0f));
                        bool finish = wideButton("Finish");
                        bool fhov = ImGui::IsItemHovered();
                        ImGui::PopStyleColor(2);
                        if (finish) recordSketchMutation([&]{ m_sketchTool->onConfirm(); });
                        if (fhov) ImGui::SetTooltip("%s", materializr::tr("Finish the current shape, keeping the points placed"));
                    }
                    // "Back" drops the last placed segment / control point and
                    // keeps the chain going. Only when there is one to drop.
                    bool canBack =
                        (mode == SketchToolMode::Line && m_sketchTool->lineSegmentCount() >= 1) ||
                        (mode == SketchToolMode::Spline && !m_sketchTool->splinePointsInProgress().empty());
                    if (canBack) {
                        if (prev) ImGui::SameLine();
                        prev = true;
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.78f, 0.54f, 0.18f, 0.97f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.64f, 0.26f, 1.0f));
                        bool back = wideButton("Back");
                        bool bhov = ImGui::IsItemHovered();
                        ImGui::PopStyleColor(2);
                        if (back) sketchChainBack();
                        if (bhov) ImGui::SetTooltip("%s", materializr::tr("Remove the last segment and keep drawing"));
                    }
                    // "Cancel" — for a chain, discard the WHOLE chain; for arc,
                    // discard the in-progress shape.
                    if (prev) ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.68f, 0.24f, 0.24f, 0.97f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.34f, 0.34f, 1.0f));
                    bool cancel = wideButton(chainTool ? "Cancel" : "Cancel Shape");
                    bool chov = ImGui::IsItemHovered();
                    ImGui::PopStyleColor(2);
                    if (cancel) {
                        if (chainTool) sketchChainCancel();
                        else recordSketchMutation([&]{ m_sketchTool->onCancel(); });
                    }
                    if (chov) ImGui::SetTooltip(chainTool
                        ? "Discard the whole chain you're drawing"
                        : "Discard the in-progress shape");
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

    }

    // Right-click face context menu. The viewport window pushed
    // WindowPadding(0,0) (the 3D scene must fill the window rect), and a popup
    // captures the style at its own Begin — so without this restore the
    // context menus render their items flush against the popup edges.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0f * uiScale(), 8.0f * uiScale()));
    if (m_contextMenuPending) {
        ImGui::OpenPopup(m_contextMenuPlaneId >= 0 ? "PlaneContextMenu"
                                                   : "FaceContextMenu");
        m_contextMenuPending = false;
    }
    if (ImGui::BeginPopup("FaceContextMenu")) {
        // Two branches — Face and Body — so the user picks which the actions
        // apply to (the touch long-press can't distinguish a single-click face
        // pick from a double-click body pick the way a mouse does). Each branch
        // first selects its entity, then lists its specific actions; body-level
        // actions that aren't face-specific are dual-listed under both.
        ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Object"));
        ImGui::Separator();

        const int bid = m_contextMenuBodyId;

        // Shared body-level actions — they operate on the whole body the face
        // belongs to, so they appear under both the Face and Body branches.
        auto sharedBodyOps = [&]() {
            // Lay Flat: planar faces only -- the alignment needs a face
            // normal that means something over the whole face.
            if (!m_contextMenuFace.IsNull() &&
                m_contextMenuFace.ShapeType() == TopAbs_FACE) {
                bool planar = false;
                try {
                    planar = BRepAdaptor_Surface(TopoDS::Face(m_contextMenuFace))
                                 .GetType() == GeomAbs_Plane;
                } catch (...) {}
                if (planar && ImGui::MenuItem(materializr::tr("Lay Flat on Plane…"))) {
                    beginAlignFaceToPlane(bid, m_contextMenuFace);
                    m_contextMenuFace.Nullify();
                }
            }
            // Both actions change visibility flags, and the renderer only
            // reflects those on a rebuild — m_meshesDirty is required or the
            // menu item "doesn't seem to do anything" (markDirty() alone only
            // flags the PROJECT as unsaved). The full rebuild skips invisible
            // bodies, so post-isolate it re-tessellates just the one body.
            if (ImGui::MenuItem(materializr::tr("Isolate"))) {
                for (int o : m_document->getAllBodyIds())
                    m_document->setBodyVisible(o, o == bid);
                markDirty();
                m_meshesDirty = true;
                m_contextMenuFace.Nullify();
            }
            // The way back from Isolate — without this the only recovery is
            // re-ticking every body's checkbox in the Items panel.
            if (ImGui::MenuItem(materializr::tr("Show All Bodies"))) {
                for (int o : m_document->getAllBodyIds())
                    m_document->setBodyVisible(o, true);
                markDirty();
                m_meshesDirty = true;
                m_contextMenuFace.Nullify();
            }
            // Export ▸ — the same submenu the Items panel offers, from the
            // same registry, acting on the whole body selection when this
            // body is part of one. Two menus for the same action have to
            // stay in step: the Items panel grew the format list and this
            // one kept a lone STL item, which read as the feature not
            // working at all (Steve, 2026-07-28).
            {
                std::vector<std::string> fmts;
                for (const auto& f : PluginRegistry::instance().ioFormats())
                    if (f.canExport && f.exportDocFn) fmts.push_back(f.name);
                if (!fmts.empty() && ImGui::BeginMenu(materializr::tr("Export"))) {
                    std::vector<int> targets;
                    if (m_selection) {
                        for (const auto& e : m_selection->getSelection())
                            if (e.type == SelectionType::Body && e.bodyId >= 0)
                                targets.push_back(e.bodyId);
                    }
                    const bool multi =
                        targets.size() > 1 &&
                        std::find(targets.begin(), targets.end(), bid) != targets.end();
                    if (!multi) { targets.clear(); targets.push_back(bid); }
                    if (multi)
                        ImGui::TextDisabled(materializr::tr("%zu selected bodies"), targets.size());
                    for (const auto& fmt : fmts) {
                        if (ImGui::MenuItem((fmt + "…").c_str())) {
                            exportBodiesAs(targets, fmt);
                            m_contextMenuFace.Nullify();
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            // Baked copy of this body into a fresh project — the "use this
            // part elsewhere" flow. Single body by design (it names the new
            // file after the part), so it stays out of the Export submenu.
            if (ImGui::MenuItem(materializr::tr("Export to New Project"))) {
                std::vector<int> targets;
                if (m_selection) {
                    for (const auto& e : m_selection->getSelection())
                        if (e.type == SelectionType::Body && e.bodyId >= 0)
                            targets.push_back(e.bodyId);
                }
                if (std::find(targets.begin(), targets.end(), bid) == targets.end() ||
                    targets.size() <= 1) {
                    targets.clear();
                    targets.push_back(bid);
                }
                exportBodiesToNewProject(targets);
                m_contextMenuFace.Nullify();
            }
            // Separate: only when the body actually holds more than one
            // disconnected solid (air-gapped lumps fused into one body).
            // Mirrors the Items-panel entry — splits them into individual
            // bodies, largest keeping this one.
            TopoDS_Shape bshape;
            try { bshape = m_document->getBody(bid); } catch (...) {}
            if (m_history && SeparateBodyOp::solidCount(bshape) > 1) {
                if (ImGui::MenuItem(materializr::tr("Separate"))) {
                    auto op = std::make_unique<SeparateBodyOp>();
                    op->setBody(bid);
                    m_history->pushOperation(std::move(op), *m_document);
                    markDirty();
                    m_meshesDirty = true;
                    m_contextMenuFace.Nullify();
                }
            }
        };

        // With Multi-Select on, the menu's Select actions ADD to the current
        // selection instead of replacing it, so you can gather several
        // faces/bodies via long-press too.
        const bool addToSel = m_multiSelectToggle;

        // Select every unique edge of a shape — a face's rim, or a whole body.
        // Lets the user set up a fillet/chamfer over an entire face or body in
        // one shot. Edges are keyed by shape identity in the SelectionManager,
        // and a face's edges share the body's TShapes, so these entries are
        // IsSame to picked edges and highlight/dedup correctly. Multi-Select
        // off → replace the current selection; on → add to it.
        auto selectAllEdgesOf = [&](const TopoDS_Shape& shape) {
            if (shape.IsNull()) return;
            if (!addToSel) m_selection->clear();
            TopTools_IndexedMapOfShape edgeMap;
            TopExp::MapShapes(shape, TopAbs_EDGE, edgeMap);
            for (int i = 1; i <= edgeMap.Extent(); ++i) {
                SelectionEntry e;
                e.type = SelectionType::Edge;
                e.bodyId = bid;
                e.shape = edgeMap(i);
                m_selection->addToSelection(e);
            }
        };

        if (!m_contextMenuFace.IsNull() && ImGui::BeginMenu(materializr::tr("Face"))) {
            if (ImGui::MenuItem(materializr::tr("Select Face"))) {
                SelectionEntry entry;
                entry.type = SelectionType::Face;
                entry.bodyId = bid;
                entry.shape = m_contextMenuFace;
                if (addToSel) m_selection->addToSelection(entry);
                else          m_selection->select(entry);
                m_contextMenuFace.Nullify();
            }
            if (ImGui::MenuItem(materializr::tr("Select All Edges of Face"))) {
                // All edges bounding this face — e.g. fillet a pocket's whole rim.
                selectAllEdgesOf(m_contextMenuFace);
                m_contextMenuFace.Nullify();
            }
            if (ImGui::MenuItem(materializr::tr("Sketch on this Face"))) {
                // Select the face, then enter sketch mode (enterSketchMode reads the selection)
                SelectionEntry entry;
                entry.type = SelectionType::Face;
                entry.bodyId = bid;
                entry.shape = m_contextMenuFace;
                m_selection->select(entry);
                enterSketchMode();
                m_contextMenuFace.Nullify();
            }
            if (ImGui::MenuItem(materializr::tr("Extrude Face"))) {
                beginInteractiveExtrude(m_contextMenuFace, ExtrudeMode::NewBody, -1);
                m_contextMenuFace.Nullify();
            }
            ImGui::Separator();
            sharedBodyOps();
            ImGui::EndMenu();
        }
        if (bid >= 0 && ImGui::BeginMenu(materializr::tr("Body"))) {
            if (ImGui::MenuItem(materializr::tr("Select Body"))) {
                SelectionEntry entry;
                entry.type = SelectionType::Body;
                entry.bodyId = bid;
                try { entry.shape = m_document->getBody(bid); } catch (...) {}
                if (addToSel) m_selection->addToSelection(entry);
                else          m_selection->select(entry);
                m_contextMenuFace.Nullify();
            }
            if (ImGui::MenuItem(materializr::tr("Select All Edges of Body"))) {
                // Every edge on the body — e.g. break all sharp edges at once.
                TopoDS_Shape body;
                try { body = m_document->getBody(bid); } catch (...) {}
                selectAllEdgesOf(body);
                m_contextMenuFace.Nullify();
            }
            ImGui::Separator();
            sharedBodyOps();
            ImGui::EndMenu();
        }
        // Sketch submenu — shown when a committed sketch is in the clicked area
        // (possibly alongside a Face/Body, e.g. a sketch lying on a face). The
        // transform actions select the sketch first so the gizmo targets it.
        if (m_contextMenuSketchId >= 0 && ImGui::BeginMenu(materializr::tr("Sketch"))) {
            const int sid = m_contextMenuSketchId;
            auto selectThisSketch = [&]() {
                if (!m_selection) return;
                m_selection->clear();
                SelectionEntry e;
                e.type = SelectionType::Sketch;
                e.sketchId = sid;
                m_selection->addToSelection(e);
            };
            if (ImGui::MenuItem(materializr::tr("Edit Sketch"))) {
                editSketch(sid);
                m_contextMenuSketchId = -1;
            }
            if (ImGui::MenuItem(materializr::tr("Lay Flat on Plane…"))) {
                beginAlignSketchToPlane(sid);
                m_contextMenuSketchId = -1;
            }
            if (ImGui::MenuItem(materializr::tr("Export as SVG…"))) {
                exportSketchAsSvg(sid);
                m_contextMenuSketchId = -1;
            }
            if (ImGui::MenuItem(materializr::tr("Export as DXF…"))) {
                exportSketchAsDxf(sid);
                m_contextMenuSketchId = -1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(materializr::tr("Move"))) {
                selectThisSketch();
                handleToolAction(static_cast<int>(ToolAction::Move));
                m_contextMenuSketchId = -1;
            }
            if (ImGui::MenuItem(materializr::tr("Rotate"))) {
                selectThisSketch();
                handleToolAction(static_cast<int>(ToolAction::Rotate));
                m_contextMenuSketchId = -1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(materializr::tr("Delete"))) {
                if (m_document) m_document->removeSketch(sid);
                if (m_selection) m_selection->clear();
                markDirty();
                m_contextMenuSketchId = -1;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(materializr::tr("Cancel"))) {
            m_contextMenuFace.Nullify();
            m_contextMenuSketchId = -1;
        }
        ImGui::EndPopup();
    }

    // Right-click construction-plane context menu — the same normal-adjustment
    // actions the Items panel offers, but reachable directly on the plane in
    // the viewport.
    if (ImGui::BeginPopup("PlaneContextMenu")) {
        const int pid = m_contextMenuPlaneId;
        ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Construction Plane"));
        ImGui::Separator();
        if (ImGui::MenuItem(materializr::tr("Flip Normal"))) {
            m_document->flipPlaneNormal(pid);
            markDirty();
        }
        if (ImGui::MenuItem(materializr::tr("Rotate About Axis…"))) {
            beginRotatePlaneAboutAxis(pid);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(materializr::tr("Delete"))) {
            m_document->removePlane(pid);
            if (m_selection) m_selection->clear();
            m_contextMenuPlaneId = -1;
        }
        ImGui::EndPopup();
    }

    // Sketch constraint context menu — appears on right-click in the sketch
    // viewport when at least one sketch element is selected. Items are filtered
    // by selection arity so the user never sees an option that can't apply
    // (e.g. "Parallel" only shows with 2+ lines selected).
    if (m_sketchCtxMenuPending) {
        ImGui::OpenPopup("SketchContextMenu");
        m_sketchCtxMenuPending = false;
    }
    if (m_inSketchMode && m_sketchTool && ImGui::BeginPopup("SketchContextMenu")) {
        int nPts = static_cast<int>(m_sketchTool->getSelectedPoints().size());
        int nLns = static_cast<int>(m_sketchTool->getSelectedLines().size());
        int nCir = static_cast<int>(m_sketchTool->getSelectedCircles().size());
        int nArc = static_cast<int>(m_sketchTool->getSelectedArcs().size());
        int nCur = nCir + nArc; // any curve selection
        ImGui::TextColored(materializr::accentText(),
                           materializr::tr("Selection: %d point%s, %d line%s, %d curve%s"),
                           nPts, nPts == 1 ? "" : "s",
                           nLns, nLns == 1 ? "" : "s",
                           nCur, nCur == 1 ? "" : "s");
        ImGui::Separator();
        if (ImGui::BeginMenu(materializr::tr("Add Constraint"))) {
            if (nLns >= 1) {
                if (ImGui::MenuItem(materializr::tr("Horizontal")))
                    applySketchConstraint(ConstraintType::Horizontal);
                if (ImGui::MenuItem(materializr::tr("Vertical")))
                    applySketchConstraint(ConstraintType::Vertical);
            }
            if (nPts >= 2) {
                if (ImGui::MenuItem(materializr::tr("Coincident")))
                    applySketchConstraint(ConstraintType::Coincident);
                if (ImGui::MenuItem(materializr::tr("Distance … (current value)")))
                    applySketchConstraint(ConstraintType::Distance);
            }
            if (nLns >= 2) {
                if (ImGui::MenuItem(materializr::tr("Parallel")))
                    applySketchConstraint(ConstraintType::Parallel);
                if (ImGui::MenuItem(materializr::tr("Perpendicular")))
                    applySketchConstraint(ConstraintType::Perpendicular);
                if (ImGui::MenuItem(materializr::tr("Equal length")))
                    applySketchConstraint(ConstraintType::Equal);
                if (ImGui::MenuItem(materializr::tr("Angle … (current value)")))
                    applySketchConstraint(ConstraintType::Angle);
            }
            if (nPts >= 1) {
                if (ImGui::MenuItem(materializr::tr("Fix Position")))
                    applySketchConstraint(ConstraintType::Fixed);
            }
            if (nCur >= 1) {
                if (ImGui::MenuItem(materializr::tr("Radius … (current value)")))
                    applySketchConstraint(ConstraintType::Radius);
            }
            if (nCur >= 1 && nLns >= 1) {
                if (ImGui::MenuItem(materializr::tr("Tangent (curve + line)")))
                    applySketchConstraint(ConstraintType::Tangent);
            }
            if (nCur >= 2) {
                if (ImGui::MenuItem(materializr::tr("Concentric")))
                    applySketchConstraint(ConstraintType::Concentric);
                if (ImGui::MenuItem(materializr::tr("Equal radius")))
                    applySketchConstraint(ConstraintType::Equal);
            }
            // ImGui automatically greys out an empty submenu, but we want to
            // hint at the cause when nothing matches the selection.
            if (nPts == 0 && nLns == 0) {
                ImGui::TextDisabled("%s", materializr::tr("(nothing selected)"));
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(); // context-menu WindowPadding

    // Gizmo hint
    if (m_gizmo->isVisible()) {
        materializr::viewportBanner(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                    "Arrows: Move | Rings: Rotate | Cubes: Scale");
    }

    // Interactive extrude UI — banner + distance well live in the
    // controller; called here because the well anchors to THIS window.
    m_extrudeCtl.renderExtrudePanel(iopContext());

    // Interactive Push/Pull UI — banner + distance well live in the
    // controller; called here because the well anchors to THIS window.
    m_ppCtl.renderPushPullPanel(iopContext());

    // Interactive fillet/chamfer UI — banner + value well live in the
    // controller; called here because the well anchors to THIS window.
    m_edgeCtl.renderEdgeOpPanel(iopContext());

    // Move / Tilt / Scale Face: banner + value wells + commit/cancel. Lives
    // in the controller; called from here because the wells anchor to THIS
    // (the viewport) window's rect.
    m_moveFaceCtl.renderMoveFacePanel(iopContext(), uiScale());

    // Scale gizmo side panel (X/Y/Z % + uniform + Apply), shown in Scale mode.
    renderScalePanel();

    // Sketch mode indicator
    if (m_inSketchMode) {
        materializr::viewportBanner(
            ImVec4(0.2f, 1.0f, 0.4f, 1.0f),
            materializr::touchMode()
                ? "SKETCH MODE - Finish Sketch applies, Exit Sketch discards"
                : "SKETCH MODE - Press Escape to finish");
    }

    // Inline dimension input while placing a sketch shape. Suppressed while
    // the im-touch confirm bubble holds the shape — the bubble carries its
    // own value field, and two live inputs for one dimension is confusing.
    if (m_inSketchMode && m_sketchTool && m_sketchTool->hasPreview() &&
        !m_sketchShapeConfirmPending) {
        SketchToolMode mode = m_sketchTool->getPreviewType();
        // im-touch on touch: circles and rectangles get the near-shape
        // confirm bubble on lift (their exact-value input) — the top-right
        // dialog would be a SECOND diameter/size input flashing during the
        // drag, so it doesn't show for them at all. Lines keep it (no
        // bubble). Desktop im-touch (mouse, click-click placement) keeps it
        // too — the bubble only exists on the touch release path.
        // Only cede the inline field to the bubble when the bubble is ACTUALLY
        // up (im-touch press-drag-release hold). Otherwise the inline field
        // shows, so a shape never gets NO input. Modern always uses the inline
        // field (Steve's choice).
        const bool bubbleOwnsInput =
            imTouchLayout() && materializr::touchMode() &&
            m_sketchShapeConfirmPending &&
            (mode == SketchToolMode::Circle ||
             mode == SketchToolMode::Rectangle);
        const char* dimLabel = nullptr;
        const char* dimHint  =
            "Type a value and press Enter. The shape extends from your first click toward the cursor.";
        if (!bubbleOwnsInput)
        switch (mode) {
            case SketchToolMode::Line:      dimLabel = "Length (%s)"; break;
            case SketchToolMode::Circle:    dimLabel = "Diameter (%s)"; break;
            // Polygon side count is picked from the toolbar popout now, and
            // radius/rotation come from the drag — so no typed dialog (it was
            // the one that clipped). dimLabel stays null → no input window.
            case SketchToolMode::Rectangle:
                // Touch shows BOTH Width and Height fields at once (below);
                // desktop keeps its two-stage single field.
                dimLabel = materializr::touchMode()
                             ? "Rectangle (%s)"
                             : (m_sketchTool->getRectDimStage() == 0
                                    ? "Width (%s)" : "Height (%s)");
                dimHint  = materializr::touchMode()
                  ? "Type Width and Height, then Apply."
                  : (m_sketchTool->getRectDimStage() == 0
                       ? "Type width and Enter to lock horizontal — cursor still "
                         "drives height. Or click for both at once."
                       : "Type height and Enter to commit the rectangle.");
                break;
            case SketchToolMode::Arc:
                // Two stages, like the rectangle: the chord first, then one
                // number for the bow. Click 3 has no input until the chord
                // exists, so clickCount drives which is on offer.
                if (m_sketchTool->getClickCount() == 1) {
                    dimLabel = "Chord (%s)";
                    dimHint  = "Type the straight-line distance between the arc's "
                               "two ends and press Enter — or just click the end.";
                } else if (m_sketchTool->getClickCount() == 2) {
                    const bool sweep = m_sketchTool->getArcDimMode() ==
                                       SketchTool::ArcDimMode::Sweep;
                    dimLabel = sweep ? "Sweep (deg)" : "Radius (%s)";
                    dimHint  = sweep
                      ? "Type the swept angle and Enter. 180 is a semicircle. "
                        "Move the cursor across the chord to flip which way it bows."
                      : "Type the radius and Enter. It cannot be smaller than half "
                        "the chord. Move the cursor across the chord to flip which "
                        "way it bows.";
                }
                break;
            default: dimLabel = nullptr;
        }
        if (dimLabel) {
            // Fixed window width + fixed field width so every step is identical.
            // The old version used AlwaysAutoResize with an unsized InputText,
            // which put the field in ImGui's auto-resize feedback loop (field
            // width wants the window width, window width wants the content width)
            // — so some steps (the rectangle's 2nd "Height" entry, the line)
            // came out too narrow and clipped the typed digits off the left.
            // Anchor to the RIGHT SIDE OF THE VIEWPORT (Steve): in Modern the
            // viewport ends where the right panel begins (m_touchVpX +
            // m_touchVpW), so park it just inside that edge — in the drawing
            // area, not over the panel. Falls back to the screen's right edge
            // when the viewport rect isn't tracked (Classic).
            const float winW = uiW(230.0f);
            const ImGuiViewport* mvp = ImGui::GetMainViewport();
            float vpRight = mvp->WorkPos.x + mvp->WorkSize.x;
            if ((modernLayout() || imTouchLayout()) && m_touchVpW > 0.0f)
                vpRight = m_touchVpX + m_touchVpW;
            float winX = vpRight - winW - uiW(10.0f);
            if (winX < mvp->WorkPos.x + 6.0f) winX = mvp->WorkPos.x + 6.0f;
            ImGui::SetNextWindowPos(ImVec2(winX, ImGui::GetWindowPos().y + 50.0f),
                                    ImGuiCond_Appearing);
            ImGui::SetNextWindowSize(ImVec2(winW, 0.0f)); // fixed width, auto height
            ImGui::Begin("##SketchDimInput", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
            opDialogDragGrip(uiScale());

            ImGui::TextColored(materializr::accentText(), "%s",
                               std::strstr(dimLabel, "%s")
                                   ? materializr::trFormat(dimLabel, materializr::unitSuffix()).c_str()
                                   : materializr::tr(dimLabel));
            ImGui::Separator();
            // Window width is fixed now, so wrapping at the window edge is safe
            // (no feedback loop).
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(materializr::tr(dimHint));
            ImGui::PopTextWrapPos();

            // Arc apex stage: choose what the number MEANS. Sticky across
            // placements, so a run of same-radius arcs is not a toggle each
            // time.
            if (mode == SketchToolMode::Arc && m_sketchTool->getClickCount() == 2) {
                ImGui::Spacing();
                const bool sweep = m_sketchTool->getArcDimMode() ==
                                   SketchTool::ArcDimMode::Sweep;
                const float halfW = (ImGui::GetContentRegionAvail().x -
                                     ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                if (ImGui::RadioButton(materializr::tr("Degrees"), sweep))
                    m_sketchTool->setArcDimMode(SketchTool::ArcDimMode::Sweep);
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x +
                                      std::max(0.0f, halfW - ImGui::GetItemRectSize().x));
                if (ImGui::RadioButton(materializr::tr("Radius"), !sweep))
                    m_sketchTool->setArcDimMode(SketchTool::ArcDimMode::Radius);
                if (!sweep) {
                    // The chord's half-length is a hard floor; say so rather
                    // than let a too-small radius be silently refused.
                    ImGui::TextDisabled("%s", materializr::trFormat("min %s (half the chord)", materializr::fmtLength(m_sketchTool->arcMinRadius())).c_str());
                }
                ImGui::Spacing();
            }

            if (materializr::touchMode()) {
                // Native keyboard via a focused ImGui field (io.WantTextInput ->
                // Window::updateTextInput raises the system IME — same path as
                // the Properties editor). TAP TO FOCUS, no auto-raise: click-
                // drag still draws and text entry is opt-in (Steve).
                if (mode == SketchToolMode::Rectangle) {
                    // BOTH Width and Height at once. Seed from the current
                    // preview the first frame; the user overrides either, then
                    // Apply commits the exact W x H rectangle.
                    if (!m_sketchDimWasShown) {
                        glm::vec2 d = m_sketchTool->getPreviewEnd() -
                                      m_sketchTool->getPreviewStart();
                        m_sketchShapeDimW = std::max(0.01f, std::fabs(d.x));
                        m_sketchShapeDimH = std::max(0.01f, std::fabs(d.y));
                        m_sketchDimWasShown = true;
                    }
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                        ImVec2(uiW(10.0f), uiW(10.0f)));
                    ImGui::TextDisabled("%s", materializr::trFormat("Width (%s)", materializr::unitSuffix()).c_str());
                    ImGui::SetNextItemWidth(-1.0f);
                    materializr::lengthField("##dimW", &m_sketchShapeDimW);
                    ImGui::TextDisabled("%s", materializr::trFormat("Height (%s)", materializr::unitSuffix()).c_str());
                    ImGui::SetNextItemWidth(-1.0f);
                    materializr::lengthField("##dimH", &m_sketchShapeDimH);
                    ImGui::PopStyleVar();
                    if (m_sketchShapeDimW < 0.01f) m_sketchShapeDimW = 0.01f;
                    if (m_sketchShapeDimH < 0.01f) m_sketchShapeDimH = 0.01f;
                    ImGui::Spacing();
                    if (ImGui::Button(materializr::tr("Apply"), ImVec2(-1.0f, uiW(44.0f)))) {
                        const glm::vec2 ps = m_sketchTool->getPreviewStart();
                        const glm::vec2 pe = m_sketchTool->getPreviewEnd();
                        glm::vec2 dir(pe.x >= ps.x ? 1.0f : -1.0f,
                                      pe.y >= ps.y ? 1.0f : -1.0f);
                        if (std::fabs(pe.x - ps.x) < 1e-6f) dir.x = 1.0f;
                        if (std::fabs(pe.y - ps.y) < 1e-6f) dir.y = 1.0f;
                        const glm::vec2 base = m_sketchTool->getFirstClick();
                        const glm::vec2 target =
                            (m_sketchTool->getRectMode() == SketchTool::RectMode::Center)
                              ? base + glm::vec2(dir.x * m_sketchShapeDimW * 0.5f,
                                                 dir.y * m_sketchShapeDimH * 0.5f)
                              : base + glm::vec2(dir.x * m_sketchShapeDimW,
                                                 dir.y * m_sketchShapeDimH);
                        recordSketchMutation([&]{ m_sketchTool->onMouseDown(target, false); });
                        m_sketchDimWasShown = false;
                    }
                } else {
                    // Line / Circle: single field.
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                        ImVec2(uiW(12.0f), uiW(12.0f)));
                    // VALUE-based, like the Rectangle W/H fields above — so
                    // touch mode gets the number pad instead of the OS
                    // keyboard. Enter (pad or hardware) and Apply both commit;
                    // Apply stays for the finger route, since the pad's own
                    // Enter is the only way digits reach us on a tablet.
                    // The same predicate the commit uses: this field holds a
                    // length, an arc's sweep in DEGREES, or a polygon's SIDE
                    // COUNT. Only a length takes the unit's decimals.
                    const bool dimIsLen = m_sketchTool->dimensionValueIsLength();
                    const bool entered = materializr::inputNumber(
                        "##sketchDimT", &m_sketchDimValue, 0.0f, 0.0f,
                        dimIsLen ? materializr::lengthFormat() : "%.2f",
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::PopStyleVar();
                    ImGui::Spacing();
                    // A radius under half the chord describes no arc through
                    // the two endpoints, so Apply is dead there rather than
                    // silently refusing.
                    const float dimFloor =
                        (mode == SketchToolMode::Arc &&
                         m_sketchTool->getClickCount() == 2 &&
                         m_sketchTool->getArcDimMode() ==
                             SketchTool::ArcDimMode::Radius)
                            ? m_sketchTool->arcMinRadius() : 0.0f;
                    // The pad edits in the display unit; the floor is mm.
                    // Same rule as the desktop path: only convert a LENGTH.
                    auto dimToModel = [&](double shown) {
                        return dimIsLen ? materializr::lengthFieldCommit(shown) : shown;
                    };
                    ImGui::BeginDisabled(m_sketchDimValue <= 0.0f ||
                                         dimToModel(m_sketchDimValue) < dimFloor);
                    const bool applied =
                        ImGui::Button(materializr::tr("Apply"), ImVec2(-1.0f, uiW(44.0f)));
                    ImGui::EndDisabled();
                    if ((entered || applied) && m_sketchDimValue > 0.0f) {
                        const float v = static_cast<float>(dimToModel(m_sketchDimValue));
                        recordSketchMutation([&]{ m_sketchTool->applyDimension(v); });
                        m_sketchDimValue = 0.0f;
                        m_sketchDimBuf[0] = '\0';
                    }
                }
            } else {
                // Desktop: keyboard entry. Grab focus the first frame
                // placement begins so typing works immediately.
                if (!m_sketchDimWasShown) {
                    ImGui::SetKeyboardFocusHere();
                    m_sketchDimWasShown = true;
                }

                ImGui::SetNextItemWidth(winW - uiW(16.0f)); // fill the fixed width, minus padding
                // No CharsDecimal: a typed unit ("2in") needs letters. parseLength
                // is the gate instead, and it refuses anything but one number
                // with an optional unit.
                if (ImGui::InputText("##sketchDim", m_sketchDimBuf, sizeof(m_sketchDimBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_AutoSelectAll)) {
                    // Not every typed dimension is a length: an arc's sweep is
                    // DEGREES and a polygon's first value is a SIDE COUNT.
                    // Converting those display->mm made a typed 180 deg arrive
                    // as 4572 (clamped to 359.9) and 6 sides arrive as 152.
                    double v0 = 0.0;
                    const bool isLen = m_sketchTool->dimensionValueIsLength();
                    const bool ok = isLen ? materializr::parseLength(m_sketchDimBuf, v0)
                                          : materializr::parseFinite(m_sketchDimBuf, v0);
                    if (ok && v0 > 0.0) {
                        const float v = static_cast<float>(v0);
                        recordSketchMutation([&]{ m_sketchTool->applyDimension(v); });
                    }
                    m_sketchDimBuf[0] = '\0';
                    m_sketchDimWasShown = false; // re-focus on the next placement
                }
            }

            ImGui::End();
        }
    } else {
        // Reset when not placing
        m_sketchDimBuf[0] = '\0';
        m_sketchDimValue = 0.0f;
        m_sketchDimWasShown = false;
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

bool Application::captureProjectThumbnailPNG(std::vector<uint8_t>& pngOut) {
    if (!m_viewport || !m_shapeRenderer || !m_edgeRenderer ||
        !m_backgroundRenderer || !m_document)
        return false;

    // Meshes can be stale when a save lands between frames (deferred slot).
    // Done BEFORE the bounding box so the box can ride on the triangulation.
    if (m_meshesDirty || !m_dirtyBodyIds.empty()) {
        rebuildMeshes();
        m_meshesDirty = false;
    }

    // Bounding box over the visible bodies (the same set ShapeRenderer holds
    // meshes for). Nothing visible → no thumbnail; the save just omits the
    // section and the landing tile shows its placeholder.
    //
    // Add-with-triangulation, NOT AddOptimal: this box only frames a 512px
    // preview, so a slightly loose fit costs nothing visible, while
    // AddOptimal's exact geometry pass is expensive on precisely the surfaces
    // this app produces — thread helicoids and lofted B-splines — and it ran
    // on EVERY Ctrl+S.
    Bnd_Box bb;
    for (int id : m_document->getAllBodyIds()) {
        if (!m_document->isBodyVisible(id)) continue;
        try {
            const TopoDS_Shape& s = m_document->getBody(id);
            if (!s.IsNull()) BRepBndLib::Add(s, bb, Standard_True);
        } catch (...) {}
    }
    if (bb.IsVoid()) return false;

    // Dedicated one-shot FBO — deliberately NOT the live viewport FBO, whose
    // texture ImGui may already have referenced this frame (destroying it
    // mid-frame leaves the draw list pointing at a dead texture). Rendered at
    // 2x and box-downscaled below: cheap antialiasing without MSAA plumbing.
    const int kThumbPx = 512;
    const int kRenderPx = kThumbPx * 2;
    GLuint fbo = 0, colorTex = 0, depthRb = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kRenderPx, kRenderPx, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, colorTex, 0);
    glGenRenderbuffers(1, &depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          kRenderPx, kRenderPx);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, depthRb);
    auto cleanup = [&]() {
        glBindFramebuffer(GL_FRAMEBUFFER, g_windowFramebuffer);
        if (colorTex) glDeleteTextures(1, &colorTex);
        if (depthRb) glDeleteRenderbuffers(1, &depthRb);
        if (fbo) glDeleteFramebuffers(1, &fbo);
    };
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        cleanup();
        return false;
    }
    glViewport(0, 0, kRenderPx, kRenderPx);

    // The camera is shared with the live viewport — restored by copy below.
    // Section planes and background colours are re-asserted by renderViewport()
    // every frame, so this pass may set them freely.
    Camera saved = m_viewport->getCamera();
    Camera& cam = m_viewport->getCamera();
    cam.reset();                       // home orientation (default isometric)
    cam.setAspect(1.0f);
    Standard_Real x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    cam.zoomToFit(glm::vec3((float)x0, (float)y0, (float)z0),
                  glm::vec3((float)x1, (float)y1, (float)z1));

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    const bool lightBg = m_themeManager &&
                         m_themeManager->getTheme() == Theme::Light;
    if (lightBg) {
        m_backgroundRenderer->setTopColor(glm::vec3(0.92f, 0.93f, 0.96f));
        m_backgroundRenderer->setBottomColor(glm::vec3(0.78f, 0.80f, 0.85f));
    } else {
        m_backgroundRenderer->setTopColor(glm::vec3(0.22f, 0.22f, 0.28f));
        m_backgroundRenderer->setBottomColor(glm::vec3(0.12f, 0.12f, 0.15f));
    }
    m_backgroundRenderer->render();
    glEnable(GL_DEPTH_TEST);

    // A live section cut would carve the thumbnail too — always off here.
    m_shapeRenderer->setSectionPlane(false, glm::vec3(0.0f),
                                     glm::vec3(0.0f, 1.0f, 0.0f));
    m_edgeRenderer->setSectionPlane(false, glm::vec3(0.0f),
                                    glm::vec3(0.0f, 1.0f, 0.0f));

    glm::mat4 view = cam.getViewMatrix();
    glm::mat4 proj = cam.getProjectionMatrix();
    m_shapeRenderer->render(view, proj, cam.getPosition());
    m_edgeRenderer->render(view, proj);
    m_viewport->getCamera() = saved;

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<uint8_t> raw(static_cast<size_t>(kRenderPx) * kRenderPx * 4);
    glReadPixels(0, 0, kRenderPx, kRenderPx, GL_RGBA, GL_UNSIGNED_BYTE,
                 raw.data());
    cleanup();

    // 2x2 box downscale + vertical flip (GL reads bottom-up) in one pass.
    std::vector<uint8_t> rgba(static_cast<size_t>(kThumbPx) * kThumbPx * 4);
    for (int y = 0; y < kThumbPx; ++y) {
        // Destination row y (top-down) samples source rows counted from the top,
        // which sit at (kRenderPx-1 - srcY) in GL's bottom-up buffer.
        const int sy0 = kRenderPx - 1 - (y * 2);
        const int sy1 = sy0 - 1;
        for (int x = 0; x < kThumbPx; ++x) {
            const int sx = x * 2;
            for (int c = 0; c < 4; ++c) {
                const unsigned sum =
                    raw[(static_cast<size_t>(sy0) * kRenderPx + sx) * 4 + c] +
                    raw[(static_cast<size_t>(sy0) * kRenderPx + sx + 1) * 4 + c] +
                    raw[(static_cast<size_t>(sy1) * kRenderPx + sx) * 4 + c] +
                    raw[(static_cast<size_t>(sy1) * kRenderPx + sx + 1) * 4 + c];
                rgba[(static_cast<size_t>(y) * kThumbPx + x) * 4 + c] =
                    static_cast<uint8_t>(sum / 4);
            }
        }
    }
    return materializr::encodePng(rgba.data(), kThumbPx, kThumbPx, pngOut);
}

} // namespace materializr
